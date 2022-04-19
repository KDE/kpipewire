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

#include "PlasmaRecordMe.h"
#include "libkpipewire/screencasting.h"
#include <QDir>
#include <QLoggingCategory>
#include <QTimer>
#include <QCoreApplication>
#include <QThread>
#include <QProcess>
#include <QRect>
#include <QQuickView>
#include <QQuickItem>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QRegularExpression>

#include <KWayland/Client/event_queue.h>
#include <KWayland/Client/connection_thread.h>
#include <KWayland/Client/output.h>
#include <KWayland/Client/xdgoutput.h>
#include <KWayland/Client/registry.h>
#include <KWayland/Client/plasmawindowmanagement.h>
#include "libkpipewire/kpipewiredeclarativeplugin.h"
#include <kpipewirerecorddeclarativeplugin.h>

using namespace KWayland::Client;

PlasmaRecordMe::PlasmaRecordMe(const QString &source, QObject* parent)
    : QObject(parent)
    , m_durationTimer(new QTimer(this))
    , m_sourceName(source)
    , m_engine(new QQmlApplicationEngine(this))
{
    auto plugin = new KPipewireDeclarativePlugin;
    plugin->setParent(this);
    plugin->registerTypes("org.kde.pipewire");

    auto pluginrec = new KPipewireRecordDeclarativePlugin;
    pluginrec->setParent(this);
    pluginrec->registerTypes("org.kde.pipewire.record");

    m_engine->setInitialProperties({
        { QStringLiteral("app"), QVariant::fromValue<QObject *>(this) },
    });
    m_engine->load(QUrl("qrc:/main.qml"));

    auto connection = ConnectionThread::fromApplication(this);
    if (!connection) {
        qWarning() << "Failed getting Wayland connection from QPA";
        QCoreApplication::exit(1);
        return;
    }

    m_durationTimer->setSingleShot(true);
    auto registry = new Registry(qApp);
    connect(registry, &KWayland::Client::Registry::plasmaWindowManagementAnnounced, this, [this, registry] (quint32 name, quint32 version) {
        m_management = registry->createPlasmaWindowManagement(name, version, this);
        auto addWindow = [this] (KWayland::Client::PlasmaWindow *window) {
            const QRegularExpression rx(m_sourceName);
            const auto match = rx.match(window->appId());
            if (match.hasMatch()) {
                auto f = [this, window] {
                    start(m_screencasting->createWindowStream(window, m_cursorMode));
                };
                qDebug() << "window" << window << window->uuid() << m_sourceName << m_screencasting;
                if (m_screencasting)
                    f();
                else
                    m_delayed << f;
            }
        };
        for (auto w : m_management->windows())
            addWindow(w);
        connect(m_management, &KWayland::Client::PlasmaWindowManagement::windowCreated, this, addWindow);
    });

    connect(registry, &KWayland::Client::Registry::outputAnnounced, this, [this, registry] (quint32 name, quint32 version) {
            auto output = new KWayland::Client::Output(this);
            output->setup(registry->bindOutput(name, version));

            connect(output, &Output::changed, this, [this, output] {
                auto xdgOutput = m_xdgOutputManager->getXdgOutput(output);
                connect(xdgOutput, &XdgOutput::changed, this, [this, xdgOutput] {
                    m_workspace |= QRect {xdgOutput->logicalPosition(), xdgOutput->logicalSize()};
                    Q_EMIT workspaceChanged();
                });

                const QRegularExpression rx(m_sourceName);
                const auto match = rx.match(output->model());
                if (match.hasMatch()) {
                    auto f = [this, output] {
                        start(m_screencasting->createOutputStream(output, m_cursorMode));
                    };
                    connect(this, &PlasmaRecordMe::cursorModeChanged, output, f);
                    if (m_screencasting)
                        f();
                    else
                        m_delayed << f;
                }
            });
    });
    connect(this, &PlasmaRecordMe::workspaceChanged, this, [this] {
        delete m_workspaceStream;
        m_workspaceStream = m_screencasting->createRegionStream(m_workspace, 1, m_cursorMode);
        start(m_workspaceStream);
    });
    connect(registry, &KWayland::Client::Registry::interfacesAnnounced, this, [this, registry] {
        const auto xdgOMData = registry->interface(Registry::Interface::XdgOutputUnstableV1);
        m_xdgOutputManager = registry->createXdgOutputManager(xdgOMData.name, xdgOMData.version);
    });

    registry->create(connection);
    registry->setup();

    m_screencasting = new Screencasting(this);

    bool ok = false;
    auto node = m_sourceName.toInt(&ok);
    if (ok) {
        const auto roots = m_engine->rootObjects();
        for (auto root : roots) {
            auto mo = root->metaObject();
            mo->invokeMethod(root, "addStream", Q_ARG(QVariant, QVariant::fromValue<int>(node)), Q_ARG(QVariant, QStringLiteral("raw node %1").arg(node)));
        }
    }
}

PlasmaRecordMe::~PlasmaRecordMe()
{
}

void PlasmaRecordMe::start(ScreencastingStream *stream)
{
    qDebug() << "start" << stream;
    connect(stream, &ScreencastingStream::failed, this, [this] (const QString &error) {
        qWarning() << "stream failed" << error;const auto roots = m_engine->rootObjects();
        for (auto root : roots) {
            auto mo = root->metaObject();
            mo->invokeMethod(root, "showPassiveNotification", Qt::QueuedConnection, Q_ARG(QVariant, QVariant(error))
                                                                                  , Q_ARG(QVariant, {})
                                                                                  , Q_ARG(QVariant, {})
                                                                                  , Q_ARG(QVariant, {})
            );
        }
    });
    connect(stream, &ScreencastingStream::closed, this, [this, stream] {
        auto nodeId = stream->property("nodeid").toInt();
        qDebug() << "bye bye" << stream << nodeId;

        const auto roots = m_engine->rootObjects();
        for (auto root : roots) {
            auto mo = root->metaObject();
            mo->invokeMethod(root, "removeStream", Qt::QueuedConnection, Q_ARG(QVariant, QVariant::fromValue<quint32>(nodeId)));
        }
    });
    connect(stream, &ScreencastingStream::created, this, [this, stream] (quint32 nodeId)
        {
            stream->setProperty("nodeid", nodeId);
            qDebug() << "starting..." << nodeId;
            const auto roots = m_engine->rootObjects();
            for (auto root : roots) {
                auto mo = root->metaObject();
                mo->invokeMethod(root, "addStream", Q_ARG(QVariant, QVariant::fromValue<quint32>(nodeId)), Q_ARG(QVariant, stream->objectName()));
            }
        }
    );
    connect(this, &PlasmaRecordMe::cursorModeChanged, stream, &ScreencastingStream::closed);
}

void PlasmaRecordMe::setDuration(int duration)
{
    m_durationTimer->setInterval(duration);
}

void PlasmaRecordMe::setCursorMode(Screencasting::CursorMode mode)
{
    m_cursorMode = mode;
    Q_EMIT cursorModeChanged(mode);
}

void PlasmaRecordMe::createVirtualMonitor()
{
    m_screencasting->createVirtualMonitorStream("recordme", {200, 200}, 1, m_cursorMode);
}
