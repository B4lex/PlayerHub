#pragma once

#include "forms/ItemFilter.h"
#include "protocol/FormReference.h"
#include "text/Text.h"

#include <imgui.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Forms
{
    class ItemCatalog;
    struct CatalogItem;
}

namespace UI
{
    class ItemPicker
    {
    public:
        bool Draw(const char* a_id, Multiplayer::FormReference& a_value, const Multiplayer::FormReference& a_default,
            const Forms::ItemFilter& a_filter, bool a_allowNone);

        void Reset();

    private:
        enum class LabelKind : std::uint8_t
        {
            kNone,
            kItem,
            kMismatch,
            kUnlisted,
            kPluginMissing
        };

        struct Label
        {
            Multiplayer::FormReference value;
            std::string text;
            LabelKind kind{ LabelKind::kNone };
            bool catalogReady{ false };
            Text::Language language{ Text::Language::kEnglish };
        };

        const Label& GetLabel(
            ImGuiID a_id, const Multiplayer::FormReference& a_value, const Forms::ItemFilter& a_filter);
        bool DrawList(Multiplayer::FormReference& a_value, const Forms::ItemFilter& a_filter, bool a_allowNone);
        void DrawDetails(const Forms::CatalogItem* a_item, float a_width);
        void Search(const Forms::ItemFilter& a_filter);

        std::shared_ptr<const Forms::ItemCatalog> catalog;
        std::unordered_map<ImGuiID, Label> labels;

        std::string query;
        std::vector<std::uint32_t> matches;
        const Forms::CatalogItem* current{ nullptr };
        int currentRow{ -1 };
        bool scrollToCurrent{ false };
    };
}
