#pragma once

#include "forms/ItemFilter.h"
#include "protocol/FormReference.h"

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
    // A dropdown that picks an item form from Forms::ItemCatalog, limited to the
    // items an ItemFilter matches. Its list is searched by name and plugin and,
    // with [UI] ItemPreview, shows the hovered item's 3D model. One picker serves
    // every field of a window, as at most one list is open at a time. Overlay
    // thread.
    class ItemPicker
    {
    public:
        // Draws the field for a_value at the cursor, as wide as the next ImGui
        // item. Returns true when the user picked an item or None.
        bool Draw(const char* a_id, Multiplayer::FormReference& a_value, const Forms::ItemFilter& a_filter);

        // Forgets cached field labels, for example when the window reopens.
        void Reset();

    private:
        enum class LabelKind : std::uint8_t
        {
            kNone,
            kItem,
            // In the catalog, but a_filter does not match it.
            kMismatch,
            // Not in the catalog, from a loaded plugin, or before the catalog is built.
            kUnlisted,
            kPluginMissing
        };

        struct Label
        {
            Multiplayer::FormReference value;
            std::string text;
            LabelKind kind{ LabelKind::kNone };
            bool catalogReady{ false };
        };

        const Label& GetLabel(
            ImGuiID a_id, const Multiplayer::FormReference& a_value, const Forms::ItemFilter& a_filter);
        bool DrawList(Multiplayer::FormReference& a_value, const Forms::ItemFilter& a_filter);
        void DrawDetails(const Forms::CatalogItem* a_item, float a_width);
        void Search(const Forms::ItemFilter& a_filter);

        // Fetched until the catalog is built; it never changes after that.
        std::shared_ptr<const Forms::ItemCatalog> catalog;
        std::unordered_map<ImGuiID, Label> labels;

        // State of the open list.
        std::string query;
        std::vector<std::uint32_t> matches;
        const Forms::CatalogItem* current{ nullptr };
        bool currentIsNone{ false };
        // The list row of the field's value, or -1 when the search hides it.
        int currentRow{ -1 };
        bool scrollToCurrent{ false };
    };
}
