/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QObject>

#include <kpipewire_export.h>

struct Fraction;
struct PipeWireEncodedStreamPrivate;
class PipeWireProduce;

class KPIPEWIRE_EXPORT PipeWireBaseEncodedStream : public QObject
{
    Q_OBJECT
    /// Specify the pipewire node id that we want to record
    Q_PROPERTY(uint nodeId READ nodeId WRITE setNodeId NOTIFY nodeIdChanged)
    /**
     * Specifies the file descriptor we are connected to, if none 0 will be returned
     *
     * Transfers the ownership of the fd, will close it when it's done with it.
     */
    Q_PROPERTY(uint fd READ fd WRITE setFd NOTIFY fdChanged)
    Q_PROPERTY(bool active READ isActive NOTIFY activeChanged)
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(Encoder encoder READ encoder WRITE setEncoder NOTIFY encoderChanged)

public:
    enum Encoder {
        NoEncoder,
        VP8,
        VP9,
        H264Main,
        H264Baseline,
        WebP,
        Gif,
    };
    Q_ENUM(Encoder)

    PipeWireBaseEncodedStream(QObject *parent = nullptr);
    ~PipeWireBaseEncodedStream() override;

    void setNodeId(uint nodeId);
    uint nodeId() const;

    void setFd(uint fd);
    uint fd() const;

    Fraction maxFramerate() const;
    void setMaxFramerate(const Fraction &framerate);
    void setMaxFramerate(quint32 numerator, quint32 denominator = 1);

    /**
     * Defines how many frames are kept in the encoding buffer.
     * New frames after the buffer is full will be dropped.
     *
     * This needs to be high enough for intra-frame analysis.
     * The default value is 50.
     *
     * There is a minimum value of 3.
     */
    void setMaxPendingFrames(int maxBufferSize);
    int maxBufferSize() const;

    bool isActive() const;
    /**
     * Set the active state of recording.
     *
     * @deprecated Since 6.4, use the separate `start()`/`stop()`calls instead.
     * This function now just calls `start()`/`stop()`.
     *
     * @note When calling `setActive(false)`, unlike `stop()`, this function will
     * block until the internal encoding threads are finished.
     */
    KPIPEWIRE_DEPRECATED void setActive(bool active);

    /**
     * Request to start recording.
     *
     * This will create everything required to perform recording, like a PipeWire
     * stream and an encoder, then start receiving frames from the stream and
     * encoding those.
     *
     * This requires a valid node ID to be set and that the current state is Idle.
     *
     * Note that recording all happens on separate threads, this method only
     * starts the process, only when state() returns Recording is recording
     * actually happening.
     */
    Q_INVOKABLE void start();
    /**
     * Request to stop recording.
     *
     * This will terminate receiving frames from PipeWire and do any cleanup
     * necessary to fully terminate recording after that.
     *
     * Note that after this request, there may still be some processing required
     * due to internal queues. As long as state() does not return Idle processing
     * is still happening and teardown has not been completed.
     */
    Q_INVOKABLE void stop();

    /**
     * The quality used for encoding.
     */
    std::optional<quint8> quality() const;
    /**
     * Set the quality to use for encoding.
     *
     * This uses a range of 0-100 as a percentage, with 0 being lowest quality
     * and 100 being highest. This is internally converted to a value relevant to
     * the encoder.
     *
     * @param quality The quality level to use.
     */
    void setQuality(quint8 quality);

    enum State {
        Idle, //< ready to get started
        Recording, //< actively recording
        Rendering, //< recording is over but there are still frames being processed.
    };
    Q_ENUM(State)
    State state() const;

    /**
     * Set the FFmpeg @p encoder that will be used to create the video
     *
     * They can be inspected using:
     * ffmpeg -encoders | grep "^ V"
     */
    void setEncoder(Encoder encoder);
    Encoder encoder() const;

    /// Returns the encoders that are tested to work, sorted by preference
    QList<PipeWireBaseEncodedStream::Encoder> suggestedEncoders() const;

    enum EncodingPreference {
        NoPreference, ///< Default settings, good for most usecases
        Quality, ///< A bit slower than default, but more consistent bitrate, use for high quality
        Speed, ///< Encode as fast as possible and use zerolatency tune, good for streaming
        Size, ///< Slowest encoding but reduces the size of the file
    };
    Q_ENUM(EncodingPreference);
    void setEncodingPreference(EncodingPreference profile);

Q_SIGNALS:
    void activeChanged(bool active);
    void nodeIdChanged(uint nodeId);
    void fdChanged(uint fd);
    void errorFound(const QString &error);
    void maxFramerateChanged();
    void maxPendingFramesChanged();
    void stateChanged();
    void encoderChanged();

protected:
    virtual std::unique_ptr<PipeWireProduce> makeProduce() = 0;
    EncodingPreference encodingPreference();

    QScopedPointer<PipeWireEncodedStreamPrivate> d;
};
