#include "ui/render/Gpu.h"

#include <REX/W32/D3DCOMPILER.h>

#include <string>

namespace UI::Gpu
{
    namespace
    {
        constexpr std::string_view kVertexSource = R"(
struct Vertex
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};
)";

        // One triangle covering the viewport, from SV_VertexID alone, so no input layout is bound.
        constexpr std::string_view kFullScreenSource = R"(
Vertex FullScreen(uint id : SV_VertexID)
{
    Vertex output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
)";

        // D3DCOMPILE_OPTIMIZATION_LEVEL3, a macro once <d3dcompiler.h> is included.
        constexpr std::uint32_t kOptimizationLevel3 = 1U << 15;

        struct SharedState
        {
            bool created{ false };
            bool failed{ false };
            bool shaderFailed{ false };
            ComPtr<REX::W32::ID3D11VertexShader> vertexShader;
            ComPtr<REX::W32::ID3D11SamplerState> sampler;
            ComPtr<REX::W32::ID3D11RasterizerState> rasterizer;
            ComPtr<REX::W32::ID3D11DepthStencilState> depthState;
            ComPtr<REX::W32::ID3D11BlendState> opaque;
            ComPtr<REX::W32::ID3D11BlendState> premultiplied;
        };

        SharedState shared;

        ComPtr<REX::W32::ID3DBlob> Compile(std::string_view a_source, const char* a_entryPoint, const char* a_target)
        {
            std::string source{ kVertexSource };
            source += a_source;
            ComPtr<REX::W32::ID3DBlob> code;
            ComPtr<REX::W32::ID3DBlob> errors;
            if (REX::W32::D3DCompile(source.data(), source.size(), a_entryPoint, nullptr, nullptr, a_entryPoint,
                    a_target, kOptimizationLevel3, 0, code.GetAddressOf(), errors.GetAddressOf())
                < 0)
            {
                const std::string_view message = errors.Get()
                    ? std::string_view{ static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize() }
                    : std::string_view{ "no compiler output" };
                logs::warn("UI overlay: shader {} did not compile: {}", a_entryPoint, message);
                return {};
            }
            return code;
        }

        REX::W32::D3D11_RENDER_TARGET_BLEND_DESC MakeBlend(bool a_enable, REX::W32::D3D11_BLEND a_source)
        {
            return REX::W32::D3D11_RENDER_TARGET_BLEND_DESC{ .blendEnable = a_enable,
                .srcBlend = a_source,
                .destBlend = a_enable ? REX::W32::D3D11_BLEND_INV_SRC_ALPHA : REX::W32::D3D11_BLEND_ZERO,
                .blendOp = REX::W32::D3D11_BLEND_OP_ADD,
                .srcBlendAlpha = REX::W32::D3D11_BLEND_ONE,
                .destBlendAlpha = a_enable ? REX::W32::D3D11_BLEND_INV_SRC_ALPHA : REX::W32::D3D11_BLEND_ZERO,
                .blendOpAlpha = REX::W32::D3D11_BLEND_OP_ADD,
                .renderTargetWriteMask = REX::W32::D3D11_COLOR_WRITE_ENABLE_ALL };
        }

        bool CreateStates(REX::W32::ID3D11Device& a_device)
        {
            if (shared.created || shared.failed)
            {
                return shared.created;
            }
            const REX::W32::D3D11_SAMPLER_DESC sampler{ .filter = REX::W32::D3D11_FILTER_MIN_MAG_MIP_LINEAR,
                .addressU = REX::W32::D3D11_TEXTURE_ADDRESS_CLAMP,
                .addressV = REX::W32::D3D11_TEXTURE_ADDRESS_CLAMP,
                .addressW = REX::W32::D3D11_TEXTURE_ADDRESS_CLAMP,
                .mipLODBias = 0.0F,
                .maxAnisotropy = 1,
                .comparisonFunc = REX::W32::D3D11_COMPARISON_ALWAYS,
                .borderColor = { 0.0F, 0.0F, 0.0F, 0.0F },
                .minLOD = 0.0F,
                .maxLOD = 0.0F };
            const REX::W32::D3D11_RASTERIZER_DESC rasterizer{ .fillMode = REX::W32::D3D11_FILL_SOLID,
                .cullMode = REX::W32::D3D11_CULL_NONE,
                .frontCounterClockwise = false,
                .depthBias = 0,
                .depthBiasClamp = 0.0F,
                .slopeScaledDepthBias = 0.0F,
                .depthClipEnable = true,
                .scissorEnable = false,
                .multisampleEnable = false,
                .antialiasedLineEnable = false };
            REX::W32::D3D11_DEPTH_STENCIL_DESC depth{};
            depth.depthEnable = false;
            depth.depthWriteMask = REX::W32::D3D11_DEPTH_WRITE_MASK_ZERO;
            depth.depthFunc = REX::W32::D3D11_COMPARISON_ALWAYS;
            depth.stencilEnable = false;
            REX::W32::D3D11_BLEND_DESC opaque{};
            opaque.renderTarget[0] = MakeBlend(false, REX::W32::D3D11_BLEND_ONE);
            REX::W32::D3D11_BLEND_DESC premultiplied{};
            premultiplied.renderTarget[0] = MakeBlend(true, REX::W32::D3D11_BLEND_ONE);
            if (a_device.CreateSamplerState(&sampler, shared.sampler.GetAddressOf()) < 0
                || a_device.CreateRasterizerState(&rasterizer, shared.rasterizer.GetAddressOf()) < 0
                || a_device.CreateDepthStencilState(&depth, shared.depthState.GetAddressOf()) < 0
                || a_device.CreateBlendState(&opaque, shared.opaque.GetAddressOf()) < 0
                || a_device.CreateBlendState(&premultiplied, shared.premultiplied.GetAddressOf()) < 0)
            {
                logs::warn("UI overlay: pipeline states could not be created; images and passes are unavailable");
                shared = SharedState{ .failed = true };
                return false;
            }
            shared.created = true;
            return true;
        }

        bool CreateVertexShader(REX::W32::ID3D11Device& a_device)
        {
            if (shared.vertexShader.Get() || shared.shaderFailed)
            {
                return shared.vertexShader.Get() != nullptr;
            }
            const auto code = Compile(kFullScreenSource, "FullScreen", "vs_4_0");
            if (!code.Get()
                || a_device.CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                       shared.vertexShader.GetAddressOf())
                    < 0)
            {
                logs::warn("UI overlay: the full-screen vertex shader could not be created; passes are unavailable");
                shared.vertexShader = nullptr;
                shared.shaderFailed = true;
                return false;
            }
            return true;
        }

        template <class T>
        void Release(T*& a_object)
        {
            if (a_object)
            {
                a_object->Release();
                a_object = nullptr;
            }
        }

        // Run inside ImGui_ImplDX11_RenderDrawData, on the device context the overlay shares
        // with the renderer.
        void SetBlend(REX::W32::ID3D11BlendState* a_state)
        {
            auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
            auto* context = renderer ? renderer->GetRuntimeData().context : nullptr;
            if (context && a_state)
            {
                context->OMSetBlendState(a_state, nullptr, 0xFFFFFFFF);
            }
        }

        void SetOpaqueBlend(const ImDrawList*, const ImDrawCmd*)
        {
            SetBlend(shared.opaque.Get());
        }

        void SetPremultipliedBlend(const ImDrawList*, const ImDrawCmd*)
        {
            SetBlend(shared.premultiplied.Get());
        }
    }

    std::optional<Frame> GetFrame()
    {
        auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
        if (!renderer)
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
        Frame frame{ .device = *data.forwarder, .context = *data.context, .target = *target, .backBuffer = backBuffer };
        backBuffer->GetDesc(&frame.description);
        return frame;
    }

    bool EnsureRenderTarget(REX::W32::ID3D11Device& a_device, RenderTarget& a_target, std::uint32_t a_width,
        std::uint32_t a_height, REX::W32::DXGI_FORMAT a_format)
    {
        if (a_target.texture.Get() && a_target.width == a_width && a_target.height == a_height
            && a_target.format == a_format)
        {
            return true;
        }
        a_target = {};
        REX::W32::D3D11_TEXTURE2D_DESC description{};
        description.width = a_width;
        description.height = a_height;
        description.mipLevels = 1;
        description.arraySize = 1;
        description.format = a_format;
        description.sampleDesc.count = 1;
        description.usage = REX::W32::D3D11_USAGE_DEFAULT;
        description.bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE | REX::W32::D3D11_BIND_RENDER_TARGET;
        if (a_device.CreateTexture2D(&description, nullptr, a_target.texture.GetAddressOf()) < 0
            || a_device.CreateShaderResourceView(a_target.texture.Get(), nullptr, a_target.view.GetAddressOf()) < 0
            || a_device.CreateRenderTargetView(a_target.texture.Get(), nullptr, a_target.target.GetAddressOf()) < 0)
        {
            a_target = {};
            return false;
        }
        a_target.width = a_width;
        a_target.height = a_height;
        a_target.format = a_format;
        return true;
    }

    ComPtr<REX::W32::ID3D11PixelShader> CompilePixelShader(
        REX::W32::ID3D11Device& a_device, std::string_view a_source, const char* a_entryPoint)
    {
        ComPtr<REX::W32::ID3D11PixelShader> shader;
        const auto code = Compile(a_source, a_entryPoint, "ps_4_0");
        if (code.Get()
            && a_device.CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                   shader.GetAddressOf())
                < 0)
        {
            logs::warn("UI overlay: shader {} could not be created", a_entryPoint);
            shader = nullptr;
        }
        return shader;
    }

    PassScope::PassScope(const Frame& a_frame)
        : context(a_frame.context)
    {
        context.IAGetInputLayout(&inputLayout);
        context.IAGetPrimitiveTopology(&topology);
        context.VSGetShader(&vertexShader, nullptr, nullptr);
        context.HSGetShader(&hullShader, nullptr, nullptr);
        context.DSGetShader(&domainShader, nullptr, nullptr);
        context.GSGetShader(&geometryShader, nullptr, nullptr);
        context.PSGetShader(&pixelShader, nullptr, nullptr);
        context.PSGetShaderResources(0, kSourceSlots, sources);
        context.PSGetSamplers(0, 1, &sampler);
        context.RSGetState(&rasterizer);
        context.RSGetViewports(&viewportCount, viewports);
        context.OMGetRenderTargets(1, &target, &depthStencil);
        context.OMGetBlendState(&blend, blendFactor, &sampleMask);
        context.OMGetDepthStencilState(&depthState, &stencilRef);

        ready = CreateStates(a_frame.device) && CreateVertexShader(a_frame.device);
        if (!ready)
        {
            return;
        }
        context.IASetInputLayout(nullptr);
        context.IASetPrimitiveTopology(REX::W32::D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context.VSSetShader(shared.vertexShader.Get(), nullptr, 0);
        context.HSSetShader(nullptr, nullptr, 0);
        context.DSSetShader(nullptr, nullptr, 0);
        context.GSSetShader(nullptr, nullptr, 0);
        auto* passSampler = shared.sampler.Get();
        context.PSSetSamplers(0, 1, &passSampler);
        context.RSSetState(shared.rasterizer.Get());
        context.OMSetBlendState(shared.opaque.Get(), nullptr, 0xFFFFFFFF);
        context.OMSetDepthStencilState(shared.depthState.Get(), 0);
    }

    PassScope::~PassScope()
    {
        // A pass's target must leave the shader slots before it can be bound as a source again.
        REX::W32::ID3D11ShaderResourceView* const none[kSourceSlots]{};
        context.PSSetShaderResources(0, kSourceSlots, none);
        context.OMSetRenderTargets(1, &target, depthStencil);
        context.IASetInputLayout(inputLayout);
        context.IASetPrimitiveTopology(topology);
        context.VSSetShader(vertexShader, nullptr, 0);
        context.HSSetShader(hullShader, nullptr, 0);
        context.DSSetShader(domainShader, nullptr, 0);
        context.GSSetShader(geometryShader, nullptr, 0);
        context.PSSetShader(pixelShader, nullptr, 0);
        context.PSSetShaderResources(0, kSourceSlots, sources);
        context.PSSetSamplers(0, 1, &sampler);
        context.RSSetState(rasterizer);
        context.RSSetViewports(viewportCount, viewports);
        context.OMSetBlendState(blend, blendFactor, sampleMask);
        context.OMSetDepthStencilState(depthState, stencilRef);

        Release(inputLayout);
        Release(vertexShader);
        Release(hullShader);
        Release(domainShader);
        Release(geometryShader);
        Release(pixelShader);
        for (auto*& source : sources)
        {
            Release(source);
        }
        Release(sampler);
        Release(rasterizer);
        Release(target);
        Release(depthStencil);
        Release(blend);
        Release(depthState);
    }

    void PassScope::Draw(REX::W32::ID3D11PixelShader& a_shader,
        std::span<REX::W32::ID3D11ShaderResourceView* const> a_sources, RenderTarget& a_target, std::uint32_t a_width,
        std::uint32_t a_height)
    {
        if (!ready || a_sources.size() > kSourceSlots || !a_target.target.Get())
        {
            return;
        }
        REX::W32::ID3D11ShaderResourceView* const none[kSourceSlots]{};
        context.PSSetShaderResources(0, kSourceSlots, none);
        auto* passTarget = a_target.target.Get();
        context.OMSetRenderTargets(1, &passTarget, nullptr);
        context.PSSetShaderResources(0, static_cast<std::uint32_t>(a_sources.size()), a_sources.data());
        context.PSSetShader(&a_shader, nullptr, 0);
        const REX::W32::D3D11_VIEWPORT viewport{ .topLeftX = 0.0F,
            .topLeftY = 0.0F,
            .width = static_cast<float>(a_width),
            .height = static_cast<float>(a_height),
            .minDepth = 0.0F,
            .maxDepth = 1.0F };
        context.RSSetViewports(1, &viewport);
        context.Draw(3, 0);
    }

    bool AddImage(ImDrawList& a_drawList, REX::W32::ID3D11ShaderResourceView& a_view, const ImVec2& a_min,
        const ImVec2& a_max, const ImVec2& a_uvMin, const ImVec2& a_uvMax, Blend a_blend)
    {
        auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
        auto* device = renderer ? renderer->GetRuntimeData().forwarder : nullptr;
        if (!device || !CreateStates(*device))
        {
            return false;
        }
        const auto texture = static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(&a_view));
        a_drawList.AddCallback(a_blend == Blend::kOpaque ? SetOpaqueBlend : SetPremultipliedBlend, nullptr);
        a_drawList.AddImage(ImTextureRef{ texture }, a_min, a_max, a_uvMin, a_uvMax);
        a_drawList.AddCallback(ImDrawCallback_ResetRenderState, nullptr);
        return true;
    }
}
