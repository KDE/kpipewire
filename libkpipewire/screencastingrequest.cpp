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

    Q_EMIT closeRunningStreams();
    setNodeid(0);

    m_uuid = uuid;
    if (!m_uuid.isEmpty()) {
        create(new Screencasting(this));
    }

    Q_EMIT uuidChanged(uuid);
}

void ScreencastingRequest::setNodeid(uint nodeId)
{
    if (nodeId == m_nodeId) {
        return;
    }

    m_nodeId = nodeId;
    Q_EMIT nodeIdChanged(nodeId);
}

void ScreencastingRequest::create(Screencasting *screencasting)
{
    auto stream = screencasting->createWindowStream(m_uuid, Screencasting::CursorMode::Hidden);
    stream->setObjectName(m_uuid);

    connect(stream, &ScreencastingStream::created, this, [stream, this](int nodeId) {
        if (stream->objectName() == m_uuid) {
            setNodeid(nodeId);
        }
    });
    connect(stream, &ScreencastingStream::failed, this, [](const QString &error) {
        qWarning() << "error creating screencast" << error;
    });
    connect(stream, &ScreencastingStream::closed, this, [this, stream] {
        if (stream->nodeId() == m_nodeId) {
            setNodeid(0);
        }
    });
    connect(this, &ScreencastingRequest::closeRunningStreams, stream, &QObject::deleteLater);
}

QString ScreencastingRequest::uuid() const
{
    return m_uuid;
}
