/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "pipewirerecord.h"
#include "glhelpers.h"
#include "pipewirerecord_p.h"
#include "pipewiresourcestream.h"
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <logging_record.h>

#include <QDateTime>
#include <QDebug>
#include <QGuiApplication>
#include <QImage>
#include <QMutex>
#include <QThreadPool>
#include <QTimer>
#include <qpa/qplatformnativeinterface.h>

#include <KShell>

#include <fcntl.h>
#include <unistd.h>

#include <gbm.h>
#include <xf86drm.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/timestamp.h>
#include <libswscale/swscale.h>
}

class CustomAVFrame
{
public:
    CustomAVFrame()
        : m_avFrame(av_frame_alloc())
    {
    }

    ~CustomAVFrame()
    {
        av_freep(m_avFrame->data);
        av_frame_free(&m_avFrame);
    }

    int alloc(int width, int height, AVPixelFormat pix_fmt)
    {
        m_avFrame->format = pix_fmt;
        m_avFrame->width = width;
        m_avFrame->height = height;
        return av_image_alloc(m_avFrame->data, m_avFrame->linesize, width, height, pix_fmt, 32);
    }

    AVFrame *m_avFrame;
};

PipeWireRecord::PipeWireRecord(QObject *parent)
    : QObject(parent)
    , d(new PipeWireRecordPrivate)
{
    d->m_encoder = "libx264rgb";
    av_log_set_level(AV_LOG_DEBUG);
}

PipeWireRecord::~PipeWireRecord()
{
    setActive(false);
}

PipeWireRecord::State PipeWireRecord::state() const
{
    if (d->m_active)
        return Recording;
    else if (d->m_recordThread || !d->m_produceThreadFinished)
        return Rendering;

    return Idle;
}

void PipeWireRecord::setNodeId(uint nodeId)
{
    if (nodeId == d->m_nodeId)
        return;

    d->m_nodeId = nodeId;
    refresh();
    Q_EMIT nodeIdChanged(nodeId);
}

void PipeWireRecord::setFd(uint fd)
{
    if (fd == d->m_fd)
        return;

    d->m_fd = fd;
    refresh();
    Q_EMIT fdChanged(fd);
}

void PipeWireRecord::setActive(bool active)
{
    if (d->m_active == active)
        return;

    d->m_active = active;
    refresh();
    Q_EMIT activeChanged(active);
}

void PipeWireRecord::setOutput(const QString &_output)
{
    const QString output = KShell::tildeExpand(_output);

    if (d->m_output == output)
        return;

    d->m_output = output;
    refresh();
    Q_EMIT outputChanged(output);
}

QByteArray fetchRenderNode()
{
    int max_devices = drmGetDevices2(0, nullptr, 0);
    if (max_devices <= 0) {
        qWarning() << "drmGetDevices2() has not found any devices (errno=" << -max_devices << ")";
        return "/dev/dri/renderD128";
    }

    std::vector<drmDevicePtr> devices(max_devices);
    int ret = drmGetDevices2(0, devices.data(), max_devices);
    if (ret < 0) {
        qWarning() << "drmGetDevices2() returned an error " << ret;
        return "/dev/dri/renderD128";
    }

    QByteArray render_node;

    for (const drmDevicePtr &device : devices) {
        if (device->available_nodes & (1 << DRM_NODE_RENDER)) {
            render_node = device->nodes[DRM_NODE_RENDER];
            break;
        }
    }

    drmFreeDevices(devices.data(), ret);
    return render_node;
}

void PipeWireRecordProduce::setupEGL()
{
    if (m_eglInitialized) {
        return;
    }

    const QList<QByteArray> extensions = GLHelpers::eglExtensions();

    // Use eglGetPlatformDisplayEXT() to get the display pointer
    // if the implementation supports it.
    if (!extensions.contains(QByteArrayLiteral("EGL_EXT_platform_base")) || !extensions.contains(QByteArrayLiteral("EGL_MESA_platform_gbm"))) {
        qWarning() << "One of required EGL extensions is missing";
        return;
    }

    m_egl.display = static_cast<EGLDisplay>(QGuiApplication::platformNativeInterface()->nativeResourceForIntegration("egldisplay"));
    if (m_egl.display == EGL_NO_DISPLAY) {
        m_egl.display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, (void *)EGL_DEFAULT_DISPLAY, nullptr);
    }
    if (m_egl.display == EGL_NO_DISPLAY) {
        const QByteArray renderNode = fetchRenderNode();
        m_drmFd = open(renderNode, O_RDWR);

        if (m_drmFd < 0) {
            qWarning() << "Failed to open drm render node" << renderNode << "with error: " << strerror(errno);
            return;
        }

        m_gbmDevice = gbm_create_device(m_drmFd);

        if (!m_gbmDevice) {
            qWarning() << "Cannot create GBM device: " << strerror(errno);
            return;
        }
        m_egl.display = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_MESA, m_gbmDevice, nullptr);
    }

    if (m_egl.display == EGL_NO_DISPLAY) {
        qWarning() << "Error during obtaining EGL display: " << GLHelpers::formatGLError(eglGetError());
        return;
    }

    EGLint major, minor;
    if (eglInitialize(m_egl.display, &major, &minor) == EGL_FALSE) {
        qWarning() << "Error during eglInitialize: " << GLHelpers::formatGLError(eglGetError());
        return;
    }

    if (eglBindAPI(EGL_OPENGL_API) == EGL_FALSE) {
        qWarning() << "bind OpenGL API failed";
        return;
    }

    m_egl.context = eglCreateContext(m_egl.display, nullptr, EGL_NO_CONTEXT, nullptr);

    if (m_egl.context == EGL_NO_CONTEXT) {
        qWarning() << "Couldn't create EGL context: " << GLHelpers::formatGLError(eglGetError());
        return;
    }

    qCDebug(PIPEWIRERECORD_LOGGING) << "Egl initialization succeeded";
    qCDebug(PIPEWIRERECORD_LOGGING) << QStringLiteral("EGL version: %1.%2").arg(major).arg(minor);

    m_eglInitialized = true;
}

PipeWireRecordProduce::PipeWireRecordProduce(const QByteArray &encoder, uint nodeId, uint fd, const QString &output)
    : QObject()
    , m_output(output)
    , m_nodeId(nodeId)
    , m_encoder(encoder)
{
    setupEGL();

    m_stream.reset(new PipeWireSourceStream(nullptr));
    bool created = m_stream->createStream(m_nodeId, fd);
    if (!created || !m_stream->error().isEmpty()) {
        qWarning() << "failed to set up stream for" << m_nodeId << m_stream->error();
        m_error = m_stream->error();
        m_stream.reset(nullptr);
        return;
    }
    m_stream->setActive(true);
    connect(m_stream.get(), &PipeWireSourceStream::streamParametersChanged, this, &PipeWireRecordProduce::setupStream);
}

PipeWireRecordProduce::~PipeWireRecordProduce() noexcept
{
    finish();
}

void PipeWireRecordProduceThread::run()
{
    PipeWireRecordProduce produce(m_encoder, m_nodeId, m_fd, m_output);
    if (!produce.m_stream) {
        Q_EMIT errorFound(produce.error());
        return;
    }
    m_producer = &produce;
    qCDebug(PIPEWIRERECORD_LOGGING) << "executing";
    const int ret = exec();
    qCDebug(PIPEWIRERECORD_LOGGING) << "finishing" << ret;
    m_producer = nullptr;
}

void PipeWireRecordProduceThread::deactivate()
{
    m_producer->m_stream->setActive(false);
}

void PipeWireRecordProduce::finish()
{
    if (!m_stream) {
        return;
    }

    disconnect(m_stream.data(), &PipeWireSourceStream::dmabufTextureReceived, this, &PipeWireRecordProduce::updateTextureDmaBuf);
    disconnect(m_stream.data(), &PipeWireSourceStream::imageTextureReceived, this, &PipeWireRecordProduce::updateTextureImage);
    if (m_writeThread) {
        m_writeThread->drain();
        bool done = QThreadPool::globalInstance()->waitForDone(-1);
        Q_ASSERT(done);
    }

    qCDebug(PIPEWIRERECORD_LOGGING) << "finished";
    avio_closep(&m_avFormatContext->pb);
    avcodec_close(m_avCodecContext);
    av_free(m_avCodecContext);
    avformat_free_context(m_avFormatContext);

    if (m_drmFd) {
        close(m_drmFd);
    }
}

void PipeWireRecordProduce::setupStream()
{
    disconnect(m_stream.get(), &PipeWireSourceStream::streamParametersChanged, this, &PipeWireRecordProduce::setupStream);
    avformat_alloc_output_context2(&m_avFormatContext, NULL, NULL, m_output.toUtf8());
    if (!m_avFormatContext) {
        qWarning() << "Could not deduce output format from file: using MPEG." << m_output;
        avformat_alloc_output_context2(&m_avFormatContext, NULL, "mpeg", m_output.toUtf8());
    }
    if (!m_avFormatContext)
        return;

    m_codec = avcodec_find_encoder_by_name(m_encoder);
    if (!m_codec) {
        qWarning() << "Codec not found";
        return;
    }

    m_avCodecContext = avcodec_alloc_context3(m_codec);
    if (!m_avCodecContext) {
        qWarning() << "Could not allocate video codec context";
        return;
    }
    m_avCodecContext->bit_rate = 50000000;

    const QSize size = m_stream->size();
    const Fraction framerate = m_stream->framerate();

    Q_ASSERT(!size.isEmpty());
    m_avCodecContext->width = size.width();
    m_avCodecContext->height = size.height();
    m_avCodecContext->max_b_frames = 1;
    m_avCodecContext->gop_size = 1;
    if (m_codec->pix_fmts && m_codec->pix_fmts[0] > 0) {
        m_avCodecContext->pix_fmt = m_codec->pix_fmts[0];
    } else {
        m_avCodecContext->pix_fmt = AV_PIX_FMT_YUV420P;
    }
    m_avCodecContext->time_base = AVRational{1, 1000};

    if (avcodec_open2(m_avCodecContext, m_codec, NULL) < 0) {
        qWarning() << "Could not open codec";
        return;
    }

    m_frame.reset(new CustomAVFrame);
    int ret = m_frame->alloc(m_avCodecContext->width, m_avCodecContext->height, m_avCodecContext->pix_fmt);
    if (ret < 0) {
#if LIBAVUTIL_VERSION_MAJOR >= 57
        qWarning() << "Could not allocate raw picture buffer" << av_err2str(ret);
#endif
        return;
    }

    ret = avio_open(&m_avFormatContext->pb, QFile::encodeName(m_output), AVIO_FLAG_WRITE);
    if (ret < 0) {
#if LIBAVUTIL_VERSION_MAJOR >= 57
        qWarning() << "Could not open" << m_output << av_err2str(ret);
#endif
        return;
    }

    auto avStream = avformat_new_stream(m_avFormatContext, nullptr);
    avStream->start_time = 0;
    avStream->r_frame_rate.num = framerate.numerator;
    avStream->r_frame_rate.den = framerate.denominator;
    avStream->avg_frame_rate.num = framerate.numerator;
    avStream->avg_frame_rate.den = framerate.denominator;

    ret = avcodec_parameters_from_context(avStream->codecpar, m_avCodecContext);
    if (ret < 0) {
#if LIBAVUTIL_VERSION_MAJOR >= 57
        qWarning() << "Error occurred when passing the codec:" << av_err2str(ret);
#endif
        return;
    }

    ret = avformat_write_header(m_avFormatContext, NULL);
    if (ret < 0) {
#if LIBAVUTIL_VERSION_MAJOR >= 57
        qWarning() << "Error occurred when writing header:" << av_err2str(ret);
#endif
        return;
    }

    connect(m_stream.data(), &PipeWireSourceStream::dmabufTextureReceived, this, &PipeWireRecordProduce::updateTextureDmaBuf);
    connect(m_stream.data(), &PipeWireSourceStream::imageTextureReceived, this, &PipeWireRecordProduce::updateTextureImage);
    m_writeThread = new PipeWireRecordWriteThread(&m_bufferNotEmpty, m_avFormatContext, m_avCodecContext);
    QThreadPool::globalInstance()->start(m_writeThread);
}

void PipeWireRecord::refresh()
{
    if (!d->m_output.isEmpty() && d->m_active && d->m_nodeId > 0) {
        d->m_recordThread = new PipeWireRecordProduceThread(d->m_encoder, d->m_nodeId, d->m_fd, d->m_output);
        connect(d->m_recordThread, &PipeWireRecordProduceThread::errorFound, this, &PipeWireRecord::errorFound);
        connect(d->m_recordThread, &PipeWireRecordProduceThread::finished, this, [this] {
            setActive(false);
        });
        d->m_recordThread->start();
    } else if (d->m_recordThread) {
        d->m_recordThread->deactivate();
        d->m_recordThread->quit();

        connect(d->m_recordThread, &PipeWireRecordProduceThread::finished, this, [this] {
            qCDebug(PIPEWIRERECORD_LOGGING) << "produce thread finished" << d->m_output;
            delete d->m_recordThread;
            d->m_produceThreadFinished = true;
            Q_EMIT stateChanged();
        });
        d->m_produceThreadFinished = false;
        d->m_recordThread = nullptr;
    }
    Q_EMIT stateChanged();
}

void PipeWireRecordProduce::updateTextureDmaBuf(const QVector<DmaBufPlane> &plane, uint32_t /*format*/)
{
    Q_ASSERT(qGuiApp->thread() != QThread::currentThread());
    const QSize streamSize = m_stream->size();
    gbm_import_fd_data importInfo = {int(plane[0].fd), uint32_t(streamSize.width()), uint32_t(streamSize.height()), plane[0].stride, GBM_BO_FORMAT_ARGB8888};
    gbm_bo *imported = gbm_bo_import(m_gbmDevice, GBM_BO_IMPORT_FD, &importInfo, GBM_BO_USE_SCANOUT);
    if (!imported) {
        qWarning() << "Failed to process buffer: Cannot import passed GBM fd - " << strerror(errno);
        return;
    }

    // bind context to render thread
    eglMakeCurrent(m_egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_egl.context);

    // create EGL image from imported BO
    EGLImageKHR image = eglCreateImageKHR(m_egl.display, nullptr, EGL_NATIVE_PIXMAP_KHR, imported, nullptr);

    if (image == EGL_NO_IMAGE_KHR) {
        qWarning() << "Failed to record frame: Error creating EGLImageKHR - " << GLHelpers::formatGLError(glGetError());
        gbm_bo_destroy(imported);
        return;
    }

    // create GL 2D texture for framebuffer
    GLuint texture;
    glGenTextures(1, &texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, texture);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);

    auto src = static_cast<uint8_t *>(malloc(plane[0].stride * streamSize.height()));
    glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, src);

    if (!src) {
        qWarning() << "Failed to get image from DMA buffer.";
        gbm_bo_destroy(imported);
        return;
    }

    QImage qimage(src, streamSize.width(), streamSize.height(), plane[0].stride, QImage::Format_ARGB32);
    updateTextureImage(qimage);

    glDeleteTextures(1, &texture);
    eglDestroyImageKHR(m_egl.display, image);

    gbm_bo_destroy(imported);

    free(src);
}

void PipeWireRecordProduce::updateTextureImage(const QImage &image)
{
    QElapsedTimer t;
    t.start();
    const std::uint8_t *buffers[] = {image.constBits(), 0};
    const int strides[] = {image.bytesPerLine(), 0, 0, 0};
    struct SwsContext *sws_context = nullptr;
    sws_context = sws_getCachedContext(sws_context,
                                       image.width(),
                                       image.height(),
                                       AV_PIX_FMT_RGB32,
                                       m_avCodecContext->width,
                                       m_avCodecContext->height,
                                       m_avCodecContext->pix_fmt,
                                       0,
                                       NULL,
                                       NULL,
                                       NULL);
    sws_scale(sws_context, buffers, strides, 0, m_avCodecContext->height, m_frame->m_avFrame->data, m_frame->m_avFrame->linesize);

    if (auto v = m_stream->currentPresentationTimestamp(); v.has_value()) {
        const auto current = std::chrono::duration_cast<std::chrono::milliseconds>(v.value()).count();
        if ((*m_avFormatContext->streams)->start_time == 0) {
            (*m_avFormatContext->streams)->start_time = current;
        }

        Q_ASSERT((*m_avFormatContext->streams)->start_time <= current);
        m_frame->m_avFrame->pts = current - (*m_avFormatContext->streams)->start_time;
    } else {
        m_frame->m_avFrame->pts = AV_NOPTS_VALUE;
    }

#if LIBAVUTIL_VERSION_MAJOR >= 57
    static int i = 0;
    ++i;
    qCDebug(PIPEWIRERECORD_LOGGING) << "sending frame" << i << av_ts2str(m_frame->m_avFrame->pts)
                                    << "fps: " << double(i * 1000) / double(m_frame->m_avFrame->pts);
#endif
    const int ret = avcodec_send_frame(m_avCodecContext, m_frame->m_avFrame);

#if LIBAVUTIL_VERSION_MAJOR >= 57
    qCDebug(PIPEWIRERECORD_LOGGING) << "sent frames" << i << av_ts2str(m_frame->m_avFrame->pts) << t.elapsed();
#endif
    if (ret < 0) {
#if LIBAVUTIL_VERSION_MAJOR >= 57
        qWarning() << "Error sending a frame for encoding:" << av_err2str(ret);
#endif
        return;
    }
    m_bufferNotEmpty.wakeAll();
}

static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt)
{
#if LIBAVUTIL_VERSION_MAJOR >= 57
    AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

    qCDebug(PIPEWIRERECORD_LOGGING,
            "pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s "
            "stream_index:%d",
            av_ts2str(pkt->pts),
            av_ts2timestr(pkt->pts, time_base),
            av_ts2str(pkt->dts),
            av_ts2timestr(pkt->dts, time_base),
            av_ts2str(pkt->duration),
            av_ts2timestr(pkt->duration, time_base),
            pkt->stream_index);
#else
    Q_UNUSED(fmt_ctx)
    Q_UNUSED(pkt)
#endif
}

void PipeWireRecord::setEncoder(const QByteArray &encoder)
{
    d->m_encoder = encoder;
}

QString PipeWireRecord::output() const
{
    return d->m_output;
}

bool PipeWireRecord::isActive() const
{
    return d->m_active;
}

uint PipeWireRecord::nodeId() const
{
    return d->m_nodeId;
}

uint PipeWireRecord::fd() const
{
    return d->m_fd;
}

PipeWireRecordWriteThread::PipeWireRecordWriteThread(QWaitCondition *notEmpty, AVFormatContext *avFormatContext, AVCodecContext *avCodecContext)
    : QRunnable()
    , m_packet(av_packet_alloc())
    , m_avFormatContext(avFormatContext)
    , m_avCodecContext(avCodecContext)
    , m_bufferNotEmpty(notEmpty)
{
}

PipeWireRecordWriteThread::~PipeWireRecordWriteThread()
{
    av_packet_free(&m_packet);
}

void PipeWireRecordWriteThread::run()
{
    QMutex mutex;
    int ret = 0;
    while (true) {
        ret = avcodec_receive_packet(m_avCodecContext, m_packet);
        if (ret == AVERROR_EOF) {
            break;
        } else if (ret == AVERROR(EAGAIN)) {
            if (m_active) {
                m_bufferNotEmpty->wait(&mutex);
            } else {
                int sent = avcodec_send_frame(m_avCodecContext, NULL);
                qCDebug(PIPEWIRERECORD_LOGGING) << "draining" << sent;
            }
            continue;
        } else if (ret < 0) {
#if LIBAVUTIL_VERSION_MAJOR >= 57
            qWarning() << "Error encoding a frame: " << av_err2str(ret);
#endif
            continue;
        }

#if LIBAVUTIL_VERSION_MAJOR >= 57
        static int i = 0;
        ++i;
        qCDebug(PIPEWIRERECORD_LOGGING) << "receiving packets" << i << m_active << av_ts2str(m_packet->pts) << (*m_avFormatContext->streams)->index;
#endif
        m_packet->stream_index = (*m_avFormatContext->streams)->index;
        av_packet_rescale_ts(m_packet, m_avCodecContext->time_base, (*m_avFormatContext->streams)->time_base);
        log_packet(m_avFormatContext, m_packet);
        ret = av_interleaved_write_frame(m_avFormatContext, m_packet);
        if (ret < 0) {
#if LIBAVUTIL_VERSION_MAJOR >= 57
            qWarning() << "Error while writing output packet:" << av_err2str(ret);
#endif
            continue;
        }
    }
    ret = av_write_trailer(m_avFormatContext);
#if LIBAVUTIL_VERSION_MAJOR >= 57
    if (ret < 0) {
        qWarning() << "failed to write trailer" << av_err2str(ret);
    }
#endif
}

void PipeWireRecordWriteThread::drain()
{
    m_active = false;
    m_bufferNotEmpty->wakeAll();
}
