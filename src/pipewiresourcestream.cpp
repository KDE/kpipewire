/*
    SPDX-FileCopyrightText: 2018-2020 Red Hat Inc
    SPDX-FileCopyrightText: 2020-2021 Aleix Pol Gonzalez <aleixpol@kde.org>
    SPDX-FileContributor: Jan Grulich <jgrulich@redhat.com>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "pipewiresourcestream.h"
#include "glhelpers.h"
#include "logging.h"
#include "pipewirecore_p.h"
#include "pwhelpers.h"
#include "vaapiutils_p.h"

#include <libdrm/drm_fourcc.h>
#include <spa/utils/result.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <QGuiApplication>
#include <QOpenGLTexture>
#include <QSocketNotifier>
#include <QThread>
#include <QVersionNumber>
#include <qpa/qplatformnativeinterface.h>

#include <KLocalizedString>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#undef Status

#if !PW_CHECK_VERSION(0, 3, 29)
#define SPA_POD_PROP_FLAG_MANDATORY (1u << 3)
#endif
#if !PW_CHECK_VERSION(0, 3, 33)
#define SPA_POD_PROP_FLAG_DONT_FIXATE (1u << 4)
#endif

#define CURSOR_BPP 4
#define CURSOR_META_SIZE(w, h) (sizeof(struct spa_meta_cursor) + sizeof(struct spa_meta_bitmap) + w * h * CURSOR_BPP)

pw_stream_events pwStreamEvents = {};

struct PipeWireSourceStreamPrivate
{
    QSharedPointer<PipeWireCore> pwCore;
    pw_stream *pwStream = nullptr;
    spa_hook streamListener;

    uint32_t pwNodeId = 0;
    std::optional<std::chrono::nanoseconds> m_currentPresentationTimestamp;

    QAtomicInt m_stopped = false;
    pw_stream_state m_state = PW_STREAM_STATE_UNCONNECTED;

    spa_video_info_raw videoFormat;
    QString m_error;
    bool m_allowDmaBuf = true;
    bool m_usingDmaBuf = false;

    QHash<spa_video_format, QList<uint64_t>> m_availableModifiers;
    spa_source *m_renegotiateEvent = nullptr;

    bool m_withDamage = false;
    Fraction maxFramerate;

    PipeWireSourceStream::UsageHint usageHint = PipeWireSourceStream::UsageHint::Render;
};

static const QVersionNumber pwClientVersion = QVersionNumber::fromString(QString::fromUtf8(pw_get_library_version()));
static const QVersionNumber kDmaBufMinVersion = {0, 3, 24};
static const QVersionNumber kDmaBufModifierMinVersion = {0, 3, 33};
static const QVersionNumber kDropSingleModifierMinVersion = {0, 3, 40};

uint32_t PipeWireSourceStream::spaVideoFormatToDrmFormat(spa_video_format spa_format)
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
    case SPA_VIDEO_FORMAT_BGR:
        return DRM_FORMAT_BGR888;
    case SPA_VIDEO_FORMAT_RGB:
        return DRM_FORMAT_RGB888;
    case SPA_VIDEO_FORMAT_xBGR:
        return DRM_FORMAT_RGBX8888;
    case SPA_VIDEO_FORMAT_ABGR:
        return DRM_FORMAT_RGBA8888;
    case SPA_VIDEO_FORMAT_GRAY8:
        return DRM_FORMAT_R8;
    default:
        qCWarning(PIPEWIRE_LOGGING) << "cannot convert spa format to fourcc" << spa_format;
        return DRM_FORMAT_INVALID;
    }
}

static QString drmFormatName(uint32_t format)
{
    return QString::asprintf("%c%c%c%c %s-endian (0x%08x)",
                             QLatin1Char(format & 0xff).toLatin1(),
                             QLatin1Char((format >> 8) & 0xff).toLatin1(),
                             QLatin1Char((format >> 16) & 0xff).toLatin1(),
                             QLatin1Char((format >> 24) & 0x7f).toLatin1(),
                             format & DRM_FORMAT_BIG_ENDIAN ? "big" : "little",
                             format);
}

spa_video_format drmFormatToSpaVideoFormat(uint32_t drm_format)
{
    switch (drm_format) {
    case DRM_FORMAT_ABGR8888:
        return SPA_VIDEO_FORMAT_RGBA;
    case DRM_FORMAT_XBGR8888:
        return SPA_VIDEO_FORMAT_RGBx;
    case DRM_FORMAT_ARGB8888:
        return SPA_VIDEO_FORMAT_BGRA;
    case DRM_FORMAT_XRGB8888:
        return SPA_VIDEO_FORMAT_BGRx;
    case DRM_FORMAT_BGR888:
        return SPA_VIDEO_FORMAT_BGR;
    case DRM_FORMAT_RGB888:
        return SPA_VIDEO_FORMAT_RGB;
    case DRM_FORMAT_YUYV:
        return SPA_VIDEO_FORMAT_YUY2;
    case DRM_FORMAT_R8:
        return SPA_VIDEO_FORMAT_GRAY8;
    default:
        qCWarning(PIPEWIRE_LOGGING) << "cannot convert drm format to spa" << drmFormatName(drm_format);
        return SPA_VIDEO_FORMAT_UNKNOWN;
    }
}

static QHash<spa_video_format, QList<uint64_t>> queryDmaBufModifiers(EGLDisplay display, const QList<spa_video_format> &formats, PipeWireSourceStream::UsageHint usageHint)
{
    QHash<spa_video_format, QList<uint64_t>> ret;
    ret.reserve(formats.size());
    const bool hasEglImageDmaBufImportExt = epoxy_has_egl_extension(display, "EGL_EXT_image_dma_buf_import");
    static auto eglQueryDmaBufModifiersEXT = (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
    static auto eglQueryDmaBufFormatsEXT = (PFNEGLQUERYDMABUFFORMATSEXTPROC)eglGetProcAddress("eglQueryDmaBufFormatsEXT");

    EGLint count = 0;
    EGLBoolean successFormats = eglQueryDmaBufFormatsEXT(display, 0, nullptr, &count);

    QList<uint32_t> drmFormats(count);
    successFormats &= eglQueryDmaBufFormatsEXT(display, count, reinterpret_cast<EGLint *>(drmFormats.data()), &count);
    if (!successFormats)
        qCWarning(PIPEWIRE_LOGGING) << "Failed to query DMA-BUF formats.";

    const QList<uint64_t> mods = hasEglImageDmaBufImportExt ? QList<uint64_t>{DRM_FORMAT_MOD_INVALID} : QList<uint64_t>{};
    if (!eglQueryDmaBufFormatsEXT || !eglQueryDmaBufModifiersEXT || !hasEglImageDmaBufImportExt || !successFormats) {
        for (spa_video_format format : formats) {
            ret[format] = mods;
        }
        return ret;
    }

    for (spa_video_format format : formats) {
        uint32_t drm_format = PipeWireSourceStream::spaVideoFormatToDrmFormat(format);
        if (drm_format == DRM_FORMAT_INVALID) {
            qCDebug(PIPEWIRE_LOGGING) << "Failed to find matching DRM format." << format;
            continue;
        }

        if (std::find(drmFormats.begin(), drmFormats.end(), drm_format) == drmFormats.end()) {
            qCDebug(PIPEWIRE_LOGGING) << "Format " << drmFormatName(drm_format) << " not supported for modifiers.";
            ret[format] = mods;
            continue;
        }

        successFormats = eglQueryDmaBufModifiersEXT(display, drm_format, 0, nullptr, nullptr, &count);
        if (!successFormats) {
            qCWarning(PIPEWIRE_LOGGING) << "Failed to query DMA-BUF modifier count.";
            ret[format] = mods;
            continue;
        }

        QList<uint64_t> queriedModifiers(count);
        QList<EGLBoolean> externalOnly(count);
        if (count > 0) {
            if (!eglQueryDmaBufModifiersEXT(display, drm_format, count, queriedModifiers.data(), externalOnly.data(), &count)) {
                qCWarning(PIPEWIRE_LOGGING) << "Failed to query DMA-BUF modifiers.";
            }
        }

        QList<uint64_t> usableModifiers;
        usableModifiers.reserve(count + 1);
        if (usageHint == PipeWireSourceStream::UsageHint::EncodeHardware) {
            auto vaapi = VaapiUtils::instance();
            for (int i = 0; i < queriedModifiers.size(); ++i) {
                if (externalOnly[i]) {
                    continue;
                }
                const uint64_t modifier = queriedModifiers[i];
                if (vaapi->supportsModifier(drm_format, modifier)) {
                    usableModifiers.append(modifier);
                }
            }
        } else {
            for (int i = 0; i < queriedModifiers.size(); ++i) {
                if (!externalOnly[i]) {
                    usableModifiers.append(queriedModifiers[i]);
                }
            }
        }

        if (!usableModifiers.isEmpty()) {
            // Support modifier-less buffers
            usableModifiers.push_back(DRM_FORMAT_MOD_INVALID);
        }

        ret[format] = usableModifiers;
    }
    return ret;
}

void PipeWireSourceStream::onStreamStateChanged(void *data, pw_stream_state old, pw_stream_state state, const char *error_message)
{
    PipeWireSourceStream *pw = static_cast<PipeWireSourceStream *>(data);
    qCDebug(PIPEWIRE_LOGGING) << "state changed" << pw_stream_state_as_string(old) << "->" << pw_stream_state_as_string(state) << error_message;
    pw->d->m_state = state;
    Q_EMIT pw->stateChanged(state, old);

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

void PipeWireSourceStream::onRenegotiate(void *data, uint64_t)
{
    PipeWireSourceStream *pw = static_cast<PipeWireSourceStream *>(data);
    uint8_t buffer[4096];
    spa_pod_builder podBuilder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    auto params = pw->createFormatsParams(podBuilder);
    pw_stream_update_params(pw->d->pwStream, params.data(), params.size());
}

void PipeWireSourceStream::renegotiateModifierFailed(spa_video_format format, quint64 modifier)
{
    if (d->pwCore->serverVersion() >= kDropSingleModifierMinVersion) {
        const int removed = d->m_availableModifiers[format].removeAll(modifier);
        if (removed == 0) {
            d->m_allowDmaBuf = false;
        }
    } else {
        d->m_allowDmaBuf = false;
    }
    qCDebug(PIPEWIRE_LOGGING) << "renegotiating, modifier didn't work" << format << modifier << "now only offering" << d->m_availableModifiers[format].count();
    pw_loop_signal_event(d->pwCore->loop(), d->m_renegotiateEvent);
}

static spa_pod *
buildFormat(spa_pod_builder *builder, spa_video_format format, const QList<uint64_t> &modifiers, bool withDontFixate, const Fraction &requestedMaxFramerate)
{
    spa_pod_frame f[2];
    const spa_rectangle pw_min_screen_bounds{1, 1};
    const spa_rectangle pw_max_screen_bounds{UINT32_MAX, UINT32_MAX};

    spa_pod_builder_push_object(builder, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(builder, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
    spa_pod_builder_add(builder, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
    spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);
    spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&pw_min_screen_bounds, &pw_min_screen_bounds, &pw_max_screen_bounds), 0);
    if (requestedMaxFramerate) {
        auto defFramerate = SPA_FRACTION(0, 1);
        auto minFramerate = SPA_FRACTION(1, 1);
        auto maxFramerate = SPA_FRACTION(requestedMaxFramerate.numerator, requestedMaxFramerate.denominator);
        spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&defFramerate), 0);
        spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(&maxFramerate, &minFramerate, &maxFramerate), 0);
    } else {
        auto defFramerate = SPA_FRACTION(0, 1);
        auto maxFramerate = SPA_FRACTION(1200, 1);
        spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&defFramerate, &defFramerate, &maxFramerate), 0);
    }

    if (!modifiers.isEmpty()) {
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

    return static_cast<spa_pod *>(spa_pod_builder_pop(builder, &f[0]));
}

static const int videoDamageRegionCount = 16;

void PipeWireSourceStream::onStreamParamChanged(void *data, uint32_t id, const struct spa_pod *format)
{
    if (!format || id != SPA_PARAM_Format) {
        return;
    }

    PipeWireSourceStream *pw = static_cast<PipeWireSourceStream *>(data);
    spa_format_video_raw_parse(format, &pw->d->videoFormat);

    uint8_t paramsBuffer[1024];
    spa_pod_builder pod_builder = SPA_POD_BUILDER_INIT(paramsBuffer, sizeof(paramsBuffer));

    // When SPA_FORMAT_VIDEO_modifier is present we can use DMA-BUFs as
    // the server announces support for it.
    // See https://github.com/PipeWire/pipewire/blob/master/doc/dma-buf.dox

    pw->d->m_usingDmaBuf = pw->d->m_allowDmaBuf && spa_pod_find_prop(format, nullptr, SPA_FORMAT_VIDEO_modifier);
    Q_ASSERT(pw->d->m_allowDmaBuf || !pw->d->m_usingDmaBuf);
    const auto bufferTypes =
        pw->d->m_usingDmaBuf ? (1 << SPA_DATA_DmaBuf) | (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr) : (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr);

    QVarLengthArray<const spa_pod *> params = {
        (spa_pod *)spa_pod_builder_add_object(&pod_builder,
                                              SPA_TYPE_OBJECT_ParamBuffers,
                                              SPA_PARAM_Buffers,
                                              SPA_PARAM_BUFFERS_buffers,
                                              SPA_POD_CHOICE_RANGE_Int(3, 2, 16),
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
        (spa_pod *)spa_pod_builder_add_object(&pod_builder,
                                              SPA_TYPE_OBJECT_ParamMeta,
                                              SPA_PARAM_Meta,
                                              SPA_PARAM_META_type,
                                              SPA_POD_Id(SPA_META_Cursor),
                                              SPA_PARAM_META_size,
                                              SPA_POD_CHOICE_RANGE_Int(CURSOR_META_SIZE(64, 64), CURSOR_META_SIZE(1, 1), CURSOR_META_SIZE(1024, 1024))),
    };

    if (pw->d->m_withDamage) {
        params.append((spa_pod *)spa_pod_builder_add_object(&pod_builder,
                                                            SPA_TYPE_OBJECT_ParamMeta,
                                                            SPA_PARAM_Meta,
                                                            SPA_PARAM_META_type,
                                                            SPA_POD_Id(SPA_META_VideoDamage),
                                                            SPA_PARAM_META_size,
                                                            SPA_POD_CHOICE_RANGE_Int(sizeof(struct spa_meta_region) * videoDamageRegionCount,
                                                                                     sizeof(struct spa_meta_region) * 1,
                                                                                     sizeof(struct spa_meta_region) * videoDamageRegionCount)));
    }

    pw_stream_update_params(pw->d->pwStream, params.data(), params.count());
    Q_EMIT pw->streamParametersChanged();
}

static void onProcess(void *data)
{
    PipeWireSourceStream *stream = static_cast<PipeWireSourceStream *>(data);
    stream->process();
}

PipeWireFrameData::PipeWireFrameData(spa_video_format format, void *data, QSize size, qint32 stride, PipeWireFrameCleanupFunction *cleanup)
    : format(format)
    , data(data)
    , size(size)
    , stride(stride)
    , cleanup(cleanup)
{
    cleanup->ref();
}

PipeWireFrameData::~PipeWireFrameData()
{
    PipeWireFrameCleanupFunction::unref(cleanup);
}

QSize PipeWireSourceStream::size() const
{
    return QSize(d->videoFormat.size.width, d->videoFormat.size.height);
}

pw_stream_state PipeWireSourceStream::state() const
{
    return d->m_state;
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
    pwStreamEvents.version = PW_VERSION_STREAM_EVENTS;
    pwStreamEvents.process = &onProcess;
    pwStreamEvents.state_changed = &PipeWireSourceStream::onStreamStateChanged;
    pwStreamEvents.param_changed = &PipeWireSourceStream::onStreamParamChanged;
    pwStreamEvents.destroy = &PipeWireSourceStream::onDestroy;
}

PipeWireSourceStream::~PipeWireSourceStream()
{
    d->m_stopped = true;
    if (d->m_renegotiateEvent) {
        pw_loop_destroy_source(d->pwCore->loop(), d->m_renegotiateEvent);
    }
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

void PipeWireSourceStream::setMaxFramerate(const Fraction &framerate)
{
    d->maxFramerate = framerate;

    if (d->pwStream) {
        pw_loop_signal_event(d->pwCore->loop(), d->m_renegotiateEvent);
    }
}

uint PipeWireSourceStream::nodeId()
{
    return d->pwNodeId;
}

PipeWireSourceStream::UsageHint PipeWireSourceStream::usageHint() const
{
    return d->usageHint;
}

void PipeWireSourceStream::setUsageHint(UsageHint hint)
{
    d->usageHint = hint;
}

QList<const spa_pod *> PipeWireSourceStream::createFormatsParams(spa_pod_builder podBuilder)
{
    const auto pwServerVersion = d->pwCore->serverVersion();
    static constexpr auto formats = {
        SPA_VIDEO_FORMAT_RGBx,
        SPA_VIDEO_FORMAT_RGBA,
        SPA_VIDEO_FORMAT_BGRx,
        SPA_VIDEO_FORMAT_BGRA,
        SPA_VIDEO_FORMAT_RGB,
        SPA_VIDEO_FORMAT_BGR,
        SPA_VIDEO_FORMAT_xBGR,
        SPA_VIDEO_FORMAT_ABGR,
        SPA_VIDEO_FORMAT_GRAY8,
    };
    QList<const spa_pod *> params;
    params.reserve(formats.size() * 2);
    const EGLDisplay display = static_cast<EGLDisplay>(QGuiApplication::platformNativeInterface()->nativeResourceForIntegration("egldisplay"));

    d->m_allowDmaBuf = d->m_allowDmaBuf && (pwServerVersion.isNull() || (pwClientVersion >= kDmaBufMinVersion && pwServerVersion >= kDmaBufMinVersion));
    const bool withDontFixate = d->m_allowDmaBuf && (pwServerVersion.isNull() || (pwClientVersion >= kDmaBufModifierMinVersion && pwServerVersion >= kDmaBufModifierMinVersion));

    if (!d->m_allowDmaBuf && d->usageHint == UsageHint::EncodeHardware) {
        qCWarning(PIPEWIRE_LOGGING) << "DMABUF is unsupported but hardware encoding is requested, which requires DMABUF import. This will not work correctly.";
    }

    if (d->m_availableModifiers.isEmpty()) {
        static const auto availableModifiers = queryDmaBufModifiers(display, formats, d->usageHint);
        d->m_availableModifiers = availableModifiers;
    }

    for (auto it = d->m_availableModifiers.constBegin(), itEnd = d->m_availableModifiers.constEnd(); it != itEnd; ++it) {
        if (d->m_allowDmaBuf && !it->isEmpty()) {
            params += buildFormat(&podBuilder, it.key(), it.value(), withDontFixate, d->maxFramerate);
        }

        params += buildFormat(&podBuilder, it.key(), {}, withDontFixate, d->maxFramerate);
    }

    // BUG 492400: Workaround for pipewire < 0.3.49 https://github.com/PipeWire/pipewire/commit/8646117374df6fa3b73f63f9b35cda78a6aaa2f4
    params.removeAll(nullptr);
    return params;
}

bool PipeWireSourceStream::createStream(uint nodeid, int fd)
{
    d->m_availableModifiers.clear();
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
    pw_stream_add_listener(d->pwStream, &d->streamListener, &pwStreamEvents, this);

    d->m_renegotiateEvent = pw_loop_add_event(d->pwCore->loop(), onRenegotiate, this);

    uint8_t buffer[4096];
    spa_pod_builder podBuilder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    auto params = createFormatsParams(podBuilder);
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

    PipeWireFrame frame;
    frame.format = d->videoFormat.format;

    struct spa_meta_header *header = (struct spa_meta_header *)spa_buffer_find_meta_data(spaBuffer, SPA_META_Header, sizeof(*header));
    if (header) {
        if (header->flags & SPA_META_HEADER_FLAG_CORRUPTED) {
            qCDebug(PIPEWIRE_LOGGING) << "buffer is corrupt";
            return;
        }

        d->m_currentPresentationTimestamp = std::chrono::nanoseconds(header->pts);
        frame.presentationTimestamp = std::chrono::nanoseconds(header->pts);
        frame.sequential = header->seq;
    } else {
        d->m_currentPresentationTimestamp = std::chrono::steady_clock::now().time_since_epoch();
        frame.presentationTimestamp = d->m_currentPresentationTimestamp;
    }

    if (spa_meta *vd = spa_buffer_find_meta(spaBuffer, SPA_META_VideoDamage)) {
        frame.damage = QRegion();
        spa_meta_region *mr;
        spa_meta_for_each(mr, vd)
        {
            *frame.damage += QRect(mr->region.position.x, mr->region.position.y, mr->region.size.width, mr->region.size.height);
        }
    }

    { // process cursor
        struct spa_meta_cursor *cursor = static_cast<struct spa_meta_cursor *>(spa_buffer_find_meta_data(spaBuffer, SPA_META_Cursor, sizeof(*cursor)));
        if (cursor && spa_meta_cursor_is_valid(cursor)) {
            struct spa_meta_bitmap *bitmap = nullptr;

            if (cursor->bitmap_offset)
                bitmap = SPA_MEMBER(cursor, cursor->bitmap_offset, struct spa_meta_bitmap);

            QImage cursorTexture;
            if (bitmap && bitmap->size.width > 0 && bitmap->size.height > 0) {
                const size_t bufferSize = bitmap->stride * bitmap->size.height * 4;
                void *bufferData = malloc(bufferSize);
                memcpy(bufferData, SPA_MEMBER(bitmap, bitmap->offset, uint8_t), bufferSize);
                cursorTexture = PWHelpers::SpaBufferToQImage(static_cast<const uchar *>(bufferData),
                                                             bitmap->size.width,
                                                             bitmap->size.height,
                                                             bitmap->stride,
                                                             spa_video_format(bitmap->format),
                                                             new PipeWireFrameCleanupFunction([bufferData] {
                                                                 free(bufferData);
                                                             }));
            }
            frame.cursor = {{cursor->position.x, cursor->position.y}, {cursor->hotspot.x, cursor->hotspot.y}, cursorTexture};
        }
    }

    if (spaBuffer->datas->chunk->flags == SPA_CHUNK_FLAG_CORRUPTED) {
        // do not get a frame
        qCDebug(PIPEWIRE_LOGGING) << "skipping empty buffer" << spaBuffer->datas->chunk->size << spaBuffer->datas->chunk->flags;
    } else if (spaBuffer->datas->type == SPA_DATA_MemFd) {
        if (spaBuffer->datas->chunk->size == 0) {
            qCDebug(PIPEWIRE_LOGGING) << "skipping empty memfd buffer";
        } else {
            const uint32_t mapEnd = spaBuffer->datas->maxsize + spaBuffer->datas->mapoffset;
            uint8_t *map = static_cast<uint8_t *>(mmap(nullptr, mapEnd, PROT_READ, MAP_PRIVATE, spaBuffer->datas->fd, 0));

            if (map == MAP_FAILED) {
                qCWarning(PIPEWIRE_LOGGING) << "Failed to mmap the memory: " << strerror(errno);
                return;
            }
            auto cleanup = [map, mapEnd]() {
                munmap(map, mapEnd);
            };
            frame.dataFrame = std::make_shared<PipeWireFrameData>(d->videoFormat.format,
                                                                  map,
                                                                  QSize(d->videoFormat.size.width, d->videoFormat.size.height),
                                                                  spaBuffer->datas->chunk->stride,
                                                                  new PipeWireFrameCleanupFunction(cleanup));
        }
    } else if (spaBuffer->datas->type == SPA_DATA_DmaBuf) {
        DmaBufAttributes attribs;
        attribs.planes.reserve(spaBuffer->n_datas);
        attribs.format = spaVideoFormatToDrmFormat(d->videoFormat.format);
        attribs.modifier = d->videoFormat.modifier;
        attribs.width = d->videoFormat.size.width;
        attribs.height = d->videoFormat.size.height;

        for (uint i = 0; i < spaBuffer->n_datas; ++i) {
            const auto &data = spaBuffer->datas[i];

            DmaBufPlane plane;
            plane.fd = data.fd;
            plane.stride = data.chunk->stride;
            plane.offset = data.chunk->offset;
            attribs.planes += plane;
        }
        Q_ASSERT(!attribs.planes.isEmpty());
        frame.dmabuf = attribs;
    } else if (spaBuffer->datas->type == SPA_DATA_MemPtr) {
        if (spaBuffer->datas->chunk->size == 0) {
            qCDebug(PIPEWIRE_LOGGING) << "skipping empty memptr buffer";
        } else {
            frame.dataFrame = std::make_shared<PipeWireFrameData>(d->videoFormat.format,
                                                                  spaBuffer->datas->data,
                                                                  QSize(d->videoFormat.size.width, d->videoFormat.size.height),
                                                                  spaBuffer->datas->chunk->stride,
                                                                  nullptr);
        }
    } else {
        if (spaBuffer->datas->type == SPA_ID_INVALID) {
            qCWarning(PIPEWIRE_LOGGING) << "invalid buffer type";
        } else {
            qCWarning(PIPEWIRE_LOGGING) << "unsupported buffer type" << spaBuffer->datas->type;
        }
        frame.dataFrame = {};
    }

    Q_EMIT frameReceived(frame);
}

void PipeWireSourceStream::coreFailed(const QString &errorMessage)
{
    qCDebug(PIPEWIRE_LOGGING) << "received error message" << errorMessage;
    d->m_error = errorMessage;
    Q_EMIT stopStreaming();
}

void PipeWireSourceStream::process()
{
#if !PW_CHECK_VERSION(0, 3, 73)
    if (Q_UNLIKELY(!d->pwStream)) {
        // Assuming it's caused by https://gitlab.freedesktop.org/pipewire/pipewire/-/issues/3314
        qCDebug(PIPEWIRE_LOGGING) << "stream was terminated before processing buffer";
        return;
    }
#endif

    pw_buffer *buf = pw_stream_dequeue_buffer(d->pwStream);
    if (!buf) {
        qCDebug(PIPEWIRE_LOGGING) << "out of buffers";
        return;
    }

    handleFrame(buf);

    pw_stream_queue_buffer(d->pwStream, buf);
}

void PipeWireSourceStream::setActive(bool active)
{
    if (!d->pwStream) {
        qCWarning(PIPEWIRE_LOGGING) << "Tried to make uncreated stream active";
        return;
    }
    pw_stream_set_active(d->pwStream, active);
}

void PipeWireSourceStream::setDamageEnabled(bool withDamage)
{
    d->m_withDamage = withDamage;
}

bool PipeWireSourceStream::usingDmaBuf() const
{
    return d->m_usingDmaBuf;
}

bool PipeWireSourceStream::allowDmaBuf() const
{
    return d->m_allowDmaBuf;
}

void PipeWireSourceStream::setAllowDmaBuf(bool allowed)
{
    d->m_allowDmaBuf = allowed;
}

void PipeWireSourceStream::onDestroy(void *data)
{
    // When PipeWire restarts the stream will auto-delete. Make sure we don't have dangling pointers!
    auto pw = static_cast<PipeWireSourceStream *>(data);
    pw->d->pwStream = nullptr;
}

#include "moc_pipewiresourcestream.cpp"
