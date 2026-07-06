/*
    SPDX-FileCopyrightText: 2023 Collabora Ltd.
    SPDX-FileCopyrightText: 2023 Fushan Wen <qydwhotmail@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "mediamonitor.h"

#include <cstdlib>

#include <QDebug>
#include <QMetaEnum>

#include "pipewirecore_p.h"

#include <pipewire/pipewire.h>

namespace
{
struct Node {
    MediaMonitor *monitor;
    QString deviceName;
    QString objectSerial;
    NodeState::State state = NodeState::Error;
    spa_hook proxyListener;
    spa_hook objectListener;
};

void updateProp(const spa_dict *props, const char *key, QString &prop, int role, QList<int> &changedRoles)
{
    const char *new_prop = spa_dict_lookup(props, key);
    if (!new_prop) {
        return;
    }
    if (QString newProp = QString::fromUtf8(new_prop); prop != newProp) {
        prop = std::move(newProp);
        changedRoles << role;
    }
}
}

struct MediaMonitorPrivate {
    void connectToCore();

    struct ProxyDeleter {
        void operator()(pw_proxy *proxy) const
        {
            MediaMonitorPrivate::onProxyDestroy(pw_proxy_get_user_data(proxy));
            pw_proxy_destroy(proxy);
        }
    };

    static void onRegistryEventGlobal(void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version, const spa_dict *props);
    static void onRegistryEventGlobalRemove(void *data, uint32_t id);
    static void onProxyDestroy(void *data);
    static void onNodeEventInfo(void *data, const pw_node_info *info);

    static void readProps(const spa_dict *props, pw_proxy *proxy, bool emitSignal);

    void disconnectFromCore();
    void reconnectOnIdle();
    void updateState();

    static pw_registry_events s_pwRegistryEvents;
    static pw_proxy_events s_pwProxyEvents;
    static pw_node_events s_pwNodeEvents;

    MediaMonitor *const q;

    bool m_componentReady = true;
    MediaRole::Role m_role = MediaRole::Unknown;
    bool m_detectionAvailable = false;
    int m_runningCount = 0;
    int m_idleCount = 0;

    QSharedPointer<PipeWireCore> m_pwCore;
    pw_registry *m_registry = nullptr;
    spa_hook m_registryListener;
    std::vector<std::unique_ptr<pw_proxy, ProxyDeleter>> m_nodeList;
    QTimer m_reconnectTimer;

    bool m_inDestructor = false;
};

pw_registry_events MediaMonitorPrivate::s_pwRegistryEvents = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = &MediaMonitorPrivate::onRegistryEventGlobal,
    .global_remove = &MediaMonitorPrivate::onRegistryEventGlobalRemove,
};

pw_proxy_events MediaMonitorPrivate::s_pwProxyEvents = {
    .version = PW_VERSION_PROXY_EVENTS,
    .destroy = &MediaMonitorPrivate::onProxyDestroy,
};

pw_node_events MediaMonitorPrivate::s_pwNodeEvents = {
    .version = PW_VERSION_NODE_EVENTS,
    .info = &MediaMonitorPrivate::onNodeEventInfo,
};

MediaMonitor::MediaMonitor(QObject *parent)
    : QAbstractListModel(parent)
    , d(std::make_unique<MediaMonitorPrivate>(this))
{
    connect(this, &QAbstractListModel::rowsInserted, this, &MediaMonitor::countChanged);
    connect(this, &QAbstractListModel::rowsRemoved, this, &MediaMonitor::countChanged);
    connect(this, &QAbstractListModel::modelReset, this, &MediaMonitor::countChanged);

    d->m_reconnectTimer.setSingleShot(true);
    d->m_reconnectTimer.setInterval(5000);
    connect(&d->m_reconnectTimer, &QTimer::timeout, this, [this] {
        d->connectToCore();
    });
}

MediaMonitor::~MediaMonitor()
{
    d->m_inDestructor = true;
    d->disconnectFromCore();
}

QVariant MediaMonitor::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid)) {
        return {};
    }

    pw_proxy *const proxy = d->m_nodeList.at(index.row()).get();
    const Node *const node = static_cast<Node *>(pw_proxy_get_user_data(proxy));

    switch (role) {
    case Qt::DisplayRole:
        return node->deviceName;
    case StateRole:
        return node->state;
    case ObjectSerialRole:
        return node->objectSerial;
    default:
        return QVariant();
    }
}

int MediaMonitor::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : d->m_nodeList.size();
}

QHash<int, QByteArray> MediaMonitor::roleNames() const
{
    return {
        {Qt::DisplayRole, QByteArrayLiteral("display")},
        {StateRole, QByteArrayLiteral("state")},
        {ObjectSerialRole, QByteArrayLiteral("objectSerial")},
    };
}

MediaRole::Role MediaMonitor::role() const
{
    return d->m_role;
}

void MediaMonitor::setRole(MediaRole::Role newRole)
{
    if (d->m_role == newRole) {
        return;
    }
    Q_ASSERT(newRole >= MediaRole::Unknown && newRole <= MediaRole::Last);
    d->m_role = std::clamp(newRole, MediaRole::Unknown, MediaRole::Last);

    if (d->m_reconnectTimer.isActive()) {
        Q_EMIT roleChanged();
        return;
    }

    d->disconnectFromCore();
    d->connectToCore();

    Q_EMIT roleChanged();
}

bool MediaMonitor::detectionAvailable() const
{
    return d->m_detectionAvailable;
}

int MediaMonitor::runningCount() const
{
    return d->m_runningCount;
}

int MediaMonitor::idleCount() const
{
    return d->m_idleCount;
}

void MediaMonitorPrivate::connectToCore()
{
    Q_ASSERT(!m_registry);
    if (!m_componentReady || m_role == MediaRole::Unknown) {
        return;
    }

    if (!m_pwCore) {
        m_pwCore = PipeWireCore::fetch(0);
    }
    if (!m_pwCore->error().isEmpty()) {
        qDebug() << "received error while creating the stream" << m_pwCore->error() << "Media monitor will not work.";
        m_pwCore.clear();
        m_reconnectTimer.start();
        return;
    }

    m_registry = pw_core_get_registry(**m_pwCore.get(), PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(m_registry, &m_registryListener, &s_pwRegistryEvents, q /*user data*/);

    m_detectionAvailable = true;
    Q_EMIT q->detectionAvailableChanged();

    QObject::connect(m_pwCore.get(), &PipeWireCore::pipeBroken, q, &MediaMonitor::onPipeBroken);
}

void MediaMonitor::onPipeBroken()
{
    d->m_registry = nullptr; // When pipe is broken, the registered object is also gone
    d->disconnectFromCore();
    d->reconnectOnIdle();
}

void MediaMonitorPrivate::onRegistryEventGlobal(void *data,
                                                uint32_t id,
                                                uint32_t /*permissions*/,
                                                const char *type,
                                                uint32_t /*version*/,
                                                const spa_dict *props)
{
    auto monitor = static_cast<MediaMonitor *>(data);

    if (!props || !(spa_streq(type, PW_TYPE_INTERFACE_Node))) {
        return;
    }

    static const QMetaEnum metaEnum = QMetaEnum::fromType<MediaRole::Role>();
    if (const char *prop_str = spa_dict_lookup(props, PW_KEY_MEDIA_ROLE); !prop_str || (strcmp(prop_str, metaEnum.valueToKey(monitor->d->m_role)) != 0)) {
        return;
    }

    auto proxy = static_cast<pw_proxy *>(pw_registry_bind(monitor->d->m_registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, sizeof(Node)));
    auto node = static_cast<Node *>(pw_proxy_get_user_data(proxy));
    node->monitor = monitor;
    readProps(props, proxy, false);

    monitor->beginInsertRows(QModelIndex(), monitor->d->m_nodeList.size(), monitor->d->m_nodeList.size());
    monitor->d->m_nodeList.emplace_back(proxy);
    monitor->endInsertRows();

    pw_proxy_add_listener(proxy, &node->proxyListener, &s_pwProxyEvents, node);
    pw_proxy_add_object_listener(proxy, &node->objectListener, &s_pwNodeEvents, node);
}

void MediaMonitorPrivate::onRegistryEventGlobalRemove(void *data, uint32_t id)
{
    auto monitor = static_cast<MediaMonitor *>(data);
    const auto proxyIt = std::find_if(monitor->d->m_nodeList.cbegin(), monitor->d->m_nodeList.cend(), [id](const auto &proxy) {
        return pw_proxy_get_bound_id(proxy.get()) == id;
    });
    if (proxyIt == monitor->d->m_nodeList.cend()) {
        return;
    }
    const int row = std::distance(monitor->d->m_nodeList.cbegin(), proxyIt);
    monitor->beginRemoveRows(QModelIndex(), row, row);
    monitor->d->m_nodeList.erase(proxyIt);
    monitor->endRemoveRows();
}

void MediaMonitorPrivate::onProxyDestroy(void *data)
{
    auto node = static_cast<Node *>(data);
    spa_hook_remove(&node->proxyListener);
    spa_hook_remove(&node->objectListener);
}

void MediaMonitorPrivate::onNodeEventInfo(void *data, const pw_node_info *info)
{
    auto node = static_cast<Node *>(data);

    NodeState::State newState;
    switch (info->state) {
    case PW_NODE_STATE_ERROR:
        newState = NodeState::Error;
        break;
    case PW_NODE_STATE_CREATING:
        newState = NodeState::Creating;
        break;
    case PW_NODE_STATE_SUSPENDED:
        newState = NodeState::Suspended;
        break;
    case PW_NODE_STATE_IDLE:
        newState = NodeState::Idle;
        break;
    case PW_NODE_STATE_RUNNING:
        newState = NodeState::Running;
        break;
    default:
        Q_ASSERT_X(false, "MediaMonitor", "Unknown node state");
        return;
    }

    const auto proxyIt = std::find_if(node->monitor->d->m_nodeList.cbegin(), node->monitor->d->m_nodeList.cend(), [data](const auto &proxy) {
        return pw_proxy_get_user_data(proxy.get()) == data;
    });
    if (node->state != newState) {
        node->state = newState;
        const int row = std::distance(node->monitor->d->m_nodeList.cbegin(), proxyIt);
        const QModelIndex idx = node->monitor->index(row, 0);
        node->monitor->dataChanged(idx, idx, {MediaMonitor::StateRole});
    }

    readProps(info->props, proxyIt->get(), true);
    node->monitor->d->updateState();
}

void MediaMonitorPrivate::readProps(const spa_dict *props, pw_proxy *proxy, bool emitSignal)
{
    auto node = static_cast<Node *>(pw_proxy_get_user_data(proxy));
    QList<int> changedRoles;

    updateProp(props, PW_KEY_NODE_NICK, node->deviceName, Qt::DisplayRole, changedRoles);
    if (node->deviceName.isEmpty()) {
        changedRoles.clear();
        updateProp(props, PW_KEY_NODE_NAME, node->deviceName, Qt::DisplayRole, changedRoles);
    }
    if (node->deviceName.isEmpty()) {
        changedRoles.clear();
        updateProp(props, PW_KEY_NODE_DESCRIPTION, node->deviceName, Qt::DisplayRole, changedRoles);
    }

    updateProp(props, PW_KEY_OBJECT_SERIAL, node->objectSerial, MediaMonitor::ObjectSerialRole, changedRoles);

    if (emitSignal && !changedRoles.empty()) {
        const auto proxyIt = std::find_if(node->monitor->d->m_nodeList.cbegin(), node->monitor->d->m_nodeList.cend(), [proxy](const auto &p) {
            return p.get() == proxy;
        });
        const int row = std::distance(node->monitor->d->m_nodeList.cbegin(), proxyIt);
        const QModelIndex idx = node->monitor->index(row, 0);
        node->monitor->dataChanged(idx, idx, changedRoles);
    }
}

void MediaMonitor::classBegin()
{
    d->m_componentReady = false;
}

void MediaMonitor::componentComplete()
{
    d->m_componentReady = true;
    d->connectToCore();
}

void MediaMonitorPrivate::disconnectFromCore()
{
    if (!m_pwCore) {
        return;
    }

    if (m_runningCount) {
        m_runningCount = 0;
        Q_EMIT q->runningCountChanged();
    }

    if (m_idleCount) {
        m_idleCount = 0;
        Q_EMIT q->idleCountChanged();
    }

    m_detectionAvailable = false;
    Q_EMIT q->detectionAvailableChanged();

    if (!m_inDestructor) {
        q->beginResetModel();
        m_nodeList.clear();
        q->endResetModel();
    }

    if (m_registry) {
        pw_proxy_destroy(reinterpret_cast<struct pw_proxy *>(m_registry));
        spa_hook_remove(&m_registryListener);
        m_registry = nullptr;
    }
    QObject::disconnect(m_pwCore.get(), &PipeWireCore::pipeBroken, q, &MediaMonitor::onPipeBroken);
}

void MediaMonitorPrivate::reconnectOnIdle()
{
    if (m_reconnectTimer.isActive()) {
        return;
    }

    static unsigned retryCount = 0;
    if (retryCount > 100) {
        qWarning() << "Camera indicator receives too many errors. Aborting...";
        return;
    }
    ++retryCount;
    m_reconnectTimer.start();
}

void MediaMonitorPrivate::updateState()
{
    int newIdleCount = 0;
    int newRunningCount = 0;
    for (const auto &proxy : m_nodeList) {
        switch (static_cast<Node *>(pw_proxy_get_user_data(proxy.get()))->state) {
        case NodeState::Idle:
            ++newIdleCount;
            break;
        case NodeState::Running:
            ++newRunningCount;
            break;
        default:
            break;
        }
    }

    const bool idleChanged = m_idleCount != newIdleCount;
    m_idleCount = newIdleCount;
    const bool runningChanged = m_runningCount != newRunningCount;
    m_runningCount = newRunningCount;

    if (idleChanged) {
        Q_EMIT q->idleCountChanged();
    }
    if (runningChanged) {
        Q_EMIT q->runningCountChanged();
    }
}

#include "moc_mediamonitor.cpp"
