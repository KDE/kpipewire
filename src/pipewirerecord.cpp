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
#include <QGuiApplication>
#include <QImage>
#include <QMutex>
#include <QPainter>
#include <QThreadPool>
#include <QTimer>
#include <qpa/qplatformnativeinterface.h>

#include <KShell>

#include <fcntl.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/timestamp.h>
#include <libswscale/swscale.h>
}

#ifdef av_err2str
#undef av_err2str
#include <string>
char str[AV_ERROR_MAX_STRING_SIZE];
av_always_inline char *av_err2str(int errnum)
{
    return av_make_error_string(str, AV_ERROR_MAX_STRING_SIZE, errnum);
}
#endif // av_err2str

#ifdef av_ts2str
#undef av_ts2str
char buf[AV_TS_MAX_STRING_SIZE];
#define av_ts2str(ts) av_ts_make_string(buf, ts)
#endif // av_ts2str

#ifdef av_ts2timestr
#undef av_ts2timestr
char timebuf[AV_TS_MAX_STRING_SIZE];
#define av_ts2timestr(ts, tb) av_ts_make_time_string(timebuf, ts, tb)
#endif // av_ts2timestr

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

    d->m_fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
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

PipeWireRecordProduce::PipeWireRecordProduce(const QByteArray &encoder, uint nodeId, uint fd, const QString &output)
    : QObject()
    , m_output(output)
    , m_nodeId(nodeId)
    , m_encoder(encoder)
{
    m_stream.reset(new PipeWireSourceStream(nullptr));
    bool created = m_stream->createStream(m_nodeId, fd);
    if (!created || !m_stream->error().isEmpty()) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "failed to set up stream for" << m_nodeId << m_stream->error();
        m_error = m_stream->error();
        m_stream.reset(nullptr);
        return;
    }
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
    if (m_producer) {
        m_producer->m_stream->setActive(false);
    }
}

void PipeWireRecordProduce::finish()
{
    if (!m_stream) {
        qCDebug(PIPEWIRERECORD_LOGGING) << "finished without a stream";
        return;
    }

    disconnect(m_stream.data(), &PipeWireSourceStream::frameReceived, this, &PipeWireRecordProduce::processFrame);
    if (m_writeThread) {
        m_writeThread->drain();
        bool done = QThreadPool::globalInstance()->waitForDone(-1);
        Q_ASSERT(done);
    }

    qCDebug(PIPEWIRERECORD_LOGGING) << "finished";
    if (m_avCodecContext) {
        avio_closep(&m_avFormatContext->pb);
        avcodec_close(m_avCodecContext);
        av_free(m_avCodecContext);
        avformat_free_context(m_avFormatContext);
    }
}

QString PipeWireRecord::extension()
{
    return QStringLiteral("mp4");
}

void PipeWireRecordProduce::setupStream()
{
    qCDebug(PIPEWIRERECORD_LOGGING) << "Setting up stream";
    disconnect(m_stream.get(), &PipeWireSourceStream::streamParametersChanged, this, &PipeWireRecordProduce::setupStream);
    avformat_alloc_output_context2(&m_avFormatContext, nullptr, nullptr, m_output.toUtf8().constData());
    if (!m_avFormatContext) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not deduce output format from file: using MPEG." << m_output;
        avformat_alloc_output_context2(&m_avFormatContext, nullptr, "mpeg", m_output.toUtf8().constData());
    }
    if (!m_avFormatContext) {
        qCDebug(PIPEWIRERECORD_LOGGING) << "could not set stream up";
        return;
    }

    m_codec = avcodec_find_encoder_by_name(m_encoder.constData());
    if (!m_codec) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Codec not found";
        return;
    }

    m_avCodecContext = avcodec_alloc_context3(m_codec);
    if (!m_avCodecContext) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not allocate video codec context";
        return;
    }
    m_avCodecContext->bit_rate = 100000000;

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

    if (avcodec_open2(m_avCodecContext, m_codec, nullptr) < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not open codec";
        return;
    }

    m_frame.reset(new CustomAVFrame);
    int ret = m_frame->alloc(m_avCodecContext->width, m_avCodecContext->height, m_avCodecContext->pix_fmt);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not allocate raw picture buffer" << av_err2str(ret);
        return;
    }

    ret = avio_open(&m_avFormatContext->pb, QFile::encodeName(m_output).constData(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not open" << m_output << av_err2str(ret);
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
        qCWarning(PIPEWIRERECORD_LOGGING) << "Error occurred when passing the codec:" << av_err2str(ret);
        return;
    }

    ret = avformat_write_header(m_avFormatContext, nullptr);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Error occurred when writing header:" << av_err2str(ret);
        return;
    }

    connect(m_stream.data(), &PipeWireSourceStream::frameReceived, this, &PipeWireRecordProduce::processFrame);
    m_writeThread = new PipeWireRecordWriteThread(&m_bufferNotEmpty, m_avFormatContext, m_avCodecContext);
    QThreadPool::globalInstance()->start(m_writeThread);
}

void PipeWireRecordProduce::processFrame(const PipeWireFrame &frame)
{
    bool cursorChanged = false;
    if (frame.cursor) {
        cursorChanged = m_cursor.position != frame.cursor->position;
        m_cursor.position = frame.cursor->position;
        m_cursor.hotspot = frame.cursor->hotspot;
        if (!frame.cursor->texture.isNull()) {
            m_cursor.dirty = true;
            m_cursor.texture = frame.cursor->texture;
        }
    }

    if (frame.dmabuf) {
        QImage qimage(m_stream->size(), QImage::Format_RGBA8888);
        if (!m_dmabufHandler.downloadFrame(qimage, frame)) {
            m_stream->renegotiateModifierFailed(frame.format, frame.dmabuf->modifier);
            return;
        }
        updateTextureImage(qimage);
    } else if (frame.image) {
        updateTextureImage(*frame.image);
    } else if (cursorChanged && !m_frameWithoutMetadataCursor.isNull()) {
        render();
    }
}

void PipeWireRecord::refresh()
{
    if (!d->m_output.isEmpty() && d->m_active && d->m_nodeId > 0) {
        d->m_recordThread.reset(new PipeWireRecordProduceThread(d->m_encoder, d->m_nodeId, d->m_fd, d->m_output));
        connect(d->m_recordThread.get(), &PipeWireRecordProduceThread::errorFound, this, &PipeWireRecord::errorFound);
        connect(d->m_recordThread.get(), &PipeWireRecordProduceThread::finished, this, [this] {
            setActive(false);
        });
        d->m_recordThread->start();
    } else if (d->m_recordThread) {
        d->m_recordThread->deactivate();
        d->m_recordThread->quit();

        connect(d->m_recordThread.get(), &PipeWireRecordProduceThread::finished, this, [this] {
            qCDebug(PIPEWIRERECORD_LOGGING) << "produce thread finished" << d->m_output;
            d->m_recordThread.reset();
            d->m_produceThreadFinished = true;
            Q_EMIT stateChanged();
        });
        d->m_produceThreadFinished = false;
        d->m_recordThread.release();
    }
    Q_EMIT stateChanged();
}

void PipeWireRecordProduce::updateTextureImage(const QImage &image)
{
    m_frameWithoutMetadataCursor = image;
    render();
}

void PipeWireRecordProduce::render()
{
    Q_ASSERT(!m_frameWithoutMetadataCursor.isNull());

    QImage image(m_frameWithoutMetadataCursor);
    if (!image.isNull() && m_cursor.position && !m_cursor.texture.isNull()) {
        image = m_frameWithoutMetadataCursor.copy();
        QPainter p(&image);
        p.drawImage(*m_cursor.position, m_cursor.texture);
    }

    const std::uint8_t *buffers[] = {image.constBits(), nullptr};
    const int strides[] = {static_cast<int>(image.bytesPerLine()), 0, 0, 0};
    struct SwsContext *sws_context = nullptr;
    sws_context = sws_getCachedContext(sws_context,
                                       image.width(),
                                       image.height(),
                                       AV_PIX_FMT_RGB32,
                                       m_avCodecContext->width,
                                       m_avCodecContext->height,
                                       m_avCodecContext->pix_fmt,
                                       0,
                                       nullptr,
                                       nullptr,
                                       nullptr);
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

    const int ret = avcodec_send_frame(m_avCodecContext, m_frame->m_avFrame);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Error sending a frame for encoding:" << av_err2str(ret);
        return;
    }
    m_bufferNotEmpty.wakeAll();
}

static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt)
{
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
                int sent = avcodec_send_frame(m_avCodecContext, nullptr);
                qCDebug(PIPEWIRERECORD_LOGGING) << "draining" << sent;
            }
            continue;
        } else if (ret < 0) {
            qCWarning(PIPEWIRERECORD_LOGGING) << "Error encoding a frame: " << av_err2str(ret);
            continue;
        }

        m_packet->stream_index = (*m_avFormatContext->streams)->index;
        av_packet_rescale_ts(m_packet, m_avCodecContext->time_base, (*m_avFormatContext->streams)->time_base);
        log_packet(m_avFormatContext, m_packet);
        ret = av_interleaved_write_frame(m_avFormatContext, m_packet);
        if (ret < 0) {
            qCWarning(PIPEWIRERECORD_LOGGING) << "Error while writing output packet:" << av_err2str(ret);
            continue;
        }
    }
    ret = av_write_trailer(m_avFormatContext);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "failed to write trailer" << av_err2str(ret);
    }
}

void PipeWireRecordWriteThread::drain()
{
    m_active = false;
    m_bufferNotEmpty->wakeAll();
}
