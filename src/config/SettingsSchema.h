#pragma once

#include "config/PluginConfig.h"
#include "forms/ItemFilter.h"
#include "protocol/FormReference.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Config
{
    // One description of every setting drives reading the file, writing it, and
    // the in-game settings window. A new option is a field in PluginConfig plus a
    // line in Describe.
    class SettingsVisitor
    {
    public:
        virtual ~SettingsVisitor() = default;

        virtual void Section(std::string_view a_name, std::string_view a_comment) = 0;
        virtual void Comment(std::string_view a_comment) = 0;
        virtual void Bool(std::string_view a_key, bool& a_value, std::string_view a_comment) = 0;
        // a_zeroName is what zero stands for where it is not a count, such as
        // Unlimited; the file still stores 0.
        virtual void Unsigned(std::string_view a_key, std::uint32_t& a_value, std::string_view a_comment,
            std::uint32_t a_maximum, std::string_view a_zeroName) = 0;
        // A percentage within a_minimum and a_maximum.
        virtual void Percent(std::string_view a_key, std::uint32_t& a_value, std::string_view a_comment,
            std::uint32_t a_minimum, std::uint32_t a_maximum) = 0;
        virtual void String(std::string_view a_key, std::string& a_value, std::string_view a_comment,
            bool (*a_isValid)(std::string_view)) = 0;
        // Grouped under a preceding Comment, so a form entry has none of its own.
        // Never empty; a_default is what a reset or an invalid value takes. Only
        // an item a_filter matches is used.
        virtual void Form(std::string_view a_key, Multiplayer::FormReference& a_value,
            const Multiplayer::FormReference& a_default, const Forms::ItemFilter& a_filter) = 0;
        // An index into a_options, which the file stores by name.
        virtual void Choice(std::string_view a_key, std::uint32_t& a_value, std::string_view a_comment,
            std::span<const std::string_view> a_options) = 0;
        // An opaque 0xRRGGBB color, which the file stores as six hexadecimal
        // digits. a_swatches are colors to offer.
        virtual void Color(std::string_view a_key, std::uint32_t& a_value, std::string_view a_comment,
            std::span<const std::uint32_t> a_swatches) = 0;
        // A DirectInput scan code from GetBindableKeys.
        virtual void Key(std::string_view a_key, std::uint32_t& a_value, std::string_view a_comment) = 0;
        // A measure of a window within a_minimum and a_maximum, which the
        // player sets by dragging that window. The settings window does not
        // show it.
        virtual void Layout(std::string_view a_key, std::uint32_t& a_value, std::string_view a_comment,
            std::uint32_t a_minimum, std::uint32_t a_maximum) = 0;
    };

    void Describe(PluginConfig& a_config, SettingsVisitor& a_visitor);

    [[nodiscard]] const PluginConfig& GetDefaultConfig();

    struct BindableKey
    {
        std::string_view name;
        std::uint32_t scanCode;
    };

    // Keys a Key setting accepts. Keys the game or the overlay already use for
    // typing and movement are left out.
    [[nodiscard]] std::span<const BindableKey> GetBindableKeys();

    // Text forms of setting values, as the file stores them.
    [[nodiscard]] std::optional<bool> ParseBool(std::string_view a_value);
    [[nodiscard]] std::optional<std::uint32_t> ParseUnsigned(std::string_view a_value);
    // `Plugin.esp|0x0012AB`: the defining plugin and the hexadecimal form ID local
    // to it. A full load-order form ID would not match across modpacks.
    [[nodiscard]] std::optional<Multiplayer::FormReference> ParseFormReference(std::string_view a_value);
    [[nodiscard]] std::string FormatFormReference(const Multiplayer::FormReference& a_reference);
    // The index of the option a_value names, compared without case.
    [[nodiscard]] std::optional<std::uint32_t> ParseChoice(
        std::string_view a_value, std::span<const std::string_view> a_options);
    // Empty for an index past a_options.
    [[nodiscard]] std::string_view FormatChoice(std::uint32_t a_value, std::span<const std::string_view> a_options);
    // `FF9000`, with or without a leading `#`.
    [[nodiscard]] std::optional<std::uint32_t> ParseColor(std::string_view a_value);
    [[nodiscard]] std::string FormatColor(std::uint32_t a_color);
    [[nodiscard]] std::optional<std::uint32_t> ParseKey(std::string_view a_value);
    // Empty for a scan code that GetBindableKeys does not list.
    [[nodiscard]] std::string_view FormatKey(std::uint32_t a_scanCode);

    // [DefaultOutfit] key names, so diagnostics name what to edit.
    [[nodiscard]] std::string_view GetOutfitArmorKey(std::size_t a_slotIndex);
    [[nodiscard]] std::string_view GetOutfitWeaponKey(std::size_t a_weaponType);

    // The forms each [DefaultOutfit] key accepts: base-game forms only, which
    // every peer can resolve.
    [[nodiscard]] Forms::ItemFilter GetOutfitArmorFilter(std::size_t a_slotIndex);
    [[nodiscard]] Forms::ItemFilter GetOutfitWeaponFilter(std::size_t a_weaponType);
    inline constexpr Forms::ItemFilter kOutfitRaceFilter{ .kind = Forms::ItemKind::kRace, .baseGameOnly = true };
    inline constexpr Forms::ItemFilter kOutfitArrowFilter{ .kind = Forms::ItemKind::kAmmo, .baseGameOnly = true };
    inline constexpr Forms::ItemFilter kOutfitBoltFilter{
        .kind = Forms::ItemKind::kAmmo, .bolt = true, .baseGameOnly = true
    };
}
