/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QObject>

#include <kpipewire_export.h>

class PipeWireRecordProduceThread;

class KPIPEWIRE_EXPORT PipeWireRecord : public QObject
{
    Q_OBJECT
    /// Specify the pipewire node id that we want to record
    Q_PROPERTY(uint nodeId READ nodeId WRITE setNodeId NOTIFY nodeIdChanged)
    Q_PROPERTY(bool active READ isActive WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool recording READ isRecording NOTIFY recordingChanged)
    Q_PROPERTY(QString output READ output WRITE setOutput NOTIFY outputChanged)
public:
    PipeWireRecord(QObject *parent = nullptr);
    ~PipeWireRecord() override;

    void setNodeId(uint nodeId);
    uint nodeId() const
    {
        return m_nodeId;
    }

    bool isRecording() const
    {
        return m_recordThread || !m_lastRecordThreadFinished;
    }
    bool isActive() const
    {
        return m_active;
    }
    void setActive(bool active);

    QString output() const
    {
        return m_output;
    }
    void setOutput(const QString &output);

    /**
     * Set the FFmpeg @p encoder that will be used to create the video
     *
     * They can be inspected using:
     * ffmpeg -encoders | grep "^ V"
     */
    void setEncoder(const QByteArray &encoder) {
        m_encoder = encoder;
    }

Q_SIGNALS:
    void activeChanged(bool active);
    void recordingChanged(bool recording);
    void nodeIdChanged(uint nodeId);
    void outputChanged(const QString &output);

private:
    void refresh();

    uint m_nodeId = 0;
    bool m_active = false;
    QString m_output;
    PipeWireRecordProduceThread *m_recordThread = nullptr;
    bool m_lastRecordThreadFinished = true;
    QByteArray m_encoder;
};
