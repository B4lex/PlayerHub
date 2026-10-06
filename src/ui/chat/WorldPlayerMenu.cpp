#include "ui/chat/WorldPlayerMenu.h"
#include "sync/chat/PlayerActivation.h"
#include "ui/Theme.h"
#include "ui/WindowManager.h"
#include "ui/chat/ChatPlayerMenu.h"

#include <imgui.h>

#include <utility>

namespace UI
{
    namespace
    {
        // Dear ImGui begins a popup inside a window; this one shows nothing.
        constexpr ImGuiWindowFlags kHostFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav
            | ImGuiWindowFlags_NoFocusOnAppearing;

        class WorldPlayerMenu final : public Window
        {
        public:
            [[nodiscard]] bool IsReleaseKey(std::uint32_t a_scanCode) const override
            {
                if (a_scanCode == RE::BSKeyboardDevice::Key::kEscape)
                {
                    return true;
                }
                const auto* controlMap = RE::ControlMap::GetSingleton();
                const auto* userEvents = RE::UserEvents::GetSingleton();
                return controlMap && userEvents
                    && controlMap->GetMappedKey(userEvents->activate, RE::INPUT_DEVICE::kKeyboard) == a_scanCode;
            }

            [[nodiscard]] bool UsesMouse() const override { return true; }

            void Draw(const WindowInput& a_input) override
            {
                if (!a_input.focused && !shown)
                {
                    if (auto activated = PlayerActivation::TakeActivated(); activated && WindowManager::Focus(*this))
                    {
                        player = std::move(*activated);
                    }
                    return;
                }

                const Theme::Scope theme;
                const auto& display = ImGui::GetIO().DisplaySize;
                const ImVec2 crosshair{ display.x * 0.55F, display.y * 0.55F };
                ImGui::SetNextWindowPos(crosshair);
                ImGui::SetNextWindowSize(ImVec2{ 1.0F, 1.0F });
                ImGui::Begin("##WorldPlayerMenuHost", nullptr, kHostFlags);
                if (!shown)
                {
                    ImGui::OpenPopup("##WorldPlayerMenu");
                    shown = true;
                }
                ImGui::SetNextWindowPos(crosshair, ImGuiCond_Appearing);
                bool whisper = false;
                if (ImGui::BeginPopup("##WorldPlayerMenu"))
                {
                    whisper = ImGui::MenuItem("Whisper");
                    ChatPlayerMenu::DrawFilterItems(player);
                    if (!a_input.focused)
                    {
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
                else
                {
                    shown = false;
                    WindowManager::ReleaseFocus(*this);
                }
                ImGui::End();

                if (whisper)
                {
                    WindowManager::ReleaseFocus(*this);
                    PlayerActivation::RequestWhisper(player);
                }
            }

        private:
            Multiplayer::RosterEntry player;
            // The menu was opened and Dear ImGui has not closed it yet.
            bool shown{ false };
        };
    }

    std::unique_ptr<Window> CreateWorldPlayerMenu()
    {
        return std::make_unique<WorldPlayerMenu>();
    }
}
