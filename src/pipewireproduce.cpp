/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "pipewireproduce_p.h"

#include <QMutex>
#include <QPainter>
#include <QThreadPool>
#include <logging_record.h>

#include <QDateTime>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <qstringliteral.h>

#include "audioconstants_p.h"
#include "audioencoder_p.h"
#include "gifencoder_p.h"
#include "h264vaapiencoder_p.h"
#include "libopenh264encoder_p.h"
#include "libvpxencoder_p.h"
#include "libvpxvp9encoder_p.h"
#include "libwebpencoder_p.h"
#include "libx264encoder_p.h"
#include "pipewireaudiosourcestream_p.h"

#include "logging_frame_statistics.h"
#if defined(Q_OS_OPENBSD)
#include <pthread.h>
#include <pthread_np.h>
#endif

extern "C" {
#include <fcntl.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

Q_DECLARE_METATYPE(std::optional<int>);
Q_DECLARE_METATYPE(std::optional<std::chrono::nanoseconds>);

namespace
{
// Audio is tracked in sample counts while the rest of the recording timeline
// uses the steady clock. These translate a span of the timeline into a number
// of samples at a given rate and back, through duration arithmetic rather than
// manual nanosecond factors.
int64_t durationToSamples(std::chrono::nanoseconds duration, quint32 rate)
{
    return std::llround(std::chrono::duration<double>(duration).count() * rate);
}

std::chrono::nanoseconds samplesToDuration(int64_t samples, quint32 rate)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(double(samples) / rate));
}

// The audio frames carry their capture instant as a duration since the steady
// clock's epoch (CLOCK_MONOTONIC); turn it into a time point on that same clock
// so it can be compared against recordEpoch() and the stop time.
std::chrono::steady_clock::time_point timePoint(std::chrono::nanoseconds sinceEpoch)
{
    return std::chrono::steady_clock::time_point(sinceEpoch);
}
}

PipeWireProduce::PipeWireProduce(PipeWireBaseEncodedStream::Encoder encoderType, uint nodeId, quint64 objectSerial, uint fd, const Fraction &framerate)
    : QObject()
    , m_nodeId(nodeId)
    , m_objectSerial(objectSerial)
    , m_encoderType(encoderType)
    , m_fd(fd)
    , m_frameRate(framerate)
{
    qRegisterMetaType<std::optional<int>>();
    qRegisterMetaType<std::optional<std::chrono::nanoseconds>>();
}

PipeWireProduce::~PipeWireProduce()
{
}

void PipeWireProduce::initialize()
{
    m_stream.reset(new PipeWireSourceStream(nullptr));
    m_stream->setMaxFramerate(m_frameRate);
    m_stream->setRequestedSize(m_requestedSize);

    // The check in supportsHardwareEncoding() is insufficient to fully
    // determine if we actually support hardware encoding the current stream,
    // but to determine that we need the stream size, which we don't get until
    // after we've created the stream, but creating the stream sets important
    // parameters that require the correct usage hint to be set. So use the
    // insufficient check to set the hint, assuming that we still get a working
    // stream when we use the wrong hint with software encoding.
    m_stream->setUsageHint(Encoder::supportsHardwareEncoding() ? PipeWireSourceStream::UsageHint::EncodeHardware
                                                               : PipeWireSourceStream::UsageHint::EncodeSoftware);
    bool created = false;
    if (m_objectSerial != quint64(-1)) {
        created = m_stream->createStream(m_objectSerial, m_fd);
    } else {
        created = m_stream->createStream(m_nodeId, m_fd);
    }
    if (!created || !m_stream->error().isEmpty()) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "failed to set up stream for" << m_nodeId << m_stream->error();
        m_error = m_stream->error();
        m_stream.reset(nullptr);
        return;
    }
    connect(m_stream.get(), &PipeWireSourceStream::streamParametersChanged, this, &PipeWireProduce::handleStreamParametersChanged);

    if (PIPEWIRERECORDFRAMESTATS_LOGGING().isDebugEnabled()) {
        m_frameStatisticsTimer = std::make_unique<QTimer>();
        m_frameStatisticsTimer->setInterval(std::chrono::seconds(1));
        connect(m_frameStatisticsTimer.get(), &QTimer::timeout, this, [this]() {
            qCDebug(PIPEWIRERECORDFRAMESTATS_LOGGING) << "Processed" << m_processedFrames << "frames in the last second.";
            qCDebug(PIPEWIRERECORDFRAMESTATS_LOGGING) << m_pendingFilterFrames << "frames pending for filter.";
            qCDebug(PIPEWIRERECORDFRAMESTATS_LOGGING) << m_pendingEncodeFrames << "frames pending for encode.";
            m_processedFrames = 0;
        });
    }

    /**
     * Kwin only sends a new frame when there's damage on screen
     * The encoder does not flush all frames whilst a stream is active
     * it will keep one frame in the queue waiting for more input until the stream is closed
     *
     * If there's no update this timer bumps the last frame through the stack again
     * to flush the last frame.
     */
    m_frameRepeatTimer.reset(new QTimer);
    m_frameRepeatTimer->setSingleShot(true);
    m_frameRepeatTimer->setInterval(100);
    connect(m_frameRepeatTimer.data(), &QTimer::timeout, this, [this]() {
        if (!m_encoder) {
            return;
        }
        auto f = m_lastFrame;
        m_lastFrame = {};
        aboutToEncode(f);
        if (!m_encoder->filterFrame(f)) {
            return;
        }

        m_pendingFilterFrames++;
        m_passthroughCondition.notify_all();
    });
}

Fraction PipeWireProduce::maxFramerate() const
{
    return m_maxFramerate;
}

void PipeWireProduce::setMaxFramerate(const Fraction &framerate)
{
    m_maxFramerate = framerate;

    const double framesPerSecond = static_cast<double>(framerate.numerator) / framerate.denominator;
    if (m_frameRepeatTimer) {
        m_frameRepeatTimer->setInterval((1000 / framesPerSecond) * 2);
    }
    if (m_stream) {
        m_stream->setMaxFramerate(framerate);
    }
}

int PipeWireProduce::maxPendingFrames() const
{
    return m_maxPendingFrames;
}

void PipeWireProduce::setMaxPendingFrames(int newMaxBufferSize)
{
    if (newMaxBufferSize < 3) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Maxmimum pending frame count of " << newMaxBufferSize << " requested. Value must be 3 or higher.";
        newMaxBufferSize = 3;
    }
    m_maxPendingFrames = newMaxBufferSize;
}

void PipeWireProduce::handleStreamParametersChanged()
{
    if (!m_encoder) {
        // First parameter negotiation: perform the full stream setup.
        setupStream();
        return;
    }

    const auto size = m_stream->size();
    if (supportsResize() && size.isValid() && !size.isEmpty() && size != m_encoderSize) {
        // The source was resized while streaming; rebuild the encoder so the
        // filter graph and hardware frames context match the new size.
        reconfigureStream();
    }
}

QSize PipeWireProduce::requestedSize() const
{
    return m_requestedSize;
}

void PipeWireProduce::setRequestedSize(const QSize &size)
{
    if (m_requestedSize == size) {
        return;
    }
    m_requestedSize = size;

    if (m_stream) {
        m_stream->setRequestedSize(size);
    }
}

void PipeWireProduce::setupStream()
{
    qCDebug(PIPEWIRERECORD_LOGGING) << "Setting up stream";

    m_encoder = makeEncoder();
    if (!m_encoder) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "No encoder could be created";
        if (!m_encodingErrorEmitted.exchange(true)) {
            Q_EMIT encodingError(QStringLiteral("No encoder could be created"));
        }
        return;
    }
    m_encoderSize = m_stream->size();

    connect(m_stream.get(), &PipeWireSourceStream::stateChanged, this, &PipeWireProduce::stateChanged);
    if (!setupFormat()) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not set up the producing thread";
        if (!m_encodingErrorEmitted.exchange(true)) {
            Q_EMIT encodingError(QStringLiteral("Could not set up the encoder output format"));
        }
        return;
    }

    if (m_audioEncoder) {
        initializeAudioStreams();
    }

    connect(m_stream.data(), &PipeWireSourceStream::frameReceived, this, &PipeWireProduce::processFrame);

    startThreads();

    if (m_frameStatisticsTimer) {
        m_frameStatisticsTimer->start();
    }
    Q_EMIT started();
}

void PipeWireProduce::reconfigureStream()
{
    const auto newSize = m_stream->size();
    qCDebug(PIPEWIRERECORD_LOGGING) << "Source size changed from" << m_encoderSize << "to" << newSize << "- rebuilding encoder";

    // Stop the worker threads so nothing touches the encoder while we swap it.
    stopThreads();

    // Drop every bit of frame state tied to the old encoder before swapping, so
    // nothing of the previous size can reach the new one.
    discardFrameState();

    m_encoder = makeEncoder();
    if (!m_encoder) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to recreate encoder after size change";
        return;
    }
    m_encoderSize = newSize;

    // Note: setupFormat() is deliberately not called here. Only the encoder is
    // rebuilt; the output format is left untouched. This is gated to consumers
    // that opt in via supportsResize() and do not write a fixed container.
    startThreads();
}

void PipeWireProduce::discardFrameState()
{
    // The repeat timer may be armed with m_lastFrame holding a frame of the
    // previous size. If it fired after an encoder swap it would push that stale
    // frame into the new encoder, allocating a hardware surface of the wrong
    // size — the exact failure the mid-stream rebuild exists to avoid. Stop it
    // and drop the frame, along with the queue counters for the old encoder.
    if (m_frameRepeatTimer) {
        m_frameRepeatTimer->stop();
    }
    m_lastFrame = {};
    m_pendingFilterFrames = 0;
    m_pendingEncodeFrames = 0;
}

void PipeWireProduce::startThreads()
{
    m_passthroughThread = std::thread([this]() {
        m_passthroughRunning = true;
        while (m_passthroughRunning) {
            std::unique_lock<std::mutex> lock(m_passthroughMutex);
            m_passthroughCondition.wait(lock);

            if (!m_passthroughRunning) {
                break;
            }

            auto [filtered, queued] = m_encoder->encodeFrame(m_maxPendingFrames - m_pendingEncodeFrames);
            m_pendingFilterFrames -= filtered;
            m_pendingEncodeFrames += queued;

            m_outputCondition.notify_all();
        }
    });
#if defined(Q_OS_OPENBSD)
    pthread_set_name_np(m_passthroughThread.native_handle(), "PipeWireProduce::passthrough");
#else
    pthread_setname_np(m_passthroughThread.native_handle(), "PipeWireProduce::passthrough");
#endif

    m_outputThread = std::thread([this]() {
        m_outputRunning = true;
        while (m_outputRunning) {
            std::unique_lock<std::mutex> lock(m_outputMutex);
            m_outputCondition.wait(lock);

            if (!m_outputRunning) {
                break;
            }

            auto received = m_encoder->receivePacket();
            m_pendingEncodeFrames -= received;
            m_processedFrames += received;
            if (received > 0) {
                m_anyFrameEncoded = true;
            }

            if (m_audioEncoder) {
                m_audioEncoder->encodeFrame(std::numeric_limits<int>::max());
                m_audioEncoder->receivePacket();
            }

            // Notify the produce thread that the count of processed frames has
            // changed and it can do cleanup if needed, making sure that that
            // handling is done on the right thread.
            QMetaObject::invokeMethod(this, &PipeWireProduce::handleEncodedFramesChanged, Qt::QueuedConnection);
        }
    });
#if defined(Q_OS_OPENBSD)
    pthread_set_name_np(m_outputThread.native_handle(), "PipeWireProduce::output");
#else
    pthread_setname_np(m_outputThread.native_handle(), "PipeWireProduce::output");
#endif
}

void PipeWireProduce::stopThreads()
{
    if (m_passthroughThread.joinable()) {
        m_passthroughRunning = false;
        m_passthroughCondition.notify_all();
        m_passthroughThread.join();
    }

    if (m_outputThread.joinable()) {
        m_outputRunning = false;
        m_outputCondition.notify_all();
        m_outputThread.join();
    }
}

void PipeWireProduce::initializeAudioStreams()
{
    std::vector<PipeWireAudioSourceStream::Source> sources;
    if (m_audioSources.testFlag(AudioSource::SystemAudio)) {
        sources.push_back(PipeWireAudioSourceStream::Source::DefaultOutputMonitor);
    }
    if (m_audioSources.testFlag(AudioSource::Microphone)) {
        sources.push_back(PipeWireAudioSourceStream::Source::DefaultInput);
    }

    m_audioInputStates.resize(sources.size());

    for (size_t i = 0; i < sources.size(); ++i) {
        const int input = int(i);
        auto stream = std::make_unique<PipeWireAudioSourceStream>(sources.at(i));
        connect(
            stream.get(),
            &PipeWireAudioSourceStream::framesReceived,
            this,
            [this, input](const PipeWireAudioFrame &frame) {
                processAudioFrame(input, frame);
            },
            Qt::DirectConnection);
        // A stream can also die mid-recording, e.g. when the device is
        // unplugged, in which case its input has to be ended as well.
        connect(stream.get(), &PipeWireAudioSourceStream::stopStreaming, this, [this, input]() {
            handleAudioStreamStopped(input);
        });
        connect(stream.get(), &PipeWireAudioSourceStream::stateChanged, this, [this, input](pw_stream_state newState, pw_stream_state) {
            if (newState == PW_STREAM_STATE_ERROR) {
                handleAudioStreamStopped(input);
            }
        });
        if (!stream->createStream() || !stream->error().isEmpty()) {
            qCWarning(PIPEWIRERECORD_LOGGING) << "failed to set up audio stream, continuing without it" << stream->error();
            handleAudioStreamStopped(input);
            continue;
        }
        m_audioStreams.push_back(std::move(stream));
    }

    // An input whose source goes quiet without dying (e.g. the monitor of a
    // suspended sink) would starve amix, which buffers the other input
    // unboundedly and emits nothing until the quiet input ends. Periodically
    // top up clearly stalled inputs with silence; the one second margin is
    // generous enough to never race a live source's capture latency.
    m_audioPadTimer = std::make_unique<QTimer>();
    m_audioPadTimer->setInterval(std::chrono::milliseconds(500));
    connect(m_audioPadTimer.get(), &QTimer::timeout, this, [this] {
        const auto target = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        for (size_t i = 0; i < m_audioInputStates.size(); ++i) {
            padAudioInputToTarget(int(i), target);
        }
    });
    m_audioPadTimer->start();
}

void PipeWireProduce::handleAudioStreamStopped(int input)
{
    auto &state = m_audioInputStates[input];
    if (state.ended) {
        return;
    }
    // Pad the track up to the moment the stream died so it does not simply
    // truncate at the last delivered buffer.
    padAudioInputToTarget(input, std::chrono::steady_clock::now());
    state.ended = true;
    // Close the encoder input so amix does not wait for data on it
    m_audioEncoder->endInput(input);
    m_outputCondition.notify_all();
}

std::chrono::steady_clock::time_point PipeWireProduce::recordEpoch()
{
    // The epoch is taken from the monotonic clock when the first media is
    // processed rather than from that media's own timestamp: the first video
    // frame of a screencast stream carries the render time of the last
    // screen update, which can be arbitrarily far in the past for a static
    // screen. All media is delivered on the produce thread: the audio
    // streams' PipeWire loop is driven by this thread's event loop via the
    // per-thread PipeWireCore, and video frames are processed here as well.
    // That single-threaded delivery is also what makes the unsynchronized
    // m_audioInputStates accesses safe.
    if (!m_recordEpoch) {
        m_recordEpoch = std::chrono::steady_clock::now();
    }
    return *m_recordEpoch;
}

void PipeWireProduce::processAudioFrame(int input, const PipeWireAudioFrame &frame)
{
    auto &state = m_audioInputStates[input];
    if (state.ended) {
        return;
    }

    const auto epoch = recordEpoch();

    if (!state.anchored) {
        // Align the start of this source with the start of the recording.
        const auto offset = timePoint(frame.presentationTimestamp) - epoch;
        const auto anchorSamples = durationToSamples(offset, frame.rate);
        if (anchorSamples > 0) {
            pushSilence(input, anchorSamples, frame.channels, frame.rate);
        } else if (anchorSamples < 0) {
            // The buffer was captured before the recording started, drop the
            // pre-epoch samples so the source starts exactly at the epoch
            // instead of playing late by the capture latency.
            state.trimSamples = -anchorSamples;
        }
        state.anchored = true;
    } else {
        // If the source stops producing buffers, e.g. a monitor of a suspended
        // sink, fill the hole with silence to keep the stream contiguous. Only
        // underruns are corrected here; drift between the PipeWire graph clock
        // and CLOCK_MONOTONIC (audio running slightly ahead) is not compensated.
        const auto expected = epoch + samplesToDuration(state.sampleCount, frame.rate);
        const auto gap = timePoint(frame.presentationTimestamp) - expected;
        if (gap > std::chrono::milliseconds(100)) {
            pushSilence(input, durationToSamples(gap, frame.rate), frame.channels, frame.rate);
        }
    }

    const float *data = frame.data;
    int sampleCount = int(frame.sampleCount);
    if (state.trimSamples > 0) {
        const auto trim = std::min<int64_t>(state.trimSamples, sampleCount);
        state.trimSamples -= trim;
        data += trim * frame.channels;
        sampleCount -= int(trim);
        if (sampleCount == 0) {
            return;
        }
    }

    auto avFrame = av_frame_alloc();
    if (!avFrame) {
        qFatal("Failed to allocate memory");
    }
    avFrame->format = AV_SAMPLE_FMT_FLT;
    avFrame->sample_rate = frame.rate;
    avFrame->nb_samples = sampleCount;
    av_channel_layout_default(&avFrame->ch_layout, frame.channels);
    avFrame->pts = state.sampleCount;
    if (av_frame_get_buffer(avFrame, 0) < 0) {
        av_frame_free(&avFrame);
        return;
    }
    // The frame data is only valid for the duration of the signal emission
    std::memcpy(avFrame->data[0], data, sampleCount * frame.channels * sizeof(float));

    if (m_audioEncoder->filterFrame(input, avFrame)) {
        state.sampleCount += sampleCount;
    }
    av_frame_free(&avFrame);

    m_outputCondition.notify_all();
}

void PipeWireProduce::pushSilence(int input, int64_t sampleCount, quint32 channels, quint32 rate)
{
    auto &state = m_audioInputStates[input];

    // Push the silence in chunks of at most one second, the gap can be
    // arbitrarily long and a single frame of that size would mean a huge
    // allocation and overflowing AVFrame's int sample count.
    while (sampleCount > 0) {
        const int chunk = int(std::min<int64_t>(sampleCount, rate));

        auto avFrame = av_frame_alloc();
        if (!avFrame) {
            qFatal("Failed to allocate memory");
        }
        avFrame->format = AV_SAMPLE_FMT_FLT;
        avFrame->sample_rate = rate;
        avFrame->nb_samples = chunk;
        av_channel_layout_default(&avFrame->ch_layout, channels);
        avFrame->pts = state.sampleCount;
        if (av_frame_get_buffer(avFrame, 0) < 0) {
            av_frame_free(&avFrame);
            return;
        }
        av_samples_set_silence(avFrame->data, 0, chunk, channels, AV_SAMPLE_FMT_FLT);

        if (!m_audioEncoder->filterFrame(input, avFrame)) {
            av_frame_free(&avFrame);
            return;
        }
        state.sampleCount += chunk;
        sampleCount -= chunk;
        av_frame_free(&avFrame);
    }
}

void PipeWireProduce::padAudioInputToTarget(int input, std::chrono::steady_clock::time_point target)
{
    auto &state = m_audioInputStates[input];
    if (state.ended) {
        return;
    }
    if (!m_recordEpoch) {
        // No media was ever processed, there is no timeline to pad against.
        return;
    }
    // The format is fixed in PipeWireAudioSourceStream::createStream(), so
    // these hold even for an input that never delivered a buffer.
    constexpr quint32 channels = AudioChannels;
    constexpr quint32 rate = AudioSampleRate;
    const auto deficit = durationToSamples(target - recordEpoch(), rate) - state.sampleCount;
    if (deficit <= 0) {
        return;
    }
    state.anchored = true;
    pushSilence(input, deficit, channels, rate);
    m_outputCondition.notify_all();
}

void PipeWireProduce::deactivate()
{
    m_deactivated = true;

    // Remember when the recording stopped, on the same clock as recordEpoch(),
    // so the final audio padding in destroy() targets this moment rather than
    // the much later instant the queued video frames finish flushing. Stop the
    // pad timer here as well so it does not keep topping up silence in the
    // meantime, which would also push the audio past the stop time.
    m_stopTimepoint = std::chrono::steady_clock::now();
    if (m_audioPadTimer) {
        m_audioPadTimer->stop();
    }

    auto streamState = PW_STREAM_STATE_PAUSED;
    if (m_stream) {
        streamState = m_stream->state();
        m_stream->setActive(false);
    }

    for (auto &audioStream : m_audioStreams) {
        disconnect(audioStream.get(), &PipeWireAudioSourceStream::framesReceived, this, nullptr);
        audioStream->setActive(false);
    }

    // If we have not been initialized properly before, ensure we still run any
    // cleanup code and exit the thread, otherwise we risk applications not closing
    // properly.
    if (!m_encoder || streamState != PW_STREAM_STATE_STREAMING) {
        QMetaObject::invokeMethod(this, &PipeWireProduce::destroy, Qt::QueuedConnection);
    }
}

void PipeWireProduce::setStreamActive(bool active)
{
    if (!m_stream) {
        return;
    }

    if (!active && m_frameRepeatTimer) {
        // Stop repeating the last frame so the encoder fully idles while paused.
        m_frameRepeatTimer->stop();
    }

    m_stream->setActive(active);
}

void PipeWireProduce::destroy()
{
    // Ensure we cleanup the PipeWireSourceStream while in the same thread we
    // created it in.
    Q_ASSERT_X(QThread::currentThread() == thread(), "PipeWireProduce", "destroy() called from a different thread than PipeWireProduce's thread");

    if (!m_stream) {
        return;
    }

    m_frameRepeatTimer->stop();

    m_frameStatisticsTimer = nullptr;

    stopThreads();

    if (m_audioEncoder) {
        if (m_audioPadTimer) {
            m_audioPadTimer->stop();
        }
        // The worker threads are joined and the muxer is still open until
        // cleanup() below, so drain the audio pipeline single-threaded here.
        // Pad to the moment the recording was stopped (captured in
        // deactivate()) rather than to now(): reaching this point can take a
        // while as the queued video frames are flushed, and padding to now()
        // would leave a tail of silence past the end of the video. Fall back to
        // the current time if deactivate() never ran.
        const auto stopTime = m_stopTimepoint.value_or(std::chrono::steady_clock::now());
        for (size_t i = 0; i < m_audioInputStates.size(); ++i) {
            if (!m_audioInputStates.at(i).ended) {
                // Pad the track up to the stop time so it does not truncate
                // at the last delivered buffer, keeping it as long as the
                // video track even when the source went quiet before stop.
                padAudioInputToTarget(int(i), stopTime);
                m_audioEncoder->endInput(int(i));
                m_audioInputStates[i].ended = true;
            }
        }
        // Alternate between encoding and receiving until the filter graph is
        // empty, then flush the codec's delayed samples.
        for (;;) {
            auto [filtered, queued] = m_audioEncoder->encodeFrame(std::numeric_limits<int>::max());
            auto received = m_audioEncoder->receivePacket();
            if (filtered == 0 && queued == 0 && received == 0) {
                break;
            }
        }
        m_audioEncoder->finish();
        while (m_audioEncoder->receivePacket() > 0) { }
    }
    m_audioStreams.clear();

    m_stream.reset();

    qCDebug(PIPEWIRERECORD_LOGGING) << "finished";
    cleanup();
    Q_EMIT finished();
    QThread::currentThread()->quit();
}

void PipeWireProduce::setQuality(const std::optional<quint8> &quality)
{
    m_quality = quality;
    if (m_encoder) {
        m_encoder->setQuality(quality);
    }
    if (m_audioEncoder) {
        m_audioEncoder->setQuality(quality);
    }
}

void PipeWireProduce::setEncodingPreference(const PipeWireBaseEncodedStream::EncodingPreference &encodingPreference)
{
    m_encodingPreference = encodingPreference;

    if (m_encoder) {
        m_encoder->setEncodingPreference(encodingPreference);
    }
}

void PipeWireProduce::setColorRange(PipeWireBaseEncodedStream::ColorRange colorRange)
{
    m_colorRange = colorRange;
    if (m_encoder) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Changing color range after encoding has started is not supported";
    }
}

void PipeWireProduce::processFrame(const PipeWireFrame &frame)
{
    if (!m_encoder) {
        return;
    }

    auto f = frame;

    m_lastFrame = frame;
    if (m_enableFrameRepeat) {
        m_frameRepeatTimer->start();
    }

    if (frame.cursor) {
        m_cursor.position = frame.cursor->position;
        m_cursor.hotspot = frame.cursor->hotspot;
        if (!frame.cursor->texture.isNull()) {
            m_cursor.dirty = true;
            m_cursor.texture = frame.cursor->texture;
        }
    }

    auto pts = framePts(frame.presentationTimestamp);
    // Always accept the first frame: it carries the initial screen content
    // and, for a static screen, may be the only frame ever delivered.
    if (m_previousPts >= 0) {
        if (pts <= m_previousPts) {
            return;
        }

        auto frameTime = 1000.0 / (m_maxFramerate.numerator / m_maxFramerate.denominator);
        if ((pts - m_previousPts) < frameTime) {
            return;
        }
    }

    if (m_pendingFilterFrames + 1 > m_maxPendingFrames) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Filter queue is full, dropping frame" << pts;
        // Frames have backed up to the limit without the encoder ever producing a
        // single packet: it is not draining (e.g. a hardware encoder that cannot map
        // its frames). Report it so consumers can fall back instead of showing nothing.
        if (!m_anyFrameEncoded && !m_encodingErrorEmitted.exchange(true)) {
            Q_EMIT encodingError(QStringLiteral("Encoder produced no output; the filter queue saturated"));
        }
        return;
    }

    aboutToEncode(f);
    if (!m_encoder->filterFrame(f)) {
        return;
    }

    m_pendingFilterFrames++;
    m_previousPts = pts;

    m_passthroughCondition.notify_all();
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

    if (m_pendingFilterFrames <= 0 && m_pendingEncodeFrames <= 0) {
        // If we have nothing pending, cleanup immediately.
        m_encoder->finish();

        // We want to clean up the source stream while in the input thread, but we
        // need to do so while not handling any PipeWire callback as that risks
        // crashing because we're stil executing PipeWire handling code.
        QMetaObject::invokeMethod(this, &PipeWireProduce::destroy, Qt::QueuedConnection);
    } else {
        // If we have pending frames, wait with cleanup until all frames have been processed.
        qCDebug(PIPEWIRERECORD_LOGGING) << "Waiting for frame queues to empty, still pending filter" << m_pendingFilterFrames << "encode"
                                        << m_pendingEncodeFrames;
        m_passthroughCondition.notify_all();
    }
}

void PipeWireProduce::handleEncodedFramesChanged()
{
    if (!m_deactivated) {
        return;
    }

    // If we're deactivating but still have frames in the queue, we want to
    // flush everything. Since at that point we are not receiving new frames, we
    // need a different trigger to make the filtering thread process frames.
    // Triggering here means the filter thread runs as fast as the encode thread
    // can process the frames.
    m_passthroughCondition.notify_all();

    if (m_pendingFilterFrames <= 0) {
        m_encoder->finish();

        if (m_pendingEncodeFrames <= 0) {
            destroy();
        }
    }
}

std::unique_ptr<Encoder> PipeWireProduce::makeEncoder()
{
    auto forcedEncoder = qEnvironmentVariable("KPIPEWIRE_FORCE_ENCODER");
    if (!forcedEncoder.isNull()) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Forcing encoder to" << forcedEncoder;
    }

    auto size = m_stream->size();

    switch (m_encoderType) {
    case PipeWireBaseEncodedStream::H264Baseline:
    case PipeWireBaseEncodedStream::H264Main: {
        auto profile = m_encoderType == PipeWireBaseEncodedStream::H264Baseline ? Encoder::H264Profile::Baseline : Encoder::H264Profile::Main;

        if (forcedEncoder.isNull() || forcedEncoder == u"h264_vaapi") {
            auto encoder = std::make_unique<H264VAAPIEncoder>(profile, this);
            if (setupEncoder(encoder.get(), size)) {
                return encoder;
            }
        }

        if (forcedEncoder.isNull() || forcedEncoder == u"libx264") {
            auto encoder = std::make_unique<LibX264Encoder>(profile, this);
            if (setupEncoder(encoder.get(), size)) {
                return encoder;
            }
        }

        // Try libopenh264 last, it's slower and has less features.
        if (forcedEncoder.isNull() || forcedEncoder == u"libopenh264") {
            auto encoder = std::make_unique<LibOpenH264Encoder>(profile, this);
            if (setupEncoder(encoder.get(), size)) {
                return encoder;
            }
        }
        break;
    }
    case PipeWireBaseEncodedStream::VP8: {
        if (forcedEncoder.isNull() || forcedEncoder == u"libvpx") {
            auto encoder = std::make_unique<LibVpxEncoder>(this);
            if (setupEncoder(encoder.get(), size)) {
                return encoder;
            }
        }
        break;
    }
    case PipeWireBaseEncodedStream::VP9: {
        if (forcedEncoder.isNull() || forcedEncoder == u"libvpx-vp9") {
            auto encoder = std::make_unique<LibVpxVp9Encoder>(this);
            if (setupEncoder(encoder.get(), size)) {
                return encoder;
            }
        }
        break;
    }
    case PipeWireBaseEncodedStream::Gif: {
        if (forcedEncoder.isNull() || forcedEncoder == u"gif") {
            auto encoder = std::make_unique<GifEncoder>(this);
            if (setupEncoder(encoder.get(), size)) {
                return encoder;
            }
        }
        break;
    }
    case PipeWireBaseEncodedStream::WebP: {
        if (forcedEncoder.isNull() || forcedEncoder == u"libwebp") {
            auto encoder = std::make_unique<LibWebPEncoder>(this);
            if (setupEncoder(encoder.get(), size)) {
                return encoder;
            }
        }
        break;
    }
    default:
        qCWarning(PIPEWIRERECORD_LOGGING) << "Unknown encoder type" << m_encoderType;
    }

    return nullptr;
}

bool PipeWireProduce::setupEncoder(Encoder *encoder, const QSize &size)
{
    encoder->setQuality(m_quality);
    encoder->setEncodingPreference(m_encodingPreference);
    encoder->setColorRange(m_colorRange);
    return encoder->initialize(size);
}

#include "moc_pipewireproduce_p.cpp"
