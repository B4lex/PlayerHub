#include "ui/settings/UnsyncedLocations.h"
#include "config/SettingsSchema.h"
#include "sync/chat/ChatRoster.h"
#include "text/Text.h"
#include "ui/Fonts.h"
#include "ui/MenuFrame.h"
#include "ui/Theme.h"
#include "ui/chat/ChatStyle.h"
#include "ui/widgets/Controls.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace UI
{
    namespace
    {
        constexpr ImVec2 kSizeFonts{ 28.0F, 22.0F };
        constexpr float kTitleScale = 1.25F;
        constexpr ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    }

    void UnsyncedLocations::Open()
    {
        open = true;
        focusRequested = true;
        search[0] = '\0';
        foldedSearch.clear();
        revision.reset();
    }

    void UnsyncedLocations::Draw()
    {
        if (!open)
        {
            return;
        }
        const auto metrics = MenuFrame::GetMetrics();
        const auto inset = std::floor((metrics.inset + metrics.corner) * 0.5F);
        const auto& style = ImGui::GetStyle();
        const ImVec2 padding{ style.WindowPadding.x + inset, style.WindowPadding.y + inset };
        const auto fontSize = ImGui::GetFontSize();
        const auto& display = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(
            ImVec2{ display.x * 0.5F, display.y * 0.5F }, ImGuiCond_Appearing, ImVec2{ 0.5F, 0.5F });
        ImGui::SetNextWindowSize(ImVec2{ std::floor(kSizeFonts.x * fontSize) + padding.x * 2.0F,
            std::floor(kSizeFonts.y * fontSize) + padding.y * 2.0F });
        if (std::exchange(focusRequested, false))
        {
            ImGui::SetNextWindowFocus();
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
        const bool visible = ImGui::Begin("##UnsyncedLocations", nullptr, kWindowFlags);
        ImGui::PopStyleVar();
        if (visible)
        {
            MenuFrame::ShowMoveCursor();
            MenuFrame::Draw(ImGui::GetColorU32(Theme::GetFrameColor()), ImGui::GetColorU32(ImGuiCol_WindowBg));
            DrawContents();
        }
        ImGui::End();
    }

    void UnsyncedLocations::DrawContents()
    {
        Refresh();
        const auto& style = ImGui::GetStyle();
        const auto top = ImGui::GetCursorPosY();
        const auto right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        ImGui::PushFont(Fonts::GetHeading(), style.FontSizeBase * kTitleScale);
        ImGui::TextUnformatted(Text::GetCString(Text::ID::kUnsyncedTitle));
        const auto titleHeight = ImGui::GetTextLineHeight();
        ImGui::PopFont();
        const auto titleBottom = ImGui::GetCursorPosY();
        // Centered on the title, so the taller button does not push the rule further from the title than from
        // the search field under it.
        ImGui::SetCursorPos(ImVec2{
            right - ImGui::GetFrameHeight(), top + std::floor((titleHeight - ImGui::GetFrameHeight()) * 0.5F) });
        if (Controls::IconButton("##UnsyncedLocationsClose", Controls::Icon::kClose))
        {
            Close();
        }
        ImGui::SetCursorPosY((std::max)(titleBottom, ImGui::GetCursorPosY()));

        ChatStyle::DrawRule(style.ItemSpacing.y);
        if (!rows.empty())
        {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ChatStyle::GetChipPadding());
            if (Controls::SearchField("##UnsyncedLocationsSearch", search))
            {
                foldedSearch = ChatRoster::FoldName(search.data());
                Match();
            }
            ImGui::PopStyleVar();
        }
        DrawRows();
        DrawLocalCell();
    }

    void UnsyncedLocations::Refresh()
    {
        const auto current = UnsyncedCells::GetRevision();
        if (revision == current)
        {
            return;
        }
        revision = current;
        std::vector<UnsyncedCells::Entry> entries;
        UnsyncedCells::Copy(entries);
        rows.clear();
        for (auto& entry : entries)
        {
            Row row{ .folded = ChatRoster::FoldName(entry.name), .reference = Config::FormatFormReference(entry.cell) };
            row.entry = std::move(entry);
            rows.push_back(std::move(row));
        }
        std::ranges::sort(rows, {}, &Row::folded);
        Match();
    }

    void UnsyncedLocations::Match()
    {
        matches.clear();
        for (std::uint32_t index = 0; index < rows.size(); ++index)
        {
            if (foldedSearch.empty() || rows[index].folded.find(foldedSearch) != std::string::npos)
            {
                matches.push_back(index);
            }
        }
    }

    void UnsyncedLocations::DrawLocalCell()
    {
        const auto local = UnsyncedCells::GetLocalCell();
        const auto* label = Text::GetCString(Text::ID::kUnsyncedAdd);
        const auto width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0F;
        ImGui::SetCursorPosX(
            (std::max)(ImGui::GetCursorPosX() + std::floor((ImGui::GetContentRegionAvail().x - width) * 0.5F),
                ImGui::GetCursorPosX()));
        ImGui::BeginDisabled(!local || local->unsynced);
        if (Controls::PrimaryButton(label))
        {
            UnsyncedCells::AddLocalCell();
        }
        ImGui::EndDisabled();
        if (local && local->unsynced
            && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("%s", Text::GetCString(Text::ID::kUnsyncedAlready));
        }
    }

    void UnsyncedLocations::DrawRows()
    {
        if (!ImGui::BeginChild("##UnsyncedLocationsRows", ImVec2{ 0.0F, -ImGui::GetFrameHeightWithSpacing() }))
        {
            ImGui::EndChild();
            return;
        }
        if (rows.empty())
        {
            Controls::MutedTextWrapped(Text::GetCString(Text::ID::kUnsyncedNone));
        }
        else if (matches.empty())
        {
            Controls::MutedTextWrapped(Text::GetCString(Text::ID::kUnsyncedNoMatch));
        }

        const auto& style = ImGui::GetStyle();
        const auto* removeLabel = Text::GetCString(Text::ID::kUnsyncedRemove);
        const auto buttonWidth = ImGui::CalcTextSize(removeLabel).x + style.FramePadding.x * 2.0F;
        const auto buttonLeft = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - buttonWidth;
        const auto frameHeight = ImGui::GetFrameHeight();
        const auto textColor = ImGui::GetColorU32(ImGuiCol_Text);
        auto* drawList = ImGui::GetWindowDrawList();

        const UnsyncedCells::Entry* removed = nullptr;
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(matches.size()), ImGui::GetFrameHeightWithSpacing());
        while (clipper.Step())
        {
            for (auto index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
            {
                const auto& row = rows[matches[static_cast<std::size_t>(index)]];
                const char* name = row.entry.name.c_str();
                const char* nameEnd = name + row.entry.name.size();
                ImGui::PushID(index);

                const auto columnWidth = (std::max)(buttonLeft - style.ItemSpacing.x - ImGui::GetCursorPosX(), 1.0F);
                const auto nameWidth = (std::min)(ImGui::CalcTextSize(name, nameEnd).x, columnWidth);
                ImGui::Dummy(ImVec2{ nameWidth, frameHeight });
                const auto min = ImGui::GetItemRectMin();
                const auto max = ImGui::GetItemRectMax();
                const ImVec4 clip{ min.x, min.y, max.x, max.y };
                const auto textTop = std::floor(min.y + (frameHeight - ImGui::GetTextLineHeight()) * 0.5F);
                drawList->AddText(nullptr, 0.0F, ImVec2{ min.x, textTop }, textColor, name, nameEnd, 0.0F, &clip);
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s", row.reference.c_str());
                }

                ImGui::SameLine(buttonLeft);
                if (ImGui::Button(removeLabel, ImVec2{ buttonWidth, 0.0F }))
                {
                    removed = &row.entry;
                }
                ImGui::PopID();
            }
        }
        // The rows change with it, so after the clipper is done with them.
        if (removed)
        {
            UnsyncedCells::Remove(removed->cell);
        }
        ImGui::EndChild();
    }
}
