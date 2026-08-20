/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QObject>

#include "pipewirebaseencodedstream.h"
#include <kpipewire_export.h>

#include <chrono>

struct PipeWireCursor;
class PipeWirePacketPrivate;

/**
 * @brief The PipeWireEncodedStream class provides a realtime stream of encoded H264 data
 */
class KPIPEWIRE_EXPORT PipeWireEncodedStream : public PipeWireBaseEncodedStream
{
    Q_OBJECT
public:
    PipeWireEncodedStream(QObject *parent = nullptr);
    ~PipeWireEncodedStream() override;

    /**
     * @brief The Packet class represents a frame of H264 encoded data
     */
    class Packet
    {
    public:
        Packet(bool isKey, const QByteArray &data, std::chrono::nanoseconds pts);

        /**
         * Whether the packet represents a key frame
         */
        bool isKeyFrame() const;
        /**
         * The raw h264 encoded data
         */
        QByteArray data() const;

        /**
         * The source presentation timestamp of the packet. This is the timestamp that was provided by the source of the stream.
         */
        std::chrono::nanoseconds presentationTimeStamp() const;

        std::shared_ptr<PipeWirePacketPrivate> d;
    };

Q_SIGNALS:
    /// will be emitted when the stream initializes as well as when the value changes
    void sizeChanged(const QSize &size);
    void cursorChanged(const PipeWireCursor &cursor);
    void newPacket(const Packet &packet);

protected:
    std::unique_ptr<PipeWireProduce> makeProduce() override;
};
