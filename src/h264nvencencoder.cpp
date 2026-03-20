/*
    SPDX-FileCopyrightText: 2026

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "h264nvencencoder_p.h"

#include <QSize>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
}

#include "logging_record.h"

#ifndef AV_PROFILE_H264_CONSTRAINED_BASELINE // ffmpeg before 8.0
#define AV_PROFILE_H264_CONSTRAINED_BASELINE FF_PROFILE_H264_CONSTRAINED_BASELINE
#define AV_PROFILE_H264_MAIN FF_PROFILE_H264_MAIN
#define AV_PROFILE_H264_HIGH FF_PROFILE_H264_HIGH
#endif

using namespace std::string_literals;

H264NVENCEncoder::H264NVENCEncoder(H264Profile profile, PipeWireProduce *produce)
    : SoftwareEncoder(produce)
    , m_profile(profile)
{
}

H264NVENCEncoder::~H264NVENCEncoder()
{
    if (m_cudaDeviceContext) {
        av_buffer_unref(&m_cudaDeviceContext);
    }
}

bool H264NVENCEncoder::initialize(const QSize &size)
{
    // Create a CUDA device explicitly (GPU 0 by default)
    int err = av_hwdevice_ctx_create(&m_cudaDeviceContext, AV_HWDEVICE_TYPE_CUDA, "0", NULL, 0);
    if (err < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to create CUDA device. Error" << av_err2str(err);
        return false;
    }

    m_filterGraphToParse = QStringLiteral("format=pix_fmts=nv12,hwupload_cuda");
    if (!createFilterGraph(size)) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to create base filter graph for NVENC";
        return false;
    }

    for (auto i = 0u; i < m_avFilterGraph->nb_filters; ++i) {
        m_avFilterGraph->filters[i]->hw_device_ctx = av_buffer_ref(m_cudaDeviceContext);
    }
    int ret = avfilter_graph_config(m_avFilterGraph, nullptr);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed configuring CUDA upload filter graph" << av_err2str(ret);
        return false;
    }

    auto codec = avcodec_find_encoder_by_name("h264_nvenc");
    if (!codec) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "h264_nvenc codec not found";
        return false;
    }

    m_avCodecContext = avcodec_alloc_context3(codec);
    if (!m_avCodecContext) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not allocate video codec context";
        return false;
    }

    Q_ASSERT(!size.isEmpty());
    m_avCodecContext->width = size.width();
    m_avCodecContext->height = size.height();
    m_avCodecContext->max_b_frames = 0;
    m_avCodecContext->gop_size = 100;
    m_avCodecContext->pix_fmt = AV_PIX_FMT_CUDA;
    m_avCodecContext->time_base = AVRational{1, 1000};

    m_avCodecContext->framerate = AVRational{(int)m_produce->m_frameRate.numerator, (int)std::max(1u, m_produce->m_frameRate.denominator)};

    qDebug() << (int)m_profile;
    switch (m_profile) {
    case H264Profile::Baseline:
        m_avCodecContext->profile = AV_PROFILE_H264_BASELINE;
        break;
    case H264Profile::Main:
        m_avCodecContext->profile = AV_PROFILE_H264_MAIN;
        break;
    case H264Profile::High:
        m_avCodecContext->profile = AV_PROFILE_H264_HIGH;
        break;
    }

    auto configure_ctx = [&](AVCodecContext *ctx) {
        ctx->width = size.width();
        ctx->height = size.height();
        ctx->max_b_frames = 0;
        ctx->gop_size = 100;
        ctx->pix_fmt = AV_PIX_FMT_CUDA;
        ctx->time_base = AVRational{1, 1000};
    };

    auto try_open = [&](AVCodecContext *ctx) {
        AVDictionary *opts = buildEncodingOptions();
        // Prefer automatic level selection
        av_dict_set(&opts, "level", "auto", 0);
        maybeLogOptions(opts);
        ctx->hw_frames_ctx = av_buffer_ref(av_buffersink_get_hw_frames_ctx(m_outputFilter));
        int open_res = avcodec_open2(ctx, codec, &opts);
        if (opts) {
            av_dict_free(&opts);
        }
        return open_res;
    };

    int result = try_open(m_avCodecContext);
    if (result < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "NVENC open failed, retrying with auto level/profile" << av_err2str(result);
        // Retry with auto level and profile
        avcodec_free_context(&m_avCodecContext);
        m_avCodecContext = avcodec_alloc_context3(codec);
        if (!m_avCodecContext) {
            qCWarning(PIPEWIRERECORD_LOGGING) << "Could not allocate video codec context (retry)";
            return false;
        }
        configure_ctx(m_avCodecContext);
        m_avCodecContext->profile = AV_PROFILE_UNKNOWN;
        m_avCodecContext->level = 0;
        result = try_open(m_avCodecContext);
    }

    if (result < 0 && m_profile == H264Profile::Baseline) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "NVENC open failed with Baseline, retrying with Main profile" << av_err2str(result);
        avcodec_free_context(&m_avCodecContext);
        m_avCodecContext = avcodec_alloc_context3(codec);
        if (!m_avCodecContext) {
            qCWarning(PIPEWIRERECORD_LOGGING) << "Could not allocate video codec context (retry 2)";
            return false;
        }
        configure_ctx(m_avCodecContext);
        m_avCodecContext->profile = AV_PROFILE_H264_MAIN;
        m_avCodecContext->level = 0; // let encoder decide
        result = try_open(m_avCodecContext);
    }

    if (result < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not open codec" << av_err2str(result);
        return false;
    }

    return true;
}

bool H264NVENCEncoder::filterFrame(const PipeWireFrame &frame)
{
    auto size = m_produce->m_stream->size();

    QImage image;
    if (frame.dmabuf) {
        qDebug() << "dma buf path";
        image = QImage(m_produce->m_stream->size(), QImage::Format_RGBA8888_Premultiplied);
        if (!m_dmaBufHandler.downloadFrame(image, frame)) {
            m_produce->m_stream->renegotiateModifierFailed(frame.format, frame.dmabuf->modifier);
            return false;
        }
    } else if (frame.dataFrame) {
        image = frame.dataFrame->toImage();
    } else {
        return false;
    }

    AVFrame *avFrame = av_frame_alloc();
    if (!avFrame) {
        qFatal("Failed to allocate memory");
    }
    avFrame->format = AV_PIX_FMT_RGBA;
    avFrame->width = size.width();
    avFrame->height = size.height();
    if (m_quality) {
        // This is pre-encode frame quality for some encoders; still set for consistency
        avFrame->quality = 1; // 1 to ffmax apparently? Wtf is this
    }

    av_frame_get_buffer(avFrame, 32);

    const std::uint8_t *buffers[] = {image.constBits(), nullptr};
    const int strides[] = {static_cast<int>(image.bytesPerLine()), 0, 0, 0};
    av_image_copy(avFrame->data, avFrame->linesize, buffers, strides, static_cast<AVPixelFormat>(avFrame->format), size.width(), size.height());

    if (frame.presentationTimestamp) {
        avFrame->pts = m_produce->framePts(frame.presentationTimestamp);
    }

    if (auto result = av_buffersrc_add_frame(m_inputFilter, avFrame); result < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to submit frame for CUDA upload filtering" << av_err2str(result);
        av_frame_unref(avFrame);
        av_frame_free(&avFrame);
        return false;
    }
    qCDebug(PIPEWIRERECORD_LOGGING) << "NVENC filterFrame: submitted frame pts=" << avFrame->pts;

    av_frame_free(&avFrame);
    return true;
}

int H264NVENCEncoder::percentageToAbsoluteQuality(const std::optional<quint8> &quality)
{
    if (!quality) {
        return -1;
    }

    return 1; // apparently low is better??
}

AVDictionary *H264NVENCEncoder::buildEncodingOptions()
{
    AVDictionary *options = Encoder::buildEncodingOptions();

    switch (m_encodingPreference) {
    case PipeWireBaseEncodedStream::EncodingPreference::Speed:
        av_dict_set(&options, "preset", "p1", 0); // fastest
        av_dict_set(&options, "tune", "ll", 0); // low-latency
        break;
    case PipeWireBaseEncodedStream::EncodingPreference::Quality:
        av_dict_set(&options, "preset", "p7", 0);
        av_dict_set(&options, "tune", "hq", 0);

        break;
    case PipeWireBaseEncodedStream::EncodingPreference::Size:
        av_dict_set(&options, "preset", "p6", 0);
        break;
    case PipeWireBaseEncodedStream::EncodingPreference::NoPreference:
    default:
        qDebug() << "DAVE!!! Please fix the caller to choose a preference!"; // yolo
        av_dict_set(&options, "preset", "p7", 0);
        av_dict_set(&options, "tune", "hq", 0);

        break;
    }

    return options;
}
