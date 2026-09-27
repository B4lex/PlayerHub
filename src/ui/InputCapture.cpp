#include "ui/InputCapture.h"
#include "runtime/PluginTime.h"
#include "runtime/Profiling.h"
#include "ui/WindowManager.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace UI::InputCapture
{
    namespace
    {
        using KeyboardKey = RE::BSKeyboardDevice::Key;
        using MouseKey = RE::BSWin32MouseDevice::Key;

        // Every event of a press goes where its press went, so a key held across
        // a focus change is still released where it was pressed.
        enum class Owner : std::uint8_t
        {
            kNone,
            kGame,
            kWindow
        };

        // Relative mouse counts are scaled as pixels at this back-buffer height, so
        // the cursor crosses the screen at the same speed at any resolution.
        constexpr float kCursorReferenceScreenHeight = 1080.0F;

        struct CapturedEvent
        {
            // ImGuiKey_None for a typed character or a mouse button.
            ImGuiKey key{ ImGuiKey_None };
            bool down{ false };
            std::uint32_t character{ 0 };
            // An ImGuiMouseButton, or -1 for a key or character.
            int mouseButton{ -1 };
        };

        struct DispatchState
        {
            bool gameplay{ false };
            bool focused{ false };
        };

        // Input dispatch runs on the main-loop thread.
        std::array<Owner, 256> s_keyboardOwners{};
        std::array<Owner, MouseKey::kWheelDown + 1> s_mouseOwners{};
        std::array<bool, 256> s_keyboardPressed{};

        // Focus keys stay inert until the overlay draws, so input is never
        // captured for windows that cannot show it.
        std::atomic_bool s_overlayActive{ false };
        std::mutex s_capturedLock;
        std::vector<CapturedEvent> s_captured;
        float s_capturedWheelSteps{ 0.0F };
        ImVec2 s_capturedMouseMotion{};

        // Overlay thread only.
        std::vector<CapturedEvent> s_forwarding;
        bool s_imguiFocused{ false };
        bool s_imguiMouse{ false };
        ImVec2 s_cursor{};

        // DirectInput scan codes are physical positions, so letters only serve
        // text-editing shortcuts; typed text arrives as character events. Tab is
        // left out so it cannot move keyboard focus off a text field.
        ImGuiKey ToImGuiKey(std::uint32_t a_scanCode)
        {
            switch (a_scanCode)
            {
                case KeyboardKey::kLeft:
                    return ImGuiKey_LeftArrow;
                case KeyboardKey::kRight:
                    return ImGuiKey_RightArrow;
                case KeyboardKey::kUp:
                    return ImGuiKey_UpArrow;
                case KeyboardKey::kDown:
                    return ImGuiKey_DownArrow;
                case KeyboardKey::kPageUp:
                    return ImGuiKey_PageUp;
                case KeyboardKey::kPageDown:
                    return ImGuiKey_PageDown;
                case KeyboardKey::kHome:
                    return ImGuiKey_Home;
                case KeyboardKey::kEnd:
                    return ImGuiKey_End;
                case KeyboardKey::kInsert:
                    return ImGuiKey_Insert;
                case KeyboardKey::kDelete:
                    return ImGuiKey_Delete;
                case KeyboardKey::kBackspace:
                    return ImGuiKey_Backspace;
                case KeyboardKey::kEnter:
                    return ImGuiKey_Enter;
                case KeyboardKey::kKP_Enter:
                    return ImGuiKey_KeypadEnter;
                case KeyboardKey::kEscape:
                    return ImGuiKey_Escape;
                case KeyboardKey::kLeftControl:
                    return ImGuiKey_LeftCtrl;
                case KeyboardKey::kRightControl:
                    return ImGuiKey_RightCtrl;
                case KeyboardKey::kLeftShift:
                    return ImGuiKey_LeftShift;
                case KeyboardKey::kRightShift:
                    return ImGuiKey_RightShift;
                case KeyboardKey::kLeftAlt:
                    return ImGuiKey_LeftAlt;
                case KeyboardKey::kRightAlt:
                    return ImGuiKey_RightAlt;
                case KeyboardKey::kA:
                    return ImGuiKey_A;
                case KeyboardKey::kC:
                    return ImGuiKey_C;
                case KeyboardKey::kV:
                    return ImGuiKey_V;
                case KeyboardKey::kX:
                    return ImGuiKey_X;
                case KeyboardKey::kY:
                    return ImGuiKey_Y;
                case KeyboardKey::kZ:
                    return ImGuiKey_Z;
                default:
                    return ImGuiKey_None;
            }
        }

        // Dear ImGui tracks each modifier as one key, whichever side is held.
        std::pair<ImGuiKey, bool> GetModifier(std::uint32_t a_scanCode)
        {
            const auto either = [](KeyboardKey a_left, KeyboardKey a_right) {
                return s_keyboardPressed[a_left] || s_keyboardPressed[a_right];
            };
            switch (a_scanCode)
            {
                case KeyboardKey::kLeftControl:
                case KeyboardKey::kRightControl:
                    return { ImGuiMod_Ctrl, either(KeyboardKey::kLeftControl, KeyboardKey::kRightControl) };
                case KeyboardKey::kLeftShift:
                case KeyboardKey::kRightShift:
                    return { ImGuiMod_Shift, either(KeyboardKey::kLeftShift, KeyboardKey::kRightShift) };
                case KeyboardKey::kLeftAlt:
                case KeyboardKey::kRightAlt:
                    return { ImGuiMod_Alt, either(KeyboardKey::kLeftAlt, KeyboardKey::kRightAlt) };
                default:
                    return { ImGuiKey_None, false };
            }
        }

        void Capture(const CapturedEvent& a_event)
        {
            const std::scoped_lock lock{ s_capturedLock };
            s_captured.push_back(a_event);
        }

        void ForwardButton(RE::INPUT_DEVICE a_device, std::uint32_t a_code, const RE::ButtonEvent& a_button)
        {
            if (a_device == RE::INPUT_DEVICE::kKeyboard)
            {
                // Held events are left out: Dear ImGui repeats held keys itself.
                if (!a_button.IsDown() && a_button.IsPressed())
                {
                    return;
                }
                if (const auto key = ToImGuiKey(a_code); key != ImGuiKey_None)
                {
                    Capture(CapturedEvent{ .key = key, .down = a_button.IsPressed() });
                }
                if (const auto [modifier, down] = GetModifier(a_code); modifier != ImGuiKey_None)
                {
                    Capture(CapturedEvent{ .key = modifier, .down = down });
                }
                return;
            }

            if (a_code <= MouseKey::kMiddleButton)
            {
                if (a_button.IsDown() || !a_button.IsPressed())
                {
                    Capture(CapturedEvent{ .down = a_button.IsPressed(), .mouseButton = static_cast<int>(a_code) });
                }
                return;
            }
            if (a_button.IsDown() && (a_code == MouseKey::kWheelUp || a_code == MouseKey::kWheelDown))
            {
                const std::scoped_lock lock{ s_capturedLock };
                s_capturedWheelSteps += a_code == MouseKey::kWheelUp ? 1.0F : -1.0F;
            }
        }

        // Returns whether the game receives the event.
        bool RouteButton(const RE::ButtonEvent& a_button, DispatchState& a_state)
        {
            const auto device = a_button.GetDevice();
            const auto code = a_button.GetIDCode();
            Owner* owner = nullptr;
            if (device == RE::INPUT_DEVICE::kKeyboard && code < s_keyboardOwners.size())
            {
                owner = &s_keyboardOwners[code];
                s_keyboardPressed[code] = a_button.IsPressed();
            }
            else if (device == RE::INPUT_DEVICE::kMouse && code < s_mouseOwners.size())
            {
                owner = &s_mouseOwners[code];
            }
            else
            {
                // Gamepads keep driving the game; windows are keyboard-only.
                return !a_state.focused
                    || (device != RE::INPUT_DEVICE::kKeyboard && device != RE::INPUT_DEVICE::kMouse);
            }

            if (a_button.IsDown())
            {
                if (!a_state.focused && a_state.gameplay && device == RE::INPUT_DEVICE::kKeyboard
                    && s_overlayActive.load(std::memory_order_acquire) && WindowManager::FocusByKey(code))
                {
                    logs::debug("UI input: a window took focus with scan code {:#04x}", code);
                    a_state.focused = true;
                    *owner = Owner::kWindow;
                    return false;
                }
                *owner = a_state.focused ? Owner::kWindow : Owner::kGame;
            }

            const bool toWindow = *owner == Owner::kWindow || (*owner == Owner::kNone && a_state.focused);
            if (toWindow && a_state.focused)
            {
                ForwardButton(device, code, a_button);
            }
            if (!a_button.IsPressed())
            {
                *owner = Owner::kNone;
            }
            return !toWindow;
        }

        bool RouteEvent(RE::InputEvent& a_event, DispatchState& a_state)
        {
            switch (a_event.GetEventType())
            {
                case RE::INPUT_EVENT_TYPE::kButton:
                    return RouteButton(*a_event.AsButtonEvent(), a_state);
                case RE::INPUT_EVENT_TYPE::kChar:
                    if (a_state.focused)
                    {
                        Capture(CapturedEvent{ .character = a_event.AsCharEvent()->keyCode });
                    }
                    return !a_state.focused;
                case RE::INPUT_EVENT_TYPE::kMouseMove:
                    if (a_state.focused)
                    {
                        const auto& motion = *a_event.AsMouseMoveEvent();
                        const std::scoped_lock lock{ s_capturedLock };
                        s_capturedMouseMotion.x += static_cast<float>(motion.mouseInputX);
                        s_capturedMouseMotion.y += static_cast<float>(motion.mouseInputY);
                    }
                    return !a_state.focused;
                default:
                    return true;
            }
        }

        // No menu, console, or other input context above gameplay.
        bool IsGameplayInputActive()
        {
            auto* ui = RE::UI::GetSingleton();
            auto* controlMap = RE::ControlMap::GetSingleton();
            if (!ui || !controlMap || ui->GameIsPaused())
            {
                return false;
            }
            // PushInputContext appends, so the active context is the last one
            // (checked against 1.7.104).
            const auto& contexts = controlMap->GetRuntimeData().contextPriorityStack;
            return !contexts.empty() && contexts.back() == RE::UserEvents::INPUT_CONTEXT_ID::kGameplay;
        }

        // Returns the events the game receives, relinked in their original order.
        RE::InputEvent* Route(RE::InputEvent* a_events)
        {
            DispatchState state{ .gameplay = IsGameplayInputActive(), .focused = WindowManager::HasFocus() };
            if (state.focused && !state.gameplay)
            {
                WindowManager::ReleaseFocus();
                state.focused = false;
                logs::debug("UI input: released window focus because a menu took over input");
            }

            RE::InputEvent* kept = nullptr;
            RE::InputEvent** tail = &kept;
            for (auto* event = a_events; event;)
            {
                auto* next = event->next;
                if (RouteEvent(*event, state))
                {
                    *tail = event;
                    tail = &event->next;
                }
                event = next;
            }
            *tail = nullptr;
            return kept;
        }

        // Dear ImGui sees the mouse only while a window that uses it has focus. The
        // cursor starts in the middle of the screen each time.
        void SetImGuiMouse(ImGuiIO& a_io, bool a_enabled)
        {
            if (a_enabled == s_imguiMouse)
            {
                return;
            }
            s_imguiMouse = a_enabled;
            a_io.MouseDrawCursor = a_enabled;
            if (a_enabled)
            {
                a_io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
                s_cursor = ImVec2{ a_io.DisplaySize.x * 0.5F, a_io.DisplaySize.y * 0.5F };
                return;
            }
            for (int button = 0; button < ImGuiMouseButton_COUNT; ++button)
            {
                a_io.AddMouseButtonEvent(button, false);
            }
            a_io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            a_io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
        }

        void MoveCursor(ImGuiIO& a_io, ImVec2 a_motion)
        {
            const auto scale = a_io.DisplaySize.y / kCursorReferenceScreenHeight;
            s_cursor.x = std::clamp(s_cursor.x + a_motion.x * scale, 0.0F, a_io.DisplaySize.x);
            s_cursor.y = std::clamp(s_cursor.y + a_motion.y * scale, 0.0F, a_io.DisplaySize.y);
            // Queued after the Win32 backend's own position, which follows the
            // hidden system cursor, so this one wins.
            a_io.AddMousePosEvent(s_cursor.x, s_cursor.y);
        }

        struct InputDispatchHook
        {
            // Replaces the call in BSInputDeviceManager::PollInputDevices that sends
            // the frame's events to the input sinks, menus and player controls among
            // them. The engine clears the event queue right after, and pushing an
            // event onto it resets its link (both checked against 1.7.104), so the
            // list can be relinked for this dispatch only.
            static void Thunk(RE::BSTEventSource<RE::InputEvent*>* a_source, RE::InputEvent* const* a_events)
            {
                if (!a_events || (!*a_events && !WindowManager::HasFocus()))
                {
                    _original(a_source, a_events);
                    return;
                }

                RE::InputEvent* events = nullptr;
                {
                    const PluginTime::Scope timing;
                    PROFILE_SCOPE("InputCapture");
                    events = Route(*a_events);
                }
                _original(a_source, &events);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };
    }

    void Install()
    {
        if (REL::Module::IsVR())
        {
            logs::warn("UI input capture is not supported in Skyrim VR; input dispatch hook not installed");
            return;
        }

        InputDispatchHook::_original = CodeDetour::WriteCallDetour(REL::ID(RELOCATION_ID(67315, 68617)), 0x7B,
            reinterpret_cast<std::uintptr_t>(&InputDispatchHook::Thunk), "input dispatch");
        logs::debug("UI input capture hook installed={}", InputDispatchHook::_original.address() != 0);
    }

    float ForwardToImGui()
    {
        s_overlayActive.store(true, std::memory_order_release);

        float wheelSteps = 0.0F;
        ImVec2 mouseMotion{};
        {
            const std::scoped_lock lock{ s_capturedLock };
            s_forwarding.swap(s_captured);
            wheelSteps = std::exchange(s_capturedWheelSteps, 0.0F);
            mouseMotion = std::exchange(s_capturedMouseMotion, ImVec2{});
        }

        auto& io = ImGui::GetIO();
        const bool focused = WindowManager::HasFocus();
        const bool mouse = focused && WindowManager::FocusedWindowUsesMouse();
        if (focused != s_imguiFocused)
        {
            // Losing focus also releases every key Dear ImGui still holds.
            io.AddFocusEvent(focused);
            s_imguiFocused = focused;
        }
        SetImGuiMouse(io, mouse);
        if (mouse)
        {
            MoveCursor(io, mouseMotion);
            if (wheelSteps != 0.0F)
            {
                io.AddMouseWheelEvent(0.0F, wheelSteps);
            }
            wheelSteps = 0.0F;
        }
        if (focused)
        {
            for (const auto& event : s_forwarding)
            {
                if (event.mouseButton >= 0)
                {
                    if (mouse)
                    {
                        io.AddMouseButtonEvent(event.mouseButton, event.down);
                    }
                }
                else if (event.key != ImGuiKey_None)
                {
                    io.AddKeyEvent(event.key, event.down);
                }
                else if (event.character <= 0xFFFF)
                {
                    io.AddInputCharacterUTF16(static_cast<ImWchar16>(event.character));
                }
                else
                {
                    io.AddInputCharacter(event.character);
                }
            }
        }
        s_forwarding.clear();
        return focused ? wheelSteps : 0.0F;
    }
}
