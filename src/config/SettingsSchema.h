#pragma once

#include "config/PluginConfig.h"
#include "protocol/EquipmentState.h"
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
        virtual void Unsigned(
            std::string_view a_key, std::uint32_t& a_value, std::string_view a_comment, std::uint32_t a_maximum) = 0;
        virtual void String(std::string_view a_key, std::string& a_value, std::string_view a_comment,
            bool (*a_isValid)(std::string_view)) = 0;
        // Grouped under a preceding Comment, so a form entry has none of its own.
        virtual void Form(std::string_view a_key, Multiplayer::FormReference& a_value) = 0;
        // A DirectInput scan code from GetBindableKeys.
        virtual void Key(std::string_view a_key, std::uint32_t& a_value, std::string_view a_comment) = 0;
    };

    void Describe(PluginConfig& a_config, SettingsVisitor& a_visitor);

    [[nodiscard]] PluginConfig MakeDefaultConfig();

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
    [[nodiscard]] std::optional<std::uint32_t> ParseKey(std::string_view a_value);
    // Empty for a scan code that GetBindableKeys does not list.
    [[nodiscard]] std::string_view FormatKey(std::uint32_t a_scanCode);

    // [EquipmentFallback] key names, so diagnostics name what to edit. No armor
    // type means the slot's any-type key.
    [[nodiscard]] std::string GetArmorFallbackKey(
        std::size_t a_slotIndex, std::optional<Multiplayer::EquipmentArmorType> a_armorType);
    [[nodiscard]] std::string_view GetWeaponFallbackKey(std::size_t a_weaponType);
}
