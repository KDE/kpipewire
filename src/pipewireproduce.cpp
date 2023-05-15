/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "pipewireproduce.h"

#include <QMutex>
#include <QPainter>
#include <QThreadPool>
#include <libavutil/pixfmt.h>
#include <logging_record.h>

#include <QDateTime>
#include <qstringliteral.h>

extern "C" {
#include <fcntl.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/timestamp.h>
#include <libswscale/swscale.h>
}

Q_DECLARE_METATYPE(std::optional<int>);
Q_DECLARE_METATYPE(std::optional<std::chrono::nanoseconds>);

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

// TODO: support VP8 vaapi

static bool encoderSupportsVaapi(PipeWireBaseEncodedStream::Encoder encoder)
{
    switch (encoder) {
    case PipeWireBaseEncodedStream::H264Main:
    case PipeWireBaseEncodedStream::H264Baseline:
        return true;
    default:
        return false;
    }
}

static QByteArray encoderName(PipeWireBaseEncodedStream::Encoder encoder, bool useVaapi)
{
    switch (encoder) {
    case PipeWireBaseEncodedStream::H264Main:
    case PipeWireBaseEncodedStream::H264Baseline:
        return useVaapi ? QByteArray("h264_vaapi") : QByteArray("libx264");
    case PipeWireBaseEncodedStream::VP8:
        return QByteArray("libvpx");
    default:
        return {};
    }
}

static AVPixelFormat pixelFormatForEncoder(const QByteArray &encoder)
{
    if (encoder == QByteArray("h264_vaapi")) {
        return AV_PIX_FMT_VAAPI;
    } else {
        return AV_PIX_FMT_YUV420P;
    }
}

QByteArray fallbackEncoder(const QByteArray &encoder)
{
    if (encoder == QByteArray("h264_vaapi")) {
        return QByteArray("libx264");
    } else {
        return {};
    }
}

PipeWireProduce::PipeWireProduce(PipeWireBaseEncodedStream::Encoder encoder, uint nodeId, uint fd, const std::optional<Fraction> &framerate)
    : QObject()
    , m_nodeId(nodeId)
    , m_encoder(encoder)
{
    qRegisterMetaType<std::optional<int>>();
    qRegisterMetaType<std::optional<std::chrono::nanoseconds>>();

    m_stream.reset(new PipeWireSourceStream(nullptr));
    if (framerate) {
        m_stream->setMaxFramerate(*framerate);
    }
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

Fraction PipeWireProduce::maxFramerate() const
{
    return m_stream->framerate();
}

void PipeWireProduce::setMaxFramerate(const Fraction &framerate)
{
    m_stream->setMaxFramerate(framerate);
}

void PipeWireProduce::initFiltersVaapi()
{
    m_avFilterGraph = avfilter_graph_alloc();
    if (!m_avFilterGraph) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not create filter graph";
        return;
    }

    int ret = avfilter_graph_create_filter(&m_bufferFilter,
                                           avfilter_get_by_name("buffer"),
                                           "in",
                                           "width=1:height=1:pix_fmt=drm_prime:time_base=1/1",
                                           nullptr,
                                           m_avFilterGraph);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to create the buffer filter";
        return;
    }

    auto parameters = av_buffersrc_parameters_alloc();

    parameters->format = AV_PIX_FMT_DRM_PRIME;
    parameters->width = m_stream->size().width();
    parameters->height = m_stream->size().height();
    parameters->time_base = {1, 1000};
    parameters->hw_frames_ctx = m_vaapi.drmFramesContext();

    av_buffersrc_parameters_set(m_bufferFilter, parameters);
    av_free(parameters);
    parameters = nullptr;

    ret = avfilter_graph_create_filter(&m_outputFilter, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, m_avFilterGraph);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not create buffer output filter";
        return;
    }

    auto inputs = avfilter_inout_alloc();
    inputs->name = av_strdup("in");
    inputs->filter_ctx = m_bufferFilter;
    inputs->pad_idx = 0;
    inputs->next = nullptr;

    auto outputs = avfilter_inout_alloc();
    outputs->name = av_strdup("out");
    outputs->filter_ctx = m_outputFilter;
    outputs->pad_idx = 0;
    outputs->next = nullptr;

    ret = avfilter_graph_parse(m_avFilterGraph, "hwmap=mode=direct:derive_device=vaapi,scale_vaapi=format=nv12:mode=fast", outputs, inputs, NULL);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed creating filter graph";
        return;
    }

    for (auto i = 0u; i < m_avFilterGraph->nb_filters; ++i) {
        m_avFilterGraph->filters[i]->hw_device_ctx = av_buffer_ref(m_vaapi.drmContext());
    }

    ret = avfilter_graph_config(m_avFilterGraph, nullptr);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed configuring filter graph";
        return;
    }

    m_avCodecContext->hw_frames_ctx = av_buffer_ref(m_outputFilter->inputs[0]->hw_frames_ctx);
}

void PipeWireProduce::initFiltersSoftware()
{
    const QSize size = m_stream->size();

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
}

void PipeWireProduce::setupStream()
{
    const QSize size = m_stream->size();

    qCDebug(PIPEWIRERECORD_LOGGING) << "Setting up stream";
    disconnect(m_stream.get(), &PipeWireSourceStream::streamParametersChanged, this, &PipeWireProduce::setupStream);

    if (encoderSupportsVaapi(m_encoder)) {
        m_vaapi.init(size);
    }
    m_encoderName = encoderName(m_encoder, m_vaapi.isValid());

    m_codec = avcodec_find_encoder_by_name(m_encoderName.constData());
    if (!m_codec) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Codec not found";
        return;
    }

    m_avCodecContext = avcodec_alloc_context3(m_codec);
    if (!m_avCodecContext) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not allocate video codec context";
        return;
    }
    m_avCodecContext->bit_rate = size.width() * size.height() * 2;

    Q_ASSERT(!size.isEmpty());
    m_avCodecContext->width = size.width();
    m_avCodecContext->height = size.height();
    m_avCodecContext->max_b_frames = 0;
    m_avCodecContext->gop_size = 100;
    m_avCodecContext->pix_fmt = m_vaapi.isValid() ? AV_PIX_FMT_VAAPI : AV_PIX_FMT_YUV420P;
    m_avCodecContext->time_base = AVRational{1, 1000};
    m_avCodecContext->profile = 578;
    m_avCodecContext->global_quality = 35;

    if (m_encoder == PipeWireBaseEncodedStream::H264Main) {
        m_avCodecContext->profile = FF_PROFILE_H264_MAIN;
    } else if (m_encoder == PipeWireBaseEncodedStream::H264Baseline) {
        if (m_vaapi.isValid()) {
            m_avCodecContext->profile = FF_PROFILE_H264_CONSTRAINED_BASELINE;
        } else {
            m_avCodecContext->profile = FF_PROFILE_H264_BASELINE;
        }
    }

    AVDictionary *options = nullptr;
    av_dict_set_int(&options, "threads", qMin(16, QThread::idealThreadCount()), 0);
    av_dict_set(&options, "preset", "veryfast", 0);
    av_dict_set(&options, "tune-content", "screen", 0);
    av_dict_set(&options, "deadline", "realtime", 0);
    // In theory a lower number should be faster, but the opposite seems to be true
    // av_dict_set(&options, "quality", "40", 0);
    // Disable motion estimation, not great while dragging windows but speeds up encoding by an order of magnitude
    av_dict_set(&options, "flags", "+mv4", 0);
    // Disable in-loop filtering
    av_dict_set(&options, "-flags", "+loop", 0);
    av_dict_set(&options, "crf", "45", 0);

    if (m_vaapi.isValid()) {
        initFiltersVaapi();
    } else {
        initFiltersSoftware();
    }

    int ret = avcodec_open2(m_avCodecContext, m_codec, &options);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not open codec" << av_err2str(ret);
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
        // We are doing conversion on drm prime
        if (m_vaapi.isValid()) {
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
            drmFrame->pts = framePts(frame.presentationTimestamp);

            if (auto result = av_buffersrc_add_frame(m_bufferFilter, drmFrame); result < 0) {
                qCDebug(PIPEWIRERECORD_LOGGING) << "Failed sending frame for encoding" << av_err2str(result);
                av_frame_unref(drmFrame);
                return;
            }

            if (!m_processing) {
                Q_EMIT producedFrames();
            }

            av_frame_unref(drmFrame);
        } else {
            // Full software fallback
            QImage downloadBuffer(m_stream->size(), QImage::Format_RGBA8888_Premultiplied);
            if (!m_dmabufHandler.downloadFrame(downloadBuffer, frame)) {
                m_stream->renegotiateModifierFailed(frame.format, frame.dmabuf->modifier);
                return;
            }
            render(downloadBuffer, frame);
            return;

            CustomAVFrame avFrame;
            int ret = avFrame.alloc(m_avCodecContext->width, m_avCodecContext->height, convertQImageFormatToAVPixelFormat(downloadBuffer.format()));

            const std::uint8_t *buffers[] = {downloadBuffer.constBits(), nullptr};
            const int strides[] = {static_cast<int>(downloadBuffer.bytesPerLine()), 0, 0, 0};

            av_image_copy(avFrame.m_avFrame->data,
                          avFrame.m_avFrame->linesize,
                          buffers,
                          strides,
                          convertQImageFormatToAVPixelFormat(downloadBuffer.format()),
                          m_avCodecContext->width,
                          m_avCodecContext->height);

            render(downloadBuffer, frame);

            if (auto result = av_buffersrc_add_frame(m_bufferFilter, avFrame.m_avFrame); result < 0) {
                qCDebug(PIPEWIRERECORD_LOGGING) << "Failed sending frame for encoding" << av_err2str(result);
                av_frame_unref(avFrame.m_avFrame);
                return;
            }

            if (!m_processing) {
                Q_EMIT producedFrames();
            }
        }
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

    if (m_produce->m_vaapi.isValid()) {
        for (;;) {
            auto frame = av_frame_alloc();
            if (auto result = av_buffersink_get_frame(m_produce->m_outputFilter, frame); result < 0) {
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

            for (;;) {
                auto ret = avcodec_receive_packet(m_avCodecContext, m_packet);
                if (ret < 0) {
                    if (ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
                        qCWarning(PIPEWIRERECORD_LOGGING) << "Error encoding a frame: " << av_err2str(ret);
                    }
                    av_packet_unref(m_packet);
                    break;
                }

                m_produce->processPacket(m_packet);
                av_packet_unref(m_packet);
            }
        }

    } else {
        for (;;) {
            auto prFrame = m_produce->dequeueFrame(&remaining);

            if (remaining == 0) {
                continue;
            }

            auto image = prFrame.image;

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
            avFrame.m_avFrame->pts = m_produce->framePts(prFrame.presentationTimestamp);

            if (auto result = av_buffersrc_add_frame(m_produce->m_bufferFilter, avFrame.m_avFrame); result < 0) {
                qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to submit frame for filtering";
                continue;
            }

            if (ret < 0) {
                qCWarning(PIPEWIRERECORD_LOGGING) << "Error sending a frame for encoding:" << av_err2str(ret);
                return;
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
    }

    m_produce->m_processing = false;
}
