#include "FfmpegVideoDecoder.h"
#include "Logger.h"
#include <algorithm>

extern "C" {
#include <libavutil/opt.h>
}

enum AVPixelFormat FfmpegVideoDecoder::GetHwFormat(AVCodecContext* /*ctx*/, const enum AVPixelFormat* pix_fmts) {
    for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_D3D11) {
            return *p;
        }
    }
    return pix_fmts[0];
}

FfmpegVideoDecoder::FfmpegVideoDecoder(ID3D11Device* d3d11Device)
    : m_device(d3d11Device) {
    if (m_device) {
        m_device->GetImmediateContext(&m_context);
    }
}

FfmpegVideoDecoder::~FfmpegVideoDecoder() {
    Shutdown();
}

void FfmpegVideoDecoder::Shutdown() {
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }
    if (m_hwDeviceCtx) {
        av_buffer_unref(&m_hwDeviceCtx);
        m_hwDeviceCtx = nullptr;
    }
    if (m_frame) {
        av_frame_free(&m_frame);
        m_frame = nullptr;
    }
    if (m_swFrame) {
        av_frame_free(&m_swFrame);
        m_swFrame = nullptr;
    }
    if (m_pkt) {
        av_packet_free(&m_pkt);
        m_pkt = nullptr;
    }
    m_renderNv12Texture = nullptr;
    m_fallbackNv12Texture = nullptr;
    m_isInitialized = false;
    m_isHwAccelActive = false;
    Logger::I("FfmpegVideoDecoder", "FFmpeg Video Decoder shutdown complete.");
}

bool FfmpegVideoDecoder::EnsureRenderTexture(int width, int height) {
    int w = (width / 2) * 2;
    int h = (height / 2) * 2;
    if (m_renderNv12Texture && m_renderWidth == w && m_renderHeight == h) {
        return true;
    }
    if (!m_device || w <= 0 || h <= 0) return false;

    m_renderWidth = w;
    m_renderHeight = h;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = m_renderWidth;
    desc.Height = m_renderHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_renderNv12Texture);
    if (FAILED(hr)) {
        Logger::E("FfmpegVideoDecoder", "Failed to create render NV12 texture, hr = 0x" + std::to_string(hr));
        return false;
    }
    return true;
}

bool FfmpegVideoDecoder::EnsureFallbackTexture(int width, int height) {
    int w = (width / 2) * 2;
    int h = (height / 2) * 2;
    if (m_fallbackNv12Texture && m_fallbackWidth == w && m_fallbackHeight == h) {
        return true;
    }
    if (!m_device || w <= 0 || h <= 0) return false;

    m_fallbackWidth = w;
    m_fallbackHeight = h;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = m_fallbackWidth;
    desc.Height = m_fallbackHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_fallbackNv12Texture);
    if (FAILED(hr)) {
        Logger::E("FfmpegVideoDecoder", "Failed to create fallback NV12 texture, hr = 0x" + std::to_string(hr));
        return false;
    }
    return true;
}

bool FfmpegVideoDecoder::Initialize(VideoCodecType codecType) {
    Shutdown();

    m_codecType = codecType;
    AVCodecID codecId = (m_codecType == VideoCodecType::H265_HEVC) ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
    std::string codecName = (m_codecType == VideoCodecType::H265_HEVC) ? "HEVC" : "H.264";

    const AVCodec* codec = avcodec_find_decoder(codecId);
    if (!codec) {
        Logger::E("FfmpegVideoDecoder", "FFmpeg codec not found for " + codecName);
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        Logger::E("FfmpegVideoDecoder", "Failed to allocate AVCodecContext.");
        return false;
    }

    // Set Low-Latency real-time streaming flags (Zero DPB delay)
    m_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    m_codecCtx->thread_count = 1; // Zero-frame buffering for interactive casting
    m_codecCtx->thread_type = FF_THREAD_SLICE;
    m_codecCtx->delay = 0;
    m_codecCtx->has_b_frames = 0;
    m_codecCtx->max_b_frames = 0;

    // Zero-latency options
    av_opt_set(m_codecCtx->priv_data, "tune", "zerolatency", 0);

    // Try initializing D3D11VA Hardware Acceleration
    if (m_device) {
        m_hwDeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (m_hwDeviceCtx) {
            AVHWDeviceContext* devCtx = reinterpret_cast<AVHWDeviceContext*>(m_hwDeviceCtx->data);
            AVD3D11VADeviceContext* d3d11Ctx = reinterpret_cast<AVD3D11VADeviceContext*>(devCtx->hwctx);
            d3d11Ctx->device = m_device.Get();
            d3d11Ctx->device->AddRef();

            if (av_hwdevice_ctx_init(m_hwDeviceCtx) >= 0) {
                m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
                m_codecCtx->get_format = FfmpegVideoDecoder::GetHwFormat;
                m_isHwAccelActive = true;
                Logger::I("FfmpegVideoDecoder", "D3D11VA Hardware Acceleration initialized for " + codecName);
            } else {
                av_buffer_unref(&m_hwDeviceCtx);
                m_hwDeviceCtx = nullptr;
                Logger::W("FfmpegVideoDecoder", "Failed to init D3D11VA hwdevice, falling back to software.");
            }
        }
    }

    int ret = avcodec_open2(m_codecCtx, codec, nullptr);
    if (ret < 0) {
        char errBuf[256] = {};
        av_strerror(ret, errBuf, sizeof(errBuf));
        Logger::E("FfmpegVideoDecoder", "Failed to open FFmpeg codec " + codecName + ": " + std::string(errBuf));
        Shutdown();
        return false;
    }

    m_frame = av_frame_alloc();
    m_swFrame = av_frame_alloc();
    m_pkt = av_packet_alloc();

    m_isInitialized = true;
    m_lastStatsTime = std::chrono::steady_clock::now();
    m_statsFramesInput = 0;
    m_statsFramesDecoded = 0;
    m_statsHwFrames = 0;
    m_statsSwFrames = 0;
    m_statsTotalDecodeMs = 0.0;
    m_statsMaxDecodeMs = 0.0;
    m_statsErrors = 0;

    Logger::I("FfmpegVideoDecoder", "FFmpeg Video Decoder initialized successfully (" + codecName + ", HW=" + std::to_string(m_isHwAccelActive) + ")");
    return true;
}

bool FfmpegVideoDecoder::DecodeNalu(const uint8_t* data, size_t size, int64_t timestampMs) {
    if (!m_isInitialized || !m_codecCtx || !data || size == 0) return false;

    auto decodeStart = std::chrono::steady_clock::now();
    m_statsFramesInput++;

    if (m_paddedPktBuffer.size() < size + AV_INPUT_BUFFER_PADDING_SIZE) {
        m_paddedPktBuffer.resize(size + AV_INPUT_BUFFER_PADDING_SIZE + 4096);
    }
    std::memcpy(m_paddedPktBuffer.data(), data, size);
    std::memset(m_paddedPktBuffer.data() + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    m_pkt->data = m_paddedPktBuffer.data();
    m_pkt->size = static_cast<int>(size);
    m_pkt->pts = timestampMs;
    m_pkt->dts = timestampMs;

    int ret = avcodec_send_packet(m_codecCtx, m_pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        m_statsErrors++;
        char errBuf[256] = {};
        av_strerror(ret, errBuf, sizeof(errBuf));
        Logger::W("FfmpegVideoDecoder", "[DECODE_SEND_ERR] avcodec_send_packet failed (" + std::to_string(ret) + "): " + errBuf);
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(m_codecCtx, m_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            m_statsErrors++;
            char errBuf[256] = {};
            av_strerror(ret, errBuf, sizeof(errBuf));
            Logger::W("FfmpegVideoDecoder", "[DECODE_RECV_ERR] avcodec_receive_frame failed (" + std::to_string(ret) + "): " + errBuf);
            break;
        }

        m_statsFramesDecoded++;
        ProcessDecodedFrame(m_frame, timestampMs);
        av_frame_unref(m_frame);
    }

    m_pkt->data = nullptr;
    m_pkt->size = 0;

    double decodeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - decodeStart).count();
    m_statsTotalDecodeMs += decodeMs;
    if (decodeMs > m_statsMaxDecodeMs) m_statsMaxDecodeMs = decodeMs;
    if (decodeMs > 15.0) {
        Logger::W("FfmpegVideoDecoder", "[SLOW_DECODE] Decode took " + std::to_string(decodeMs) +
                  " ms for frame (size=" + std::to_string(size) + " bytes, pts=" + std::to_string(timestampMs) +
                  ", HW=" + std::to_string(m_isHwAccelActive) + ")");
    }

    LogPeriodicStats();
    return true;
}

void FfmpegVideoDecoder::ProcessDecodedFrame(AVFrame* frame, int64_t timestampMs) {
    if (!frame || frame->width <= 0 || frame->height <= 0) return;

    // 1. Direct D3D11VA Hardware Accelerated Frame
    if (frame->format == AV_PIX_FMT_D3D11) {
        m_statsHwFrames++;
        ID3D11Texture2D* hwTexture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
        intptr_t sliceIndex = reinterpret_cast<intptr_t>(frame->data[1]);

        if (hwTexture) {
            D3D11_TEXTURE2D_DESC hwDesc;
            hwTexture->GetDesc(&hwDesc);

            if (EnsureRenderTexture(hwDesc.Width, hwDesc.Height) && m_context) {
                m_context->CopySubresourceRegion(
                    m_renderNv12Texture.Get(), 0, 0, 0, 0,
                    hwTexture, static_cast<UINT>(sliceIndex), nullptr
                );
                if (m_decodedCallback) {
                    m_decodedCallback(m_renderNv12Texture.Get(), timestampMs, frame->width, frame->height);
                }
                return;
            }
        }
    }

    // 2. Software Fallback Frame (YUV420P / NV12 in CPU memory)
    m_statsSwFrames++;
    AVFrame* srcFrame = frame;
    if (frame->format == AV_PIX_FMT_D3D11) {
        if (av_hwframe_transfer_data(m_swFrame, frame, 0) >= 0) {
            srcFrame = m_swFrame;
        }
    }

    if (EnsureFallbackTexture(srcFrame->width, srcFrame->height) && m_context) {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = m_context->Map(m_fallbackNv12Texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            uint8_t* pDst = reinterpret_cast<uint8_t*>(mapped.pData);
            int dstPitch = mapped.RowPitch;

            if (srcFrame->format == AV_PIX_FMT_NV12) {
                for (int y = 0; y < srcFrame->height; ++y) {
                    std::memcpy(pDst + y * dstPitch, srcFrame->data[0] + y * srcFrame->linesize[0], srcFrame->width);
                }
                uint8_t* pDstUV = pDst + dstPitch * srcFrame->height;
                for (int y = 0; y < srcFrame->height / 2; ++y) {
                    std::memcpy(pDstUV + y * dstPitch, srcFrame->data[1] + y * srcFrame->linesize[1], srcFrame->width);
                }
            } else if (srcFrame->format == AV_PIX_FMT_YUV420P) {
                for (int y = 0; y < srcFrame->height; ++y) {
                    std::memcpy(pDst + y * dstPitch, srcFrame->data[0] + y * srcFrame->linesize[0], srcFrame->width);
                }
                uint8_t* pDstUV = pDst + dstPitch * srcFrame->height;
                const uint8_t* pSrcU = srcFrame->data[1];
                const uint8_t* pSrcV = srcFrame->data[2];
                int halfW = srcFrame->width / 2;
                int halfH = srcFrame->height / 2;

                for (int y = 0; y < halfH; ++y) {
                    uint8_t* dstLine = pDstUV + y * dstPitch;
                    const uint8_t* uLine = pSrcU + y * srcFrame->linesize[1];
                    const uint8_t* vLine = pSrcV + y * srcFrame->linesize[2];
                    for (int x = 0; x < halfW; ++x) {
                        dstLine[x * 2 + 0] = uLine[x];
                        dstLine[x * 2 + 1] = vLine[x];
                    }
                }
            }
            m_context->Unmap(m_fallbackNv12Texture.Get(), 0);

            if (m_decodedCallback) {
                m_decodedCallback(m_fallbackNv12Texture.Get(), timestampMs, srcFrame->width, srcFrame->height);
            }
        }
    }

    if (srcFrame == m_swFrame) {
        av_frame_unref(m_swFrame);
    }
}

void FfmpegVideoDecoder::LogPeriodicStats() {
    auto now = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastStatsTime).count();
    if (elapsedMs >= 1000) {
        double avgDecodeMs = m_statsFramesInput > 0 ? (m_statsTotalDecodeMs / m_statsFramesInput) : 0.0;
        Logger::I("FfmpegVideoDecoder", "[STATS 1s] In: " + std::to_string(m_statsFramesInput) + " pkts, Decoded: " +
                  std::to_string(m_statsFramesDecoded) + " frames (HW: " + std::to_string(m_statsHwFrames) +
                  ", SW: " + std::to_string(m_statsSwFrames) + "), Avg Decode: " +
                  std::to_string(avgDecodeMs) + " ms, Max: " + std::to_string(m_statsMaxDecodeMs) +
                  " ms, Errors: " + std::to_string(m_statsErrors));

        m_statsFramesInput = 0;
        m_statsFramesDecoded = 0;
        m_statsHwFrames = 0;
        m_statsSwFrames = 0;
        m_statsTotalDecodeMs = 0.0;
        m_statsMaxDecodeMs = 0.0;
        m_statsErrors = 0;
        m_lastStatsTime = now;
    }
}
