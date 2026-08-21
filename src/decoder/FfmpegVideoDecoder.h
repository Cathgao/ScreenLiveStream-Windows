#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <functional>
#include <string>
#include <atomic>
#include <vector>
#include <chrono>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

#include "../encoder/WmfVideoEncoder.h" // For VideoCodecType enum

class FfmpegVideoDecoder {
public:
    using DecodedFrameCallback = std::function<void(ID3D11Texture2D* texture, int64_t timestampMs, int width, int height)>;

    FfmpegVideoDecoder(ID3D11Device* d3d11Device);
    ~FfmpegVideoDecoder();

    bool Initialize(VideoCodecType codecType);
    void Shutdown();
    bool IsInitialized() const { return m_isInitialized; }
    VideoCodecType GetCodecType() const { return m_codecType; }

    bool DecodeNalu(const uint8_t* data, size_t size, int64_t timestampMs);
    void SetDecodedCallback(DecodedFrameCallback cb) { m_decodedCallback = cb; }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;

    AVCodecContext* m_codecCtx = nullptr;
    AVBufferRef* m_hwDeviceCtx = nullptr;
    AVFrame* m_frame = nullptr;
    AVFrame* m_swFrame = nullptr;
    AVPacket* m_pkt = nullptr;
    std::vector<uint8_t> m_paddedPktBuffer;

    VideoCodecType m_codecType = VideoCodecType::H265_HEVC;
    bool m_isInitialized = false;
    bool m_isHwAccelActive = false;

    // GPU-side D3D11 render texture for D3D11VA hardware frame copying
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_renderNv12Texture;
    int m_renderWidth = 0;
    int m_renderHeight = 0;

    // CPU dynamic upload texture for software decoding fallback
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_fallbackNv12Texture;
    int m_fallbackWidth = 0;
    int m_fallbackHeight = 0;

    DecodedFrameCallback m_decodedCallback;

    // 1-second Periodic Stats Tracking
    std::chrono::steady_clock::time_point m_lastStatsTime;
    uint32_t m_statsFramesInput = 0;
    uint32_t m_statsFramesDecoded = 0;
    uint32_t m_statsHwFrames = 0;
    uint32_t m_statsSwFrames = 0;
    double m_statsTotalDecodeMs = 0.0;
    double m_statsMaxDecodeMs = 0.0;
    uint32_t m_statsErrors = 0;

    static enum AVPixelFormat GetHwFormat(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts);
    bool EnsureRenderTexture(int width, int height);
    bool EnsureFallbackTexture(int width, int height);
    void ProcessDecodedFrame(AVFrame* frame, int64_t timestampMs);
    void LogPeriodicStats();
};
