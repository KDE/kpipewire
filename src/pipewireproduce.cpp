/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "pipewireproduce.h"

#include <QMutex>
#include <QPainter>
#include <QThreadPool>
#include <logging_record.h>

#include <QDateTime>
#include <memory>
#include <qstringliteral.h>

#include "h264vaapiencoder.h"

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
    qDebug() << __PRETTY_FUNCTION__;

    const QSize size = m_stream->size();

    qCDebug(PIPEWIRERECORD_LOGGING) << "Setting up stream";
    disconnect(m_stream.get(), &PipeWireSourceStream::streamParametersChanged, this, &PipeWireProduce::setupStream);

    switch (m_encoderType) {
    case PipeWireBaseEncodedStream::H264Baseline:
    case PipeWireBaseEncodedStream::H264Main: {
        auto profile = m_encoderType == PipeWireBaseEncodedStream::H264Baseline ? H264VAAPIEncoder::Profile::Baseline : H264VAAPIEncoder::Profile::Main;
        auto encoder = std::make_unique<H264VAAPIEncoder>(profile, this);
        if (encoder->initialize(size)) {
            m_encoder = std::move(encoder);
            break;
        }

        // m_encoder = std::make_unique<LibX264Encoder>();
        // if (!m_encoder->initialize(size)) {
        //     qCWarning(PIPEWIRERECORD_LOGGING) << "Could not initialize H264 encoder";
        //     return;
        // }
        break;
    }
    case PipeWireBaseEncodedStream::VP8:
        // m_encoder = std::make_unique<VP8Encoder>();
        // if (!m_encoder->initialize(size)) {
        //     qCWarning(PIPEWIRERECORD_LOGGING) << "Could not initialize VP8 encoder";
        //     return;
        // }
        break;
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

    m_passthroughThread = std::jthread([this](std::stop_token stopToken) {
        while (!stopToken.stop_requested()) {
            m_encoder->encodeFrame();
        }
    });

    m_outputThread = std::jthread([this](std::stop_token stopToken) {
        while (!stopToken.stop_requested()) {
            m_encoder->receivePacket();
        }
    });
}

void PipeWireProduce::deactivate()
{
}

void PipeWireProduce::processFrame(const PipeWireFrame &frame)
{
    if (frame.cursor) {
        m_cursor.position = frame.cursor->position;
        m_cursor.hotspot = frame.cursor->hotspot;
        if (!frame.cursor->texture.isNull()) {
            m_cursor.dirty = true;
            m_cursor.texture = frame.cursor->texture;
        }
    }

    m_encoder->filterFrame(frame);
}

void PipeWireProduce::stateChanged(pw_stream_state state)
{
    qDebug() << __PRETTY_FUNCTION__ << state;
    if (state != PW_STREAM_STATE_PAUSED || !m_deactivated) {
        return;
    }
    if (!m_stream) {
        qCDebug(PIPEWIRERECORD_LOGGING) << "finished without a stream";
        return;
    }

    disconnect(m_stream.data(), &PipeWireSourceStream::frameReceived, this, &PipeWireProduce::processFrame);

    if (m_passthroughThread.joinable()) {
        m_passthroughThread.request_stop();
        m_passthroughThread.join();
    }

    if (m_outputThread.joinable()) {
        m_outputThread.request_stop();
        m_outputThread.join();
    }

    qCDebug(PIPEWIRERECORD_LOGGING) << "finished";
    cleanup();
    QThread::currentThread()->quit();
}
