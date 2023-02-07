/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "PlasmaRecordMe.h"
#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickView>
#include <QRegularExpression>
#include <QScreen>
#include <QThread>
#include <QTimer>

#include <KWayland/Client/event_queue.h>
#include <KWayland/Client/connection_thread.h>
#include <KWayland/Client/output.h>
#include <KWayland/Client/xdgoutput.h>
#include <KWayland/Client/registry.h>
#include <KWayland/Client/plasmawindowmanagement.h>

using namespace KWayland::Client;

PlasmaRecordMe::PlasmaRecordMe(Screencasting::CursorMode cursorMode, const QString &source, QObject *parent)
    : QObject(parent)
    , m_cursorMode(cursorMode)
    , m_durationTimer(new QTimer(this))
    , m_sourceName(source)
    , m_engine(new QQmlApplicationEngine(this))
{
    m_engine->setInitialProperties({
        { QStringLiteral("app"), QVariant::fromValue<QObject *>(this) },
    });
    m_engine->load(QUrl(QStringLiteral("qrc:/main.qml")));

    m_connection = ConnectionThread::fromApplication(this);
    if (!m_connection) {
        m_connectionThread = new QThread();
        m_connection = new KWayland::Client::ConnectionThread();
        m_connection->moveToThread(m_connectionThread);
        m_eventQueue = new EventQueue(this);
        m_connectionThread->start();
        m_connection->initConnection();
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

    connect(registry, &Registry::interfaceAnnounced, this, [this, registry](const QByteArray &interface, quint32 name, quint32 version) {
        if (interface != QByteArrayLiteral("zkde_screencast_unstable_v1")) {
            return;
        }
        m_screencasting->bind(*registry, name, version);
    });

    // if (m_sourceName.isEmpty() || m_sourceName == QLatin1String("region")) {
    //     connect(this, &PlasmaRecordMe::workspaceChanged, this, [this] {
    //         delete m_workspaceStream;
    //         m_workspaceStream = m_screencasting->createRegionStream(m_workspace, 1, m_cursorMode);
    //         start(m_workspaceStream);
    //     });
    // }

    connect(m_connection, &ConnectionThread::connected, this, [this, registry] {
        registry->create(m_connection);
        registry->setEventQueue(m_eventQueue);
        registry->setup();
    });

    m_screencasting = new Screencasting(this);

    // bool ok = false;
    // auto node = m_sourceName.toInt(&ok);
    // if (ok) {
    //     const auto roots = m_engine->rootObjects();
    //     for (auto root : roots) {
    //         auto mo = root->metaObject();
    //         mo->invokeMethod(root,
    //                          "addStream",
    //                          Q_ARG(QVariant, QVariant::fromValue<int>(node)),
    //                          Q_ARG(QVariant, QStringLiteral("raw node %1").arg(node)),
    //                          Q_ARG(QVariant, 0));
    //     }
    // }

    for (auto screen : qGuiApp->screens()) {
        m_workspace |= screen->geometry();
    }
    Q_EMIT workspaceChanged();
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
                mo->invokeMethod(root,
                                 "addStream",
                                 Q_ARG(QVariant, QVariant::fromValue<quint32>(nodeId)),
                                 Q_ARG(QVariant, stream->objectName()),
                                 Q_ARG(QVariant, 0));
            }
        }
    );
    connect(this, &PlasmaRecordMe::cursorModeChanged, stream, &ScreencastingStream::closed);
}

void PlasmaRecordMe::setDuration(int duration)
{
    m_durationTimer->setInterval(duration);
}

void PlasmaRecordMe::createVirtualMonitor()
{
    m_screencasting->createVirtualMonitorStream(QStringLiteral("recordme"), {1920, 1080}, 1, m_cursorMode);
}
