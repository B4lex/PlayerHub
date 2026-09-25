#include "ui/Overlay.h"
#include "runtime/PluginTime.h"
#include "ui/hud/StatusPanel.h"

#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>

#include <algorithm>
#include <cstdint>

namespace UI::Overlay
{
    namespace
    {
        // Sizes are authored for a 1080p back buffer and scaled to the real one.
        constexpr float kReferenceScreenHeight = 1080.0F;
        constexpr float kMinimumScale = 0.5F;
        constexpr float kBaseFontSize = 18.0F;

        bool initialized = false;
        bool initializationFailed = false;
        ImGuiStyle baseStyle;
        float appliedScale = 0.0F;

        // Returns false while the renderer is not ready yet; sets
        // initializationFailed if the ImGui backends reject the device.
        bool Initialize(RE::BSGraphics::RendererData& a_data)
        {
            const auto& window = a_data.renderWindows[0];
            if (!a_data.forwarder || !a_data.context || !window.hWnd)
            {
                return false;
            }

            IMGUI_CHECKVERSION();
            ImGui::CreateContext();

            auto& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            // Display only for now: Skyrim reads input through DirectInput, so an
            // interactive menu needs its own input routing before these are lifted.
            io.ConfigFlags
                |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoKeyboard;

            ImFontConfig fontConfig;
            fontConfig.SizePixels = kBaseFontSize;
            io.Fonts->AddFontDefaultVector(&fontConfig);

            ImGui::StyleColorsDark();
            auto& style = ImGui::GetStyle();
            style.FontSizeBase = kBaseFontSize;
            style.WindowRounding = 4.0F;
            style.WindowBorderSize = 0.0F;
            baseStyle = style;

            if (!ImGui_ImplWin32_Init(window.hWnd))
            {
                logs::error("UI overlay: ImGui Win32 backend initialization failed");
                ImGui::DestroyContext();
                initializationFailed = true;
                return false;
            }

            if (!ImGui_ImplDX11_Init(reinterpret_cast<::ID3D11Device*>(a_data.forwarder),
                    reinterpret_cast<::ID3D11DeviceContext*>(a_data.context)))
            {
                logs::error("UI overlay: ImGui DirectX 11 backend initialization failed");
                ImGui_ImplWin32_Shutdown();
                ImGui::DestroyContext();
                initializationFailed = true;
                return false;
            }

            initialized = true;
            logs::debug("UI overlay: initialized Dear ImGui {}", IMGUI_VERSION);
            return true;
        }

        void ApplyScale(float a_displayHeight)
        {
            const auto scale = (std::max)(a_displayHeight / kReferenceScreenHeight, kMinimumScale);
            if (scale == appliedScale)
            {
                return;
            }

            appliedScale = scale;
            auto& style = ImGui::GetStyle();
            style = baseStyle;
            style.ScaleAllSizes(scale);
            style.FontScaleMain = scale;
        }

        void DrawFrame()
        {
            if (initializationFailed)
            {
                return;
            }

            const auto renderer = RE::BSGraphics::Renderer::GetSingleton();
            if (!renderer)
            {
                return;
            }

            auto& data = renderer->GetRuntimeData();
            const auto& window = data.renderWindows[0];
            if (!window.swapChain || !window.renderView || (!initialized && !Initialize(data)))
            {
                return;
            }

            REX::W32::DXGI_SWAP_CHAIN_DESC swapChainDesc{};
            if (window.swapChain->GetDesc(&swapChainDesc) < 0 || swapChainDesc.bufferDesc.width == 0
                || swapChainDesc.bufferDesc.height == 0)
            {
                return;
            }

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            // The Win32 backend uses the client rectangle, which can differ from
            // the back buffer when Skyrim renders at another resolution.
            auto& io = ImGui::GetIO();
            io.DisplaySize = ImVec2{ static_cast<float>(swapChainDesc.bufferDesc.width),
                static_cast<float>(swapChainDesc.bufferDesc.height) };
            ApplyScale(io.DisplaySize.y);

            ImGui::NewFrame();
            StatusPanel::Draw();
            ImGui::Render();

            // The DX11 backend restores the pipeline state it changes, except the
            // output merger, so target the back buffer and restore it here.
            REX::W32::ID3D11RenderTargetView* previousTarget = nullptr;
            REX::W32::ID3D11DepthStencilView* previousDepthStencil = nullptr;
            data.context->OMGetRenderTargets(1, &previousTarget, &previousDepthStencil);

            REX::W32::ID3D11RenderTargetView* const backBuffer = window.renderView;
            data.context->OMSetRenderTargets(1, &backBuffer, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            data.context->OMSetRenderTargets(1, &previousTarget, previousDepthStencil);

            if (previousTarget)
            {
                previousTarget->Release();
            }
            if (previousDepthStencil)
            {
                previousDepthStencil->Release();
            }
        }

        struct RendererEndHook
        {
            // Replaces the timer call at the start of BSGraphics::Renderer::End.
            // The same function presents the swap chain afterwards (checked
            // against 1.7.104), so everything drawn here lands on top of the
            // finished frame.
            static void Thunk(std::uint32_t a_timer)
            {
                _original(a_timer);
                const PluginTime::Scope timing;
                DrawFrame();
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };
    }

    void Install()
    {
        if (REL::Module::IsVR())
        {
            logs::warn("UI overlay is not supported in Skyrim VR; renderer hook not installed");
            return;
        }

        REL::Relocation<std::uintptr_t> rendererEndTimerCall{ RELOCATION_ID(75461, 77246), 0x9 };
        RendererEndHook::_original
            = SKSE::GetTrampoline().write_call<5>(rendererEndTimerCall.address(), RendererEndHook::Thunk);

        logs::debug("Installed UI overlay renderer hook");
    }
}
