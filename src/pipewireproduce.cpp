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
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/timestamp.h>
#include <libswscale/swscale.h>
}

Q_DECLARE_METATYPE(std::optional<int>);
Q_DECLARE_METATYPE(std::optional<std::chrono::nanoseconds>);

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
        av_frame_free(&m_avFrame);
    }

    int alloc(int width, int height, AVPixelFormat pix_fmt)
    {
        m_avFrame->format = pix_fmt;
        m_avFrame->width = width;
        m_avFrame->height = height;
        m_avFrame->format = pix_fmt;
        return av_frame_get_buffer(m_avFrame, 32);
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

PipeWireProduce::PipeWireProduce(const QByteArray &encoder, uint nodeId, uint fd)
    : QObject()
    , m_nodeId(nodeId)
    , m_encoder(encoder)
{
    qRegisterMetaType<std::optional<int>>();
    qRegisterMetaType<std::optional<std::chrono::nanoseconds>>();

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

    m_avFilterGraph = avfilter_graph_alloc();
    if (!m_avFilterGraph) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not create filter graph";
        return;
    }

    auto args = QStringLiteral("width=%1:height=%2:pix_fmt=%3:time_base=1/1000:sar=1").arg(size.width()).arg(size.height()).arg(AV_PIX_FMT_RGBA);
    auto result = avfilter_graph_create_filter(&m_bufferFilter, avfilter_get_by_name("buffer"), "input", args.toUtf8().data(), nullptr, m_avFilterGraph);
    if (result < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not create buffer input filter";
        return;
    }

    result = avfilter_graph_create_filter(&m_formatFilter, avfilter_get_by_name("format"), "format", "pix_fmts=yuv420p", nullptr, m_avFilterGraph);
    if (result < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not create format filter";
        return;
    }

    result = avfilter_graph_create_filter(&m_outputFilter, avfilter_get_by_name("buffersink"), "output", nullptr, nullptr, m_avFilterGraph);
    if (result < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not create buffer output filter";
        return;
    }
    result = avfilter_link(m_bufferFilter, 0, m_formatFilter, 0);
    if (result < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed linking filters";
        return;
    }
    result = avfilter_link(m_formatFilter, 0, m_outputFilter, 0);
    if (result < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed linking filters";
        return;
    }
    result = avfilter_graph_config(m_avFilterGraph, nullptr);
    if (result < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed configuring filter graph";
        return;
    }

    connect(m_stream.get(), &PipeWireSourceStream::stateChanged, this, &PipeWireProduce::stateChanged);
    if (!setupFormat()) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not set up the producing thread";
        return;
    }

    connect(m_stream.data(), &PipeWireSourceStream::frameReceived, this, &PipeWireProduce::processFrame);
    m_writeThread = new PipeWireReceiveEncodedThread(this, m_avCodecContext);
    m_writeThread->start();
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
        QImage downloadBuffer(m_stream->size(), QImage::Format_RGBA8888_Premultiplied);
        if (!m_dmabufHandler.downloadFrame(downloadBuffer, frame)) {
            m_stream->renegotiateModifierFailed(frame.format, frame.dmabuf->modifier);
            return;
        }
        render(downloadBuffer, frame);
    } else if (frame.image) {
        render(*frame.image, frame);
    }
}

void PipeWireProduce::render(const QImage &image, const PipeWireFrame &frame)
{
    PipeWireRecordFrame recordFrame{image, frame.sequential, frame.presentationTimestamp};
    aboutToEncode(recordFrame.image);

    enqueueFrame(recordFrame);
}

void PipeWireProduce::enqueueFrame(const PipeWireRecordFrame &frame)
{
    QMutexLocker lock(&m_framesMutex);
    m_frames.enqueue(frame);
    if (m_frames.count() > 30) {
        qDebug() << "many frames waiting!" << m_frames.count();
    }

    if (!m_processing) {
        lock.unlock();
        Q_EMIT producedFrames();
    }
}

PipeWireRecordFrame PipeWireProduce::dequeueFrame(int *remaining)
{
    QMutexLocker lock(&m_framesMutex);
    *remaining = m_frames.count();
    if (m_frames.isEmpty()) {
        return {};
    }
    return m_frames.dequeue();
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
        m_writeThread->quit();
        bool done = m_writeThread->wait();
        Q_ASSERT(done);
    }

    qCDebug(PIPEWIRERECORD_LOGGING) << "finished";
    cleanup();
    QThread::currentThread()->quit();
}

PipeWireReceiveEncodedThread::PipeWireReceiveEncodedThread(PipeWireProduce *produce, AVCodecContext *avCodecContext)
    : QThread()
    , m_produce(produce)
    , m_avCodecContext(avCodecContext)
{
}

PipeWireReceiveEncoded::PipeWireReceiveEncoded(PipeWireProduce *produce, AVCodecContext *avCodecContext)
    : QObject()
    , m_packet(av_packet_alloc())
    , m_avCodecContext(avCodecContext)
    , m_produce(produce)
{
    connect(produce, &PipeWireProduce::producedFrames, this, &PipeWireReceiveEncoded::addFrame);
}

PipeWireReceiveEncoded::~PipeWireReceiveEncoded()
{
    m_produce->processPacket(nullptr);
    av_packet_free(&m_packet);
}

void PipeWireReceiveEncodedThread::run()
{
    PipeWireReceiveEncoded writer(m_produce, m_avCodecContext);
    QThread::exec();

    AVPacket pkt;
    avcodec_send_frame(m_avCodecContext, nullptr);

    for (;;) {
        if (avcodec_receive_packet(m_avCodecContext, &pkt) < 0)
            break;

        m_produce->processPacket(&pkt);
        av_packet_unref(&pkt);
    }
}

void PipeWireReceiveEncoded::addFrame()
{
    m_produce->m_processing = true;
    int remaining = 1;
    while (remaining > 0) {
        auto frame = m_produce->dequeueFrame(&remaining);
        if (remaining == 0) {
            break;
        }

        auto image = frame.image;

        CustomAVFrame avFrame;
        int ret = avFrame.alloc(m_avCodecContext->width, m_avCodecContext->height, convertQImageFormatToAVPixelFormat(image.format()));

        const std::uint8_t *buffers[] = {image.constBits(), nullptr};
        const int strides[] = {static_cast<int>(image.bytesPerLine()), 0, 0, 0};

        av_image_copy(avFrame.m_avFrame->data,
                      avFrame.m_avFrame->linesize,
                      buffers,
                      strides,
                      convertQImageFormatToAVPixelFormat(image.format()),
                      m_avCodecContext->width,
                      m_avCodecContext->height);

        avFrame.m_avFrame->pts = m_produce->framePts(frame.presentationTimestamp);

        // Let's add a key frame every 100 frames and also the first frame
        if (frame.sequential && (*frame.sequential == 0 || (*frame.sequential - m_lastKeyFrame) > 100)) {
            avFrame.m_avFrame->key_frame = 1;
            m_lastKeyFrame = *frame.sequential;
        }

        if (m_lastPts > 0 && avFrame.m_avFrame->pts <= m_lastPts) {
            // Make sure we don't have two frames at the same presentation time
            avFrame.m_avFrame->pts = m_lastPts + 1;
        }
        m_lastPts = avFrame.m_avFrame->pts;

        if (auto result = av_buffersrc_add_frame(m_produce->m_bufferFilter, avFrame.m_avFrame); result < 0) {
            qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to submit frame for filtering";
            continue;
        }
        for (;;) {
            auto frame = av_frame_alloc();
            if (auto result = av_buffersink_get_frame(m_produce->m_outputFilter, frame); result < 0) {
                if (ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
                    qCWarning(PIPEWIRERECORD_LOGGING) << "Failed receiving filtered frame:" << av_err2str(result);
                }
                break;
            }

            ret = avcodec_send_frame(m_avCodecContext, frame);
            if (ret < 0) {
                qCWarning(PIPEWIRERECORD_LOGGING) << "Error sending a frame for encoding:" << av_err2str(ret);
                return;
            }
            av_frame_unref(frame);
            for (;;) {
                ret = avcodec_receive_packet(m_avCodecContext, m_packet);
                if (ret < 0) {
                    if (ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
                        qCWarning(PIPEWIRERECORD_LOGGING) << "Error encoding a frame: " << av_err2str(ret);
                    }
                    break;
                }

                m_produce->processPacket(m_packet);
                av_packet_unref(m_packet);
            }
        }
    }
    m_produce->m_processing = false;
}
