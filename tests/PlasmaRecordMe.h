/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QObject>
#include <QDBusObjectPath>
#include <QRect>
#include <screencasting.h>

class QQmlApplicationEngine;
class QTimer;
class Screencasting;

namespace KWayland {
    namespace Client {
        class Registry;
        class PlasmaWindowManagement;
        class Output;
        class XdgOutputManager;
    }
}
class ScreencastingStream;
class OrgFreedesktopPortalScreenCastInterface;

class PlasmaRecordMe : public QObject
{
    Q_OBJECT
    Q_PROPERTY(Screencasting::CursorMode cursorMode READ cursorMode WRITE setCursorMode NOTIFY cursorModeChanged)
public:
    PlasmaRecordMe(const QString &source, QObject* parent = nullptr);
    ~PlasmaRecordMe() override;

    void setDuration(int duration);

    Screencasting::CursorMode cursorMode() const { return m_cursorMode; }
    void setCursorMode(Screencasting::CursorMode mode);

    Q_SCRIPTABLE void createVirtualMonitor();

Q_SIGNALS:
    void cursorModeChanged(Screencasting::CursorMode cursorMode);
    void workspaceChanged();

private:
    void start(ScreencastingStream* stream);

    Screencasting::CursorMode m_cursorMode = Screencasting::Hidden;
    QTimer* const m_durationTimer;
    const QString m_sourceName;
    QVector<std::function<void()>> m_delayed;
    KWayland::Client::PlasmaWindowManagement* m_management = nullptr;
    Screencasting* m_screencasting = nullptr;
    QQmlApplicationEngine* m_engine;
    ScreencastingStream *m_workspaceStream = nullptr;
    QRect m_workspace;
    KWayland::Client::XdgOutputManager *m_xdgOutputManager = nullptr;
};
