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
    class SettingsVisitor
    {
    public:
        virtual ~SettingsVisitor() = default;

        virtual void Section(std::string_view a_name, std::string_view a_comment) = 0;
        virtual void Comment(std::string_view a_comment) = 0;
        virtual void Bool(std::string_view a_key, bool& a_value, std::string_view a_comment) = 0;
        virtual void Unsigned(std::string_view a_key, std::uint32_t& a_value, std::string_view a_comment,
            std::uint32_t a_maximum, std::string_view a_zeroName) = 0;
        virtual void Percent(std::string_view a_key, std::uint32_t& a_value, std::string_view a_comment,
            std::uint32_t a_minimum, std::uint32_t a_maximum) = 0;
        virtual void String(std::string_view a_key, std::string& a_value, std::string_view a_comment,
            bool (*a_isValid)(std::string_view)) = 0;
        virtual void Form(std::string_view a_key, Multiplayer::FormReference& a_value,
            const Multiplayer::FormReference& a_default, const Forms::ItemFilter& a_filter) = 0;
        virtual void Choice(std::string_view a_key, std::uint32_t& a_value, std::string_view a_comment,
            std::span<const std::string_view> a_options) = 0;
        virtual void Color(std::string_view a_key, std::uint32_t& a_value, std::string_view a_comment,
            std::span<const std::uint32_t> a_swatches) = 0;
        virtual void Key(std::string_view a_key, std::uint32_t& a_value, std::string_view a_comment) = 0;
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

    [[nodiscard]] std::span<const BindableKey> GetBindableKeys();

    [[nodiscard]] std::optional<bool> ParseBool(std::string_view a_value);
    [[nodiscard]] std::optional<std::uint32_t> ParseUnsigned(std::string_view a_value);
    [[nodiscard]] std::optional<Multiplayer::FormReference> ParseFormReference(std::string_view a_value);
    [[nodiscard]] std::string FormatFormReference(const Multiplayer::FormReference& a_reference);
    [[nodiscard]] std::optional<std::uint32_t> ParseChoice(
        std::string_view a_value, std::span<const std::string_view> a_options);
    [[nodiscard]] std::string_view FormatChoice(std::uint32_t a_value, std::span<const std::string_view> a_options);
    [[nodiscard]] std::optional<std::uint32_t> ParseColor(std::string_view a_value);
    [[nodiscard]] std::string FormatColor(std::uint32_t a_color);
    [[nodiscard]] std::optional<std::uint32_t> ParseKey(std::string_view a_value);
    [[nodiscard]] std::string_view FormatKey(std::uint32_t a_scanCode);

    [[nodiscard]] std::string_view GetOutfitArmorKey(std::size_t a_slotIndex);
    [[nodiscard]] std::string_view GetOutfitWeaponKey(std::size_t a_weaponType);

    [[nodiscard]] Forms::ItemFilter GetOutfitArmorFilter(std::size_t a_slotIndex);
    [[nodiscard]] Forms::ItemFilter GetOutfitWeaponFilter(std::size_t a_weaponType);
    inline constexpr Forms::ItemFilter kOutfitRaceFilter{ .kind = Forms::ItemKind::kRace, .baseGameOnly = true };
    inline constexpr Forms::ItemFilter kOutfitArrowFilter{ .kind = Forms::ItemKind::kAmmo, .baseGameOnly = true };
    inline constexpr Forms::ItemFilter kOutfitBoltFilter{
        .kind = Forms::ItemKind::kAmmo, .bolt = true, .baseGameOnly = true
    };
}
