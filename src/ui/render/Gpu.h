#pragma once

#include <REX/W32/COMPTR.h>
#include <REX/W32/D3D11.h>
#include <imgui.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace UI::Gpu
{
    using REX::W32::ComPtr;

    struct Frame
    {
        REX::W32::ID3D11Device& device;
        REX::W32::ID3D11DeviceContext& context;
        REX::W32::ID3D11RenderTargetView& target;
        ComPtr<REX::W32::ID3D11Texture2D> backBuffer;
        REX::W32::D3D11_TEXTURE2D_DESC description{};
    };

    // The renderer's device and context and the back buffer the overlay draws on.
    [[nodiscard]] std::optional<Frame> GetFrame();

    struct RenderTarget
    {
        ComPtr<REX::W32::ID3D11Texture2D> texture;
        ComPtr<REX::W32::ID3D11ShaderResourceView> view;
        ComPtr<REX::W32::ID3D11RenderTargetView> target;
        std::uint32_t width{ 0 };
        std::uint32_t height{ 0 };
        REX::W32::DXGI_FORMAT format{};
    };

    // Keeps a_target as is when it already has this size and format.
    [[nodiscard]] bool EnsureRenderTarget(REX::W32::ID3D11Device& a_device, RenderTarget& a_target,
        std::uint32_t a_width, std::uint32_t a_height, REX::W32::DXGI_FORMAT a_format);

    // a_source is HLSL whose entry point takes the full-screen triangle's `Vertex` (position in
    // target pixels, uv over the target) and returns the color. Logs why it failed.
    [[nodiscard]] ComPtr<REX::W32::ID3D11PixelShader> CompilePixelShader(
        REX::W32::ID3D11Device& a_device, std::string_view a_source, const char* a_entryPoint);

    // Draws full-screen passes with blending off. The engine tracks what it bound, so the scope
    // puts back every pipeline state a pass changes when it ends.
    class PassScope
    {
    public:
        explicit PassScope(const Frame& a_frame);
        ~PassScope();
        PassScope(const PassScope&) = delete;
        PassScope& operator=(const PassScope&) = delete;

        [[nodiscard]] bool IsReady() const { return ready; }

        // Covers a_width x a_height pixels of the target from its top left corner, sampling
        // a_sources in slots t0 onwards with a linear clamped sampler in s0.
        void Draw(REX::W32::ID3D11PixelShader& a_shader,
            std::span<REX::W32::ID3D11ShaderResourceView* const> a_sources, RenderTarget& a_target,
            std::uint32_t a_width, std::uint32_t a_height);

    private:
        static constexpr std::uint32_t kSourceSlots = 2;
        // D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE, a macro once <d3d11.h> is included.
        static constexpr std::uint32_t kMaximumViewports = 16;

        REX::W32::ID3D11DeviceContext& context;
        bool ready{ false };
        REX::W32::ID3D11InputLayout* inputLayout{ nullptr };
        REX::W32::D3D11_PRIMITIVE_TOPOLOGY topology{};
        REX::W32::ID3D11VertexShader* vertexShader{ nullptr };
        REX::W32::ID3D11HullShader* hullShader{ nullptr };
        REX::W32::ID3D11DomainShader* domainShader{ nullptr };
        REX::W32::ID3D11GeometryShader* geometryShader{ nullptr };
        REX::W32::ID3D11PixelShader* pixelShader{ nullptr };
        REX::W32::ID3D11ShaderResourceView* sources[kSourceSlots]{};
        REX::W32::ID3D11SamplerState* sampler{ nullptr };
        REX::W32::ID3D11RasterizerState* rasterizer{ nullptr };
        std::uint32_t viewportCount{ kMaximumViewports };
        REX::W32::D3D11_VIEWPORT viewports[kMaximumViewports]{};
        REX::W32::ID3D11RenderTargetView* target{ nullptr };
        REX::W32::ID3D11DepthStencilView* depthStencil{ nullptr };
        REX::W32::ID3D11BlendState* blend{ nullptr };
        float blendFactor[4]{};
        std::uint32_t sampleMask{ 0 };
        REX::W32::ID3D11DepthStencilState* depthState{ nullptr };
        std::uint32_t stencilRef{ 0 };
    };

    enum class Blend : std::uint8_t
    {
        // The image replaces what is under it, for captures whose alpha is not coverage.
        kOpaque,
        // The image's color is already multiplied by its alpha.
        kPremultiplied
    };

    // Returns false, drawing nothing, when the blend states could not be created.
    bool AddImage(ImDrawList& a_drawList, REX::W32::ID3D11ShaderResourceView& a_view, const ImVec2& a_min,
        const ImVec2& a_max, const ImVec2& a_uvMin, const ImVec2& a_uvMax, Blend a_blend);
}
