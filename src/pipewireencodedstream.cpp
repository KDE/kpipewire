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
                                             std::function<void(const PipeWireEncodedStream::Packet &)> callback,
                                             PipeWireEncodedStream *stream)
    : PipeWireProduce(encoder, nodeId, fd)
    , m_encodedStream(stream)
    , m_callback(callback)
{
}

void PipeWireEncodeProduce::processPacket(AVPacket *packet)
{
    if (!packet) {
        return;
    }

    m_callback(PipeWireEncodedStream::Packet(packet->flags & AV_PKT_FLAG_KEY, QByteArray(reinterpret_cast<char *>(packet->data), packet->size)));
}

void PipeWireEncodeProduce::processFrame(const PipeWireFrame &frame)
{
    if (m_size != m_stream->size()) {
        m_size = m_stream->size();
        Q_EMIT m_encodedStream->sizeChanged(m_size);
    }

    PipeWireProduce::processFrame(frame);
    if (frame.cursor) {
        Q_EMIT m_encodedStream->cursorChanged(*frame.cursor);
    }
}

class PipeWireEncodedStreamPrivate
{
public:
    std::function<void(const PipeWireEncodedStream::Packet &)> m_callback;
};

PipeWireEncodedStream::PipeWireEncodedStream(QObject *parent)
    : PipeWireBaseEncodedStream(parent)
    , d(new PipeWireEncodedStreamPrivate)
{
}

PipeWireEncodedStream::~PipeWireEncodedStream() = default;

PipeWireProduce *PipeWireEncodedStream::createThread()
{
    auto produce = new PipeWireEncodeProduce(PipeWireBaseEncodedStream::d->m_encoder,
                                             PipeWireBaseEncodedStream::d->m_nodeId,
                                             PipeWireBaseEncodedStream::d->m_fd.value_or(0),
                                             d->m_callback,
                                             this);
    return produce;
}

void PipeWireEncodedStream::setPacketCallback(const std::function<void(const Packet &)> &callback)
{
    d->m_callback = callback;
}
