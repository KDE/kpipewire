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

PipeWireProduce::PipeWireProduce(PipeWireBaseEncodedStream::Encoder encoderType, uint nodeId, uint fd, const Fraction &framerate)
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
    m_stream->setMaxFramerate(m_frameRate);
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
    qCDebug(PIPEWIRERECORD_LOGGING) << "Setting up stream";
    disconnect(m_stream.get(), &PipeWireSourceStream::streamParametersChanged, this, &PipeWireProduce::setupStream);

    m_encoder = makeEncoder();
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

std::unique_ptr<Encoder> PipeWireProduce::makeEncoder()
{
    auto encoderType = m_encoderType;
    bool forceSoftware = false;
    bool forceHardware = false;

    if (qEnvironmentVariableIsSet("KPIPEWIRE_FORCE_ENCODER")) {
        auto forcedEncoder = qEnvironmentVariable("KPIPEWIRE_FORCE_ENCODER");
        if (forcedEncoder == u"libvpx") {
            qCWarning(PIPEWIRERECORD_LOGGING) << "Forcing VP8 Software encoding";
            encoderType = PipeWireBaseEncodedStream::VP8;
            forceSoftware = true;
        } else if (forcedEncoder == u"libx264") {
            qCWarning(PIPEWIRERECORD_LOGGING) << "Forcing H264 Software encoding, main profile";
            encoderType = PipeWireBaseEncodedStream::H264Main;
            forceSoftware = true;
        } else if (forcedEncoder == u"h264_vaapi") {
            qCWarning(PIPEWIRERECORD_LOGGING) << "Forcing H264 Hardware encoding, main profile";
            encoderType = PipeWireBaseEncodedStream::H264Main;
            forceHardware = true;
        } else if (forcedEncoder == u"libx264_baseline") {
            qCWarning(PIPEWIRERECORD_LOGGING) << "Forcing H264 Software encoding, baseline profile";
            encoderType = PipeWireBaseEncodedStream::H264Baseline;
            forceSoftware = true;
        } else if (forcedEncoder == u"h264_vaapi_baseline") {
            qCWarning(PIPEWIRERECORD_LOGGING) << "Forcing H264 Hardware encoding, baseline profile";
            encoderType = PipeWireBaseEncodedStream::H264Baseline;
            forceHardware = true;
        }
    }

    auto size = m_stream->size();

    switch (encoderType) {
    case PipeWireBaseEncodedStream::H264Baseline:
    case PipeWireBaseEncodedStream::H264Main: {
        auto profile = m_encoderType == PipeWireBaseEncodedStream::H264Baseline ? Encoder::H264Profile::Baseline : Encoder::H264Profile::Main;

        if (!forceSoftware) {
            auto hardwareEncoder = std::make_unique<H264VAAPIEncoder>(profile, this);
            hardwareEncoder->setQuality(m_quality);
            if (hardwareEncoder->initialize(size)) {
                return std::move(hardwareEncoder);
            }
        }

        if (!forceHardware) {
            auto softwareEncoder = std::make_unique<LibX264Encoder>(profile, this);
            softwareEncoder->setQuality(m_quality);
            if (softwareEncoder->initialize(size)) {
                return std::move(softwareEncoder);
            }
        }
        break;
    }
    case PipeWireBaseEncodedStream::VP8: {
        if (!forceHardware) {
            auto encoder = std::make_unique<LibVpxEncoder>(this);
            encoder->setQuality(m_quality);
            if (encoder->initialize(size)) {
                return std::move(encoder);
            }
        }
        break;
    }
    default:
        qCWarning(PIPEWIRERECORD_LOGGING) << "Unknown encoder type" << m_encoderType;
    }

    return nullptr;
}

#include "moc_pipewireproduce_p.cpp"
