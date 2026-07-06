/*
    SPDX-FileCopyrightText: 2023 Fushan Wen <qydwhotmail@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QAbstractListModel>
#include <QQmlParserStatus>
#include <QSharedPointer>
#include <QTimer>
#include <qqmlregistration.h>

#include <kpipewiremonitor_export.h>

class PipeWireCore;
struct MediaMonitorPrivate;

namespace MediaRole
{
Q_NAMESPACE
QML_ELEMENT
// Match PW_KEY_MEDIA_ROLE
enum Role : int {
    Unknown = -1,
    Movie,
    Music,
    Camera,
    Screen,
    Communication,
    Game,
    Notification,
    DSP,
    Production,
    Accessibility,
    Test,
    Last = Test,
};
Q_ENUM_NS(Role)
}

namespace NodeState
{
Q_NAMESPACE
QML_ELEMENT
// Match enum pw_node_state
enum State : int {
    Error = -1, /**< error state */
    Creating = 0, /**< the node is being created */
    Suspended = 1, /**< the node is suspended, the device might be closed */
    Idle = 2, /**< the node is running but there is no active port */
    Running = 3, /**< the node is running */
};
Q_ENUM_NS(State)
}

class KPIPEWIREMONITOR_EXPORT MediaMonitor : public QAbstractListModel, public QQmlParserStatus
{
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    QML_ELEMENT

    /**
     * Role for media streams. Only media streams with their @p PW_KEY_MEDIA_ROLE matching @p role will be monitored.
     * @default MediaRole::Unknown
     */
    Q_PROPERTY(MediaRole::Role role READ role WRITE setRole NOTIFY roleChanged)

    /**
     * Whether this monitor is able to detect apps using Pipewire for media access
     */
    Q_PROPERTY(bool detectionAvailable READ detectionAvailable NOTIFY detectionAvailableChanged)

    /**
     * Total count of media streams with the given role on the system
     */
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

    /**
     * The number of media streams that are currently used by applications
     */
    Q_PROPERTY(int runningCount READ runningCount NOTIFY runningCountChanged)

    /**
     * The number of media streams that are in idle state.
     */
    Q_PROPERTY(int idleCount READ idleCount NOTIFY idleCountChanged)

public:
    enum Role {
        StateRole = Qt::UserRole + 1,
        ObjectSerialRole,
    };
    Q_ENUM(Role);

    explicit MediaMonitor(QObject *parent = nullptr);
    ~MediaMonitor() override;

    QVariant data(const QModelIndex &index, int role) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QHash<int, QByteArray> roleNames() const override;

    MediaRole::Role role() const;
    void setRole(MediaRole::Role newRole);

    bool detectionAvailable() const;
    int runningCount() const;
    int idleCount() const;

Q_SIGNALS:
    void roleChanged();
    void detectionAvailableChanged();
    void countChanged();
    void runningCountChanged();
    void idleCountChanged();

private:
    void classBegin() override;
    void componentComplete() override;
    void onPipeBroken();

    std::unique_ptr<MediaMonitorPrivate> d;
    friend struct MediaMonitorPrivate;
};
