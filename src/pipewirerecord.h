/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QObject>
#include <QUrl>

#include "pipewirebaseencodedstream.h"
#include <kpipewire_export.h>

struct PipeWireRecordPrivate;

class KPIPEWIRE_EXPORT PipeWireRecord : public PipeWireBaseEncodedStream
{
    Q_OBJECT
    Q_PROPERTY(QUrl output READ output WRITE setOutput NOTIFY outputChanged)
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString extension READ extension NOTIFY encoderChanged)
public:
    PipeWireRecord(QObject *parent = nullptr);
    ~PipeWireRecord() override;

    QUrl output() const;
    void setOutput(const QUrl &output);
    QString extension() const;

Q_SIGNALS:
    void outputChanged(const QUrl &output);

private:
    PipeWireProduce *createThread() override;

    QScopedPointer<PipeWireRecordPrivate> d;
};
