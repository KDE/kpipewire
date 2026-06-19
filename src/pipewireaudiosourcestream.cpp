/*
    SPDX-FileCopyrightText: 2026 Khudoberdi <xudoyberdi0410@gmail.com>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "audioconstants_p.h"
#include "logging.h"
#include "pipewireaudiosourcestream_p.h"
#include "pipewirecore_p.h"

#include <spa/param/audio/format-utils.h>

#include <QSharedPointer>

struct PipeWireAudioSourceStreamPrivate {
    QSharedPointer<PipeWireCore> pwCore;
    pw_stream *pwStream = nullptr;
    spa_hook streamListener{};

    PipeWireAudioSourceStream::Source source = PipeWireAudioSourceStream::Source::DefaultOutputMonitor;

    QAtomicInt m_stopped = false;
    pw_stream_state m_state = PW_STREAM_STATE_UNCONNECTED;

    spa_audio_info_raw audioFormat{};
    QString m_error;
};

void PipeWireAudioSourceStream::onStreamStateChanged(void *data, pw_stream_state old, pw_stream_state state, const char *error_message)
{
    PipeWireAudioSourceStream *pw = static_cast<PipeWireAudioSourceStream *>(data);
    qCDebug(PIPEWIRE_LOGGING) << "audio state changed" << pw_stream_state_as_string(old) << "->" << pw_stream_state_as_string(state) << error_message;
    pw->d->m_state = state;
    Q_EMIT pw->stateChanged(state, old);

    switch (state) {
    case PW_STREAM_STATE_ERROR:
        qCWarning(PIPEWIRE_LOGGING) << "Audio stream error: " << error_message;
        break;
    case PW_STREAM_STATE_PAUSED:
    case PW_STREAM_STATE_STREAMING:
    case PW_STREAM_STATE_CONNECTING:
        break;
    case PW_STREAM_STATE_UNCONNECTED:
        if (!pw->d->m_stopped) {
            Q_EMIT pw->stopStreaming();
        }
        break;
    }
}

void PipeWireAudioSourceStream::onStreamParamChanged(void *data, uint32_t id, const struct spa_pod *format)
{
    if (!format || id != SPA_PARAM_Format) {
        return;
    }

    PipeWireAudioSourceStream *pw = static_cast<PipeWireAudioSourceStream *>(data);
    spa_format_audio_raw_parse(format, &pw->d->audioFormat);
}

static void onAudioProcess(void *data)
{
    PipeWireAudioSourceStream *stream = static_cast<PipeWireAudioSourceStream *>(data);
    stream->process();
}

pw_stream_state PipeWireAudioSourceStream::state() const
{
    return d->m_state;
}

QString PipeWireAudioSourceStream::error() const
{
    return d->m_error;
}

PipeWireAudioSourceStream::PipeWireAudioSourceStream(Source source, QObject *parent)
    : QObject(parent)
    , d(new PipeWireAudioSourceStreamPrivate)
{
    d->source = source;
}

PipeWireAudioSourceStream::~PipeWireAudioSourceStream()
{
    d->m_stopped = true;
    if (d->pwStream) {
        pw_stream_destroy(d->pwStream);
    }
}

bool PipeWireAudioSourceStream::createStream()
{
    // Audio comes from the user's session daemon, not from the portal remote
    d->pwCore = PipeWireCore::fetch(0);
    if (!d->pwCore->error().isEmpty()) {
        qCDebug(PIPEWIRE_LOGGING) << "received error while creating the audio stream" << d->pwCore->error();
        d->m_error = d->pwCore->error();
        return false;
    }

    connect(d->pwCore.data(), &PipeWireCore::pipewireFailed, this, &PipeWireAudioSourceStream::coreFailed);

    if (objectName().isEmpty()) {
        setObjectName(d->source == Source::DefaultOutputMonitor ? QStringLiteral("plasma-screencast-audio-monitor")
                                                                : QStringLiteral("plasma-screencast-audio-input"));
    }

    pw_properties *properties = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr);
    if (d->source == Source::DefaultOutputMonitor) {
        pw_properties_set(properties, PW_KEY_STREAM_CAPTURE_SINK, "true");
    }

    d->pwStream = pw_stream_new(**d->pwCore, objectName().toUtf8().constData(), properties);

    // Immutable and shared between all streams, PipeWire dispatches through
    // it for as long as the listener is registered.
    static const pw_stream_events pwAudioStreamEvents = {
        .version = PW_VERSION_STREAM_EVENTS,
        .destroy = &PipeWireAudioSourceStream::onDestroy,
        .state_changed = &PipeWireAudioSourceStream::onStreamStateChanged,
        .param_changed = &PipeWireAudioSourceStream::onStreamParamChanged,
        .process = &onAudioProcess,
    };
    pw_stream_add_listener(d->pwStream, &d->streamListener, &pwAudioStreamEvents, this);

    uint8_t buffer[1024];
    spa_pod_builder podBuilder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    // A single fixed format: the PipeWire adapter resamples and remixes
    // whatever the device produces, so this negotiation always succeeds
    spa_audio_info_raw audioInfo{};
    audioInfo.format = SPA_AUDIO_FORMAT_F32;
    audioInfo.rate = AudioSampleRate;
    audioInfo.channels = AudioChannels;
    audioInfo.position[0] = SPA_AUDIO_CHANNEL_FL;
    audioInfo.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod *param = spa_format_audio_raw_build(&podBuilder, SPA_PARAM_EnumFormat, &audioInfo);

    // Deliberately no PW_STREAM_FLAG_DONT_RECONNECT: when the current default
    // device disappears (e.g. a USB headset is unplugged) we want PipeWire to
    // reconnect the stream to the new default rather than leaving it
    // UNCONNECTED and the track silent for the rest of the recording. The
    // resulting gap is filled with silence by the pad timer and the
    // gap-correction in PipeWireProduce::processAudioFrame(), so A/V stays in
    // sync. Real daemon death is still handled, via PipeWireCore's
    // pipewireFailed signal feeding into coreFailed().
    pw_stream_flags s = (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS);
    if (pw_stream_connect(d->pwStream, PW_DIRECTION_INPUT, PW_ID_ANY, s, &param, 1) != 0) {
        qCWarning(PIPEWIRE_LOGGING) << "Could not connect audio stream";
        pw_stream_destroy(d->pwStream);
        d->pwStream = nullptr;
        return false;
    }
    qCDebug(PIPEWIRE_LOGGING) << "audio stream created successfully" << objectName();
    return true;
}

void PipeWireAudioSourceStream::handleFrame(struct pw_buffer *buffer)
{
    spa_buffer *spaBuffer = buffer->buffer;

    PipeWireAudioFrame frame;
    frame.channels = d->audioFormat.channels;
    frame.rate = d->audioFormat.rate;

    // PipeWire does not fill the buffer header meta for audio capture
    // streams, so derive a CLOCK_MONOTONIC capture time from the stream's
    // time report by subtracting the capture latency from the graph time.
    pw_time time{};
#if PW_CHECK_VERSION(0, 3, 50)
    const int timeResult = pw_stream_get_time_n(d->pwStream, &time, sizeof(time));
#else
    const int timeResult = pw_stream_get_time(d->pwStream, &time);
#endif
    if (timeResult == 0 && time.now > 0 && time.rate.denom > 0) {
        // time.delay is the capture latency for an input device (the buffer was
        // captured that long before time.now), so subtracting it yields the
        // real capture time. For a sink monitor, however, time.delay is the
        // playback latency of the output, and subtracting it would shift the
        // system audio earlier than the video by up to hundreds of ms (e.g. on
        // Bluetooth). The monitored content is already present at time.now, so
        // only correct for capture latency on actual input devices.
        const int64_t delay = d->source == Source::DefaultInput ? time.delay * SPA_NSEC_PER_SEC * time.rate.num / time.rate.denom : 0;
        frame.presentationTimestamp = std::chrono::nanoseconds(time.now - delay);
    } else {
        frame.presentationTimestamp = std::chrono::steady_clock::now().time_since_epoch();
    }

    const spa_chunk *chunk = spaBuffer->datas[0].chunk;
    if (chunk->flags & SPA_CHUNK_FLAG_CORRUPTED || chunk->size == 0 || !spaBuffer->datas[0].data || frame.channels == 0) {
        qCDebug(PIPEWIRE_LOGGING) << "skipping empty audio buffer" << chunk->size << chunk->flags;
        return;
    }

    frame.data = reinterpret_cast<const float *>(static_cast<const uint8_t *>(spaBuffer->datas[0].data) + chunk->offset);
    frame.sampleCount = chunk->size / (frame.channels * sizeof(float));

    // The frame data is only valid during the emission, receivers must copy it
    Q_EMIT framesReceived(frame);
}

void PipeWireAudioSourceStream::coreFailed(const QString &errorMessage)
{
    qCDebug(PIPEWIRE_LOGGING) << "received error message" << errorMessage;
    d->m_error = errorMessage;
    Q_EMIT stopStreaming();
}

void PipeWireAudioSourceStream::process()
{
#if !PW_CHECK_VERSION(0, 3, 73)
    if (Q_UNLIKELY(!d->pwStream)) {
        // Assuming it's caused by https://gitlab.freedesktop.org/pipewire/pipewire/-/issues/3314
        qCDebug(PIPEWIRE_LOGGING) << "stream was terminated before processing buffer";
        return;
    }
#endif

    pw_buffer *buf = pw_stream_dequeue_buffer(d->pwStream);
    if (!buf) {
        qCDebug(PIPEWIRE_LOGGING) << "out of buffers";
        return;
    }

    handleFrame(buf);

    pw_stream_queue_buffer(d->pwStream, buf);
}

void PipeWireAudioSourceStream::setActive(bool active)
{
    if (!d->pwStream) {
        qCWarning(PIPEWIRE_LOGGING) << "Tried to make uncreated stream active";
        return;
    }
    pw_stream_set_active(d->pwStream, active);
}

void PipeWireAudioSourceStream::onDestroy(void *data)
{
    // When PipeWire restarts the stream will auto-delete. Make sure we don't have dangling pointers!
    auto pw = static_cast<PipeWireAudioSourceStream *>(data);
    pw->d->pwStream = nullptr;
}

#include "moc_pipewireaudiosourcestream_p.cpp"
