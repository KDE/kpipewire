/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QObject>

#include <kpipewire_export.h>

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
    Q_PROPERTY(bool active READ isActive WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QByteArray encoder READ encoder WRITE setEncoder NOTIFY encoderChanged)

public:
    PipeWireBaseEncodedStream(QObject *parent = nullptr);
    ~PipeWireBaseEncodedStream() override;

    void setNodeId(uint nodeId);
    uint nodeId() const;

    void setFd(uint fd);
    uint fd() const;

    bool isActive() const;
    void setActive(bool active);

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
    void setEncoder(const QByteArray &encoder);
    QByteArray encoder() const;

    /// Returns the encoders that are tested to work, sorted by preference
    QList<QByteArray> suggestedEncoders() const;

Q_SIGNALS:
    void activeChanged(bool active);
    void nodeIdChanged(uint nodeId);
    void fdChanged(uint fd);
    void errorFound(const QString &error);
    void stateChanged();
    void encoderChanged();

protected:
    friend class PipeWireProduceThread;
    virtual PipeWireProduce *createThread() = 0;

    void refresh();
    QScopedPointer<PipeWireEncodedStreamPrivate> d;
};
