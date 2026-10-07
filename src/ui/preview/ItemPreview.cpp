#include "ui/preview/ItemPreview.h"
#include "config/PluginConfig.h"
#include "runtime/GameThread.h"
#include "runtime/Profiling.h"

#include <REX/W32/COMPTR.h>
#include <REX/W32/D3D11_1.h>

#include <algorithm>
#include <array>
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

        constexpr std::uint32_t kTextureSize = 2048;
        constexpr float kCaptureMargin = 1.05F;
        constexpr double kLoadingNoteDelaySeconds = 0.3;
        constexpr std::chrono::seconds kLoadWaitLogDelay{ 1 };
        constexpr ImVec4 kBackground{ 0.11F, 0.11F, 0.12F, 1.0F };

        constexpr std::uint32_t kThumbnailSize = 256;
        constexpr std::uint32_t kThumbnailColumns = kTextureSize / kThumbnailSize;
        constexpr std::size_t kThumbnailSlots = kThumbnailColumns * kThumbnailColumns;
        constexpr std::size_t kNoSlot = kThumbnailSlots;
        constexpr float kThumbnailTimeoutSeconds = 5.0F;
        constexpr float kMaximumFrameSeconds = 0.1F;

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
            ComPtr<REX::W32::ID3D11Texture2D> saved;
            // The frame's alpha under the model is not coverage, so the opaque
            // capture is drawn without blending.
            ComPtr<REX::W32::ID3D11BlendState> opaque;
            REX::W32::DXGI_FORMAT format{};
        };

        struct ThumbnailTextures
        {
            // A capture is copied here and reduced by its mip chain to fit a slot.
            ComPtr<REX::W32::ID3D11Texture2D> scratch;
            ComPtr<REX::W32::ID3D11ShaderResourceView> scratchView;
            ComPtr<REX::W32::ID3D11Texture2D> atlas;
            ComPtr<REX::W32::ID3D11ShaderResourceView> atlasView;
            REX::W32::DXGI_FORMAT format{};
        };

        struct ThumbnailSlot
        {
            enum class State : std::uint8_t
            {
                kPending,
                kCaptured,
                kFailed
            };

            // Zero while the slot is free.
            std::uint32_t formID{ 0 };
            ImU32 background{ 0 };
            State state{ State::kPending };
            std::uint64_t order{ 0 };
            int usedFrame{ -1 };
            std::uint32_t width{ 0 };
            std::uint32_t height{ 0 };
        };

        bool requested{ false };
        std::uint32_t requestedFormID{ 0 };
        std::uint32_t drawnFormID{ 0 };
        double drawnSince{ 0.0 };

        bool running{ false };
        bool unavailable{ false };
        bool captureLogged{ false };
        std::uint32_t loadedFormID{ 0 };
        std::chrono::steady_clock::time_point loadedSince;
        bool loadWaitLogged{ false };
        std::uint32_t capturedFormID{ 0 };
        ImVec2 capturedSize{};
        CaptureTextures textures;

        bool thumbnailsRequested{ false };
        bool thumbnailsUnavailable{ false };
        std::array<ThumbnailSlot, kThumbnailSlots> thumbnailSlots;
        std::uint64_t thumbnailOrder{ 0 };
        ThumbnailTextures thumbnailTextures;
        std::size_t capturingSlot{ kNoSlot };
        bool capturingAttached{ false };
        float capturingWaited{ 0.0F };

        void Disable(std::string_view a_reason)
        {
            if (!unavailable)
            {
                logs::warn("Item preview unavailable: {}", a_reason);
                unavailable = true;
            }
        }

        void StopRenderer()
        {
            if (!running)
            {
                return;
            }
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
            capturingSlot = kNoSlot;
            logs::debug("Item preview stopped");
        }

        void Release()
        {
            StopRenderer();
            if (textures.image.Get() || textures.saved.Get() || textures.opaque.Get())
            {
                textures = {};
            }
            if (thumbnailTextures.atlas.Get() || thumbnailOrder != 0)
            {
                thumbnailTextures = {};
                thumbnailSlots = {};
                thumbnailOrder = 0;
            }
        }

        void Start(RE::Inventory3DManager& a_manager)
        {
            if (!running)
            {
                a_manager.Begin3D(RE::INTERFACE_LIGHT_SCHEME::kInventory);
                running = true;
                captureLogged = false;
                logs::debug("Item preview started");
            }
        }

        // Returns whether the form is an item the renderer was asked to load.
        bool Load(RE::Inventory3DManager& a_manager, std::uint32_t a_formID)
        {
            a_manager.UnloadInventoryItem();
            loadedFormID = a_formID;
            loadedSince = std::chrono::steady_clock::now();
            loadWaitLogged = false;
            capturedFormID = 0;
            if (a_formID == 0)
            {
                return false;
            }
            auto* object = RE::TESForm::LookupByID<RE::TESBoundObject>(a_formID);
            if (!object)
            {
                logs::warn("Item preview: form {:08X} is not an item", a_formID);
                return false;
            }
            a_manager.LoadInventoryItem(object, nullptr);
            return true;
        }

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

        void DisableThumbnails(std::string_view a_reason)
        {
            logs::warn("Item thumbnails unavailable: {}", a_reason);
            thumbnailsUnavailable = true;
            thumbnailTextures = {};
        }

        bool EnsureThumbnailTextures(REX::W32::ID3D11Device& a_device, REX::W32::DXGI_FORMAT a_format)
        {
            if (thumbnailTextures.atlas.Get() && thumbnailTextures.format == a_format)
            {
                return true;
            }
            thumbnailTextures = {};
            for (auto& slot : thumbnailSlots)
            {
                if (slot.state == ThumbnailSlot::State::kCaptured)
                {
                    slot.state = ThumbnailSlot::State::kPending;
                }
            }

            std::uint32_t support = 0;
            if (a_device.CheckFormatSupport(a_format, &support) < 0
                || (support & REX::W32::D3D11_FORMAT_SUPPORT_MIP_AUTOGEN) == 0)
            {
                DisableThumbnails(std::format("format {} cannot generate mipmaps", static_cast<int>(a_format)));
                return false;
            }
            REX::W32::D3D11_TEXTURE2D_DESC description{};
            description.width = kTextureSize;
            description.height = kTextureSize;
            description.mipLevels = 0;
            description.arraySize = 1;
            description.format = a_format;
            description.sampleDesc.count = 1;
            description.usage = REX::W32::D3D11_USAGE_DEFAULT;
            description.bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE | REX::W32::D3D11_BIND_RENDER_TARGET;
            description.miscFlags = REX::W32::D3D11_RESOURCE_MISC_GENERATE_MIPS;
            bool created
                = a_device.CreateTexture2D(&description, nullptr, thumbnailTextures.scratch.GetAddressOf()) >= 0
                && a_device.CreateShaderResourceView(
                       thumbnailTextures.scratch.Get(), nullptr, thumbnailTextures.scratchView.GetAddressOf())
                    >= 0;
            description.mipLevels = 1;
            description.bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE;
            description.miscFlags = 0;
            created = created
                && a_device.CreateTexture2D(&description, nullptr, thumbnailTextures.atlas.GetAddressOf()) >= 0
                && a_device.CreateShaderResourceView(
                       thumbnailTextures.atlas.Get(), nullptr, thumbnailTextures.atlasView.GetAddressOf())
                    >= 0;
            if (!created)
            {
                DisableThumbnails(std::format("textures could not be created: format={}", static_cast<int>(a_format)));
                return false;
            }
            thumbnailTextures.format = a_format;
            return true;
        }

        void ClearBackground(REX::W32::ID3D11DeviceContext& a_context, REX::W32::ID3D11RenderTargetView& a_target,
            const CaptureBox& a_box, const ImVec4& a_color)
        {
            ComPtr<REX::W32::ID3D11DeviceContext1> context;
            if (a_context.QueryInterface(
                    REX::W32::IID_ID3D11DeviceContext1, reinterpret_cast<void**>(context.GetAddressOf()))
                    < 0
                || !context.Get())
            {
                return;
            }
            const float color[4]{ a_color.x, a_color.y, a_color.z, a_color.w };
            const REX::W32::D3D11_RECT rect{ static_cast<std::int32_t>(a_box.left),
                static_cast<std::int32_t>(a_box.top), static_cast<std::int32_t>(a_box.left + a_box.width),
                static_cast<std::int32_t>(a_box.top + a_box.height) };
            context->ClearView(&a_target, color, &rect, 1);
        }

        // Inventory3DManager::Render attaches a finished load, then calls the scene
        // render (1.7.104 ID 52727), which draws only while UI::numDisablePauseMenu
        // is positive, as it is while an inventory menu is open.
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

        // Render attaches the model at load task state 5, and the scene render
        // requires both light schemes to match.
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

        // Returns the attached model, attaching one that loaded in the background.
        const RE::NiAVObject* AttachModel(RE::Inventory3DManager& a_manager, RE::UI& a_ui)
        {
            // A model that loaded in the background is attached only by Render.
            // With the count at zero, the scene render draws nothing.
            if (a_manager.GetRuntimeData().loadedModels.empty())
            {
                const PauseMenuDisabledCountScope attachOnly{ a_ui, 0 };
                a_manager.Render();
            }
            const auto& models = a_manager.GetRuntimeData().loadedModels;
            const auto* model = models.empty() ? nullptr : models.back().spModel.get();
            return model && model->worldBound.radius > 0.0F ? model : nullptr;
        }

        enum class Destination : std::uint8_t
        {
            kImage,
            kThumbnailScratch
        };

        // Renders the attached model over a_background and copies it to the destination at its
        // origin, leaving the back buffer as it was.
        std::optional<CaptureBox> RenderModel(RE::Inventory3DManager& a_manager, RE::UI& a_ui,
            const RE::NiAVObject& a_model, const ImVec4& a_background, Destination a_destination)
        {
            auto* scene = RE::UI3DSceneManager::GetSingleton();
            auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
            if (!scene || !renderer)
            {
                return std::nullopt;
            }
            auto& data = renderer->GetRuntimeData();
            auto* target = data.renderWindows[0].renderView;
            if (!data.forwarder || !data.context || !target)
            {
                return std::nullopt;
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
                return std::nullopt;
            }
            REX::W32::D3D11_TEXTURE2D_DESC description{};
            backBuffer->GetDesc(&description);
            const auto box = ProjectModel(*scene, a_model, description.width, description.height);
            if (!box || !EnsureTextures(*data.forwarder, description.format)
                || (a_destination == Destination::kThumbnailScratch
                    && !EnsureThumbnailTextures(*data.forwarder, description.format)))
            {
                return std::nullopt;
            }
            auto* destination
                = a_destination == Destination::kImage ? textures.image.Get() : thumbnailTextures.scratch.Get();

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
            ClearBackground(context, *target, *box, a_background);
            {
                const PauseMenuDisabledCountScope draw{ a_ui, (std::max)(a_ui.numDisablePauseMenu, 1U) };
                a_manager.Render();
            }
            context.CopySubresourceRegion(destination, 0, 0, 0, 0, backBuffer.Get(), 0, &screenBox);
            context.CopySubresourceRegion(
                backBuffer.Get(), 0, box->left, box->top, 0, textures.saved.Get(), 0, &textureBox);

            if (!captureLogged)
            {
                captureLogged = true;
                logs::debug("Item preview captured: formID={:08X} box=({}, {}, {}x{}) screen={}x{} radius={}",
                    loadedFormID, box->left, box->top, box->width, box->height, description.width, description.height,
                    a_model.worldBound.radius);
            }
            return box;
        }

        bool CaptureModel(RE::Inventory3DManager& a_manager, RE::UI& a_ui)
        {
            const auto* model = AttachModel(a_manager, a_ui);
            const auto box
                = model ? RenderModel(a_manager, a_ui, *model, kBackground, Destination::kImage) : std::nullopt;
            if (!box)
            {
                return false;
            }
            capturedSize = ImVec2{ static_cast<float>(box->width), static_cast<float>(box->height) };
            return true;
        }

        bool CaptureThumbnail(RE::Inventory3DManager& a_manager, RE::UI& a_ui, ThumbnailSlot& a_slot)
        {
            const auto* model = AttachModel(a_manager, a_ui);
            // Gives a newly attached model one engine update pass before the single capture its
            // still gets.
            if (!model || !std::exchange(capturingAttached, true))
            {
                return false;
            }
            const auto box = RenderModel(a_manager, a_ui, *model, ImGui::ColorConvertU32ToFloat4(a_slot.background),
                Destination::kThumbnailScratch);
            auto* context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
            if (!box || !context)
            {
                return false;
            }
            // The scratch mip chain halves the capture until it fits a slot. Each level drops
            // a partial last texel, which would hold pixels from outside the capture.
            context->GenerateMips(thumbnailTextures.scratchView.Get());
            std::uint32_t level = 0;
            while (((std::max)(box->width, box->height) >> level) > kThumbnailSize)
            {
                ++level;
            }
            a_slot.width = (std::max)(box->width >> level, 1U);
            a_slot.height = (std::max)(box->height >> level, 1U);
            const auto index = static_cast<std::uint32_t>(&a_slot - thumbnailSlots.data());
            const REX::W32::D3D11_BOX source{
                .left = 0, .top = 0, .front = 0, .right = a_slot.width, .bottom = a_slot.height, .back = 1
            };
            context->CopySubresourceRegion(thumbnailTextures.atlas.Get(), 0, index % kThumbnailColumns * kThumbnailSize,
                index / kThumbnailColumns * kThumbnailSize, 0, thumbnailTextures.scratch.Get(), level, &source);
            logs::debug("Item thumbnail captured: formID={:08X} box={}x{} level={}", a_slot.formID, box->width,
                box->height, level);
            return true;
        }

        ThumbnailSlot* FindThumbnail(std::uint32_t a_formID, ImU32 a_background)
        {
            const auto slot = std::ranges::find_if(thumbnailSlots, [&](const ThumbnailSlot& a_slot) {
                return a_slot.formID == a_formID && a_slot.background == a_background;
            });
            return slot != thumbnailSlots.end() ? std::addressof(*slot) : nullptr;
        }

        // A free slot, else the one drawn longest ago that no window drew this frame.
        ThumbnailSlot* ClaimThumbnail()
        {
            const int frame = ImGui::GetFrameCount();
            ThumbnailSlot* claimed = nullptr;
            for (auto& slot : thumbnailSlots)
            {
                if (slot.formID == 0)
                {
                    return std::addressof(slot);
                }
                if (slot.usedFrame != frame && (!claimed || slot.usedFrame < claimed->usedFrame))
                {
                    claimed = std::addressof(slot);
                }
            }
            if (claimed && static_cast<std::size_t>(claimed - thumbnailSlots.data()) == capturingSlot)
            {
                capturingSlot = kNoSlot;
            }
            return claimed;
        }

        // The earliest requested slot still waiting for its still that a window drew last frame.
        ThumbnailSlot* NextPendingThumbnail()
        {
            // Update runs before ImGui::NewFrame, so this is the frame the windows last drew.
            const int frame = ImGui::GetFrameCount();
            ThumbnailSlot* next = nullptr;
            for (auto& slot : thumbnailSlots)
            {
                if (slot.formID != 0 && slot.state == ThumbnailSlot::State::kPending && slot.usedFrame == frame
                    && (!next || slot.order < next->order))
                {
                    next = std::addressof(slot);
                }
            }
            return next;
        }

        void UpdateLive(RE::Inventory3DManager& a_manager, RE::UI& a_ui)
        {
            Start(a_manager);
            if (requestedFormID != loadedFormID)
            {
                capturingSlot = kNoSlot;
                Load(a_manager, requestedFormID);
            }
            if (loadedFormID != 0 && CaptureModel(a_manager, a_ui))
            {
                capturedFormID = loadedFormID;
            }
            else if (loadedFormID != 0 && capturedFormID == 0 && !loadWaitLogged
                && std::chrono::steady_clock::now() - loadedSince >= kLoadWaitLogDelay)
            {
                loadWaitLogged = true;
                LogLoadWait(a_manager);
            }
        }

        void UpdateThumbnails(RE::Inventory3DManager& a_manager, RE::UI& a_ui)
        {
            auto* slot = NextPendingThumbnail();
            if (!slot)
            {
                StopRenderer();
                return;
            }
            Start(a_manager);
            const auto index = static_cast<std::size_t>(slot - thumbnailSlots.data());
            if (capturingSlot != index || loadedFormID != slot->formID)
            {
                capturingSlot = index;
                capturingAttached = false;
                capturingWaited = 0.0F;
                if (!Load(a_manager, slot->formID))
                {
                    slot->state = ThumbnailSlot::State::kFailed;
                    capturingSlot = kNoSlot;
                    return;
                }
            }
            if (CaptureThumbnail(a_manager, a_ui, *slot))
            {
                slot->state = ThumbnailSlot::State::kCaptured;
                capturingSlot = kNoSlot;
                return;
            }
            capturingWaited += (std::min)(ImGui::GetIO().DeltaTime, kMaximumFrameSeconds);
            if (capturingWaited > kThumbnailTimeoutSeconds || thumbnailsUnavailable)
            {
                if (!thumbnailsUnavailable)
                {
                    logs::warn("Item thumbnail: form {:08X} did not render within {} s", slot->formID,
                        kThumbnailTimeoutSeconds);
                    LogLoadWait(a_manager);
                }
                slot->state = ThumbnailSlot::State::kFailed;
                capturingSlot = kNoSlot;
            }
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

        void AddOpaqueImage(ImDrawList& a_drawList, REX::W32::ID3D11ShaderResourceView& a_view, const ImVec2& a_min,
            const ImVec2& a_max, const ImVec2& a_uvMin, const ImVec2& a_uvMax)
        {
            const auto texture = static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(&a_view));
            a_drawList.AddCallback(SetOpaqueBlend, nullptr);
            a_drawList.AddImage(ImTextureRef{ texture }, a_min, a_max, a_uvMin, a_uvMax);
            a_drawList.AddCallback(ImDrawCallback_ResetRenderState, nullptr);
        }

        // The largest rectangle of the image's aspect centered in the area.
        std::pair<ImVec2, ImVec2> Fit(const ImVec2& a_image, const ImVec2& a_min, const ImVec2& a_max)
        {
            const ImVec2 area{ a_max.x - a_min.x, a_max.y - a_min.y };
            const float scale = (std::min)(area.x / a_image.x, area.y / a_image.y);
            const ImVec2 drawn{ a_image.x * scale, a_image.y * scale };
            const ImVec2 origin{ a_min.x + (area.x - drawn.x) * 0.5F, a_min.y + (area.y - drawn.y) * 0.5F };
            return { origin, ImVec2{ origin.x + drawn.x, origin.y + drawn.y } };
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
            const auto [min, max] = Fit(capturedSize, position, end);
            AddOpaqueImage(*drawList, *textures.imageView.Get(), min, max, ImVec2{ 0.0F, 0.0F },
                ImVec2{ capturedSize.x / static_cast<float>(kTextureSize),
                    capturedSize.y / static_cast<float>(kTextureSize) });
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

    Thumbnail DrawThumbnail(
        ImDrawList& a_drawList, std::uint32_t a_formID, ImU32 a_background, const ImVec2& a_min, const ImVec2& a_max)
    {
        a_drawList.AddRectFilled(a_min, a_max, a_background);
        if (a_formID == 0 || unavailable || thumbnailsUnavailable || !Config::Get()->ui.itemPreview)
        {
            return Thumbnail::kUnavailable;
        }
        thumbnailsRequested = true;
        auto* slot = FindThumbnail(a_formID, a_background);
        if (!slot)
        {
            slot = ClaimThumbnail();
            if (!slot)
            {
                return Thumbnail::kUnavailable;
            }
            *slot = ThumbnailSlot{ .formID = a_formID, .background = a_background, .order = ++thumbnailOrder };
        }
        slot->usedFrame = ImGui::GetFrameCount();
        if (slot->state == ThumbnailSlot::State::kFailed)
        {
            return Thumbnail::kUnavailable;
        }
        if (slot->state != ThumbnailSlot::State::kCaptured || !thumbnailTextures.atlasView.Get()
            || !textures.opaque.Get())
        {
            return Thumbnail::kLoading;
        }
        const auto index = static_cast<std::uint32_t>(slot - thumbnailSlots.data());
        // Half a texel in from the slot's edges, so filtering never reaches the next slot.
        const float texel = 1.0F / static_cast<float>(kTextureSize);
        const ImVec2 uvMin{ static_cast<float>(index % kThumbnailColumns * kThumbnailSize) * texel + texel * 0.5F,
            static_cast<float>(index / kThumbnailColumns * kThumbnailSize) * texel + texel * 0.5F };
        const ImVec2 uvMax{ uvMin.x + (static_cast<float>(slot->width) - 1.0F) * texel,
            uvMin.y + (static_cast<float>(slot->height) - 1.0F) * texel };
        const auto [min, max]
            = Fit(ImVec2{ static_cast<float>(slot->width), static_cast<float>(slot->height) }, a_min, a_max);
        AddOpaqueImage(a_drawList, *thumbnailTextures.atlasView.Get(), min, max, uvMin, uvMax);
        return Thumbnail::kDrawn;
    }

    void Update()
    {
        const bool live = std::exchange(requested, false);
        const bool thumbnails = std::exchange(thumbnailsRequested, false);
        if ((!live && !thumbnails) || unavailable || !Config::Get()->ui.itemPreview)
        {
            Release();
            return;
        }
        // The overlay's renderer hook is expected, not known, to run on the main-loop thread.
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
        if (live)
        {
            UpdateLive(*manager, *ui);
        }
        else
        {
            UpdateThumbnails(*manager, *ui);
        }
    }
}
