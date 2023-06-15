/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>
    SPDX-FileCopyrightText: 2023 Marco Martin <mart@kde.org>
    SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QObject>

struct PipeWireFrame;
class PipeWireProduce;

/**
 * Base class for objects that encapsulate encoder logic and state.
 */
class Encoder : public QObject
{
    Q_OBJECT
public:
    /**
     * Constructor.
     *
     * @param produce The PipeWireProduce instance that owns this encoder.
     */
    Encoder(PipeWireProduce *produce);
    ~Encoder() override;

    /**
     * Initialize and setup the encoder.
     *
     * @param size The size of the stream being encoded.
     *
     * @return true if initailization was succesful, false if not.
     */
    virtual bool initialize(const QSize &size) = 0;
    /**
     * Process a PipeWire frame and pass it to libav for filtering.
     *
     * @param frame The frame to process.
     *
     * @note This method will be called on its own thread.
     */
    virtual void filterFrame(const PipeWireFrame &frame) = 0;
    /**
     * Get the next finished frames from the libav filter chain and queue them for encoding.
     *
     * @note This method will be called on its own thread.
     */
    virtual void encodeFrame();
    /**
     * Get the next encoded frames from libav and pass them to PipeWireProduce.
     *
     * @note This method will be called on its own thread.
     */
    virtual void receivePacket();
    /**
     * End encoding and perform any necessary cleanup.
     */
    virtual void finish();

protected:
    PipeWireProduce *m_produce;
};

