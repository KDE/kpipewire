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

void PipeWireProduce::initFilters()
{
    char args[512];
    m_filterGraph = avfilter_graph_alloc();

    enum AVPixelFormat out_format_list[] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};

    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=1/1:pixel_aspect=1/1", 1920, 1080, AV_PIX_FMT_RGBA);
    int ret = avfilter_graph_create_filter(&m_bufferSrc, avfilter_get_by_name("buffer"), "in", args, NULL, m_filterGraph);
    if (ret < 0) {
        fprintf(stderr, "Error creating buffer source filter\n");
        return;
    }

    // Initialize sink filter
    ret = avfilter_graph_create_filter(&m_bufferSink, avfilter_get_by_name("buffersink"), "out", NULL, NULL, m_filterGraph);
    if (ret < 0) {
        fprintf(stderr, "Error creating buffer sink filter\n");
        return;
    }

    // Set filter parameters
    ret = av_opt_set_int_list(m_bufferSink, "pix_fmts", out_format_list, -1, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        fprintf(stderr, "Error setting output pixel formats\n");
        return;
    }

    // Link filters
    ret = avfilter_link(m_bufferSrc, 0, m_bufferSink, 0);
    if (ret < 0) {
        fprintf(stderr, "Error linking filters\n");
        return;
    }

    // Configure filter graph
    ret = avfilter_graph_config(m_filterGraph, NULL);
    if (ret < 0) {
        fprintf(stderr, "Error configuring filter graph\n");
        return;
    }
}

void PipeWireProduce::filterFrame(AVFrame *inFrame, AVFrame *outFrame)
{
    assert(av_buffersrc_add_frame_flags(m_bufferSrc, inFrame, AV_BUFFERSRC_FLAG_KEEP_REF) == 0);
    assert(av_buffersink_get_frame(m_bufferSink, outFrame) == 0);
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
    av_dict_set(&options, "profile", "baseline", 0);

    initFilters();

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

void PipeWireProduce::render(const PipeWireFrame &frame)
{
    Q_ASSERT(!m_frameWithoutMetadataCursor.isNull());

    QScopedPointer<CustomAVFrame> avFrame;
    avFrame.reset(new CustomAVFrame);
    int ret = avFrame->alloc(m_avCodecContext->width, m_avCodecContext->height, m_avCodecContext->pix_fmt);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not allocate raw picture buffer" << av_err2str(ret);
        return;
    }

    QImage image(m_frameWithoutMetadataCursor);
    aboutToEncode(image);

    Q_EMIT producedFrame(image, frame.sequential, frame.presentationTimestamp);
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
    connect(produce, &PipeWireProduce::producedFrame, this, &PipeWireReceiveEncoded::addFrame);
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

void PipeWireReceiveEncoded::addFrame(const QImage &image, std::optional<int> sequential, std::optional<std::chrono::nanoseconds> presentationTimestamp)
{
    /*if (!sws_context || m_lastReceivedSize != image.size()) {
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
    }*/
    m_lastReceivedSize = image.size();

    CustomAVFrame avFrame;
    int ret = avFrame.alloc(m_avCodecContext->width, m_avCodecContext->height, m_avCodecContext->pix_fmt);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not allocate raw picture buffer" << av_err2str(ret);
        return;
    }
    CustomAVFrame avFrameToFilter;
    ret = avFrameToFilter.alloc(m_avCodecContext->width, m_avCodecContext->height, convertQImageFormatToAVPixelFormat(image.format()));
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not allocate raw picture buffer" << av_err2str(ret);
        return;
    }
    const std::uint8_t *buffers[] = {image.constBits(), nullptr};
    int strides[] = {static_cast<int>(image.bytesPerLine()), 0, 0, 0};
    // sws_scale(sws_context, buffers, strides, 0, m_avCodecContext->height, avFrame.m_avFrame->data, avFrame.m_avFrame->linesize);

    // Fill AVFrame with input buffer
    /*  ret = av_image_fill_arrays(avFrameToFilter.m_avFrame->data,
                                 avFrameToFilter.m_avFrame->linesize,
                                 *buffers,
                                 convertQImageFormatToAVPixelFormat(image.format()),
                                 m_avCodecContext->width,
                                 m_avCodecContext->height,
                                 1);*/

    av_image_copy(avFrameToFilter.m_avFrame->data,
                  avFrameToFilter.m_avFrame->linesize,
                  buffers,
                  strides,
                  convertQImageFormatToAVPixelFormat(image.format()),
                  m_avCodecContext->width,
                  m_avCodecContext->height);

    if (ret < 0) {
        fprintf(stderr, "Error filling input frame\n");
        return;
    }

    // Send input frame to filter graph
    ret = av_buffersrc_add_frame_flags(m_produce->m_bufferSrc, avFrameToFilter.m_avFrame, AV_BUFFERSRC_FLAG_KEEP_REF);

    if (ret < 0) {
        fprintf(stderr, "Error sending frame to buffer source\n");
        return;
    }

    ret = av_buffersink_get_frame(m_produce->m_bufferSink, avFrame.m_avFrame);
    if (ret < 0) {
        fprintf(stderr, "Error getting filtered frame\n");
        return;
    }

    // Print output frame information
    printf("Output frame: width=%d height=%d format=%d\n", avFrame.m_avFrame->width, avFrame.m_avFrame->height, avFrame.m_avFrame->format);

    avFrame.m_avFrame->pts = m_produce->framePts(presentationTimestamp);

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
                qCWarning(PIPEWIRERECORD_LOGGING) << "Error encoding a frame: " << av_err2str(ret);
            }
            break;
        }

        m_produce->processPacket(m_packet);
        av_packet_unref(m_packet);
    }
}
