/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "pipewireencodedstream.h"
#include "pipewireencodedstream_p.h"
#include "pipewireproduce.h"
#include <QDebug>

extern "C" {
#include <libavcodec/packet.h>
}

PipeWireEncodeProduce::PipeWireEncodeProduce(const QByteArray &encoder, uint nodeId, uint fd, PipeWireEncodedStream *stream)
    : PipeWireProduce(encoder, nodeId, fd)
    , m_stream(stream)
{
}

void PipeWireEncodeProduce::processPacket(AVPacket *packet)
{
    if (!packet) {
        return;
    }

    Q_EMIT newPacket(QByteArray(reinterpret_cast<char *>(packet->data), packet->size));
}

void PipeWireEncodeProduce::processFrame(const PipeWireFrame &frame)
{
    PipeWireProduce::processFrame(frame);
    if (frame.cursor) {
        Q_EMIT m_stream->cursorChanged(*frame.cursor);
    }
}

PipeWireEncodedStream::PipeWireEncodedStream(QObject *parent)
    : PipeWireBaseEncodedStream(parent)
{
}

PipeWireEncodedStream::~PipeWireEncodedStream() = default;

PipeWireProduce *PipeWireEncodedStream::createThread()
{
    auto produce = new PipeWireEncodeProduce(PipeWireBaseEncodedStream::d->m_encoder,
                                             PipeWireBaseEncodedStream::d->m_nodeId,
                                             PipeWireBaseEncodedStream::d->m_fd.value_or(0),
                                             this);
    connect(produce, &PipeWireEncodeProduce::newPacket, this, &PipeWireEncodedStream::newPacket);
    return produce;
}
