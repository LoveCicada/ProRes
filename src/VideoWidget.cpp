#include "VideoWidget.h"

#include "Player.h"

#include <QPaintEngine>
#include <QResizeEvent>
#include <QShowEvent>

#include <cstring>
#include <memory>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

const char* kShader = R"(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};
VSOut vsMain(float2 pos : POSITION, float2 uv : TEXCOORD0) {
    VSOut o;
    o.pos = float4(pos, 0.0, 1.0);
    o.uv = uv;
    return o;
}
Texture2D frameTex : register(t0);
SamplerState samp : register(s0);
cbuffer Params : register(b0) { float useAlpha; float3 pad; };
float4 psMain(VSOut i) : SV_Target {
    float4 c = frameTex.Sample(samp, i.uv);
    float2 cell = floor(i.uv * float2(28.0, 16.0));
    float checker = fmod(cell.x + cell.y, 2.0);
    float3 bg = lerp(float3(0.15, 0.13, 0.11), float3(0.32, 0.28, 0.22), checker);
    float a = useAlpha > 0.5 ? c.a : 1.0;
    return float4(lerp(bg, c.rgb, a), 1.0);
}
)";

struct Vertex {
    float x, y, u, v;
};

} // namespace

struct VideoWidget::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> vertices;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    int texW = 0;
    int texH = 0;
    struct SharedView {
        quintptr handle = 0;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> srv;
    };
    std::vector<SharedView> shared;
    std::shared_ptr<void> held[2];
    int hold = 0;
    bool ready = false;
    bool hasLuid = false;
    qint64 luidHigh = 0;
    quint32 luidLow = 0;
};

VideoWidget::VideoWidget(QWidget* parent)
    : QWidget(parent)
    , d(std::make_unique<Impl>())
{
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
    setMinimumHeight(280);
}

VideoWidget::~VideoWidget() = default;

QPaintEngine* VideoWidget::paintEngine() const
{
    return nullptr;
}

void VideoWidget::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    ensureDevice();
    applyRound();
}

void VideoWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    resizeSwapChain();
    applyRound();
}

void VideoWidget::paintEvent(QPaintEvent*)
{
}

void VideoWidget::applyRound()
{
    if (!internalWinId())
        return;
    const int radius = 18;
    HRGN region = CreateRoundRectRgn(0, 0, width() + 1, height() + 1, radius, radius);
    SetWindowRgn(reinterpret_cast<HWND>(winId()), region, TRUE);
}

void VideoWidget::releaseDevice()
{
    if (d->context)
        d->context->OMSetRenderTargets(0, nullptr, nullptr);
    d->target.Reset();
    d->srv.Reset();
    d->texture.Reset();
    d->shared.clear();
    d->held[0].reset();
    d->held[1].reset();
    d->texW = 0;
    d->texH = 0;
    d->swap.Reset();
    d->vs.Reset();
    d->ps.Reset();
    d->layout.Reset();
    d->vertices.Reset();
    d->constants.Reset();
    d->sampler.Reset();
    d->context.Reset();
    d->device.Reset();
    d->ready = false;
}

void VideoWidget::setAdapter(qint64 luidHigh, quint32 luidLow)
{
    if (d->hasLuid && d->luidHigh == luidHigh && d->luidLow == luidLow)
        return;
    d->hasLuid = true;
    d->luidHigh = luidHigh;
    d->luidLow = luidLow;
    releaseDevice();
    ensureDevice();
}

void VideoWidget::ensureDevice()
{
    if (d->ready || width() < 2 || height() < 2)
        return;

    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 1;
    desc.BufferDesc.Width = static_cast<UINT>(width());
    desc.BufferDesc.Height = static_cast<UINT>(height());
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = reinterpret_cast<HWND>(winId());
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ComPtr<IDXGIAdapter1> adapter;
    if (d->hasLuid) {
        ComPtr<IDXGIFactory1> factory;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) {
            ComPtr<IDXGIAdapter1> current;
            for (UINT i = 0; factory->EnumAdapters1(i, current.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 info = {};
                if (FAILED(current->GetDesc1(&info)))
                    continue;
                if (info.AdapterLuid.HighPart == d->luidHigh
                    && info.AdapterLuid.LowPart == d->luidLow) {
                    adapter = current;
                    break;
                }
            }
        }
    }

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL chosen = D3D_FEATURE_LEVEL_11_0;
    const D3D_DRIVER_TYPE driver = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        adapter.Get(), driver, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, 2, D3D11_SDK_VERSION, &desc, d->swap.GetAddressOf(),
        d->device.GetAddressOf(), &chosen, d->context.GetAddressOf());
    if (FAILED(hr))
        return;

    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> psBlob;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr,
                          "vsMain", "vs_4_0", 0, 0, vsBlob.GetAddressOf(), errors.GetAddressOf())))
        return;
    errors.Reset();
    if (FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr,
                          "psMain", "ps_4_0", 0, 0, psBlob.GetAddressOf(), errors.GetAddressOf())))
        return;

    d->device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, d->vs.GetAddressOf());
    d->device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, d->ps.GetAddressOf());

    const D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    d->device->CreateInputLayout(il, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), d->layout.GetAddressOf());

    const Vertex verts[] = {
        { -1.f, -1.f, 0.f, 1.f },
        { -1.f,  1.f, 0.f, 0.f },
        {  1.f, -1.f, 1.f, 1.f },
        {  1.f,  1.f, 1.f, 0.f },
    };
    D3D11_BUFFER_DESC vb = {};
    vb.ByteWidth = sizeof(verts);
    vb.Usage = D3D11_USAGE_IMMUTABLE;
    vb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = verts;
    d->device->CreateBuffer(&vb, &init, d->vertices.GetAddressOf());

    D3D11_BUFFER_DESC cb = {};
    cb.ByteWidth = 16;
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    d->device->CreateBuffer(&cb, nullptr, d->constants.GetAddressOf());

    D3D11_SAMPLER_DESC samp = {};
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d->device->CreateSamplerState(&samp, d->sampler.GetAddressOf());

    resizeSwapChain();
    d->ready = d->target != nullptr;
}

void VideoWidget::resizeSwapChain()
{
    if (!d->swap || width() < 2 || height() < 2)
        return;
    d->target.Reset();
    d->context->OMSetRenderTargets(0, nullptr, nullptr);
    d->swap->ResizeBuffers(0, static_cast<UINT>(width()), static_cast<UINT>(height()),
                           DXGI_FORMAT_UNKNOWN, 0);
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(d->swap->GetBuffer(0, IID_PPV_ARGS(back.GetAddressOf()))))
        return;
    d->device->CreateRenderTargetView(back.Get(), nullptr, d->target.GetAddressOf());
}

void VideoWidget::clear()
{
    ensureDevice();
    if (!d->ready)
        return;
    const float black[4] = { 0.08f, 0.07f, 0.05f, 1.f };
    d->context->OMSetRenderTargets(1, d->target.GetAddressOf(), nullptr);
    d->context->ClearRenderTargetView(d->target.Get(), black);
    d->swap->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
}

void VideoWidget::releaseShared()
{
    if (!d)
        return;
    d->shared.clear();
    d->held[0].reset();
    d->held[1].reset();
}

void VideoWidget::present(const VideoFrame& frame)
{
    ensureDevice();
    if (!d->ready || frame.width < 1 || frame.height < 1)
        return;
    if (!frame.zeroCopy && frame.bgra.empty())
        return;

    ID3D11ShaderResourceView* source = nullptr;
    ComPtr<ID3D11ShaderResourceView> sharedSrv;
    if (frame.zeroCopy) {
        d->held[d->hold] = frame.slot;
        d->hold ^= 1;
        Impl::SharedView* found = nullptr;
        for (auto& item : d->shared) {
            if (item.handle == frame.sharedHandle) {
                found = &item;
                break;
            }
        }
        if (!found) {
            ComPtr<ID3D11Device1> device1;
            if (FAILED(d->device.As(&device1)))
                return;
            ComPtr<ID3D11Texture2D> opened;
            if (FAILED(device1->OpenSharedResource1(reinterpret_cast<HANDLE>(frame.sharedHandle),
                                                    IID_PPV_ARGS(opened.GetAddressOf()))))
                return;
            D3D11_SHADER_RESOURCE_VIEW_DESC view = {};
            view.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
            view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipLevels = 1;
            if (FAILED(d->device->CreateShaderResourceView(opened.Get(), &view, sharedSrv.GetAddressOf())))
                return;
            Impl::SharedView item;
            item.handle = frame.sharedHandle;
            item.texture = opened;
            item.srv = sharedSrv;
            d->shared.push_back(item);
            source = sharedSrv.Get();
        } else {
            source = found->srv.Get();
        }
    } else if (d->texW != frame.width || d->texH != frame.height) {
        d->srv.Reset();
        d->texture.Reset();
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(frame.width);
        td.Height = static_cast<UINT>(frame.height);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(d->device->CreateTexture2D(&td, nullptr, d->texture.GetAddressOf())))
            return;
        if (FAILED(d->device->CreateShaderResourceView(d->texture.Get(), nullptr, d->srv.GetAddressOf())))
            return;
        d->texW = frame.width;
        d->texH = frame.height;
    }

    if (!frame.zeroCopy) {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(d->context->Map(d->texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return;
        const int srcPitch = frame.width * 4;
        auto* dst = static_cast<uint8_t*>(mapped.pData);
        for (int y = 0; y < frame.height; ++y)
            memcpy(dst + y * mapped.RowPitch, frame.bgra.data() + y * srcPitch, static_cast<size_t>(srcPitch));
        d->context->Unmap(d->texture.Get(), 0);
        source = d->srv.Get();
    }
    if (!source)
        return;

    D3D11_MAPPED_SUBRESOURCE cb = {};
    if (SUCCEEDED(d->context->Map(d->constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &cb))) {
        float params[4] = { frame.alpha ? 1.f : 0.f, 0.f, 0.f, 0.f };
        memcpy(cb.pData, params, sizeof(params));
        d->context->Unmap(d->constants.Get(), 0);
    }

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(width());
    vp.Height = static_cast<float>(height());
    vp.MaxDepth = 1.f;
    d->context->RSSetViewports(1, &vp);
    d->context->OMSetRenderTargets(1, d->target.GetAddressOf(), nullptr);
    const float clear[4] = { 0.08f, 0.07f, 0.05f, 1.f };
    d->context->ClearRenderTargetView(d->target.Get(), clear);
    d->context->IASetInputLayout(d->layout.Get());
    d->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    const UINT stride = sizeof(Vertex);
    const UINT offset = 0;
    d->context->IASetVertexBuffers(0, 1, d->vertices.GetAddressOf(), &stride, &offset);
    d->context->VSSetShader(d->vs.Get(), nullptr, 0);
    d->context->PSSetShader(d->ps.Get(), nullptr, 0);
    d->context->PSSetShaderResources(0, 1, &source);
    d->context->PSSetSamplers(0, 1, d->sampler.GetAddressOf());
    d->context->PSSetConstantBuffers(0, 1, d->constants.GetAddressOf());
    d->context->Draw(4, 0);
    ID3D11ShaderResourceView* empty = nullptr;
    d->context->PSSetShaderResources(0, 1, &empty);
    d->context->Flush();
    d->swap->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
}
