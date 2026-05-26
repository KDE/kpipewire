/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>
    SPDX-FileCopyrightText: 2023 Marco Martin <mart@kde.org>
    SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "h264vaapiencoder_p.h"

#include <format>

#include <QImage>
#include <QSize>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "logging_record.h"
#include "vaapiutils_p.h"

#ifndef AV_PROFILE_H264_CONSTRAINED_BASELINE // ffmpeg before 8.0
#define AV_PROFILE_H264_CONSTRAINED_BASELINE FF_PROFILE_H264_CONSTRAINED_BASELINE
#define AV_PROFILE_H264_MAIN FF_PROFILE_H264_MAIN
#define AV_PROFILE_H264_HIGH FF_PROFILE_H264_HIGH
#endif

using namespace std::string_literals;

H264VAAPIEncoder::H264VAAPIEncoder(H264Profile profile, PipeWireProduce *produce)
    : HardwareEncoder(produce)
    , m_profile(profile)
{
}

H264VAAPIEncoder::~H264VAAPIEncoder()
{
    if (m_vaapiDeviceContext) {
        av_buffer_unref(&m_vaapiDeviceContext);
    }
}

bool H264VAAPIEncoder::initialize(const QSize &size)
{
    if (!createDrmContext(size)) {
        return false;
    }

    m_useSoftwareConversion = !VaapiUtils::instance()->supportsVideoProcessing();
    qCDebug(PIPEWIRERECORD_LOGGING) << "H264VAAPIEncoder: VideoProc supported =" << !m_useSoftwareConversion;

    m_avFilterGraph = avfilter_graph_alloc();
    if (!m_avFilterGraph) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not create filter graph";
        return false;
    }

    if (m_useSoftwareConversion) {
        qCDebug(PIPEWIRERECORD_LOGGING) << "VAAPI: VideoProc not supported, using software format conversion path";
    }

    // Set up the input buffer filter. The input format differs depending on
    // whether we can use VAAPI VideoProc for format conversion:
    //
    // - With VideoProc: input is DRM_PRIME (dmabuf), mapped directly to a
    //   VAAPI surface and then converted to NV12 via scale_vaapi.
    // - Without VideoProc: input is NV12 CPU memory, uploaded to a VAAPI
    //   surface via hwupload. The dmabuf is downloaded and converted in
    //   filterFrame().
    m_inputFilter = avfilter_graph_alloc_filter(m_avFilterGraph, avfilter_get_by_name("buffer"), "in");
    if (!m_inputFilter) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to create the buffer filter";
        return false;
    }

    auto parameters = av_buffersrc_parameters_alloc();
    if (!parameters) {
        qFatal("Failed to allocate memory");
    }

    if (m_useSoftwareConversion) {
        parameters->format = AV_PIX_FMT_NV12;
    } else {
        parameters->format = AV_PIX_FMT_DRM_PRIME;
        parameters->hw_frames_ctx = m_drmFramesContext;
    }
    parameters->width = size.width();
    parameters->height = size.height();
    parameters->time_base = {1, 1000};

    av_buffersrc_parameters_set(m_inputFilter, parameters);
    av_free(parameters);
    parameters = nullptr;

    int ret = avfilter_init_str(m_inputFilter, nullptr);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to create the buffer filter";
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

    // Create and configure the filter graph.
    // For the VPP path: use avfilter_graph_parse to build hwmap+scale_vaapi
    // For the no-VPP path: manually create hwupload so we can set hw_device_ctx before its init()
    if (m_useSoftwareConversion) {
        // No-VPP path: create hwupload filter manually.
        // hwupload needs a VAAPI device context set before init(), so we
        // cannot use avfilter_graph_parse() which would call init() too early.

        // Create VAAPI device context
        auto devicePath = VaapiUtils::instance()->devicePath();
        int err = av_hwdevice_ctx_create(&m_vaapiDeviceContext, AV_HWDEVICE_TYPE_VAAPI, devicePath.data(), NULL, 0);
        if (err < 0) {
            qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to create VAAPI device context:" << av_err2str(err);
            return false;
        }

        // Create hwupload filter with device context pre-set
        AVFilterContext *hwuploadFilter = avfilter_graph_alloc_filter(m_avFilterGraph, avfilter_get_by_name("hwupload"), "hwupload");
        if (!hwuploadFilter) {
            qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to create hwupload filter";
            return false;
        }
        hwuploadFilter->hw_device_ctx = av_buffer_ref(m_vaapiDeviceContext);

        ret = avfilter_init_str(hwuploadFilter, nullptr);
        if (ret < 0) {
            qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to init hwupload filter";
            return false;
        }

        // Link: buffer -> hwupload -> buffersink
        ret = avfilter_link(m_inputFilter, 0, hwuploadFilter, 0);
        if (ret < 0) {
            qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to link buffer to hwupload";
            return false;
        }
        ret = avfilter_link(hwuploadFilter, 0, m_outputFilter, 0);
        if (ret < 0) {
            qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to link hwupload to buffersink";
            return false;
        }
    } else {
        // VPP path: use graph parsing for the hwmap+scale_vaapi chain
        const auto colorRange = m_colorRange == PipeWireBaseEncodedStream::ColorRange::Full ? "full"s : "limited"s;
        const auto filterGraph = std::format("hwmap=mode=direct:derive_device=vaapi,scale_vaapi=format=nv12:mode=fast:out_range={}", colorRange);

        ret = avfilter_graph_parse(m_avFilterGraph, filterGraph.data(), outputs, inputs, NULL);
        if (ret < 0) {
            qCWarning(PIPEWIRERECORD_LOGGING) << "Failed creating filter graph";
            return false;
        }

        for (auto i = 0u; i < m_avFilterGraph->nb_filters; ++i) {
            m_avFilterGraph->filters[i]->hw_device_ctx = av_buffer_ref(m_drmContext);
        }
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

    Q_ASSERT(!size.isEmpty());
    m_avCodecContext->width = size.width();
    m_avCodecContext->height = size.height();
    m_avCodecContext->max_b_frames = 0;
    m_avCodecContext->gop_size = 100;
    m_avCodecContext->pix_fmt = AV_PIX_FMT_VAAPI;
    m_avCodecContext->time_base = AVRational{1, 1000};

    if (m_quality) {
        m_avCodecContext->global_quality = percentageToAbsoluteQuality(m_quality);
    } else {
        m_avCodecContext->global_quality = 35;
    }

    switch (m_profile) {
    case H264Profile::Baseline:
        m_avCodecContext->profile = AV_PROFILE_H264_CONSTRAINED_BASELINE;
        break;
    case H264Profile::Main:
        m_avCodecContext->profile = AV_PROFILE_H264_MAIN;
        break;
    case H264Profile::High:
        m_avCodecContext->profile = AV_PROFILE_H264_HIGH;
        break;
    }

    AVDictionary *options = buildEncodingOptions();
    maybeLogOptions(options);

    // Assign the right hardware context for encoding frames.
    // We rely on FFmpeg for creating the VAAPI hardware context as part of
    // `avfilter_graph_parse()`. The codec context needs the VAAPI context to be
    // able to encode properly, so get that from the output filter.
    m_avCodecContext->hw_frames_ctx = av_buffer_ref(av_buffersink_get_hw_frames_ctx(m_outputFilter));

    if (int result = avcodec_open2(m_avCodecContext, codec, &options); result < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not open codec" << av_err2str(result);
        return false;
    }

    return true;
}

bool H264VAAPIEncoder::filterFrame(const PipeWireFrame &frame)
{
    if (!m_useSoftwareConversion) {
        // Hardware conversion path: use the default hardware encoder behavior
        // that passes the dmabuf directly as a DRM_PRIME frame.
        return HardwareEncoder::filterFrame(frame);
    }

    // Software conversion fallback: when the VAAPI driver does not support
    // VAEntrypointVideoProc, we cannot use scale_vaapi for pixel format
    // conversion. Instead, download the dmabuf to a CPU buffer, convert to
    // NV12 via sws_scale, then upload to a VAAPI surface via hwupload.

    if (!frame.dmabuf) {
        return false;
    }

    auto attribs = frame.dmabuf.value();
    QSize size(m_produce->m_stream->size());

    // Download the dmabuf to a QImage (RGBA on CPU)
    QImage image(size, QImage::Format_RGBA8888_Premultiplied);
    if (!m_dmaBufHandler.downloadFrame(image, frame)) {
        m_produce->m_stream->renegotiateModifierFailed(frame.format, frame.dmabuf->modifier);
        return false;
    }

    // Allocate an NV12 AVFrame for the upload
    auto nv12Frame = av_frame_alloc();
    if (!nv12Frame) {
        qFatal("Failed to allocate memory");
    }
    nv12Frame->format = AV_PIX_FMT_NV12;
    nv12Frame->width = size.width();
    nv12Frame->height = size.height();
    av_frame_get_buffer(nv12Frame, 32);

    // Convert RGBA QImage to NV12 using sws_scale
    SwsContext *swsCtx =
        sws_getContext(size.width(), size.height(), AV_PIX_FMT_RGBA, size.width(), size.height(), AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!swsCtx) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to create sws context for NV12 conversion";
        av_frame_free(&nv12Frame);
        return false;
    }

    const uint8_t *srcData[4] = {image.constBits(), nullptr, nullptr, nullptr};
    int srcLinesize[4] = {static_cast<int>(image.bytesPerLine()), 0, 0, 0};
    sws_scale(swsCtx, srcData, srcLinesize, 0, size.height(), nv12Frame->data, nv12Frame->linesize);
    sws_freeContext(swsCtx);

    if (frame.presentationTimestamp) {
        nv12Frame->pts = m_produce->framePts(frame.presentationTimestamp);
    }

    // Submit the NV12 frame to the filter graph (hwupload will upload it to VAAPI)
    if (auto result = av_buffersrc_add_frame(m_inputFilter, nv12Frame); result < 0) {
        qCDebug(PIPEWIRERECORD_LOGGING) << "Failed sending NV12 frame for encoding" << av_err2str(result);
        av_frame_free(&nv12Frame);
        return false;
    }

    av_frame_free(&nv12Frame);
    return true;
}

int H264VAAPIEncoder::percentageToAbsoluteQuality(std::optional<quint8> quality)
{
    if (!quality) {
        return -1;
    }

    constexpr int MinQuality = 51 + 6 * 6;
    return std::max(1, int(MinQuality - (m_quality.value() / 100.0) * MinQuality));
}

AVDictionary *H264VAAPIEncoder::buildEncodingOptions()
{
    AVDictionary *options = HardwareEncoder::buildEncodingOptions();
    // Disable motion estimation, not great while dragging windows but speeds up encoding by an order of magnitude
    av_dict_set(&options, "flags", "+mv4", 0);
    // Disable in-loop filtering
    av_dict_set(&options, "-flags", "+loop", 0);

    return options;
}
