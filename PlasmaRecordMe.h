/*
 * App To Record systems
 * Copyright 2020 Aleix Pol Gonzalez <aleixpol@kde.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License or (at your option) version 3 or any later version
 * accepted by the membership of KDE e.V. (or its successor approved
 * by the membership of KDE e.V.), which shall act as a proxy
 * defined in Section 14 of version 3 of the license.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <QObject>
#include <QDBusObjectPath>
#include <QRect>
#include "libkpipewire/screencasting.h"

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
