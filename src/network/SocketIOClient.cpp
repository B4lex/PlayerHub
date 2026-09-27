#include "network/SocketIOClient.h"
#include "config/PluginConfig.h"
#include "runtime/Profiling.h"

#include <sio_client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Network
{
    namespace
    {
        std::atomic_bool connected{ false };
        std::atomic_uint64_t connectionGeneration{ 0 };
        std::atomic_bool reconnectWorkerRunning{ false };
        std::atomic_bool attemptFailed{ false };
        std::mutex remoteChangesLock;
        std::unordered_map<std::string, RemotePlayerTransform> pendingTransforms;
        std::unordered_map<std::string, PlayerTransform> latestReceivedTransforms;
        std::unordered_map<std::string, RemotePlayerAppearance> pendingAppearances;
        std::unordered_map<std::string, std::uint64_t> latestReceivedAppearanceRevisions;
        std::unordered_map<std::string, RemotePlayerEquipment> pendingEquipment;
        std::unordered_map<std::string, std::uint64_t> latestReceivedEquipmentRevisions;
        std::deque<AnimationAction> pendingAnimationActions;
        std::unordered_map<std::string, std::uint64_t> latestReceivedActionSequences;
        std::deque<RemoteMountTransition> pendingMountTransitions;
        std::unordered_set<std::string> pendingExits;
        std::unordered_set<std::string> pendingDisconnects;
        std::uint64_t localActionSequence = 0;

        // Bounds memory while nothing drains chat, for example without the overlay.
        constexpr std::size_t kMaximumPendingChatMessages = 128;
        std::mutex chatMessagesLock;
        std::deque<Multiplayer::ChatMessage> pendingChatMessages;

        const std::string& GetServerEndpoint()
        {
            return Config::Get().network.serverEndpoint;
        }

        sio::client& GetClient()
        {
            // Keep the Socket.IO client alive for the lifetime of the SKSE DLL.
            static sio::client client;
            return client;
        }

        // A copy, because the client drops its socket object when the server closes
        // the namespace.
        sio::socket::ptr GetSocket()
        {
            return GetClient().socket();
        }

        bool ReadString(
            const std::map<std::string, sio::message::ptr>& a_values, const std::string& a_key, std::string& a_result)
        {
            const auto value = a_values.find(a_key);
            if (value == a_values.end() || !value->second || value->second->get_flag() != sio::message::flag_string)
            {
                return false;
            }

            a_result = value->second->get_string();
            return !a_result.empty();
        }

        sio::message::ptr FindValue(const std::map<std::string, sio::message::ptr>& a_values, const std::string& a_key)
        {
            const auto value = a_values.find(a_key);
            return value == a_values.end() ? sio::message::ptr{} : value->second;
        }

        bool ReadNumberValue(const sio::message::ptr& a_value, double& a_result)
        {
            if (!a_value)
            {
                return false;
            }

            switch (a_value->get_flag())
            {
                case sio::message::flag_double:
                    a_result = a_value->get_double();
                    break;
                case sio::message::flag_integer:
                    a_result = static_cast<double>(a_value->get_int());
                    break;
                default:
                    return false;
            }

            return std::isfinite(a_result);
        }

        bool ReadNumber(
            const std::map<std::string, sio::message::ptr>& a_values, const std::string& a_key, double& a_result)
        {
            return ReadNumberValue(FindValue(a_values, a_key), a_result);
        }

        bool ReadMovementState(
            const std::map<std::string, sio::message::ptr>& a_values, Multiplayer::MovementState& a_result)
        {
            std::string state;
            if (!ReadString(a_values, "movementState", state))
            {
                return false;
            }

            if (state == "idle")
            {
                a_result = Multiplayer::MovementState::kIdle;
            }
            else if (state == "walking")
            {
                a_result = Multiplayer::MovementState::kWalking;
            }
            else if (state == "running")
            {
                a_result = Multiplayer::MovementState::kRunning;
            }
            else if (state == "sprinting")
            {
                a_result = Multiplayer::MovementState::kSprinting;
            }
            else if (state == "sneakingIdle")
            {
                a_result = Multiplayer::MovementState::kSneakingIdle;
            }
            else if (state == "sneakingWalking")
            {
                a_result = Multiplayer::MovementState::kSneakingWalking;
            }
            else if (state == "sneakingRunning")
            {
                a_result = Multiplayer::MovementState::kSneakingRunning;
            }
            else
            {
                return false;
            }

            return true;
        }

        bool ReadBool(
            const std::map<std::string, sio::message::ptr>& a_values, const std::string& a_key, bool& a_result)
        {
            const auto value = a_values.find(a_key);
            if (value == a_values.end() || !value->second || value->second->get_flag() != sio::message::flag_boolean)
            {
                return false;
            }

            a_result = value->second->get_bool();
            return true;
        }

        // A missing field reads as false; a present one must be a boolean.
        bool ReadOptionalBool(
            const std::map<std::string, sio::message::ptr>& a_values, const std::string& a_key, bool& a_result)
        {
            if (!a_values.contains(a_key))
            {
                a_result = false;
                return true;
            }
            return ReadBool(a_values, a_key, a_result);
        }

        bool ReadUnsignedIntegerValue(
            const sio::message::ptr& a_value, std::uint64_t a_maximum, std::uint64_t& a_result)
        {
            if (!a_value || a_value->get_flag() != sio::message::flag_integer)
            {
                return false;
            }

            const auto integer = a_value->get_int();
            if (integer < 0 || static_cast<std::uint64_t>(integer) > a_maximum)
            {
                return false;
            }

            a_result = static_cast<std::uint64_t>(integer);
            return true;
        }

        bool ReadUnsignedInteger(const std::map<std::string, sio::message::ptr>& a_values, const std::string& a_key,
            std::uint64_t a_maximum, std::uint64_t& a_result)
        {
            return ReadUnsignedIntegerValue(FindValue(a_values, a_key), a_maximum, a_result);
        }

        bool ReadArray(const std::map<std::string, sio::message::ptr>& a_values, const std::string& a_key,
            std::size_t a_maximumSize, const std::vector<sio::message::ptr>*& a_result)
        {
            const auto value = FindValue(a_values, a_key);
            if (!value || value->get_flag() != sio::message::flag_array || value->get_vector().size() > a_maximumSize)
            {
                return false;
            }

            a_result = &value->get_vector();
            return true;
        }

        bool ReadAppearanceColor(const sio::message::ptr& a_value, Multiplayer::AppearanceColor& a_result)
        {
            if (!a_value || a_value->get_flag() != sio::message::flag_object)
            {
                return false;
            }

            const auto& values = a_value->get_map();
            std::uint64_t red = 0;
            std::uint64_t green = 0;
            std::uint64_t blue = 0;
            if (!ReadUnsignedInteger(values, "r", 0xFF, red) || !ReadUnsignedInteger(values, "g", 0xFF, green)
                || !ReadUnsignedInteger(values, "b", 0xFF, blue))
            {
                return false;
            }

            a_result = Multiplayer::AppearanceColor{ .red = static_cast<std::uint8_t>(red),
                .green = static_cast<std::uint8_t>(green),
                .blue = static_cast<std::uint8_t>(blue) };
            return true;
        }

        bool ReadFormReference(
            const std::map<std::string, sio::message::ptr>& a_values, Multiplayer::FormReference& a_result)
        {
            std::uint64_t localFormID = 0;
            if (!ReadString(a_values, "plugin", a_result.plugin)
                || !ReadUnsignedInteger(
                    a_values, "formId", Multiplayer::FormReference::kMaximumLocalFormID, localFormID))
            {
                return false;
            }

            a_result.localFormID = static_cast<std::uint32_t>(localFormID);
            return true;
        }

        bool IsNullValue(const sio::message::ptr& a_value)
        {
            return !a_value || a_value->get_flag() == sio::message::flag_null;
        }

        sio::message::ptr CreateFormReference(const Multiplayer::FormReference& a_reference)
        {
            if (a_reference.IsEmpty())
            {
                return sio::null_message::create();
            }
            auto reference = sio::object_message::create();
            auto& values = reference->get_map();
            values["plugin"] = sio::string_message::create(a_reference.plugin);
            values["formId"] = sio::int_message::create(a_reference.localFormID);
            return reference;
        }

        bool ParseRemoteTransform(const sio::message::ptr& a_data, RemotePlayerTransform& a_result)
        {
            if (!a_data || a_data->get_flag() != sio::message::flag_object)
            {
                return false;
            }

            const auto& values = a_data->get_map();
            double x = 0.0;
            double y = 0.0;
            double z = 0.0;
            double heading = 0.0;
            double speed = 0.0;
            double direction = 0.0;
            Multiplayer::MovementState movementState{ Multiplayer::MovementState::kIdle };
            bool jumping = false;
            bool weaponDrawn = false;
            Multiplayer::HandPresentation handPresentation;
            bool dead = false;
            bool mounted = false;
            bool swimming = false;
            std::uint64_t cellID = 0;
            std::uint64_t worldspaceID = 0;
            std::uint64_t furnitureRefID = 0;
            std::uint64_t graphEpoch = 0;
            std::uint64_t sequence = 0;
            if (!ReadString(values, "playerId", a_result.playerID) || !ReadNumber(values, "x", x)
                || !ReadNumber(values, "y", y) || !ReadNumber(values, "z", z) || !ReadNumber(values, "heading", heading)
                || !ReadNumber(values, "speed", speed) || speed < 0.0 || speed > (std::numeric_limits<float>::max)()
                || !ReadNumber(values, "direction", direction) || direction < 0.0 || direction > 1.0
                || !ReadMovementState(values, movementState) || !ReadBool(values, "jumping", jumping)
                || !ReadBool(values, "weaponDrawn", weaponDrawn)
                || !ReadOptionalBool(values, "bowDrawn", handPresentation.bowDrawn)
                || !ReadOptionalBool(values, "castingLeft", handPresentation.castingLeft)
                || !ReadOptionalBool(values, "castingRight", handPresentation.castingRight)
                || !ReadOptionalBool(values, "blocking", handPresentation.blocking) || !ReadBool(values, "dead", dead)
                || !ReadBool(values, "mounted", mounted) || !ReadBool(values, "swimming", swimming)
                || !ReadUnsignedInteger(values, "cellId", (std::numeric_limits<std::uint32_t>::max)(), cellID)
                || !ReadUnsignedInteger(
                    values, "worldspaceId", (std::numeric_limits<std::uint32_t>::max)(), worldspaceID)
                || !ReadUnsignedInteger(
                    values, "furnitureRefId", (std::numeric_limits<std::uint32_t>::max)(), furnitureRefID)
                || !ReadUnsignedInteger(values, "graphEpoch", (std::numeric_limits<std::int64_t>::max)(), graphEpoch)
                || graphEpoch == 0
                || !ReadUnsignedInteger(values, "sequence", (std::numeric_limits<std::int64_t>::max)(), sequence))
            {
                return false;
            }

            a_result.transform = PlayerTransform{ .x = static_cast<float>(x),
                .y = static_cast<float>(y),
                .z = static_cast<float>(z),
                .heading = static_cast<float>(heading),
                .speed = static_cast<float>(speed),
                .direction = Multiplayer::NormalizeMovementDirection(static_cast<float>(direction)),
                .animationState = Multiplayer::AnimationState{ .movementState = movementState,
                    .jumping = jumping,
                    .weaponDrawn = weaponDrawn },
                .handPresentation = handPresentation,
                .dead = dead,
                .mounted = mounted,
                .swimming = swimming,
                .cellID = static_cast<std::uint32_t>(cellID),
                .worldspaceID = static_cast<std::uint32_t>(worldspaceID),
                .furnitureRefID = static_cast<std::uint32_t>(furnitureRefID),
                .graphEpoch = graphEpoch,
                .sequence = sequence };
            return true;
        }

        bool ReadAttackAction(const std::map<std::string, sio::message::ptr>& a_values, AttackAction& a_result)
        {
            std::string action;
            if (!ReadString(a_values, "action", action))
            {
                return false;
            }
            for (const auto candidate :
                { AttackAction::kNormal, AttackAction::kLeft, AttackAction::kDual, AttackAction::kBash })
            {
                if (action == GetAttackActionName(candidate))
                {
                    a_result = candidate;
                    return true;
                }
            }
            return false;
        }

        // Only a power attack carries a direction, and it always does.
        bool ReadPowerAttack(const std::map<std::string, sio::message::ptr>& a_values, Attack& a_result)
        {
            if (!ReadBool(a_values, "power", a_result.power))
            {
                return false;
            }
            a_result.direction = PowerAttackDirection::kStanding;
            if (!a_result.power)
            {
                return !a_values.contains("direction");
            }
            std::string direction;
            if (!ReadString(a_values, "direction", direction))
            {
                return false;
            }
            for (const auto candidate : { PowerAttackDirection::kStanding, PowerAttackDirection::kForward,
                     PowerAttackDirection::kBackward, PowerAttackDirection::kLeft, PowerAttackDirection::kRight })
            {
                if (direction == GetPowerAttackDirectionName(candidate))
                {
                    a_result.direction = candidate;
                    return true;
                }
            }
            return false;
        }

        bool ParseRemoteAnimationAction(const sio::message::ptr& a_data, AnimationAction& a_result)
        {
            if (!a_data || a_data->get_flag() != sio::message::flag_object)
            {
                return false;
            }

            const auto& values = a_data->get_map();
            std::string family;
            std::uint64_t actionSequence = 0;
            std::uint64_t graphEpoch = 0;
            std::uint64_t checkpointSequence = 0;
            if (!ReadString(values, "playerId", a_result.playerID) || !ReadString(values, "family", family)
                || family != "attack" || !ReadAttackAction(values, a_result.attack.action)
                || !ReadPowerAttack(values, a_result.attack)
                || !ReadUnsignedInteger(
                    values, "actionSequence", (std::numeric_limits<std::int64_t>::max)(), actionSequence)
                || actionSequence == 0
                || !ReadUnsignedInteger(values, "graphEpoch", (std::numeric_limits<std::int64_t>::max)(), graphEpoch)
                || graphEpoch == 0
                || !ReadUnsignedInteger(
                    values, "checkpointSequence", (std::numeric_limits<std::int64_t>::max)(), checkpointSequence)
                || checkpointSequence == 0)
            {
                return false;
            }

            const auto prerequisites = values.find("prerequisites");
            if (prerequisites == values.end() || !prerequisites->second
                || prerequisites->second->get_flag() != sio::message::flag_object
                || prerequisites->second->get_map().size() != 1)
            {
                return false;
            }
            bool weaponDrawn = false;
            if (!ReadBool(prerequisites->second->get_map(), "weaponDrawn", weaponDrawn) || !weaponDrawn)
            {
                return false;
            }

            a_result.actionSequence = actionSequence;
            a_result.graphEpoch = graphEpoch;
            a_result.checkpointSequence = checkpointSequence;
            return true;
        }

        bool ParseRemoteMountTransition(const sio::message::ptr& a_data, RemoteMountTransition& a_result)
        {
            if (!a_data || a_data->get_flag() != sio::message::flag_object)
            {
                return false;
            }

            const auto& values = a_data->get_map();
            std::string transition;
            if (!ReadString(values, "playerId", a_result.playerID) || !ReadString(values, "transition", transition)
                || !ReadUnsignedInteger(
                    values, "graphEpoch", (std::numeric_limits<std::int64_t>::max)(), a_result.graphEpoch)
                || a_result.graphEpoch == 0
                || !ReadUnsignedInteger(values, "checkpointSequence", (std::numeric_limits<std::int64_t>::max)(),
                    a_result.checkpointSequence)
                || a_result.checkpointSequence == 0)
            {
                return false;
            }
            for (const auto candidate : { MountTransition::kEnter, MountTransition::kExit })
            {
                if (transition == GetMountTransitionName(candidate))
                {
                    a_result.transition = candidate;
                    return true;
                }
            }
            return false;
        }

        // Returns nullptr on success, otherwise the name of the rejected field.
        const char* ParseRemoteAppearance(const sio::message::ptr& a_data, RemotePlayerAppearance& a_result)
        {
            if (!a_data || a_data->get_flag() != sio::message::flag_object)
            {
                return "payload";
            }

            using Multiplayer::PlayerAppearance;
            const auto& values = a_data->get_map();
            auto& appearance = a_result.appearance;

            std::uint64_t revision = 0;
            if (!ReadString(values, "playerId", a_result.playerID))
            {
                return "playerId";
            }
            if (!ReadUnsignedInteger(values, "revision", (std::numeric_limits<std::int64_t>::max)(), revision)
                || revision == 0)
            {
                return "revision";
            }
            appearance.revision = revision;

            // Recordings made before name synchronization have no name.
            const auto name = FindValue(values, "name");
            if (IsNullValue(name))
            {
                appearance.name.clear();
            }
            else if (name->get_flag() != sio::message::flag_string
                || !Multiplayer::IsValidAppearanceName(name->get_string()))
            {
                return "name";
            }
            else
            {
                appearance.name = name->get_string();
            }

            const auto race = FindValue(values, "race");
            if (!race || race->get_flag() != sio::message::flag_object
                || !ReadFormReference(race->get_map(), appearance.race))
            {
                return "race";
            }
            if (!ReadBool(values, "female", appearance.female))
            {
                return "female";
            }

            double weight = 0.0;
            if (!ReadNumber(values, "weight", weight) || std::abs(weight) > (std::numeric_limits<float>::max)())
            {
                return "weight";
            }
            appearance.weight = static_cast<float>(weight);

            double height = 0.0;
            if (!ReadNumber(values, "height", height) || std::abs(height) > (std::numeric_limits<float>::max)())
            {
                return "height";
            }
            appearance.height = static_cast<float>(height);

            const std::vector<sio::message::ptr>* headParts = nullptr;
            if (!ReadArray(values, "headParts", PlayerAppearance::kMaximumHeadParts, headParts))
            {
                return "headParts";
            }
            for (const auto& headPart : *headParts)
            {
                Multiplayer::AppearanceHeadPart result;
                std::uint64_t type = 0;
                if (!headPart || headPart->get_flag() != sio::message::flag_object
                    || !ReadFormReference(headPart->get_map(), result.form)
                    || !ReadUnsignedInteger(headPart->get_map(), "type", PlayerAppearance::kMaximumHeadPartType, type))
                {
                    return "headParts[]";
                }
                result.type = static_cast<std::uint8_t>(type);
                appearance.headParts.push_back(std::move(result));
            }

            // JSON null may decode to either a null message or an empty pointer.
            const auto hairColor = FindValue(values, "hairColor");
            if (IsNullValue(hairColor))
            {
                appearance.hairColor.reset();
            }
            else
            {
                Multiplayer::AppearanceColor color;
                if (!ReadAppearanceColor(hairColor, color))
                {
                    return "hairColor";
                }
                appearance.hairColor = color;
            }

            const auto faceDetails = FindValue(values, "faceDetails");
            if (IsNullValue(faceDetails))
            {
                appearance.faceDetails = {};
            }
            else if (faceDetails->get_flag() != sio::message::flag_object
                || !ReadFormReference(faceDetails->get_map(), appearance.faceDetails))
            {
                return "faceDetails";
            }

            if (!ReadAppearanceColor(FindValue(values, "bodyTint"), appearance.bodyTint))
            {
                return "bodyTint";
            }

            const std::vector<sio::message::ptr>* faceMorphs = nullptr;
            if (!ReadArray(values, "faceMorphs", PlayerAppearance::kFaceMorphCount, faceMorphs)
                || faceMorphs->size() != PlayerAppearance::kFaceMorphCount)
            {
                return "faceMorphs";
            }
            for (std::size_t i = 0; i < PlayerAppearance::kFaceMorphCount; ++i)
            {
                double morph = 0.0;
                if (!ReadNumberValue((*faceMorphs)[i], morph))
                {
                    return "faceMorphs[]";
                }
                // Unset morphs use FLT_MAX as a sentinel. The JSON parser is not full
                // precision, so that value can come back marginally above FLT_MAX.
                const double floatLimit = (std::numeric_limits<float>::max)();
                appearance.faceMorphs[i] = static_cast<float>(std::clamp(morph, -floatLimit, floatLimit));
            }

            const std::vector<sio::message::ptr>* faceParts = nullptr;
            if (!ReadArray(values, "faceParts", PlayerAppearance::kFacePartCount, faceParts)
                || faceParts->size() != PlayerAppearance::kFacePartCount)
            {
                return "faceParts";
            }
            for (std::size_t i = 0; i < PlayerAppearance::kFacePartCount; ++i)
            {
                const auto& part = (*faceParts)[i];
                if (!part || part->get_flag() != sio::message::flag_integer
                    || part->get_int() < (std::numeric_limits<std::int32_t>::min)()
                    || part->get_int() > (std::numeric_limits<std::int32_t>::max)())
                {
                    return "faceParts[]";
                }
                appearance.faceParts[i] = static_cast<std::int32_t>(part->get_int());
            }

            const std::vector<sio::message::ptr>* tints = nullptr;
            if (!ReadArray(values, "tints", PlayerAppearance::kMaximumTints, tints))
            {
                return "tints";
            }
            for (const auto& tint : *tints)
            {
                Multiplayer::AppearanceTint result;
                std::uint64_t index = 0;
                std::uint64_t type = 0;
                double alpha = 0.0;
                if (!tint || tint->get_flag() != sio::message::flag_object
                    || !ReadUnsignedInteger(tint->get_map(), "index", 0xFFFF, index)
                    || !ReadUnsignedInteger(tint->get_map(), "type", PlayerAppearance::kMaximumTintType, type)
                    || !ReadAppearanceColor(tint, result.color) || !ReadNumber(tint->get_map(), "alpha", alpha))
                {
                    return "tints[]";
                }
                result.index = static_cast<std::uint16_t>(index);
                result.type = static_cast<std::uint8_t>(type);
                result.alpha = static_cast<float>(alpha);
                appearance.tints.push_back(result);
            }

            return Multiplayer::IsValidAppearance(appearance) ? nullptr : "value ranges";
        }

        bool ReadEquipmentHand(const sio::message::ptr& a_value, std::optional<Multiplayer::EquipmentHand>& a_result)
        {
            // JSON null may decode to either a null message or an empty pointer.
            if (IsNullValue(a_value))
            {
                a_result.reset();
                return true;
            }
            if (a_value->get_flag() != sio::message::flag_object)
            {
                return false;
            }

            const auto& values = a_value->get_map();
            Multiplayer::EquipmentHand hand;
            std::string kind;
            if (!ReadString(values, "kind", kind))
            {
                return false;
            }
            if (kind == "spell")
            {
                hand.kind = Multiplayer::EquipmentHandKind::kSpell;
                a_result = std::move(hand);
                return true;
            }
            if (!ReadFormReference(values, hand.form))
            {
                return false;
            }
            if (kind == "torch")
            {
                hand.kind = Multiplayer::EquipmentHandKind::kTorch;
            }
            else if (kind == "weapon")
            {
                std::uint64_t weaponType = 0;
                if (!ReadUnsignedInteger(
                        values, "weaponType", Multiplayer::PlayerEquipment::kMaximumWeaponType, weaponType))
                {
                    return false;
                }
                hand.kind = Multiplayer::EquipmentHandKind::kWeapon;
                hand.weaponType = static_cast<std::uint8_t>(weaponType);
            }
            else
            {
                return false;
            }
            a_result = std::move(hand);
            return true;
        }

        // Returns nullptr on success, otherwise the name of the rejected field.
        const char* ParseRemoteEquipment(const sio::message::ptr& a_data, RemotePlayerEquipment& a_result)
        {
            if (!a_data || a_data->get_flag() != sio::message::flag_object)
            {
                return "payload";
            }

            using Multiplayer::PlayerEquipment;
            const auto& values = a_data->get_map();
            auto& equipment = a_result.equipment;

            std::uint64_t revision = 0;
            if (!ReadString(values, "playerId", a_result.playerID))
            {
                return "playerId";
            }
            if (!ReadUnsignedInteger(values, "revision", (std::numeric_limits<std::int64_t>::max)(), revision)
                || revision == 0)
            {
                return "revision";
            }
            equipment.revision = revision;

            const std::vector<sio::message::ptr>* armor = nullptr;
            if (!ReadArray(values, "armor", PlayerEquipment::kMaximumArmor, armor))
            {
                return "armor";
            }
            for (const auto& entry : *armor)
            {
                Multiplayer::EquipmentArmor result;
                std::uint64_t slotMask = 0;
                std::string armorType;
                if (!entry || entry->get_flag() != sio::message::flag_object
                    || !ReadFormReference(entry->get_map(), result.form)
                    || !ReadUnsignedInteger(
                        entry->get_map(), "slotMask", (std::numeric_limits<std::uint32_t>::max)(), slotMask)
                    || !ReadString(entry->get_map(), "armorType", armorType))
                {
                    return "armor[]";
                }
                if (armorType == "light")
                {
                    result.armorType = Multiplayer::EquipmentArmorType::kLight;
                }
                else if (armorType == "heavy")
                {
                    result.armorType = Multiplayer::EquipmentArmorType::kHeavy;
                }
                else if (armorType == "clothing")
                {
                    result.armorType = Multiplayer::EquipmentArmorType::kClothing;
                }
                else
                {
                    return "armor[].armorType";
                }
                result.slotMask = static_cast<std::uint32_t>(slotMask);
                equipment.armor.push_back(std::move(result));
            }

            const auto rightHand = values.find("rightHand");
            if (rightHand == values.end() || !ReadEquipmentHand(rightHand->second, equipment.rightHand))
            {
                return "rightHand";
            }
            const auto leftHand = values.find("leftHand");
            if (leftHand == values.end() || !ReadEquipmentHand(leftHand->second, equipment.leftHand))
            {
                return "leftHand";
            }
            if (const auto ammo = FindValue(values, "ammo"); !IsNullValue(ammo)
                && (ammo->get_flag() != sio::message::flag_object
                    || !ReadFormReference(ammo->get_map(), equipment.ammo)))
            {
                return "ammo";
            }

            return Multiplayer::IsValidEquipment(equipment) ? nullptr : "value ranges";
        }

        void OnRemoteTransform(std::string const&, sio::message::ptr const& a_data, bool, sio::message::list&)
        {
            PROFILE_FUNCTION();
            RemotePlayerTransform update;
            if (!ParseRemoteTransform(a_data, update))
            {
                logs::warn("Ignored malformed Socket.IO player:state payload");
                return;
            }

            const auto playerID = update.playerID;
            std::scoped_lock lock{ remoteChangesLock };
            pendingDisconnects.erase(playerID);

            const auto latest = latestReceivedTransforms.find(playerID);
            if (latest != latestReceivedTransforms.end()
                && (update.transform.sequence <= latest->second.sequence
                    || update.transform.graphEpoch < latest->second.graphEpoch))
            {
                logs::debug(
                    "Animation sync ignored stale network snapshot: player={} sequence={} state={} newestSequence={} "
                    "newestState={}",
                    playerID, static_cast<unsigned long long>(update.transform.sequence),
                    Multiplayer::GetMovementStateName(update.transform.animationState.movementState),
                    static_cast<unsigned long long>(latest->second.sequence),
                    Multiplayer::GetMovementStateName(latest->second.animationState.movementState));
                return;
            }
            // Transform, locomotion, jump state, and combat stance are one latest-wins
            // checkpoint. Transient graph events are consequences of changing between
            // checkpoints; retaining them separately can replay obsolete transitions.
            latestReceivedTransforms.insert_or_assign(playerID, update.transform);
            pendingTransforms.insert_or_assign(playerID, std::move(update));
        }

        void OnRemoteAnimationAction(std::string const&, sio::message::ptr const& a_data, bool, sio::message::list&)
        {
            PROFILE_FUNCTION();
            AnimationAction action;
            if (!ParseRemoteAnimationAction(a_data, action))
            {
                logs::warn("Ignored malformed Socket.IO player:action payload");
                return;
            }

            std::scoped_lock lock{ remoteChangesLock };
            pendingDisconnects.erase(action.playerID);
            const auto latest = latestReceivedActionSequences.find(action.playerID);
            if (latest != latestReceivedActionSequences.end())
            {
                if (action.actionSequence <= latest->second)
                {
                    logs::debug(
                        "Animation sync ignored stale action: player={} actionSequence={} newestActionSequence={}",
                        action.playerID, static_cast<unsigned long long>(action.actionSequence),
                        static_cast<unsigned long long>(latest->second));
                    return;
                }
                if (action.actionSequence != latest->second + 1)
                {
                    // Socket.IO is reliable and ordered within a connection. A gap means
                    // continuity was lost; reject rather than playing actions out of order.
                    logs::warn("Animation sync rejected action sequence gap: player={} expected={} received={}",
                        action.playerID, static_cast<unsigned long long>(latest->second + 1),
                        static_cast<unsigned long long>(action.actionSequence));
                    return;
                }
            }

            latestReceivedActionSequences.insert_or_assign(action.playerID, action.actionSequence);
            logs::debug(
                "Animation sync received attack: player={} action={} power={} direction={} actionSequence={} "
                "graphEpoch={} checkpointSequence={}",
                action.playerID, GetAttackActionName(action.attack.action), action.attack.power,
                GetPowerAttackDirectionName(action.attack.direction),
                static_cast<unsigned long long>(action.actionSequence),
                static_cast<unsigned long long>(action.graphEpoch),
                static_cast<unsigned long long>(action.checkpointSequence));
            pendingAnimationActions.push_back(std::move(action));
        }

        void OnRemoteMountTransition(std::string const&, sio::message::ptr const& a_data, bool, sio::message::list&)
        {
            PROFILE_FUNCTION();
            RemoteMountTransition transition;
            if (!ParseRemoteMountTransition(a_data, transition))
            {
                logs::warn("Ignored malformed Socket.IO player:mount payload");
                return;
            }

            logs::debug("Mount sync received transition: player={} transition={} graphEpoch={} checkpointSequence={}",
                transition.playerID, GetMountTransitionName(transition.transition),
                static_cast<unsigned long long>(transition.graphEpoch),
                static_cast<unsigned long long>(transition.checkpointSequence));
            std::scoped_lock lock{ remoteChangesLock };
            pendingDisconnects.erase(transition.playerID);
            pendingMountTransitions.push_back(std::move(transition));
        }

        void OnRemoteAppearance(std::string const&, sio::message::ptr const& a_data, bool, sio::message::list&)
        {
            PROFILE_FUNCTION();
            RemotePlayerAppearance update;
            if (const auto* rejectedField = ParseRemoteAppearance(a_data, update))
            {
                logs::warn("Ignored malformed Socket.IO player:appearance payload: player={} field={}",
                    update.playerID.empty() ? "<unknown>" : update.playerID, rejectedField);
                return;
            }

            const auto playerID = update.playerID;
            std::scoped_lock lock{ remoteChangesLock };
            pendingDisconnects.erase(playerID);

            const auto latest = latestReceivedAppearanceRevisions.find(playerID);
            if (latest != latestReceivedAppearanceRevisions.end() && update.appearance.revision <= latest->second)
            {
                logs::debug("Appearance sync ignored stale appearance: player={} revision={} newestRevision={}",
                    playerID, static_cast<unsigned long long>(update.appearance.revision),
                    static_cast<unsigned long long>(latest->second));
                return;
            }
            latestReceivedAppearanceRevisions.insert_or_assign(playerID, update.appearance.revision);
            pendingAppearances.insert_or_assign(playerID, std::move(update));
        }

        void OnRemoteEquipment(std::string const&, sio::message::ptr const& a_data, bool, sio::message::list&)
        {
            PROFILE_FUNCTION();
            RemotePlayerEquipment update;
            if (const auto* rejectedField = ParseRemoteEquipment(a_data, update))
            {
                logs::warn("Ignored malformed Socket.IO player:equipment payload: player={} field={}",
                    update.playerID.empty() ? "<unknown>" : update.playerID, rejectedField);
                return;
            }

            const auto playerID = update.playerID;
            std::scoped_lock lock{ remoteChangesLock };
            pendingDisconnects.erase(playerID);

            const auto latest = latestReceivedEquipmentRevisions.find(playerID);
            if (latest != latestReceivedEquipmentRevisions.end() && update.equipment.revision <= latest->second)
            {
                logs::debug("Equipment sync ignored stale equipment: player={} revision={} newestRevision={}", playerID,
                    static_cast<unsigned long long>(update.equipment.revision),
                    static_cast<unsigned long long>(latest->second));
                return;
            }
            latestReceivedEquipmentRevisions.insert_or_assign(playerID, update.equipment.revision);
            pendingEquipment.insert_or_assign(playerID, std::move(update));
        }

        bool ParseRemoteChatMessage(const sio::message::ptr& a_data, Multiplayer::ChatMessage& a_result)
        {
            if (!a_data || a_data->get_flag() != sio::message::flag_object)
            {
                return false;
            }

            const auto& values = a_data->get_map();
            const auto name = FindValue(values, "name");
            const auto text = FindValue(values, "text");
            if (!ReadString(values, "playerId", a_result.playerID) || !name
                || name->get_flag() != sio::message::flag_string || !text
                || text->get_flag() != sio::message::flag_string)
            {
                return false;
            }

            a_result.senderName = name->get_string();
            a_result.text = text->get_string();
            return Multiplayer::IsValidAppearanceName(a_result.senderName)
                && Multiplayer::IsValidChatText(a_result.text);
        }

        void OnRemoteChatMessage(std::string const&, sio::message::ptr const& a_data, bool, sio::message::list&)
        {
            PROFILE_FUNCTION();
            Multiplayer::ChatMessage message;
            if (!ParseRemoteChatMessage(a_data, message))
            {
                logs::warn("Ignored malformed Socket.IO chat:message payload");
                return;
            }

            std::scoped_lock lock{ chatMessagesLock };
            if (pendingChatMessages.size() >= kMaximumPendingChatMessages)
            {
                pendingChatMessages.pop_front();
            }
            pendingChatMessages.push_back(std::move(message));
        }

        // Drops every pending change and per-player receive history, as if each
        // remote player had left. Requires remoteChangesLock.
        void ForgetRemotePlayersLocked()
        {
            pendingTransforms.clear();
            latestReceivedTransforms.clear();
            pendingAppearances.clear();
            latestReceivedAppearanceRevisions.clear();
            pendingEquipment.clear();
            latestReceivedEquipmentRevisions.clear();
            pendingAnimationActions.clear();
            latestReceivedActionSequences.clear();
            pendingMountTransitions.clear();
            pendingExits.clear();
            pendingDisconnects.clear();
        }

        // Remote player IDs are the server's per-connection sids, so every known
        // remote player is gone once our connection closes or is replaced. The
        // connection state changes under the lock Drain uses, so a drain never
        // mixes changes from different connections.
        void MarkDisconnected()
        {
            std::scoped_lock lock{ remoteChangesLock };
            connected.store(false, std::memory_order_release);
            attemptFailed.store(true, std::memory_order_release);
            ForgetRemotePlayersLocked();
        }

        bool ReadPlayerID(const sio::message::ptr& a_data, std::string& a_result)
        {
            return a_data && a_data->get_flag() == sio::message::flag_object
                && ReadString(a_data->get_map(), "playerId", a_result);
        }

        // Forgets the player's checkpoint and action stream. Requires
        // remoteChangesLock.
        void ForgetRemoteWorldStateLocked(const std::string& a_playerID)
        {
            pendingTransforms.erase(a_playerID);
            latestReceivedTransforms.erase(a_playerID);
            latestReceivedActionSequences.erase(a_playerID);
            std::erase_if(pendingAnimationActions,
                [&a_playerID](const AnimationAction& a_action) { return a_action.playerID == a_playerID; });
            std::erase_if(pendingMountTransitions, [&a_playerID](const RemoteMountTransition& a_transition) {
                return a_transition.playerID == a_playerID;
            });
        }

        void OnRemoteExit(std::string const&, sio::message::ptr const& a_data, bool, sio::message::list&)
        {
            PROFILE_FUNCTION();
            std::string playerID;
            if (!ReadPlayerID(a_data, playerID))
            {
                logs::warn("Ignored malformed Socket.IO player:exit payload");
                return;
            }

            logs::debug("Remote player {} left the world", playerID);
            std::scoped_lock lock{ remoteChangesLock };
            ForgetRemoteWorldStateLocked(playerID);
            // Unlike a disconnect, a later message does not cancel the exit: the
            // main-loop pass must drop the old actor before a new checkpoint.
            pendingExits.insert(std::move(playerID));
        }

        void OnRemoteDisconnect(std::string const&, sio::message::ptr const& a_data, bool, sio::message::list&)
        {
            PROFILE_FUNCTION();
            std::string playerID;
            if (!ReadPlayerID(a_data, playerID))
            {
                logs::warn("Ignored malformed Socket.IO player:disconnect payload");
                return;
            }

            std::scoped_lock lock{ remoteChangesLock };
            ForgetRemoteWorldStateLocked(playerID);
            pendingAppearances.erase(playerID);
            latestReceivedAppearanceRevisions.erase(playerID);
            pendingEquipment.erase(playerID);
            latestReceivedEquipmentRevisions.erase(playerID);
            pendingExits.erase(playerID);
            pendingDisconnects.insert(std::move(playerID));
        }

        // The client discards the socket object, with its handlers, when the
        // server closes the namespace, so every attempt registers them again on
        // whichever object it is about to open.
        void RegisterSocketHandlers()
        {
            const auto socket = GetSocket();
            socket->on("player:state", sio::socket::event_listener_aux(OnRemoteTransform));
            socket->on("player:action", sio::socket::event_listener_aux(OnRemoteAnimationAction));
            socket->on("player:mount", sio::socket::event_listener_aux(OnRemoteMountTransition));
            socket->on("player:appearance", sio::socket::event_listener_aux(OnRemoteAppearance));
            socket->on("player:equipment", sio::socket::event_listener_aux(OnRemoteEquipment));
            socket->on("player:exit", sio::socket::event_listener_aux(OnRemoteExit));
            socket->on("player:disconnect", sio::socket::event_listener_aux(OnRemoteDisconnect));
            socket->on("chat:message", sio::socket::event_listener_aux(OnRemoteChatMessage));
        }
    }

    void Initialize()
    {
        static std::once_flag connectOnce;

        std::call_once(connectOnce, [] {
            auto& client = GetClient();
            RegisterSocketHandlers();

            // The library's own retry gives up on any close with a normal status,
            // which is how a server drops a client, and leaves the namespace closed
            // on a live transport. Every loss is reported once and recovered by
            // ConnectionMonitor instead.
            client.set_reconnect_attempts(0);

            client.set_open_listener([] {
                PROFILE_THREAD_NAME("Socket.IO");
                RE::ConsoleLog::GetSingleton()->Print("MP: Connected.");
                logs::info("Socket.IO transport connected to {}", GetServerEndpoint());
            });
            client.set_socket_open_listener([](std::string const&) {
                // A reopened namespace does not always follow a reported close, so the
                // connected flag alone cannot reveal that the server lost our state.
                unsigned long long generation;
                bool wasConnected;
                {
                    std::scoped_lock lock{ remoteChangesLock };
                    ForgetRemotePlayersLocked();
                    generation = connectionGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
                    wasConnected = connected.exchange(true, std::memory_order_acq_rel);
                }
                logs::info("Socket.IO default namespace ready: connectionGeneration={} closeObserved={}", generation,
                    !wasConnected);
            });
            client.set_socket_close_listener([](std::string const&) {
                MarkDisconnected();
                RE::ConsoleLog::GetSingleton()->Print("MP: Connection closed.");
                logs::info("Socket.IO namespace closed; forgetting remote players");
            });
            client.set_fail_listener([] {
                MarkDisconnected();
                logs::warn("Socket.IO could not connect to {}", GetServerEndpoint());
            });
            client.set_close_listener([](sio::client::close_reason const& a_reason) {
                MarkDisconnected();
                logs::info("Socket.IO connection closed by {}; forgetting remote players",
                    a_reason == sio::client::close_reason_drop ? "the transport" : "a normal close");
            });

            logs::info("Opening Socket.IO connection to {}", GetServerEndpoint());
            client.connect(GetServerEndpoint());
        });
    }

    bool IsConnected()
    {
        return connected.load(std::memory_order_acquire);
    }

    std::uint64_t GetConnectionGeneration()
    {
        return connectionGeneration.load(std::memory_order_acquire);
    }

    void PublishLocalWorldEnter()
    {
        if (IsConnected())
        {
            GetSocket()->emit("player:enter");
        }
    }

    void PublishLocalWorldExit()
    {
        if (IsConnected())
        {
            GetSocket()->emit("player:exit");
        }
    }

    void PublishLocalPlayerTransform(const PlayerTransform& a_transform)
    {
        PROFILE_FUNCTION();
        if (!IsConnected())
        {
            return;
        }

        auto payload = sio::object_message::create();
        auto& values = payload->get_map();
        values["x"] = sio::double_message::create(a_transform.x);
        values["y"] = sio::double_message::create(a_transform.y);
        values["z"] = sio::double_message::create(a_transform.z);
        values["heading"] = sio::double_message::create(a_transform.heading);
        values["speed"] = sio::double_message::create(a_transform.speed);
        values["direction"] = sio::double_message::create(a_transform.direction);
        values["movementState"]
            = sio::string_message::create(Multiplayer::GetMovementStateName(a_transform.animationState.movementState));
        values["jumping"] = sio::bool_message::create(a_transform.animationState.jumping);
        values["weaponDrawn"] = sio::bool_message::create(a_transform.animationState.weaponDrawn);
        values["bowDrawn"] = sio::bool_message::create(a_transform.handPresentation.bowDrawn);
        values["castingLeft"] = sio::bool_message::create(a_transform.handPresentation.castingLeft);
        values["castingRight"] = sio::bool_message::create(a_transform.handPresentation.castingRight);
        values["blocking"] = sio::bool_message::create(a_transform.handPresentation.blocking);
        values["dead"] = sio::bool_message::create(a_transform.dead);
        values["mounted"] = sio::bool_message::create(a_transform.mounted);
        values["swimming"] = sio::bool_message::create(a_transform.swimming);
        values["cellId"] = sio::int_message::create(a_transform.cellID);
        values["worldspaceId"] = sio::int_message::create(a_transform.worldspaceID);
        values["furnitureRefId"] = sio::int_message::create(a_transform.furnitureRefID);
        values["graphEpoch"] = sio::int_message::create(static_cast<std::int64_t>(a_transform.graphEpoch));
        values["sequence"] = sio::int_message::create(static_cast<std::int64_t>(a_transform.sequence));

        GetSocket()->emit("player:update", payload);
    }

    void PublishLocalAppearance(const Multiplayer::PlayerAppearance& a_appearance)
    {
        PROFILE_FUNCTION();
        if (!IsConnected() || !Multiplayer::IsValidAppearance(a_appearance))
        {
            return;
        }

        const auto createColor = [](const Multiplayer::AppearanceColor& a_color) {
            auto color = sio::object_message::create();
            auto& values = color->get_map();
            values["r"] = sio::int_message::create(a_color.red);
            values["g"] = sio::int_message::create(a_color.green);
            values["b"] = sio::int_message::create(a_color.blue);
            return color;
        };
        auto headParts = sio::array_message::create();
        for (const auto& headPart : a_appearance.headParts)
        {
            auto reference = CreateFormReference(headPart.form);
            reference->get_map()["type"] = sio::int_message::create(headPart.type);
            headParts->get_vector().push_back(std::move(reference));
        }

        auto faceMorphs = sio::array_message::create();
        for (const auto morph : a_appearance.faceMorphs)
        {
            faceMorphs->get_vector().push_back(sio::double_message::create(morph));
        }

        auto faceParts = sio::array_message::create();
        for (const auto part : a_appearance.faceParts)
        {
            faceParts->get_vector().push_back(sio::int_message::create(part));
        }

        auto tints = sio::array_message::create();
        for (const auto& tint : a_appearance.tints)
        {
            auto layer = createColor(tint.color);
            auto& values = layer->get_map();
            values["index"] = sio::int_message::create(tint.index);
            values["type"] = sio::int_message::create(tint.type);
            values["alpha"] = sio::double_message::create(tint.alpha);
            tints->get_vector().push_back(std::move(layer));
        }

        auto payload = sio::object_message::create();
        auto& values = payload->get_map();
        values["revision"] = sio::int_message::create(static_cast<std::int64_t>(a_appearance.revision));
        values["name"] = sio::string_message::create(a_appearance.name);
        values["race"] = CreateFormReference(a_appearance.race);
        values["female"] = sio::bool_message::create(a_appearance.female);
        values["weight"] = sio::double_message::create(a_appearance.weight);
        values["height"] = sio::double_message::create(a_appearance.height);
        values["headParts"] = std::move(headParts);
        values["hairColor"]
            = a_appearance.hairColor ? createColor(*a_appearance.hairColor) : sio::null_message::create();
        values["faceDetails"] = CreateFormReference(a_appearance.faceDetails);
        values["bodyTint"] = createColor(a_appearance.bodyTint);
        values["faceMorphs"] = std::move(faceMorphs);
        values["faceParts"] = std::move(faceParts);
        values["tints"] = std::move(tints);

        GetSocket()->emit("player:appearance", payload);
    }

    void PublishLocalEquipment(const Multiplayer::PlayerEquipment& a_equipment)
    {
        PROFILE_FUNCTION();
        if (!IsConnected() || !Multiplayer::IsValidEquipment(a_equipment))
        {
            return;
        }

        const auto createHand = [](const std::optional<Multiplayer::EquipmentHand>& a_hand) {
            if (!a_hand)
            {
                return sio::null_message::create();
            }
            if (a_hand->kind == Multiplayer::EquipmentHandKind::kSpell)
            {
                auto hand = sio::object_message::create();
                hand->get_map()["kind"] = sio::string_message::create("spell");
                return hand;
            }
            auto hand = CreateFormReference(a_hand->form);
            auto& values = hand->get_map();
            values["kind"]
                = sio::string_message::create(std::string{ Multiplayer::GetEquipmentHandKindName(a_hand->kind) });
            if (a_hand->kind == Multiplayer::EquipmentHandKind::kWeapon)
            {
                values["weaponType"] = sio::int_message::create(a_hand->weaponType);
            }
            return hand;
        };

        auto armor = sio::array_message::create();
        for (const auto& entry : a_equipment.armor)
        {
            auto reference = CreateFormReference(entry.form);
            auto& values = reference->get_map();
            values["slotMask"] = sio::int_message::create(entry.slotMask);
            values["armorType"]
                = sio::string_message::create(std::string{ Multiplayer::GetEquipmentArmorTypeName(entry.armorType) });
            armor->get_vector().push_back(std::move(reference));
        }

        auto payload = sio::object_message::create();
        auto& values = payload->get_map();
        values["revision"] = sio::int_message::create(static_cast<std::int64_t>(a_equipment.revision));
        values["armor"] = std::move(armor);
        values["rightHand"] = createHand(a_equipment.rightHand);
        values["leftHand"] = createHand(a_equipment.leftHand);
        values["ammo"] = CreateFormReference(a_equipment.ammo);

        GetSocket()->emit("player:equipment", payload);
    }

    void PublishLocalAttack(const Attack& a_attack, std::uint64_t a_graphEpoch, std::uint64_t a_checkpointSequence)
    {
        PROFILE_FUNCTION();
        if (!IsConnected() || a_graphEpoch == 0 || a_checkpointSequence == 0)
        {
            return;
        }

        if (localActionSequence >= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
        {
            localActionSequence = 0;
        }
        ++localActionSequence;

        auto prerequisites = sio::object_message::create();
        prerequisites->get_map()["weaponDrawn"] = sio::bool_message::create(true);
        auto payload = sio::object_message::create();
        auto& values = payload->get_map();
        values["actionSequence"] = sio::int_message::create(static_cast<std::int64_t>(localActionSequence));
        values["graphEpoch"] = sio::int_message::create(static_cast<std::int64_t>(a_graphEpoch));
        values["checkpointSequence"] = sio::int_message::create(static_cast<std::int64_t>(a_checkpointSequence));
        values["family"] = sio::string_message::create("attack");
        values["action"] = sio::string_message::create(GetAttackActionName(a_attack.action));
        values["power"] = sio::bool_message::create(a_attack.power);
        if (a_attack.power)
        {
            values["direction"] = sio::string_message::create(GetPowerAttackDirectionName(a_attack.direction));
        }
        values["prerequisites"] = std::move(prerequisites);

        logs::debug(
            "Animation sync publishing attack: action={} power={} direction={} actionSequence={} graphEpoch={} "
            "checkpointSequence={}",
            GetAttackActionName(a_attack.action), a_attack.power, GetPowerAttackDirectionName(a_attack.direction),
            static_cast<unsigned long long>(localActionSequence), static_cast<unsigned long long>(a_graphEpoch),
            static_cast<unsigned long long>(a_checkpointSequence));
        GetSocket()->emit("animation:action", payload);
    }

    void PublishLocalMountTransition(
        MountTransition a_transition, std::uint64_t a_graphEpoch, std::uint64_t a_checkpointSequence)
    {
        PROFILE_FUNCTION();
        if (!IsConnected() || a_graphEpoch == 0 || a_checkpointSequence == 0)
        {
            return;
        }

        auto payload = sio::object_message::create();
        auto& values = payload->get_map();
        values["transition"] = sio::string_message::create(GetMountTransitionName(a_transition));
        values["graphEpoch"] = sio::int_message::create(static_cast<std::int64_t>(a_graphEpoch));
        values["checkpointSequence"] = sio::int_message::create(static_cast<std::int64_t>(a_checkpointSequence));

        logs::debug("Mount sync publishing transition: transition={} graphEpoch={} checkpointSequence={}",
            GetMountTransitionName(a_transition), static_cast<unsigned long long>(a_graphEpoch),
            static_cast<unsigned long long>(a_checkpointSequence));
        GetSocket()->emit("mount:transition", payload);
    }

    bool PublishChatMessage(std::string_view a_text, std::function<void(std::string_view)> a_onRejected)
    {
        if (!IsConnected())
        {
            return false;
        }

        auto payload = sio::object_message::create();
        payload->get_map()["text"] = sio::string_message::create(std::string{ a_text });
        GetSocket()->emit("chat:send", payload,
            [onRejected = std::move(a_onRejected)](sio::message::list const& a_acknowledgement) {
                const auto& result = a_acknowledgement.size() > 0 ? a_acknowledgement[0] : sio::message::ptr{};
                if (!result || result->get_flag() != sio::message::flag_object)
                {
                    return;
                }
                bool ok = false;
                if (ReadBool(result->get_map(), "ok", ok) && ok)
                {
                    return;
                }
                std::string error;
                ReadString(result->get_map(), "error", error);
                logs::info("Server rejected chat message: {}", error.empty() ? "<no reason>" : error);
                onRejected(error);
            });
        return true;
    }

    void EmitLatencyProbe(std::function<void()> a_onAcknowledged)
    {
        if (!IsConnected())
        {
            return;
        }

        GetSocket()->emit("latency:probe", nullptr,
            [onAcknowledged = std::move(a_onAcknowledged)](sio::message::list const&) { onAcknowledged(); });
    }

    void AbandonConnection()
    {
        logs::warn("Abandoning the Socket.IO connection to {}; forgetting remote players", GetServerEndpoint());
        MarkDisconnected();
    }

    bool BeginReconnect()
    {
        if (reconnectWorkerRunning.exchange(true, std::memory_order_acq_rel))
        {
            return false;
        }

        // The client cannot be reopened from its own transport thread, and
        // closing a wedged transport waits on its close handshake.
        std::thread([] {
            try
            {
                auto& client = GetClient();
                // Joins the old transport thread, so nothing from the dead
                // connection is reported once the new attempt starts.
                client.sync_close();
                RegisterSocketHandlers();
                attemptFailed.store(false, std::memory_order_release);
                client.connect(GetServerEndpoint());
            }
            catch (const std::exception& a_exception)
            {
                logs::error("Socket.IO reconnect attempt threw: {}", a_exception.what());
                attemptFailed.store(true, std::memory_order_release);
            }
            reconnectWorkerRunning.store(false, std::memory_order_release);
        }).detach();
        return true;
    }

    bool DidReconnectAttemptFail()
    {
        return !reconnectWorkerRunning.load(std::memory_order_acquire) && attemptFailed.load(std::memory_order_acquire);
    }

    RemotePlayerChanges DrainRemotePlayerChanges()
    {
        PROFILE_FUNCTION();
        RemotePlayerChanges changes;
        std::scoped_lock lock{ remoteChangesLock };

        // An abandoned transport may still deliver events until it is replaced.
        if (!connected.load(std::memory_order_acquire))
        {
            ForgetRemotePlayersLocked();
        }

        changes.transforms.reserve(pendingTransforms.size());
        for (auto& entry : pendingTransforms)
        {
            changes.transforms.push_back(std::move(entry.second));
        }
        pendingTransforms.clear();

        changes.appearances.reserve(pendingAppearances.size());
        for (auto& entry : pendingAppearances)
        {
            changes.appearances.push_back(std::move(entry.second));
        }
        pendingAppearances.clear();

        changes.equipment.reserve(pendingEquipment.size());
        for (auto& entry : pendingEquipment)
        {
            changes.equipment.push_back(std::move(entry.second));
        }
        pendingEquipment.clear();

        changes.animationActions.reserve(pendingAnimationActions.size());
        while (!pendingAnimationActions.empty())
        {
            changes.animationActions.push_back(std::move(pendingAnimationActions.front()));
            pendingAnimationActions.pop_front();
        }

        changes.mountTransitions.reserve(pendingMountTransitions.size());
        while (!pendingMountTransitions.empty())
        {
            changes.mountTransitions.push_back(std::move(pendingMountTransitions.front()));
            pendingMountTransitions.pop_front();
        }

        changes.exitedPlayerIDs.reserve(pendingExits.size());
        for (auto& playerID : pendingExits)
        {
            changes.exitedPlayerIDs.push_back(std::move(playerID));
        }
        pendingExits.clear();

        changes.disconnectedPlayerIDs.reserve(pendingDisconnects.size());
        for (auto& playerID : pendingDisconnects)
        {
            changes.disconnectedPlayerIDs.push_back(std::move(playerID));
        }
        pendingDisconnects.clear();

        changes.connectionGeneration = connectionGeneration.load(std::memory_order_acquire);
        changes.connected = connected.load(std::memory_order_acquire);

        return changes;
    }

    std::vector<Multiplayer::ChatMessage> DrainChatMessages()
    {
        std::vector<Multiplayer::ChatMessage> messages;
        std::scoped_lock lock{ chatMessagesLock };
        messages.reserve(pendingChatMessages.size());
        for (auto& message : pendingChatMessages)
        {
            messages.push_back(std::move(message));
        }
        pendingChatMessages.clear();
        return messages;
    }
}
