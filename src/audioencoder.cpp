/*
    SPDX-FileCopyrightText: 2026 Khudoberdi <xudoyberdi0410@gmail.com>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "audioencoder_p.h"

#include <format>
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

#include "logging_record.h"

AudioEncoder::AudioEncoder(PipeWireProduce *produce)
    : QObject(nullptr)
    , m_produce(produce)
{
}

AudioEncoder::~AudioEncoder()
{
    if (m_avFilterGraph) {
        avfilter_graph_free(&m_avFilterGraph);
    }

    if (m_avCodecContext) {
        avcodec_free_context(&m_avCodecContext);
    }
}

bool AudioEncoder::filterFrame(int input, AVFrame *frame)
{
    std::lock_guard guard(m_avFilterGraphMutex);
    if (auto result = av_buffersrc_add_frame(m_inputFilters.at(input), frame); result < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to submit audio frame for filtering:" << av_err2str(result);
        return false;
    }

    return true;
}

void AudioEncoder::endInput(int input)
{
    std::lock_guard guard(m_avFilterGraphMutex);
    if (auto result = av_buffersrc_add_frame(m_inputFilters.at(input), nullptr); result < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to end audio input:" << av_err2str(result);
    }
}

std::pair<int, int> AudioEncoder::encodeFrame(int maximumFrames)
{
    auto frame = av_frame_alloc();
    if (!frame) {
        qFatal("Failed to allocate memory");
    }

    int filtered = 0;
    int queued = 0;

    for (;;) {
        if (queued + 1 >= maximumFrames) {
            // Out of queue budget. Unlike video, where dropping frames is
            // tolerable, discarding audio samples would leave a permanent
            // hole in the track, so leave the remaining frames in the
            // buffersink to be retrieved on the next call.
            break;
        }

        {
            // Frames are pushed into the graph from a different thread and
            // libavfilter does not allow concurrent access to a graph.
            std::lock_guard guard(m_avFilterGraphMutex);
            if (auto result = av_buffersink_get_frame(m_outputFilter, frame); result < 0) {
                if (result != AVERROR_EOF && result != AVERROR(EAGAIN)) {
                    qCWarning(PIPEWIRERECORD_LOGGING) << "Failed receiving filtered audio frame:" << av_err2str(result);
                }
                break;
            }
        }

        filtered++;

        auto ret = -1;
        for (;;) {
            {
                std::lock_guard guard(m_avCodecMutex);
                ret = avcodec_send_frame(m_avCodecContext, frame);
            }
            // The codec accepts at most two frames between receives, so
            // drain it and send the same frame again rather than dropping
            // the frame's samples.
            if (ret != AVERROR(EAGAIN) || receivePacket() == 0) {
                break;
            }
        }
        if (ret < 0) {
            if (ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
                qCWarning(PIPEWIRERECORD_LOGGING) << "Error sending an audio frame for encoding:" << av_err2str(ret);
            }
            break;
        }
        queued++;
        av_frame_unref(frame);
    }

    av_frame_free(&frame);

    return std::make_pair(filtered, queued);
}

int AudioEncoder::receivePacket()
{
    auto packet = av_packet_alloc();
    if (!packet) {
        qFatal("Failed to allocate memory");
    }

    int received = 0;

    for (;;) {
        auto ret = -1;
        {
            std::lock_guard guard(m_avCodecMutex);
            ret = avcodec_receive_packet(m_avCodecContext, packet);
        }
        if (ret < 0) {
            if (ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
                qCWarning(PIPEWIRERECORD_LOGGING) << "Error encoding an audio frame: " << av_err2str(ret);
            }
            av_packet_unref(packet);
            break;
        }

        received++;

        m_produce->processAudioPacket(packet);
        av_packet_unref(packet);
    }

    av_packet_free(&packet);

    return received;
}

void AudioEncoder::finish()
{
    for (;;) {
        auto ret = -1;
        {
            std::lock_guard guard(m_avCodecMutex);
            ret = avcodec_send_frame(m_avCodecContext, nullptr);
        }
        // EAGAIN means a frame is still parked in the codec; drain packets and
        // retry, otherwise the encoder never enters draining mode and its
        // delayed samples are lost.
        if (ret != AVERROR(EAGAIN) || receivePacket() == 0) {
            break;
        }
    }
}

AVCodecContext *AudioEncoder::avCodecContext() const
{
    return m_avCodecContext;
}

void AudioEncoder::setQuality(std::optional<quint8> quality)
{
    m_quality = quality;
    // The audio codecs read the bit rate at avcodec_open2() time only, and
    // once encoding has started the context is in concurrent use by the
    // output thread, so quality changes only apply before initialize().
    std::lock_guard guard(m_avCodecMutex);
    if (m_avCodecContext && !avcodec_is_open(m_avCodecContext)) {
        m_avCodecContext->bit_rate = qualityToBitrate(quality);
    }
}

int64_t AudioEncoder::qualityToBitrate(const std::optional<quint8> &quality) const
{
    if (!quality) {
        return 128000;
    }

    return 32000 + int64_t((quality.value() / 100.0) * (256000 - 32000));
}

bool AudioEncoder::createFilterGraph(int inputCount, AVSampleFormat outputFormat, const AVChannelLayout &layout, int sampleRate)
{
    m_avFilterGraph = avfilter_graph_alloc();
    if (!m_avFilterGraph) {
        qFatal("Failed to allocate memory");
    }

    char layoutDescription[64];
    av_channel_layout_describe(&layout, layoutDescription, sizeof(layoutDescription));

    // Every capture stream feeds the graph through its own abuffer source. They
    // share the negotiated format, so the source arguments only differ by name.
    const auto sourceArguments = std::format("time_base=1/{0}:sample_rate={0}:sample_fmt={1}:channel_layout={2}",
                                             sampleRate,
                                             av_get_sample_fmt_name(AV_SAMPLE_FMT_FLT),
                                             layoutDescription);

    for (int i = 0; i < inputCount; ++i) {
        AVFilterContext *inputFilter = nullptr;
        const auto name = std::format("in{}", i);
        int ret = avfilter_graph_create_filter(&inputFilter, avfilter_get_by_name("abuffer"), name.c_str(), sourceArguments.c_str(), nullptr, m_avFilterGraph);
        if (ret < 0) {
            qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to create the audio buffer filter:" << av_err2str(ret);
            return false;
        }
        m_inputFilters.append(inputFilter);
    }

    int ret = avfilter_graph_create_filter(&m_outputFilter, avfilter_get_by_name("abuffersink"), "out", nullptr, nullptr, m_avFilterGraph);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not create audio buffer output filter:" << av_err2str(ret);
        return false;
    }

    // The last node before the sink converts to the format the encoder expects,
    // regardless of how many inputs there are.
    const auto formatArguments =
        std::format("sample_fmts={}:sample_rates={}:channel_layouts={}", av_get_sample_fmt_name(outputFormat), sampleRate, layoutDescription);
    AVFilterContext *formatFilter = nullptr;
    ret = avfilter_graph_create_filter(&formatFilter, avfilter_get_by_name("aformat"), "format", formatArguments.c_str(), nullptr, m_avFilterGraph);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to create the audio format filter:" << av_err2str(ret);
        return false;
    }

    if (inputCount > 1) {
        // normalize=0 keeps each source at its captured level instead of
        // scaling by 1/inputCount, so the volume does not drop or float as
        // inputs come and go. The trade-off is that the summed signal can
        // exceed +-1.0 and clip when both sources are loud at once; a limiter
        // (e.g. alimiter) after the mix would be the follow-up fix.
        const auto mixArguments = std::format("inputs={}:duration=longest:normalize=0", inputCount);
        AVFilterContext *mixFilter = nullptr;
        ret = avfilter_graph_create_filter(&mixFilter, avfilter_get_by_name("amix"), "mix", mixArguments.c_str(), nullptr, m_avFilterGraph);
        if (ret < 0) {
            qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to create the audio mix filter:" << av_err2str(ret);
            return false;
        }

        for (int i = 0; i < inputCount; ++i) {
            if (ret = avfilter_link(m_inputFilters.at(i), 0, mixFilter, i); ret < 0) {
                qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to link audio input to mixer:" << av_err2str(ret);
                return false;
            }
        }
        if (ret = avfilter_link(mixFilter, 0, formatFilter, 0); ret < 0) {
            qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to link audio mixer to format filter:" << av_err2str(ret);
            return false;
        }
    } else if (ret = avfilter_link(m_inputFilters.at(0), 0, formatFilter, 0); ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to link audio input to format filter:" << av_err2str(ret);
        return false;
    }

    if (ret = avfilter_link(formatFilter, 0, m_outputFilter, 0); ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to link audio format filter to sink:" << av_err2str(ret);
        return false;
    }

    ret = avfilter_graph_config(m_avFilterGraph, nullptr);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed configuring filter graph:" << av_err2str(ret);
        return false;
    }

    // Have the sink emit frames of exactly the size the encoder expects,
    // including a final partial frame at end of stream. This avoids needing
    // a separate FIFO between filtering and encoding.
    if (m_avCodecContext->frame_size > 0) {
        av_buffersink_set_frame_size(m_outputFilter, m_avCodecContext->frame_size);
    }

    return true;
}

#include "moc_audioencoder_p.cpp"
