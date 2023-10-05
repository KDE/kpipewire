/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>
    SPDX-FileCopyrightText: 2023 Marco Martin <mart@kde.org>
    SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
    SPDX-FileCopyrightText: 2023 Noah Davis <noahadvs@gmail.com>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "vp9vaapiencoder_p.h"

#include "pipewireproduce_p.h"

#include <QSize>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

#include "logging_record.h"

Vp9VAAPIEncoder::Vp9VAAPIEncoder(PipeWireProduce *produce)
    : HardwareEncoder(produce)
{
}

bool Vp9VAAPIEncoder::initialize(const QSize &size)
{
    if (!createDrmContext(size)) {
        return false;
    }

    m_avFilterGraph = avfilter_graph_alloc();
    if (!m_avFilterGraph) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not create filter graph";
        return false;
    }

    int ret = avfilter_graph_create_filter(&m_inputFilter,
                                           avfilter_get_by_name("buffer"),
                                           "in",
                                           "width=1:height=1:pix_fmt=drm_prime:time_base=1/1",
                                           nullptr,
                                           m_avFilterGraph);
    if (ret < 0) {
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

    ret = avfilter_graph_parse(m_avFilterGraph, "hwmap=mode=direct:derive_device=vaapi,scale_vaapi=format=nv12:mode=fast", outputs, inputs, NULL);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed creating filter graph";
        return false;
    }

    for (auto i = 0u; i < m_avFilterGraph->nb_filters; ++i) {
        m_avFilterGraph->filters[i]->hw_device_ctx = av_buffer_ref(m_drmContext);
    }

    ret = avfilter_graph_config(m_avFilterGraph, nullptr);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed configuring filter graph";
        return false;
    }

    auto codec = avcodec_find_encoder_by_name("vp9_vaapi");
    if (!codec) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "vp9_vaapi codec not found";
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
    m_avCodecContext->pix_fmt = AV_PIX_FMT_VAAPI;
    m_avCodecContext->time_base = AVRational{1, 1000};

    AVDictionary *options = nullptr;

    // We're probably capturing a screen
    av_dict_set(&options, "tune-content", "screen", 0);

    const auto area = size.width() * size.height();
    // m_avCodecContext->framerate is not set, so we use m_produce->maxFramerate() instead.
    const auto maxFramerate = m_produce->maxFramerate();
    const auto fps = qreal(maxFramerate.numerator) / std::max(quint32(1), maxFramerate.denominator);

    m_avCodecContext->gop_size = fps * 2;

    // 30FPS gets 1x bitrate, 60FPS gets 2x bitrate, etc.
    const qreal fpsFactor = std::max(fps / 30, 1.0);
    m_avCodecContext->bit_rate = std::round(area * fpsFactor);
    m_avCodecContext->rc_min_rate = std::round(area * fpsFactor / 2);
    m_avCodecContext->rc_max_rate = std::round(area * fpsFactor * 1.5);

    m_avCodecContext->rc_buffer_size = m_avCodecContext->bit_rate;

    int globalQuality = 31;
    if (m_quality) {
        globalQuality = percentageToAbsoluteQuality(m_quality);
    }
    m_avCodecContext->global_quality = globalQuality;
    m_avCodecContext->qmin = std::clamp(globalQuality / 2, 0, globalQuality);
    m_avCodecContext->qmax = std::clamp(qRound(globalQuality * 1.5), globalQuality, 63);

    // 0-4 are for Video-On-Demand with the good or best deadline.
    // Don't use best, it's not worth it.
    // 5-8 are for streaming with the realtime deadline.
    // Lower is higher quality.
    int cpuUsed = 5 + std::max(1, int(3 - std::round(m_quality.value_or(50) / 100.0 * 3)));
    m_avCodecContext->compression_level = cpuUsed;

    m_avCodecContext->thread_count = QThread::idealThreadCount();
    av_dict_set_int(&options, "async_depth", m_avCodecContext->thread_count, 0);

    m_avCodecContext->hw_frames_ctx = av_buffer_ref(m_outputFilter->inputs[0]->hw_frames_ctx);

    if (int result = avcodec_open2(m_avCodecContext, codec, &options); result < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not open codec" << av_err2str(ret);
        return false;
    }

    return true;
}

int Vp9VAAPIEncoder::percentageToAbsoluteQuality(const std::optional<quint8> &quality)
{
    if (!quality) {
        return -1;
    }

    constexpr int MinQuality = 63;
    return std::max(1, int(MinQuality - (m_quality.value() / 100.0) * MinQuality));
}
