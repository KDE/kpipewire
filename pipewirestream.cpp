/*
 * Copyright © 2018-2020 Red Hat, Inc
 * Copyright © 2020 Aleix Pol Gonzalez <aleixpol@kde.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Jan Grulich <jgrulich@redhat.com>
 *       Aleix Pol Gonzalez <aleixpol@kde.org>
 */

#include "pipewirestream.h"
#include "logging.h"

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <libdrm/drm_fourcc.h>
#include <unistd.h>
#include <gbm.h>

#include <QOpenGLTexture>
#include <QSocketNotifier>
#include <QLoggingCategory>

#include <KLocalizedString>

void PipeWireSourceStream::onStreamStateChanged(void *data, pw_stream_state old, pw_stream_state state, const char *error_message)
{
    PipeWireSourceStream *pw = static_cast<PipeWireSourceStream*>(data);
    qCDebug(PIPEWIRE_LOGGING) << "state changed"<< pw_stream_state_as_string(old) << " -> " << pw_stream_state_as_string(state) << error_message;

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
        if (!pw->m_stopped) {
            Q_EMIT pw->stopStreaming();
        }
        break;
    }
}

void PipeWireSourceStream::onStreamParamChanged(void *data, uint32_t id, const struct spa_pod *format)
{
    if (!format || id != SPA_PARAM_Format) {
        return;
    }

    PipeWireSourceStream *pw = static_cast<PipeWireSourceStream*>(data);
    qCDebug(PIPEWIRE_LOGGING) << "Stream format changed" << pw;
    spa_format_video_raw_parse (format, &pw->videoFormat);

    const int32_t width = pw->videoFormat.size.width;
    const int32_t height =pw->videoFormat.size.height;
    pw->m_stride = SPA_ROUND_UP_N (width * 4, 4); //TODO see what pixel format is suggested to infer the bytes-per-pixel
    const int32_t size = height * pw->m_stride;

    uint8_t paramsBuffer[1024];
    spa_pod_builder pod_builder = SPA_POD_BUILDER_INIT (paramsBuffer, sizeof (paramsBuffer));

    const spa_pod *param = (spa_pod*) spa_pod_builder_add_object(&pod_builder,
                                                      SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                                                      SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(16, 2, 16),
                                                      SPA_PARAM_BUFFERS_blocks, SPA_POD_Int (1),
                                                      SPA_PARAM_BUFFERS_size, SPA_POD_Int(size),
                                                      SPA_PARAM_BUFFERS_stride, SPA_POD_CHOICE_RANGE_Int(pw->m_stride, pw->m_stride, INT32_MAX),
                                                      SPA_PARAM_BUFFERS_align, SPA_POD_Int(16));
    pw_stream_update_params(pw->pwStream, &param, 1);
}

static void onProcess (void *data)
{
    PipeWireSourceStream *stream = static_cast<PipeWireSourceStream*>(data);
    pw_buffer *buf = pw_stream_dequeue_buffer(stream->pwStream);
    if (!buf) {
        return;
    }

    stream->handleFrame(buf);

    pw_stream_queue_buffer(stream->pwStream, buf);
}

class PipeWireCore : public QObject
{
    Q_OBJECT
public:
    PipeWireCore() {
        pw_init(nullptr, nullptr);
        pwCoreEvents.version = PW_VERSION_CORE_EVENTS;
        pwCoreEvents.error = &PipeWireCore::onCoreError;
    }

    static void onCoreError(void *data, uint32_t id, int seq, int res, const char *message)
    {
        Q_UNUSED(seq)

        qCWarning(PIPEWIRE_LOGGING) << "PipeWire remote error: " << message;
        if (id == PW_ID_CORE && res == -EPIPE) {
            PipeWireCore *pw = static_cast<PipeWireCore*>(data);
            Q_EMIT pw->pipewireFailed(QString::fromUtf8(message));
        }
    }

    ~PipeWireCore()
    {
        if (pwMainLoop) {
            pw_loop_leave(pwMainLoop);
        }

        if (pwCore) {
            pw_core_disconnect(pwCore);
        }

        if (pwContext) {
            pw_context_destroy(pwContext);
        }

        if (pwMainLoop) {
            pw_loop_destroy(pwMainLoop);
        }
    }

    bool init() {
        pwMainLoop = pw_loop_new(nullptr);
        pw_loop_enter(pwMainLoop);

        QSocketNotifier *notifier = new QSocketNotifier(pw_loop_get_fd(pwMainLoop), QSocketNotifier::Read, this);
        connect(notifier, &QSocketNotifier::activated, this, [this] {
                int result = pw_loop_iterate (pwMainLoop, 0);
                if (result < 0)
                    qCWarning(PIPEWIRE_LOGGING) << "pipewire_loop_iterate failed: " << spa_strerror (result);
            }
        );

        pwContext = pw_context_new(pwMainLoop, nullptr, 0);
        if (!pwContext) {
            qCWarning(PIPEWIRE_LOGGING) << "Failed to create PipeWire context";
            m_error = i18n("Failed to create PipeWire context");
            return false;
        }

        pwCore = pw_context_connect(pwContext, nullptr, 0);
        if (!pwCore) {
            qCWarning(PIPEWIRE_LOGGING) << "Failed to connect PipeWire context";
            m_error = i18n("Failed to connect PipeWire context");
            return false;
        }

        if (pw_loop_iterate(pwMainLoop, 0) < 0) {
            qCWarning(PIPEWIRE_LOGGING) << "Failed to start main PipeWire loop";
            m_error = i18n("Failed to start main PipeWire loop");
            return false;
        }

        pw_core_add_listener(pwCore, &coreListener, &pwCoreEvents, this);
        return true;
    }

    static QSharedPointer<PipeWireCore> self()
    {
        static QWeakPointer<PipeWireCore> global;
        QSharedPointer<PipeWireCore> ret;
        if (global) {
            ret = global.toStrongRef();
        } else {
            ret.reset(new PipeWireCore);
            ret->init();
            global = ret;
        }
        return ret;
    }

    struct pw_core *pwCore = nullptr;
    struct pw_context *pwContext = nullptr;
    struct pw_loop *pwMainLoop = nullptr;
    spa_hook coreListener;
    QString m_error;

    pw_core_events pwCoreEvents = {};

Q_SIGNALS:
    void pipewireFailed(const QString &message);
};

PipeWireSourceStream::PipeWireSourceStream(EGLDisplay eglDisplay, const QSize &resolution, QObject *parent)
    : QObject(parent)
    , m_resolution(resolution)
    , m_eglDisplay(eglDisplay)
{
    pwStreamEvents.version = PW_VERSION_STREAM_EVENTS;
    pwStreamEvents.process = &onProcess;
    pwStreamEvents.state_changed = &PipeWireSourceStream::onStreamStateChanged;
    pwStreamEvents.param_changed = &PipeWireSourceStream::onStreamParamChanged;
}

PipeWireSourceStream::~PipeWireSourceStream()
{
    m_stopped = true;
    if (pwStream) {
        pw_stream_destroy(pwStream);
    }
}

uint PipeWireSourceStream::framerate()
{
    if (pwStream) {
        return videoFormat.max_framerate.num / videoFormat.max_framerate.denom;
    }

    return 0;
}

uint PipeWireSourceStream::nodeId()
{
    return pwNodeId;
}

bool PipeWireSourceStream::createStream(uint nodeid)
{
    pwCore = PipeWireCore::self();
    if (!pwCore->m_error.isEmpty()) {
        m_error = pwCore->m_error;
        return false;
    }

    connect(pwCore.data(), &PipeWireCore::pipewireFailed, this, &PipeWireSourceStream::coreFailed);

    pwStream = pw_stream_new(pwCore->pwCore, "kwin-screen-cast", nullptr);
    pwNodeId = nodeid;
    pw_stream_add_listener(pwStream, &streamListener, &pwStreamEvents, this);

    uint8_t buffer[1024];
    spa_pod_builder podBuilder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_fraction minFramerate = SPA_FRACTION(1, 1);
    spa_fraction maxFramerate = SPA_FRACTION(25, 1);
    spa_fraction defaultFramerate = SPA_FRACTION(0, 1);

    spa_rectangle minResolution = SPA_RECTANGLE(1, 1);
    spa_rectangle maxResolution = spa_rectangle{INT32_MAX, INT32_MAX};

    const spa_pod *param = (spa_pod*)spa_pod_builder_add_object(&podBuilder,
                                        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
                                        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                                        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                                        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(4,
                                                    SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA,
                                                    SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA),
                                        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&minResolution, &minResolution, &maxResolution),
                                        SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&defaultFramerate),
                                        SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(&maxFramerate, &minFramerate, &maxFramerate));
    if (pw_stream_connect(pwStream, PW_DIRECTION_INPUT, pwNodeId, PW_STREAM_FLAG_AUTOCONNECT, &param, 1) != 0) {
        qCWarning(PIPEWIRE_LOGGING) << "Could not connect to stream";
        pw_stream_destroy(pwStream);
        return false;
    }

    pw_stream_set_active(pwStream, true);
    return true;
}

void PipeWireSourceStream::handleFrame(struct pw_buffer* buffer)
{
    spa_buffer* spaBuffer = buffer->buffer;

    if (spaBuffer->datas->chunk->size == 0) {
        return;
    }

    if (spaBuffer->datas->type == SPA_DATA_MemFd) {
        uint8_t *map = static_cast<uint8_t*>(mmap(
            nullptr, spaBuffer->datas->maxsize + spaBuffer->datas->mapoffset,
            PROT_READ, MAP_PRIVATE, spaBuffer->datas->fd, 0));

        if (map == MAP_FAILED) {
            qCWarning(PIPEWIRE_LOGGING) << "Failed to mmap the memory: " << strerror(errno);
            return;
        }

        QImage img(map, videoFormat.size.width, videoFormat.size.height, spaBuffer->datas->chunk->stride, QImage::Format_ARGB32);
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
            planes += plane;
        }
        const QSize size(videoFormat.size.width, videoFormat.size.height);
        Q_EMIT dmabufTextureReceived(planes, DRM_FORMAT_ARGB8888, size);
    } else if (spaBuffer->datas->type == SPA_DATA_MemPtr) {
        QImage img(static_cast<uint8_t*>(spaBuffer->datas->data), videoFormat.size.width, videoFormat.size.height, spaBuffer->datas->chunk->stride, QImage::Format_ARGB32);
        Q_EMIT imageTextureReceived(img);
    } else {
        qWarning() << "unsupported buffer type" << spaBuffer->datas->type;
        QImage errorImage(200, 200, QImage::Format_ARGB32);
        errorImage.fill(Qt::red);
        Q_EMIT imageTextureReceived(errorImage);
    }
}

void PipeWireSourceStream::coreFailed(const QString &errorMessage)
{
    m_error = errorMessage;
    Q_EMIT stopStreaming();
}

void PipeWireSourceStream::stop()
{
    if (!m_stopped)
        pw_stream_set_active(pwStream, false);
    m_stopped = true;
    delete this;
}

#include "pipewirestream.moc"
