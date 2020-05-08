/*
 * App To Record systems using xdg-desktop-portal
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

#include "RecordMe.h"
#include "PipelineItem.h"
#include <QLoggingCategory>
#include <QTimer>

#include <gst/gst.h>
#include "gstpointer.h"
#include "xdp_dbus_screencast_interface.h"

Q_DECLARE_METATYPE(Stream)

QDebug operator<<(QDebug debug, const Stream& plug)
{
    QDebugStateSaver saver(debug);
    debug.nospace() << "Stream(id: " << plug.id << ", opts: " << plug.opts << ')';
    return debug;
}


const QDBusArgument &operator<<(const QDBusArgument &argument, const Stream &/*stream*/)
{
    argument.beginStructure();
//     argument << stream.id << stream.opts;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, Stream &stream)
{
    argument.beginStructure();
    argument >> stream.id >> stream.opts;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, QVector<Stream> &stream)
{
    argument.beginArray();
    while ( !argument.atEnd() ) {
        Stream element;
        argument >> element;
        stream.append( element );
    }
    argument.endArray();
    return argument;
}

RecordMe::RecordMe(QObject* parent)
    : QObject(parent)
    , iface(new OrgFreedesktopPortalScreenCastInterface(
        QLatin1String("org.freedesktop.portal.Desktop"), QLatin1String("/org/freedesktop/portal/desktop"), QDBusConnection::sessionBus(), this))
    , m_durationTimer(new QTimer(this))
    , m_handleToken(QStringLiteral("RecordMe%1").arg(QRandomGenerator::global()->generate()))
{
    // create session
    const auto sessionParameters = QVariantMap {
        { QLatin1String("session_handle_token"), m_handleToken },
        { QLatin1String("handle_token"), m_handleToken }
    };
    auto sessionReply = iface->CreateSession(sessionParameters);
    sessionReply.waitForFinished();
    if (!sessionReply.isValid()) {
        qWarning("Couldn't initialize the remote control session");
        exit(1);
        return;
    }

    const bool ret = QDBusConnection::sessionBus().connect(QString(),
                                                           sessionReply.value().path(),
                                                           QLatin1String("org.freedesktop.portal.Request"),
                                                           QLatin1String("Response"),
                                                           this,
                                                           SLOT(response(uint, QVariantMap)));
    if (!ret) {
        qWarning() << "failed to create session";
        exit(2);
        return;
    }

    qDBusRegisterMetaType<Stream>();
    qDBusRegisterMetaType<QVector<Stream>>();

    m_durationTimer->setSingleShot(true);
}

RecordMe::~RecordMe() = default;

void RecordMe::init(const QDBusObjectPath& path)
{
    m_path = path;
    {
        const QVariantMap sourcesParameters = {
            { QLatin1String("handle_token"), m_handleToken },
            { QLatin1String("types"), uint(Monitor|Window) },
            { QLatin1String("multiple"), false }, //for now?
//             { QLatin1String("cursor_mode"), uint(Embedded) }
        };

        auto reply = iface->SelectSources(m_path, sourcesParameters);
        reply.waitForFinished();

        if (reply.isError()) {
            qWarning() << "Could not select sources" << reply.error();
            exit(1);
            return;
        }
        qDebug() << "select sources done" << reply.value().path();
    }
}

void RecordMe::response(uint code, const QVariantMap& results)
{
    if (code > 0) {
        qWarning() << "error!!!" << results;
        exit(666);
        return;
    }

    const auto streamsIt = results.constFind("streams");
    if (streamsIt != results.constEnd()) {
        QVector<Stream> streams;
        streamsIt->value<QDBusArgument>() >> streams;

        qDebug() << "fuuuuuuu" << streams;
        handleStreams(streams);
        return;
    }

    const auto handleIt = results.constFind(QStringLiteral("session_handle"));
    if (handleIt != results.constEnd()) {
        init(QDBusObjectPath(handleIt->toString()));
        return;
    }

    qDebug() << "params" << results << code;
    if (results.isEmpty()) {
        start();
        return;
    }
}

void RecordMe::start()
{
    const QVariantMap startParameters = {
        { QLatin1String("handle_token"), m_handleToken }
    };

    auto reply = iface->Start(m_path, QStringLiteral("org.freedesktop.RecordMe"), startParameters);
    reply.waitForFinished();

    if (reply.isError()) {
        qWarning() << "Could not start stream" << reply.error();
        exit(1);
        return;
    }
    qDebug() << "started!" << reply.value().path();
}

void RecordMe::handleStreams(const QVector<Stream> &streams)
{
    if (streams.isEmpty())
        return;

    const QVariantMap startParameters = {
        { QLatin1String("handle_token"), m_handleToken }
    };

    auto reply = iface->OpenPipeWireRemote(m_path, startParameters);
    reply.waitForFinished();

    if (reply.isError()) {
        qWarning() << "Could not start stream" << reply.error();
        exit(1);
        return;
    }

    const int fd = reply.value().fileDescriptor();
    qDebug() << "feeding pipewire" << fd;

    auto pipe = new PipelineItem(/*fd, streams.constFirst().id, */nullptr);
    if (m_durationTimer->interval() > 0) {
        connect(m_durationTimer, &QTimer::timeout, pipe, &PipelineItem::stop);
        m_durationTimer->start();
    }
    pipe->setPlaying(true);
}

void RecordMe::setDuration(int duration)
{
    m_durationTimer->setInterval(duration);
}
