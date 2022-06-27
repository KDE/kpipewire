/*
    SPDX-FileCopyrightText: 2018-2020 Red Hat Inc
    SPDX-FileCopyrightText: 2020-2021 Aleix Pol Gonzalez <aleixpol@kde.org>
    SPDX-FileContributor: Jan Grulich <jgrulich@redhat.com>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "pipewiresourcestream.h"
#include "glhelpers.h"
#include "logging.h"
#include "pipewirecore.h"

#include <fcntl.h>
#include <libdrm/drm_fourcc.h>
#include <spa/utils/result.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <QGuiApplication>
#include <QLoggingCategory>
#include <QOpenGLTexture>
#include <QSocketNotifier>
#include <QVersionNumber>
#include <QThread>
#include <qpa/qplatformnativeinterface.h>

#include <KLocalizedString>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QtPlatformHeaders/QEGLNativeContext>
#endif
#undef Status

#if !PW_CHECK_VERSION(0, 3, 29)
#define SPA_POD_PROP_FLAG_MANDATORY (1u << 3)
#endif
#if !PW_CHECK_VERSION(0, 3, 33)
#define SPA_POD_PROP_FLAG_DONT_FIXATE (1u << 4)
#endif

pw_stream_events pwStreamEvents = {};

struct PipeWireSourceStreamPrivate
{
    QSharedPointer<PipeWireCore> pwCore;
    pw_stream *pwStream = nullptr;
    spa_hook streamListener;

    uint32_t pwNodeId = 0;
    std::optional<std::chrono::nanoseconds> m_currentPresentationTimestamp;

    QAtomicInt m_stopped = false;

    spa_video_info_raw videoFormat;
    QString m_error;
    bool m_allowDmaBuf = true;
};

static const QVersionNumber pwClientVersion = QVersionNumber::fromString(QString::fromUtf8(pw_get_library_version()));
static const QVersionNumber kDmaBufMinVersion = {0, 3, 24};
static const QVersionNumber kDmaBufModifierMinVersion = {0, 3, 33};

static uint32_t SpaPixelFormatToDrmFormat(uint32_t spa_format)
{
    switch (spa_format) {
    case SPA_VIDEO_FORMAT_RGBA:
        return DRM_FORMAT_ABGR8888;
    case SPA_VIDEO_FORMAT_RGBx:
        return DRM_FORMAT_XBGR8888;
    case SPA_VIDEO_FORMAT_BGRA:
        return DRM_FORMAT_ARGB8888;
    case SPA_VIDEO_FORMAT_BGRx:
        return DRM_FORMAT_XRGB8888;
    default:
        return DRM_FORMAT_INVALID;
    }
}

Q_GLOBAL_STATIC_WITH_ARGS(bool, hasEglImageDmaBufImportExt, (GLHelpers::hasEglExtension("EGL_EXT_image_dma_buf_import")))

static std::vector<uint64_t> queryDmaBufModifiers(EGLDisplay display, uint32_t format)
{
    static auto eglQueryDmaBufModifiersEXT = (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
    static auto eglQueryDmaBufFormatsEXT = (PFNEGLQUERYDMABUFFORMATSEXTPROC)eglGetProcAddress("eglQueryDmaBufFormatsEXT");
    if (!eglQueryDmaBufFormatsEXT || !eglQueryDmaBufModifiersEXT) {
        return hasEglImageDmaBufImportExt ? std::vector<uint64_t>{DRM_FORMAT_MOD_INVALID} : std::vector<uint64_t>{};
    }

    uint32_t drm_format = SpaPixelFormatToDrmFormat(format);
    if (drm_format == DRM_FORMAT_INVALID) {
        qCDebug(PIPEWIRE_LOGGING) << "Failed to find matching DRM format." << format;
        return {DRM_FORMAT_MOD_INVALID};
    }

    EGLint count = 0;
    EGLBoolean success = eglQueryDmaBufFormatsEXT(display, 0, nullptr, &count);

    if (!success || count == 0) {
        qCWarning(PIPEWIRE_LOGGING) << "Failed to query DMA-BUF format count.";
        return {DRM_FORMAT_MOD_INVALID};
    }

    std::vector<uint32_t> formats(count);
    if (!eglQueryDmaBufFormatsEXT(display, count, reinterpret_cast<EGLint *>(formats.data()), &count)) {
        if (!success)
            qCWarning(PIPEWIRE_LOGGING) << "Failed to query DMA-BUF formats.";
        return {DRM_FORMAT_MOD_INVALID};
    }

    if (std::find(formats.begin(), formats.end(), drm_format) == formats.end()) {
        qCDebug(PIPEWIRE_LOGGING) << "Format " << drm_format << " not supported for modifiers.";
        return {DRM_FORMAT_MOD_INVALID};
    }

    success = eglQueryDmaBufModifiersEXT(display, drm_format, 0, nullptr, nullptr, &count);
    if (!success) {
        qCWarning(PIPEWIRE_LOGGING) << "Failed to query DMA-BUF modifier count.";
        return {DRM_FORMAT_MOD_INVALID};
    }

    std::vector<uint64_t> modifiers(count);
    if (count > 0) {
        if (!eglQueryDmaBufModifiersEXT(display, drm_format, count, modifiers.data(), nullptr, &count)) {
            qCWarning(PIPEWIRE_LOGGING) << "Failed to query DMA-BUF modifiers.";
        }
    }

    // Support modifier-less buffers
    modifiers.push_back(DRM_FORMAT_MOD_INVALID);
    return modifiers;
}

void PipeWireSourceStream::onStreamStateChanged(void *data, pw_stream_state old, pw_stream_state state, const char *error_message)
{
    PipeWireSourceStream *pw = static_cast<PipeWireSourceStream *>(data);
    qCDebug(PIPEWIRE_LOGGING) << "state changed" << pw_stream_state_as_string(old) << "->" << pw_stream_state_as_string(state) << error_message;

    switch (state) {
    case PW_STREAM_STATE_ERROR:
        qCWarning(PIPEWIRE_LOGGING) << "Stream error: " << error_message;
        break;
    case PW_STREAM_STATE_PAUSED:
        Q_EMIT pw->streamReady();
        break;
    case PW_STREAM_STATE_STREAMING:
        Q_EMIT pw->startStreaming();
        break;
    case PW_STREAM_STATE_CONNECTING:
        break;
    case PW_STREAM_STATE_UNCONNECTED:
        if (!pw->d->m_stopped) {
            Q_EMIT pw->stopStreaming();
        }
        break;
    }
}

static spa_pod *buildFormat(spa_pod_builder *builder, spa_video_format format, const std::vector<uint64_t> &modifiers, bool withDontFixate)
{
    spa_pod_frame f[2];
    const spa_rectangle pw_min_screen_bounds{1, 1};
    const spa_rectangle pw_max_screen_bounds{UINT32_MAX, UINT32_MAX};

    spa_pod_builder_push_object(builder, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(builder, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
    spa_pod_builder_add(builder, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
    spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);

    if (modifiers.size()) {
        // SPA_POD_PROP_FLAG_DONT_FIXATE can be used with PipeWire >= 0.3.33
        if (withDontFixate) {
            spa_pod_builder_prop(builder, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
        } else {
            spa_pod_builder_prop(builder, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
        }
        spa_pod_builder_push_choice(builder, &f[1], SPA_CHOICE_Enum, 0);
        // mofifiers from the array
        for (auto it = modifiers.begin(); it != modifiers.end(); it++) {
            spa_pod_builder_long(builder, *it);
            if (it == modifiers.begin()) {
                spa_pod_builder_long(builder, *it);
            }
        }
        spa_pod_builder_pop(builder, &f[1]);
    }

    spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&pw_min_screen_bounds, &pw_min_screen_bounds, &pw_max_screen_bounds), 0);

    return static_cast<spa_pod *>(spa_pod_builder_pop(builder, &f[0]));
}

void PipeWireSourceStream::onStreamParamChanged(void *data, uint32_t id, const struct spa_pod *format)
{
    if (!format || id != SPA_PARAM_Format) {
        return;
    }

    PipeWireSourceStream *pw = static_cast<PipeWireSourceStream *>(data);
    spa_format_video_raw_parse(format, &pw->d->videoFormat);

    const int32_t width = pw->d->videoFormat.size.width;
    const int32_t height = pw->d->videoFormat.size.height;
    const int bpp = pw->d->videoFormat.format == SPA_VIDEO_FORMAT_RGB || pw->d->videoFormat.format == SPA_VIDEO_FORMAT_BGR ? 3 : 4;
    const quint32 stride = SPA_ROUND_UP_N(width * bpp, 4);
    qCDebug(PIPEWIRE_LOGGING) << "Stream format changed";
    const int32_t size = height * stride;

    uint8_t paramsBuffer[1024];
    spa_pod_builder pod_builder = SPA_POD_BUILDER_INIT(paramsBuffer, sizeof(paramsBuffer));

    const auto bufferTypes = pw->d->m_allowDmaBuf && spa_pod_find_prop(format, nullptr, SPA_FORMAT_VIDEO_modifier)
        ? (1 << SPA_DATA_DmaBuf) | (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)
        : (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr);

    QVarLengthArray<const spa_pod *> params = {
        (spa_pod *)spa_pod_builder_add_object(&pod_builder,
                                              SPA_TYPE_OBJECT_ParamBuffers,
                                              SPA_PARAM_Buffers,
                                              SPA_PARAM_BUFFERS_buffers,
                                              SPA_POD_CHOICE_RANGE_Int(16, 2, 16),
                                              SPA_PARAM_BUFFERS_blocks,
                                              SPA_POD_Int(1),
                                              SPA_PARAM_BUFFERS_size,
                                              SPA_POD_Int(size),
                                              SPA_PARAM_BUFFERS_stride,
                                              SPA_POD_CHOICE_RANGE_Int(stride, stride, INT32_MAX),
                                              SPA_PARAM_BUFFERS_align,
                                              SPA_POD_Int(16),
                                              SPA_PARAM_BUFFERS_dataType,
                                              SPA_POD_CHOICE_FLAGS_Int(bufferTypes)),
        (spa_pod *)spa_pod_builder_add_object(&pod_builder,
                                              SPA_TYPE_OBJECT_ParamMeta,
                                              SPA_PARAM_Meta,
                                              SPA_PARAM_META_type,
                                              SPA_POD_Id(SPA_META_Header),
                                              SPA_PARAM_META_size,
                                              SPA_POD_Int(sizeof(struct spa_meta_header))),

    };
    pw_stream_update_params(pw->d->pwStream, params.data(), params.count());
    Q_EMIT pw->streamParametersChanged();
}

static void onProcess(void *data)
{
    PipeWireSourceStream *stream = static_cast<PipeWireSourceStream *>(data);
    stream->process();
}

QSize PipeWireSourceStream::size() const
{
    return QSize(d->videoFormat.size.width, d->videoFormat.size.height);
}

std::optional< std::chrono::nanoseconds > PipeWireSourceStream::currentPresentationTimestamp() const
{
    return d->m_currentPresentationTimestamp;
}

QString PipeWireSourceStream::error() const
{
    return d->m_error;
}

PipeWireSourceStream::PipeWireSourceStream(QObject *parent)
    : QObject(parent)
    , d(new PipeWireSourceStreamPrivate)
{
    qRegisterMetaType<QVector<DmaBufPlane>>();

    pwStreamEvents.version = PW_VERSION_STREAM_EVENTS;
    pwStreamEvents.process = &onProcess;
    pwStreamEvents.state_changed = &PipeWireSourceStream::onStreamStateChanged;
    pwStreamEvents.param_changed = &PipeWireSourceStream::onStreamParamChanged;
}

PipeWireSourceStream::~PipeWireSourceStream()
{
    d->m_stopped = true;
    if (d->pwStream) {
        pw_stream_destroy(d->pwStream);
    }
}

Fraction PipeWireSourceStream::framerate() const
{
    if (d->pwStream) {
        return {d->videoFormat.max_framerate.num, d->videoFormat.max_framerate.denom};
    }

    return {0, 1};
}

uint PipeWireSourceStream::nodeId()
{
    return d->pwNodeId;
}

bool PipeWireSourceStream::createStream(uint nodeid, int fd)
{
    d->pwCore = PipeWireCore::fetch(fd);
    if (!d->pwCore->error().isEmpty()) {
        qCDebug(PIPEWIRE_LOGGING) << "received error while creating the stream" << d->pwCore->error();
        d->m_error = d->pwCore->error();
        return false;
    }

    connect(d->pwCore.data(), &PipeWireCore::pipewireFailed, this, &PipeWireSourceStream::coreFailed);

    if (objectName().isEmpty()) {
        setObjectName(QStringLiteral("plasma-screencast-%1").arg(nodeid));
    }

    const auto pwServerVersion = d->pwCore->serverVersion();
    d->pwStream = pw_stream_new(**d->pwCore, objectName().toUtf8().constData(), nullptr);
    d->pwNodeId = nodeid;
    d->m_allowDmaBuf = pwServerVersion.isNull() || (pwClientVersion >= kDmaBufMinVersion && pwServerVersion >= kDmaBufMinVersion);
    pw_stream_add_listener(d->pwStream, &d->streamListener, &pwStreamEvents, this);

    uint8_t buffer[4096];
    spa_pod_builder podBuilder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    const QVector<spa_video_format> formats =
        {SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGB, SPA_VIDEO_FORMAT_BGR};
    QVector<const spa_pod *> params;
    params.reserve(formats.size() * 2);
    const EGLDisplay display = static_cast<EGLDisplay>(QGuiApplication::platformNativeInterface()->nativeResourceForIntegration("egldisplay"));

    const bool withDontFixate = pwClientVersion >= kDmaBufModifierMinVersion && pwServerVersion >= kDmaBufModifierMinVersion;

    for (spa_video_format format : formats) {
        if (d->m_allowDmaBuf) {
            if (auto modifiers = queryDmaBufModifiers(display, format); modifiers.size() > 0) {
                params += buildFormat(&podBuilder, format, modifiers, withDontFixate);
            }
        }

        params += buildFormat(&podBuilder, format, {}, withDontFixate);
    }

    pw_stream_flags s = (pw_stream_flags)(PW_STREAM_FLAG_DONT_RECONNECT | PW_STREAM_FLAG_AUTOCONNECT);
    if (pw_stream_connect(d->pwStream, PW_DIRECTION_INPUT, d->pwNodeId, s, params.data(), params.size()) != 0) {
        qCWarning(PIPEWIRE_LOGGING) << "Could not connect to stream";
        pw_stream_destroy(d->pwStream);
        d->pwStream = nullptr;
        return false;
    }
    qCDebug(PIPEWIRE_LOGGING) << "created successfully" << nodeid;
    return true;
}

void PipeWireSourceStream::handleFrame(struct pw_buffer *buffer)
{
    spa_buffer *spaBuffer = buffer->buffer;

    if (spaBuffer->datas->chunk->size == 0) {
        return;
    }

    struct spa_meta_header *h = (struct spa_meta_header *)spa_buffer_find_meta_data(spaBuffer, SPA_META_Header, sizeof(*h));
    if (h) {
        d->m_currentPresentationTimestamp = std::chrono::nanoseconds(h->pts);
    } else {
        using namespace std::chrono;
        auto now = system_clock::now();
        d->m_currentPresentationTimestamp = time_point_cast<nanoseconds>(now).time_since_epoch();
    }

    if (spaBuffer->datas->type == SPA_DATA_MemFd) {
        if (spaBuffer->datas->chunk->size == 0)
            return;

        uint8_t *map =
            static_cast<uint8_t *>(mmap(nullptr, spaBuffer->datas->maxsize + spaBuffer->datas->mapoffset, PROT_READ, MAP_PRIVATE, spaBuffer->datas->fd, 0));

        if (map == MAP_FAILED) {
            qCWarning(PIPEWIRE_LOGGING) << "Failed to mmap the memory: " << strerror(errno);
            return;
        }
        const QImage::Format format = spaBuffer->datas->chunk->stride / d->videoFormat.size.width == 3 ? QImage::Format_RGB888 : QImage::Format_ARGB32;

        QImage img(map, d->videoFormat.size.width, d->videoFormat.size.height, spaBuffer->datas->chunk->stride, format);
        Q_EMIT imageTextureReceived(img.copy());

        munmap(map, spaBuffer->datas->maxsize + spaBuffer->datas->mapoffset);
    } else if (spaBuffer->datas->type == SPA_DATA_DmaBuf) {
        QVector<DmaBufPlane> planes;
        planes.reserve(spaBuffer->n_datas);
        for (uint i = 0; i < spaBuffer->n_datas; ++i) {
            const auto &data = spaBuffer->datas[i];

            DmaBufPlane plane;
            plane.fd = data.fd;
            plane.stride = data.chunk->stride;
            plane.offset = data.chunk->offset;
            plane.modifier = DRM_FORMAT_MOD_INVALID;
            planes += plane;
        }
        Q_ASSERT(!planes.isEmpty());
        Q_EMIT dmabufTextureReceived(planes, DRM_FORMAT_ARGB8888);
    } else if (spaBuffer->datas->type == SPA_DATA_MemPtr) {
        QImage img(static_cast<uint8_t *>(spaBuffer->datas->data),
                   d->videoFormat.size.width,
                   d->videoFormat.size.height,
                   spaBuffer->datas->chunk->stride,
                   QImage::Format_ARGB32);
        Q_EMIT imageTextureReceived(img);
    } else {
        qWarning() << "unsupported buffer type" << spaBuffer->datas->type;
        QImage errorImage(200, 200, QImage::Format_ARGB32_Premultiplied);
        errorImage.fill(Qt::red);
        Q_EMIT imageTextureReceived(errorImage);
    }
}

void PipeWireSourceStream::coreFailed(const QString &errorMessage)
{
    qCDebug(PIPEWIRE_LOGGING) << "received error message" << errorMessage;
    d->m_error = errorMessage;
    Q_EMIT stopStreaming();
}

void PipeWireSourceStream::process()
{
    pw_buffer *buf = pw_stream_dequeue_buffer(d->pwStream);
    if (!buf) {
        return;
    }

    handleFrame(buf);

    pw_stream_queue_buffer(d->pwStream, buf);
}

void PipeWireSourceStream::setActive(bool active)
{
    Q_ASSERT(d->pwStream);
    pw_stream_set_active(d->pwStream, active);
}
