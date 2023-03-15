/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QObject>

#include <kpipewire_export.h>

struct PipeWireRecordPrivate;

class KPIPEWIRE_EXPORT PipeWireRecord : public QObject
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
    Q_PROPERTY(QString output READ output WRITE setOutput NOTIFY outputChanged)
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString extension READ extension NOTIFY encoderChanged)
    Q_PROPERTY(QByteArray encoder READ encoder WRITE setEncoder NOTIFY encoderChanged)
public:
    PipeWireRecord(QObject *parent = nullptr);
    ~PipeWireRecord() override;

    enum State {
        Idle, //< ready to get started
        Recording, //< actively recording
        Rendering, //< recording is over but the video file is still being written
    };
    Q_ENUM(State)

    void setNodeId(uint nodeId);
    uint nodeId() const;

    void setFd(uint fd);
    uint fd() const;

    bool isActive() const;
    void setActive(bool active);
    State state() const;

    QString output() const;
    void setOutput(const QString &output);

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

    QString currentExtension() const;
    Q_DECL_DEPRECATED static QString extension();

Q_SIGNALS:
    void activeChanged(bool active);
    void nodeIdChanged(uint nodeId);
    void fdChanged(uint fd);
    void outputChanged(const QString &output);
    void errorFound(const QString &error);
    void stateChanged();
    void encoderChanged();

private:
    void refresh();
    QScopedPointer<PipeWireRecordPrivate> d;
};
