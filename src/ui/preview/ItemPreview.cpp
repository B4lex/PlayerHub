#include "ui/preview/ItemPreview.h"
#include "config/PluginConfig.h"
#include "runtime/FrameTime.h"
#include "runtime/GameThread.h"
#include "runtime/Profiling.h"
#include "ui/render/Gpu.h"

#include <REX/W32/D3D11_1.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// The capture follows Item3DPreview in ModExplorerMenu by patchulidev (GPL-3.0).
namespace UI::ItemPreview
{
    namespace
    {
        using Gpu::ComPtr;

        constexpr std::uint32_t kTextureSize = 2048;
        constexpr float kCaptureMargin = 1.05F;
        // The model is captured at twice the pixels it is drawn at. Drawn at exactly half size on
        // whole pixels, each pixel's bilinear sample falls on the corner of a 2x2 texel block and
        // averages it, so the capture is box-filtered without a pass of its own.
        constexpr float kSupersample = 2.0F;
        // A model within this share of its fitted size is not scaled again.
        constexpr float kFitTolerance = 0.01F;
        constexpr double kLoadingNoteDelaySeconds = 0.3;
        constexpr float kLoadWaitLogSeconds = 1.0F;
        constexpr float kCaptureTimeoutSeconds = 5.0F;
        constexpr float kRefreshDelaySeconds = 1.5F;
        // The game attaches an enchantment's glow a frame or two after the model (2026-10-10 log),
        // so an enchanted model keeps being captured this long after a load's first capture, and
        // an enchanted thumbnail waits up to this long for it.
        constexpr float kEffectWatchSeconds = 0.5F;
        // Captures of an animated model whose effect state is logged, for checking that it advances.
        constexpr std::uint32_t kEffectSampleLogs = 6;
        // Dragging across this many font sizes turns the model by one radian.
        constexpr float kTurnFontsPerRadian = 8.0F;
        constexpr ImVec4 kBlack{ 0.0F, 0.0F, 0.0F, 1.0F };
        constexpr ImVec4 kWhite{ 1.0F, 1.0F, 1.0F, 1.0F };
        // Every direction's ambient color in captures, so faces turned from the scene's light are lit.
        constexpr float kCaptureAmbient = 0.45F;

        constexpr std::uint32_t kThumbnailSize = 256;
        // Twice a slot, so the first mip level halves the capture into it.
        constexpr float kThumbnailCaptureSide = 2.0F * kThumbnailSize;
        constexpr std::uint32_t kThumbnailColumns = kTextureSize / kThumbnailSize;
        constexpr std::size_t kThumbnailSlots = kThumbnailColumns * kThumbnailColumns;
        constexpr std::size_t kModelTarget = kThumbnailSlots;
        constexpr std::size_t kNoTarget = kThumbnailSlots + 1;

        // Over black a pixel holds the model's color times its coverage; over white the
        // background adds one minus the coverage, so the difference gives the coverage.
        constexpr std::string_view kSeparateSource = R"(
Texture2D overBlack : register(t0);
Texture2D overWhite : register(t1);

float4 Separate(Vertex input) : SV_Target
{
    int3 position = int3(input.position.xy, 0);
    float3 black = overBlack.Load(position).rgb;
    float3 white = overWhite.Load(position).rgb;
    float alpha = saturate(1.0 - dot(white - black, float3(1.0, 1.0, 1.0) / 3.0));
    return float4(min(black, alpha.xxx), alpha);
}
)";

        struct CaptureBox
        {
            std::uint32_t left;
            std::uint32_t top;
            std::uint32_t width;
            std::uint32_t height;
        };

        enum class CaptureState : std::uint8_t
        {
            kPending,
            // Drawn, with one more capture due kRefreshDelaySeconds later.
            kCaptured,
            kSettled,
            kFailed
        };

        struct Texture
        {
            ComPtr<REX::W32::ID3D11Texture2D> texture;
            ComPtr<REX::W32::ID3D11ShaderResourceView> view;
        };

        struct CaptureTextures
        {
            // The back buffer under the capture, put back afterwards.
            ComPtr<REX::W32::ID3D11Texture2D> saved;
            REX::W32::DXGI_FORMAT format{};
        };

        struct ModelTextures
        {
            Texture overBlack;
            Texture overWhite;
            // Premultiplied by the coverage Separate finds.
            Gpu::RenderTarget model;
            ComPtr<REX::W32::ID3D11PixelShader> separate;
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
            // Zero while the slot is free.
            std::uint32_t formID{ 0 };
            ImU32 background{ 0 };
            CaptureState state{ CaptureState::kPending };
            std::uint64_t order{ 0 };
            int usedFrame{ -1 };
            float capturedAt{ 0.0F };
            std::uint32_t width{ 0 };
            std::uint32_t height{ 0 };
        };

        struct Model
        {
            std::uint32_t formID{ 0 };
            CaptureState state{ CaptureState::kPending };
            float capturedAt{ 0.0F };
            ImVec2 size{};
            // Pixels across its capture, kSupersample times the area it is drawn in, and across its
            // last capture's.
            float side{ 0.0F };
            float capturedSide{ 0.0F };
            // Radians the player turned it by that the next capture applies.
            ImVec2 turn{};
            // Its local transform as fitted and turned, put back when it is loaded again.
            std::optional<RE::NiTransform> transform;
            double requestedSince{ 0.0 };
            bool animated{ false };
            std::uint32_t effectSamples{ 0 };
            float watchedUntil{ 0.0F };
        };

        // The item the renderer loads and what it is captured for.
        struct CaptureJob
        {
            std::size_t target{ kNoTarget };
            bool attached{ false };
            bool captured{ false };
            bool texturesLogged{ false };
            bool waitLogged{ false };
            float waited{ 0.0F };
            bool enchanted{ false };
            // Seconds a thumbnail waited for its glow.
            float watched{ 0.0F };
        };

        struct GameTransform
        {
            std::uint32_t formID{ 0 };
            RE::NiTransform transform;
        };

        bool running{ false };
        bool unavailable{ false };
        bool captureLogged{ false };
        std::uint32_t loadedFormID{ 0 };
        CaptureJob job;
        // In 1.7.104 LoadInventoryItem (ID 51758) reuses a model still in loadedModels (up to 7,
        // keyed by model object) as it was left, fitted and turned, and only End3D (ID 51756)
        // releases them. A form's transform at its first attach since Begin3D is the game's.
        std::vector<GameTransform> gameTransforms;
        float clock{ 0.0F };
        CaptureTextures textures;

        bool modelRequested{ false };
        bool modelUnavailable{ false };
        Model model;
        ModelTextures modelTextures;

        bool thumbnailsRequested{ false };
        bool thumbnailsUnavailable{ false };
        std::array<ThumbnailSlot, kThumbnailSlots> thumbnailSlots;
        std::uint64_t thumbnailOrder{ 0 };
        ThumbnailTextures thumbnailTextures;

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
            job = {};
            gameTransforms.clear();
            logs::debug("Item preview stopped");
        }

        void Release()
        {
            StopRenderer();
            textures = {};
            if (model.formID != 0 || modelTextures.model.texture.Get())
            {
                model = {};
                modelTextures = {};
            }
            if (thumbnailTextures.atlas.Get() || thumbnailOrder != 0)
            {
                thumbnailTextures = {};
                thumbnailSlots = {};
                thumbnailOrder = 0;
            }
            clock = 0.0F;
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

        // Starts a job for a_target; returns whether the form is an item the renderer was asked to load.
        bool Load(RE::Inventory3DManager& a_manager, std::size_t a_target, std::uint32_t a_formID)
        {
            a_manager.UnloadInventoryItem();
            loadedFormID = a_formID;
            job = CaptureJob{ .target = a_target };
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
            // Loaded without extra data, only the form's own enchantment can glow.
            const auto* enchantable = object->As<RE::TESEnchantableForm>();
            job.enchanted = enchantable && enchantable->formEnchanting;
            a_manager.LoadInventoryItem(object, nullptr);
            return true;
        }

        std::optional<CaptureBox> ProjectModel(const RE::UI3DSceneManager& a_scene, const RE::NiAVObject& a_model,
            float a_depth, std::uint32_t a_screenWidth, std::uint32_t a_screenHeight)
        {
            const auto& frustum = a_scene.viewFrustum;
            const float depth = a_depth;
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
            const auto boxLeft = static_cast<std::uint32_t>(left);
            const auto boxTop = static_cast<std::uint32_t>(top);
            const CaptureBox box{ .left = boxLeft,
                .top = boxTop,
                .width = (std::min)(static_cast<std::uint32_t>(std::lround(right - left)), a_screenWidth - boxLeft),
                .height = (std::min)(static_cast<std::uint32_t>(std::lround(bottom - top)), a_screenHeight - boxTop) };
            if (box.width == 0 || box.height == 0)
            {
                return std::nullopt;
            }
            return box;
        }

        REX::W32::D3D11_TEXTURE2D_DESC DescribeTexture(REX::W32::DXGI_FORMAT a_format, std::uint32_t a_bindFlags)
        {
            REX::W32::D3D11_TEXTURE2D_DESC description{};
            description.width = kTextureSize;
            description.height = kTextureSize;
            description.mipLevels = 1;
            description.arraySize = 1;
            description.format = a_format;
            description.sampleDesc.count = 1;
            description.usage = REX::W32::D3D11_USAGE_DEFAULT;
            description.bindFlags = a_bindFlags;
            return description;
        }

        bool CreateTexture(REX::W32::ID3D11Device& a_device, REX::W32::DXGI_FORMAT a_format, Texture& a_texture)
        {
            const auto description = DescribeTexture(a_format, REX::W32::D3D11_BIND_SHADER_RESOURCE);
            return a_device.CreateTexture2D(&description, nullptr, a_texture.texture.GetAddressOf()) >= 0
                && a_device.CreateShaderResourceView(a_texture.texture.Get(), nullptr, a_texture.view.GetAddressOf())
                >= 0;
        }

        bool EnsureTextures(REX::W32::ID3D11Device& a_device, REX::W32::DXGI_FORMAT a_format)
        {
            if (textures.saved.Get() && textures.format == a_format)
            {
                return true;
            }
            textures = {};
            const auto description = DescribeTexture(a_format, 0);
            if (a_device.CreateTexture2D(&description, nullptr, textures.saved.GetAddressOf()) < 0)
            {
                textures = {};
                Disable(std::format("capture textures could not be created: format={}", static_cast<int>(a_format)));
                return false;
            }
            textures.format = a_format;
            return true;
        }

        void DisableModel(std::string_view a_reason)
        {
            logs::warn("Item model preview unavailable: {}", a_reason);
            modelUnavailable = true;
            modelTextures = {};
        }

        bool EnsureModelTextures(REX::W32::ID3D11Device& a_device, REX::W32::DXGI_FORMAT a_format)
        {
            if (modelTextures.model.texture.Get() && modelTextures.format == a_format)
            {
                return true;
            }
            modelTextures = {};
            if (!CreateTexture(a_device, a_format, modelTextures.overBlack)
                || !CreateTexture(a_device, a_format, modelTextures.overWhite)
                || !Gpu::EnsureRenderTarget(a_device, modelTextures.model, kTextureSize, kTextureSize,
                    REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM))
            {
                DisableModel(std::format("textures could not be created: format={}", static_cast<int>(a_format)));
                return false;
            }
            modelTextures.separate = Gpu::CompilePixelShader(a_device, kSeparateSource, "Separate");
            if (!modelTextures.separate.Get())
            {
                DisableModel("its shader could not be created");
                return false;
            }
            modelTextures.format = a_format;
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
                if (slot.state == CaptureState::kCaptured || slot.state == CaptureState::kSettled)
                {
                    slot.state = CaptureState::kPending;
                }
            }

            std::uint32_t support = 0;
            if (a_device.CheckFormatSupport(a_format, &support) < 0
                || (support & REX::W32::D3D11_FORMAT_SUPPORT_MIP_AUTOGEN) == 0)
            {
                DisableThumbnails(std::format("format {} cannot generate mipmaps", static_cast<int>(a_format)));
                return false;
            }
            auto description = DescribeTexture(
                a_format, REX::W32::D3D11_BIND_SHADER_RESOURCE | REX::W32::D3D11_BIND_RENDER_TARGET);
            description.mipLevels = 0;
            description.miscFlags = REX::W32::D3D11_RESOURCE_MISC_GENERATE_MIPS;
            bool created
                = a_device.CreateTexture2D(&description, nullptr, thumbnailTextures.scratch.GetAddressOf()) >= 0
                && a_device.CreateShaderResourceView(
                       thumbnailTextures.scratch.Get(), nullptr, thumbnailTextures.scratchView.GetAddressOf())
                    >= 0;
            description = DescribeTexture(a_format, REX::W32::D3D11_BIND_SHADER_RESOURCE);
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

        // With ImageSpaceManager's TAA on, the UI 3D scene render (1.7.104 ID 52727) jitters the
        // camera, adds the motion vector target, and resolves the item through ISTemporalAA
        // (ImageSpaceManager + 0x1F0, taaEnabled at + 0x18). A single capture has no history to
        // converge on and comes out soft, so its renders run with TAA off.
        class TemporalAAOffScope
        {
        public:
            TemporalAAOffScope()
            {
                auto* manager = RE::ImageSpaceManager::GetSingleton();
                taa = manager ? manager->GetRuntimeData().BSImagespaceShaderISTemporalAA : nullptr;
                if (taa)
                {
                    previous = std::exchange(taa->taaEnabled, false);
                }
            }
            ~TemporalAAOffScope()
            {
                if (taa)
                {
                    taa->taaEnabled = previous;
                }
            }
            TemporalAAOffScope(const TemporalAAOffScope&) = delete;
            TemporalAAOffScope& operator=(const TemporalAAOffScope&) = delete;

            [[nodiscard]] bool WasEnabled() const { return previous; }

        private:
            RE::ImageSpaceManager::UNK_BSImagespaceShaderISTemporalAA* taa{ nullptr };
            bool previous{ false };
        };

        // In 1.7.104 the inventory scene sets no directional ambient, so the lighting shader takes
        // the world's from BSShaderManager::State, which ID 26232 sets from the current cell's
        // lighting or the weather. MistMenu's render (ID 52308) sets its own around its scene render
        // with ID 105643, as this does around a capture's: it takes six directional colors, an
        // ambient specular tint and a fresnel power, writes the state's ambient transform and
        // specular, and keeps the colors at ID 414646, which MistMenu reads back (ID 105644).
        class CaptureAmbientScope
        {
        public:
            CaptureAmbientScope()
                : state(RE::BSShaderManager::State::GetSingleton())
                , transform(state.directionalAmbientTransform)
                , specular(state.ambientSpecular)
                , specularEnabled(state.ambientSpecularEnabled)
                , colors(*LastColors())
            {
                using Function = void(const RE::NiColor*, const RE::NiColor*, float);
                static REL::Relocation<Function*> setAmbient{ REL::ID(105643) };
                const RE::NiColor ambient{ kCaptureAmbient, kCaptureAmbient, kCaptureAmbient };
                const std::array<RE::NiColor, 6> uniform{ ambient, ambient, ambient, ambient, ambient, ambient };
                // MistMenu's tint and fresnel power.
                const RE::NiColor tint{ 1.0F, 1.0F, 1.0F };
                setAmbient(uniform.data(), &tint, 0.0F);
            }
            ~CaptureAmbientScope()
            {
                state.directionalAmbientTransform = transform;
                state.ambientSpecular = specular;
                state.ambientSpecularEnabled = specularEnabled;
                *LastColors() = colors;
            }
            CaptureAmbientScope(const CaptureAmbientScope&) = delete;
            CaptureAmbientScope& operator=(const CaptureAmbientScope&) = delete;

            // The world's ambient without direction.
            [[nodiscard]] const RE::NiPoint3& WorldAmbient() const { return transform.translate; }

        private:
            static std::array<RE::NiColor, 6>* LastColors()
            {
                static REL::Relocation<std::array<RE::NiColor, 6>*> colors{ REL::ID(414646) };
                return colors.get();
            }

            RE::BSShaderManager::State& state;
            RE::NiTransform transform;
            RE::NiColorA specular;
            bool specularEnabled;
            std::array<RE::NiColor, 6> colors;
        };

        // Render attaches the model at load task state 5, and the scene render
        // requires both light schemes to match.
        void LogLoadWait(RE::Inventory3DManager& a_manager)
        {
            const auto& runtime = a_manager.GetRuntimeData();
            const auto* task = reinterpret_cast<const std::uint8_t*>(runtime.loadTask.get());
            const auto* scene = RE::UI3DSceneManager::GetSingleton();
            const auto* loaded = runtime.loadedModels.empty() ? nullptr : std::addressof(runtime.loadedModels.back());
            const auto* object = loaded ? loaded->spModel.get() : nullptr;
            logs::debug(
                "Item preview waiting: formID={:08X} models={} modelForm={:08X} modelRadius={} loadTaskState={} "
                "managerScheme={} sceneScheme={}",
                loadedFormID, runtime.loadedModels.size(),
                loaded && loaded->itemBase ? loaded->itemBase->GetFormID() : 0U,
                object ? object->worldBound.radius : 0.0F,
                task ? static_cast<std::int64_t>(*reinterpret_cast<const std::uint32_t*>(task + 0xC)) : -1,
                static_cast<int>(a_manager.currentLightScheme),
                scene ? static_cast<int>(scene->currentlightScheme) : -1);
        }

        // Returns the attached model, attaching one that loaded in the background.
        RE::NiAVObject* AttachedModel(const RE::Inventory3DManager& a_manager)
        {
            const auto& models = a_manager.GetRuntimeData().loadedModels;
            if (models.empty())
            {
                return nullptr;
            }
            // After UnloadInventoryItem and a new load, the previous item's model was seen
            // still attached for a frame (2026-10-08 log), so the entry must be the loaded form's.
            const auto& loaded = models.back();
            auto* object = loaded.spModel.get();
            return loaded.itemBase && loaded.itemBase->GetFormID() == loadedFormID && object
                    && object->worldBound.radius > 0.0F
                ? object
                : nullptr;
        }

        RE::NiAVObject* AttachModel(RE::Inventory3DManager& a_manager, RE::UI& a_ui)
        {
            if (auto* object = AttachedModel(a_manager))
            {
                return object;
            }
            // A model that loaded in the background is attached only by Render.
            // With the count at zero, the scene render draws nothing.
            {
                const PauseMenuDisabledCountScope attachOnly{ a_ui, 0 };
                a_manager.Render();
            }
            return AttachedModel(a_manager);
        }

        struct TextureCheck
        {
            std::uint32_t total{ 0 };
            std::uint32_t pending{ 0 };
            std::string_view firstDiffuse;
            std::string_view firstPending;
        };

        // In 1.7.104 a model's textures load on its loader thread: BSShaderManager::GetTexture
        // (ID 105640) reads the file synchronously, and BSShaderResourceManager (vtable slot 0xD0,
        // ID 108531) sets rendererTexture as the NiSourceTexture is created. Whether the D3D
        // texture behind it can still be missing after attaching was not traced.
        TextureCheck InspectTextures(RE::NiAVObject& a_model)
        {
            TextureCheck result;
            const auto inspect = [&](const RE::NiSourceTexture* a_texture) {
                if (!a_texture)
                {
                    return;
                }
                ++result.total;
                const auto* data = a_texture->rendererTexture;
                if (!data || !data->texture || !data->resourceView)
                {
                    ++result.pending;
                    if (result.firstPending.empty())
                    {
                        result.firstPending = a_texture->name.c_str();
                    }
                }
            };
            RE::BSVisit::TraverseScenegraphGeometries(&a_model, [&](RE::BSGeometry* a_geometry) {
                auto* property = a_geometry->GetGeometryRuntimeData().shaderProperty.get();
                auto* material = property ? property->GetBaseMaterial() : nullptr;
                if (material && material->GetType() == RE::BSShaderMaterial::Type::kLighting)
                {
                    const auto& lighting = *static_cast<const RE::BSLightingShaderMaterialBase*>(material);
                    if (result.firstDiffuse.empty() && lighting.diffuseTexture)
                    {
                        result.firstDiffuse = lighting.diffuseTexture->name.c_str();
                    }
                    inspect(lighting.diffuseTexture.get());
                    inspect(lighting.normalTexture.get());
                }
                return RE::BSVisit::BSVisitControl::kContinue;
            });
            return result;
        }

        struct EffectCheck
        {
            std::uint32_t effects{ 0 };
            std::uint32_t controllers{ 0 };
            const RE::BSEffectShaderData* first{ nullptr };
        };

        // An enchanted item's glow is effect data on its shader properties, which the game
        // advances between frames; time controllers advance on each Render.
        EffectCheck InspectEffects(RE::NiAVObject& a_model)
        {
            EffectCheck result;
            const auto count = [&](const RE::NiObjectNET* a_object) {
                if (a_object && a_object->GetControllers())
                {
                    ++result.controllers;
                }
            };
            RE::BSVisit::TraverseScenegraphObjects(&a_model, [&](RE::NiAVObject* a_object) {
                count(a_object);
                if (auto* geometry = a_object->AsGeometry())
                {
                    const auto& data = geometry->GetGeometryRuntimeData();
                    count(data.alphaProperty.get());
                    const auto* property = data.shaderProperty.get();
                    count(property);
                    if (property && property->effectData)
                    {
                        ++result.effects;
                        if (!result.first)
                        {
                            result.first = property->effectData.get();
                        }
                    }
                }
                return RE::BSVisit::BSVisitControl::kContinue;
            });
            return result;
        }

        void CheckAnimation(RE::NiAVObject& a_model)
        {
            if (model.animated && model.effectSamples >= kEffectSampleLogs)
            {
                return;
            }
            const auto effects = InspectEffects(a_model);
            if (!model.animated)
            {
                if (effects.effects == 0 && effects.controllers == 0)
                {
                    return;
                }
                model.animated = true;
                logs::debug("Item model animated: formID={:08X} effects={} controllers={}", model.formID,
                    effects.effects, effects.controllers);
            }
            ++model.effectSamples;
            if (const auto* data = effects.first)
            {
                logs::debug("Item model effect: formID={:08X} clock={:.3f} offset=({:.3f}, {:.3f}) fill={:.3f} "
                            "baseFill={:.3f} rim={:.3f}",
                    model.formID, clock, data->uOffset, data->vOffset, data->fillColor.alpha, data->baseFillAlpha,
                    data->baseRimAlpha);
            }
        }

        // ID 70251, which Render calls on the model with these flags before drawing; a time of zero
        // leaves its controllers where they are. Refreshes the world bound after a transform change.
        void UpdateWorld(RE::NiAVObject& a_model)
        {
            RE::NiUpdateData data{ .time = 0.0F, .flags = RE::NiUpdateData::Flag::kDirty };
            a_model.Update(data);
        }

        const RE::NiTransform& GameTransformOf(const RE::NiAVObject& a_model)
        {
            const auto known = std::ranges::find(gameTransforms, loadedFormID, &GameTransform::formID);
            if (known != gameTransforms.end())
            {
                return known->transform;
            }
            return gameTransforms.emplace_back(loadedFormID, a_model.local).transform;
        }

        // The loaded model once it can be captured: attached, given one more engine update pass,
        // and with every texture present unless a_force. A model loaded again is given back the
        // transform the player turned it to, and a still the game's.
        RE::NiAVObject* ReadyModel(
            RE::Inventory3DManager& a_manager, RE::UI& a_ui, bool a_force, TextureCheck& a_textures)
        {
            auto* object = AttachModel(a_manager, a_ui);
            if (!object)
            {
                return nullptr;
            }
            if (!std::exchange(job.attached, true))
            {
                const auto& game = GameTransformOf(*object);
                object->local = job.target == kModelTarget && model.transform ? *model.transform : game;
                UpdateWorld(*object);
                return nullptr;
            }
            a_textures = InspectTextures(*object);
            if (a_textures.pending > 0 && !a_force)
            {
                if (!std::exchange(job.texturesLogged, true))
                {
                    logs::debug("Item preview waiting for textures: formID={:08X} pending={}/{} first={}",
                        loadedFormID, a_textures.pending, a_textures.total, a_textures.firstPending);
                }
                return nullptr;
            }
            return object;
        }

        // 1.7.104 ID 51778, which the inventory menu's mouse (ID 51770) and thumbstick handlers
        // call with the drag scaled to radians, x negated: turns the last loaded model about its
        // bound's center, unless it is hidden or the item is a light (form type 0x1F).
        void Turn(RE::Inventory3DManager& a_manager, const ImVec2& a_angles)
        {
            using Function = void(RE::Inventory3DManager*, const RE::NiPoint2*);
            static REL::Relocation<Function*> function{ REL::ID(51778) };
            const RE::NiPoint2 angles{ a_angles.x, a_angles.y };
            function(&a_manager, &angles);
        }

        // Scales the model about its bound's center so its capture is a_side pixels square, and
        // moves it across the view only as far as keeps that square on
        // the screen. Returns the depth to project it at.
        float FitModel(RE::NiAVObject& a_model, const RE::UI3DSceneManager& a_scene, float a_side,
            std::uint32_t a_screenWidth, std::uint32_t a_screenHeight)
        {
            const auto& frustum = a_scene.viewFrustum;
            const auto screenWidth = static_cast<float>(a_screenWidth);
            const auto screenHeight = static_cast<float>(a_screenHeight);
            auto center = a_model.worldBound.center;
            const float depth = center.y;
            const float worldLeft = -frustum.fLeft * depth;
            const float worldBottom = -frustum.fBottom * depth;
            const float unitsPerPixelX = (-frustum.fRight * depth - worldLeft) / screenWidth;
            const float unitsPerPixelY = (-frustum.fTop * depth - worldBottom) / screenHeight;
            const float side = std::clamp(
                a_side, 1.0F, (std::min)({ screenWidth, screenHeight, static_cast<float>(kTextureSize) }));
            const float radius
                = side * 0.5F * (std::min)(std::abs(unitsPerPixelX), std::abs(unitsPerPixelY)) / kCaptureMargin;
            const float current = a_model.worldBound.radius;
            if (!std::isfinite(unitsPerPixelX) || !std::isfinite(unitsPerPixelY) || unitsPerPixelX == 0.0F
                || unitsPerPixelY == 0.0F || !std::isfinite(radius) || !(current > 0.0F))
            {
                return depth;
            }
            auto& local = a_model.local;
            bool changed = false;
            const float scale = radius / current;
            if (std::abs(scale - 1.0F) > kFitTolerance)
            {
                local.scale *= scale;
                local.translate = center + (local.translate - center) * scale;
                changed = true;
            }
            const float half = side * 0.5F;
            const float centerX = -(center.x + worldLeft) / unitsPerPixelX;
            const float centerY = -(center.z + worldBottom) / unitsPerPixelY;
            const float x = std::clamp(centerX, half, screenWidth - half);
            const float y = std::clamp(centerY, half, screenHeight - half);
            if (x != centerX || y != centerY)
            {
                const RE::NiPoint3 shift{ (centerX - x) * unitsPerPixelX, 0.0F, (centerY - y) * unitsPerPixelY };
                local.translate += shift;
                changed = true;
            }
            if (changed)
            {
                UpdateWorld(a_model);
            }
            return depth;
        }

        struct Layer
        {
            ImVec4 background;
            REX::W32::ID3D11Texture2D* destination;
        };

        // Renders the attached model over each layer's background and copies it to the layer's
        // destination at its origin, leaving the back buffer as it was.
        std::optional<CaptureBox> RenderModel(RE::Inventory3DManager& a_manager, RE::UI& a_ui,
            const RE::NiAVObject& a_model, float a_depth, const Gpu::Frame& a_frame, std::span<const Layer> a_layers)
        {
            auto* scene = RE::UI3DSceneManager::GetSingleton();
            if (!scene)
            {
                return std::nullopt;
            }
            const auto& description = a_frame.description;
            const auto box = ProjectModel(*scene, a_model, a_depth, description.width, description.height);
            if (!box || !EnsureTextures(a_frame.device, description.format))
            {
                return std::nullopt;
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
            auto& context = a_frame.context;
            auto* backBuffer = a_frame.backBuffer.Get();
            context.CopySubresourceRegion(textures.saved.Get(), 0, 0, 0, 0, backBuffer, 0, &screenBox);
            const TemporalAAOffScope noTemporalAA;
            const CaptureAmbientScope ambient;
            for (const auto& layer : a_layers)
            {
                ClearBackground(context, a_frame.target, *box, layer.background);
                {
                    const PauseMenuDisabledCountScope draw{ a_ui, (std::max)(a_ui.numDisablePauseMenu, 1U) };
                    a_manager.Render();
                }
                context.CopySubresourceRegion(layer.destination, 0, 0, 0, 0, backBuffer, 0, &screenBox);
            }
            context.CopySubresourceRegion(backBuffer, 0, box->left, box->top, 0, textures.saved.Get(), 0, &textureBox);

            if (!captureLogged)
            {
                captureLogged = true;
                logs::debug("Item preview captured: formID={:08X} box=({}, {}, {}x{}) screen={}x{} radius={} "
                            "gameTAA={} worldAmbient=({:.3f}, {:.3f}, {:.3f})",
                    loadedFormID, box->left, box->top, box->width, box->height, description.width, description.height,
                    a_model.worldBound.radius, noTemporalAA.WasEnabled(), ambient.WorldAmbient().x,
                    ambient.WorldAmbient().y, ambient.WorldAmbient().z);
            }
            return box;
        }

        // Fits the model to its capture size, captures it over black and over white, and
        // separates its coverage from them.
        bool CaptureModel(RE::Inventory3DManager& a_manager, RE::UI& a_ui, RE::NiAVObject& a_model)
        {
            const auto frame = Gpu::GetFrame();
            auto* scene = RE::UI3DSceneManager::GetSingleton();
            if (!frame || !scene || !EnsureModelTextures(frame->device, frame->description.format))
            {
                return false;
            }
            const auto& description = frame->description;
            const float depth = FitModel(a_model, *scene, model.side, description.width, description.height);
            model.transform = a_model.local;
            const std::array layers{ Layer{ kBlack, modelTextures.overBlack.texture.Get() },
                Layer{ kWhite, modelTextures.overWhite.texture.Get() } };
            const auto box = RenderModel(a_manager, a_ui, a_model, depth, *frame, layers);
            if (!box)
            {
                return false;
            }
            Gpu::PassScope passes{ *frame };
            if (!passes.IsReady())
            {
                DisableModel("its pipeline states could not be created");
                return false;
            }
            const std::array sources{ modelTextures.overBlack.view.Get(), modelTextures.overWhite.view.Get() };
            passes.Draw(*modelTextures.separate.Get(), sources, modelTextures.model, box->width, box->height);
            model.size = ImVec2{ static_cast<float>(box->width), static_cast<float>(box->height) };
            model.capturedSide = model.side;
            return true;
        }

        bool CaptureThumbnail(RE::Inventory3DManager& a_manager, RE::UI& a_ui, RE::NiAVObject& a_model,
            ThumbnailSlot& a_slot, const TextureCheck& a_textures)
        {
            const auto frame = Gpu::GetFrame();
            auto* scene = RE::UI3DSceneManager::GetSingleton();
            if (!frame || !scene || !EnsureThumbnailTextures(frame->device, frame->description.format))
            {
                return false;
            }
            const auto& description = frame->description;
            const float depth
                = FitModel(a_model, *scene, kThumbnailCaptureSide, description.width, description.height);
            const std::array layers{ Layer{ ImGui::ColorConvertU32ToFloat4(a_slot.background),
                thumbnailTextures.scratch.Get() } };
            const auto box = RenderModel(a_manager, a_ui, a_model, depth, *frame, layers);
            if (!box)
            {
                return false;
            }
            auto& context = frame->context;
            // The scratch mip chain halves the capture until it fits a slot. Each level drops
            // a partial last texel, which would hold pixels from outside the capture.
            context.GenerateMips(thumbnailTextures.scratchView.Get());
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
            context.CopySubresourceRegion(thumbnailTextures.atlas.Get(), 0, index % kThumbnailColumns * kThumbnailSize,
                index / kThumbnailColumns * kThumbnailSize, 0, thumbnailTextures.scratch.Get(), level, &source);
            logs::debug(
                "Item thumbnail captured: formID={:08X} box={}x{} level={} refresh={} textures={} pending={} "
                "diffuse={}",
                a_slot.formID, box->width, box->height, level, a_slot.state == CaptureState::kCaptured,
                a_textures.total, a_textures.pending, a_textures.firstDiffuse);
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
            if (claimed && static_cast<std::size_t>(claimed - thumbnailSlots.data()) == job.target)
            {
                job.target = kNoTarget;
            }
            return claimed;
        }

        // Among the slots a window drew last frame, the earliest requested one waiting for its
        // still, else the earliest one due for its refresh.
        ThumbnailSlot* NextThumbnail()
        {
            // Update runs before ImGui::NewFrame, so this is the frame the windows last drew.
            const int frame = ImGui::GetFrameCount();
            ThumbnailSlot* next = nullptr;
            bool nextPending = false;
            for (auto& slot : thumbnailSlots)
            {
                if (slot.formID == 0 || slot.usedFrame != frame)
                {
                    continue;
                }
                const bool pending = slot.state == CaptureState::kPending;
                const bool refresh
                    = slot.state == CaptureState::kCaptured && clock - slot.capturedAt >= kRefreshDelaySeconds;
                if (!pending && !refresh)
                {
                    continue;
                }
                if (!next || (pending && !nextPending) || (pending == nextPending && slot.order < next->order))
                {
                    next = std::addressof(slot);
                    nextPending = pending;
                }
            }
            return next;
        }

        // A capture, a refresh, a turn, or a fresh load being watched waits for the model.
        bool IsModelDue()
        {
            if (model.formID == 0 || modelUnavailable)
            {
                return false;
            }
            switch (model.state)
            {
            case CaptureState::kPending:
                return true;
            case CaptureState::kCaptured:
                return clock - model.capturedAt >= kRefreshDelaySeconds || clock < model.watchedUntil
                    || model.turn.x != 0.0F || model.turn.y != 0.0F || model.side != model.capturedSide;
            case CaptureState::kSettled:
                return clock < model.watchedUntil || model.turn.x != 0.0F || model.turn.y != 0.0F
                    || model.side != model.capturedSide;
            case CaptureState::kFailed:
                return false;
            }
            return false;
        }

        // Captured on every update while nothing else is due, as the inventory menu renders it.
        bool IsModelAnimating()
        {
            return model.animated && !modelUnavailable
                && (model.state == CaptureState::kCaptured || model.state == CaptureState::kSettled);
        }

        void UpdateModel(RE::Inventory3DManager& a_manager, RE::UI& a_ui, float a_delta)
        {
            Start(a_manager);
            const bool refresh
                = model.state == CaptureState::kCaptured && clock - model.capturedAt >= kRefreshDelaySeconds;
            // A refresh loads the item again; a turn reuses the loaded model.
            if (job.target != kModelTarget || loadedFormID != model.formID || (refresh && job.captured))
            {
                if (!Load(a_manager, kModelTarget, model.formID))
                {
                    model.state = CaptureState::kFailed;
                    return;
                }
            }
            const bool timedOut = job.waited > kCaptureTimeoutSeconds;
            TextureCheck check;
            if (auto* object = ReadyModel(a_manager, a_ui, timedOut, check))
            {
                if (model.turn.x != 0.0F || model.turn.y != 0.0F)
                {
                    Turn(a_manager, std::exchange(model.turn, ImVec2{}));
                }
                if (CaptureModel(a_manager, a_ui, *object))
                {
                    if (!job.captured)
                    {
                        logs::debug("Item model captured: formID={:08X} size={}x{} refresh={} textures={} pending={} "
                                    "diffuse={}",
                            model.formID, model.size.x, model.size.y, refresh, check.total, check.pending,
                            check.firstDiffuse);
                    }
                    if (!job.captured && job.enchanted)
                    {
                        model.watchedUntil = clock + kEffectWatchSeconds;
                    }
                    job.captured = true;
                    CheckAnimation(*object);
                    if (model.state == CaptureState::kPending || refresh)
                    {
                        model.state = refresh ? CaptureState::kSettled : CaptureState::kCaptured;
                        model.capturedAt = clock;
                    }
                    return;
                }
            }
            job.waited += a_delta;
            if (job.waited >= kLoadWaitLogSeconds && !std::exchange(job.waitLogged, true))
            {
                LogLoadWait(a_manager);
            }
            if (timedOut || modelUnavailable)
            {
                if (!modelUnavailable && model.state == CaptureState::kPending)
                {
                    logs::warn(
                        "Item model: form {:08X} did not render within {} s", model.formID, kCaptureTimeoutSeconds);
                }
                // A refresh or a turn that cannot render keeps the image it has.
                model.state
                    = model.state == CaptureState::kPending ? CaptureState::kFailed : CaptureState::kSettled;
                model.turn = {};
                job.captured = true;
            }
        }

        void UpdateThumbnails(RE::Inventory3DManager& a_manager, RE::UI& a_ui, float a_delta)
        {
            auto* slot = NextThumbnail();
            if (!slot)
            {
                StopRenderer();
                return;
            }
            Start(a_manager);
            const auto index = static_cast<std::size_t>(slot - thumbnailSlots.data());
            const bool refresh = slot->state == CaptureState::kCaptured;
            // A refresh that cannot render keeps the still it has.
            const auto finish = [&](bool a_captured) {
                if (refresh)
                {
                    slot->state = CaptureState::kSettled;
                }
                else
                {
                    slot->state = a_captured ? CaptureState::kCaptured : CaptureState::kFailed;
                }
                slot->capturedAt = clock;
                job.target = kNoTarget;
            };
            if (job.target != index || loadedFormID != slot->formID)
            {
                if (!Load(a_manager, index, slot->formID))
                {
                    finish(false);
                    return;
                }
            }
            const bool timedOut = job.waited > kCaptureTimeoutSeconds;
            TextureCheck check;
            auto* object = ReadyModel(a_manager, a_ui, timedOut, check);
            // A still is captured once, so an enchanted item's waits for its glow. Meanwhile only
            // Render's update pass runs, which draws nothing with the count at zero.
            if (object && job.enchanted && job.watched < kEffectWatchSeconds && InspectEffects(*object).effects == 0)
            {
                const PauseMenuDisabledCountScope updateOnly{ a_ui, 0 };
                a_manager.Render();
                job.watched += a_delta;
                return;
            }
            if (object && CaptureThumbnail(a_manager, a_ui, *object, *slot, check))
            {
                if (job.texturesLogged)
                {
                    logs::warn("Item thumbnail: form {:08X} captured after {:.2f} s waiting for textures",
                        slot->formID, job.waited);
                }
                if (job.enchanted)
                {
                    logs::debug("Item thumbnail: form {:08X} waited {:.3f} s for its effect", slot->formID,
                        job.watched);
                }
                finish(true);
                return;
            }
            job.waited += a_delta;
            if (timedOut || thumbnailsUnavailable)
            {
                if (!thumbnailsUnavailable)
                {
                    logs::warn("Item thumbnail: form {:08X} did not render within {} s", slot->formID,
                        kCaptureTimeoutSeconds);
                    LogLoadWait(a_manager);
                }
                finish(false);
            }
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

    void DrawModel(std::uint32_t a_formID, const ImVec2& a_size)
    {
        if (a_size.x <= 0.0F || a_size.y <= 0.0F)
        {
            return;
        }
        const auto origin = ImGui::GetCursorScreenPos();
        const ImVec2 end{ origin.x + a_size.x, origin.y + a_size.y };
        ImGui::InvisibleButton("##model", a_size);
        if (a_formID == 0 || !Config::Get()->ui.itemPreview)
        {
            return;
        }
        modelRequested = true;
        const auto now = ImGui::GetTime();
        if (a_formID != model.formID)
        {
            model = Model{ .formID = a_formID, .requestedSince = now };
        }
        model.side = std::floor((std::min)(a_size.x, a_size.y)) * kSupersample;
        if (ImGui::IsItemActive() && model.state != CaptureState::kFailed)
        {
            const auto& delta = ImGui::GetIO().MouseDelta;
            const float scale = 1.0F / (kTurnFontsPerRadian * ImGui::GetFontSize());
            model.turn.x -= delta.x * scale;
            model.turn.y += delta.y * scale;
        }

        auto& drawList = *ImGui::GetWindowDrawList();
        const bool captured = model.state == CaptureState::kCaptured || model.state == CaptureState::kSettled;
        if (captured && modelTextures.model.view.Get() && model.size.x > 0.0F && model.size.y > 0.0F)
        {
            auto [min, max] = Fit(model.size, origin, end);
            const auto texel = 1.0F / static_cast<float>(kTextureSize);
            ImVec2 uvMax{ model.size.x * texel, model.size.y * texel };
            const ImVec2 drawn{ std::floor(model.size.x / kSupersample), std::floor(model.size.y / kSupersample) };
            // A capture fitted to this area, not clamped to the screen, is drawn at exactly
            // 1 / kSupersample on whole pixels, leaving out a last odd texel.
            if (std::abs(max.x - min.x - drawn.x) <= 2.0F && std::abs(max.y - min.y - drawn.y) <= 2.0F)
            {
                min = ImVec2{ std::floor(origin.x + (a_size.x - drawn.x) * 0.5F),
                    std::floor(origin.y + (a_size.y - drawn.y) * 0.5F) };
                max = ImVec2{ min.x + drawn.x, min.y + drawn.y };
                uvMax = ImVec2{ drawn.x * kSupersample * texel, drawn.y * kSupersample * texel };
            }
            if (Gpu::AddImage(drawList, *modelTextures.model.view.Get(), min, max, ImVec2{ 0.0F, 0.0F }, uvMax,
                    Gpu::Blend::kPremultiplied))
            {
                return;
            }
        }

        const char* note = unavailable || modelUnavailable || model.state == CaptureState::kFailed ? "No preview"
            : now - model.requestedSince >= kLoadingNoteDelaySeconds                             ? "Loading..."
                                                                                                 : nullptr;
        if (note)
        {
            const auto textSize = ImGui::CalcTextSize(note);
            drawList.AddText(
                ImVec2{ origin.x + (a_size.x - textSize.x) * 0.5F, origin.y + (a_size.y - textSize.y) * 0.5F },
                ImGui::GetColorU32(ImGuiCol_TextDisabled), note);
        }
    }

    Thumbnail DrawThumbnail(ImDrawList& a_drawList, std::uint32_t a_formID, ImU32 a_background, const ImVec2& a_min,
        const ImVec2& a_max)
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
        if (slot->state == CaptureState::kFailed)
        {
            return Thumbnail::kUnavailable;
        }
        if ((slot->state != CaptureState::kCaptured && slot->state != CaptureState::kSettled)
            || !thumbnailTextures.atlasView.Get())
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
        // The still was captured on its background, whose alpha under the model is not coverage.
        return Gpu::AddImage(
                   a_drawList, *thumbnailTextures.atlasView.Get(), min, max, uvMin, uvMax, Gpu::Blend::kOpaque)
            ? Thumbnail::kDrawn
            : Thumbnail::kUnavailable;
    }

    void Update()
    {
        const bool modelDrawn = std::exchange(modelRequested, false);
        const bool thumbnails = std::exchange(thumbnailsRequested, false);
        if ((!modelDrawn && !thumbnails) || unavailable || !Config::Get()->ui.itemPreview)
        {
            Release();
            return;
        }
        if (!modelDrawn && model.formID != 0)
        {
            model = {};
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
        const float delta = FrameTime::Clamp(ImGui::GetIO().DeltaTime);
        clock += delta;
        if (IsModelDue())
        {
            UpdateModel(*manager, *ui, delta);
        }
        // An animated model gives way to stills only while one is due, so the loaded item does
        // not switch every frame.
        else if (thumbnails && NextThumbnail())
        {
            UpdateThumbnails(*manager, *ui, delta);
        }
        else if (IsModelAnimating())
        {
            UpdateModel(*manager, *ui, delta);
        }
        else
        {
            StopRenderer();
        }
    }
}
