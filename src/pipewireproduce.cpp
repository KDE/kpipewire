/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "pipewireproduce_p.h"

#include <QMutex>
#include <QPainter>
#include <QThreadPool>
#include <logging_record.h>

#include <QDateTime>
#include <memory>
#include <qstringliteral.h>

#include "h264vaapiencoder_p.h"
#include "libvpxencoder_p.h"
#include "libx264encoder_p.h"

extern "C" {
#include <fcntl.h>
}

Q_DECLARE_METATYPE(std::optional<int>);
Q_DECLARE_METATYPE(std::optional<std::chrono::nanoseconds>);

PipeWireProduce::PipeWireProduce(PipeWireBaseEncodedStream::Encoder encoderType, uint nodeId, uint fd, const std::optional<Fraction> &framerate)
    : QObject()
    , m_nodeId(nodeId)
    , m_encoderType(encoderType)
    , m_fd(fd)
    , m_frameRate(framerate)
{
    qRegisterMetaType<std::optional<int>>();
    qRegisterMetaType<std::optional<std::chrono::nanoseconds>>();
}

PipeWireProduce::~PipeWireProduce()
{
}

void PipeWireProduce::initialize()
{
    m_stream.reset(new PipeWireSourceStream(nullptr));
    if (m_frameRate) {
        m_stream->setMaxFramerate(*m_frameRate);
    }
    bool created = m_stream->createStream(m_nodeId, m_fd);
    if (!created || !m_stream->error().isEmpty()) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "failed to set up stream for" << m_nodeId << m_stream->error();
        m_error = m_stream->error();
        m_stream.reset(nullptr);
        return;
    }
    connect(m_stream.get(), &PipeWireSourceStream::streamParametersChanged, this, &PipeWireProduce::setupStream);
}

Fraction PipeWireProduce::maxFramerate() const
{
    return m_stream->framerate();
}

void PipeWireProduce::setMaxFramerate(const Fraction &framerate)
{
    m_stream->setMaxFramerate(framerate);
}

void PipeWireProduce::setupStream()
{
    const QSize size = m_stream->size();

    qCDebug(PIPEWIRERECORD_LOGGING) << "Setting up stream";
    disconnect(m_stream.get(), &PipeWireSourceStream::streamParametersChanged, this, &PipeWireProduce::setupStream);

    switch (m_encoderType) {
    case PipeWireBaseEncodedStream::H264Baseline:
    case PipeWireBaseEncodedStream::H264Main: {
        auto profile = m_encoderType == PipeWireBaseEncodedStream::H264Baseline ? Encoder::H264Profile::Baseline : Encoder::H264Profile::Main;
        auto hardwareEncoder = std::make_unique<H264VAAPIEncoder>(profile, this);
        hardwareEncoder->setQuality(m_quality);
        if (hardwareEncoder->initialize(size)) {
            m_encoder = std::move(hardwareEncoder);
            break;
        }

        auto softwareEncoder = std::make_unique<LibX264Encoder>(profile, this);
        softwareEncoder->setQuality(m_quality);
        if (!softwareEncoder->initialize(size)) {
            qCWarning(PIPEWIRERECORD_LOGGING) << "Could not initialize H264 encoder";
            return;
        }
        m_encoder = std::move(softwareEncoder);
        break;
    }
    case PipeWireBaseEncodedStream::VP8: {
        auto encoder = std::make_unique<LibVpxEncoder>(this);
        encoder->setQuality(m_quality);
        if (!encoder->initialize(size)) {
            qCWarning(PIPEWIRERECORD_LOGGING) << "Could not initialize VP8 encoder";
            return;
        }
        m_encoder = std::move(encoder);
        break;
    }
    default:
        qCWarning(PIPEWIRERECORD_LOGGING) << "Unknown encoder type" << m_encoderType;
        return;
    }

    if (!m_encoder) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "No encoder could be created";
        return;
    }

    connect(m_stream.get(), &PipeWireSourceStream::stateChanged, this, &PipeWireProduce::stateChanged);
    if (!setupFormat()) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not set up the producing thread";
        return;
    }

    connect(m_stream.data(), &PipeWireSourceStream::frameReceived, this, &PipeWireProduce::processFrame);

    m_passthroughThread = std::thread([this]() {
        m_passthroughRunning = true;
        while (m_passthroughRunning) {
            std::unique_lock<std::mutex> lock(m_frameReceivedMutex);
            m_frameReceivedCondition.wait(lock);

            if (!m_passthroughRunning) {
                break;
            }

            m_encoder->encodeFrame();

            m_frameReceivedCondition.notify_all();
        }
    });
    pthread_setname_np(m_passthroughThread.native_handle(), "PipeWireProduce::passthrough");

    m_outputThread = std::thread([this]() {
        m_outputRunning = true;
        while (m_outputRunning) {
            std::unique_lock<std::mutex> lock(m_frameReceivedMutex);
            m_frameReceivedCondition.wait(lock);

            if (!m_outputRunning) {
                break;
            }

            m_encoder->receivePacket();
        }
    });
    pthread_setname_np(m_outputThread.native_handle(), "PipeWireProduce::output");
}

void PipeWireProduce::deactivate()
{
    m_deactivated = true;
    m_stream->setActive(false);
}

void PipeWireProduce::setQuality(const std::optional<quint8> &quality)
{
    m_quality = quality;
    if (m_encoder) {
        m_encoder->setQuality(quality);
    }
}

void PipeWireProduce::processFrame(const PipeWireFrame &frame)
{
    auto f = frame;

    if (frame.cursor) {
        m_cursor.position = frame.cursor->position;
        m_cursor.hotspot = frame.cursor->hotspot;
        if (!frame.cursor->texture.isNull()) {
            m_cursor.dirty = true;
            m_cursor.texture = frame.cursor->texture;
        }
    }

    aboutToEncode(f);
    m_encoder->filterFrame(f);

    m_frameReceivedCondition.notify_all();
}

void PipeWireProduce::stateChanged(pw_stream_state state)
{
    if (state != PW_STREAM_STATE_PAUSED || !m_deactivated) {
        return;
    }
    if (!m_stream) {
        qCDebug(PIPEWIRERECORD_LOGGING) << "finished without a stream";
        return;
    }

    disconnect(m_stream.data(), &PipeWireSourceStream::frameReceived, this, &PipeWireProduce::processFrame);

    m_encoder->finish();

    if (m_passthroughThread.joinable()) {
        m_passthroughRunning = false;
        m_frameReceivedCondition.notify_all();
        m_passthroughThread.join();
    }

    if (m_outputThread.joinable()) {
        m_outputRunning = false;
        m_frameReceivedCondition.notify_all();
        m_outputThread.join();
    }

    qCDebug(PIPEWIRERECORD_LOGGING) << "finished";
    cleanup();
    QThread::currentThread()->quit();
}
