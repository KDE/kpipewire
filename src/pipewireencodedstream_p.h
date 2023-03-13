/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include "pipewireproduce.h"

class PipeWireEncodeProduce : public PipeWireProduce
{
    Q_OBJECT
public:
    PipeWireEncodeProduce(const QByteArray &encoder, uint nodeId, uint fd, PipeWireEncodedStream *stream);

    void processPacket(AVPacket *packet) override;
    int64_t framePts(const PipeWireFrame &frame) override;
    void processFrame(const PipeWireFrame &frame) override;

Q_SIGNALS:
    void newPacket(const QByteArray &packetData);

private:
    PipeWireEncodedStream *const m_stream;
};