#include "ui/preview/ItemPreview.h"
#include "config/PluginConfig.h"
#include "runtime/GameThread.h"
#include "runtime/Profiling.h"

#include <REX/W32/COMPTR.h>
#include <REX/W32/D3D11_1.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <string_view>
#include <utility>

// The capture follows Item3DPreview in ModExplorerMenu by patchulidev (GPL-3.0).
namespace UI::ItemPreview
{
    namespace
    {
        using REX::W32::ComPtr;

        // Captures go to the top-left corner of textures this large.
        constexpr std::uint32_t kTextureSize = 2048;
        // Room around the model's bounding sphere, which fits the model closely.
        constexpr float kCaptureMargin = 1.05F;
        constexpr double kLoadingNoteDelaySeconds = 0.3;
        constexpr std::chrono::seconds kLoadWaitLogDelay{ 1 };
        // Also cleared behind the model, so the capture blends into the frame.
        constexpr ImVec4 kBackground{ 0.11F, 0.11F, 0.12F, 1.0F };

        struct CaptureBox
        {
            std::uint32_t left;
            std::uint32_t top;
            std::uint32_t width;
            std::uint32_t height;
        };

        struct CaptureTextures
        {
            ComPtr<REX::W32::ID3D11Texture2D> image;
            ComPtr<REX::W32::ID3D11ShaderResourceView> imageView;
            // The frame's pixels under the capture, put back after it.
            ComPtr<REX::W32::ID3D11Texture2D> saved;
            // The frame's alpha under the model is not coverage, so the opaque
            // capture is drawn without blending.
            ComPtr<REX::W32::ID3D11BlendState> opaque;
            REX::W32::DXGI_FORMAT format{};
        };

        // Set by Draw for the next Update.
        bool requested{ false };
        std::uint32_t requestedFormID{ 0 };
        // Draw's own record of when it started showing an item.
        std::uint32_t drawnFormID{ 0 };
        double drawnSince{ 0.0 };

        // Inventory3DManager::Begin3D was called and End3D is due.
        bool running{ false };
        // Set once for the session when a requirement fails.
        bool unavailable{ false };
        // The first capture of each start is logged.
        bool captureLogged{ false };
        std::uint32_t loadedFormID{ 0 };
        std::chrono::steady_clock::time_point loadedSince;
        bool loadWaitLogged{ false };
        std::uint32_t capturedFormID{ 0 };
        ImVec2 capturedSize{};
        CaptureTextures textures;

        void Disable(std::string_view a_reason)
        {
            if (!unavailable)
            {
                logs::warn("Item preview unavailable: {}", a_reason);
                unavailable = true;
            }
        }

        void Release()
        {
            if (running)
            {
                // An item menu that opened meanwhile has taken over the inventory
                // renderer and ends it itself when it closes.
                auto* ui = RE::UI::GetSingleton();
                auto* manager = RE::Inventory3DManager::GetSingleton();
                if (ui && manager && !ui->IsItemMenuOpen())
                {
                    manager->UnloadInventoryItem();
                    manager->End3D();
                }
                running = false;
                loadedFormID = 0;
                capturedFormID = 0;
                logs::debug("Item preview stopped");
            }
            if (textures.image.Get() || textures.saved.Get() || textures.opaque.Get())
            {
                textures = {};
            }
        }

        // The model's bounding sphere on screen, clamped to the screen and the
        // textures. The inventory camera's frustum at the model's depth maps the
        // scene to the screen.
        std::optional<CaptureBox> ProjectModel(const RE::UI3DSceneManager& a_scene, const RE::NiAVObject& a_model,
            std::uint32_t a_screenWidth, std::uint32_t a_screenHeight)
        {
            const auto& frustum = a_scene.viewFrustum;
            const float depth = a_model.local.translate.y;
            const float worldLeft = -frustum.fLeft * depth;
            const float worldBottom = -frustum.fBottom * depth;
            const float unitsPerPixelX = (-frustum.fRight * depth - worldLeft) / static_cast<float>(a_screenWidth);
            const float unitsPerPixelY = (-frustum.fTop * depth - worldBottom) / static_cast<float>(a_screenHeight);
            if (!std::isfinite(unitsPerPixelX) || !std::isfinite(unitsPerPixelY) || unitsPerPixelX == 0.0F
                || unitsPerPixelY == 0.0F)
            {
                return std::nullopt;
            }

            const auto& bound = a_model.worldBound;
            const float centerX = -(bound.center.x + worldLeft) / unitsPerPixelX;
            const float centerY = -(bound.center.z + worldBottom) / unitsPerPixelY;
            const float halfWidth = bound.radius * kCaptureMargin / std::abs(unitsPerPixelX);
            const float halfHeight = bound.radius * kCaptureMargin / std::abs(unitsPerPixelY);
            if (!std::isfinite(centerX) || !std::isfinite(centerY) || !std::isfinite(halfWidth)
                || !std::isfinite(halfHeight))
            {
                return std::nullopt;
            }

            const auto screenWidth = static_cast<float>(a_screenWidth);
            const auto screenHeight = static_cast<float>(a_screenHeight);
            const float left = std::clamp(centerX - halfWidth, 0.0F, screenWidth);
            const float top = std::clamp(centerY - halfHeight, 0.0F, screenHeight);
            const float right = std::clamp(centerX + halfWidth, left, (std::min)(screenWidth, left + kTextureSize));
            const float bottom = std::clamp(centerY + halfHeight, top, (std::min)(screenHeight, top + kTextureSize));
            const CaptureBox box{ .left = static_cast<std::uint32_t>(left),
                .top = static_cast<std::uint32_t>(top),
                .width = static_cast<std::uint32_t>(right - left),
                .height = static_cast<std::uint32_t>(bottom - top) };
            if (box.width == 0 || box.height == 0)
            {
                return std::nullopt;
            }
            return box;
        }

        bool EnsureTextures(REX::W32::ID3D11Device& a_device, REX::W32::DXGI_FORMAT a_format)
        {
            if (textures.image.Get() && textures.format == a_format)
            {
                return true;
            }
            textures = {};

            REX::W32::D3D11_TEXTURE2D_DESC description{};
            description.width = kTextureSize;
            description.height = kTextureSize;
            description.mipLevels = 1;
            description.arraySize = 1;
            description.format = a_format;
            description.sampleDesc.count = 1;
            description.usage = REX::W32::D3D11_USAGE_DEFAULT;
            description.bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE;
            const bool created = a_device.CreateTexture2D(&description, nullptr, textures.image.GetAddressOf()) >= 0
                && a_device.CreateShaderResourceView(textures.image.Get(), nullptr, textures.imageView.GetAddressOf())
                    >= 0;
            description.bindFlags = 0;
            REX::W32::D3D11_BLEND_DESC blend{};
            blend.renderTarget[0] = REX::W32::D3D11_RENDER_TARGET_BLEND_DESC{ .blendEnable = 0,
                .srcBlend = REX::W32::D3D11_BLEND_ONE,
                .destBlend = REX::W32::D3D11_BLEND_ZERO,
                .blendOp = REX::W32::D3D11_BLEND_OP_ADD,
                .srcBlendAlpha = REX::W32::D3D11_BLEND_ONE,
                .destBlendAlpha = REX::W32::D3D11_BLEND_ZERO,
                .blendOpAlpha = REX::W32::D3D11_BLEND_OP_ADD,
                .renderTargetWriteMask = REX::W32::D3D11_COLOR_WRITE_ENABLE_ALL };
            if (!created || a_device.CreateTexture2D(&description, nullptr, textures.saved.GetAddressOf()) < 0
                || a_device.CreateBlendState(&blend, textures.opaque.GetAddressOf()) < 0)
            {
                textures = {};
                Disable(std::format("capture textures could not be created: format={}", static_cast<int>(a_format)));
                return false;
            }
            textures.format = a_format;
            return true;
        }

        void ClearBackground(REX::W32::ID3D11DeviceContext& a_context, REX::W32::ID3D11RenderTargetView& a_target,
            const CaptureBox& a_box)
        {
            ComPtr<REX::W32::ID3D11DeviceContext1> context;
            if (a_context.QueryInterface(
                    REX::W32::IID_ID3D11DeviceContext1, reinterpret_cast<void**>(context.GetAddressOf()))
                    < 0
                || !context.Get())
            {
                return;
            }
            const float color[4]{ kBackground.x, kBackground.y, kBackground.z, kBackground.w };
            const REX::W32::D3D11_RECT rect{ static_cast<std::int32_t>(a_box.left),
                static_cast<std::int32_t>(a_box.top), static_cast<std::int32_t>(a_box.left + a_box.width),
                static_cast<std::int32_t>(a_box.top + a_box.height) };
            context->ClearView(&a_target, color, &rect, 1);
        }

        // Inventory3DManager::Render attaches a finished load, then calls the scene
        // render (1.7.104 ID 52727), which draws only while UI::numDisablePauseMenu
        // is positive, as it is while an inventory menu is open. The overlay is not a
        // menu, so it sets the count for the call and restores it. Main-loop thread,
        // like the menus that change it.
        class PauseMenuDisabledCountScope
        {
        public:
            PauseMenuDisabledCountScope(RE::UI& a_ui, std::uint32_t a_count)
                : ui(a_ui)
                , previous(a_ui.numDisablePauseMenu)
            {
                ui.numDisablePauseMenu = a_count;
            }
            ~PauseMenuDisabledCountScope() { ui.numDisablePauseMenu = previous; }
            PauseMenuDisabledCountScope(const PauseMenuDisabledCountScope&) = delete;
            PauseMenuDisabledCountScope& operator=(const PauseMenuDisabledCountScope&) = delete;

        private:
            RE::UI& ui;
            std::uint32_t previous;
        };

        // Diagnostic for a model that has not appeared a second after loading
        // started: the models Inventory3DManager holds, the state of its load task
        // (Render attaches the model at state 5), and both light schemes, which the
        // scene render requires to match.
        void LogLoadWait(RE::Inventory3DManager& a_manager)
        {
            const auto& runtime = a_manager.GetRuntimeData();
            const auto* task = reinterpret_cast<const std::uint8_t*>(runtime.loadTask.get());
            const auto* scene = RE::UI3DSceneManager::GetSingleton();
            const auto* model = runtime.loadedModels.empty() ? nullptr : runtime.loadedModels.back().spModel.get();
            logs::debug(
                "Item preview waiting: formID={:08X} models={} modelRadius={} loadTaskState={} "
                "managerScheme={} sceneScheme={}",
                loadedFormID, runtime.loadedModels.size(), model ? model->worldBound.radius : 0.0F,
                task ? static_cast<std::int64_t>(*reinterpret_cast<const std::uint32_t*>(task + 0xC)) : -1,
                static_cast<int>(a_manager.currentLightScheme),
                scene ? static_cast<int>(scene->currentlightScheme) : -1);
        }

        // Renders the loaded model onto the finished frame, copies it into the
        // image texture, and restores the frame. False until the model is loaded.
        bool CaptureModel(RE::Inventory3DManager& a_manager, RE::UI& a_ui)
        {
            // A model that loaded in the background is attached only by Render.
            // With the count at zero, the scene render draws nothing.
            if (a_manager.GetRuntimeData().loadedModels.empty())
            {
                const PauseMenuDisabledCountScope attachOnly{ a_ui, 0 };
                a_manager.Render();
            }

            const auto& models = a_manager.GetRuntimeData().loadedModels;
            auto* scene = RE::UI3DSceneManager::GetSingleton();
            auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
            if (models.empty() || !scene || !renderer)
            {
                return false;
            }
            const auto* model = models.back().spModel.get();
            if (!model || !(model->worldBound.radius > 0.0F))
            {
                return false;
            }

            auto& data = renderer->GetRuntimeData();
            auto* target = data.renderWindows[0].renderView;
            if (!data.forwarder || !data.context || !target)
            {
                return false;
            }
            ComPtr<REX::W32::ID3D11Resource> resource;
            target->GetResource(resource.GetAddressOf());
            ComPtr<REX::W32::ID3D11Texture2D> backBuffer;
            if (!resource.Get()
                || resource->QueryInterface(
                       REX::W32::IID_ID3D11Texture2D, reinterpret_cast<void**>(backBuffer.GetAddressOf()))
                    < 0
                || !backBuffer.Get())
            {
                return false;
            }
            REX::W32::D3D11_TEXTURE2D_DESC description{};
            backBuffer->GetDesc(&description);
            const auto box = ProjectModel(*scene, *model, description.width, description.height);
            if (!box || !EnsureTextures(*data.forwarder, description.format))
            {
                return false;
            }

            const REX::W32::D3D11_BOX screenBox{ .left = box->left,
                .top = box->top,
                .front = 0,
                .right = box->left + box->width,
                .bottom = box->top + box->height,
                .back = 1 };
            const REX::W32::D3D11_BOX textureBox{
                .left = 0, .top = 0, .front = 0, .right = box->width, .bottom = box->height, .back = 1
            };
            auto& context = *data.context;
            context.CopySubresourceRegion(textures.saved.Get(), 0, 0, 0, 0, backBuffer.Get(), 0, &screenBox);
            ClearBackground(context, *target, *box);
            {
                const PauseMenuDisabledCountScope draw{ a_ui, (std::max)(a_ui.numDisablePauseMenu, 1U) };
                a_manager.Render();
            }
            context.CopySubresourceRegion(textures.image.Get(), 0, 0, 0, 0, backBuffer.Get(), 0, &screenBox);
            context.CopySubresourceRegion(
                backBuffer.Get(), 0, box->left, box->top, 0, textures.saved.Get(), 0, &textureBox);

            capturedSize = ImVec2{ static_cast<float>(box->width), static_cast<float>(box->height) };
            if (!captureLogged)
            {
                captureLogged = true;
                logs::debug("Item preview captured: formID={:08X} box=({}, {}, {}x{}) screen={}x{} radius={}",
                    loadedFormID, box->left, box->top, box->width, box->height, description.width, description.height,
                    model->worldBound.radius);
            }
            return true;
        }
    }

    namespace
    {
        // Runs inside ImGui_ImplDX11_RenderDrawData, on the device context the
        // overlay shares with the renderer.
        void SetOpaqueBlend(const ImDrawList*, const ImDrawCmd*)
        {
            auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
            auto* context = renderer ? renderer->GetRuntimeData().context : nullptr;
            if (context && textures.opaque.Get())
            {
                context->OMSetBlendState(textures.opaque.Get(), nullptr, 0xFFFFFFFF);
            }
        }
    }

    void Draw(std::uint32_t a_formID, ImVec2 a_size)
    {
        requested = true;
        requestedFormID = a_formID;
        const auto now = ImGui::GetTime();
        if (a_formID != drawnFormID)
        {
            drawnFormID = a_formID;
            drawnSince = now;
        }

        const auto position = ImGui::GetCursorScreenPos();
        const ImVec2 end{ position.x + a_size.x, position.y + a_size.y };
        ImGui::Dummy(a_size);
        auto* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(
            position, end, ImGui::ColorConvertFloat4ToU32(kBackground), ImGui::GetStyle().FrameRounding);
        if (a_formID == 0)
        {
            return;
        }

        if (capturedFormID == a_formID && textures.imageView.Get() && capturedSize.x > 0.0F && capturedSize.y > 0.0F)
        {
            const float scale = (std::min)(a_size.x / capturedSize.x, a_size.y / capturedSize.y);
            const ImVec2 drawn{ capturedSize.x * scale, capturedSize.y * scale };
            const ImVec2 origin{ position.x + (a_size.x - drawn.x) * 0.5F, position.y + (a_size.y - drawn.y) * 0.5F };
            const auto texture = static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(textures.imageView.Get()));
            drawList->AddCallback(SetOpaqueBlend, nullptr);
            drawList->AddImage(ImTextureRef{ texture }, origin, ImVec2{ origin.x + drawn.x, origin.y + drawn.y },
                ImVec2{ 0.0F, 0.0F },
                ImVec2{ capturedSize.x / static_cast<float>(kTextureSize),
                    capturedSize.y / static_cast<float>(kTextureSize) });
            drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
            return;
        }

        const char* note = unavailable                     ? "No preview"
            : now - drawnSince >= kLoadingNoteDelaySeconds ? "Loading..."
                                                           : nullptr;
        if (note)
        {
            const auto textSize = ImGui::CalcTextSize(note);
            drawList->AddText(
                ImVec2{ position.x + (a_size.x - textSize.x) * 0.5F, position.y + (a_size.y - textSize.y) * 0.5F },
                ImGui::GetColorU32(ImGuiCol_TextDisabled), note);
        }
    }

    void Update()
    {
        if (!std::exchange(requested, false) || unavailable || !Config::Get()->ui.itemPreview)
        {
            Release();
            return;
        }
        // The inventory renderer is main-loop state. The overlay's renderer hook
        // is expected, not known, to run on that thread.
        if (!GameThread::IsAuthoritative())
        {
            Disable("the overlay does not run on the main-loop thread");
            Release();
            return;
        }
        auto* ui = RE::UI::GetSingleton();
        auto* manager = RE::Inventory3DManager::GetSingleton();
        if (!ui || !manager)
        {
            Disable("the inventory renderer is missing");
            Release();
            return;
        }
        if (ui->IsItemMenuOpen())
        {
            Release();
            return;
        }

        PROFILE_SCOPE("ItemPreview");
        if (!running)
        {
            manager->Begin3D(RE::INTERFACE_LIGHT_SCHEME::kInventory);
            running = true;
            captureLogged = false;
            logs::debug("Item preview started");
        }
        if (requestedFormID != loadedFormID)
        {
            manager->UnloadInventoryItem();
            loadedFormID = requestedFormID;
            loadedSince = std::chrono::steady_clock::now();
            loadWaitLogged = false;
            capturedFormID = 0;
            if (loadedFormID != 0)
            {
                if (auto* object = RE::TESForm::LookupByID<RE::TESBoundObject>(loadedFormID))
                {
                    manager->LoadInventoryItem(object, nullptr);
                }
                else
                {
                    logs::warn("Item preview: form {:08X} is not an item", loadedFormID);
                }
            }
        }
        if (loadedFormID != 0 && CaptureModel(*manager, *ui))
        {
            capturedFormID = loadedFormID;
        }
        else if (loadedFormID != 0 && capturedFormID == 0 && !loadWaitLogged
            && std::chrono::steady_clock::now() - loadedSince >= kLoadWaitLogDelay)
        {
            loadWaitLogged = true;
            LogLoadWait(*manager);
        }
    }
}
