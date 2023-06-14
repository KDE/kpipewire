/*
    SPDX-FileCopyrightText: 2022-2023 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "pipewirebaseencodedstream.h"
#include "pipewirerecord_p.h"
#include <logging_libav.h>
#include <logging_record.h>
#include <memory>

extern "C" {
#include <libavcodec/codec.h>
#include <libavutil/log.h>
}
#include <unistd.h>

#include "vaapiutils_p.h"

struct PipeWireEncodedStreamPrivate {
    uint m_nodeId = 0;
    std::optional<uint> m_fd;
    std::optional<Fraction> m_maxFramerate;
    bool m_active = false;
    PipeWireBaseEncodedStream::Encoder m_encoder;

    std::unique_ptr<QThread> m_produceThread;
    PipeWireProduce *m_produce;
};

PipeWireBaseEncodedStream::State PipeWireBaseEncodedStream::state() const
{
    if (isActive()) {
        return Recording;
    }
    // else if (d->m_recordThread || !d->m_produceThreadFinished)
    //  return Rendering;

    return Idle;
}

PipeWireBaseEncodedStream::PipeWireBaseEncodedStream(QObject *parent)
    : QObject(parent)
    , d(new PipeWireEncodedStreamPrivate)
{
    d->m_encoder = Encoder::VP8;

    const auto &category = PIPEWIRELIBAV_LOGGING();
    if (category.isDebugEnabled()) {
        av_log_set_level(AV_LOG_DEBUG);
    } else if (category.isInfoEnabled()) {
        av_log_set_level(AV_LOG_INFO);
    } else if (category.isWarningEnabled()) {
        av_log_set_level(AV_LOG_WARNING);
    } else {
        av_log_set_level(AV_LOG_ERROR);
    }
}

PipeWireBaseEncodedStream::~PipeWireBaseEncodedStream()
{
    setActive(false);

    if (d->m_fd) {
        close(*d->m_fd);
    }
}

void PipeWireBaseEncodedStream::setNodeId(uint nodeId)
{
    if (nodeId == d->m_nodeId)
        return;

    d->m_nodeId = nodeId;
    refresh();
    Q_EMIT nodeIdChanged(nodeId);
}

void PipeWireBaseEncodedStream::setFd(uint fd)
{
    if (fd == d->m_fd)
        return;

    if (d->m_fd) {
        close(*d->m_fd);
    }
    d->m_fd = fd;
    refresh();
    Q_EMIT fdChanged(fd);
}

Fraction PipeWireBaseEncodedStream::maxFramerate() const
{
    if (d->m_maxFramerate) {
        return d->m_maxFramerate.value();
    }
    return Fraction{60, 1};
}

void PipeWireBaseEncodedStream::setMaxFramerate(const Fraction &framerate)
{
    if (d->m_maxFramerate.has_value() && d->m_maxFramerate.value().numerator == framerate.numerator
        && d->m_maxFramerate.value().denominator == framerate.denominator) {
        return;
    }
    d->m_maxFramerate = framerate;
    Q_EMIT maxFramerateChanged();
}

void PipeWireBaseEncodedStream::setActive(bool active)
{
    if (d->m_active == active)
        return;

    d->m_active = active;
    refresh();
    Q_EMIT activeChanged(active);
}

void PipeWireBaseEncodedStream::refresh()
{
    if (d->m_active && d->m_nodeId > 0) {
        d->m_produceThread = std::make_unique<QThread>();
        d->m_produce = makeProduce();
        d->m_produce->moveToThread(d->m_produceThread.get());
        d->m_produceThread->start();
        QMetaObject::invokeMethod(d->m_produce, &PipeWireProduce::initialize, Qt::QueuedConnection);
    } else if (d->m_produceThread) {
        d->m_produce->deactivate();
        d->m_produceThread->exit();
        d->m_produceThread->wait();
    }

    Q_EMIT stateChanged();
}

void PipeWireBaseEncodedStream::setEncoder(Encoder encoder)
{
    if (d->m_encoder == encoder) {
        return;
    }
    d->m_encoder = encoder;
    Q_EMIT encoderChanged();
}

PipeWireBaseEncodedStream::Encoder PipeWireBaseEncodedStream::encoder() const
{
    return d->m_encoder;
}

QList<PipeWireBaseEncodedStream::Encoder> PipeWireBaseEncodedStream::suggestedEncoders() const
{
    VaapiUtils vaapi;

    QList<PipeWireBaseEncodedStream::Encoder> ret = {PipeWireBaseEncodedStream::VP8,
                                                     PipeWireBaseEncodedStream::H264Main,
                                                     PipeWireBaseEncodedStream::H264Baseline};
    std::remove_if(ret.begin(), ret.end(), [&vaapi](PipeWireBaseEncodedStream::Encoder &encoder) {
        switch (encoder) {
        case PipeWireBaseEncodedStream::VP8:
            if (vaapi.supportsProfile(VAProfileVP8Version0_3) && avcodec_find_encoder_by_name("vp8_vaapi")) {
                return false;
            } else {
                return !avcodec_find_encoder_by_name("libvpx");
            }
        case PipeWireBaseEncodedStream::H264Main:
        case PipeWireBaseEncodedStream::H264Baseline:
            if (vaapi.supportsProfile(encoder == PipeWireBaseEncodedStream::H264Main ? VAProfileH264Main : VAProfileH264ConstrainedBaseline)
                && avcodec_find_encoder_by_name("h264_vaapi")) {
                return false;
            } else {
                return !avcodec_find_encoder_by_name("libx264");
            }
        default:
            return true;
        }
    });
    return ret;
}

bool PipeWireBaseEncodedStream::isActive() const
{
    return d->m_active;
}

uint PipeWireBaseEncodedStream::nodeId() const
{
    return d->m_nodeId;
}

uint PipeWireBaseEncodedStream::fd() const
{
    return d->m_fd.value_or(0);
}
