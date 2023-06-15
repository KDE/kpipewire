/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>
    SPDX-FileCopyrightText: 2023 Marco Martin <mart@kde.org>
    SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
}

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
}

void Encoder::receivePacket()
{
}

void Encoder::finish()
{

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

bool SoftwareEncoder::createFilterGraph(const QSize &size)
{
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
