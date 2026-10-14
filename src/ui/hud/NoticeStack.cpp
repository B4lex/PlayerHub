#include "ui/hud/NoticeStack.h"
#include "network/ServerClient.h"
#include "runtime/GameSession.h"
#include "sync/filters/PlayerFilters.h"
#include "sync/notices/Notices.h"
#include "ui/Theme.h"
#include "ui/widgets/Controls.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace UI
{
    namespace
    {
        constexpr std::size_t kMaximumShown = 4;
        constexpr double kMinimumShowSeconds = 5.0;
        constexpr double kMaximumShowSeconds = 10.0;
        constexpr double kReadingDelaySeconds = 2.0;
        constexpr double kReadingCharactersPerSecond = 15.0;
        constexpr double kFadeSeconds = 1.0;

        constexpr float kTopFraction = 0.4F;
        constexpr float kRightMarginFonts = 0.75F;
        constexpr float kMaximumWidthFonts = 22.0F;
        constexpr float kGapFonts = 0.375F;
        constexpr float kStripFonts = 0.2F;
        constexpr float kBackgroundAlpha = 0.85F;

        struct Toast
        {
            std::string text;
            double postedAt{ 0.0 };
            double seconds{ 0.0 };
        };

        double GetShowSeconds(const std::string& a_text)
        {
            return std::clamp(kReadingDelaySeconds + static_cast<double>(a_text.size()) / kReadingCharactersPerSecond,
                kMinimumShowSeconds, kMaximumShowSeconds);
        }

        ImVec4 WithAlpha(ImVec4 a_color, float a_alpha)
        {
            a_color.w *= a_alpha;
            return a_color;
        }

        class NoticeStack final : public Window
        {
        public:
            void Draw(const WindowInput&) override
            {
                const auto now = ImGui::GetTime();
                Collect(now);
                std::erase_if(toasts, [now](const Toast& a_toast) { return now - a_toast.postedAt >= a_toast.seconds; });
                if (toasts.empty() || !GameSession::IsInGame())
                {
                    return;
                }
                DrawToasts(now);
            }

        private:
            void Collect(double a_now)
            {
                // Every welcome, a reconnect's too, starts a generation; one that already ended is not announced.
                const auto generation = Network::GetConnectionGeneration();
                if (generation != connectionGeneration)
                {
                    connectionGeneration = generation;
                    if (Network::IsConnected())
                    {
                        Add("Connected to the server", a_now);
                    }
                }

                PlayerFilters::CollectArrivedFriends(friends);
                for (const auto& player : friends)
                {
                    Add(std::format("{} is online", player.name), a_now);
                }
                friends.clear();

                Notices::Drain(incoming);
                for (auto& notice : incoming)
                {
                    Add(std::move(notice), a_now);
                }
                incoming.clear();
            }

            void Add(std::string a_text, double a_now)
            {
                if (toasts.size() >= kMaximumShown)
                {
                    toasts.erase(toasts.begin());
                }
                const auto seconds = GetShowSeconds(a_text);
                toasts.push_back(Toast{ .text = std::move(a_text), .postedAt = a_now, .seconds = seconds });
            }

            void DrawToasts(double a_now) const
            {
                const Theme::Scope theme;
                const auto& style = ImGui::GetStyle();
                const auto& display = ImGui::GetIO().DisplaySize;
                const auto fontSize = ImGui::GetFontSize();
                auto* font = ImGui::GetFont();
                const auto padding = style.WindowPadding;
                const auto strip = std::floor(kStripFonts * fontSize);
                const auto right = display.x - std::floor(kRightMarginFonts * fontSize);
                const auto wrapWidth = std::floor(kMaximumWidthFonts * fontSize) - padding.x * 2.0F - strip;
                const auto gap = std::floor(kGapFonts * fontSize);
                const auto rounding = style.WindowRounding;
                const auto& background = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
                const auto& textColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
                const auto accent = Theme::GetAccent();
                // Under every window, so a notice never covers one the player uses.
                auto* drawList = ImGui::GetBackgroundDrawList();

                auto top = std::floor(display.y * kTopFraction);
                for (const auto& toast : toasts)
                {
                    const auto alpha = static_cast<float>(
                        std::clamp((toast.postedAt + toast.seconds - a_now) / kFadeSeconds, 0.0, 1.0));
                    const auto* const text = toast.text.c_str();
                    const auto* const textEnd = text + toast.text.size();
                    const auto textSize = ImGui::CalcTextSize(text, textEnd, false, wrapWidth);
                    const ImVec2 min{ std::floor(right - (strip + padding.x * 2.0F + std::ceil(textSize.x))), top };
                    const ImVec2 max{ right, top + padding.y * 2.0F + textSize.y };

                    Controls::StripedBox(*drawList, min, max, strip, ImGui::GetColorU32(WithAlpha(accent, alpha)),
                        ImGui::GetColorU32(WithAlpha(background, kBackgroundAlpha * alpha)), rounding);
                    drawList->AddText(font, fontSize, ImVec2{ min.x + strip + padding.x, min.y + padding.y },
                        ImGui::GetColorU32(WithAlpha(textColor, alpha)), text, textEnd, wrapWidth);
                    top = max.y + gap;
                }
            }

            std::vector<Toast> toasts;
            std::uint64_t connectionGeneration{ 0 };
            std::vector<std::string> incoming;
            std::vector<Multiplayer::RosterEntry> friends;
        };
    }

    std::unique_ptr<Window> CreateNoticeStack()
    {
        return std::make_unique<NoticeStack>();
    }
}
