/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QObject>
#include <qqmlintegration.h>

#include "pipewirebaseencodedstream.h"
#include <kpipewire_export.h>

struct PipeWireRecordPrivate;

class KPIPEWIRE_EXPORT PipeWireRecord : public PipeWireBaseEncodedStream
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString output READ output WRITE setOutput NOTIFY outputChanged)
    Q_PROPERTY(QString extension READ extension NOTIFY encoderChanged)
    Q_PROPERTY(bool recordSystemAudio READ recordSystemAudio WRITE setRecordSystemAudio NOTIFY recordSystemAudioChanged)
    Q_PROPERTY(bool recordMicrophone READ recordMicrophone WRITE setRecordMicrophone NOTIFY recordMicrophoneChanged)
public:
    PipeWireRecord(QObject *parent = nullptr);
    ~PipeWireRecord() override;

    QString output() const;
    void setOutput(const QString &output);
    QString extension() const;

    /// Whether to also record what is being played on the default audio output
    bool recordSystemAudio() const;
    void setRecordSystemAudio(bool recordSystemAudio);
    /// Whether to also record the default audio input, e.g. a microphone
    bool recordMicrophone() const;
    void setRecordMicrophone(bool recordMicrophone);

    // Only for compatibility with 5.27
    KPIPEWIRE_DEPRECATED QString currentExtension() const
    {
        return extension();
    }

Q_SIGNALS:
    void outputChanged(const QString &output);
    void recordSystemAudioChanged(bool recordSystemAudio);
    void recordMicrophoneChanged(bool recordMicrophone);

private:
    std::unique_ptr<PipeWireProduce> makeProduce() override;

    QScopedPointer<PipeWireRecordPrivate> d;
};
