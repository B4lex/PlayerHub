#include "ui/widgets/ItemPicker.h"
#include "config/PluginConfig.h"
#include "config/SettingsSchema.h"
#include "forms/ItemCatalog.h"
#include "text/Text.h"
#include "ui/Theme.h"
#include "ui/preview/ItemPreview.h"
#include "ui/widgets/Controls.h"

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <format>

namespace UI
{
    namespace
    {
        using Multiplayer::FormReference;

        constexpr float kPopupWidthFonts = 40.0F;
        constexpr float kPopupHeightFonts = 22.0F;
        constexpr float kPreviewWidthFonts = 13.0F;
        constexpr ImU32 kPreviewBackground = IM_COL32(28, 28, 31, 255);
        constexpr float kPluginColumnWidthFonts = 11.0F;

        constexpr ImGuiTableFlags kListFlags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg;
    }

    bool ItemPicker::Draw(const char* a_id, FormReference& a_value, const FormReference& a_default,
        const Forms::ItemFilter& a_filter, bool a_allowNone)
    {
        if (!catalog)
        {
            catalog = Forms::GetItemCatalog();
        }

        ImGui::PushID(a_id);
        const auto& label = GetLabel(ImGui::GetID("##item"), a_value, a_filter);
        const auto fontSize = ImGui::GetFontSize();
        const auto& style = ImGui::GetStyle();
        const bool resettable = a_value != a_default;
        const auto resetWidth = resettable ? ImGui::GetFrameHeight() + style.ItemSpacing.x : 0.0F;
        const auto comboWidth = (std::max)(ImGui::CalcItemWidth() - resetWidth, fontSize);
        ImGui::SetNextItemWidth(comboWidth);
        ImGui::SetNextWindowSize(
            ImVec2{ (std::max)(comboWidth, kPopupWidthFonts * fontSize), kPopupHeightFonts * fontSize });

        const bool warning = label.kind == LabelKind::kMismatch || label.kind == LabelKind::kPluginMissing;
        const bool dim = label.kind == LabelKind::kNone;
        if (warning || dim)
        {
            ImGui::PushStyleColor(
                ImGuiCol_Text, warning ? Theme::kWarning : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        }
        const bool open = ImGui::BeginCombo("##item", label.text.c_str());
        if (warning || dim)
        {
            ImGui::PopStyleColor();
        }

        if (!open && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        {
            switch (label.kind)
            {
                case LabelKind::kMismatch:
                    ImGui::SetTooltip("%s",
                        Text::Format(Text::Id::kItemPickerMismatch, Config::FormatFormReference(a_value)).c_str());
                    break;
                case LabelKind::kPluginMissing:
                    ImGui::SetTooltip("%s", Text::Format(Text::Id::kItemPickerPluginMissing, a_value.plugin).c_str());
                    break;
                case LabelKind::kItem:
                    ImGui::SetTooltip("%s", Config::FormatFormReference(a_value).c_str());
                    break;
                default:
                    break;
            }
        }

        bool changed = false;
        if (open)
        {
            changed = DrawList(a_value, a_filter, a_allowNone);
            ImGui::EndCombo();
        }

        if (resettable)
        {
            ImGui::SameLine();
            const bool reset = Controls::IconButton("##reset", Controls::Icon::kReset);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            {
                ImGui::SetTooltip("%s",
                    Text::Format(Text::Id::kItemPickerReset, GetLabel(ImGui::GetID("##default"), a_default, a_filter).text)
                        .c_str());
            }
            if (reset)
            {
                a_value = a_default;
                changed = true;
            }
        }
        ImGui::PopID();
        return changed;
    }

    void ItemPicker::Reset()
    {
        labels.clear();
    }

    const ItemPicker::Label& ItemPicker::GetLabel(
        ImGuiID a_id, const FormReference& a_value, const Forms::ItemFilter& a_filter)
    {
        auto& label = labels[a_id];
        const bool catalogReady = catalog != nullptr;
        const auto language = Text::GetLanguage();
        if (!label.text.empty() && label.value == a_value && label.catalogReady == catalogReady
            && label.language == language)
        {
            return label;
        }

        label.value = a_value;
        label.catalogReady = catalogReady;
        label.language = language;
        const auto* item = catalog ? catalog->Find(a_value) : nullptr;
        if (a_value.IsEmpty())
        {
            label.kind = LabelKind::kNone;
            label.text = Text::Get(Text::Id::kItemPickerNone);
        }
        else if (item)
        {
            label.kind = a_filter.Matches(item->traits) ? LabelKind::kItem : LabelKind::kMismatch;
            label.text = item->name;
        }
        else if (catalog && !catalog->IsPluginLoaded(a_value.plugin))
        {
            label.kind = LabelKind::kPluginMissing;
            label.text = Text::Format(Text::Id::kItemPickerNotLoaded, Config::FormatFormReference(a_value));
        }
        else
        {
            label.kind = LabelKind::kUnlisted;
            label.text = Config::FormatFormReference(a_value);
        }
        return label;
    }

    bool ItemPicker::DrawList(FormReference& a_value, const Forms::ItemFilter& a_filter, bool a_allowNone)
    {
        if (!catalog)
        {
            ImGui::TextDisabled("%s", Text::GetCString(Text::Id::kItemPickerNotReady));
            return false;
        }

        const auto fontSize = ImGui::GetFontSize();
        if (ImGui::IsWindowAppearing())
        {
            query.clear();
            current = catalog->Find(a_value);
            Search(a_filter);
            scrollToCurrent = true;
            ImGui::SetKeyboardFocusHere();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##search", Text::GetCString(Text::Id::kItemPickerSearchHint), &query))
        {
            Search(a_filter);
        }
        const auto items = catalog->GetItems();
        const bool noneRow = a_allowNone && query.empty();
        if (ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Enter, false) && (noneRow || !matches.empty()))
        {
            a_value = noneRow ? FormReference{} : items[matches.front()].reference;
            ImGui::CloseCurrentPopup();
            return true;
        }

        const bool preview = Config::Get()->ui.itemPreview && Forms::HasModel(a_filter.kind);
        const float previewWidth = preview ? kPreviewWidthFonts * fontSize : 0.0F;
        const auto available = ImGui::GetContentRegionAvail();
        const float listWidth = preview ? available.x - previewWidth - ImGui::GetStyle().ItemSpacing.x : 0.0F;

        bool changed = false;
        const Forms::CatalogItem* hovered = nullptr;
        bool noneHovered = false;
        if (ImGui::BeginTable("##items", 2, kListFlags, ImVec2{ listWidth, available.y }))
        {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Plugin", ImGuiTableColumnFlags_WidthFixed, kPluginColumnWidthFonts * fontSize);

            if (noneRow)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable(Text::GetCString(Text::Id::kItemPickerNone), a_value.IsEmpty(),
                        ImGuiSelectableFlags_SpanAllColumns))
                {
                    a_value = {};
                    changed = true;
                    ImGui::CloseCurrentPopup();
                }
                noneHovered = ImGui::IsItemHovered();
            }

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(matches.size()));
            if (scrollToCurrent && currentRow >= 0)
            {
                clipper.IncludeItemByIndex(currentRow);
            }
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                {
                    const auto* item = &items[matches[static_cast<std::size_t>(row)]];
                    const bool selected = row == currentRow;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushID(row);
                    if (ImGui::Selectable(item->name.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
                    {
                        a_value = item->reference;
                        changed = true;
                        // The table's scrolling child is not the popup, so the
                        // selectable does not close it.
                        ImGui::CloseCurrentPopup();
                    }
                    if (ImGui::IsItemHovered())
                    {
                        hovered = item;
                    }
                    if (selected && scrollToCurrent)
                    {
                        ImGui::SetScrollHereY(0.5F);
                    }
                    ImGui::PopID();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", item->reference.plugin.c_str());
                }
            }
            scrollToCurrent = false;
            if (matches.empty() && !noneRow)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", Text::GetCString(Text::Id::kItemsNoMatch));
            }
            ImGui::EndTable();
        }

        if (preview)
        {
            ImGui::SameLine();
            DrawDetails(noneHovered ? nullptr : (hovered ? hovered : current), previewWidth);
        }
        return changed;
    }

    void ItemPicker::DrawDetails(const Forms::CatalogItem* a_item, float a_width)
    {
        ImGui::BeginGroup();
        const auto origin = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(origin, ImVec2{ origin.x + a_width, origin.y + a_width },
            kPreviewBackground, ImGui::GetStyle().FrameRounding);
        ItemPreview::DrawModel(a_item ? a_item->formID : 0, ImVec2{ a_width, a_width });
        if (a_item)
        {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + a_width);
            ImGui::TextUnformatted(a_item->name.c_str());
            ImGui::TextDisabled("%s", Config::FormatFormReference(a_item->reference).c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndGroup();
    }

    void ItemPicker::Search(const Forms::ItemFilter& a_filter)
    {
        catalog->Search(a_filter, query, matches);
        const auto items = catalog->GetItems();
        const auto match = std::ranges::find_if(
            matches, [this, items](std::uint32_t a_index) { return &items[a_index] == current; });
        currentRow = match != matches.end() ? static_cast<int>(match - matches.begin()) : -1;
    }
}
