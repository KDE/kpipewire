/*
    SPDX-FileCopyrightText: 2020 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "screencastingrequest.h"
#include <KWayland/Client/connection_thread.h>
#include <KWayland/Client/registry.h>
#include <QCoreApplication>
#include <QDebug>
#include <QPointer>
#include <functional>

ScreencastingRequest::ScreencastingRequest(QObject *parent)
    : QObject(parent)
{
}

ScreencastingRequest::~ScreencastingRequest() = default;

quint32 ScreencastingRequest::nodeId() const
{
    return m_nodeId;
}

void ScreencastingRequest::setUuid(const QString &uuid)
{
    if (m_uuid == uuid) {
        return;
    }

    setNodeid(0);
    m_uuid = uuid;
    if (!m_uuid.isEmpty()) {
        auto screencasting = new Screencasting(this);
        auto stream = screencasting->createWindowStream(m_uuid, Screencasting::CursorMode::Hidden);
        stream->setObjectName(m_uuid);
    }

    Q_EMIT uuidChanged(uuid);
}

void ScreencastingRequest::setOutputName(const QString &outputName)
{
    if (m_outputName == outputName) {
        return;
    }

    setNodeid(0);
    m_outputName = outputName;
    if (!m_outputName.isEmpty()) {
        auto screencasting = new Screencasting(this);
        auto stream = screencasting->createOutputStream(m_outputName, Screencasting::CursorMode::Hidden);
        stream->setObjectName(m_outputName);
    }

    Q_EMIT outputNameChanged(outputName);
}

void ScreencastingRequest::adopt(ScreencastingStream *stream)
{
    connect(this, &ScreencastingRequest::uuidChanged, stream, &QObject::deleteLater);
    connect(stream, &ScreencastingStream::created, this, &ScreencastingRequest::setNodeid);
    connect(stream, &ScreencastingStream::failed, this, [](const QString &error) {
        qWarning() << "error creating screencast" << error;
    });
    connect(stream, &ScreencastingStream::closed, this, [this, stream] {
        if (stream->nodeId() == m_nodeId) {
            setNodeid(0);
        }
    });
}

void ScreencastingRequest::setNodeid(uint nodeId)
{
    if (nodeId == m_nodeId) {
        return;
    }

    m_nodeId = nodeId;
    Q_EMIT nodeIdChanged(nodeId);
}

QString ScreencastingRequest::uuid() const
{
    return m_uuid;
}

QString ScreencastingRequest::outputName() const
{
    return m_outputName;
}
