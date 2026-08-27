/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "pipewireencodedstream.h"
#include "pipewireencodedstream_p.h"
#include "pipewireproduce_p.h"
#include <QDebug>

extern "C" {
#include <libavcodec/packet.h>
}

class PipeWirePacketPrivate
{
public:
    PipeWirePacketPrivate(bool isKey, const QByteArray &data, std::chrono::nanoseconds pts, std::optional<QRegion> damage)
        : isKey(isKey)
        , data(data)
        , presentationTimeStamp(pts)
        , damage(std::move(damage))
    {
    }

    const bool isKey;
    const QByteArray data;
    const std::chrono::nanoseconds presentationTimeStamp;
    const std::optional<QRegion> damage;
};

PipeWireEncodedStream::Packet::Packet(bool isKey, const QByteArray &data, std::chrono::nanoseconds pts, std::optional<QRegion> damage)
    : d(std::make_shared<PipeWirePacketPrivate>(isKey, data, pts, std::move(damage)))
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

std::optional<QRegion> PipeWireEncodedStream::Packet::damage() const
{
    return d->damage;
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
    setDamageTrackingEnabled(true);
}

void PipeWireEncodeProduce::processPacket(AVPacket *packet)
{
    if (!packet) {
        return;
    }

    // This must match the conversion created by PipeWireProduce::framePts
    const auto castedTimeStamp = std::chrono::milliseconds(packet->pts);
    std::optional<QRegion> damage;
    {
        std::lock_guard lock(m_damageMutex);
        if (!m_pendingPacketDamage.empty()) {
            damage = std::move(m_pendingPacketDamage.front());
            m_pendingPacketDamage.pop_front();
        }
    }
    Q_EMIT newPacket(PipeWireEncodedStream::Packet(packet->flags & AV_PKT_FLAG_KEY,
                                                   QByteArray(reinterpret_cast<char *>(packet->data), packet->size),
                                                   castedTimeStamp,
                                                   std::move(damage)));
}

void PipeWireEncodeProduce::frameAcceptedForEncoding(const PipeWireFrame &frame)
{
    std::lock_guard lock(m_damageMutex);
    m_damageByPts.insert_or_assign(framePts(frame.presentationTimestamp), frame.damage);
}

void PipeWireEncodeProduce::frameQueuedForEncoding(int64_t pts)
{
    std::lock_guard lock(m_damageMutex);
    const auto it = m_damageByPts.find(pts);
    if (it == m_damageByPts.end()) {
        m_pendingPacketDamage.push_back({});
        return;
    }
    m_pendingPacketDamage.push_back(std::move(it->second));
    m_damageByPts.erase(it);
}

void PipeWireEncodeProduce::discardEncoderFrameMetadata()
{
    std::lock_guard lock(m_damageMutex);
    m_damageByPts.clear();
    m_pendingPacketDamage.clear();
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
