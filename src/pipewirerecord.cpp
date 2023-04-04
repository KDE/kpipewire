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

Q_DECLARE_METATYPE(std::optional<int>);
Q_DECLARE_METATYPE(std::optional<std::chrono::nanoseconds>);

PipeWireRecord::PipeWireRecord(QObject *parent)
    : QObject(parent)
    , d(new PipeWireRecordPrivate)
{
    d->m_encoder = "libvpx";
    av_log_set_level(AV_LOG_DEBUG);
    qRegisterMetaType<std::optional<int>>();
    qRegisterMetaType<std::optional<std::chrono::nanoseconds>>();
}

PipeWireRecord::~PipeWireRecord()
{
    setActive(false);
    if (d->m_fd) {
        close(*d->m_fd);
    }

    if (d->m_recordThread) {
        d->m_recordThread->wait();
    }
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

    if (d->m_fd) {
        close(*d->m_fd);
    }
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
    connect(m_stream.get(), &PipeWireSourceStream::stateChanged, this, &PipeWireRecordProduce::stateChanged);
}

PipeWireRecordProduce::~PipeWireRecordProduce()
{
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
        m_producer->m_deactivated = true;
        m_producer->m_stream->setActive(false);
    }
}

void PipeWireRecordProduce::stateChanged(pw_stream_state state)
{
    if (state != PW_STREAM_STATE_PAUSED || !m_deactivated) {
        return;
    }
    if (!m_stream) {
        qCDebug(PIPEWIRERECORD_LOGGING) << "finished without a stream";
        return;
    }

    disconnect(m_stream.data(), &PipeWireSourceStream::frameReceived, this, &PipeWireRecordProduce::processFrame);
    if (m_writeThread) {
        m_writeThread->quit();
        bool done = m_writeThread->wait();
        Q_ASSERT(done);
    }

    qCDebug(PIPEWIRERECORD_LOGGING) << "finished";
    if (m_avCodecContext) {
        avio_closep(&m_avFormatContext->pb);
        avcodec_close(m_avCodecContext);
        av_free(m_avCodecContext);
        avformat_free_context(m_avFormatContext);
    }
    QThread::currentThread()->quit();
}

QString PipeWireRecord::extension()
{
    return QStringLiteral("webm");
}

QString PipeWireRecord::currentExtension() const
{
    static QHash<QByteArray, QString> s_extensions = {
        {"libx264", QStringLiteral("mp4")},
        {"libvpx", QStringLiteral("webm")},
    };
    return s_extensions.value(d->m_encoder, QStringLiteral("mkv"));
}

void PipeWireRecordProduce::setupStream()
{
    qCDebug(PIPEWIRERECORD_LOGGING) << "Setting up stream";
    disconnect(m_stream.get(), &PipeWireSourceStream::streamParametersChanged, this, &PipeWireRecordProduce::setupStream);
    avformat_alloc_output_context2(&m_avFormatContext, nullptr, nullptr, m_output.toUtf8().constData());
    if (!m_avFormatContext) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not deduce output format from file: using WebM." << m_output;
        avformat_alloc_output_context2(&m_avFormatContext, nullptr, "webm", m_output.toUtf8().constData());
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

    const QSize size = m_stream->size();
    const Fraction framerate = m_stream->framerate();

    // Have the bitrate depend on the size of the input stream. What looks acceptable on a small
    // stream on a big one will look bad.
    m_avCodecContext->bit_rate = size.width() * size.height() * 2;

    Q_ASSERT(!size.isEmpty());
    m_avCodecContext->width = size.width();
    m_avCodecContext->height = size.height();
    m_avCodecContext->max_b_frames = 1;
    m_avCodecContext->gop_size = 100;
    if (m_codec->pix_fmts && m_codec->pix_fmts[0] > 0) {
        m_avCodecContext->pix_fmt = m_codec->pix_fmts[0];
    } else {
        m_avCodecContext->pix_fmt = AV_PIX_FMT_YUV420P;
    }
    m_avCodecContext->time_base = AVRational{1, 1000};

    AVDictionary *options = nullptr;
    av_dict_set_int(&options, "threads", qMin(16, QThread::idealThreadCount()), 0);
    av_dict_set(&options, "preset", "veryfast", 0);
    av_dict_set(&options, "tune-content", "screen", 0);
    av_dict_set(&options, "deadline", "realtime", 0);
    // In theory a lower number should be faster, but the opposite seems to be true
    av_dict_set(&options, "quality", "40", 0);
    av_dict_set(&options, "cpu-used", "6", 0);
    // Disable motion estimation, not great while dragging windows but speeds up encoding by an order of magnitude
    av_dict_set(&options, "flags", "+mv4", 0);
    // Disable in-loop filtering
    av_dict_set(&options, "-flags", "+loop", 0);

    int ret = avcodec_open2(m_avCodecContext, m_codec, &options);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not open codec" << av_err2str(ret);
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
    m_writeThread = new PipeWireRecordWriteThread(this, m_avFormatContext, m_avCodecContext);
    m_writeThread->start();
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
        if (m_frameWithoutMetadataCursor.size() != m_stream->size()) {
            m_frameWithoutMetadataCursor = QImage(m_stream->size(), QImage::Format_RGBA8888_Premultiplied);
        }

        if (!m_dmabufHandler.downloadFrame(m_frameWithoutMetadataCursor, frame)) {
            m_stream->renegotiateModifierFailed(frame.format, frame.dmabuf->modifier);
            return;
        }
        render(frame);
    } else if (frame.image) {
        updateTextureImage(*frame.image, frame);
    } else if (cursorChanged && !m_frameWithoutMetadataCursor.isNull()) {
        render(frame);
    }
}

void PipeWireRecord::refresh()
{
    if (!d->m_output.isEmpty() && d->m_active && d->m_nodeId > 0) {
        d->m_recordThread.reset(new PipeWireRecordProduceThread(d->m_encoder, d->m_nodeId, d->m_fd.value_or(0), d->m_output));
        connect(d->m_recordThread.get(), &PipeWireRecordProduceThread::errorFound, this, &PipeWireRecord::errorFound);
        connect(d->m_recordThread.get(), &PipeWireRecordProduceThread::finished, this, [this] {
            setActive(false);
        });
        d->m_recordThread->start();
    } else if (d->m_recordThread) {
        d->m_recordThread->deactivate();

        connect(d->m_recordThread.get(), &PipeWireRecordProduceThread::finished, this, [this] {
            qCDebug(PIPEWIRERECORD_LOGGING) << "produce thread finished" << d->m_output;
            d->m_recordThread.reset();
            d->m_produceThreadFinished = true;
            Q_EMIT stateChanged();
        });
        d->m_produceThreadFinished = false;
    }
    Q_EMIT stateChanged();
}

void PipeWireRecordProduce::updateTextureImage(const QImage &image, const PipeWireFrame &frame)
{
    m_frameWithoutMetadataCursor = image;
    render(frame);
}

void PipeWireRecordProduce::render(const PipeWireFrame &frame)
{
    Q_ASSERT(!m_frameWithoutMetadataCursor.isNull());

    QImage image(m_frameWithoutMetadataCursor);
    if (!image.isNull() && m_cursor.position && !m_cursor.texture.isNull()) {
        image = m_frameWithoutMetadataCursor.copy();
        QPainter p(&image);
        p.drawImage(*m_cursor.position, m_cursor.texture);
    }

    Q_EMIT producedFrame(image, frame.sequential, frame.presentationTimestamp);
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
    if (d->m_encoder == encoder) {
        return;
    }
    d->m_encoder = encoder;
    Q_EMIT encoderChanged();
}

QByteArray PipeWireRecord::encoder() const
{
    return d->m_encoder;
}

QList<QByteArray> PipeWireRecord::suggestedEncoders() const
{
    QList<QByteArray> ret = {"libvpx", "libx264"};
    std::remove_if(ret.begin(), ret.end(), [](const QByteArray &encoder) {
        return !avcodec_find_encoder_by_name(encoder.constData());
    });
    return ret;
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
    return d->m_fd.value_or(0);
}

PipeWireRecordWrite::PipeWireRecordWrite(PipeWireRecordProduce *produce, AVFormatContext *avFormatContext, AVCodecContext *avCodecContext)
    : QObject()
    , m_packet(av_packet_alloc())
    , m_avFormatContext(avFormatContext)
    , m_avCodecContext(avCodecContext)
{
    connect(produce, &PipeWireRecordProduce::producedFrame, this, &PipeWireRecordWrite::addFrame);
}

PipeWireRecordWrite::~PipeWireRecordWrite()
{
    int ret = av_write_trailer(m_avFormatContext);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "failed to write trailer" << av_err2str(ret);
    }
    av_packet_free(&m_packet);
}

PipeWireRecordWriteThread::PipeWireRecordWriteThread(PipeWireRecordProduce *produce, AVFormatContext *avFormatContext, AVCodecContext *avCodecContext)
    : QThread(produce)
    , m_produce(produce)
    , m_avFormatContext(avFormatContext)
    , m_avCodecContext(avCodecContext)
{
}

void PipeWireRecordWrite::addFrame(const QImage &image, std::optional<int> sequential, std::optional<std::chrono::nanoseconds> presentationTimestamp)
{
    if (!sws_context || m_lastReceivedSize != image.size()) {
        sws_context = sws_getCachedContext(sws_context,
                                           image.width(),
                                           image.height(),
                                           convertQImageFormatToAVPixelFormat(image.format()),
                                           m_avCodecContext->width,
                                           m_avCodecContext->height,
                                           m_avCodecContext->pix_fmt,
                                           0,
                                           nullptr,
                                           nullptr,
                                           nullptr);
    }

    CustomAVFrame avFrame;
    int ret = avFrame.alloc(m_avCodecContext->width, m_avCodecContext->height, m_avCodecContext->pix_fmt);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not allocate raw picture buffer" << av_err2str(ret);
        return;
    }
    const std::uint8_t *buffers[] = {image.constBits(), nullptr};
    const int strides[] = {static_cast<int>(image.bytesPerLine()), 0, 0, 0};
    sws_scale(sws_context, buffers, strides, 0, m_avCodecContext->height, avFrame.m_avFrame->data, avFrame.m_avFrame->linesize);

    if (presentationTimestamp.has_value()) {
        const auto current = std::chrono::duration_cast<std::chrono::milliseconds>(*presentationTimestamp).count();
        if ((*m_avFormatContext->streams)->start_time == 0) {
            (*m_avFormatContext->streams)->start_time = current;
        }

        Q_ASSERT((*m_avFormatContext->streams)->start_time <= current);
        avFrame.m_avFrame->pts = current - (*m_avFormatContext->streams)->start_time;
    } else {
        avFrame.m_avFrame->pts = AV_NOPTS_VALUE;
    }

    // Let's add a key frame every 100 frames and also the first frame
    if (sequential && (*sequential == 0 || (*sequential - m_lastKeyFrame) > 100)) {
        avFrame.m_avFrame->key_frame = 1;
        m_lastKeyFrame = *sequential;
    }

    if (m_lastPts > 0 && avFrame.m_avFrame->pts <= m_lastPts) {
        // Make sure we don't have two frames at the same presentation time
        avFrame.m_avFrame->pts = m_lastPts + 1;
    }
    m_lastPts = avFrame.m_avFrame->pts;

    ret = avcodec_send_frame(m_avCodecContext, avFrame.m_avFrame);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Error sending a frame for encoding:" << av_err2str(ret);
        return;
    }
    for (;;) {
        ret = avcodec_receive_packet(m_avCodecContext, m_packet);
        if (ret < 0) {
            if (ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
                qCWarning(PIPEWIRERECORD_LOGGING) << "Error encoding a frame: " << av_err2str(ret) << ret;
            }
            break;
        }

        m_packet->stream_index = (*m_avFormatContext->streams)->index;
        av_packet_rescale_ts(m_packet, m_avCodecContext->time_base, (*m_avFormatContext->streams)->time_base);
        log_packet(m_avFormatContext, m_packet);
        ret = av_interleaved_write_frame(m_avFormatContext, m_packet);
        if (ret < 0) {
            qCWarning(PIPEWIRERECORD_LOGGING) << "Error while writing output packet:" << av_err2str(ret);
        }
        av_packet_unref(m_packet);
    }
}

void PipeWireRecordWriteThread::run()
{
    PipeWireRecordWrite writer(m_produce, m_avFormatContext, m_avCodecContext);
    QThread::exec();
    AVPacket *pkt = av_packet_alloc();
    avcodec_send_frame(m_avCodecContext, nullptr);

    for (;;) {
        if (avcodec_receive_packet(m_avCodecContext, pkt) < 0)
            break;

        pkt->stream_index = (*m_avFormatContext->streams)->index;
        av_packet_rescale_ts(pkt, m_avCodecContext->time_base, (*m_avFormatContext->streams)->time_base);
        log_packet(m_avFormatContext, pkt);
        int ret = av_interleaved_write_frame(m_avFormatContext, pkt);
        if (ret < 0) {
            qCWarning(PIPEWIRERECORD_LOGGING) << "Error while writing output packet:" << av_err2str(ret);
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
}
