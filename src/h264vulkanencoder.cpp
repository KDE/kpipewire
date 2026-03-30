/*
    SPDX-FileCopyrightText: 2026 David Edmundson <davidedmundson@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "h264vulkanencoder_p.h"

#include <format>

#include <QSize>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/pixdesc.h>
}

#include "logging_record.h"

#ifndef AV_PROFILE_H264_CONSTRAINED_BASELINE // ffmpeg before 8.0
#define AV_PROFILE_H264_CONSTRAINED_BASELINE FF_PROFILE_H264_CONSTRAINED_BASELINE
#define AV_PROFILE_H264_MAIN FF_PROFILE_H264_MAIN
#define AV_PROFILE_H264_HIGH FF_PROFILE_H264_HIGH
#endif

using namespace std::string_literals;

H264VulkanEncoder::H264VulkanEncoder(H264Profile profile, PipeWireProduce *produce)
    : HardwareEncoder(produce)
    , m_profile(profile)
{
}

bool H264VulkanEncoder::initialize(const QSize &size)
{
    // Create DRM device + frames context to import DMA-BUF frames as DRM_PRIME.
    // Prefer BGR0 for Vulkan interop; fall back through a few common RGBx formats.
    const AVPixelFormat candidates[] = {
        AV_PIX_FMT_BGR0,
        AV_PIX_FMT_BGRA,
        AV_PIX_FMT_RGB0,
        AV_PIX_FMT_RGBA,
        AV_PIX_FMT_0RGB,
        AV_PIX_FMT_ARGB,
    };
    bool drmOk = false;
    for (auto fmt : candidates) {
        qCDebug(PIPEWIRERECORD_LOGGING) << "Trying DRM frames sw_format for Vulkan:" << av_get_pix_fmt_name(fmt);
        if (createDrmContext(size, fmt)) {
            drmOk = true;
            break;
        }
    }
    if (!drmOk) {
        return false;
    }

    // Build a Vulkan-based filter graph that hw-maps DRM_PRIME frames to Vulkan
    // and converts to NV12 fully on GPU.
    m_avFilterGraph = avfilter_graph_alloc();
    if (!m_avFilterGraph) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not create filter graph";
        return false;
    }

    m_inputFilter = avfilter_graph_alloc_filter(m_avFilterGraph, avfilter_get_by_name("buffer"), "in");
    if (!m_inputFilter) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to create the buffer filter";
        return false;
    }

    auto parameters = av_buffersrc_parameters_alloc();
    if (!parameters) {
        qFatal("Failed to allocate memory");
    }

    parameters->format = AV_PIX_FMT_DRM_PRIME;
    parameters->width = size.width();
    parameters->height = size.height();
    parameters->time_base = {1, 1000};
    parameters->hw_frames_ctx = m_drmFramesContext;

    av_buffersrc_parameters_set(m_inputFilter, parameters);
    av_free(parameters);
    parameters = nullptr;

    int ret = avfilter_init_str(m_inputFilter, nullptr);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to init the buffer filter";
        return false;
    }

    ret = avfilter_graph_create_filter(&m_outputFilter, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, m_avFilterGraph);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not create buffer output filter";
        return false;
    }

    auto inputs = avfilter_inout_alloc();
    if (!inputs) {
        qFatal("Failed to allocate memory");
    }
    inputs->name = av_strdup("in");
    inputs->filter_ctx = m_inputFilter;
    inputs->pad_idx = 0;
    inputs->next = nullptr;

    auto outputs = avfilter_inout_alloc();
    if (!outputs) {
        qFatal("Failed to allocate memory");
    }
    outputs->name = av_strdup("out");
    outputs->filter_ctx = m_outputFilter;
    outputs->pad_idx = 0;
    outputs->next = nullptr;

    // Map to Vulkan (derive_device=vulkan) and convert to NV12 on GPU.
    // Keep processing fully on the GPU.
    const auto filterGraph = std::string("hwmap=mode=direct:derive_device=vulkan,scale_vulkan=format=nv12");

    ret = avfilter_graph_parse(m_avFilterGraph, filterGraph.data(), outputs, inputs, NULL);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed creating Vulkan filter graph" << av_err2str(ret);
        return false;
    }

    for (unsigned i = 0; i < m_avFilterGraph->nb_filters; ++i) {
        m_avFilterGraph->filters[i]->hw_device_ctx = av_buffer_ref(m_drmContext);
    }

    ret = avfilter_graph_config(m_avFilterGraph, nullptr);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed configuring Vulkan filter graph" << av_err2str(ret);
        return false;
    }

    auto codec = avcodec_find_encoder_by_name("h264_vulkan");
    if (!codec) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "h264_vulkan codec not found";
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
    m_avCodecContext->pix_fmt = AV_PIX_FMT_VULKAN;
    m_avCodecContext->time_base = AVRational{1, 1000};

    if (m_quality) {
        m_avCodecContext->global_quality = percentageToAbsoluteQuality(m_quality);
    } else {
        m_avCodecContext->global_quality = 35;
    }

    // TODO should be m_profile, but seems to explode...yolo
    m_avCodecContext->profile = AV_PROFILE_UNKNOWN;

    AVDictionary *options = buildEncodingOptions();
    maybeLogOptions(options);

    m_avCodecContext->hw_frames_ctx = av_buffer_ref(av_buffersink_get_hw_frames_ctx(m_outputFilter));

    if (int result = avcodec_open2(m_avCodecContext, codec, &options); result < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not open codec" << av_err2str(result);
        return false;
    }

    return true;
}

int H264VulkanEncoder::percentageToAbsoluteQuality(const std::optional<quint8> &quality)
{
    if (!quality) {
        return -1;
    }
    // Same mapping as VAAPI path; lower absolute value = higher quality.
    constexpr int MinQuality = 51 + 6 * 6;
    return std::max(1, int(MinQuality - (quality.value() / 100.0) * MinQuality));
}
