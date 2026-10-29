#include "actor/RaceMenuOverlays.h"
#include "forms/TextureFiles.h"
#include "runtime/GameThread.h"
#include "text/AsciiCase.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace RaceMenuOverlays
{
    namespace
    {
        // SKEE's interfaces as RaceMenu publishes them in
        // ModderResource/IPluginInterface.h. Only the vtable order matters; the
        // functions this file never calls keep their slots with simplified
        // parameters.
        class IPluginInterface
        {
        public:
            virtual ~IPluginInterface() = default;
            virtual std::uint32_t GetVersion() = 0;
            virtual void Revert() = 0;
        };

        class IInterfaceMap
        {
        public:
            virtual IPluginInterface* QueryInterface(const char* a_name) = 0;
            virtual bool AddInterface(const char* a_name, IPluginInterface* a_interface) = 0;
            virtual IPluginInterface* RemoveInterface(const char* a_name) = 0;
        };

        struct InterfaceExchangeMessage
        {
            static constexpr std::uint32_t kExchangeInterface = 0x9E3779B9;

            IInterfaceMap* interfaceMap{ nullptr };
        };

        class IOverlayInterface : public IPluginInterface
        {
        public:
            enum class OverlayType : std::int32_t
            {
                kNormal,
                kSpell
            };
            enum class OverlayLocation : std::int32_t
            {
                kBody,
                kHand,
                kFeet,
                kFace
            };

            virtual bool HasOverlays(RE::TESObjectREFR* a_reference) = 0;
            virtual void AddOverlays(RE::TESObjectREFR* a_reference, bool a_defer) = 0;
            virtual void RemoveOverlays(RE::TESObjectREFR* a_reference, bool a_defer) = 0;
            virtual void RevertOverlays(void*, bool, bool) = 0;
            virtual void RevertOverlay(void*, const char*, std::uint32_t, std::uint32_t, bool, bool) = 0;
            virtual void EraseOverlays(void*, bool) = 0;
            virtual void RevertHeadOverlays(void*, bool, bool) = 0;
            virtual void RevertHeadOverlay(void*, const char*, std::uint32_t, std::uint32_t, bool, bool) = 0;
            virtual std::uint32_t GetOverlayCount(OverlayType a_type, OverlayLocation a_location) = 0;
        };

        class IOverrideInterface : public IPluginInterface
        {
        public:
            class GetVariant
            {
            public:
                virtual void Int(std::int32_t a_value) = 0;
                virtual void Float(float a_value) = 0;
                virtual void String(const char* a_value) = 0;
                virtual void Bool(bool a_value) = 0;
                virtual void TextureSet(const RE::BGSTextureSet* a_value) = 0;
            };

            class SetVariant
            {
            public:
                enum class Type : std::int32_t
                {
                    kNone,
                    kInt,
                    kFloat,
                    kString,
                    kBool,
                    kTextureSet
                };
                virtual Type GetType() { return Type::kNone; }
                virtual std::int32_t Int() { return 0; }
                virtual float Float() { return 0.0F; }
                virtual const char* String() { return nullptr; }
                virtual bool Bool() { return false; }
                virtual RE::BGSTextureSet* TextureSet() { return nullptr; }
            };

            virtual bool HasArmorAddonNode(void*, bool, void*, void*, const char*, bool) = 0;
            virtual bool HasArmorOverride(void*, bool, void*, void*, const char*, std::uint16_t, std::uint8_t) = 0;
            virtual void AddArmorOverride(void*, bool, void*, void*, const char*, std::uint16_t, std::uint8_t, void*) = 0;
            virtual bool GetArmorOverride(void*, bool, void*, void*, const char*, std::uint16_t, std::uint8_t, void*) = 0;
            virtual void RemoveArmorOverride(void*, bool, void*, void*, const char*, std::uint16_t, std::uint8_t) = 0;
            virtual void SetArmorProperties(void*, bool) = 0;
            virtual void SetArmorProperty(void*, bool, void*, void*, const char*, std::uint16_t, std::uint8_t, void*,
                bool) = 0;
            virtual bool GetArmorProperty(void*, bool, void*, void*, const char*, std::uint16_t, std::uint8_t, void*) = 0;
            virtual void ApplyArmorOverrides(void*, void*, void*, void*, bool) = 0;
            virtual void RemoveAllArmorOverrides() = 0;
            virtual void RemoveAllArmorOverridesByReference(void*) = 0;
            virtual void RemoveAllArmorOverridesByArmor(void*, bool, void*) = 0;
            virtual void RemoveAllArmorOverridesByAddon(void*, bool, void*, void*) = 0;
            virtual void RemoveAllArmorOverridesByNode(void*, bool, void*, void*, const char*) = 0;

            virtual bool HasNodeOverride(void*, bool, const char*, std::uint16_t, std::uint8_t) = 0;
            virtual void AddNodeOverride(RE::TESObjectREFR* a_reference, bool a_female, const char* a_node,
                std::uint16_t a_key, std::uint8_t a_index, SetVariant& a_value) = 0;
            virtual bool GetNodeOverride(RE::TESObjectREFR* a_reference, bool a_female, const char* a_node,
                std::uint16_t a_key, std::uint8_t a_index, GetVariant& a_visitor) = 0;
            virtual void RemoveNodeOverride(void*, bool, const char*, std::uint16_t, std::uint8_t) = 0;
            virtual void SetNodeProperties(RE::TESObjectREFR* a_reference, bool a_immediate) = 0;
            virtual void SetNodeProperty(void*, bool, const char*, std::uint16_t, std::uint8_t, void*, bool) = 0;
            virtual bool GetNodeProperty(RE::TESObjectREFR* a_reference, bool a_firstPerson, const char* a_node,
                std::uint16_t a_key, std::uint8_t a_index, GetVariant& a_visitor) = 0;
            virtual void ApplyNodeOverrides(void*, void*, bool) = 0;
            virtual void RemoveAllNodeOverrides() = 0;
            virtual void RemoveAllNodeOverridesByReference(RE::TESObjectREFR* a_reference) = 0;
        };

        // The version that added GetOverlayCount, and the one that introduced the
        // variant wrappers.
        constexpr std::uint32_t kMinimumOverlayVersion = 2;
        constexpr std::uint32_t kMinimumOverrideVersion = 2;

        // NiOverride keys, as RaceMenu.psc sets them on overlay nodes. Keys other
        // than the texture take index -1.
        constexpr std::uint16_t kEmissiveColorKey = 0;
        constexpr std::uint16_t kEmissiveMultipleKey = 1;
        constexpr std::uint16_t kTintColorKey = 7;
        constexpr std::uint16_t kAlphaKey = 8;
        constexpr std::uint16_t kTextureKey = 9;
        constexpr std::uint8_t kDiffuseIndex = 0;
        constexpr std::uint8_t kNoIndex = 0xFF;

        constexpr std::string_view kDefaultTexture = "actors\\character\\overlays\\default.dds";

        using Multiplayer::AppearanceColor;
        using Multiplayer::AppearanceOverlay;
        using Multiplayer::OverlayArea;
        using Multiplayer::PlayerAppearance;

        constexpr std::array kAreas{ OverlayArea::kBody, OverlayArea::kHands, OverlayArea::kFeet, OverlayArea::kFace };

        IOverlayInterface* s_overlays{ nullptr };
        IOverrideInterface* s_overrides{ nullptr };
        std::vector<RE::ActorHandle> s_applied;

        class ValueReader final : public IOverrideInterface::GetVariant
        {
        public:
            void Int(std::int32_t a_value) override { integer = a_value; }
            void Float(float a_value) override { real = a_value; }
            void String(const char* a_value) override { text = a_value ? a_value : ""; }
            void Bool(bool) override {}
            void TextureSet(const RE::BGSTextureSet*) override {}

            std::int32_t integer{ 0 };
            float real{ 0.0F };
            std::string text;
        };

        class IntValue final : public IOverrideInterface::SetVariant
        {
        public:
            explicit IntValue(std::int32_t a_value) :
                value(a_value)
            {}
            Type GetType() override { return Type::kInt; }
            std::int32_t Int() override { return value; }

        private:
            std::int32_t value;
        };

        class FloatValue final : public IOverrideInterface::SetVariant
        {
        public:
            explicit FloatValue(float a_value) :
                value(a_value)
            {}
            Type GetType() override { return Type::kFloat; }
            float Float() override { return value; }

        private:
            float value;
        };

        class StringValue final : public IOverrideInterface::SetVariant
        {
        public:
            explicit StringValue(const char* a_value) :
                value(a_value)
            {}
            Type GetType() override { return Type::kString; }
            const char* String() override { return value; }

        private:
            const char* value;
        };

        IOverlayInterface::OverlayLocation GetLocation(OverlayArea a_area)
        {
            switch (a_area)
            {
                case OverlayArea::kHands:
                    return IOverlayInterface::OverlayLocation::kHand;
                case OverlayArea::kFeet:
                    return IOverlayInterface::OverlayLocation::kFeet;
                case OverlayArea::kFace:
                    return IOverlayInterface::OverlayLocation::kFace;
                case OverlayArea::kBody:
                default:
                    return IOverlayInterface::OverlayLocation::kBody;
            }
        }

        // SKEE names the overlay shapes "Body [Ovl0]", "Hands [Ovl0]", "Feet [Ovl0]",
        // and "Face [Ovl0]"; spell overlays use "[SOvl#]" and are not synchronized.
        using NodeName = std::array<char, 24>;

        NodeName FormatNodeName(OverlayArea a_area, std::uint32_t a_slot)
        {
            std::string_view prefix = "Body";
            switch (a_area)
            {
                case OverlayArea::kHands:
                    prefix = "Hands";
                    break;
                case OverlayArea::kFeet:
                    prefix = "Feet";
                    break;
                case OverlayArea::kFace:
                    prefix = "Face";
                    break;
                case OverlayArea::kBody:
                default:
                    break;
            }
            NodeName name{};
            std::format_to_n(name.data(), name.size() - 1, "{} [Ovl{}]", prefix, a_slot);
            return name;
        }

        bool IsDefaultTexture(std::string_view a_path)
        {
            constexpr std::string_view kTexturesFolder = "textures";
            if (AsciiCase::StartsWith(a_path, kTexturesFolder) && a_path.size() > kTexturesFolder.size()
                && (a_path[kTexturesFolder.size()] == '\\' || a_path[kTexturesFolder.size()] == '/'))
            {
                a_path.remove_prefix(kTexturesFolder.size() + 1);
            }
            if (a_path.size() != kDefaultTexture.size())
            {
                return false;
            }
            return std::ranges::equal(a_path, kDefaultTexture, [](char a_left, char a_right) {
                return AsciiCase::ToLower(a_left == '/' ? '\\' : a_left) == a_right;
            });
        }

        AppearanceColor ToColor(std::int32_t a_value)
        {
            const auto value = static_cast<std::uint32_t>(a_value);
            return AppearanceColor{ .red = static_cast<std::uint8_t>(value >> 16),
                .green = static_cast<std::uint8_t>(value >> 8),
                .blue = static_cast<std::uint8_t>(value) };
        }

        std::int32_t ToInt(const AppearanceColor& a_color)
        {
            return static_cast<std::int32_t>(
                (static_cast<std::uint32_t>(a_color.red) << 16) | (static_cast<std::uint32_t>(a_color.green) << 8)
                | a_color.blue);
        }

        // The override is what SKEE saves; the property is the node's current value,
        // which RaceMenu reads first when the node exists.
        bool ReadValue(RE::Actor& a_actor, bool a_female, const char* a_node, std::uint16_t a_key,
            std::uint8_t a_index, ValueReader& a_reader)
        {
            return s_overrides->GetNodeOverride(std::addressof(a_actor), a_female, a_node, a_key, a_index, a_reader)
                || s_overrides->GetNodeProperty(std::addressof(a_actor), false, a_node, a_key, a_index, a_reader);
        }

        float ClampFinite(float a_value, float a_maximum, float a_fallback)
        {
            return std::isfinite(a_value) ? std::clamp(a_value, 0.0F, a_maximum) : a_fallback;
        }
    }

    void Initialize()
    {
        InterfaceExchangeMessage message;
        const auto* messaging = SKSE::GetMessagingInterface();
        if (!messaging
            || !messaging->Dispatch(InterfaceExchangeMessage::kExchangeInterface, std::addressof(message),
                sizeof(InterfaceExchangeMessage*), "skee")
            || !message.interfaceMap)
        {
            logs::info("RaceMenu (SKEE) did not answer; overlays are not synchronized");
            return;
        }

        auto* overlays = static_cast<IOverlayInterface*>(message.interfaceMap->QueryInterface("Overlay"));
        auto* overrides = static_cast<IOverrideInterface*>(message.interfaceMap->QueryInterface("Override"));
        const auto overlayVersion = overlays ? overlays->GetVersion() : 0;
        const auto overrideVersion = overrides ? overrides->GetVersion() : 0;
        if (overlayVersion < kMinimumOverlayVersion || overrideVersion < kMinimumOverrideVersion)
        {
            logs::warn("RaceMenu (SKEE) interfaces are too old; overlays are not synchronized: overlay={} override={}",
                overlayVersion, overrideVersion);
            return;
        }
        s_overlays = overlays;
        s_overrides = overrides;
        logs::info("RaceMenu (SKEE) interfaces found; overlays are synchronized: overlay={} override={}",
            overlayVersion, overrideVersion);
    }

    std::vector<AppearanceOverlay> Capture(RE::Actor& a_player)
    {
        std::vector<AppearanceOverlay> result;
        const auto* base = a_player.GetActorBase();
        if (!s_overlays || !base)
        {
            return result;
        }

        static std::string invalidReported;
        const bool female = base->IsFemale();
        for (const auto area : kAreas)
        {
            const auto count = s_overlays->GetOverlayCount(IOverlayInterface::OverlayType::kNormal, GetLocation(area));
            for (std::uint32_t slot = 0; slot < count && result.size() < PlayerAppearance::kMaximumOverlays; ++slot)
            {
                const auto node = FormatNodeName(area, slot);
                ValueReader texture;
                if (!ReadValue(a_player, female, node.data(), kTextureKey, kDiffuseIndex, texture)
                    || texture.text.empty() || IsDefaultTexture(texture.text))
                {
                    continue;
                }
                if (!Multiplayer::IsValidTexturePath(texture.text))
                {
                    if (texture.text != invalidReported)
                    {
                        logs::warn("Overlay capture skipped a texture path peers would reject: node=\"{}\" texture=\"{}\"",
                            node.data(), texture.text);
                        invalidReported = texture.text;
                    }
                    continue;
                }

                AppearanceOverlay overlay{ .area = area, .texture = std::move(texture.text) };
                ValueReader value;
                overlay.color = ReadValue(a_player, female, node.data(), kTintColorKey, kNoIndex, value)
                    ? ToColor(value.integer)
                    : AppearanceColor{ 0xFF, 0xFF, 0xFF };
                overlay.alpha = ReadValue(a_player, female, node.data(), kAlphaKey, kNoIndex, value)
                    ? ClampFinite(value.real, 1.0F, 1.0F)
                    : 1.0F;
                if (overlay.alpha <= 0.0F)
                {
                    continue;
                }
                if (ReadValue(a_player, female, node.data(), kEmissiveColorKey, kNoIndex, value))
                {
                    overlay.glowColor = ToColor(value.integer);
                }
                if (ReadValue(a_player, female, node.data(), kEmissiveMultipleKey, kNoIndex, value))
                {
                    overlay.glowStrength
                        = ClampFinite(value.real, PlayerAppearance::kMaximumOverlayGlowStrength, 0.0F);
                }
                result.push_back(std::move(overlay));
            }
        }
        return result;
    }

    void Apply(RE::Actor& a_actor, std::span<const AppearanceOverlay> a_overlays)
    {
        const auto* base = a_actor.GetActorBase();
        if (!s_overlays || a_overlays.empty() || !base || !GameThread::Require("RaceMenuOverlays::Apply"))
        {
            return;
        }

        const bool female = base->IsFemale();
        std::array<std::uint32_t, kAreas.size()> nextSlot{};
        std::size_t applied = 0;
        std::size_t missing = 0;
        std::size_t overflow = 0;
        const AppearanceOverlay* firstMissing = nullptr;
        for (const auto& overlay : a_overlays)
        {
            const auto location = GetLocation(overlay.area);
            auto& slot = nextSlot[static_cast<std::size_t>(location)];
            if (slot >= s_overlays->GetOverlayCount(IOverlayInterface::OverlayType::kNormal, location))
            {
                ++overflow;
                continue;
            }
            if (!Forms::TextureExists(overlay.texture))
            {
                firstMissing = firstMissing ? firstMissing : std::addressof(overlay);
                ++missing;
                continue;
            }

            const auto node = FormatNodeName(overlay.area, slot++);
            auto* reference = std::addressof(a_actor);
            StringValue texture{ overlay.texture.c_str() };
            IntValue color{ ToInt(overlay.color) };
            FloatValue alpha{ overlay.alpha };
            IntValue glowColor{ ToInt(overlay.glowColor) };
            FloatValue glowStrength{ overlay.glowStrength };
            s_overrides->AddNodeOverride(reference, female, node.data(), kTextureKey, kDiffuseIndex, texture);
            s_overrides->AddNodeOverride(reference, female, node.data(), kTintColorKey, kNoIndex, color);
            s_overrides->AddNodeOverride(reference, female, node.data(), kAlphaKey, kNoIndex, alpha);
            s_overrides->AddNodeOverride(reference, female, node.data(), kEmissiveColorKey, kNoIndex, glowColor);
            s_overrides->AddNodeOverride(reference, female, node.data(), kEmissiveMultipleKey, kNoIndex, glowStrength);
            ++applied;
        }

        if (applied > 0)
        {
            s_applied.push_back(a_actor.GetHandle());
            s_overlays->AddOverlays(std::addressof(a_actor), false);
            s_overrides->SetNodeProperties(std::addressof(a_actor), false);
        }
        logs::debug("Applied RaceMenu overlays: actor={:08X} applied={}/{} missingTextures={} noFreeSlot={} "
                    "firstMissing=\"{}\"",
            a_actor.GetHandle().native_handle(), applied, a_overlays.size(), missing, overflow,
            firstMissing ? firstMissing->texture : std::string{});
    }

    void Release(RE::ActorHandle a_handle, RE::Actor* a_actor)
    {
        if (std::erase(s_applied, a_handle) == 0 || !a_actor)
        {
            return;
        }
        // SKEE saves both lists in its co-save, keyed by the reference.
        s_overrides->RemoveAllNodeOverridesByReference(a_actor);
        s_overlays->RemoveOverlays(a_actor, true);
    }
}
