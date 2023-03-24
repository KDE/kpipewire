/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include "pipewirebaseencodedstream.h"
#include <functional>
#include <kpipewire_export.h>
#include <memory>

struct PipeWireCursor;
class PipeWirePacketPrivate;
class PipeWireEncodedStreamPrivate;

class KPIPEWIRE_EXPORT PipeWireEncodedStream : public PipeWireBaseEncodedStream
{
    Q_OBJECT
public:
    PipeWireEncodedStream(QObject *parent = nullptr);
    ~PipeWireEncodedStream() override;

    class Packet
    {
    public:
        Packet(bool isKey, const QByteArray &data);

        /// Whether the packet represents a key frame
        bool isKeyFrame() const;
        QByteArray data() const;

        std::shared_ptr<PipeWirePacketPrivate> d;
    };

    /**
     * Provide a @p callback that will inform us about packets as they are received
     *
     * Note that the callbacks might happen in a different thread than this object's
     *
     * Make sure to set this property before activating the stream.
     */
    void setPacketCallback(const std::function<void(const Packet &)> &callback);

Q_SIGNALS:
    /// will be emitted when the stream initializes as well as when the value changes
    void sizeChanged(const QSize &size);
    void cursorChanged(const PipeWireCursor &cursor);

protected:
    PipeWireProduce *createThread() override;
    std::unique_ptr<PipeWireEncodedStreamPrivate> d;
};
