/*
    SPDX-FileCopyrightText: 2022-2023 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "pipewirebaseencodedstream.h"
#include "pipewirerecord_p.h"
#include <logging_libav.h>
#include <logging_record.h>

extern "C" {
#include <libavutil/log.h>
}
#include <unistd.h>

PipeWireBaseEncodedStream::State PipeWireBaseEncodedStream::state() const
{
    if (isActive())
        return Recording;
    else if (d->m_recordThread || !d->m_produceThreadFinished)
        return Rendering;

    return Idle;
}

PipeWireBaseEncodedStream::PipeWireBaseEncodedStream(QObject *parent)
    : QObject(parent)
    , d(new PipeWireEncodedStreamPrivate)
{
    d->m_encoder = "libvpx";

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

    if (d->m_recordThread && d->m_recordThread->isRunning()) {
        d->m_recordThread->wait();
    }

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
        d->m_recordThread.reset(new PipeWireProduceThread(d->m_encoder, d->m_nodeId, d->m_fd.value_or(0), this));
        connect(d->m_recordThread.get(), &PipeWireProduceThread::errorFound, this, &PipeWireBaseEncodedStream::errorFound);
        connect(d->m_recordThread.get(), &PipeWireProduceThread::finished, this, [this] {
            setActive(false);
        });
        d->m_recordThread->start();
    } else if (d->m_recordThread) {
        d->m_recordThread->deactivate();

        connect(d->m_recordThread.get(), &PipeWireProduceThread::finished, this, [this] {
            qCDebug(PIPEWIRERECORD_LOGGING) << "produce thread finished" << d->m_recordThread.get();
            d->m_recordThread.reset();
            d->m_produceThreadFinished = true;
            Q_EMIT stateChanged();
        });
        d->m_produceThreadFinished = false;
    }
    Q_EMIT stateChanged();
}

void PipeWireBaseEncodedStream::setEncoder(const QByteArray &encoder)
{
    d->m_encoder = encoder;
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
