/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QObject>
#include <QDBusObjectPath>

class QTimer;
class QQmlApplicationEngine;

struct Stream {
    uint id;
    QVariantMap opts;
};

class OrgFreedesktopPortalScreenCastInterface;

class RecordMe : public QObject
{
    Q_OBJECT
public:
    RecordMe(QObject* parent = nullptr);
    ~RecordMe() override;

    enum CursorModes {
        Hidden = 1,
        Embedded = 2,
        Metadata = 4
    };
    Q_ENUM(CursorModes);

    enum SourceTypes {
        Monitor = 1,
        Window = 2
    };
    Q_ENUM(SourceTypes);

    void setDuration(int duration);

public Q_SLOTS:
    void response(uint code, const QVariantMap& results);

private:
    void init(const QDBusObjectPath &path);
    void handleStreams(const QVector<Stream> &streams);
    void start();

    OrgFreedesktopPortalScreenCastInterface *iface;
    QDBusObjectPath m_path;
    QTimer* const m_durationTimer;
    const QString m_handleToken;
    QQmlApplicationEngine* m_engine;
};
