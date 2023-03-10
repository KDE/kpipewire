/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QObject>

#include "pipewirebaseencodedstream.h"
#include <kpipewire_export.h>

struct PipeWireCursor;

class KPIPEWIRE_EXPORT PipeWireEncodedStream : public PipeWireBaseEncodedStream
{
    Q_OBJECT
public:
    PipeWireEncodedStream(QObject *parent = nullptr);
    ~PipeWireEncodedStream() override;

Q_SIGNALS:
    void cursorChanged(const PipeWireCursor &cursor);
    void newPacket(const QByteArray &packet);

protected:
    PipeWireProduce *createThread() override;
};
