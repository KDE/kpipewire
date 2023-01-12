/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include <QCommandLineParser>
#include <QDebug>
#include <QGuiApplication>
#include <QScreen>

#include "screencasting.h"
#include "xdp_dbus_remotedesktop_interface.h"
#include "xdp_dbus_screencast_interface.h"
#include <DmaBufHandler>
#include <PipeWireSourceStream>
#include <QDBusArgument>
#include <unistd.h>

static QString createHandleToken()
{
    return QStringLiteral("kpipewireheadlesstest%1").arg(QRandomGenerator::global()->generate());
}

void checkPlasmaScreens()
{
    auto screencasting = new Screencasting(qGuiApp);
    for (auto screen : qGuiApp->screens()) {
        auto stream = screencasting->createOutputStream(screen->name(), Screencasting::Embedded);
        QObject::connect(stream, &ScreencastingStream::created, qGuiApp, [stream] {
            auto pwStream = new PipeWireSourceStream(qGuiApp);
            if (!pwStream->createStream(stream->nodeId(), 0)) {
                qWarning() << "failed!" << pwStream->error();
                exit(1);
            }

            auto handler = std::make_shared<DmaBufHandler>();
            QObject::connect(pwStream, &PipeWireSourceStream::frameReceived, qGuiApp, [handler, pwStream](const PipeWireFrame &frame) {
                QImage qimage(pwStream->size(), QImage::Format_RGBA8888);
                if (!handler->downloadFrame(qimage, frame)) {
                    qDebug() << "failed to download frame";
                    pwStream->renegotiateModifierFailed(frame.format, frame.dmabuf->modifier);
                } else {
                    qDebug() << ".";
                }
            });
        });
    }
}

void checkPlasmaWorkspace()
{
    auto screencasting = new Screencasting(qGuiApp);
    QRegion region;
    for (auto screen : qGuiApp->screens()) {
        region |= screen->geometry();
    }
    auto stream = screencasting->createRegionStream(region.boundingRect(), 1, Screencasting::Embedded);
    QObject::connect(stream, &ScreencastingStream::created, qGuiApp, [stream] {
        auto pwStream = new PipeWireSourceStream(qGuiApp);
        if (!pwStream->createStream(stream->nodeId(), 0)) {
            qWarning() << "failed!" << pwStream->error();
            exit(1);
        }

        auto handler = std::make_shared<DmaBufHandler>();
        QObject::connect(pwStream, &PipeWireSourceStream::frameReceived, qGuiApp, [handler, pwStream](const PipeWireFrame &frame) {
            QImage qimage(pwStream->size(), QImage::Format_RGBA8888);
            if (!handler->downloadFrame(qimage, frame)) {
                qDebug() << "failed to download frame";
                pwStream->renegotiateModifierFailed(frame.format, frame.dmabuf->modifier);
            } else {
                qDebug() << ".";
            }
        });
    });
}

using Stream = struct {
    uint nodeId;
    QVariantMap map;
};
using Streams = QList<Stream>;

Q_DECLARE_METATYPE(Stream);
Q_DECLARE_METATYPE(Streams);

const QDBusArgument &operator>>(const QDBusArgument &arg, Stream &stream)
{
    arg.beginStructure();
    arg >> stream.nodeId;

    arg.beginMap();
    while (!arg.atEnd()) {
        QString key;
        QVariant map;
        arg.beginMapEntry();
        arg >> key >> map;
        arg.endMapEntry();
        stream.map.insert(key, map);
    }
    arg.endMap();
    arg.endStructure();

    return arg;
}

class XdpScreenCast : public QObject
{
    Q_OBJECT
public:
    XdpScreenCast(QObject *parent)
        : QObject(parent)
    {
        initDbus();
    }

    void initDbus()
    {
        dbusXdpScreenCastService.reset(new OrgFreedesktopPortalScreenCastInterface(QStringLiteral("org.freedesktop.portal.Desktop"),
                                                                                   QStringLiteral("/org/freedesktop/portal/desktop"),
                                                                                   QDBusConnection::sessionBus()));

        qInfo() << "Initializing D-Bus connectivity with XDG Desktop Portal" << dbusXdpScreenCastService->version();
        Q_ASSERT(dbusXdpScreenCastService->isValid());

        // create session
        auto sessionParameters =
            QVariantMap{{QStringLiteral("session_handle_token"), createHandleToken()}, {QStringLiteral("handle_token"), createHandleToken()}};
        auto sessionReply = dbusXdpScreenCastService->CreateSession(sessionParameters);
        sessionReply.waitForFinished();
        if (!sessionReply.isValid()) {
            qWarning() << "Couldn't initialize XDP-KDE screencast session" << sessionReply.error();
            exit(1);
            return;
        }

        qInfo() << "DBus session created: " << sessionReply.value().path()
                << QDBusConnection::sessionBus().connect(QString(),
                                                         sessionReply.value().path(),
                                                         QStringLiteral("org.freedesktop.portal.Request"),
                                                         QStringLiteral("Response"),
                                                         this,
                                                         SLOT(handleSessionCreated(uint, QVariantMap)));
    }

public Q_SLOTS:
    void handleSessionCreated(quint32 code, const QVariantMap &results)
    {
        if (code != 0) {
            qWarning() << "Failed to create session: " << code;
            exit(1);
            return;
        }

        sessionPath = QDBusObjectPath(results.value(QStringLiteral("session_handle")).toString());

        // select sources for the session
        const QVariantMap sourcesParameters = {{QLatin1String("handle_token"), createHandleToken()},
                                               {QLatin1String("types"), dbusXdpScreenCastService->availableSourceTypes()},
                                               {QLatin1String("multiple"), false},
                                               {QLatin1String("cursor_mode"), uint(2 /*Embedded*/)}};
        auto selectorReply = dbusXdpScreenCastService->SelectSources(sessionPath, sourcesParameters);
        selectorReply.waitForFinished();
        if (!selectorReply.isValid()) {
            qWarning() << "Couldn't select devices for the remote-desktop session";
            exit(1);
            return;
        }
        QDBusConnection::sessionBus().connect(QString(),
                                              selectorReply.value().path(),
                                              QStringLiteral("org.freedesktop.portal.Request"),
                                              QStringLiteral("Response"),
                                              this,
                                              SLOT(handleSourcesSelected(uint, QVariantMap)));
    }

    void handleSourcesSelected(quint32 code, const QVariantMap &)
    {
        if (code != 0) {
            qWarning() << "Failed to select sources: " << code;
            exit(1);
            return;
        }

        // start session
        auto startParameters = QVariantMap{{QStringLiteral("handle_token"), createHandleToken()}};
        auto startReply = dbusXdpScreenCastService->Start(sessionPath, QString(), startParameters);
        startReply.waitForFinished();
        QDBusConnection::sessionBus().connect(QString(),
                                              startReply.value().path(),
                                              QStringLiteral("org.freedesktop.portal.Request"),
                                              QStringLiteral("Response"),
                                              this,
                                              SLOT(handleRemoteDesktopStarted(uint, QVariantMap)));
    }

    void handleRemoteDesktopStarted(quint32 code, const QVariantMap &results)
    {
        if (code != 0) {
            qWarning() << "Failed to start screencast: " << code;
            exit(1);
            return;
        }

        // there should be only one stream
        const Streams streams = qdbus_cast<Streams>(results.value(QStringLiteral("streams")));
        if (streams.isEmpty()) {
            // maybe we should check deeper with qdbus_cast but this suffices for now
            qWarning() << "Failed to get screencast streams";
            exit(1);
            return;
        }

        const QVariantMap startParameters = {
            { QLatin1String("handle_token"), createHandleToken() }
        };

        auto streamReply = dbusXdpScreenCastService->OpenPipeWireRemote(sessionPath, startParameters);
        streamReply.waitForFinished();
        if (!streamReply.isValid()) {
            qWarning() << "Couldn't open pipewire remote for the screen-casting session";
            exit(1);
            return;
        }

        auto pipewireFd = streamReply.value();
        if (!pipewireFd.isValid()) {
            qWarning() << "Couldn't get pipewire connection file descriptor";
            exit(1);
            return;
        }

        const int fd = pipewireFd.takeFileDescriptor();
        if (!stream.createStream(streams.first().nodeId, fd)) {
            qWarning() << "Couldn't create the pipewire stream";
            exit(1);
            return;
        }

        QObject::connect(&stream, &PipeWireSourceStream::frameReceived, this, [](const PipeWireFrame &frame) {
            qDebug() << "." << frame.format;
        });

        QObject::connect(&stream, &PipeWireSourceStream::stopStreaming, this, [fd] {
            close(fd);
        });
    }

private:
    QScopedPointer<OrgFreedesktopPortalScreenCastInterface> dbusXdpScreenCastService;
    QDBusObjectPath sessionPath;
    PipeWireSourceStream stream;
};

class XdpRemoteDesktop : public QObject
{
    Q_OBJECT
public:
    XdpRemoteDesktop(QObject *parent)
        : QObject(parent)
    {
        initDbus();
    }

    void initDbus()
    {
        dbusXdpScreenCastService.reset(new OrgFreedesktopPortalScreenCastInterface(QStringLiteral("org.freedesktop.portal.Desktop"),
                                                                                   QStringLiteral("/org/freedesktop/portal/desktop"),
                                                                                   QDBusConnection::sessionBus()));
        dbusXdpRemoteDesktopService.reset(new OrgFreedesktopPortalRemoteDesktopInterface(QStringLiteral("org.freedesktop.portal.Desktop"),
                                                                                         QStringLiteral("/org/freedesktop/portal/desktop"),
                                                                                         QDBusConnection::sessionBus()));

        qInfo() << "Initializing D-Bus connectivity with XDG Desktop Portal" << dbusXdpScreenCastService->version();
        Q_ASSERT(dbusXdpScreenCastService->isValid());
        Q_ASSERT(dbusXdpRemoteDesktopService->isValid());

        // create session
        auto sessionParameters =
            QVariantMap{{QStringLiteral("session_handle_token"), createHandleToken()}, {QStringLiteral("handle_token"), createHandleToken()}};
        auto sessionReply = dbusXdpRemoteDesktopService->CreateSession(sessionParameters);
        sessionReply.waitForFinished();
        if (!sessionReply.isValid()) {
            qWarning() << "Couldn't initialize XDP-KDE screencast session" << sessionReply.error();
            exit(1);
            return;
        }

        qInfo() << "DBus session created: " << sessionReply.value().path()
                << QDBusConnection::sessionBus().connect(QString(),
                                                         sessionReply.value().path(),
                                                         QStringLiteral("org.freedesktop.portal.Request"),
                                                         QStringLiteral("Response"),
                                                         this,
                                                         SLOT(handleSessionCreated(uint, QVariantMap)));
    }

public Q_SLOTS:
    void handleSessionCreated(quint32 code, const QVariantMap &results)
    {
        if (code != 0) {
            qWarning() << "Failed to create session: " << code;
            exit(1);
            return;
        }

        sessionPath = QDBusObjectPath(results.value(QStringLiteral("session_handle")).toString());

        // select sources for the session
        auto selectionOptions = QVariantMap{// We have to specify it's an uint, otherwise xdg-desktop-portal will not forward it to backend implementation
                                            {QStringLiteral("types"), QVariant::fromValue<uint>(7)}, // request all (KeyBoard, Pointer, TouchScreen)
                                            {QStringLiteral("handle_token"), createHandleToken()}};
        auto selectorReply = dbusXdpRemoteDesktopService->SelectDevices(sessionPath, selectionOptions);
        selectorReply.waitForFinished();
        if (!selectorReply.isValid()) {
            qWarning() << "Couldn't select devices for the remote-desktop session";
            exit(1);
            return;
        }
        QDBusConnection::sessionBus().connect(QString(),
                                              selectorReply.value().path(),
                                              QStringLiteral("org.freedesktop.portal.Request"),
                                              QStringLiteral("Response"),
                                              this,
                                              SLOT(handleDevicesSelected(uint, QVariantMap)));
    }

    void handleDevicesSelected(quint32 code, const QVariantMap &results)
    {
        Q_UNUSED(results)
        if (code != 0) {
            qWarning() << "Failed to select devices: " << code;
            exit(1);
            return;
        }

        // select sources for the session
        auto selectionOptions = QVariantMap{{QStringLiteral("types"), QVariant::fromValue<uint>(7)},
                                            {QStringLiteral("multiple"), false},
                                            {QStringLiteral("handle_token"), createHandleToken()}};
        auto selectorReply = dbusXdpScreenCastService->SelectSources(sessionPath, selectionOptions);
        selectorReply.waitForFinished();
        if (!selectorReply.isValid()) {
            qWarning() << "Couldn't select sources for the screen-casting session";
            exit(1);
            return;
        }
        QDBusConnection::sessionBus().connect(QString(),
                                              selectorReply.value().path(),
                                              QStringLiteral("org.freedesktop.portal.Request"),
                                              QStringLiteral("Response"),
                                              this,
                                              SLOT(handleSourcesSelected(uint, QVariantMap)));
    }

    void handleSourcesSelected(quint32 code, const QVariantMap &)
    {
        if (code != 0) {
            qWarning() << "Failed to select sources: " << code;
            exit(1);
            return;
        }

        // start session
        auto startParameters = QVariantMap{{QStringLiteral("handle_token"), createHandleToken()}};
        auto startReply = dbusXdpRemoteDesktopService->Start(sessionPath, QString(), startParameters);
        startReply.waitForFinished();
        QDBusConnection::sessionBus().connect(QString(),
                                              startReply.value().path(),
                                              QStringLiteral("org.freedesktop.portal.Request"),
                                              QStringLiteral("Response"),
                                              this,
                                              SLOT(handleRemoteDesktopStarted(uint, QVariantMap)));
    }

    void handleRemoteDesktopStarted(quint32 code, const QVariantMap &results)
    {
        if (code != 0) {
            qWarning() << "Failed to start screencast: " << code;
            exit(1);
            return;
        }

        if (results.value(QStringLiteral("devices")).toUInt() == 0) {
            qWarning() << "No devices were granted" << results;
            exit(1);
            return;
        }

        // there should be only one stream
        const Streams streams = qdbus_cast<Streams>(results.value(QStringLiteral("streams")));
        if (streams.isEmpty()) {
            // maybe we should check deeper with qdbus_cast but this suffices for now
            qWarning() << "Failed to get screencast streams";
            exit(1);
            return;
        }

        auto streamReply = dbusXdpScreenCastService->OpenPipeWireRemote(sessionPath, QVariantMap());
        streamReply.waitForFinished();
        if (!streamReply.isValid()) {
            qWarning() << "Couldn't open pipewire remote for the screen-casting session";
            exit(1);
            return;
        }

        auto pipewireFd = streamReply.value();
        if (!pipewireFd.isValid()) {
            qWarning() << "Couldn't get pipewire connection file descriptor";
            exit(1);
            return;
        }

        const uint fd = pipewireFd.takeFileDescriptor();
        if (!stream.createStream(streams.first().nodeId, fd)) {
            qWarning() << "Couldn't create the pipewire stream";
            exit(1);
            return;
        }

        QObject::connect(&stream, &PipeWireSourceStream::frameReceived, this, [](const PipeWireFrame &frame) {
            qDebug() << "." << frame.format;
        });

        QObject::connect(&stream, &PipeWireSourceStream::stopStreaming, this, [fd] {
            close(fd);
        });
    }

private:
    QScopedPointer<OrgFreedesktopPortalScreenCastInterface> dbusXdpScreenCastService;
    QScopedPointer<OrgFreedesktopPortalRemoteDesktopInterface> dbusXdpRemoteDesktopService;
    QDBusObjectPath sessionPath;
    PipeWireSourceStream stream;
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);

    {
        QCommandLineParser parser;
        QCommandLineOption useXdpRD(QStringLiteral("xdp-remotedesktop"), QStringLiteral("Uses the XDG Desktop Portal RemoteDesktop interface"));
        parser.addOption(useXdpRD);
        QCommandLineOption useXdpSC(QStringLiteral("xdp-screencast"), QStringLiteral("Uses the XDG Desktop Portal ScreenCast interface"));
        parser.addOption(useXdpSC);
        QCommandLineOption useWorkspace(QStringLiteral("workspace"), QStringLiteral("Uses the Plasma screencasting workspace feed"));
        parser.addOption(useWorkspace);
        parser.addHelpOption();
        parser.process(app);

        if (parser.isSet(useXdpRD)) {
            new XdpRemoteDesktop(&app);
        } else if (parser.isSet(useXdpSC)) {
            new XdpScreenCast(&app);
        } else if (parser.isSet(useWorkspace)) {
            checkPlasmaWorkspace();
        } else {
            checkPlasmaScreens();
        }
    }

    return app.exec();
}

#include "HeadlessTest.moc"
