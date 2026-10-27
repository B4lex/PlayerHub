#include "ui/render/Backdrop.h"
#include "runtime/Profiling.h"
#include "ui/Theme.h"
#include "ui/render/Gpu.h"

#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace UI::Backdrop
{
    namespace
    {
        using Gpu::ComPtr;

        constexpr std::uint32_t kDownscale = 4;
        // The blurred frame keeps 60% of its brightness.
        constexpr ImU32 kTint = IM_COL32(153, 153, 153, 255);
        constexpr ImU32 kDim = IM_COL32(0, 0, 0, 102);

        constexpr std::string_view kSource = R"(
Texture2D source : register(t0);
SamplerState linearClamp : register(s0);

// Each bilinear tap averages a 2x2 block, so four cover the 4x4 block under the pixel.
float4 Downsample(Vertex input) : SV_Target
{
    uint width;
    uint height;
    source.GetDimensions(width, height);
    float2 texel = 1.0 / float2(width, height);
    float3 color = source.SampleLevel(linearClamp, input.uv + texel * float2(-1.0, -1.0), 0).rgb
        + source.SampleLevel(linearClamp, input.uv + texel * float2(1.0, -1.0), 0).rgb
        + source.SampleLevel(linearClamp, input.uv + texel * float2(-1.0, 1.0), 0).rgb
        + source.SampleLevel(linearClamp, input.uv + texel * float2(1.0, 1.0), 0).rgb;
    return float4(color * 0.25, 1.0);
}

static const int kRadius = 8;
static const float kSigma = 3.5;

float4 Blur(Vertex input, int2 direction)
{
    uint width;
    uint height;
    source.GetDimensions(width, height);
    int2 last = int2(width, height) - 1;
    int2 center = int2(input.position.xy);
    float3 sum = 0.0;
    float total = 0.0;
    [unroll] for (int i = -kRadius; i <= kRadius; ++i)
    {
        float weight = exp(-float(i * i) / (2.0 * kSigma * kSigma));
        sum += source.Load(int3(clamp(center + direction * i, int2(0, 0), last), 0)).rgb * weight;
        total += weight;
    }
    return float4(sum / total, 1.0);
}

float4 BlurHorizontal(Vertex input) : SV_Target
{
    return Blur(input, int2(1, 0));
}

float4 BlurVertical(Vertex input) : SV_Target
{
    return Blur(input, int2(0, 1));
}
)";

        struct Shaders
        {
            ComPtr<REX::W32::ID3D11PixelShader> downsample;
            ComPtr<REX::W32::ID3D11PixelShader> horizontal;
            ComPtr<REX::W32::ID3D11PixelShader> vertical;
        };

        struct Textures
        {
            ComPtr<REX::W32::ID3D11Texture2D> copy;
            ComPtr<REX::W32::ID3D11ShaderResourceView> copyView;
            REX::W32::D3D11_TEXTURE2D_DESC copyDescription{};
            Gpu::RenderTarget reduced;
            Gpu::RenderTarget blurred;
        };

        Shaders shaders;
        Textures textures;
        bool unavailable{ false };
        bool requested{ false };
        int drawnFrame{ -1 };
        int blurredFrame{ -1 };
        bool frameBlurred{ false };
        bool uiLayerLogged{ false };
        std::vector<std::unique_ptr<ImDrawList>> popupLists;

        bool Disable(std::string_view a_reason)
        {
            logs::warn("UI backdrop blur unavailable: {}; the backdrop is only darkened", a_reason);
            unavailable = true;
            textures = {};
            shaders = {};
            return false;
        }

        bool EnsureShaders(REX::W32::ID3D11Device& a_device)
        {
            if (shaders.downsample.Get())
            {
                return true;
            }
            shaders.downsample = Gpu::CompilePixelShader(a_device, kSource, "Downsample");
            shaders.horizontal = Gpu::CompilePixelShader(a_device, kSource, "BlurHorizontal");
            shaders.vertical = Gpu::CompilePixelShader(a_device, kSource, "BlurVertical");
            return shaders.downsample.Get() && shaders.horizontal.Get() && shaders.vertical.Get();
        }

        bool EnsureTextures(REX::W32::ID3D11Device& a_device, const REX::W32::D3D11_TEXTURE2D_DESC& a_backBuffer)
        {
            auto& copy = textures.copyDescription;
            if (!textures.copy.Get() || copy.width != a_backBuffer.width || copy.height != a_backBuffer.height
                || copy.format != a_backBuffer.format)
            {
                textures.copy = nullptr;
                textures.copyView = nullptr;
                copy = REX::W32::D3D11_TEXTURE2D_DESC{};
                copy.width = a_backBuffer.width;
                copy.height = a_backBuffer.height;
                copy.mipLevels = 1;
                copy.arraySize = 1;
                copy.format = a_backBuffer.format;
                copy.sampleDesc.count = 1;
                copy.usage = REX::W32::D3D11_USAGE_DEFAULT;
                copy.bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE;
                if (a_device.CreateTexture2D(&copy, nullptr, textures.copy.GetAddressOf()) < 0
                    || a_device.CreateShaderResourceView(textures.copy.Get(), nullptr, textures.copyView.GetAddressOf())
                        < 0)
                {
                    textures.copy = nullptr;
                    textures.copyView = nullptr;
                    return false;
                }
            }
            const auto width = (a_backBuffer.width + kDownscale - 1) / kDownscale;
            const auto height = (a_backBuffer.height + kDownscale - 1) / kDownscale;
            return Gpu::EnsureRenderTarget(
                       a_device, textures.reduced, width, height, REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM)
                && Gpu::EnsureRenderTarget(
                    a_device, textures.blurred, width, height, REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM);
        }

        // Leaves the blurred frame in textures.reduced.
        bool BlurFrame()
        {
            PROFILE_SCOPE("Backdrop");
            const auto frame = Gpu::GetFrame();
            if (!frame)
            {
                return false;
            }
            if (frame->uiLayer && !std::exchange(uiLayerLogged, true))
            {
                logs::debug("UI backdrop: the frame target is a UI layer another plugin composites over the scene; {}",
                    frame->scene.Get() ? "blurring the swap chain's buffer" : "the scene is not readable, no blur");
            }
            if (!frame->scene.Get())
            {
                return false;
            }
            const auto& description = frame->sceneDescription;
            if (description.sampleDesc.count != 1)
            {
                return Disable("the back buffer is multisampled");
            }
            if (!EnsureShaders(frame->device))
            {
                return Disable("its shaders could not be created");
            }
            if (!EnsureTextures(frame->device, description))
            {
                return Disable(
                    std::format("its textures could not be created: format={}", static_cast<int>(description.format)));
            }
            frame->context.CopyResource(textures.copy.Get(), frame->scene.Get());
            Gpu::PassScope passes{ *frame };
            if (!passes.IsReady())
            {
                return Disable("its pipeline states could not be created");
            }
            auto& reduced = textures.reduced;
            auto& blurred = textures.blurred;
            const std::array copy{ textures.copyView.Get() };
            passes.Draw(*shaders.downsample.Get(), copy, reduced, reduced.width, reduced.height);
            const std::array reducedView{ reduced.view.Get() };
            passes.Draw(*shaders.horizontal.Get(), reducedView, blurred, blurred.width, blurred.height);
            const std::array blurredView{ blurred.view.Get() };
            passes.Draw(*shaders.vertical.Get(), blurredView, reduced, reduced.width, reduced.height);
            return true;
        }
    }

    void Draw()
    {
        const int frame = ImGui::GetFrameCount();
        if (std::exchange(drawnFrame, frame) == frame)
        {
            return;
        }
        auto& drawList = *ImGui::GetBackgroundDrawList();
        const ImVec2 origin{ 0.0F, 0.0F };
        const auto& display = ImGui::GetIO().DisplaySize;
        if (const auto blur = GetBlur())
        {
            drawList.AddImage(blur->texture, origin, display, blur->ToUv(origin), blur->ToUv(display), blur->tint);
            return;
        }
        drawList.AddRectFilled(origin, display, kDim);
    }

    std::optional<Blur> GetBlur()
    {
        requested = true;
        if (unavailable)
        {
            return std::nullopt;
        }
        const int frame = ImGui::GetFrameCount();
        if (std::exchange(blurredFrame, frame) != frame)
        {
            frameBlurred = BlurFrame();
        }
        if (!frameBlurred)
        {
            return std::nullopt;
        }
        const auto& reduced = textures.reduced;
        const auto& display = ImGui::GetIO().DisplaySize;
        const auto texture = static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(reduced.view.Get()));
        // Half a texel in, so filtering never wraps to the opposite edge.
        const ImVec2 inset{ 0.5F / static_cast<float>(reduced.width), 0.5F / static_cast<float>(reduced.height) };
        return Blur{ .texture = ImTextureRef{ texture },
            .tint = drawnFrame == frame ? kTint : IM_COL32_WHITE,
            .uvOrigin = inset,
            .uvPerPixel = ImVec2{ (1.0F - 2.0F * inset.x) / display.x, (1.0F - 2.0F * inset.y) / display.y } };
    }

    void DrawBlur(ImDrawList& a_list, const ImVec2& a_min, const ImVec2& a_max, float a_rounding, float a_alpha)
    {
        const auto blur = GetBlur();
        if (!blur || a_alpha <= 0.0F)
        {
            return;
        }
        auto tint = ImGui::ColorConvertU32ToFloat4(blur->tint);
        tint.w *= a_alpha;
        a_list.AddImageRounded(blur->texture, a_min, a_max, blur->ToUv(a_min), blur->ToUv(a_max),
            ImGui::ColorConvertFloat4ToU32(tint), a_rounding);
    }

    void BlurPopups(ImDrawData& a_drawData)
    {
        if (Theme::GetBackgroundOpacity() >= 1.0F)
        {
            return;
        }
        constexpr ImGuiWindowFlags kBlurred = ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip;
        constexpr ImGuiWindowFlags kSkipped = ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_NoBackground;
        std::optional<Blur> blur;
        std::size_t used = 0;
        for (auto* const window : GImGui->Windows)
        {
            if (!window->Active || window->Hidden || (window->Flags & kBlurred) == 0 || (window->Flags & kSkipped) != 0)
            {
                continue;
            }
            auto* const position = std::find(a_drawData.CmdLists.begin(), a_drawData.CmdLists.end(), window->DrawList);
            if (position == a_drawData.CmdLists.end())
            {
                continue;
            }
            if (!blur && !(blur = GetBlur()))
            {
                return;
            }
            if (used == popupLists.size())
            {
                popupLists.push_back(std::make_unique<ImDrawList>(ImGui::GetDrawListSharedData()));
            }
            auto& list = *popupLists[used++];
            list._ResetForNewFrame();
            list.PushClipRectFullScreen();
            const ImVec2 max{ window->Pos.x + window->Size.x, window->Pos.y + window->Size.y };
            list.AddImageRounded(blur->texture, window->Pos, max, blur->ToUv(window->Pos), blur->ToUv(max), blur->tint,
                window->WindowRounding);
            list._PopUnusedDrawCmd();
            // Drawn just before the popup's own list, so over every window under it.
            a_drawData.CmdLists.insert(position, &list);
            a_drawData.TotalVtxCount += list.VtxBuffer.Size;
            a_drawData.TotalIdxCount += list.IdxBuffer.Size;
        }
    }

    void Update()
    {
        if (!std::exchange(requested, false) && textures.copy.Get())
        {
            textures = {};
        }
    }
}
