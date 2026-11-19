#include "ui/hud/TimeBreakdownPanel.h"

#ifdef PLAYERHUB_TIME_BREAKDOWN
#    include "config/PluginConfig.h"
#    include "runtime/GameSession.h"
#    include "runtime/PluginTime.h"

#    include <imgui.h>
#    include <imgui_internal.h>

#    include <chrono>
#    include <vector>

namespace UI
{
    namespace
    {
        constexpr float kMarginPixels = 12.0F;
        constexpr float kStatusPanelGapPixels = 6.0F;
        constexpr float kBackgroundAlpha = 0.35F;
        constexpr int kIndentSpaces = 2;
        constexpr ImVec4 kMutedColor{ 0.70F, 0.70F, 0.70F, 1.0F };
        constexpr const char* kStatusPanelName = "##StatusPanel";

        constexpr ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_AlwaysAutoResize;

        // The row names come from code, so they are not translated.
        void DrawRows(const char* a_id, const std::vector<PluginTime::BreakdownRow>& a_rows)
        {
            if (a_rows.empty() || !ImGui::BeginTable(a_id, 3, ImGuiTableFlags_SizingFixedFit))
            {
                return;
            }
            for (const auto& row : a_rows)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(kMutedColor, "%*s%s", row.depth * kIndentSpaces, "", row.name);
                ImGui::TableNextColumn();
                const std::chrono::duration<double, std::milli> milliseconds = row.time;
                ImGui::TextColored(kMutedColor, "%.2f", milliseconds.count());
                ImGui::TableNextColumn();
                if (row.callsPerPass)
                {
                    ImGui::TextColored(kMutedColor, "%.0f/f", *row.callsPerPass);
                }
            }
            ImGui::EndTable();
        }

        // Below the status panel when it was drawn this frame, which it is before this window.
        float GetTop()
        {
            const auto* statusPanel = ImGui::FindWindowByName(kStatusPanelName);
            return statusPanel && statusPanel->Active
                ? statusPanel->Pos.y + statusPanel->Size.y + kStatusPanelGapPixels
                : kMarginPixels;
        }

        class TimeBreakdownPanel final : public Window
        {
        public:
            void Draw(const WindowInput&) override;

        private:
            std::vector<PluginTime::BreakdownRow> stageRows;
            std::vector<PluginTime::BreakdownRow> hookRows;
        };
    }

    void TimeBreakdownPanel::Draw(const WindowInput&)
    {
        if (!GameSession::IsInGame() || !Config::Get()->advanced.statusPanel)
        {
            return;
        }

        PluginTime::CopyBreakdown(stageRows, hookRows);
        if (stageRows.empty() && hookRows.empty())
        {
            return;
        }

        ImGui::SetNextWindowPos(ImVec2{ ImGui::GetIO().DisplaySize.x - kMarginPixels, GetTop() }, ImGuiCond_Always,
            ImVec2{ 1.0F, 0.0F });
        ImGui::SetNextWindowBgAlpha(kBackgroundAlpha);
        if (ImGui::Begin("##TimeBreakdownPanel", nullptr, kWindowFlags))
        {
            DrawRows("##Stages", stageRows);
            // Last, so its changing rows never move the stages.
            if (!hookRows.empty())
            {
                if (!stageRows.empty())
                {
                    ImGui::Separator();
                }
                DrawRows("##Hooks", hookRows);
            }
        }
        ImGui::End();
    }

    std::unique_ptr<Window> CreateTimeBreakdownPanel()
    {
        return std::make_unique<TimeBreakdownPanel>();
    }
}
#endif
