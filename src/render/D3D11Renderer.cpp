#include "D3D11Renderer.h"
#include "Logger.h"
#include <algorithm>

D3D11Renderer::D3D11Renderer(ID3D11Device* device, ID3D11DeviceContext* context)
    : m_device(device), m_context(context) {}

D3D11Renderer::~D3D11Renderer() {
    Shutdown();
}

void D3D11Renderer::Shutdown() {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    m_outputView = nullptr;
    m_videoProcessor = nullptr;
    m_videoProcessorEnum = nullptr;
    m_videoContext = nullptr;
    m_videoDevice = nullptr;
    m_renderTargetView = nullptr;
    m_swapChain = nullptr;
    m_isInitialized = false;
}

bool D3D11Renderer::Initialize(HWND hwnd, int width, int height) {
    Shutdown();

    std::lock_guard<std::mutex> lock(m_renderMutex);
    m_hwnd = hwnd;
    m_windowWidth = width > 0 ? width : 1280;
    m_windowHeight = height > 0 ? height : 720;

    if (!CreateSwapChain(m_windowWidth, m_windowHeight)) {
        return false;
    }

    HRESULT hr = m_device.As(&m_videoDevice);
    if (FAILED(hr)) {
        Logger::E("D3D11Renderer", "Failed to query ID3D11VideoDevice");
        return false;
    }

    hr = m_context.As(&m_videoContext);
    if (FAILED(hr)) {
        Logger::E("D3D11Renderer", "Failed to query ID3D11VideoContext");
        return false;
    }

    m_isInitialized = true;
    Logger::I("D3D11Renderer", "Renderer initialized (" + std::to_string(m_windowWidth) + "x" + std::to_string(m_windowHeight) + ", Tearing=" + std::to_string(m_allowTearing) + ")");
    return true;
}

bool D3D11Renderer::CreateSwapChain(int width, int height) {
    if (!m_device || !m_hwnd) return false;

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(m_device.As(&dxgiDevice))) return false;

    Microsoft::WRL::ComPtr<IDXGIAdapter> dxgiAdapter;
    if (FAILED(dxgiDevice->GetAdapter(&dxgiAdapter))) return false;

    Microsoft::WRL::ComPtr<IDXGIFactory2> dxgiFactory;
    if (FAILED(dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory)))) return false;

    BOOL allowTearing = FALSE;
    Microsoft::WRL::ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(dxgiFactory.As(&factory5))) {
        factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));
    }
    m_allowTearing = (allowTearing == TRUE);

    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.Width = width;
    scDesc.Height = height;
    scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scDesc.Stereo = FALSE;
    scDesc.SampleDesc.Count = 1;
    scDesc.SampleDesc.Quality = 0;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = 2;
    scDesc.Scaling = DXGI_SCALING_STRETCH;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    scDesc.Flags = m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    HRESULT hr = dxgiFactory->CreateSwapChainForHwnd(
        m_device.Get(),
        m_hwnd,
        &scDesc,
        nullptr,
        nullptr,
        &m_swapChain
    );

    if (FAILED(hr)) {
        Logger::E("D3D11Renderer", "Failed to create SwapChainForHwnd, hr = " + std::to_string(hr));
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) return false;

    hr = m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_renderTargetView);
    return SUCCEEDED(hr);
}

void D3D11Renderer::Resize(int width, int height) {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    if (!m_swapChain || width <= 0 || height <= 0) return;

    m_windowWidth = width;
    m_windowHeight = height;

    m_renderTargetView = nullptr;
    m_outputView = nullptr;
    m_videoProcessor = nullptr;
    m_videoProcessorEnum = nullptr;

    UINT flags = m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, flags);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    if (SUCCEEDED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_renderTargetView);
    }
}

bool D3D11Renderer::SetupVideoProcessor(int inWidth, int inHeight, DXGI_FORMAT inFormat) {
    (void)inFormat;
    if (!m_videoDevice || !m_swapChain || inWidth <= 0 || inHeight <= 0) return false;

    if (m_videoProcessor && m_lastVideoWidth == inWidth && m_lastVideoHeight == inHeight && m_outputView) {
        return true;
    }

    m_lastVideoWidth = inWidth;
    m_lastVideoHeight = inHeight;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
    contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    contentDesc.InputWidth = inWidth;
    contentDesc.InputHeight = inHeight;
    contentDesc.OutputWidth = m_windowWidth;
    contentDesc.OutputHeight = m_windowHeight;
    contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    HRESULT hr = m_videoDevice->CreateVideoProcessorEnumerator(&contentDesc, &m_videoProcessorEnum);
    if (FAILED(hr)) return false;

    hr = m_videoDevice->CreateVideoProcessor(m_videoProcessorEnum.Get(), 0, &m_videoProcessor);
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) return false;

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outViewDesc = {};
    outViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outViewDesc.Texture2D.MipSlice = 0;
    hr = m_videoDevice->CreateVideoProcessorOutputView(backBuffer.Get(), m_videoProcessorEnum.Get(), &outViewDesc, &m_outputView);
    return SUCCEEDED(hr);
}

void D3D11Renderer::RenderFrame(ID3D11Texture2D* frameTexture, int videoWidth, int videoHeight, const std::wstring& statsText) {
    (void)statsText;
    std::lock_guard<std::mutex> lock(m_renderMutex);
    if (!m_isInitialized || !m_swapChain || !frameTexture || !m_videoContext || !m_context) return;

    D3D11_TEXTURE2D_DESC desc;
    frameTexture->GetDesc(&desc);

    int inW = videoWidth > 0 ? videoWidth : desc.Width;
    int inH = videoHeight > 0 ? videoHeight : desc.Height;

    if (!SetupVideoProcessor(inW, inH, desc.Format)) {
        return;
    }

    // 1. Aspect Ratio Preservation (Letterbox / Pillarbox)
    float videoAspect = static_cast<float>(inW) / static_cast<float>(inH);
    float windowAspect = static_cast<float>(m_windowWidth) / static_cast<float>(m_windowHeight);

    RECT destRect = {};
    if (windowAspect > videoAspect) {
        int renderW = static_cast<int>(m_windowHeight * videoAspect);
        destRect.left = (m_windowWidth - renderW) / 2;
        destRect.right = destRect.left + renderW;
        destRect.top = 0;
        destRect.bottom = m_windowHeight;
    } else {
        int renderH = static_cast<int>(m_windowWidth / videoAspect);
        destRect.left = 0;
        destRect.right = m_windowWidth;
        destRect.top = (m_windowHeight - renderH) / 2;
        destRect.bottom = destRect.top + renderH;
    }

    RECT srcRect = { 0, 0, inW, inH };

    m_videoContext->VideoProcessorSetStreamSourceRect(m_videoProcessor.Get(), 0, TRUE, &srcRect);
    m_videoContext->VideoProcessorSetStreamDestRect(m_videoProcessor.Get(), 0, TRUE, &destRect);

    if (m_renderTargetView) {
        const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        m_context->ClearRenderTargetView(m_renderTargetView.Get(), black);
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inViewDesc = {};
    inViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inViewDesc.Texture2D.MipSlice = 0;
    inViewDesc.Texture2D.ArraySlice = 0;

    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inputView;
    HRESULT hr = m_videoDevice->CreateVideoProcessorInputView(frameTexture, m_videoProcessorEnum.Get(), &inViewDesc, &inputView);
    if (FAILED(hr)) return;

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.Get();

    hr = m_videoContext->VideoProcessorBlt(m_videoProcessor.Get(), m_outputView.Get(), 0, 1, &stream);
    if (SUCCEEDED(hr)) {
        UINT presentFlags = m_allowTearing ? DXGI_PRESENT_ALLOW_TEARING : DXGI_PRESENT_DO_NOT_WAIT;
        m_swapChain->Present(0, presentFlags);
    }
}
