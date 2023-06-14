/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>
    SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "encoder.h"

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext_drm.h>
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

        auto ret = avcodec_send_frame(m_avCodecContext, frame);
        if (ret < 0) {
            qCWarning(PIPEWIRERECORD_LOGGING) << "Error sending a frame for encoding:" << av_err2str(ret);
            return;
        }
        av_frame_unref(frame);
    }

    av_frame_free(&frame);
}

void Encoder::receivePacket()
{
    auto packet = av_packet_alloc();

    for (;;) {
        auto ret = avcodec_receive_packet(m_avCodecContext, packet);
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
}

HardwareEncoder::HardwareEncoder(PipeWireProduce *produce)
    : Encoder(produce)
{
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
        drmFrame->pts = std::chrono::duration_cast<std::chrono::milliseconds>(frame.presentationTimestamp.value()).count();
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

bool HardwareEncoder::createDrmContext(const QByteArray &path, const QSize &size)
{
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
