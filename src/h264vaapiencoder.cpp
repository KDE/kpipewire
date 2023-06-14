/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>
    SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "h264vaapiencoder.h"

#include <QSize>

#include <libavcodec/avcodec.h>

extern "C" {
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

#include "logging_record.h"

H264VAAPIEncoder::H264VAAPIEncoder(Profile profile, PipeWireProduce *produce)
    : HardwareEncoder(produce)
    , m_profile(profile)
{
}

bool H264VAAPIEncoder::initialize(const QSize &size)
{
    auto path = checkVaapi(size);
    if (path.isEmpty()) {
        return false;
    }

    if (!createDrmContext(path, size)) {
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
    inputs->name = av_strdup("in");
    inputs->filter_ctx = m_inputFilter;
    inputs->pad_idx = 0;
    inputs->next = nullptr;

    auto outputs = avfilter_inout_alloc();
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

    auto codec = avcodec_find_encoder_by_name("h264_vaapi");
    if (!codec) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "h264_vaapi codec not found";
        return false;
    }

    m_avCodecContext = avcodec_alloc_context3(codec);
    if (!m_avCodecContext) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not allocate video codec context";
        return false;
    }
    m_avCodecContext->bit_rate = size.width() * size.height() * 2;

    Q_ASSERT(!size.isEmpty());
    m_avCodecContext->width = size.width();
    m_avCodecContext->height = size.height();
    m_avCodecContext->max_b_frames = 0;
    m_avCodecContext->gop_size = 100;
    m_avCodecContext->pix_fmt = AV_PIX_FMT_VAAPI;
    m_avCodecContext->time_base = AVRational{1, 1000};
    m_avCodecContext->global_quality = 35;

    switch (m_profile) {
    case Profile::Baseline:
        m_avCodecContext->profile = FF_PROFILE_H264_CONSTRAINED_BASELINE;
        break;
    case Profile::Main:
        m_avCodecContext->profile = FF_PROFILE_H264_MAIN;
        break;
    case Profile::High:
        m_avCodecContext->profile = FF_PROFILE_H264_HIGH;
        break;
    }

    AVDictionary *options = nullptr;
    // av_dict_set_int(&options, "threads", qMin(16, QThread::idealThreadCount()), 0);
    av_dict_set(&options, "preset", "veryfast", 0);
    av_dict_set(&options, "tune-content", "screen", 0);
    av_dict_set(&options, "deadline", "realtime", 0);
    // In theory a lower number should be faster, but the opposite seems to be true
    // av_dict_set(&options, "quality", "40", 0);
    // Disable motion estimation, not great while dragging windows but speeds up encoding by an order of magnitude
    av_dict_set(&options, "flags", "+mv4", 0);
    // Disable in-loop filtering
    av_dict_set(&options, "-flags", "+loop", 0);
    av_dict_set(&options, "crf", "45", 0);

    m_avCodecContext->hw_frames_ctx = av_buffer_ref(m_outputFilter->inputs[0]->hw_frames_ctx);

    if (int result = avcodec_open2(m_avCodecContext, codec, &options); result < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not open codec" << av_err2str(ret);
        return false;
    }

    return true;
}
