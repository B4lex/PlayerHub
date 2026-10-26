#include "ui/hud/NameplateLayer.h"
#include "config/PluginConfig.h"
#include "runtime/GameSession.h"
#include "runtime/Profiling.h"
#include "sync/nameplates/NameplateSync.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"
#include "ui/hud/GameView.h"

#include <imgui.h>

#include <RE/Skyrim.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <optional>
#include <span>

namespace UI
{
    namespace
    {
        // World units above the head node's origin, at the node's scale.
        constexpr float kHeadClearance = 20.0F;
        constexpr float kNearFontScale = 1.2F;
        constexpr float kFarFontScale = 0.8F;
        constexpr float kDistanceFadeFraction = 0.15F;
        constexpr float kOutlineAlpha = 0.8F;
        constexpr float kMinimumDepth = 1e-5F;
        constexpr std::array<ImVec2, 4> kOutlineOffsets{ { { -1.0F, -1.0F }, { 1.0F, -1.0F }, { -1.0F, 1.0F },
            { 1.0F, 1.0F } } };
        constexpr std::size_t kStandInPips = 3;
        // A pip's width in font sizes; pips are a width apart and two before the name.
        constexpr float kPipWidth = 0.12F;
        constexpr auto kStandInMarkWidths = static_cast<float>(2 * kStandInPips + 1);
        // Font sizes from the text's top.
        constexpr float kPipBaseline = 0.8F;
        constexpr float kPipMinimumHeight = 0.3F;
        constexpr float kPipHeightStep = 0.2F;
        constexpr float kUnlitPipAlpha = 0.3F;

        std::optional<ImVec2> Project(RE::NiCamera& a_camera, const RE::NiPoint3& a_point, const ImVec2& a_display)
        {
            // NiCamera::WorldPtToScreenPt3 divides by the depth's absolute value, so it also places points
            // behind the camera on screen; its sign is tested here first.
            const auto& matrix = a_camera.GetRuntimeData().worldToCam;
            const auto depth = matrix[3][0] * a_point.x + matrix[3][1] * a_point.y + matrix[3][2] * a_point.z
                             + matrix[3][3];
            float x = 0.0F;
            float y = 0.0F;
            float z = 0.0F;
            if (depth <= kMinimumDepth || !a_camera.WorldPtToScreenPt3(a_point, x, y, z, kMinimumDepth))
            {
                return std::nullopt;
            }
            return ImVec2{ x * a_display.x, (1.0F - y) * a_display.y };
        }

        ImVec4 GetStandInColor(StandIn::Level a_level)
        {
            switch (a_level)
            {
                case StandIn::Level::kMajor:
                    return Theme::kNegative;
                case StandIn::Level::kNotable:
                    return Theme::kWarning;
                default:
                    return Theme::Tint(1.0F);
            }
        }

        // Rising bars, as many lit as the level.
        void DrawStandInPips(ImDrawList& a_drawList, ImVec2 a_origin, float a_size, float a_pipWidth, float a_outline,
            StandIn::Level a_level, float a_alpha)
        {
            const auto bottom = std::round(a_origin.y + a_size * kPipBaseline);
            const auto shadow = ImGui::GetColorU32(ImVec4{ 0.0F, 0.0F, 0.0F, kOutlineAlpha * a_alpha });
            auto litColor = GetStandInColor(a_level);
            litColor.w = a_alpha;
            const auto lit = ImGui::GetColorU32(litColor);
            const auto unlit = ImGui::GetColorU32(Theme::Tint(kUnlitPipAlpha * a_alpha));
            for (std::size_t pip = 0; pip < kStandInPips; ++pip)
            {
                const auto x = a_origin.x + a_pipWidth * static_cast<float>(2 * pip);
                const auto height = std::round(a_size * (kPipMinimumHeight + kPipHeightStep * static_cast<float>(pip)));
                const ImVec2 min{ x, bottom - height };
                const ImVec2 max{ x + a_pipWidth, bottom };
                a_drawList.AddRectFilled(ImVec2{ min.x - a_outline, min.y - a_outline },
                    ImVec2{ max.x + a_outline, max.y + a_outline }, shadow);
                a_drawList.AddRectFilled(min, max, pip < static_cast<std::size_t>(a_level) ? lit : unlit);
            }
        }

        class NameplateLayer final : public Window
        {
        public:
            void Draw(const WindowInput&) override
            {
                if (!GameSession::IsInGame() || !Config::Get()->ui.nameplates)
                {
                    return;
                }
                const auto plates = NameplateSync::GetPlates();
                if (plates.empty() || !IsGameViewShown())
                {
                    return;
                }
                auto* camera = RE::Main::WorldRootCamera();
                if (!camera)
                {
                    return;
                }
                PROFILE_SCOPE("Nameplates");
                DrawPlates(*camera, plates, Config::Get()->ui.nameplateStandInMark);
            }

        private:
            static void DrawPlates(
                RE::NiCamera& a_camera, std::span<const NameplateSync::Plate> a_plates, bool a_standInMark)
            {
                const auto& display = ImGui::GetIO().DisplaySize;
                auto* font = Fonts::GetHeading();
                if (!font)
                {
                    font = ImGui::GetFont();
                }
                const auto baseSize = ImGui::GetFontSize();
                const auto outline = (std::max)(1.0F, std::floor(baseSize / 16.0F));
                const auto accent = Theme::GetAccent();
                // Under every window and the trade window's backdrop, which hides them.
                auto* drawList = ImGui::GetBackgroundDrawList();

                for (const auto& plate : a_plates)
                {
                    const auto& world = plate.head->world;
                    auto anchor = world.translate;
                    anchor.z += kHeadClearance * world.scale;
                    const auto point = Project(a_camera, anchor, display);
                    const auto nearness = 1.0F - std::clamp(plate.distanceFraction, 0.0F, 1.0F);
                    const auto alpha = plate.alpha * std::clamp(nearness / kDistanceFadeFraction, 0.0F, 1.0F);
                    if (!point || alpha <= 0.0F)
                    {
                        continue;
                    }

                    const auto size = std::round(baseSize * std::lerp(kFarFontScale, kNearFontScale, nearness));
                    const auto* const text = plate.label.data();
                    const auto* const textEnd = text + plate.label.size();
                    const auto textSize = font->CalcTextSizeA(size, FLT_MAX, 0.0F, text, textEnd);
                    const bool mark = a_standInMark && plate.standIn != StandIn::Level::kNone;
                    const auto pipWidth = (std::max)(1.0F, std::round(size * kPipWidth));
                    const auto markWidth = mark ? pipWidth * kStandInMarkWidths : 0.0F;
                    const auto width = markWidth + textSize.x;
                    const ImVec2 markOrigin{ std::round(point->x - width * 0.5F), std::round(point->y - textSize.y) };
                    const ImVec2 origin{ markOrigin.x + markWidth, markOrigin.y };
                    if (markOrigin.x > display.x || origin.y > display.y || markOrigin.x + width < 0.0F
                        || origin.y + textSize.y < 0.0F)
                    {
                        continue;
                    }

                    const auto shadow = ImGui::GetColorU32(ImVec4{ 0.0F, 0.0F, 0.0F, kOutlineAlpha * alpha });
                    for (const auto& offset : kOutlineOffsets)
                    {
                        drawList->AddText(font, size, ImVec2{ origin.x + offset.x * outline, origin.y + offset.y * outline },
                            shadow, text, textEnd);
                    }
                    const auto color = plate.isFriend ? ImVec4{ accent.x, accent.y, accent.z, alpha } : Theme::Tint(alpha);
                    drawList->AddText(font, size, origin, ImGui::GetColorU32(color), text, textEnd);
                    if (mark)
                    {
                        DrawStandInPips(*drawList, markOrigin, size, pipWidth, outline, plate.standIn, alpha);
                    }
                }
            }
        };
    }

    std::unique_ptr<Window> CreateNameplateLayer()
    {
        return std::make_unique<NameplateLayer>();
    }
}
