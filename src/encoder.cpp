/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>
    SPDX-FileCopyrightText: 2023 Marco Martin <mart@kde.org>
    SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "encoder.h"

#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/imgutils.h>
}

#include "pipewireproduce.h"
#include "vaapiutils_p.h"

#include "logging_record.h"

#undef av_err2str
// The one provided by libav fails to compile on GCC due to passing data from the function scope outside
char str[AV_ERROR_MAX_STRING_SIZE];
char *av_err2str(int errnum)
{
    return av_make_error_string(str, AV_ERROR_MAX_STRING_SIZE, errnum);
}

static AVPixelFormat convertQImageFormatToAVPixelFormat(QImage::Format format)
{
    // Listing those handed by SpaToQImageFormat
    switch (format) {
    case QImage::Format_BGR888:
        return AV_PIX_FMT_BGR24;
    case QImage::Format_RGBX8888:
    case QImage::Format_RGBA8888_Premultiplied:
        return AV_PIX_FMT_RGBA;
    case QImage::Format_RGB32:
        return AV_PIX_FMT_RGB32;
    default:
        qDebug() << "Unexpected pixel format" << format;
        return AV_PIX_FMT_RGB32;
    }
}

Encoder::Encoder(PipeWireProduce *produce)
    : QObject(nullptr)
    , m_produce(produce)
{
}

Encoder::~Encoder()
{
    if (m_avFilterGraph) {
        avfilter_graph_free(&m_avFilterGraph);
    }

    if (m_avCodecContext) {
        avcodec_close(m_avCodecContext);
        av_free(m_avCodecContext);
    }
}

void Encoder::encodeFrame()
{
    auto frame = av_frame_alloc();

    for (;;) {
        if (auto result = av_buffersink_get_frame(m_outputFilter, frame); result < 0) {
            if (result != AVERROR_EOF && result != AVERROR(EAGAIN)) {
                qCWarning(PIPEWIRERECORD_LOGGING) << "Failed receiving filtered frame:" << av_err2str(result);
            }
            break;
        }

        auto ret = -1;
        {
            std::lock_guard guard(m_avCodecMutex);
            ret = avcodec_send_frame(m_avCodecContext, frame);
        }
        if (ret < 0) {
            if (ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
                qCWarning(PIPEWIRERECORD_LOGGING) << "Error sending a frame for encoding:" << av_err2str(ret);
            }
            break;
        }
        av_frame_unref(frame);
    }

    av_frame_free(&frame);
}

void Encoder::receivePacket()
{
    auto packet = av_packet_alloc();

    for (;;) {
        auto ret = -1;
        {
            std::lock_guard guard(m_avCodecMutex);
            ret = avcodec_receive_packet(m_avCodecContext, packet);
        }
        if (ret < 0) {
            if (ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
                qCWarning(PIPEWIRERECORD_LOGGING) << "Error encoding a frame: " << av_err2str(ret);
            }
            av_packet_unref(packet);
            break;
        }

        m_produce->processPacket(packet);
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
}

void Encoder::finish()
{
    std::lock_guard guard(m_avCodecMutex);
    avcodec_send_frame(m_avCodecContext, nullptr);
}

AVCodecContext *Encoder::avCodecContext() const
{
    return m_avCodecContext;
}

SoftwareEncoder::SoftwareEncoder(PipeWireProduce *produce)
    : Encoder(produce)
{
}

void SoftwareEncoder::filterFrame(const PipeWireFrame &frame)
{
    auto size = m_produce->m_stream->size();

    QImage image;
    if (frame.dmabuf) {
        image = QImage(m_produce->m_stream->size(), QImage::Format_RGBA8888_Premultiplied);
        if (!m_dmaBufHandler.downloadFrame(image, frame)) {
            m_produce->m_stream->renegotiateModifierFailed(frame.format, frame.dmabuf->modifier);
            return;
        }
    } else if (frame.image) {
        image = frame.image.value();
    } else {
        return;
    }

    AVFrame *avFrame = av_frame_alloc();
    avFrame->format = convertQImageFormatToAVPixelFormat(image.format());
    avFrame->width = size.width();
    avFrame->height = size.height();
    av_frame_get_buffer(avFrame, 32);

    const std::uint8_t *buffers[] = {image.constBits(), nullptr};
    const int strides[] = {static_cast<int>(image.bytesPerLine()), 0, 0, 0};

    av_image_copy(avFrame->data, avFrame->linesize, buffers, strides, static_cast<AVPixelFormat>(avFrame->format), size.width(), size.height());

    if (frame.presentationTimestamp) {
        avFrame->pts = m_produce->framePts(frame.presentationTimestamp);
    }

    if (auto result = av_buffersrc_add_frame(m_inputFilter, avFrame); result < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to submit frame for filtering";
    }
}

bool SoftwareEncoder::createFilterGraph(const QSize &size)
{
    m_avFilterGraph = avfilter_graph_alloc();
    if (!m_avFilterGraph) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not create filter graph";
        return false;
    }

    int ret = avfilter_graph_create_filter(&m_inputFilter,
                                           avfilter_get_by_name("buffer"),
                                           "in",
                                           "width=1:height=1:pix_fmt=rgba:time_base=1/1",
                                           nullptr,
                                           m_avFilterGraph);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to create the buffer filter";
        return false;
    }

    auto parameters = av_buffersrc_parameters_alloc();

    parameters->format = AV_PIX_FMT_RGBA;
    parameters->width = size.width();
    parameters->height = size.height();
    parameters->time_base = {1, 1000};

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

    ret = avfilter_graph_parse(m_avFilterGraph, "format=pix_fmts=yuv420p", outputs, inputs, NULL);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed creating filter graph";
        return false;
    }

    ret = avfilter_graph_config(m_avFilterGraph, nullptr);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed configuring filter graph";
        return false;
    }

    return true;
}

HardwareEncoder::HardwareEncoder(PipeWireProduce *produce)
    : Encoder(produce)
{
}

HardwareEncoder::~HardwareEncoder()
{
    if (m_drmFramesContext) {
        av_free(m_drmFramesContext);
    }

    if (m_drmContext) {
        av_free(m_drmContext);
    }
}

void HardwareEncoder::filterFrame(const PipeWireFrame &frame)
{
    if (!frame.dmabuf) {
        return;
    }

    auto attribs = frame.dmabuf.value();
    auto drmFrame = av_frame_alloc();
    drmFrame->format = AV_PIX_FMT_DRM_PRIME;
    drmFrame->width = attribs.width;
    drmFrame->height = attribs.height;

    auto frameDesc = new AVDRMFrameDescriptor;
    frameDesc->nb_layers = 1;
    frameDesc->layers[0].nb_planes = attribs.planes.count();
    frameDesc->layers[0].format = attribs.format;
    for (int i = 0; i < attribs.planes.count(); ++i) {
        const auto &plane = attribs.planes[i];
        frameDesc->layers[0].planes[i].object_index = 0;
        frameDesc->layers[0].planes[i].offset = plane.offset;
        frameDesc->layers[0].planes[i].pitch = plane.stride;
    }
    frameDesc->nb_objects = 1;
    frameDesc->objects[0].fd = attribs.planes[0].fd;
    frameDesc->objects[0].format_modifier = attribs.modifier;
    frameDesc->objects[0].size = attribs.width * attribs.height * 4;

    drmFrame->data[0] = reinterpret_cast<uint8_t *>(frameDesc);
    drmFrame->buf[0] = av_buffer_create(reinterpret_cast<uint8_t *>(frameDesc), sizeof(*frameDesc), av_buffer_default_free, nullptr, 0);
    if (frame.presentationTimestamp) {
        drmFrame->pts = m_produce->framePts(frame.presentationTimestamp);
    }

    if (auto result = av_buffersrc_add_frame(m_inputFilter, drmFrame); result < 0) {
        qCDebug(PIPEWIRERECORD_LOGGING) << "Failed sending frame for encoding" << av_err2str(result);
        av_frame_unref(drmFrame);
        return;
    }

    av_frame_free(&drmFrame);
}

QByteArray HardwareEncoder::checkVaapi(const QSize &size)
{
    VaapiUtils utils;
    if (utils.devicePath().isEmpty()) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Hardware encoding is not supported on this device.";
        return QByteArray{};
    }

    auto minSize = utils.minimumSize();
    if (size.width() < minSize.width() || size.height() < minSize.height()) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Requested size" << size << "less than minimum supported hardware size" << minSize;
        return QByteArray{};
    }

    auto maxSize = utils.maximumSize();
    if (size.width() > maxSize.width() || size.height() > maxSize.height()) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Requested size" << size << "exceeds maximum supported hardware size" << maxSize;
        return QByteArray{};
    }

    return utils.devicePath();
}

bool HardwareEncoder::createDrmContext(const QSize &size)
{
    auto path = checkVaapi(size);
    if (path.isEmpty()) {
        return false;
    }

    int err = av_hwdevice_ctx_create(&m_drmContext, AV_HWDEVICE_TYPE_DRM, path.data(), NULL, AV_HWFRAME_MAP_READ);
    if (err < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to create DRM device. Error" << av_err2str(err);
        return false;
    }

    m_drmFramesContext = av_hwframe_ctx_alloc(m_drmContext);
    if (!m_drmFramesContext) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to create DRM frames context";
        return false;
    }

    auto framesContext = reinterpret_cast<AVHWFramesContext *>(m_drmFramesContext->data);
    framesContext->format = AV_PIX_FMT_DRM_PRIME;
    framesContext->sw_format = AV_PIX_FMT_0BGR;
    framesContext->width = size.width();
    framesContext->height = size.height();

    if (auto result = av_hwframe_ctx_init(m_drmFramesContext); result < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed initializing DRM frames context" << av_err2str(result);
        av_buffer_unref(&m_drmFramesContext);
        return false;
    }

    return true;
}