#include "network/SocketIOClient.h"
#include "network/LatencyProbe.h"

#include <sio_client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Network
{
    namespace
    {
        constexpr auto kDefaultServerEndpoint = "https://8fa21dsv.lmfao.pp.ua";
        constexpr auto kServerEndpointPath = "Data/SKSE/Plugins/skyrim-mp-server.txt";
        std::atomic_bool connected{ false };
        std::atomic_uint64_t connectionGeneration{ 0 };
        std::atomic_uint64_t serverResponseCount{ 0 };
        std::mutex remoteChangesLock;
        std::unordered_map<std::string, RemotePlayerTransform> pendingTransforms;
        std::unordered_map<std::string, PlayerTransform> latestReceivedTransforms;
        std::unordered_map<std::string, RemotePlayerAppearance> pendingAppearances;
        std::unordered_map<std::string, std::uint64_t> latestReceivedAppearanceRevisions;
        std::unordered_map<std::string, RemotePlayerEquipment> pendingEquipment;
        std::unordered_map<std::string, std::uint64_t> latestReceivedEquipmentRevisions;
        std::deque<AnimationAction> pendingAnimationActions;
        std::unordered_map<std::string, std::uint64_t> latestReceivedActionSequences;
        std::unordered_set<std::string> pendingDisconnects;
        std::uint64_t localActionSequence = 0;

        const std::string& GetServerEndpoint()
        {
            static const std::string endpoint = [] {
                std::ifstream input{ kServerEndpointPath };
                std::string configuredEndpoint;
                if (!std::getline(input, configuredEndpoint))
                {
                    return std::string{ kDefaultServerEndpoint };
                }

                const auto first = configuredEndpoint.find_first_not_of(" \t\r\n");
                const auto last = configuredEndpoint.find_last_not_of(" \t\r\n");
                if (first == std::string::npos)
                {
                    return std::string{ kDefaultServerEndpoint };
                }
                configuredEndpoint = configuredEndpoint.substr(first, last - first + 1);

                if (!configuredEndpoint.starts_with("http://") && !configuredEndpoint.starts_with("https://"))
                {
                    logs::warn(
                        "Ignoring invalid endpoint in {}; using {}", kServerEndpointPath, kDefaultServerEndpoint);
                    return std::string{ kDefaultServerEndpoint };
                }

                return configuredEndpoint;
            }();

            return endpoint;
        }

        sio::client& GetClient()
        {
            // Keep the Socket.IO client alive for the lifetime of the SKSE DLL.
            static sio::client client;
            return client;
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
                || !ReadOptionalBool(values, "blocking", handPresentation.blocking)
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

            // Optional transport metadata: a malformed stamp only loses the delay
            // sample, never the checkpoint.
            double sentAt = 0.0;
            if (ReadNumber(values, "sentAtMs", sentAt) && sentAt >= 0.0)
            {
                a_result.sentAtServerMilliseconds = sentAt;
            }
            else
            {
                a_result.sentAtServerMilliseconds.reset();
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
                logs::info(
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
                    logs::info(
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
            logs::info(
                "Animation sync received attack: player={} action={} power={} direction={} actionSequence={} "
                "graphEpoch={} checkpointSequence={}",
                action.playerID, GetAttackActionName(action.attack.action), action.attack.power,
                GetPowerAttackDirectionName(action.attack.direction),
                static_cast<unsigned long long>(action.actionSequence),
                static_cast<unsigned long long>(action.graphEpoch),
                static_cast<unsigned long long>(action.checkpointSequence));
            pendingAnimationActions.push_back(std::move(action));
        }

        void OnRemoteAppearance(std::string const&, sio::message::ptr const& a_data, bool, sio::message::list&)
        {
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
                logs::info("Appearance sync ignored stale appearance: player={} revision={} newestRevision={}",
                    playerID, static_cast<unsigned long long>(update.appearance.revision),
                    static_cast<unsigned long long>(latest->second));
                return;
            }
            latestReceivedAppearanceRevisions.insert_or_assign(playerID, update.appearance.revision);
            pendingAppearances.insert_or_assign(playerID, std::move(update));
        }

        void OnRemoteEquipment(std::string const&, sio::message::ptr const& a_data, bool, sio::message::list&)
        {
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
                logs::info("Equipment sync ignored stale equipment: player={} revision={} newestRevision={}", playerID,
                    static_cast<unsigned long long>(update.equipment.revision),
                    static_cast<unsigned long long>(latest->second));
                return;
            }
            latestReceivedEquipmentRevisions.insert_or_assign(playerID, update.equipment.revision);
            pendingEquipment.insert_or_assign(playerID, std::move(update));
        }

        void OnRemoteDisconnect(std::string const&, sio::message::ptr const& a_data, bool, sio::message::list&)
        {
            if (!a_data || a_data->get_flag() != sio::message::flag_object)
            {
                return;
            }

            std::string playerID;
            if (!ReadString(a_data->get_map(), "playerId", playerID))
            {
                return;
            }

            std::scoped_lock lock{ remoteChangesLock };
            pendingTransforms.erase(playerID);
            latestReceivedTransforms.erase(playerID);
            pendingAppearances.erase(playerID);
            latestReceivedAppearanceRevisions.erase(playerID);
            pendingEquipment.erase(playerID);
            latestReceivedEquipmentRevisions.erase(playerID);
            latestReceivedActionSequences.erase(playerID);
            std::erase_if(pendingAnimationActions,
                [&playerID](const AnimationAction& a_action) { return a_action.playerID == playerID; });
            pendingDisconnects.insert(std::move(playerID));
        }
    }

    void Initialize()
    {
        static std::once_flag connectOnce;

        std::call_once(connectOnce, [] {
            auto& client = GetClient();
            auto socket = client.socket();

            socket->on("player:state", sio::socket::event_listener_aux(OnRemoteTransform));
            socket->on("player:action", sio::socket::event_listener_aux(OnRemoteAnimationAction));
            socket->on("player:appearance", sio::socket::event_listener_aux(OnRemoteAppearance));
            socket->on("player:equipment", sio::socket::event_listener_aux(OnRemoteEquipment));
            socket->on("player:left", sio::socket::event_listener_aux(OnRemoteDisconnect));

            client.set_open_listener([] {
                RE::ConsoleLog::GetSingleton()->Print("MP: Connected.");
                logs::info("Socket.IO transport connected to {}", GetServerEndpoint());
            });
            client.set_socket_open_listener([](std::string const&) {
                // Automatic reconnects do not always report a close first, so the
                // connected flag alone cannot reveal that the server lost our state.
                const auto generation = connectionGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
                const bool wasConnected = connected.exchange(true, std::memory_order_acq_rel);
                logs::info("Socket.IO default namespace ready: connectionGeneration={} closeObserved={}",
                    static_cast<unsigned long long>(generation), !wasConnected);
            });
            client.set_socket_close_listener([](std::string const&) {
                connected.store(false, std::memory_order_release);
                // A local transport interruption does not prove that any remote
                // player left. Keep their remote actors until the server explicitly emits
                // player:left, allowing automatic Socket.IO reconnects to be seamless.
                RE::ConsoleLog::GetSingleton()->Print("MP: Connection closed.");
                logs::info("Socket.IO namespace closed; retaining remote actors");
            });
            client.set_fail_listener([] {
                connected.store(false, std::memory_order_release);
                logs::warn("Socket.IO could not connect to {}", GetServerEndpoint());
            });
            client.set_close_listener([](sio::client::close_reason const&) {
                connected.store(false, std::memory_order_release);
                logs::info("Socket.IO connection closed; retaining remote actors");
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

    std::uint64_t GetServerResponseCount()
    {
        return serverResponseCount.load(std::memory_order_acquire);
    }

    void PublishLocalPlayerTransform(const PlayerTransform& a_transform)
    {
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
        values["cellId"] = sio::int_message::create(a_transform.cellID);
        values["worldspaceId"] = sio::int_message::create(a_transform.worldspaceID);
        values["furnitureRefId"] = sio::int_message::create(a_transform.furnitureRefID);
        values["graphEpoch"] = sio::int_message::create(static_cast<std::int64_t>(a_transform.graphEpoch));
        values["sequence"] = sio::int_message::create(static_cast<std::int64_t>(a_transform.sequence));
        if (const auto serverTime = GetServerTimeMilliseconds())
        {
            values["sentAtMs"] = sio::double_message::create(*serverTime);
        }

        GetClient().socket()->emit("player:update", payload);
    }

    void PublishLocalAppearance(const Multiplayer::PlayerAppearance& a_appearance)
    {
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

        GetClient().socket()->emit("player:appearance", payload);
    }

    void PublishLocalEquipment(const Multiplayer::PlayerEquipment& a_equipment)
    {
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

        GetClient().socket()->emit("player:equipment", payload);
    }

    void PublishLocalAttack(const Attack& a_attack, std::uint64_t a_graphEpoch, std::uint64_t a_checkpointSequence)
    {
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

        logs::info(
            "Animation sync publishing attack: action={} power={} direction={} actionSequence={} graphEpoch={} "
            "checkpointSequence={}",
            GetAttackActionName(a_attack.action), a_attack.power, GetPowerAttackDirectionName(a_attack.direction),
            static_cast<unsigned long long>(localActionSequence), static_cast<unsigned long long>(a_graphEpoch),
            static_cast<unsigned long long>(a_checkpointSequence));
        GetClient().socket()->emit("animation:action", payload);
    }

    void EmitLatencyProbe(std::function<void(std::optional<double> a_serverTimeMilliseconds)> a_onAcknowledged)
    {
        if (!IsConnected())
        {
            return;
        }

        GetClient().socket()->emit("latency:probe", nullptr,
            [onAcknowledged = std::move(a_onAcknowledged)](sio::message::list const& a_response) {
                serverResponseCount.fetch_add(1, std::memory_order_acq_rel);
                std::optional<double> serverTime;
                if (a_response.size() > 0 && a_response[0] && a_response[0]->get_flag() == sio::message::flag_object)
                {
                    double value = 0.0;
                    if (ReadNumber(a_response[0]->get_map(), "serverTimeMs", value) && value >= 0.0)
                    {
                        serverTime = value;
                    }
                }
                onAcknowledged(serverTime);
            });
    }

    void ForgetRemotePlayers()
    {
        std::scoped_lock lock{ remoteChangesLock };
        pendingTransforms.clear();
        latestReceivedTransforms.clear();
        pendingAppearances.clear();
        latestReceivedAppearanceRevisions.clear();
        pendingEquipment.clear();
        latestReceivedEquipmentRevisions.clear();
        pendingAnimationActions.clear();
        latestReceivedActionSequences.clear();
        pendingDisconnects.clear();
    }

    RemotePlayerChanges DrainRemotePlayerChanges()
    {
        RemotePlayerChanges changes;
        std::scoped_lock lock{ remoteChangesLock };

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

        changes.disconnectedPlayerIDs.reserve(pendingDisconnects.size());
        for (auto& playerID : pendingDisconnects)
        {
            changes.disconnectedPlayerIDs.push_back(std::move(playerID));
        }
        pendingDisconnects.clear();

        return changes;
    }
}
