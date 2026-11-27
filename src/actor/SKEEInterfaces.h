#pragma once

#include <RE/Skyrim.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

// RaceMenu's SKEE interfaces as it publishes them in ModderResource/IPluginInterface.h.
// Only the vtable order matters; the functions the plugin never calls keep their
// slots with simplified parameters.
namespace SKEEInterfaces
{
    class IPluginInterface
    {
    public:
        virtual ~IPluginInterface() = default;
        virtual std::uint32_t GetVersion() = 0;
        virtual void Revert() = 0;
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
        virtual void SetArmorProperty(void*, bool, void*, void*, const char*, std::uint16_t, std::uint8_t, void*, bool)
            = 0;
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

    // Checked against skee64.dll of RaceMenu for 1.7.104: the vtable of
    // BodyMorphInterface holds these slots in this order, and GetVersion returns 5.
    class IBodyMorphInterface : public IPluginInterface
    {
    public:
        class MorphValueVisitor
        {
        public:
            virtual void Visit(RE::TESObjectREFR* a_reference, const char* a_name, const char* a_key, float a_value)
                = 0;
        };

        virtual void SetMorph(RE::TESObjectREFR* a_reference, const char* a_name, const char* a_key, float a_value) = 0;
        virtual float GetMorph(void*, const char*, const char*) = 0;
        virtual void ClearMorph(void*, const char*, const char*) = 0;
        virtual float GetBodyMorphs(void*, const char*) = 0;
        virtual void ClearBodyMorphNames(void*, const char*) = 0;
        virtual void VisitMorphs(void*, void*) = 0;
        virtual void VisitKeys(void*, const char*, void*) = 0;
        virtual void VisitMorphValues(RE::TESObjectREFR* a_reference, MorphValueVisitor& a_visitor) = 0;
        virtual void ClearMorphs(RE::TESObjectREFR* a_reference) = 0;
        virtual void ApplyVertexDiff(void*, void*, bool) = 0;
        virtual void ApplyBodyMorphs(RE::TESObjectREFR* a_reference, bool a_deferUpdate) = 0;
        virtual void UpdateModelWeight(RE::TESObjectREFR* a_reference, bool a_immediate) = 0;
        virtual void SetCacheLimit(std::uint64_t) = 0;
        virtual bool HasMorphs(RE::TESObjectREFR* a_reference) = 0;
    };

    // SKEE's SKEEFixedString: an MSVC std::string followed by a hash that SKEE's
    // string table and value maps look names up by. SKEE only reads it and copies the
    // string with its own allocator.
    class SKEEString
    {
    public:
        explicit SKEEString(std::string_view a_text);

        SKEEString(SKEEString&&) noexcept = default;
        SKEEString& operator=(SKEEString&&) noexcept = default;

        struct Layout
        {
            // MSVC keeps up to 15 characters in place and points at the heap beyond.
            union
            {
                char buffer[16];
                const char* pointer;
            } storage;
            std::size_t size;
            std::size_t capacity;
            std::size_t hash;
        };
        static_assert(sizeof(Layout) == 40);

        [[nodiscard]] const Layout& Get() const noexcept { return layout; }

    private:
        static constexpr std::size_t kInPlaceCapacity = 15;

        // Holds a longer text, so the layout's pointer stays valid across moves.
        std::unique_ptr<char[]> heap;
        Layout layout{};
    };

    // RaceMenu's FaceMorphInterface: SKEE's own slider values per actor base, which
    // its UpdateNPCMorphs hook applies to a head built at runtime. Checked against
    // skee64.dll of RaceMenu for 1.7.104 (GetVersion returns 2); older builds put
    // Load and Save between GetVersion and Revert under the same version.
    class IFaceMorphInterface : public IPluginInterface
    {
    public:
        virtual float GetMorphValueByName(RE::TESNPC* a_npc, const SKEEString::Layout& a_name) = 0;
        virtual void SetMorphValue(RE::TESNPC* a_npc, const SKEEString::Layout& a_name, float a_value) = 0;
    };

    // Asks SKEE for its interfaces; at SKSE's kPostPostLoad, once every plugin listens.
    void Initialize();

    // Null when RaceMenu is missing or too old; the overlay pair is null together.
    [[nodiscard]] IOverlayInterface* GetOverlays();
    [[nodiscard]] IOverrideInterface* GetOverrides();
    [[nodiscard]] IBodyMorphInterface* GetBodyMorphs();
    [[nodiscard]] IFaceMorphInterface* GetFaceMorphs();
}
