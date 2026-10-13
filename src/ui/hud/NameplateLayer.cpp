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
        constexpr float kHeadClearance = 30.0F;
        constexpr float kNearFontScale = 1.2F;
        constexpr float kFarFontScale = 0.8F;
        constexpr float kDistanceFadeFraction = 0.15F;
        constexpr float kOutlineAlpha = 0.8F;
        constexpr float kMinimumDepth = 1e-5F;
        constexpr std::array<ImVec2, 4> kOutlineOffsets{ { { -1.0F, -1.0F }, { 1.0F, -1.0F }, { -1.0F, 1.0F },
            { 1.0F, 1.0F } } };

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
                DrawPlates(*camera, plates);
            }

        private:
            static void DrawPlates(RE::NiCamera& a_camera, std::span<const NameplateSync::Plate> a_plates)
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
                    const ImVec2 origin{ std::round(point->x - textSize.x * 0.5F), std::round(point->y - textSize.y) };
                    if (origin.x > display.x || origin.y > display.y || origin.x + textSize.x < 0.0F
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
                }
            }
        };
    }

    std::unique_ptr<Window> CreateNameplateLayer()
    {
        return std::make_unique<NameplateLayer>();
    }
}
