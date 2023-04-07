// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "BackendD3D.h"

#include <custom_shader_ps.h>
#include <custom_shader_vs.h>
#include <shader_ps.h>
#include <shader_vs.h>

#include "dwrite.h"

#if ATLAS_DEBUG_SHOW_DIRTY || ATLAS_DEBUG_COLORIZE_GLYPH_ATLAS
#include "colorbrewer.h"
#endif

#if ATLAS_DEBUG_DUMP_RENDER_TARGET
#include "wic.h"
#endif

TIL_FAST_MATH_BEGIN

// This code packs various data into smaller-than-int types to save both CPU and GPU memory. This warning would force
// us to add dozens upon dozens of gsl::narrow_cast<>s throughout the file which is more annoying than helpful.
#pragma warning(disable : 4242) // '=': conversion from '...' to '...', possible loss of data
#pragma warning(disable : 4244) // 'initializing': conversion from '...' to '...', possible loss of data
#pragma warning(disable : 4267) // 'argument': conversion from '...' to '...', possible loss of data
#pragma warning(disable : 4838) // conversion from '...' to '...' requires a narrowing conversion
#pragma warning(disable : 26472) // Don't use a static_cast for arithmetic conversions. Use brace initialization, gsl::narrow_cast or gsl::narrow (type.1).
// Disable a bunch of warnings which get in the way of writing performant code.
#pragma warning(disable : 26429) // Symbol 'data' is never tested for nullness, it can be marked as not_null (f.23).
#pragma warning(disable : 26446) // Prefer to use gsl::at() instead of unchecked subscript operator (bounds.4).
#pragma warning(disable : 26459) // You called an STL function '...' with a raw pointer parameter at position '...' that may be unsafe [...].
#pragma warning(disable : 26481) // Don't use pointer arithmetic. Use span instead (bounds.1).
#pragma warning(disable : 26482) // Only index into arrays using constant expressions (bounds.2).

using namespace Microsoft::Console::Render::Atlas;

template<>
struct ::std::hash<BackendD3D::AtlasGlyphEntry>
{
    constexpr size_t operator()(u16 key) const noexcept
    {
        return til::flat_set_hash_integer(key);
    }

    constexpr size_t operator()(const BackendD3D::AtlasGlyphEntry& slot) const noexcept
    {
        return til::flat_set_hash_integer(slot.glyphIndex);
    }
};

template<>
struct ::std::hash<BackendD3D::AtlasFontFaceEntry>
{
    using T = BackendD3D::AtlasFontFaceEntry;

    size_t operator()(const BackendD3D::AtlasFontFaceKey& key) const noexcept
    {
        return til::flat_set_hash_integer(std::bit_cast<uintptr_t>(key.fontFace) | static_cast<u8>(key.lineRendition));
    }

    size_t operator()(const BackendD3D::AtlasFontFaceEntry& slot) const noexcept
    {
        const auto& inner = *slot.inner;
        return til::flat_set_hash_integer(std::bit_cast<uintptr_t>(inner.fontFace.get()) | static_cast<u8>(inner.lineRendition));
    }
};

BackendD3D::BackendD3D(wil::com_ptr<ID3D11Device2> device, wil::com_ptr<ID3D11DeviceContext2> deviceContext) :
    _device{ std::move(device) },
    _deviceContext{ std::move(deviceContext) }
{
    THROW_IF_FAILED(_device->CreateVertexShader(&shader_vs[0], sizeof(shader_vs), nullptr, _vertexShader.addressof()));
    THROW_IF_FAILED(_device->CreatePixelShader(&shader_ps[0], sizeof(shader_ps), nullptr, _pixelShader.addressof()));

    {
        static constexpr D3D11_INPUT_ELEMENT_DESC layout[]{
            { "SV_Position", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "shadingType", 0, DXGI_FORMAT_R32_UINT, 1, offsetof(QuadInstance, shadingType), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            { "position", 0, DXGI_FORMAT_R16G16_SINT, 1, offsetof(QuadInstance, position), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            { "size", 0, DXGI_FORMAT_R16G16_UINT, 1, offsetof(QuadInstance, size), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            { "texcoord", 0, DXGI_FORMAT_R16G16_UINT, 1, offsetof(QuadInstance, texcoord), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            { "color", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, offsetof(QuadInstance, color), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        };
        THROW_IF_FAILED(_device->CreateInputLayout(&layout[0], std::size(layout), &shader_vs[0], sizeof(shader_vs), _inputLayout.addressof()));
    }

    {
        static constexpr f32x2 vertices[]{
            { 0, 0 },
            { 1, 0 },
            { 1, 1 },
            { 0, 1 },
        };
        static constexpr D3D11_SUBRESOURCE_DATA initialData{ &vertices[0] };

        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = sizeof(vertices);
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        THROW_IF_FAILED(_device->CreateBuffer(&desc, &initialData, _vertexBuffer.addressof()));
    }

    {
        static constexpr u16 indices[]{
            0, // { 0, 0 }
            1, // { 1, 0 }
            2, // { 1, 1 }
            2, // { 1, 1 }
            3, // { 0, 1 }
            0, // { 0, 0 }
        };
        static constexpr D3D11_SUBRESOURCE_DATA initialData{ &indices[0] };

        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = sizeof(indices);
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        THROW_IF_FAILED(_device->CreateBuffer(&desc, &initialData, _indexBuffer.addressof()));
    }

    {
        static constexpr D3D11_BUFFER_DESC desc{
            .ByteWidth = sizeof(VSConstBuffer),
            .Usage = D3D11_USAGE_DEFAULT,
            .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
        };
        THROW_IF_FAILED(_device->CreateBuffer(&desc, nullptr, _vsConstantBuffer.addressof()));
    }

    {
        static constexpr D3D11_BUFFER_DESC desc{
            .ByteWidth = sizeof(PSConstBuffer),
            .Usage = D3D11_USAGE_DEFAULT,
            .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
        };
        THROW_IF_FAILED(_device->CreateBuffer(&desc, nullptr, _psConstantBuffer.addressof()));
    }

    {
        // The final step of the ClearType blending algorithm is a lerp() between the premultiplied alpha
        // background color and straight alpha foreground color given the 3 RGB weights in alphaCorrected:
        //   lerp(background, foreground, weights)
        // Which is equivalent to:
        //   background * (1 - weights) + foreground * weights
        //
        // This COULD be implemented using dual source color blending like so:
        //   .SrcBlend = D3D11_BLEND_SRC1_COLOR
        //   .DestBlend = D3D11_BLEND_INV_SRC1_COLOR
        //   .BlendOp = D3D11_BLEND_OP_ADD
        // Because:
        //   background * (1 - weights) + foreground * weights
        //       ^             ^        ^     ^           ^
        //      Dest     INV_SRC1_COLOR |    Src      SRC1_COLOR
        //                            OP_ADD
        //
        // BUT we need simultaneous support for regular "source over" alpha blending
        // (SHADING_TYPE_PASSTHROUGH)  like this:
        //   background * (1 - alpha) + foreground
        //
        // This is why we set:
        //   .SrcBlend = D3D11_BLEND_ONE
        //
        // --> We need to multiply the foreground with the weights ourselves.
        static constexpr D3D11_BLEND_DESC desc{
            .RenderTarget = { {
                .BlendEnable = TRUE,
                .SrcBlend = D3D11_BLEND_ONE,
                .DestBlend = D3D11_BLEND_INV_SRC1_COLOR,
                .BlendOp = D3D11_BLEND_OP_ADD,
                .SrcBlendAlpha = D3D11_BLEND_ONE,
                .DestBlendAlpha = D3D11_BLEND_INV_SRC1_ALPHA,
                .BlendOpAlpha = D3D11_BLEND_OP_ADD,
                .RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL,
            } },
        };
        THROW_IF_FAILED(_device->CreateBlendState(&desc, _blendState.addressof()));
    }

    {
        static constexpr D3D11_BLEND_DESC desc{
            .RenderTarget = { {
                .BlendEnable = TRUE,
                .SrcBlend = D3D11_BLEND_ONE,
                .DestBlend = D3D11_BLEND_ONE,
                .BlendOp = D3D11_BLEND_OP_SUBTRACT,
                // In order for D3D to be okay with us using dual source blending in the shader, we need to use dual
                // source blending in the blend state. Alternatively we could write an extra shader for these cursors.
                .SrcBlendAlpha = D3D11_BLEND_SRC1_ALPHA,
                .DestBlendAlpha = D3D11_BLEND_ZERO,
                .BlendOpAlpha = D3D11_BLEND_OP_ADD,
                .RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL,
            } },
        };
        THROW_IF_FAILED(_device->CreateBlendState(&desc, _blendStateInvert.addressof()));
    }

#ifndef NDEBUG
    _sourceDirectory = std::filesystem::path{ __FILE__ }.parent_path();
    _sourceCodeWatcher = wil::make_folder_change_reader_nothrow(_sourceDirectory.c_str(), false, wil::FolderChangeEvents::FileName | wil::FolderChangeEvents::LastWriteTime, [this](wil::FolderChangeEvent, PCWSTR path) {
        if (til::ends_with(path, L".hlsl"))
        {
            auto expected = INT64_MAX;
            const auto invalidationTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            _sourceCodeInvalidationTime.compare_exchange_strong(expected, invalidationTime.time_since_epoch().count(), std::memory_order_relaxed);
        }
    });
#endif
}

void BackendD3D::Render(RenderingPayload& p)
{
    if (_generation != p.s.generation())
    {
        _handleSettingsUpdate(p);
    }

#ifndef NDEBUG
    _debugUpdateShaders(p);
#endif

    // After a Present() the render target becomes unbound.
    _deviceContext->OMSetRenderTargets(1, _renderTargetView.addressof(), nullptr);

    // Invalidating the render target helps with spotting invalid quad instances and Present1() bugs.
#if ATLAS_DEBUG_SHOW_DIRTY || ATLAS_DEBUG_DUMP_RENDER_TARGET
    {
        static constexpr f32 clearColor[4]{};
        _deviceContext->ClearView(_renderTargetView.get(), &clearColor[0], nullptr, 0);
    }
#endif

    _drawBackground(p);
    _drawCursorPart1(p);
    _drawText(p);
    _drawGridlines(p);
    _drawCursorPart2(p);
    _drawSelection(p);
#if ATLAS_DEBUG_SHOW_DIRTY
    _debugShowDirty(p);
#endif
    _flushQuads(p);

    if (_customPixelShader)
    {
        _executeCustomShader(p);
    }

#if ATLAS_DEBUG_DUMP_RENDER_TARGET
    _debugDumpRenderTarget(p);
#endif
    _swapChainManager.Present(p);
}

bool BackendD3D::RequiresContinuousRedraw() noexcept
{
    return _requiresContinuousRedraw;
}

void BackendD3D::WaitUntilCanRender() noexcept
{
    _swapChainManager.WaitUntilCanRender();
}

void BackendD3D::_handleSettingsUpdate(const RenderingPayload& p)
{
    _swapChainManager.UpdateSwapChainSettings(
        p,
        _device.get(),
        [this]() {
            _renderTargetView.reset();
            _customRenderTargetView.reset();
            _deviceContext->ClearState();
            _deviceContext->Flush();
        },
        [this]() {
            _renderTargetView.reset();
            _customRenderTargetView.reset();
            _deviceContext->ClearState();
        });

    if (!_renderTargetView)
    {
        const auto buffer = _swapChainManager.GetBuffer();
        THROW_IF_FAILED(_device->CreateRenderTargetView(buffer.get(), nullptr, _renderTargetView.put()));
    }

    const auto fontChanged = _fontGeneration != p.s->font.generation();
    const auto miscChanged = _miscGeneration != p.s->misc.generation();
    const auto cellCountChanged = _cellCount != p.s->cellCount;

    if (fontChanged)
    {
        _updateFontDependents(p);
    }
    if (miscChanged)
    {
        _recreateCustomShader(p);
    }
    if (cellCountChanged)
    {
        _recreateColorBitmap(p.s->cellCount);
    }

    // Similar to _renderTargetView above, we might have to recreate the _customRenderTargetView whenever _swapChainManager
    // resets it. We only do it after calling _recreateCustomShader however, since that sets the _customPixelShader.
    if (_customPixelShader && !_customRenderTargetView)
    {
        _recreateCustomRenderTargetView(p.s->targetSize);
    }

    _recreateConstBuffer(p);
    _setupDeviceContextState(p);

    _generation = p.s.generation();
    _fontGeneration = p.s->font.generation();
    _miscGeneration = p.s->misc.generation();
    _targetSize = p.s->targetSize;
    _cellCount = p.s->cellCount;
}

void BackendD3D::_updateFontDependents(const RenderingPayload& p)
{
    DWrite_GetRenderParams(p.dwriteFactory.get(), &_gamma, &_cleartypeEnhancedContrast, &_grayscaleEnhancedContrast, _textRenderingParams.put());
    // Clearing the atlas requires BeginDraw(), which is expensive. Defer this until we need Direct2D anyways.
    _fontChangedResetGlyphAtlas = true;
    _textShadingType = p.s->font->antialiasingMode == AntialiasingMode::ClearType ? ShadingType::TextClearType : ShadingType::TextGrayscale;

    if (_d2dRenderTarget)
    {
        _d2dRenderTargetUpdateFontSettings(*p.s->font);
    }

    _softFontBitmap.reset();
}

void BackendD3D::_recreateCustomShader(const RenderingPayload& p)
{
    _customRenderTargetView.reset();
    _customOffscreenTexture.reset();
    _customOffscreenTextureView.reset();
    _customVertexShader.reset();
    _customPixelShader.reset();
    _customShaderConstantBuffer.reset();
    _customShaderSamplerState.reset();
    _requiresContinuousRedraw = false;

    if (!p.s->misc->customPixelShaderPath.empty())
    {
        const char* target = nullptr;
        switch (_device->GetFeatureLevel())
        {
        case D3D_FEATURE_LEVEL_10_0:
            target = "ps_4_0";
            break;
        case D3D_FEATURE_LEVEL_10_1:
            target = "ps_4_1";
            break;
        default:
            target = "ps_5_0";
            break;
        }

        static constexpr auto flags =
            D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR
#ifdef NDEBUG
            | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#else
            // Only enable strictness and warnings in DEBUG mode
            //  as these settings makes it very difficult to develop
            //  shaders as windows terminal is not telling the user
            //  what's wrong, windows terminal just fails.
            //  Keep it in DEBUG mode to catch errors in shaders
            //  shipped with windows terminal
            | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS | D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

        wil::com_ptr<ID3DBlob> error;
        wil::com_ptr<ID3DBlob> blob;
        const auto hr = D3DCompileFromFile(
            /* pFileName   */ p.s->misc->customPixelShaderPath.c_str(),
            /* pDefines    */ nullptr,
            /* pInclude    */ D3D_COMPILE_STANDARD_FILE_INCLUDE,
            /* pEntrypoint */ "main",
            /* pTarget     */ target,
            /* Flags1      */ flags,
            /* Flags2      */ 0,
            /* ppCode      */ blob.addressof(),
            /* ppErrorMsgs */ error.addressof());

        // Unless we can determine otherwise, assume this shader requires evaluation every frame
        _requiresContinuousRedraw = true;

        if (SUCCEEDED(hr))
        {
            THROW_IF_FAILED(_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, _customPixelShader.put()));

            // Try to determine whether the shader uses the Time variable
            wil::com_ptr<ID3D11ShaderReflection> reflector;
            if (SUCCEEDED_LOG(D3DReflect(blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(reflector.put()))))
            {
                if (ID3D11ShaderReflectionConstantBuffer* constantBufferReflector = reflector->GetConstantBufferByIndex(0)) // shader buffer
                {
                    if (ID3D11ShaderReflectionVariable* variableReflector = constantBufferReflector->GetVariableByIndex(0)) // time
                    {
                        D3D11_SHADER_VARIABLE_DESC variableDescriptor;
                        if (SUCCEEDED_LOG(variableReflector->GetDesc(&variableDescriptor)))
                        {
                            // only if time is used
                            _requiresContinuousRedraw = WI_IsFlagSet(variableDescriptor.uFlags, D3D_SVF_USED);
                        }
                    }
                }
            }
        }
        else
        {
            if (error)
            {
                LOG_HR_MSG(hr, "%*hs", error->GetBufferSize(), error->GetBufferPointer());
            }
            else
            {
                LOG_HR(hr);
            }
            if (p.warningCallback)
            {
                p.warningCallback(D2DERR_SHADER_COMPILE_FAILED);
            }
        }
    }
    else if (p.s->misc->useRetroTerminalEffect)
    {
        THROW_IF_FAILED(_device->CreatePixelShader(&custom_shader_ps[0], sizeof(custom_shader_ps), nullptr, _customPixelShader.put()));
        // We know the built-in retro shader doesn't require continuous redraw.
        _requiresContinuousRedraw = false;
    }

    if (_customPixelShader)
    {
        THROW_IF_FAILED(_device->CreateVertexShader(&custom_shader_vs[0], sizeof(custom_shader_vs), nullptr, _customVertexShader.put()));

        {
            static constexpr D3D11_BUFFER_DESC desc{
                .ByteWidth = sizeof(CustomConstBuffer),
                .Usage = D3D11_USAGE_DYNAMIC,
                .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
                .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
            };
            THROW_IF_FAILED(_device->CreateBuffer(&desc, nullptr, _customShaderConstantBuffer.put()));
        }

        {
            static constexpr D3D11_SAMPLER_DESC desc{
                .Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
                .AddressU = D3D11_TEXTURE_ADDRESS_BORDER,
                .AddressV = D3D11_TEXTURE_ADDRESS_BORDER,
                .AddressW = D3D11_TEXTURE_ADDRESS_BORDER,
                .MaxAnisotropy = 1,
                .ComparisonFunc = D3D11_COMPARISON_ALWAYS,
                .MaxLOD = D3D11_FLOAT32_MAX,
            };
            THROW_IF_FAILED(_device->CreateSamplerState(&desc, _customShaderSamplerState.put()));
        }

        _customShaderStartTime = std::chrono::steady_clock::now();
    }
}

void BackendD3D::_recreateCustomRenderTargetView(u16x2 targetSize)
{
    // Avoid memory usage spikes by releasing memory first.
    _customOffscreenTexture.reset();
    _customOffscreenTextureView.reset();

    // This causes our regular rendered contents to end up in the offscreen texture. We'll then use the
    // `_customRenderTargetView` to render into the swap chain using the custom (user provided) shader.
    _customRenderTargetView = std::move(_renderTargetView);

    const D3D11_TEXTURE2D_DESC desc{
        .Width = targetSize.x,
        .Height = targetSize.y,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .SampleDesc = { 1, 0 },
        .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
    };
    THROW_IF_FAILED(_device->CreateTexture2D(&desc, nullptr, _customOffscreenTexture.addressof()));
    THROW_IF_FAILED(_device->CreateShaderResourceView(_customOffscreenTexture.get(), nullptr, _customOffscreenTextureView.addressof()));
    THROW_IF_FAILED(_device->CreateRenderTargetView(_customOffscreenTexture.get(), nullptr, _renderTargetView.addressof()));
}

void BackendD3D::_recreateColorBitmap(u16x2 cellCount)
{
    // Avoid memory usage spikes by releasing memory first.
    _colorBitmap.reset();
    _colorBitmapView.reset();

    const D3D11_TEXTURE2D_DESC desc{
        .Width = cellCount.x,
        .Height = cellCount.y * 2u,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = { 1, 0 },
        .Usage = D3D11_USAGE_DYNAMIC,
        .BindFlags = D3D11_BIND_SHADER_RESOURCE,
        .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
    };
    THROW_IF_FAILED(_device->CreateTexture2D(&desc, nullptr, _colorBitmap.addressof()));
    THROW_IF_FAILED(_device->CreateShaderResourceView(_colorBitmap.get(), nullptr, _colorBitmapView.addressof()));
    _colorBitmapGenerations = {};
}

void BackendD3D::_d2dRenderTargetUpdateFontSettings(const FontSettings& font) const noexcept
{
    _d2dRenderTarget->SetDpi(font.dpi, font.dpi);
    _d2dRenderTarget->SetTextAntialiasMode(static_cast<D2D1_TEXT_ANTIALIAS_MODE>(font.antialiasingMode));
}

void BackendD3D::_recreateConstBuffer(const RenderingPayload& p) const
{
    {
        VSConstBuffer data{};
        data.positionScale = { 2.0f / p.s->targetSize.x, -2.0f / p.s->targetSize.y };
        _deviceContext->UpdateSubresource(_vsConstantBuffer.get(), 0, nullptr, &data, 0, 0);
    }
    {
        PSConstBuffer data{};
        data.backgroundColor = colorFromU32Premultiply<f32x4>(p.s->misc->backgroundColor);
        data.cellSize = { static_cast<f32>(p.s->font->cellSize.x), static_cast<f32>(p.s->font->cellSize.y) };
        data.cellCount = { static_cast<f32>(p.s->cellCount.x), static_cast<f32>(p.s->cellCount.y) };
        DWrite_GetGammaRatios(_gamma, data.gammaRatios);
        data.enhancedContrast = p.s->font->antialiasingMode == AntialiasingMode::ClearType ? _cleartypeEnhancedContrast : _grayscaleEnhancedContrast;
        data.dashedLineLength = p.s->font->underlineWidth * 3.0f;
        _deviceContext->UpdateSubresource(_psConstantBuffer.get(), 0, nullptr, &data, 0, 0);
    }
}

void BackendD3D::_setupDeviceContextState(const RenderingPayload& p)
{
    // IA: Input Assembler
    ID3D11Buffer* vertexBuffers[]{ _vertexBuffer.get(), _instanceBuffer.get() };
    static constexpr UINT strides[]{ sizeof(f32x2), sizeof(QuadInstance) };
    static constexpr UINT offsets[]{ 0, 0 };
    _deviceContext->IASetIndexBuffer(_indexBuffer.get(), DXGI_FORMAT_R16_UINT, 0);
    _deviceContext->IASetInputLayout(_inputLayout.get());
    _deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    _deviceContext->IASetVertexBuffers(0, 2, &vertexBuffers[0], &strides[0], &offsets[0]);

    // VS: Vertex Shader
    _deviceContext->VSSetShader(_vertexShader.get(), nullptr, 0);
    _deviceContext->VSSetConstantBuffers(0, 1, _vsConstantBuffer.addressof());

    // RS: Rasterizer Stage
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<f32>(p.s->targetSize.x);
    viewport.Height = static_cast<f32>(p.s->targetSize.y);
    _deviceContext->RSSetViewports(1, &viewport);

    // PS: Pixel Shader
    ID3D11ShaderResourceView* resources[]{ _colorBitmapView.get(), _glyphAtlasView.get() };
    _deviceContext->PSSetShader(_pixelShader.get(), nullptr, 0);
    _deviceContext->PSSetConstantBuffers(0, 1, _psConstantBuffer.addressof());
    _deviceContext->PSSetShaderResources(0, 2, &resources[0]);

    // OM: Output Merger
    _deviceContext->OMSetBlendState(_blendState.get(), nullptr, 0xffffffff);
    _deviceContext->OMSetRenderTargets(1, _renderTargetView.addressof(), nullptr);
}

#ifndef NDEBUG
void BackendD3D::_debugUpdateShaders(const RenderingPayload& p) noexcept
try
{
    const auto invalidationTime = _sourceCodeInvalidationTime.load(std::memory_order_relaxed);

    if (invalidationTime == INT64_MAX || invalidationTime > std::chrono::steady_clock::now().time_since_epoch().count())
    {
        return;
    }

    _sourceCodeInvalidationTime.store(INT64_MAX, std::memory_order_relaxed);

    static const auto compile = [](const std::filesystem::path& path, const char* target) {
        wil::com_ptr<ID3DBlob> error;
        wil::com_ptr<ID3DBlob> blob;
        const auto hr = D3DCompileFromFile(
            /* pFileName   */ path.c_str(),
            /* pDefines    */ nullptr,
            /* pInclude    */ D3D_COMPILE_STANDARD_FILE_INCLUDE,
            /* pEntrypoint */ "main",
            /* pTarget     */ target,
            /* Flags1      */ D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS,
            /* Flags2      */ 0,
            /* ppCode      */ blob.addressof(),
            /* ppErrorMsgs */ error.addressof());

        if (error)
        {
            std::thread t{ [error = std::move(error)]() noexcept {
                MessageBoxA(nullptr, static_cast<const char*>(error->GetBufferPointer()), "Compilation error", MB_ICONERROR | MB_OK);
            } };
            t.detach();
        }

        THROW_IF_FAILED(hr);
        return blob;
    };

    struct FileVS
    {
        std::wstring_view filename;
        wil::com_ptr<ID3D11VertexShader> BackendD3D::*target;
    };
    struct FilePS
    {
        std::wstring_view filename;
        wil::com_ptr<ID3D11PixelShader> BackendD3D::*target;
    };

    static constexpr std::array filesVS{
        FileVS{ L"shader_vs.hlsl", &BackendD3D::_vertexShader },
    };
    static constexpr std::array filesPS{
        FilePS{ L"shader_ps.hlsl", &BackendD3D::_pixelShader },
    };

    std::array<wil::com_ptr<ID3D11VertexShader>, filesVS.size()> compiledVS;
    std::array<wil::com_ptr<ID3D11PixelShader>, filesPS.size()> compiledPS;

    // Compile our files before moving them into `this` below to ensure we're
    // always in a consistent state where all shaders are seemingly valid.
    for (size_t i = 0; i < filesVS.size(); ++i)
    {
        const auto blob = compile(_sourceDirectory / filesVS[i].filename, "vs_4_0");
        THROW_IF_FAILED(_device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, compiledVS[i].addressof()));
    }
    for (size_t i = 0; i < filesPS.size(); ++i)
    {
        const auto blob = compile(_sourceDirectory / filesPS[i].filename, "ps_4_0");
        THROW_IF_FAILED(_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, compiledPS[i].addressof()));
    }

    for (size_t i = 0; i < filesVS.size(); ++i)
    {
        this->*filesVS[i].target = std::move(compiledVS[i]);
    }
    for (size_t i = 0; i < filesPS.size(); ++i)
    {
        this->*filesPS[i].target = std::move(compiledPS[i]);
    }

    _setupDeviceContextState(p);
}
CATCH_LOG()
#endif

void BackendD3D::_d2dBeginDrawing() noexcept
{
    if (!_d2dBeganDrawing)
    {
        _d2dRenderTarget->BeginDraw();
        _d2dBeganDrawing = true;
    }
}

void BackendD3D::_d2dEndDrawing()
{
    if (_d2dBeganDrawing)
    {
        THROW_IF_FAILED(_d2dRenderTarget->EndDraw());
        _d2dBeganDrawing = false;
    }
}

void BackendD3D::_resetGlyphAtlas(const RenderingPayload& p)
{
    // The index returned by _BitScanReverse is undefined when the input is 0. We can simultaneously guard
    // against that and avoid unreasonably small textures, by clamping the min. texture size to `minArea`.
    // `minArea` results in a 64kB RGBA texture which is the min. alignment for placed memory.
    static constexpr u32 minArea = 128 * 128;
    static constexpr u32 maxArea = D3D10_REQ_TEXTURE2D_U_OR_V_DIMENSION * D3D10_REQ_TEXTURE2D_U_OR_V_DIMENSION;

    const auto cellArea = static_cast<u32>(p.s->font->cellSize.x) * p.s->font->cellSize.y;
    const auto targetArea = static_cast<u32>(p.s->targetSize.x) * p.s->targetSize.y;

    const auto minAreaByFont = cellArea * 95; // Covers all printable ASCII characters
    const auto minAreaByGrowth = static_cast<u32>(_rectPacker.width) * _rectPacker.height * 2;
    const auto min = std::max(minArea, std::max(minAreaByFont, minAreaByGrowth));

    // It's hard to say what the max. size of the cache should be. Optimally I think we should use as much
    // memory as is available, but the rendering code in this project is a big mess and so integrating
    // memory pressure feedback (RegisterVideoMemoryBudgetChangeNotificationEvent) is rather difficult.
    // As an alternative I'm using 1.25x the size of the swap chain. The 1.25x is there to avoid situations, where
    // we're locked into a state, where on every render pass we're starting with a half full atlas, drawing once,
    // filling it with the remaining half and drawing again, requiring two rendering passes on each frame.
    const auto maxAreaByFont = targetArea + targetArea / 4;
    const auto area = std::min(maxArea, std::min(maxAreaByFont, min));

    // This block of code calculates the size of a power-of-2 texture that has an area larger than the given `area`.
    // For instance, for an area of 985x1946 = 1916810 it would result in a u/v of 2048x1024 (area = 2097152).
    // This has 2 benefits: GPUs like power-of-2 textures and it ensures that we don't resize the texture
    // every time you resize the window by a pixel. Instead it only grows/shrinks by a factor of 2.
    unsigned long index;
    _BitScanReverse(&index, area - 1);
    const auto u = static_cast<u16>(1u << ((index + 2) / 2));
    const auto v = static_cast<u16>(1u << ((index + 1) / 2));

    if (u != _rectPacker.width || v != _rectPacker.height)
    {
        _d2dRenderTarget.reset();
        _d2dRenderTarget4.reset();
        _glyphAtlas.reset();
        _glyphAtlasView.reset();

        {
            const D3D11_TEXTURE2D_DESC desc{
                .Width = u,
                .Height = v,
                .MipLevels = 1,
                .ArraySize = 1,
                .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
                .SampleDesc = { 1, 0 },
                .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
            };
            THROW_IF_FAILED(_device->CreateTexture2D(&desc, nullptr, _glyphAtlas.addressof()));
            THROW_IF_FAILED(_device->CreateShaderResourceView(_glyphAtlas.get(), nullptr, _glyphAtlasView.addressof()));
        }

        {
            const auto surface = _glyphAtlas.query<IDXGISurface>();

            static constexpr D2D1_RENDER_TARGET_PROPERTIES props{
                .type = D2D1_RENDER_TARGET_TYPE_DEFAULT,
                .pixelFormat = { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED },
            };
            // ID2D1RenderTarget and ID2D1DeviceContext are the same and I'm tired of pretending they're not.
            THROW_IF_FAILED(p.d2dFactory->CreateDxgiSurfaceRenderTarget(surface.get(), &props, reinterpret_cast<ID2D1RenderTarget**>(_d2dRenderTarget.addressof())));
            _d2dRenderTarget.try_query_to(_d2dRenderTarget4.addressof());

            _d2dRenderTarget->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
            // We don't really use D2D for anything except DWrite, but it
            // can't hurt to ensure that everything it does is pixel aligned.
            _d2dRenderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            // Ensure that D2D uses the exact same gamma as our shader uses.
            _d2dRenderTarget->SetTextRenderingParams(_textRenderingParams.get());

            _d2dRenderTargetUpdateFontSettings(*p.s->font);
        }

        // We have our own glyph cache so Direct2D's cache doesn't help much.
        // This saves us 1MB of RAM, which is not much, but also not nothing.
        {
            wil::com_ptr<ID2D1Device> device;
            _d2dRenderTarget4->GetDevice(device.addressof());

            device->SetMaximumTextureMemory(0);
            if (const auto device4 = device.try_query<ID2D1Device4>())
            {
                device4->SetMaximumColorGlyphCacheMemory(0);
            }
        }

        {
            static constexpr D2D1_COLOR_F color{ 1, 1, 1, 1 };
            THROW_IF_FAILED(_d2dRenderTarget->CreateSolidColorBrush(&color, nullptr, _brush.put()));
        }

        ID3D11ShaderResourceView* resources[]{ _colorBitmapView.get(), _glyphAtlasView.get() };
        _deviceContext->PSSetShaderResources(0, 2, &resources[0]);

        _rectPackerData = Buffer<stbrp_node>{ u };
    }

    stbrp_init_target(&_rectPacker, u, v, _rectPackerData.data(), _rectPackerData.size());

    for (auto& slot : _glyphAtlasMap.container())
    {
        slot.inner.reset();
    }

    _d2dBeginDrawing();
    _d2dRenderTarget->Clear();

    _fontChangedResetGlyphAtlas = false;
}

void BackendD3D::_markStateChange(ID3D11BlendState* blendState)
{
    _instancesStateChanges.emplace_back(StateChange{
        .blendState = blendState,
        .offset = _instancesCount,
    });
}

BackendD3D::QuadInstance& BackendD3D::_getLastQuad() noexcept
{
    assert(_instancesCount != 0);
    return _instances[_instancesCount - 1];
}

// NOTE: Up to 5M calls per second -> no std::vector, no std::unordered_map.
// This function is an easy >100x faster than std::vector, can be
// inlined and reduces overall (!) renderer CPU usage by 5%.
BackendD3D::QuadInstance& BackendD3D::_appendQuad()
{
    if (_instancesCount >= _instances.size())
    {
        _bumpInstancesSize();
    }

    return _instances[_instancesCount++];
}

void BackendD3D::_bumpInstancesSize()
{
    const auto newSize = std::max<size_t>(256, _instances.size() * 2);
    Expects(newSize > _instances.size());

    // Our render loop heavily relies on memcpy() which is up to between 1.5x (Intel)
    // and 40x (AMD) faster for allocations with an alignment of 32 or greater.
    auto newInstances = Buffer<QuadInstance, 32>{ newSize };
    std::copy_n(_instances.data(), _instances.size(), newInstances.data());

    _instances = std::move(newInstances);
}

void BackendD3D::_flushQuads(const RenderingPayload& p)
{
    if (!_instancesCount)
    {
        return;
    }

    _uploadColorBitmap(p);

    // TODO: Shrink instances buffer
    if (_instancesCount > _instanceBufferCapacity)
    {
        _recreateInstanceBuffers(p);
    }

    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        THROW_IF_FAILED(_deviceContext->Map(_instanceBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        memcpy(mapped.pData, _instances.data(), _instancesCount * sizeof(QuadInstance));
        _deviceContext->Unmap(_instanceBuffer.get(), 0);
    }

    // I found 4 approaches to drawing lots of quads quickly. There are probably even more.
    // They can often be found in discussions about "particle" or "point sprite" rendering in game development.
    // * Compute Shader: My understanding is that at the time of writing games are moving over to bucketing
    //   particles into "tiles" on the screen and drawing them with a compute shader. While this improves
    //   performance, it doesn't mix well with our goal of allowing arbitrary overlaps between glyphs.
    //   Additionally none of the next 3 approaches use any significant amount of GPU time in the first place.
    // * Geometry Shader: Geometry shaders can generate vertices on the fly, which would neatly replace our need
    //   for an index buffer. However, many sources claim they're significantly slower than the following approaches.
    // * DrawIndexed & DrawInstanced: Again, many sources claim that GPU instancing (Draw(Indexed)Instanced) performs
    //   poorly for small meshes, and instead indexed vertices with a SRV (shader resource view) should be used.
    //   The popular "Vertex Shader Tricks" talk from Bill Bilodeau at GDC 2014 suggests this approach, explains
    //   how it works (you divide the `SV_VertexID` by 4 and index into the SRV that contains the per-instance data;
    //   it's basically manual instancing inside the vertex shader) and shows how it outperforms regular instancing.
    //   However on my own limited test hardware (built around ~2020), I found that for at least our use case,
    //   GPU instancing matches the performance of using a custom buffer. In fact on my Nvidia GPU in particular,
    //   instancing with ~10k instances appears to be about 50% faster and so DrawInstanced was chosen.
    //   Instead I found that packing instance data as tightly as possible made the biggest performance difference,
    //   and packing 16 bit integers with ID3D11InputLayout is quite a bit more convenient too.

    // This will cause the loop below to emit one final DrawIndexedInstanced() for the remainder of instances.
    _markStateChange(nullptr);

    size_t previousOffset = 0;
    for (const auto& state : _instancesStateChanges)
    {
        if (const auto count = state.offset - previousOffset)
        {
            _deviceContext->DrawIndexedInstanced(6, count, 0, 0, previousOffset);
        }
        if (state.blendState)
        {
            _deviceContext->OMSetBlendState(state.blendState, nullptr, 0xffffffff);
        }
        previousOffset = state.offset;
    }

    _instancesStateChanges.clear();
    _instancesCount = 0;
}

void BackendD3D::_uploadColorBitmap(const RenderingPayload& p)
{
    // Not uploading the bitmap halves (!) the GPU load for any given frame.
    // We don't need to upload if the background and foreground bitmaps are the same
    // or when the _drawText() function determined that no glyph has the LigatureMarker,
    // because then the pixel shader doesn't need to access the foreground bitmap anyways.
    if (_colorBitmapGenerations[0] == p.colorBitmapGenerations[0] &&
        (_colorBitmapGenerations[1] == p.colorBitmapGenerations[1] || _skipForegroundBitmapUpload))
    {
        return;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    THROW_IF_FAILED(_deviceContext->Map(_colorBitmap.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));

    auto src = std::bit_cast<const char*>(&*p.colorBitmap.begin());
    const auto srcEnd = std::bit_cast<const char*>(&*p.colorBitmap.end());
    const auto srcStride = p.colorBitmapRowStride * sizeof(u32);
    auto dst = static_cast<char*>(mapped.pData);

    while (src < srcEnd)
    {
        memcpy(dst, src, srcStride);
        src += srcStride;
        dst += mapped.RowPitch;
    }

    _deviceContext->Unmap(_colorBitmap.get(), 0);
    _colorBitmapGenerations = p.colorBitmapGenerations;
}

void BackendD3D::_recreateInstanceBuffers(const RenderingPayload& p)
{
    // We use the viewport size of the terminal as the initial estimate for the amount of instances we'll see.
    const auto minCapacity = static_cast<size_t>(p.s->cellCount.x) * p.s->cellCount.y;
    auto newCapacity = std::max(_instancesCount, minCapacity);
    auto newSize = newCapacity * sizeof(QuadInstance);
    // Round up to multiples of 64kB to avoid reallocating too often.
    // 64kB is the minimum alignment for committed resources in D3D12.
    newSize = (newSize + 0xffff) & ~size_t{ 0xffff };
    newCapacity = newSize / sizeof(QuadInstance);

    _instanceBuffer.reset();

    {
        const D3D11_BUFFER_DESC desc{
            .ByteWidth = gsl::narrow<UINT>(newSize),
            .Usage = D3D11_USAGE_DYNAMIC,
            .BindFlags = D3D11_BIND_VERTEX_BUFFER,
            .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
            .StructureByteStride = sizeof(QuadInstance),
        };
        THROW_IF_FAILED(_device->CreateBuffer(&desc, nullptr, _instanceBuffer.addressof()));
    }

    // IA: Input Assembler
    ID3D11Buffer* vertexBuffers[]{ _vertexBuffer.get(), _instanceBuffer.get() };
    static constexpr UINT strides[]{ sizeof(f32x2), sizeof(QuadInstance) };
    static constexpr UINT offsets[]{ 0, 0 };
    _deviceContext->IASetVertexBuffers(0, 2, &vertexBuffers[0], &strides[0], &offsets[0]);

    _instanceBufferCapacity = newCapacity;
}

void BackendD3D::_drawBackground(const RenderingPayload& p)
{
    _appendQuad() = {
        .shadingType = ShadingType::Background,
        .size = p.s->targetSize,
    };
}

void BackendD3D::_drawText(RenderingPayload& p)
{
    if (_fontChangedResetGlyphAtlas)
    {
        _resetGlyphAtlas(p);
    }

    auto shadingTypeAccumulator = ShadingType::Default;
    _skipForegroundBitmapUpload = false;

    til::CoordType dirtyTop = til::CoordTypeMax;
    til::CoordType dirtyBottom = til::CoordTypeMin;

    u16 y = 0;
    for (const auto row : p.rows)
    {
        f32 baselineX = 0;
        const auto baselineY = y * p.s->font->cellSize.y + p.s->font->baseline;
        const auto lineRenditionScale = static_cast<uint8_t>(row->lineRendition != LineRendition::SingleWidth);

        for (const auto& m : row->mappings)
        {
            auto x = m.glyphsFrom;
            const AtlasFontFaceKey fontFaceKey{
                .fontFace = m.fontFace.get(),
                .lineRendition = row->lineRendition,
            };

        // This goto label exists to allow us to retry rendering a glyph if the glyph atlas was full.
        // We need to goto here, because a retry will cause the atlas texture as well as the
        // _glyphCache hashmap to be cleared, and so we'll have to call insert() again.
        drawGlyphRetry:
            auto& fontFaceEntry = *_glyphAtlasMap.insert(fontFaceKey).first.inner;

            while (x < m.glyphsTo)
            {
                const auto [glyphEntry, inserted] = fontFaceEntry.glyphs.insert(row->glyphIndices[x]);

                if (inserted && !_drawGlyph(p, row->glyphAdvances[x], fontFaceEntry, glyphEntry))
                {
                    // A deadlock in this retry loop is detected in _drawGlyphPrepareRetry.
                    //
                    // Yes, I agree, avoid goto. Sometimes. It's not my fault that C++ still doesn't
                    // have a `continue outerloop;` like other languages had it for decades. :(
#pragma warning(suppress : 26438) // Avoid 'goto' (es.76).
#pragma warning(suppress : 26448) // Consider using gsl::finally if final action is intended (gsl.util).
                    goto drawGlyphRetry;
                }

                if (glyphEntry.data.shadingType != ShadingType::Default)
                {
                    auto l = static_cast<til::CoordType>(lrintf(baselineX + row->glyphOffsets[x].advanceOffset));
                    auto t = static_cast<til::CoordType>(lrintf(baselineY - row->glyphOffsets[x].ascenderOffset));

                    // A non-standard line rendition will make characters appear twice as wide, which requires us to scale the baseline advance by 2.
                    // We need to do this before applying the glyph offset however, since the offset is already 2x scaled in case of such glyphs.
                    l <<= lineRenditionScale;

                    l += glyphEntry.data.offset.x;
                    t += glyphEntry.data.offset.y;

                    row->dirtyTop = std::min(row->dirtyTop, t);
                    row->dirtyBottom = std::max(row->dirtyBottom, t + glyphEntry.data.size.y);

                    _appendQuad() = {
                        .shadingType = glyphEntry.data.shadingType,
                        .position = { static_cast<i16>(l), static_cast<i16>(t) },
                        .size = glyphEntry.data.size,
                        .texcoord = glyphEntry.data.texcoord,
                        .color = row->colors[x],
                    };

                    shadingTypeAccumulator |= glyphEntry.data.shadingType;
                }

                baselineX += row->glyphAdvances[x];
                ++x;
            }
        }

        if (p.invalidatedRows.contains(y))
        {
            dirtyTop = std::min(dirtyTop, row->dirtyTop);
            dirtyBottom = std::max(dirtyBottom, row->dirtyBottom);
        }

        ++y;
    }

    if (dirtyTop < dirtyBottom)
    {
        p.dirtyRectInPx.top = std::min(p.dirtyRectInPx.top, dirtyTop);
        p.dirtyRectInPx.bottom = std::max(p.dirtyRectInPx.bottom, dirtyBottom);
    }

    _d2dEndDrawing();

    _skipForegroundBitmapUpload = WI_IsFlagClear(shadingTypeAccumulator, ShadingType::LigatureMarker);
}

bool BackendD3D::_drawGlyph(const RenderingPayload& p, f32 glyphAdvance, const AtlasFontFaceEntryInner& fontFaceEntry, AtlasGlyphEntry& glyphEntry)
{
    if (!fontFaceEntry.fontFace)
    {
        return _drawSoftFontGlyph(p, fontFaceEntry, glyphEntry);
    }

    const DWRITE_GLYPH_RUN glyphRun{
        .fontFace = fontFaceEntry.fontFace.get(),
        .fontEmSize = p.s->font->fontSize,
        .glyphCount = 1,
        .glyphIndices = &glyphEntry.glyphIndex,
    };

    // It took me a while to figure out how to rasterize glyphs manually with DirectWrite without depending on Direct2D.
    // The benefits are a reduction in memory usage, an increase in performance under certain circumstances and most
    // importantly, the ability to debug the renderer more easily, because many graphics debuggers don't support Direct2D.
    // Direct2D has one big advantage however: It features a clever glyph uploader with a pool of D3D11_USAGE_STAGING textures,
    // which I was too short on time to implement myself. This makes rasterization with Direct2D roughly 2x faster.
    //
    // This code remains, because it features some parts that are slightly more and some parts that are outright difficult to figure out.
#if 0
    const auto wantsClearType = p.s->font->antialiasingMode == AntialiasingMode::ClearType;
    const auto wantsAliased = p.s->font->antialiasingMode == AntialiasingMode::Aliased;
    const auto antialiasMode = wantsClearType ? DWRITE_TEXT_ANTIALIAS_MODE_CLEARTYPE : DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE;
    const auto outlineThreshold = wantsAliased ? DWRITE_OUTLINE_THRESHOLD_ALIASED : DWRITE_OUTLINE_THRESHOLD_ANTIALIASED;

    DWRITE_RENDERING_MODE renderingMode{};
    DWRITE_GRID_FIT_MODE gridFitMode{};
    THROW_IF_FAILED(fontFaceEntry.fontFace->GetRecommendedRenderingMode(
        /* fontEmSize       */ fontEmSize,
        /* dpiX             */ 1, // fontEmSize is already in pixel
        /* dpiY             */ 1, // fontEmSize is already in pixel
        /* transform        */ nullptr,
        /* isSideways       */ FALSE,
        /* outlineThreshold */ outlineThreshold,
        /* measuringMode    */ DWRITE_MEASURING_MODE_NATURAL,
        /* renderingParams  */ _textRenderingParams.get(),
        /* renderingMode    */ &renderingMode,
        /* gridFitMode      */ &gridFitMode));

    wil::com_ptr<IDWriteGlyphRunAnalysis> glyphRunAnalysis;
    THROW_IF_FAILED(p.dwriteFactory->CreateGlyphRunAnalysis(
        /* glyphRun         */ &glyphRun,
        /* transform        */ nullptr,
        /* renderingMode    */ renderingMode,
        /* measuringMode    */ DWRITE_MEASURING_MODE_NATURAL,
        /* gridFitMode      */ gridFitMode,
        /* antialiasMode    */ antialiasMode,
        /* baselineOriginX  */ 0,
        /* baselineOriginY  */ 0,
        /* glyphRunAnalysis */ glyphRunAnalysis.put()));

    RECT textureBounds{};

    if (wantsClearType)
    {
        THROW_IF_FAILED(glyphRunAnalysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &textureBounds));

        // Some glyphs cannot be drawn with ClearType, such as bitmap fonts. In that case
        // GetAlphaTextureBounds() supposedly returns an empty RECT, but I haven't tested that yet.
        if (!IsRectEmpty(&textureBounds))
        {
            // Allocate a buffer of `3 * width * height` bytes.
            THROW_IF_FAILED(glyphRunAnalysis->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1, &textureBounds, buffer.data(), buffer.size()));
            // The buffer contains RGB ClearType weights which can now be packed into RGBA and uploaded to the glyph atlas.
            return;
        }

        // --> Retry with grayscale AA.
    }

    // Even though it says "ALIASED" and even though the docs for the flag still say:
    // > [...] that is, each pixel is either fully opaque or fully transparent [...]
    // don't be confused: It's grayscale antialiased. lol
    THROW_IF_FAILED(glyphRunAnalysis->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &textureBounds));

    // Allocate a buffer of `width * height` bytes.
    THROW_IF_FAILED(glyphRunAnalysis->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &textureBounds, buffer.data(), buffer.size()));
    // The buffer now contains a grayscale alpha mask.
#endif

    const auto lineRendition = static_cast<LineRendition>(fontFaceEntry.lineRendition);
    std::optional<D2D1_MATRIX_3X2_F> transform;

    if (lineRendition != LineRendition::SingleWidth)
    {
        auto& t = transform.emplace();
        t.m11 = 2.0f;
        t.m22 = lineRendition >= LineRendition::DoubleHeightTop ? 2.0f : 1.0f;
        _d2dRenderTarget->SetTransform(&t);
        glyphAdvance *= 2;
    }

    const auto restoreTransform = wil::scope_exit([&]() noexcept {
        if (transform)
        {
            static constexpr D2D1_MATRIX_3X2_F identity{ .m11 = 1, .m22 = 1 };
            _d2dRenderTarget->SetTransform(&identity);
        }
    });

    // This calculates the black box of the glyph, or in other words,
    // it's extents/size relative to its baseline origin (at 0,0).
    //
    //  box.top --------++-----######--+
    //   (-7)           ||  ############
    //                  ||####      ####
    //                  |###       #####
    //  baseline _____  |###      #####|
    //   origin       \ |############# |
    //  (= 0,0)        \||###########  |
    //                  ++-------###---+
    //                  ##      ###    |
    //  box.bottom -----+#########-----+
    //    (+2)          |              |
    //               box.left       box.right
    //                 (-1)           (+14)
    //
    D2D1_RECT_F box{};
    THROW_IF_FAILED(_d2dRenderTarget->GetGlyphRunWorldBounds({}, &glyphRun, DWRITE_MEASURING_MODE_NATURAL, &box));

    // box may be empty if the glyph is whitespace.
    if (box.left >= box.right || box.top >= box.bottom)
    {
        return true;
    }

    const auto bl = lrintf(box.left);
    const auto bt = lrintf(box.top);
    const auto br = lrintf(box.right);
    const auto bb = lrintf(box.bottom);

    stbrp_rect rect{
        .w = br - bl,
        .h = bb - bt,
    };
    if (!stbrp_pack_rects(&_rectPacker, &rect, 1))
    {
        _drawGlyphPrepareRetry(p);
        return false;
    }

    const D2D1_POINT_2F baselineOrigin{
        static_cast<f32>(rect.x - bl),
        static_cast<f32>(rect.y - bt),
    };

    if (transform)
    {
        auto& t = *transform;
        t.dx = (1.0f - t.m11) * baselineOrigin.x;
        t.dy = (1.0f - t.m22) * baselineOrigin.y;
        _d2dRenderTarget->SetTransform(&t);
    }

    _d2dBeginDrawing();
    const auto colorGlyph = DrawGlyphRun(_d2dRenderTarget.get(), _d2dRenderTarget4.get(), p.dwriteFactory4.get(), baselineOrigin, &glyphRun, _brush.get());
    auto shadingType = colorGlyph ? ShadingType::Passthrough : _textShadingType;

    // Ligatures are drawn with strict cell-wise foreground color, while other text allows colors to overhang
    // their cells. This makes sure that italics and such retain their color and don't look "cut off".
    //
    // The former condition makes sure to exclude diacritics and such from being considered a ligature,
    // while the latter condition-pair makes sure to exclude regular BMP wide glyphs that overlap a little.
    if (rect.w >= p.s->font->cellSize.x && (bl <= p.s->font->ligatureOverhangTriggerLeft || br >= p.s->font->ligatureOverhangTriggerRight))
    {
        shadingType |= ShadingType::LigatureMarker;
    }

    glyphEntry.data.shadingType = shadingType;
    glyphEntry.data.offset.x = bl;
    glyphEntry.data.offset.y = bt;
    glyphEntry.data.size.x = rect.w;
    glyphEntry.data.size.y = rect.h;
    glyphEntry.data.texcoord.x = rect.x;
    glyphEntry.data.texcoord.y = rect.y;

    if (lineRendition >= LineRendition::DoubleHeightTop)
    {
        _splitDoubleHeightGlyph(p, fontFaceEntry, glyphEntry);
    }

    return true;
}

bool BackendD3D::_drawSoftFontGlyph(const RenderingPayload& p, const AtlasFontFaceEntryInner& fontFaceEntry, AtlasGlyphEntry& glyphEntry)
{
    stbrp_rect rect{
        .w = p.s->font->cellSize.x,
        .h = p.s->font->cellSize.y,
    };

    const auto lineRendition = static_cast<LineRendition>(fontFaceEntry.lineRendition);
    if (lineRendition != LineRendition::SingleWidth)
    {
        rect.w <<= 1;
        rect.h <<= static_cast<u8>(lineRendition >= LineRendition::DoubleHeightTop);
    }

    if (!stbrp_pack_rects(&_rectPacker, &rect, 1))
    {
        _drawGlyphPrepareRetry(p);
        return false;
    }

    if (!_softFontBitmap)
    {
        // Allocating such a tiny texture is very wasteful (min. texture size on GPUs
        // right now is 64kB), but this is a seldomly used feature so it's fine...
        const D2D1_SIZE_U size{
            static_cast<UINT32>(p.s->font->softFontCellSize.width),
            static_cast<UINT32>(p.s->font->softFontCellSize.height),
        };
        const D2D1_BITMAP_PROPERTIES1 bitmapProperties{
            .pixelFormat = { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED },
            .dpiX = static_cast<f32>(p.s->font->dpi),
            .dpiY = static_cast<f32>(p.s->font->dpi),
        };
        THROW_IF_FAILED(_d2dRenderTarget->CreateBitmap(size, nullptr, 0, &bitmapProperties, _softFontBitmap.addressof()));
    }

    {
        const auto width = static_cast<size_t>(p.s->font->softFontCellSize.width);
        const auto height = static_cast<size_t>(p.s->font->softFontCellSize.height);

        auto bitmapData = Buffer<u32>{ width * height };
        const auto glyphIndex = glyphEntry.glyphIndex - 0xEF20u;
        auto src = p.s->font->softFontPattern.begin() + height * glyphIndex;
        auto dst = bitmapData.begin();

        for (size_t y = 0; y < height; y++)
        {
            auto srcBits = *src++;
            for (size_t x = 0; x < width; x++)
            {
                const auto srcBitIsSet = (srcBits & 0x8000) != 0;
                *dst++ = srcBitIsSet ? 0xffffffff : 0x00000000;
                srcBits <<= 1;
            }
        }

        const auto pitch = static_cast<UINT32>(width * sizeof(u32));
        THROW_IF_FAILED(_softFontBitmap->CopyFromMemory(nullptr, bitmapData.data(), pitch));
    }

    const auto interpolation = p.s->font->antialiasingMode == AntialiasingMode::Aliased ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC;
    const D2D1_RECT_F dest{
        static_cast<f32>(rect.x),
        static_cast<f32>(rect.y),
        static_cast<f32>(rect.x + rect.w),
        static_cast<f32>(rect.y + rect.h),
    };

    _d2dBeginDrawing();
    _d2dRenderTarget->DrawBitmap(_softFontBitmap.get(), &dest, 1, interpolation, nullptr, nullptr);

    glyphEntry.data.shadingType = ShadingType::TextGrayscale;
    glyphEntry.data.offset.x = 0;
    glyphEntry.data.offset.y = -p.s->font->baseline;
    glyphEntry.data.size.x = rect.w;
    glyphEntry.data.size.y = rect.h;
    glyphEntry.data.texcoord.x = rect.x;
    glyphEntry.data.texcoord.y = rect.y;

    if (lineRendition >= LineRendition::DoubleHeightTop)
    {
        glyphEntry.data.offset.y -= p.s->font->cellSize.y;
        _splitDoubleHeightGlyph(p, fontFaceEntry, glyphEntry);
    }

    return true;
}

void BackendD3D::_drawGlyphPrepareRetry(const RenderingPayload& p)
{
    THROW_HR_IF_MSG(E_UNEXPECTED, _glyphAtlasMap.empty(), "BackendD3D::_drawGlyph deadlock");
    _d2dEndDrawing();
    _flushQuads(p);
    _resetGlyphAtlas(p);
}

// If this is a double-height glyph (DECDHL), we need to split it into 2 glyph entries:
// One for the top half and one for the bottom half, because that's how DECDHL works.This will clip
// `glyphEntry` to only contain the top/bottom half (as specified by `fontFaceEntry.lineRendition`)
// and create a second entry in our glyph cache hashmap that contains the other half.
void BackendD3D::_splitDoubleHeightGlyph(const RenderingPayload& p, const AtlasFontFaceEntryInner& fontFaceEntry, AtlasGlyphEntry& glyphEntry)
{
    // Twice the line height, twice the descender gap. For both.
    glyphEntry.data.offset.y -= p.s->font->descender;

    const auto isTop = fontFaceEntry.lineRendition == LineRendition::DoubleHeightTop;

    const AtlasFontFaceKey key2{
        .fontFace = fontFaceEntry.fontFace.get(),
        .lineRendition = isTop ? LineRendition::DoubleHeightBottom : LineRendition::DoubleHeightTop,
    };

    auto& glyphCache = _glyphAtlasMap.insert(key2).first.inner->glyphs;
    auto& entry2 = glyphCache.insert(glyphEntry.glyphIndex).first;
    entry2.data = glyphEntry.data;

    auto& top = isTop ? glyphEntry : entry2;
    auto& bottom = isTop ? entry2 : glyphEntry;

    const auto topSize = clamp(-glyphEntry.data.offset.y - p.s->font->baseline, 0, static_cast<int>(glyphEntry.data.size.y));
    top.data.offset.y += p.s->font->cellSize.y;
    top.data.size.y = topSize;
    bottom.data.offset.y += topSize;
    bottom.data.size.y = std::max(0, bottom.data.size.y - topSize);
    bottom.data.texcoord.y += topSize;

    // Things like diacritics might be so small that they only exist on either half of the
    // double-height row. This effectively turns the other (unneeded) side into whitespace.
    if (!top.data.size.y)
    {
        top.data.shadingType = ShadingType::Default;
    }
    if (!bottom.data.size.y)
    {
        bottom.data.shadingType = ShadingType::Default;
    }
}

void BackendD3D::_drawGridlines(const RenderingPayload& p)
{
    u16 y = 0;
    for (const auto row : p.rows)
    {
        if (!row->gridLineRanges.empty())
        {
            _drawGridlineRow(p, row, y);
        }
        y++;
    }
}

void BackendD3D::_drawGridlineRow(const RenderingPayload& p, const ShapedRow* row, u16 y)
{
    const auto top = static_cast<i16>(p.s->font->cellSize.y * y);

    for (const auto& r : row->gridLineRanges)
    {
        // AtlasEngine.cpp shouldn't add any gridlines if they don't do anything.
        assert(r.lines.any());

        const auto left = static_cast<i16>(r.from * p.s->font->cellSize.x);
        const auto width = static_cast<u16>((r.to - r.from) * p.s->font->cellSize.x);
        const auto appendHorizontalLine = [&](u16 offsetY, u16 height) {
            _appendQuad() = QuadInstance{
                .shadingType = ShadingType::SolidFill,
                .position = { left, static_cast<i16>(top + offsetY) },
                .size = { width, height },
                .color = r.color,
            };
        };
        const auto appendVerticalLine = [&](int col) {
            _appendQuad() = QuadInstance{
                .shadingType = ShadingType::SolidFill,
                .position = { static_cast<i16>(col * p.s->font->cellSize.x), top },
                .size = { p.s->font->thinLineWidth, p.s->font->cellSize.y },
                .color = r.color,
            };
        };

        if (r.lines.test(GridLines::Left))
        {
            for (auto i = r.from; i < r.to; ++i)
            {
                appendVerticalLine(i);
            }
        }
        if (r.lines.test(GridLines::Top))
        {
            appendHorizontalLine(0, p.s->font->thinLineWidth);
        }
        if (r.lines.test(GridLines::Right))
        {
            for (auto i = r.to; i > r.from; --i)
            {
                appendVerticalLine(i);
            }
        }
        if (r.lines.test(GridLines::Bottom))
        {
            appendHorizontalLine(p.s->font->cellSize.y - p.s->font->thinLineWidth, p.s->font->thinLineWidth);
        }
        if (r.lines.test(GridLines::Underline))
        {
            appendHorizontalLine(p.s->font->underlinePos, p.s->font->underlineWidth);
        }
        if (r.lines.test(GridLines::HyperlinkUnderline))
        {
            appendHorizontalLine(p.s->font->underlinePos, p.s->font->underlineWidth);
        }
        if (r.lines.test(GridLines::DoubleUnderline))
        {
            appendHorizontalLine(p.s->font->doubleUnderlinePos.x, p.s->font->thinLineWidth);
            appendHorizontalLine(p.s->font->doubleUnderlinePos.y, p.s->font->thinLineWidth);
        }
        if (r.lines.test(GridLines::Strikethrough))
        {
            appendHorizontalLine(p.s->font->strikethroughPos, p.s->font->strikethroughWidth);
        }
    }
}

void BackendD3D::_drawCursorPart1(const RenderingPayload& p)
{
    _cursorRects.clear();

    if (p.cursorRect.empty())
    {
        return;
    }

    const auto cursorColor = p.s->cursor->cursorColor;
    const auto offset = p.cursorRect.top * p.colorBitmapRowStride;

    for (auto x1 = p.cursorRect.left; x1 < p.cursorRect.right; ++x1)
    {
        const auto x0 = x1;
        const auto bg = p.colorBitmap[offset + x1] | 0xff000000;

        for (; x1 < p.cursorRect.right && (p.colorBitmap[offset + x1] | 0xff000000) == bg; ++x1)
        {
        }

        const i16x2 position{
            p.s->font->cellSize.x * x0,
            p.s->font->cellSize.y * p.cursorRect.top,
        };
        const u16x2 size{
            static_cast<u16>(p.s->font->cellSize.x * (x1 - x0)),
            p.s->font->cellSize.y,
        };
        const auto color = cursorColor == 0xffffffff ? bg ^ 0x3f3f3f : cursorColor;
        auto& c0 = _cursorRects.emplace_back(CursorRect{ position, size, color });

        switch (static_cast<CursorType>(p.s->cursor->cursorType))
        {
        case CursorType::Legacy:
        {
            const auto height = (c0.size.y * p.s->cursor->heightPercentage + 50) / 100;
            c0.position.y += c0.size.y - height;
            c0.size.y = height;
            break;
        }
        case CursorType::VerticalBar:
            c0.size.x = p.s->font->thinLineWidth;
            break;
        case CursorType::Underscore:
            c0.position.y += p.s->font->underlinePos;
            c0.size.y = p.s->font->underlineWidth;
            break;
        case CursorType::EmptyBox:
        {
            auto& c1 = _cursorRects.emplace_back(c0);
            if (x0 == p.cursorRect.left)
            {
                auto& c = _cursorRects.emplace_back(c0);
                // Make line a little shorter vertically so it doesn't overlap with the top/bottom horizontal lines.
                c.position.y += p.s->font->thinLineWidth;
                c.size.y -= 2 * p.s->font->thinLineWidth;
                // The actual adjustment...
                c.size.x = p.s->font->thinLineWidth;
            }
            if (x1 == p.cursorRect.right)
            {
                auto& c = _cursorRects.emplace_back(c0);
                // Make line a little shorter vertically so it doesn't overlap with the top/bottom horizontal lines.
                c.position.y += p.s->font->thinLineWidth;
                c.size.y -= 2 * p.s->font->thinLineWidth;
                // The actual adjustment...
                c.position.x += c.size.x - p.s->font->thinLineWidth;
                c.size.x = p.s->font->thinLineWidth;
            }
            c0.size.y = p.s->font->thinLineWidth;
            c1.position.y += c1.size.y - p.s->font->thinLineWidth;
            c1.size.y = p.s->font->thinLineWidth;
            break;
        }
        case CursorType::FullBox:
            break;
        case CursorType::DoubleUnderscore:
        {
            auto& c1 = _cursorRects.emplace_back(c0);
            c0.position.y += p.s->font->doubleUnderlinePos.x;
            c0.size.y = p.s->font->thinLineWidth;
            c1.position.y += p.s->font->doubleUnderlinePos.y;
            c1.size.y = p.s->font->thinLineWidth;
            break;
        }
        default:
            break;
        }
    }

    if (cursorColor == 0xffffffff)
    {
        for (auto& c : _cursorRects)
        {
            _appendQuad() = {
                .shadingType = ShadingType::SolidFill,
                .position = c.position,
                .size = c.size,
                .color = c.color,
            };
            c.color = 0xffffffff;
        }
    }
}

void BackendD3D::_drawCursorPart2(const RenderingPayload& p)
{
    if (_cursorRects.empty())
    {
        return;
    }

    const auto color = p.s->cursor->cursorColor;

    if (color == 0xffffffff)
    {
        _markStateChange(_blendStateInvert.get());
    }

    for (const auto& c : _cursorRects)
    {
        _appendQuad() = {
            .shadingType = ShadingType::SolidFill,
            .position = c.position,
            .size = c.size,
            .color = c.color,
        };
    }

    if (color == 0xffffffff)
    {
        _markStateChange(_blendState.get());
    }
}

void BackendD3D::_drawSelection(const RenderingPayload& p)
{
    u16 y = 0;
    u16 lastFrom = 0;
    u16 lastTo = 0;

    for (const auto& row : p.rows)
    {
        if (row->selectionTo > row->selectionFrom)
        {
            // If the current selection line matches the previous one, we can just extend the previous quad downwards.
            // The way this is implemented isn't very smart, but we also don't have very many rows to iterate through.
            if (row->selectionFrom == lastFrom && row->selectionTo == lastTo)
            {
                _getLastQuad().size.y += p.s->font->cellSize.y;
            }
            else
            {
                _appendQuad() = {
                    .shadingType = ShadingType::SolidFill,
                    .position = {
                        p.s->font->cellSize.x * row->selectionFrom,
                        p.s->font->cellSize.y * y,
                    },
                    .size = {
                        static_cast<u16>(p.s->font->cellSize.x * (row->selectionTo - row->selectionFrom)),
                        p.s->font->cellSize.y,
                    },
                    .color = p.s->misc->selectionColor,
                };
                lastFrom = row->selectionFrom;
                lastTo = row->selectionTo;
            }
        }

        y++;
    }
}

#if ATLAS_DEBUG_SHOW_DIRTY
void BackendD3D::_debugShowDirty(const RenderingPayload& p)
{
    _presentRects[_presentRectsPos] = p.dirtyRectInPx;
    _presentRectsPos = (_presentRectsPos + 1) % std::size(_presentRects);

    for (size_t i = 0; i < std::size(_presentRects); ++i)
    {
        if (const auto& rect = _presentRects[i])
        {
            _appendQuad() = {
                .shadingType = ShadingType::SolidFill,
                .position = {
                    static_cast<i16>(rect.left),
                    static_cast<i16>(rect.top),
                },
                .size = {
                    static_cast<u16>(rect.right - rect.left),
                    static_cast<u16>(rect.bottom - rect.top),
                },
                .color = colorbrewer::pastel1[i] | 0x1f000000,
            };
        }
    }
}
#endif

#if ATLAS_DEBUG_DUMP_RENDER_TARGET
void BackendD3D::_debugDumpRenderTarget(const RenderingPayload& p)
{
    if (_dumpRenderTargetCounter == 0)
    {
        ExpandEnvironmentStringsW(ATLAS_DEBUG_DUMP_RENDER_TARGET_PATH, &_dumpRenderTargetBasePath[0], gsl::narrow_cast<DWORD>(std::size(_dumpRenderTargetBasePath)));
        std::filesystem::create_directories(_dumpRenderTargetBasePath);
    }

    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\%u_%08zu.png", &_dumpRenderTargetBasePath[0], GetCurrentProcessId(), _dumpRenderTargetCounter);
    SaveTextureToPNG(_deviceContext.get(), _swapChainManager.GetBuffer().get(), p.s->font->dpi, &path[0]);
    _dumpRenderTargetCounter++;
}
#endif

void BackendD3D::_executeCustomShader(RenderingPayload& p)
{
    {
        const CustomConstBuffer data{
            .time = std::chrono::duration<f32>(std::chrono::steady_clock::now() - _customShaderStartTime).count(),
            .scale = static_cast<f32>(p.s->font->dpi) / static_cast<f32>(USER_DEFAULT_SCREEN_DPI),
            .resolution = {
                static_cast<f32>(_cellCount.x * p.s->font->cellSize.x),
                static_cast<f32>(_cellCount.y * p.s->font->cellSize.y),
            },
            .background = colorFromU32Premultiply<f32x4>(p.s->misc->backgroundColor),
        };

        D3D11_MAPPED_SUBRESOURCE mapped{};
        THROW_IF_FAILED(_deviceContext->Map(_customShaderConstantBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        memcpy(mapped.pData, &data, sizeof(data));
        _deviceContext->Unmap(_customShaderConstantBuffer.get(), 0);
    }

    {
        // Before we do anything else we have to unbound _renderTargetView from being
        // a render target, otherwise we can't use it as a shader resource below.
        _deviceContext->OMSetRenderTargets(1, _customRenderTargetView.addressof(), nullptr);

        // IA: Input Assembler
        _deviceContext->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
        _deviceContext->IASetInputLayout(nullptr);
        _deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        _deviceContext->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);

        // VS: Vertex Shader
        _deviceContext->VSSetShader(_customVertexShader.get(), nullptr, 0);
        _deviceContext->VSSetConstantBuffers(0, 0, nullptr);

        // PS: Pixel Shader
        _deviceContext->PSSetShader(_customPixelShader.get(), nullptr, 0);
        _deviceContext->PSSetConstantBuffers(0, 1, _customShaderConstantBuffer.addressof());
        _deviceContext->PSSetShaderResources(0, 1, _customOffscreenTextureView.addressof());
        _deviceContext->PSSetSamplers(0, 1, _customShaderSamplerState.addressof());

        // OM: Output Merger
        _deviceContext->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    }

    _deviceContext->Draw(4, 0);

    {
        // IA: Input Assembler
        ID3D11Buffer* vertexBuffers[]{ _vertexBuffer.get(), _instanceBuffer.get() };
        static constexpr UINT strides[]{ sizeof(f32x2), sizeof(QuadInstance) };
        static constexpr UINT offsets[]{ 0, 0 };
        _deviceContext->IASetIndexBuffer(_indexBuffer.get(), DXGI_FORMAT_R16_UINT, 0);
        _deviceContext->IASetInputLayout(_inputLayout.get());
        _deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _deviceContext->IASetVertexBuffers(0, 2, &vertexBuffers[0], &strides[0], &offsets[0]);

        // VS: Vertex Shader
        _deviceContext->VSSetShader(_vertexShader.get(), nullptr, 0);
        _deviceContext->VSSetConstantBuffers(0, 1, _vsConstantBuffer.addressof());

        // PS: Pixel Shader
        ID3D11ShaderResourceView* resources[]{ _colorBitmapView.get(), _glyphAtlasView.get() };
        _deviceContext->PSSetShader(_pixelShader.get(), nullptr, 0);
        _deviceContext->PSSetConstantBuffers(0, 1, _psConstantBuffer.addressof());
        _deviceContext->PSSetShaderResources(0, 2, &resources[0]);
        _deviceContext->PSSetSamplers(0, 0, nullptr);

        // OM: Output Merger
        _deviceContext->OMSetBlendState(_blendState.get(), nullptr, 0xffffffff);
        _deviceContext->OMSetRenderTargets(1, _renderTargetView.addressof(), nullptr);
    }

    // With custom shaders, everything might be invalidated, so we have to
    // indirectly disable Present1() and its dirty rects this way.
    p.dirtyRectInPx = { 0, 0, p.s->targetSize.x, p.s->targetSize.y };
}

TIL_FAST_MATH_END