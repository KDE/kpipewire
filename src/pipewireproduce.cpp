/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "pipewireproduce.h"
#include <QMutex>
#include <QPainter>
#include <QThreadPool>
#include <logging_record.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/timestamp.h>
#include <libswscale/swscale.h>
}

#undef av_err2str
// The one provided by libav fails to compile on GCC due to passing data from the function scope outside
char str[AV_ERROR_MAX_STRING_SIZE];
char *av_err2str(int errnum)
{
    return av_make_error_string(str, AV_ERROR_MAX_STRING_SIZE, errnum);
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

PipeWireProduce::PipeWireProduce(const QByteArray &encoder, uint nodeId, uint fd)
    : QObject()
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
    connect(m_stream.get(), &PipeWireSourceStream::streamParametersChanged, this, &PipeWireProduce::setupStream);
}

PipeWireProduce::~PipeWireProduce()
{
    if (m_avCodecContext) {
        avcodec_close(m_avCodecContext);
        av_free(m_avCodecContext);
    }
}

void PipeWireProduce::setupStream()
{
    qCDebug(PIPEWIRERECORD_LOGGING) << "Setting up stream";
    disconnect(m_stream.get(), &PipeWireSourceStream::streamParametersChanged, this, &PipeWireProduce::setupStream);

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
    m_avCodecContext->bit_rate = size.width() * size.height() * 2;

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

    AVDictionary *options = nullptr;
    av_dict_set_int(&options, "threads", 4, 0);
    av_dict_set(&options, "preset", "ultrafast", 0);
    av_dict_set(&options, "tune-content", "screen", 0);
    av_dict_set(&options, "deadline", "good", 0);

    int ret = avcodec_open2(m_avCodecContext, m_codec, &options);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not open codec" << av_err2str(ret);
        return;
    }

    m_frame.reset(new CustomAVFrame);
    ret = m_frame->alloc(m_avCodecContext->width, m_avCodecContext->height, m_avCodecContext->pix_fmt);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not allocate raw picture buffer" << av_err2str(ret);
        return;
    }

    connect(m_stream.get(), &PipeWireSourceStream::stateChanged, this, &PipeWireProduce::stateChanged);
    if (!setupFormat()) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not set up the producing thread";
        return;
    }

    connect(m_stream.data(), &PipeWireSourceStream::frameReceived, this, &PipeWireProduce::processFrame);
    m_writeThread = new PipeWireReceiveEncodedThread(m_avCodecContext, this);
    QThreadPool::globalInstance()->start(m_writeThread);
}

void PipeWireProduceThread::run()
{
    std::unique_ptr<PipeWireProduce> produce(m_base->createThread());
    m_producer = produce.get();
    if (!produce->m_stream) {
        Q_EMIT errorFound(produce->error());
        return;
    }
    qCDebug(PIPEWIRERECORD_LOGGING) << "executing";
    const int ret = exec();
    qCDebug(PIPEWIRERECORD_LOGGING) << "finishing" << ret;
}

void PipeWireProduceThread::deactivate()
{
    if (m_producer) {
        m_producer->m_deactivated = true;
        m_producer->m_stream->setActive(false);
    }
}

void PipeWireProduce::processFrame(const PipeWireFrame &frame)
{
    if (frame.cursor) {
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
        m_frameWithoutMetadataCursor = *frame.image;
        render(frame);
    }
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

void PipeWireProduce::render(const PipeWireFrame &frame)
{
    Q_ASSERT(!m_frameWithoutMetadataCursor.isNull());

    QImage image(m_frameWithoutMetadataCursor);
    aboutToEncode(image);

    const std::uint8_t *buffers[] = {image.constBits(), nullptr};
    const int strides[] = {static_cast<int>(image.bytesPerLine()), 0, 0, 0};
    struct SwsContext *sws_context = nullptr;
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
    sws_scale(sws_context, buffers, strides, 0, m_avCodecContext->height, m_frame->m_avFrame->data, m_frame->m_avFrame->linesize);

    m_frame->m_avFrame->pts = framePts(frame);

    // Let's add a key frame every 100 frames and also the first frame
    if (frame.sequential && (*frame.sequential == 0 || (*frame.sequential - m_lastKeyFrame) > 100)) {
        m_frame->m_avFrame->key_frame = 1;
        m_lastKeyFrame = *frame.sequential;
    }

    if (m_lastPts > 0 && m_frame->m_avFrame->pts <= m_lastPts) {
        // Make sure we don't have two frames at the same presentation time
        m_frame->m_avFrame->pts = m_lastPts + 1;
    }
    m_lastPts = m_frame->m_avFrame->pts;

    const int ret = avcodec_send_frame(m_avCodecContext, m_frame->m_avFrame);
    // qDebug() << "issued" << m_frame->m_avFrame->pts;
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Error sending a frame for encoding:" << av_err2str(ret);
        return;
    }
    m_bufferNotEmpty.wakeAll();
}

void PipeWireProduce::stateChanged(pw_stream_state state)
{
    if (state != PW_STREAM_STATE_PAUSED || !m_deactivated) {
        return;
    }
    if (!m_stream) {
        qCDebug(PIPEWIRERECORD_LOGGING) << "finished without a stream";
        return;
    }

    disconnect(m_stream.data(), &PipeWireSourceStream::frameReceived, this, &PipeWireProduce::processFrame);

    if (m_writeThread) {
        m_writeThread->m_active = false;
        m_bufferNotEmpty.wakeAll();
        bool done = QThreadPool::globalInstance()->waitForDone(-1);
        Q_ASSERT(done);
    }

    qCDebug(PIPEWIRERECORD_LOGGING) << "finished";
    cleanup();
    QThread::currentThread()->quit();
}

PipeWireReceiveEncodedThread::PipeWireReceiveEncodedThread(AVCodecContext *avCodecContext, PipeWireProduce *producer)
    : QRunnable()
    , m_packet(av_packet_alloc())
    , m_avCodecContext(avCodecContext)
    , m_producer(producer)
{
}

PipeWireReceiveEncodedThread::~PipeWireReceiveEncodedThread()
{
    av_packet_free(&m_packet);
}

void PipeWireReceiveEncodedThread::run()
{
    int ret = 0;
    while (true) {
        ret = avcodec_receive_packet(m_avCodecContext, m_packet);
        if (ret == AVERROR_EOF) {
            break;
        } else if (ret == AVERROR(EAGAIN)) {
            if (m_active) {
                m_producer->m_readyToWrite.lock();
                m_producer->m_bufferNotEmpty.wait(&m_producer->m_readyToWrite);
                m_producer->m_readyToWrite.unlock();
            } else {
                int sent = avcodec_send_frame(m_avCodecContext, nullptr);
                qCDebug(PIPEWIRERECORD_LOGGING) << "draining" << sent;
            }
            continue;
        } else if (ret < 0) {
            qCWarning(PIPEWIRERECORD_LOGGING) << "Error encoding a frame: " << av_err2str(ret);
            continue;
        }

        m_producer->processPacket(m_packet);
    }

    m_producer->processPacket(nullptr);
}
