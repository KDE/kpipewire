/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "pipewireencodedstream.h"
#include "logging_frame_tracking.h"
#include "pipewireencodedstream_p.h"
#include "pipewireproduce_p.h"
#include <QDebug>

extern "C" {
#include <libavcodec/packet.h>
}

class PipeWirePacketPrivate
{
public:
    PipeWirePacketPrivate(bool isKey, const QByteArray &data, std::chrono::nanoseconds pts)
        : isKey(isKey)
        , data(data)
        , presentationTimeStamp(pts)
    {
    }

    const bool isKey;
    const QByteArray data;
    const std::chrono::nanoseconds presentationTimeStamp;
};

PipeWireEncodedStream::Packet::Packet(bool isKey, const QByteArray &data, std::chrono::nanoseconds pts)
    : d(std::make_shared<PipeWirePacketPrivate>(isKey, data, pts))
{
}

QByteArray PipeWireEncodedStream::Packet::data() const
{
    return d->data;
}

std::chrono::nanoseconds PipeWireEncodedStream::Packet::presentationTimeStamp() const
{
    return d->presentationTimeStamp;
}

bool PipeWireEncodedStream::Packet::isKeyFrame() const
{
    return d->isKey;
}

PipeWireEncodeProduce::PipeWireEncodeProduce(PipeWireBaseEncodedStream::Encoder encoder,
                                             uint nodeId,
                                             quint64 objectSerial,
                                             uint fd,
                                             const Fraction &framerate,
                                             PipeWireEncodedStream *stream)
    : PipeWireProduce(encoder, nodeId, objectSerial, fd, framerate)
    , m_encodedStream(stream)
{
}

void PipeWireEncodeProduce::processPacket(AVPacket *packet)
{
    if (!packet) {
        return;
    }

    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    qCInfo(PIPEWIREFRAMETRACKING_LOGGING).nospace() << "LOG FRAME " << packet->pts << ",encoded," << now;

    // This must match the conversion created by PipeWireProduce::framePts
    auto castedTimeStamp = std::chrono::milliseconds(packet->pts);

    Q_EMIT newPacket(
        PipeWireEncodedStream::Packet(packet->flags & AV_PKT_FLAG_KEY, QByteArray(reinterpret_cast<char *>(packet->data), packet->size), castedTimeStamp));
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

std::unique_ptr<PipeWireProduce> PipeWireEncodedStream::makeProduce()
{
    auto produce = new PipeWireEncodeProduce(encoder(), nodeId(), objectSerial(), fd(), maxFramerate(), this);
    connect(produce, &PipeWireEncodeProduce::newPacket, this, &PipeWireEncodedStream::newPacket);
    connect(this, &PipeWireEncodedStream::maxFramerateChanged, produce, [this, produce]() {
        produce->setMaxFramerate(maxFramerate());
    });
    return std::unique_ptr<PipeWireProduce>(produce);
}

#include "moc_pipewireencodedstream_p.cpp"

#include "moc_pipewireencodedstream.cpp"
