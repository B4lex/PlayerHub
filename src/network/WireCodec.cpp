#include "network/WireCodec.h"

#include <pb_decode.h>
#include <pb_encode.h>
#include <playerhub/v1/messages.pb.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>

namespace Network::Wire
{
    namespace
    {
        // The generated ClientMessage union is large, so the envelope is written by hand.
        std::string EncodeClientMessage(pb_size_t a_tag, const pb_msgdesc_t* a_fields, const void* a_value)
        {
            pb_ostream_t sizing = PB_OSTREAM_SIZING;
            if (!pb_encode_tag(&sizing, PB_WT_STRING, a_tag) || !pb_encode_submessage(&sizing, a_fields, a_value))
            {
                return {};
            }
            std::string buffer(sizing.bytes_written, '\0');
            pb_ostream_t stream = pb_ostream_from_buffer(reinterpret_cast<pb_byte_t*>(buffer.data()), buffer.size());
            if (!pb_encode_tag(&stream, PB_WT_STRING, a_tag) || !pb_encode_submessage(&stream, a_fields, a_value))
            {
                return {};
            }
            return buffer;
        }

        template <std::size_t N>
        bool CopyString(char (&a_target)[N], std::string_view a_source)
        {
            if (a_source.size() >= N)
            {
                return false;
            }
            std::memcpy(a_target, a_source.data(), a_source.size());
            a_target[a_source.size()] = '\0';
            return true;
        }

        bool WriteFormReference(const Multiplayer::FormReference& a_reference, playerhub_v1_FormRef& a_result)
        {
            a_result.form_id = a_reference.localFormID;
            return CopyString(a_result.plugin, a_reference.plugin);
        }

        bool WriteOptionalFormReference(
            const Multiplayer::FormReference& a_reference, bool& a_has, playerhub_v1_FormRef& a_result)
        {
            a_has = !a_reference.IsEmpty();
            return !a_has || WriteFormReference(a_reference, a_result);
        }

        playerhub_v1_Color WriteColor(const Multiplayer::AppearanceColor& a_color)
        {
            return playerhub_v1_Color{ a_color.red, a_color.green, a_color.blue };
        }

        playerhub_v1_HandKind WriteHandKind(Multiplayer::EquipmentHandKind a_kind)
        {
            switch (a_kind)
            {
                case Multiplayer::EquipmentHandKind::kTorch:
                    return playerhub_v1_HandKind_HAND_KIND_TORCH;
                case Multiplayer::EquipmentHandKind::kSpell:
                    return playerhub_v1_HandKind_HAND_KIND_SPELL;
                default:
                    return playerhub_v1_HandKind_HAND_KIND_WEAPON;
            }
        }

        bool WriteHand(const Multiplayer::EquipmentHand& a_hand, playerhub_v1_Hand& a_result)
        {
            a_result.kind = WriteHandKind(a_hand.kind);
            a_result.weapon_type = a_hand.weaponType;
            return WriteOptionalFormReference(a_hand.form, a_result.has_form, a_result.form);
        }

        bool WriteArmor(const Multiplayer::EquipmentArmor& a_armor, playerhub_v1_Armor& a_result)
        {
            a_result.slot_mask = a_armor.slotMask;
            a_result.has_form = true;
            return WriteFormReference(a_armor.form, a_result.form);
        }

        bool ReadFormReference(const playerhub_v1_FormRef& a_reference, Multiplayer::FormReference& a_result)
        {
            a_result.plugin = a_reference.plugin;
            a_result.localFormID = a_reference.form_id;
            return !a_result.plugin.empty() && Multiplayer::IsValidFormReference(a_result, false);
        }

        bool ReadOptionalFormReference(
            bool a_has, const playerhub_v1_FormRef& a_reference, Multiplayer::FormReference& a_result)
        {
            a_result = {};
            return !a_has || ReadFormReference(a_reference, a_result);
        }

        bool ReadColor(const playerhub_v1_Color& a_color, Multiplayer::AppearanceColor& a_result)
        {
            if (a_color.r > 0xFF || a_color.g > 0xFF || a_color.b > 0xFF)
            {
                return false;
            }
            a_result = { static_cast<std::uint8_t>(a_color.r), static_cast<std::uint8_t>(a_color.g),
                static_cast<std::uint8_t>(a_color.b) };
            return true;
        }

        bool ReadContext(const playerhub_v1_Context& a_context, CheckpointContext& a_result)
        {
            a_result.cellID = a_context.cell_id;
            a_result.worldspaceID = a_context.worldspace_id;
            a_result.graphEpoch = a_context.graph_epoch;
            return a_result.graphEpoch != 0
                && ReadOptionalFormReference(a_context.has_furniture, a_context.furniture, a_result.furniture);
        }

        QuantizedCheckpoint ReadCheckpoint(const playerhub_v1_Checkpoint& a_checkpoint)
        {
            return QuantizedCheckpoint{ .sequence = a_checkpoint.sequence,
                .x = a_checkpoint.x,
                .y = a_checkpoint.y,
                .z = a_checkpoint.z,
                .heading = a_checkpoint.heading,
                .pitch = a_checkpoint.pitch,
                .speed = a_checkpoint.speed,
                .direction = a_checkpoint.direction,
                .flags = a_checkpoint.flags,
                .time = a_checkpoint.time };
        }

        std::expected<AnimationAction, std::string> ReadAction(
            Multiplayer::PeerSlot a_slot, const playerhub_v1_Action& a_action)
        {
            AnimationAction result{ .playerID = a_slot,
                .actionSequence = a_action.action_sequence,
                .graphEpoch = a_action.graph_epoch,
                .checkpointSequence = a_action.checkpoint_sequence };
            if (result.actionSequence == 0 || result.graphEpoch == 0 || result.checkpointSequence == 0)
            {
                return std::unexpected("action sequences");
            }
            switch (a_action.action)
            {
                case playerhub_v1_AttackAction_ATTACK_ACTION_NORMAL:
                    result.attack.action = AttackAction::kNormal;
                    break;
                case playerhub_v1_AttackAction_ATTACK_ACTION_LEFT:
                    result.attack.action = AttackAction::kLeft;
                    break;
                case playerhub_v1_AttackAction_ATTACK_ACTION_DUAL:
                    result.attack.action = AttackAction::kDual;
                    break;
                case playerhub_v1_AttackAction_ATTACK_ACTION_BASH:
                    result.attack.action = AttackAction::kBash;
                    break;
                default:
                    return std::unexpected("action");
            }
            result.attack.power = a_action.power;
            const auto direction = static_cast<int>(a_action.direction);
            if (a_action.power != (direction != playerhub_v1_PowerDirection_POWER_DIRECTION_NONE)
                || direction > playerhub_v1_PowerDirection_POWER_DIRECTION_RIGHT || direction < 0)
            {
                return std::unexpected("direction");
            }
            if (a_action.power)
            {
                result.attack.direction = static_cast<PowerAttackDirection>(
                    direction - playerhub_v1_PowerDirection_POWER_DIRECTION_STANDING);
            }
            return result;
        }

        const char* ReadAppearance(const playerhub_v1_Appearance& a_appearance, Multiplayer::PlayerAppearance& a_result)
        {
            using Multiplayer::PlayerAppearance;
            a_result.revision = a_appearance.revision;
            a_result.name = a_appearance.name;
            if (a_appearance.level > (std::numeric_limits<std::uint16_t>::max)())
            {
                return "level";
            }
            a_result.level = static_cast<std::uint16_t>(a_appearance.level);
            if (!a_appearance.has_race || !ReadFormReference(a_appearance.race, a_result.race))
            {
                return "race";
            }
            a_result.female = a_appearance.female;
            a_result.weight = a_appearance.weight;
            a_result.height = a_appearance.height;

            a_result.headParts.clear();
            for (pb_size_t i = 0; i < a_appearance.head_parts_count; ++i)
            {
                const auto& part = a_appearance.head_parts[i];
                auto& result = a_result.headParts.emplace_back();
                if (!part.has_form || !ReadFormReference(part.form, result.form)
                    || part.type > PlayerAppearance::kMaximumHeadPartType)
                {
                    return "headParts";
                }
                result.type = static_cast<std::uint8_t>(part.type);
            }

            a_result.hairColor.reset();
            if (a_appearance.has_hair_color && !ReadColor(a_appearance.hair_color, a_result.hairColor.emplace()))
            {
                return "hairColor";
            }
            if (!ReadOptionalFormReference(a_appearance.has_face_details, a_appearance.face_details, a_result.faceDetails))
            {
                return "faceDetails";
            }
            if (!a_appearance.has_body_tint || !ReadColor(a_appearance.body_tint, a_result.bodyTint))
            {
                return "bodyTint";
            }
            if (a_appearance.face_morphs_count != PlayerAppearance::kFaceMorphCount)
            {
                return "faceMorphs";
            }
            std::copy_n(a_appearance.face_morphs, PlayerAppearance::kFaceMorphCount, a_result.faceMorphs.begin());
            if (a_appearance.face_parts_count != PlayerAppearance::kFacePartCount)
            {
                return "faceParts";
            }
            std::copy_n(a_appearance.face_parts, PlayerAppearance::kFacePartCount, a_result.faceParts.begin());

            a_result.tints.clear();
            for (pb_size_t i = 0; i < a_appearance.tints_count; ++i)
            {
                const auto& tint = a_appearance.tints[i];
                auto& result = a_result.tints.emplace_back();
                if (tint.index > (std::numeric_limits<std::uint16_t>::max)() || tint.type > PlayerAppearance::kMaximumTintType
                    || !tint.has_color || !ReadColor(tint.color, result.color))
                {
                    return "tints";
                }
                result.index = static_cast<std::uint16_t>(tint.index);
                result.type = static_cast<std::uint8_t>(tint.type);
                result.alpha = tint.alpha;
            }

            a_result.defaultRace.reset();
            if (a_appearance.has_default_race)
            {
                auto& defaultRace = a_result.defaultRace.emplace();
                defaultRace.female = a_appearance.default_race.female;
                if (!a_appearance.default_race.has_race
                    || !ReadFormReference(a_appearance.default_race.race, defaultRace.race))
                {
                    return "defaultRace";
                }
            }
            return Multiplayer::IsValidAppearance(a_result) ? nullptr : "value ranges";
        }

        bool ReadHand(const playerhub_v1_Hand& a_hand, Multiplayer::EquipmentHand& a_result)
        {
            switch (a_hand.kind)
            {
                case playerhub_v1_HandKind_HAND_KIND_WEAPON:
                    a_result.kind = Multiplayer::EquipmentHandKind::kWeapon;
                    break;
                case playerhub_v1_HandKind_HAND_KIND_TORCH:
                    a_result.kind = Multiplayer::EquipmentHandKind::kTorch;
                    break;
                case playerhub_v1_HandKind_HAND_KIND_SPELL:
                    a_result.kind = Multiplayer::EquipmentHandKind::kSpell;
                    break;
                default:
                    return false;
            }
            if (a_hand.weapon_type > (std::numeric_limits<std::uint8_t>::max)())
            {
                return false;
            }
            a_result.weaponType = static_cast<std::uint8_t>(a_hand.weapon_type);
            return ReadOptionalFormReference(a_hand.has_form, a_hand.form, a_result.form);
        }

        bool ReadArmor(const playerhub_v1_Armor& a_armor, Multiplayer::EquipmentArmor& a_result)
        {
            a_result.slotMask = a_armor.slot_mask;
            return a_armor.has_form && ReadFormReference(a_armor.form, a_result.form);
        }

        const char* ReadEquipment(const playerhub_v1_Equipment& a_equipment, Multiplayer::PlayerEquipment& a_result)
        {
            a_result.revision = a_equipment.revision;
            a_result.armor.clear();
            for (pb_size_t i = 0; i < a_equipment.armor_count; ++i)
            {
                if (!ReadArmor(a_equipment.armor[i], a_result.armor.emplace_back()))
                {
                    return "armor";
                }
            }
            a_result.rightHand.reset();
            if (a_equipment.has_right_hand && !ReadHand(a_equipment.right_hand, a_result.rightHand.emplace()))
            {
                return "rightHand";
            }
            a_result.leftHand.reset();
            if (a_equipment.has_left_hand && !ReadHand(a_equipment.left_hand, a_result.leftHand.emplace()))
            {
                return "leftHand";
            }
            if (!ReadOptionalFormReference(a_equipment.has_ammo, a_equipment.ammo, a_result.ammo))
            {
                return "ammo";
            }
            a_result.defaultOutfit = {};
            if (a_equipment.has_default_outfit)
            {
                const auto& outfit = a_equipment.default_outfit;
                for (pb_size_t i = 0; i < outfit.armor_count; ++i)
                {
                    if (!ReadArmor(outfit.armor[i], a_result.defaultOutfit.armor.emplace_back()))
                    {
                        return "defaultOutfit.armor";
                    }
                }
                for (pb_size_t i = 0; i < outfit.hands_count; ++i)
                {
                    if (!ReadHand(outfit.hands[i], a_result.defaultOutfit.hands.emplace_back()))
                    {
                        return "defaultOutfit.hands";
                    }
                }
                if (!ReadOptionalFormReference(outfit.has_ammo, outfit.ammo, a_result.defaultOutfit.ammo))
                {
                    return "defaultOutfit.ammo";
                }
            }
            return Multiplayer::IsValidEquipment(a_result) ? nullptr : "value ranges";
        }

        bool ReadChatSpeaker(const playerhub_v1_ChatSpeaker& a_speaker, Multiplayer::ChatSpeaker& a_result)
        {
            a_result = Multiplayer::ChatSpeaker{ .playerID = a_speaker.slot,
                .name = a_speaker.name,
                .level = static_cast<std::uint16_t>(a_speaker.level),
                .session = a_speaker.session_id };
            return a_speaker.slot != 0 && a_speaker.session_id != 0
                && a_speaker.level <= (std::numeric_limits<std::uint16_t>::max)()
                && Multiplayer::IsValidAppearanceName(a_result.name);
        }

        std::optional<Multiplayer::ChatChannel> ReadChatChannel(playerhub_v1_ChatChannel a_channel)
        {
            switch (a_channel)
            {
                case playerhub_v1_ChatChannel_CHAT_CHANNEL_GLOBAL:
                    return Multiplayer::ChatChannel::kGlobal;
                case playerhub_v1_ChatChannel_CHAT_CHANNEL_NEARBY:
                    return Multiplayer::ChatChannel::kNearby;
                case playerhub_v1_ChatChannel_CHAT_CHANNEL_WHISPER:
                    return Multiplayer::ChatChannel::kWhisper;
            }
            return std::nullopt;
        }

        playerhub_v1_ChatChannel WriteChatChannel(Multiplayer::ChatChannel a_channel)
        {
            switch (a_channel)
            {
                case Multiplayer::ChatChannel::kNearby:
                    return playerhub_v1_ChatChannel_CHAT_CHANNEL_NEARBY;
                case Multiplayer::ChatChannel::kWhisper:
                    return playerhub_v1_ChatChannel_CHAT_CHANNEL_WHISPER;
                case Multiplayer::ChatChannel::kGlobal:
                    break;
            }
            return playerhub_v1_ChatChannel_CHAT_CHANNEL_GLOBAL;
        }

        bool WriteTradeItem(const Multiplayer::TradeItem& a_item, playerhub_v1_TradeItem& a_result)
        {
            if (!Multiplayer::IsValidTradeItem(a_item))
            {
                return false;
            }
            a_result = playerhub_v1_TradeItem_init_zero;
            a_result.has_item = true;
            a_result.count = a_item.count;
            a_result.maximum_charge = a_item.maximumCharge;
            a_result.charge = a_item.charge;
            a_result.health = a_item.health;
            a_result.soul = a_item.soul;
            bool written = WriteFormReference(a_item.item, a_result.item)
                && WriteOptionalFormReference(a_item.enchantment, a_result.has_enchantment, a_result.enchantment)
                && CopyString(a_result.name, a_item.name);
            a_result.effects_count = static_cast<pb_size_t>(a_item.effects.size());
            for (std::size_t i = 0; i < a_item.effects.size(); ++i)
            {
                const auto& effect = a_item.effects[i];
                auto& result = a_result.effects[i];
                result.has_effect = true;
                result.magnitude = effect.magnitude;
                result.area = effect.area;
                result.duration = effect.duration;
                result.cost = effect.cost;
                written = written && WriteFormReference(effect.effect, result.effect);
            }
            return written;
        }

        bool ReadTradeItem(const playerhub_v1_TradeItem& a_item, Multiplayer::TradeItem& a_result)
        {
            if (!a_item.has_item || !ReadFormReference(a_item.item, a_result.item)
                || !ReadOptionalFormReference(a_item.has_enchantment, a_item.enchantment, a_result.enchantment)
                || a_item.maximum_charge > Multiplayer::TradeItem::kMaximumCharge
                || a_item.soul > Multiplayer::TradeItem::kMaximumSoul)
            {
                return false;
            }
            a_result.count = a_item.count;
            a_result.maximumCharge = static_cast<std::uint16_t>(a_item.maximum_charge);
            a_result.charge = a_item.charge;
            a_result.health = a_item.health;
            a_result.name = a_item.name;
            a_result.soul = static_cast<std::uint8_t>(a_item.soul);
            a_result.effects.clear();
            for (pb_size_t i = 0; i < a_item.effects_count; ++i)
            {
                const auto& effect = a_item.effects[i];
                auto& result = a_result.effects.emplace_back();
                if (!effect.has_effect || !ReadFormReference(effect.effect, result.effect))
                {
                    return false;
                }
                result.magnitude = effect.magnitude;
                result.area = effect.area;
                result.duration = effect.duration;
                result.cost = effect.cost;
            }
            return Multiplayer::IsValidTradeItem(a_result);
        }

        std::optional<Multiplayer::TradeCloseReason> ReadTradeCloseReason(playerhub_v1_TradeCloseReason a_reason)
        {
            if (a_reason < playerhub_v1_TradeCloseReason_TRADE_CLOSE_REASON_DECLINED
                || a_reason > playerhub_v1_TradeCloseReason_TRADE_CLOSE_REASON_UNAVAILABLE)
            {
                return std::nullopt;
            }
            static_assert(static_cast<int>(Multiplayer::TradeCloseReason::kUnavailable)
                == playerhub_v1_TradeCloseReason_TRADE_CLOSE_REASON_UNAVAILABLE);
            return static_cast<Multiplayer::TradeCloseReason>(a_reason);
        }

        std::expected<ServerMessage, std::string> DecodeTradeOffer(pb_istream_t& a_stream)
        {
            Multiplayer::TradeOffer offer;
            pb_istream_t message;
            if (!pb_make_string_substream(&a_stream, &message))
            {
                return std::unexpected("trade offer framing");
            }
            while (message.bytes_left > 0)
            {
                pb_wire_type_t wireType;
                std::uint32_t tag = 0;
                bool eof = false;
                if (!pb_decode_tag(&message, &wireType, &tag, &eof))
                {
                    return std::unexpected("trade offer field tag");
                }
                if (tag == playerhub_v1_TradeOffer_trade_id_tag && wireType == PB_WT_VARINT)
                {
                    if (!pb_decode_varint32(&message, &offer.id))
                    {
                        return std::unexpected("trade offer id");
                    }
                    continue;
                }
                if (tag == playerhub_v1_TradeOffer_revision_tag && wireType == PB_WT_VARINT)
                {
                    if (!pb_decode_varint(&message, &offer.revision))
                    {
                        return std::unexpected("trade offer revision");
                    }
                    continue;
                }
                if (tag != playerhub_v1_TradeOffer_items_tag || wireType != PB_WT_STRING)
                {
                    if (!pb_skip_field(&message, wireType))
                    {
                        return std::unexpected("trade offer field");
                    }
                    continue;
                }
                if (offer.items.size() >= Multiplayer::kMaximumTradeItems)
                {
                    return std::unexpected("trade offer item count");
                }
                // About 2 KB; kept off the stack of the transport thread.
                static thread_local playerhub_v1_TradeItem item;
                item = playerhub_v1_TradeItem_init_zero;
                if (!pb_decode_ex(&message, playerhub_v1_TradeItem_fields, &item, PB_DECODE_DELIMITED))
                {
                    return std::unexpected(std::string{ "trade offer item: " } + PB_GET_ERROR(&message));
                }
                if (!ReadTradeItem(item, offer.items.emplace_back()))
                {
                    return std::unexpected("trade offer item values");
                }
            }
            if (!pb_close_string_substream(&a_stream, &message))
            {
                return std::unexpected("trade offer framing");
            }
            if (offer.id == 0 || offer.revision == 0)
            {
                return std::unexpected("trade offer");
            }
            return Multiplayer::TradeEvent{ std::move(offer) };
        }

        std::expected<ServerMessage, std::string> DecodePeerStates(pb_istream_t& a_stream)
        {
            PeerStates states;
            pb_istream_t batch;
            if (!pb_make_string_substream(&a_stream, &batch))
            {
                return std::unexpected("peer states framing");
            }
            while (batch.bytes_left > 0)
            {
                pb_wire_type_t wireType;
                std::uint32_t tag = 0;
                bool eof = false;
                if (!pb_decode_tag(&batch, &wireType, &tag, &eof))
                {
                    return std::unexpected("peer states entry tag");
                }
                if (tag != playerhub_v1_PeerStates_states_tag || wireType != PB_WT_STRING)
                {
                    if (!pb_skip_field(&batch, wireType))
                    {
                        return std::unexpected("peer states field");
                    }
                    continue;
                }
                if (states.entries.size() >= kMaximumPeerStatesEntries)
                {
                    return std::unexpected("peer states entry count");
                }
                playerhub_v1_Checkpoint entry = playerhub_v1_Checkpoint_init_zero;
                if (!pb_decode_ex(&batch, playerhub_v1_Checkpoint_fields, &entry, PB_DECODE_DELIMITED))
                {
                    return std::unexpected(std::string{ "peer states entry: " } + PB_GET_ERROR(&batch));
                }
                const auto checkpoint = ReadCheckpoint(entry);
                if (entry.player == 0 || !IsValidQuantizedCheckpoint(checkpoint))
                {
                    return std::unexpected("peer states entry values");
                }
                states.entries.push_back(PeerEntry{ .slot = entry.player, .checkpoint = checkpoint });
            }
            if (!pb_close_string_substream(&a_stream, &batch))
            {
                return std::unexpected("peer states framing");
            }
            return states;
        }

        std::expected<ServerMessage, std::string> DecodeRoster(pb_istream_t& a_stream)
        {
            RosterChange change;
            pb_istream_t roster;
            if (!pb_make_string_substream(&a_stream, &roster))
            {
                return std::unexpected("roster framing");
            }
            while (roster.bytes_left > 0)
            {
                pb_wire_type_t wireType;
                std::uint32_t tag = 0;
                bool eof = false;
                if (!pb_decode_tag(&roster, &wireType, &tag, &eof))
                {
                    return std::unexpected("roster field tag");
                }
                if (tag == playerhub_v1_Roster_left_tag && wireType == PB_WT_VARINT)
                {
                    if (!pb_decode_varint32(&roster, &change.left))
                    {
                        return std::unexpected("roster left");
                    }
                    continue;
                }
                if (tag != playerhub_v1_Roster_joined_tag || wireType != PB_WT_STRING)
                {
                    if (!pb_skip_field(&roster, wireType))
                    {
                        return std::unexpected("roster field");
                    }
                    continue;
                }
                if (change.joined.size() >= kMaximumRosterEntries)
                {
                    return std::unexpected("roster entry count");
                }
                playerhub_v1_RosterEntry entry = playerhub_v1_RosterEntry_init_zero;
                if (!pb_decode_ex(&roster, playerhub_v1_RosterEntry_fields, &entry, PB_DECODE_DELIMITED))
                {
                    return std::unexpected(std::string{ "roster entry: " } + PB_GET_ERROR(&roster));
                }
                Multiplayer::RosterEntry player{ .playerID = entry.slot, .name = entry.name, .session = entry.session_id };
                if (player.playerID == 0 || player.session == 0 || player.name.empty()
                    || !Multiplayer::IsValidAppearanceName(player.name))
                {
                    return std::unexpected("roster entry values");
                }
                change.joined.push_back(std::move(player));
            }
            if (!pb_close_string_substream(&a_stream, &roster))
            {
                return std::unexpected("roster framing");
            }
            return change;
        }

        std::expected<ServerMessage, std::string> DecodeNearbyPlayers(pb_istream_t& a_stream)
        {
            NearbyPlayersChange change;
            pb_istream_t nearby;
            if (!pb_make_string_substream(&a_stream, &nearby))
            {
                return std::unexpected("nearby players framing");
            }
            const auto read = [](pb_istream_t& a_values, std::vector<Multiplayer::PeerSlot>& a_slots) {
                Multiplayer::PeerSlot slot = 0;
                if (a_slots.size() >= kMaximumRosterEntries || !pb_decode_varint32(&a_values, &slot) || slot == 0)
                {
                    return false;
                }
                a_slots.push_back(slot);
                return true;
            };
            while (nearby.bytes_left > 0)
            {
                pb_wire_type_t wireType;
                std::uint32_t tag = 0;
                bool eof = false;
                if (!pb_decode_tag(&nearby, &wireType, &tag, &eof))
                {
                    return std::unexpected("nearby players field tag");
                }
                auto* slots = tag == playerhub_v1_NearbyPlayers_joined_tag ? &change.joined
                    : tag == playerhub_v1_NearbyPlayers_left_tag           ? &change.left
                                                                          : nullptr;
                if (!slots)
                {
                    if (!pb_skip_field(&nearby, wireType))
                    {
                        return std::unexpected("nearby players field");
                    }
                    continue;
                }
                if (wireType == PB_WT_VARINT)
                {
                    if (!read(nearby, *slots))
                    {
                        return std::unexpected("nearby players slot");
                    }
                    continue;
                }
                pb_istream_t packed;
                if (wireType != PB_WT_STRING || !pb_make_string_substream(&nearby, &packed))
                {
                    return std::unexpected("nearby players framing");
                }
                while (packed.bytes_left > 0)
                {
                    if (!read(packed, *slots))
                    {
                        return std::unexpected("nearby players slot");
                    }
                }
                if (!pb_close_string_substream(&nearby, &packed))
                {
                    return std::unexpected("nearby players framing");
                }
            }
            if (!pb_close_string_substream(&a_stream, &nearby))
            {
                return std::unexpected("nearby players framing");
            }
            return change;
        }
    }

    std::string EncodeHello(std::uint32_t a_viewRadius, std::string_view a_displayName,
        const std::optional<Multiplayer::SessionToken>& a_token, std::span<const Multiplayer::SessionID> a_vanished)
    {
        playerhub_v1_Hello hello = playerhub_v1_Hello_init_zero;
        hello.protocol_version = playerhub_v1_Protocol_PROTOCOL_VERSION;
        hello.view_radius = a_viewRadius;
        static_assert(sizeof(hello.session_token.bytes) == Multiplayer::kSessionTokenBytes);
        if (a_token)
        {
            hello.session_token.size = static_cast<pb_size_t>(a_token->size());
            std::ranges::copy(*a_token, hello.session_token.bytes);
        }
        if (!CopyString(hello.display_name, a_displayName) || a_vanished.size() > Multiplayer::kMaximumVanishedSessions)
        {
            return {};
        }
        hello.vanished.arg = &a_vanished;
        hello.vanished.funcs.encode = [](pb_ostream_t* a_stream, const pb_field_t* a_field, void* const* a_arg) {
            for (const auto session : *static_cast<const std::span<const Multiplayer::SessionID>*>(*a_arg))
            {
                if (!pb_encode_tag_for_field(a_stream, a_field) || !pb_encode_varint(a_stream, session))
                {
                    return false;
                }
            }
            return true;
        };
        return EncodeClientMessage(playerhub_v1_ClientMessage_hello_tag, playerhub_v1_Hello_fields, &hello);
    }

    std::string EncodeEnter()
    {
        playerhub_v1_Enter enter = playerhub_v1_Enter_init_zero;
        return EncodeClientMessage(playerhub_v1_ClientMessage_enter_tag, playerhub_v1_Enter_fields, &enter);
    }

    std::string EncodeExit()
    {
        playerhub_v1_Exit exit = playerhub_v1_Exit_init_zero;
        return EncodeClientMessage(playerhub_v1_ClientMessage_exit_tag, playerhub_v1_Exit_fields, &exit);
    }

    std::string EncodeContext(const CheckpointContext& a_context)
    {
        playerhub_v1_Context context = playerhub_v1_Context_init_zero;
        context.cell_id = a_context.cellID;
        context.worldspace_id = a_context.worldspaceID;
        context.graph_epoch = a_context.graphEpoch;
        if (!WriteOptionalFormReference(a_context.furniture, context.has_furniture, context.furniture))
        {
            return {};
        }
        return EncodeClientMessage(playerhub_v1_ClientMessage_context_tag, playerhub_v1_Context_fields, &context);
    }

    std::string EncodeCheckpoint(const QuantizedCheckpoint& a_checkpoint)
    {
        const playerhub_v1_Checkpoint checkpoint{ .player = 0,
            .sequence = a_checkpoint.sequence,
            .x = a_checkpoint.x,
            .y = a_checkpoint.y,
            .z = a_checkpoint.z,
            .heading = a_checkpoint.heading,
            .pitch = a_checkpoint.pitch,
            .speed = a_checkpoint.speed,
            .direction = a_checkpoint.direction,
            .flags = a_checkpoint.flags,
            .time = a_checkpoint.time };
        return EncodeClientMessage(playerhub_v1_ClientMessage_checkpoint_tag, playerhub_v1_Checkpoint_fields, &checkpoint);
    }

    std::string EncodeAction(
        std::uint64_t a_actionSequence, std::uint64_t a_graphEpoch, std::uint64_t a_checkpointSequence, const Attack& a_attack)
    {
        playerhub_v1_Action action = playerhub_v1_Action_init_zero;
        action.action_sequence = a_actionSequence;
        action.graph_epoch = a_graphEpoch;
        action.checkpoint_sequence = a_checkpointSequence;
        switch (a_attack.action)
        {
            case AttackAction::kLeft:
                action.action = playerhub_v1_AttackAction_ATTACK_ACTION_LEFT;
                break;
            case AttackAction::kDual:
                action.action = playerhub_v1_AttackAction_ATTACK_ACTION_DUAL;
                break;
            case AttackAction::kBash:
                action.action = playerhub_v1_AttackAction_ATTACK_ACTION_BASH;
                break;
            default:
                action.action = playerhub_v1_AttackAction_ATTACK_ACTION_NORMAL;
                break;
        }
        action.power = a_attack.power;
        action.direction = a_attack.power
            ? static_cast<playerhub_v1_PowerDirection>(
                  playerhub_v1_PowerDirection_POWER_DIRECTION_STANDING + static_cast<int>(a_attack.direction))
            : playerhub_v1_PowerDirection_POWER_DIRECTION_NONE;
        return EncodeClientMessage(playerhub_v1_ClientMessage_action_tag, playerhub_v1_Action_fields, &action);
    }

    std::string EncodeAppearance(const Multiplayer::PlayerAppearance& a_appearance)
    {
        using Multiplayer::PlayerAppearance;
        if (a_appearance.headParts.size() > PlayerAppearance::kMaximumHeadParts
            || a_appearance.tints.size() > PlayerAppearance::kMaximumTints)
        {
            return {};
        }
        // About 8 KB; kept off the stack.
        auto appearance = std::make_unique<playerhub_v1_Appearance>();
        *appearance = playerhub_v1_Appearance_init_zero;
        appearance->revision = a_appearance.revision;
        appearance->level = a_appearance.level;
        appearance->has_race = true;
        appearance->female = a_appearance.female;
        appearance->weight = a_appearance.weight;
        appearance->height = a_appearance.height;
        bool written = CopyString(appearance->name, a_appearance.name)
            && WriteFormReference(a_appearance.race, appearance->race)
            && WriteOptionalFormReference(a_appearance.faceDetails, appearance->has_face_details, appearance->face_details);
        appearance->head_parts_count = static_cast<pb_size_t>(a_appearance.headParts.size());
        for (std::size_t i = 0; i < a_appearance.headParts.size(); ++i)
        {
            auto& part = appearance->head_parts[i];
            part.has_form = true;
            part.type = a_appearance.headParts[i].type;
            written = written && WriteFormReference(a_appearance.headParts[i].form, part.form);
        }
        if (a_appearance.hairColor)
        {
            appearance->has_hair_color = true;
            appearance->hair_color = WriteColor(*a_appearance.hairColor);
        }
        appearance->has_body_tint = true;
        appearance->body_tint = WriteColor(a_appearance.bodyTint);
        appearance->face_morphs_count = PlayerAppearance::kFaceMorphCount;
        std::ranges::copy(a_appearance.faceMorphs, appearance->face_morphs);
        appearance->face_parts_count = PlayerAppearance::kFacePartCount;
        std::ranges::copy(a_appearance.faceParts, appearance->face_parts);
        appearance->tints_count = static_cast<pb_size_t>(a_appearance.tints.size());
        for (std::size_t i = 0; i < a_appearance.tints.size(); ++i)
        {
            const auto& tint = a_appearance.tints[i];
            appearance->tints[i] = playerhub_v1_Tint{ .index = tint.index,
                .type = tint.type,
                .has_color = true,
                .color = WriteColor(tint.color),
                .alpha = tint.alpha };
        }
        if (a_appearance.defaultRace)
        {
            appearance->has_default_race = true;
            appearance->default_race.has_race = true;
            appearance->default_race.female = a_appearance.defaultRace->female;
            written = written && WriteFormReference(a_appearance.defaultRace->race, appearance->default_race.race);
        }
        if (!written)
        {
            return {};
        }
        return EncodeClientMessage(playerhub_v1_ClientMessage_appearance_tag, playerhub_v1_Appearance_fields, appearance.get());
    }

    std::string EncodeEquipment(const Multiplayer::PlayerEquipment& a_equipment)
    {
        using Multiplayer::DefaultOutfit;
        using Multiplayer::PlayerEquipment;
        const auto& outfit = a_equipment.defaultOutfit;
        if (a_equipment.armor.size() > PlayerEquipment::kMaximumArmor || outfit.armor.size() > DefaultOutfit::kMaximumArmor
            || outfit.hands.size() > DefaultOutfit::kMaximumHands)
        {
            return {};
        }
        // About 13 KB; kept off the stack.
        auto equipment = std::make_unique<playerhub_v1_Equipment>();
        *equipment = playerhub_v1_Equipment_init_zero;
        equipment->revision = a_equipment.revision;
        bool written = true;
        equipment->armor_count = static_cast<pb_size_t>(a_equipment.armor.size());
        for (std::size_t i = 0; i < a_equipment.armor.size(); ++i)
        {
            written = written && WriteArmor(a_equipment.armor[i], equipment->armor[i]);
        }
        if (a_equipment.rightHand)
        {
            equipment->has_right_hand = true;
            written = written && WriteHand(*a_equipment.rightHand, equipment->right_hand);
        }
        if (a_equipment.leftHand)
        {
            equipment->has_left_hand = true;
            written = written && WriteHand(*a_equipment.leftHand, equipment->left_hand);
        }
        written = written && WriteOptionalFormReference(a_equipment.ammo, equipment->has_ammo, equipment->ammo);
        if (!outfit.armor.empty() || !outfit.hands.empty() || !outfit.ammo.IsEmpty())
        {
            auto& result = equipment->default_outfit;
            equipment->has_default_outfit = true;
            result.armor_count = static_cast<pb_size_t>(outfit.armor.size());
            for (std::size_t i = 0; i < outfit.armor.size(); ++i)
            {
                written = written && WriteArmor(outfit.armor[i], result.armor[i]);
            }
            result.hands_count = static_cast<pb_size_t>(outfit.hands.size());
            for (std::size_t i = 0; i < outfit.hands.size(); ++i)
            {
                written = written && WriteHand(outfit.hands[i], result.hands[i]);
            }
            written = written && WriteOptionalFormReference(outfit.ammo, result.has_ammo, result.ammo);
        }
        if (!written)
        {
            return {};
        }
        return EncodeClientMessage(playerhub_v1_ClientMessage_equipment_tag, playerhub_v1_Equipment_fields, equipment.get());
    }

    std::string EncodeChat(std::uint32_t a_requestID, Multiplayer::ChatChannel a_channel,
        Multiplayer::PeerSlot a_recipient, std::string_view a_text)
    {
        playerhub_v1_ChatSend chat = playerhub_v1_ChatSend_init_zero;
        chat.request_id = a_requestID;
        chat.channel = WriteChatChannel(a_channel);
        chat.recipient = a_recipient;
        if (!CopyString(chat.text, a_text))
        {
            return {};
        }
        return EncodeClientMessage(playerhub_v1_ClientMessage_chat_tag, playerhub_v1_ChatSend_fields, &chat);
    }

    std::string EncodePing(std::uint32_t a_requestID)
    {
        playerhub_v1_Ping ping{ a_requestID };
        return EncodeClientMessage(playerhub_v1_ClientMessage_ping_tag, playerhub_v1_Ping_fields, &ping);
    }

    std::string EncodeVanish(Multiplayer::SessionID a_session, bool a_vanished)
    {
        playerhub_v1_Vanish vanish{ a_session, a_vanished };
        return EncodeClientMessage(playerhub_v1_ClientMessage_vanish_tag, playerhub_v1_Vanish_fields, &vanish);
    }

    std::string EncodeInspect(Multiplayer::PeerSlot a_slot)
    {
        playerhub_v1_Inspect inspect{ a_slot };
        return EncodeClientMessage(playerhub_v1_ClientMessage_inspect_tag, playerhub_v1_Inspect_fields, &inspect);
    }

    std::string EncodeTradeRequest(Multiplayer::PeerSlot a_slot)
    {
        playerhub_v1_TradeRequest request{ a_slot };
        return EncodeClientMessage(playerhub_v1_ClientMessage_trade_request_tag, playerhub_v1_TradeRequest_fields, &request);
    }

    std::string EncodeTradeAnswer(Multiplayer::TradeID a_trade, bool a_accept)
    {
        playerhub_v1_TradeAnswer answer{ a_trade, a_accept };
        return EncodeClientMessage(playerhub_v1_ClientMessage_trade_answer_tag, playerhub_v1_TradeAnswer_fields, &answer);
    }

    std::string EncodeTradeOffer(const Multiplayer::TradeOffer& a_offer)
    {
        if (a_offer.items.size() > Multiplayer::kMaximumTradeItems
            || !std::ranges::all_of(a_offer.items, Multiplayer::IsValidTradeItem))
        {
            return {};
        }
        playerhub_v1_TradeOffer offer = playerhub_v1_TradeOffer_init_zero;
        offer.trade_id = a_offer.id;
        offer.revision = a_offer.revision;
        offer.items.arg = const_cast<std::vector<Multiplayer::TradeItem>*>(&a_offer.items);
        offer.items.funcs.encode = [](pb_ostream_t* a_stream, const pb_field_t* a_field, void* const* a_arg) {
            auto item = std::make_unique<playerhub_v1_TradeItem>();
            for (const auto& entry : *static_cast<const std::vector<Multiplayer::TradeItem>*>(*a_arg))
            {
                if (!WriteTradeItem(entry, *item) || !pb_encode_tag_for_field(a_stream, a_field)
                    || !pb_encode_submessage(a_stream, playerhub_v1_TradeItem_fields, item.get()))
                {
                    return false;
                }
            }
            return true;
        };
        return EncodeClientMessage(playerhub_v1_ClientMessage_trade_offer_tag, playerhub_v1_TradeOffer_fields, &offer);
    }

    std::string EncodeTradeReport(const Multiplayer::TradeReport& a_report)
    {
        if (!Multiplayer::IsValidTradeSkips(a_report.skipped))
        {
            return {};
        }
        // About 9 KB; kept off the stack.
        auto report = std::make_unique<playerhub_v1_TradeReport>();
        *report = playerhub_v1_TradeReport_init_zero;
        report->trade_id = a_report.id;
        report->revision = a_report.revision;
        report->skipped_count = static_cast<pb_size_t>(a_report.skipped.size());
        for (std::size_t i = 0; i < a_report.skipped.size(); ++i)
        {
            const auto& skip = a_report.skipped[i];
            auto& result = report->skipped[i];
            result.index = skip.index;
            result.reason = static_cast<playerhub_v1_TradeSkipReason>(skip.reason);
            if (!CopyString(result.plugin, skip.plugin))
            {
                return {};
            }
        }
        return EncodeClientMessage(playerhub_v1_ClientMessage_trade_report_tag, playerhub_v1_TradeReport_fields, report.get());
    }

    std::string EncodeTradeAccept(const Multiplayer::TradeAcceptance& a_acceptance)
    {
        playerhub_v1_TradeAccept accept{ a_acceptance.id, a_acceptance.ownRevision, a_acceptance.peerRevision };
        return EncodeClientMessage(playerhub_v1_ClientMessage_trade_accept_tag, playerhub_v1_TradeAccept_fields, &accept);
    }

    std::string EncodeTradeCancel(Multiplayer::TradeID a_trade, Multiplayer::TradeCloseReason a_reason)
    {
        playerhub_v1_TradeCancel cancel{ a_trade, static_cast<playerhub_v1_TradeCloseReason>(a_reason) };
        return EncodeClientMessage(playerhub_v1_ClientMessage_trade_cancel_tag, playerhub_v1_TradeCancel_fields, &cancel);
    }

    struct Decoder::Buffer
    {
        playerhub_v1_ServerMessage message;
    };

    Decoder::Decoder() :
        buffer(std::make_unique<Buffer>())
    {}

    Decoder::~Decoder() = default;

    std::expected<ServerMessage, std::string> Decoder::Decode(std::span<const std::byte> a_data)
    {
        const auto* bytes = reinterpret_cast<const pb_byte_t*>(a_data.data());

        // The generated decoder would need a callback set per oneof member.
        {
            pb_istream_t stream = pb_istream_from_buffer(bytes, a_data.size());
            pb_wire_type_t wireType;
            std::uint32_t tag = 0;
            bool eof = false;
            if (!pb_decode_tag(&stream, &wireType, &tag, &eof))
            {
                return std::unexpected("envelope");
            }
            if (tag == playerhub_v1_ServerMessage_peer_states_tag && wireType == PB_WT_STRING)
            {
                auto states = DecodePeerStates(stream);
                if (states && stream.bytes_left != 0)
                {
                    return std::unexpected("trailing envelope fields");
                }
                return states;
            }
            if (tag == playerhub_v1_ServerMessage_roster_tag && wireType == PB_WT_STRING)
            {
                auto roster = DecodeRoster(stream);
                if (roster && stream.bytes_left != 0)
                {
                    return std::unexpected("trailing envelope fields");
                }
                return roster;
            }
            if (tag == playerhub_v1_ServerMessage_nearby_players_tag && wireType == PB_WT_STRING)
            {
                auto nearby = DecodeNearbyPlayers(stream);
                if (nearby && stream.bytes_left != 0)
                {
                    return std::unexpected("trailing envelope fields");
                }
                return nearby;
            }
            if (tag == playerhub_v1_ServerMessage_trade_offer_tag && wireType == PB_WT_STRING)
            {
                auto offer = DecodeTradeOffer(stream);
                if (offer && stream.bytes_left != 0)
                {
                    return std::unexpected("trailing envelope fields");
                }
                return offer;
            }
        }

        auto& message = buffer->message;
        pb_istream_t stream = pb_istream_from_buffer(bytes, a_data.size());
        if (!pb_decode(&stream, playerhub_v1_ServerMessage_fields, &message))
        {
            return std::unexpected(std::string{ "decode: " } + PB_GET_ERROR(&stream));
        }

        switch (message.which_message)
        {
            case playerhub_v1_ServerMessage_welcome_tag:
            {
                const auto& welcome = message.message.welcome;
                Welcome result{
                    .slot = welcome.slot, .sessionID = welcome.session_id, .displayName = welcome.display_name
                };
                if (result.slot == 0)
                {
                    return std::unexpected("welcome slot");
                }
                if (result.sessionID == 0)
                {
                    return std::unexpected("welcome session");
                }
                if (!Multiplayer::IsValidDisplayName(result.displayName))
                {
                    return std::unexpected("welcome display name");
                }
                if (welcome.session_token.size == Multiplayer::kSessionTokenBytes)
                {
                    std::copy_n(welcome.session_token.bytes, Multiplayer::kSessionTokenBytes,
                        result.issuedToken.emplace().begin());
                }
                else if (welcome.session_token.size != 0)
                {
                    return std::unexpected("welcome session token");
                }
                return result;
            }
            case playerhub_v1_ServerMessage_result_tag:
            {
                const auto& result = message.message.result;
                return Result{ .requestID = result.request_id, .ok = result.ok, .error = result.error };
            }
            case playerhub_v1_ServerMessage_peer_enter_tag:
            {
                const auto& enter = message.message.peer_enter;
                PeerEnter result;
                result.slot = enter.slot;
                if (enter.slot == 0 || !enter.has_context || !enter.has_state)
                {
                    return std::unexpected("peer enter");
                }
                if (!ReadContext(enter.context, result.context))
                {
                    return std::unexpected("peer enter context");
                }
                result.state = ReadCheckpoint(enter.state);
                if (!IsValidQuantizedCheckpoint(result.state))
                {
                    return std::unexpected("peer enter state");
                }
                if (enter.has_appearance)
                {
                    if (const auto* field = ReadAppearance(enter.appearance, result.appearance.emplace()))
                    {
                        return std::unexpected(std::string{ "peer enter appearance " } + field);
                    }
                }
                if (enter.has_equipment)
                {
                    if (const auto* field = ReadEquipment(enter.equipment, result.equipment.emplace()))
                    {
                        return std::unexpected(std::string{ "peer enter equipment " } + field);
                    }
                }
                return result;
            }
            case playerhub_v1_ServerMessage_peer_context_tag:
            {
                const auto& context = message.message.peer_context;
                PeerContext result;
                result.slot = context.slot;
                if (context.slot == 0 || !context.has_context || !ReadContext(context.context, result.context))
                {
                    return std::unexpected("peer context");
                }
                return result;
            }
            case playerhub_v1_ServerMessage_peer_leave_tag:
            {
                const auto& leave = message.message.peer_leave;
                PeerLeave result{ .slot = leave.slot };
                switch (leave.reason)
                {
                    case playerhub_v1_LeaveReason_LEAVE_REASON_OUT_OF_RANGE:
                        result.reason = LeaveReason::kOutOfRange;
                        break;
                    case playerhub_v1_LeaveReason_LEAVE_REASON_EXCLUDED:
                        result.reason = LeaveReason::kExcluded;
                        break;
                    case playerhub_v1_LeaveReason_LEAVE_REASON_EXITED:
                        result.reason = LeaveReason::kExited;
                        break;
                    case playerhub_v1_LeaveReason_LEAVE_REASON_DISCONNECTED:
                        result.reason = LeaveReason::kDisconnected;
                        break;
                    case playerhub_v1_LeaveReason_LEAVE_REASON_VIEWER_UNPLACED:
                        result.reason = LeaveReason::kViewerUnplaced;
                        break;
                    default:
                        return std::unexpected("peer leave reason");
                }
                if (leave.slot == 0)
                {
                    return std::unexpected("peer leave slot");
                }
                return result;
            }
            case playerhub_v1_ServerMessage_peer_action_tag:
            {
                const auto& action = message.message.peer_action;
                if (action.slot == 0 || !action.has_action)
                {
                    return std::unexpected("peer action");
                }
                auto result = ReadAction(action.slot, action.action);
                if (!result)
                {
                    return std::unexpected(std::format("peer action {}", result.error()));
                }
                return *result;
            }
            case playerhub_v1_ServerMessage_peer_appearance_tag:
            {
                const auto& appearance = message.message.peer_appearance;
                PeerAppearance result;
                result.slot = appearance.slot;
                if (appearance.slot == 0 || !appearance.has_appearance)
                {
                    return std::unexpected("peer appearance");
                }
                if (const auto* field = ReadAppearance(appearance.appearance, result.appearance))
                {
                    return std::unexpected(std::string{ "peer appearance " } + field);
                }
                return result;
            }
            case playerhub_v1_ServerMessage_peer_equipment_tag:
            {
                const auto& equipment = message.message.peer_equipment;
                PeerEquipment result;
                result.slot = equipment.slot;
                if (equipment.slot == 0 || !equipment.has_equipment)
                {
                    return std::unexpected("peer equipment");
                }
                if (const auto* field = ReadEquipment(equipment.equipment, result.equipment))
                {
                    return std::unexpected(std::string{ "peer equipment " } + field);
                }
                return result;
            }
            case playerhub_v1_ServerMessage_chat_message_tag:
            {
                const auto& chat = message.message.chat_message;
                Multiplayer::ChatMessage result{ .text = chat.text };
                const auto channel = ReadChatChannel(chat.channel);
                const bool whisper = channel == Multiplayer::ChatChannel::kWhisper;
                if (!channel || !chat.has_sender || !ReadChatSpeaker(chat.sender, result.sender)
                    || chat.has_recipient != whisper
                    || (whisper && !ReadChatSpeaker(chat.recipient, result.recipient))
                    || !Multiplayer::IsValidChatText(result.text))
                {
                    return std::unexpected("chat message");
                }
                result.channel = *channel;
                return result;
            }
            case playerhub_v1_ServerMessage_inspection_tag:
            {
                const auto& inspection = message.message.inspection;
                if (inspection.slot == 0 || inspection.level > (std::numeric_limits<std::uint16_t>::max)())
                {
                    return std::unexpected("inspection");
                }
                Multiplayer::PlayerInspection result{ .playerID = inspection.slot,
                    .level = static_cast<std::uint16_t>(inspection.level) };
                if (inspection.has_race && !ReadFormReference(inspection.race, result.race.emplace()))
                {
                    return std::unexpected("inspection race");
                }
                if (inspection.has_default_race)
                {
                    auto& defaultRace = result.defaultRace.emplace();
                    defaultRace.female = inspection.default_race.female;
                    if (!inspection.default_race.has_race
                        || !ReadFormReference(inspection.default_race.race, defaultRace.race)
                        || !Multiplayer::IsBaseGamePlugin(defaultRace.race.plugin))
                    {
                        return std::unexpected("inspection defaultRace");
                    }
                }
                return result;
            }
            case playerhub_v1_ServerMessage_trade_invite_tag:
            {
                const auto& invite = message.message.trade_invite;
                Multiplayer::TradeInvite result{ .id = invite.trade_id };
                if (invite.trade_id == 0 || !invite.has_sender || !ReadChatSpeaker(invite.sender, result.sender)
                    || !invite.has_recipient || !ReadChatSpeaker(invite.recipient, result.recipient))
                {
                    return std::unexpected("trade invite");
                }
                return Multiplayer::TradeEvent{ std::move(result) };
            }
            case playerhub_v1_ServerMessage_trade_open_tag:
            {
                const auto& open = message.message.trade_open;
                if (open.trade_id == 0)
                {
                    return std::unexpected("trade open");
                }
                return Multiplayer::TradeEvent{ Multiplayer::TradeOpen{ .id = open.trade_id } };
            }
            case playerhub_v1_ServerMessage_trade_report_tag:
            {
                const auto& report = message.message.trade_report;
                Multiplayer::TradeReport result{ .id = report.trade_id, .revision = report.revision };
                for (pb_size_t i = 0; i < report.skipped_count; ++i)
                {
                    const auto& skip = report.skipped[i];
                    if (skip.index >= Multiplayer::kMaximumTradeItems)
                    {
                        return std::unexpected("trade report index");
                    }
                    result.skipped.push_back(Multiplayer::TradeSkip{ .index = static_cast<std::uint8_t>(skip.index),
                        .reason = static_cast<Multiplayer::TradeSkipReason>(skip.reason),
                        .plugin = skip.plugin });
                }
                if (result.id == 0 || result.revision == 0 || !Multiplayer::IsValidTradeSkips(result.skipped))
                {
                    return std::unexpected("trade report");
                }
                return Multiplayer::TradeEvent{ std::move(result) };
            }
            case playerhub_v1_ServerMessage_trade_accept_tag:
            {
                const auto& accept = message.message.trade_accept;
                if (accept.trade_id == 0)
                {
                    return std::unexpected("trade accept");
                }
                return Multiplayer::TradeEvent{ Multiplayer::TradeAcceptance{ .id = accept.trade_id,
                    .ownRevision = accept.own_revision,
                    .peerRevision = accept.peer_revision } };
            }
            case playerhub_v1_ServerMessage_trade_commit_tag:
            {
                const auto& commit = message.message.trade_commit;
                if (commit.trade_id == 0)
                {
                    return std::unexpected("trade commit");
                }
                return Multiplayer::TradeEvent{ Multiplayer::TradeCommit{ .id = commit.trade_id,
                    .ownRevision = commit.own_revision,
                    .peerRevision = commit.peer_revision } };
            }
            case playerhub_v1_ServerMessage_trade_close_tag:
            {
                const auto& close = message.message.trade_close;
                const auto reason = ReadTradeCloseReason(close.reason);
                if (!reason)
                {
                    return std::unexpected("trade close reason");
                }
                return Multiplayer::TradeEvent{ Multiplayer::TradeClose{ .id = close.trade_id, .reason = *reason } };
            }
            default:
                return std::unexpected("unknown message");
        }
    }
}
