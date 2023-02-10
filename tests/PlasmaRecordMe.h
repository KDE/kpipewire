/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QDBusObjectPath>
#include <QObject>
#include <QRect>
#include <screencasting.h>

class QQmlApplicationEngine;
class QTimer;
class QScreen;
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
    Q_PROPERTY(QRect region READ region NOTIFY regionChanged)
public:
    PlasmaRecordMe(Screencasting::CursorMode cursorMode, const QString &source, bool doSelection, QObject *parent = nullptr);
    ~PlasmaRecordMe() override;

    void setDuration(int duration);

    QRect region() const;
    Q_SCRIPTABLE void createVirtualMonitor();
    Q_SCRIPTABLE void requestSelection();
    Q_SCRIPTABLE void setRegionPressed(const QString &screen, int x, int y);
    Q_SCRIPTABLE void setRegionMoved(const QString &screen, int x, int y);
    Q_SCRIPTABLE void setRegionReleased(const QString &screen, int x, int y);

Q_SIGNALS:
    void cursorModeChanged(Screencasting::CursorMode cursorMode);
    void workspaceChanged();
    void regionFinal(const QRect &region);
    void regionChanged(const QRect &region);

private:
    void start(ScreencastingStream* stream);
    void addScreen(QScreen *screen);

    const Screencasting::CursorMode m_cursorMode;
    QTimer* const m_durationTimer;
    const QString m_sourceName;
    KWayland::Client::PlasmaWindowManagement* m_management = nullptr;
    Screencasting* m_screencasting = nullptr;
    QQmlApplicationEngine* m_engine;
    ScreencastingStream *m_workspaceStream = nullptr;
    ScreencastingStream *m_regionStream = nullptr;
    QRect m_workspace;

    QRect m_region;
};
