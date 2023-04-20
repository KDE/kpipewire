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

class PipeWirePacketPrivate
{
public:
    PipeWirePacketPrivate(bool isKey, const QByteArray &data)
        : isKey(isKey)
        , data(data)
    {
    }

    const bool isKey;
    const QByteArray data;
};

PipeWireEncodedStream::Packet::Packet(bool isKey, const QByteArray &data)
    : d(std::make_shared<PipeWirePacketPrivate>(isKey, data))
{
}

QByteArray PipeWireEncodedStream::Packet::data() const
{
    return d->data;
}

bool PipeWireEncodedStream::Packet::isKeyFrame() const
{
    return d->isKey;
}

PipeWireEncodeProduce::PipeWireEncodeProduce(const QByteArray &encoder,
                                             uint nodeId,
                                             uint fd,
                                             const std::optional<Fraction> &framerate,
                                             PipeWireEncodedStream *stream)
    : PipeWireProduce(encoder, nodeId, fd, framerate)
    , m_encodedStream(stream)
{
}

void PipeWireEncodeProduce::processPacket(AVPacket *packet)
{
    if (!packet) {
        return;
    }

    Q_EMIT newPacket(PipeWireEncodedStream::Packet(packet->flags & AV_PKT_FLAG_KEY, QByteArray(reinterpret_cast<char *>(packet->data), packet->size)));
}

void PipeWireEncodeProduce::processFrame(const PipeWireFrame &frame)
{
    if (m_size != m_stream->size()) {
        m_size = m_stream->size();
        Q_EMIT m_encodedStream->sizeChanged(m_size);
    }

    PipeWireProduce::processFrame(frame);
    if (frame.cursor && m_cursor != *frame.cursor) {
        m_cursor = *frame.cursor;
        Q_EMIT m_encodedStream->cursorChanged(m_cursor);
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
                                             PipeWireBaseEncodedStream::d->m_maxFramerate,
                                             this);
    connect(produce, &PipeWireEncodeProduce::newPacket, this, &PipeWireEncodedStream::newPacket);
    connect(this, &PipeWireEncodedStream::maxFramerateChanged, produce, [this, produce]() {
        produce->setMaxFramerate(maxFramerate());
    });
    return produce;
}
