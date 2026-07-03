/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QObject>
#include <epoxy/egl.h>

extern "C" {
#include <pipewire/pipewire.h>
#include <spa/param/format-utils.h>
#include <spa/param/props.h>
#include <spa/param/video/format-utils.h>
}

#include <QFile>
#include <QImage>
#include <QMutex>
#include <QPoint>
#include <QQueue>
#include <QRunnable>
#include <QSize>
#include <QThread>
#include <QTimer>
#include <QWaitCondition>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include "pipewirebaseencodedstream.h"
#include "pipewiresourcestream.h"

struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

struct AVFilterContext;
struct AVFilterGraph;

class CustomAVFrame;
class Encoder;
class PipeWireReceiveEncodedThread;

class PipeWireProduce : public QObject
{
    Q_OBJECT
public:
    PipeWireProduce(PipeWireBaseEncodedStream::Encoder encoderType, uint nodeId, quint64 objectSerial, uint fd, const Fraction &framerate);
    ~PipeWireProduce() override;

    virtual void initialize();

    QString error() const
    {
        return m_error;
    }

    Fraction maxFramerate() const;
    void setMaxFramerate(const Fraction &framerate);

    int maxPendingFrames() const;
    void setMaxPendingFrames(int newMaxBufferSize);

    QSize requestedSize() const;
    void setRequestedSize(const QSize &size);

    virtual int64_t framePts(const std::optional<std::chrono::nanoseconds> &presentationTimestamp)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(presentationTimestamp.value()).count();
    }

    virtual void processPacket(AVPacket *packet) = 0;
    virtual bool setupFormat()
    {
        return true;
    }
    // Whether this producer can cope with the source changing size while
    // streaming. Producers that write a container/muxer (e.g. recording to a
    // file) cannot, since the format is fixed once the header is written, so
    // they keep the default and ignore size changes. Live consumers that can
    // re-key the stream override this to opt into mid-stream encoder rebuilds.
    virtual bool supportsResize() const
    {
        return false;
    }
    virtual void cleanup()
    {
    }

    void stateChanged(pw_stream_state state);
    void setupStream();
    // Handles the source stream (re)negotiating its parameters. On the first
    // call it performs the full stream setup; on later calls it rebuilds the
    // encoder when the source size changed, so a resolution change is handled
    // mid-stream without tearing down the connection.
    void handleStreamParametersChanged();
    // Rebuild the encoder (and its worker threads) to match the current source
    // size after a mid-stream resize.
    void reconfigureStream();
    // Drop all frame state (last frame, repeat timer, queue counters) tied to
    // the encoder being replaced, so nothing of the previous size reaches the
    // new encoder. Called from reconfigureStream() while the workers are stopped.
    void discardFrameState();
    // Start/stop the passthrough and output worker threads that drive the
    // encoder. Split out so the encoder can be swapped safely on a resize.
    void startThreads();
    void stopThreads();
    virtual void processFrame(const PipeWireFrame &frame);
    void render(const QImage &image, const PipeWireFrame &frame);
    virtual void aboutToEncode(PipeWireFrame &frame)
    {
        Q_UNUSED(frame);
    }

    // Pause/resume frame delivery without tearing the stream down (suppress-output).
    // Runs on the produce thread. Does not set m_deactivated, so it does not trigger
    // teardown in stateChanged().
    void setStreamActive(bool active);

    void deactivate();

    void destroy();

    void setQuality(const std::optional<quint8> &quality);

    void setEncodingPreference(const PipeWireBaseEncodedStream::EncodingPreference &encodingPreference);

    void setColorRange(PipeWireBaseEncodedStream::ColorRange colorRange);

    void handleEncodedFramesChanged();

    const uint m_nodeId;
    const quint64 m_objectSerial;
    QScopedPointer<PipeWireSourceStream> m_stream;
    QString m_error;

    PipeWireBaseEncodedStream::Encoder m_encoderType;
    QByteArray m_encoderName;
    std::unique_ptr<Encoder> m_encoder;
    // The source size the current encoder was created for, used to detect
    // mid-stream resizes that require rebuilding the encoder.
    QSize m_encoderSize;

    uint m_fd;
    Fraction m_frameRate;
    QSize m_requestedSize;

    std::optional<quint8> m_quality;

    PipeWireBaseEncodedStream::EncodingPreference m_encodingPreference;
    PipeWireBaseEncodedStream::ColorRange m_colorRange = PipeWireBaseEncodedStream::ColorRange::Limited;

    struct {
        QImage texture;
        std::optional<QPoint> position;
        QPoint hotspot;
        bool dirty = false;
    } m_cursor;

    QScopedPointer<QTimer> m_frameRepeatTimer;
    bool m_enableFrameRepeat = true;
    PipeWireFrame m_lastFrame;

    std::thread m_passthroughThread;
    std::thread m_outputThread;
    // Can't use jthread directly as it's not available in libc++ yet,
    // so manually handle the stop source.
    std::atomic_bool m_passthroughRunning = false;
    std::atomic_bool m_outputRunning = false;

    std::condition_variable m_passthroughCondition;
    std::mutex m_passthroughMutex;
    std::condition_variable m_outputCondition;
    std::mutex m_outputMutex;

    std::atomic_bool m_deactivated = false;

    int64_t m_previousPts = -1;

    std::atomic_int m_pendingFilterFrames = 0;
    std::atomic_int m_pendingEncodeFrames = 0;
    std::atomic_int m_processedFrames = 0;

    // Controls how many frames we can push into ffmpeg's encoding stream
    std::atomic_int m_maxPendingFrames = 50;

    Fraction m_maxFramerate = {60, 1};

    std::unique_ptr<QTimer> m_frameStatisticsTimer;

Q_SIGNALS:
    void producedFrames();
    void started();
    void finished();

private:
    void initFiltersVaapi();
    void initFiltersSoftware();

    std::unique_ptr<Encoder> makeEncoder();
    bool setupEncoder(Encoder *encoder, const QSize &size);
};
