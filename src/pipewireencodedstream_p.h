/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include "pipewireencodedstream.h"
#include "pipewireproduce.h"

class PipeWireEncodeProduce : public PipeWireProduce
{
    Q_OBJECT
public:
    PipeWireEncodeProduce(const QByteArray &encoder,
                          uint nodeId,
                          uint fd,
                          std::function<void(const PipeWireEncodedStream::Packet &)> callback,
                          PipeWireEncodedStream *stream);

    void processPacket(AVPacket *packet) override;
    void processFrame(const PipeWireFrame &frame) override;

private:
    PipeWireEncodedStream *const m_encodedStream;
    QSize m_size;
    std::function<void(const PipeWireEncodedStream::Packet &)> m_callback;
};
