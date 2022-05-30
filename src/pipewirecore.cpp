/*
    SPDX-FileCopyrightText: 2020 Aleix Pol Gonzalez <aleixpol@kde.org>
    SPDX-FileContributor: Jan Grulich <jgrulich@redhat.com>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "pipewirecore.h"
#include "logging.h"
#include <KLocalizedString>
#include <QSocketNotifier>
#include <QThread>
#include <QThreadStorage>
#include <spa/utils/result.h>
#include <mutex>

pw_core_events PipeWireCore::s_pwCoreEvents = {
    .version = PW_VERSION_CORE_EVENTS,
    .info = &PipeWireCore::onCoreInfo,
    .done = nullptr,
    .ping = nullptr,
    .error = &PipeWireCore::onCoreError,
    .remove_id = nullptr,
    .bound_id = nullptr,
    .add_mem = nullptr,
    .remove_mem = nullptr,
};

PipeWireCore::PipeWireCore()
{
    static std::once_flag pwInitOnce;
    std::call_once(pwInitOnce, [] { pw_init(nullptr, nullptr); });
}

void PipeWireCore::onCoreError(void *data, uint32_t id, int seq, int res, const char *message)
{
    Q_UNUSED(seq)

    qCWarning(PIPEWIRE_LOGGING) << "PipeWire remote error: " << res << message;
    if (id == PW_ID_CORE) {
        PipeWireCore *pw = static_cast<PipeWireCore *>(data);
        Q_EMIT pw->pipewireFailed(QString::fromUtf8(message));
    }
}

void PipeWireCore::onCoreInfo(void *data, const struct pw_core_info *info)
{
    PipeWireCore *pw = static_cast<PipeWireCore *>(data);
    pw->m_serverVersion = QVersionNumber::fromString(QString::fromUtf8(info->version));
}

PipeWireCore::~PipeWireCore()
{
    if (m_pwMainLoop) {
        pw_loop_leave(m_pwMainLoop);
    }

    if (m_pwCore) {
        pw_core_disconnect(m_pwCore);
    }

    if (m_pwContext) {
        pw_context_destroy(m_pwContext);
    }

    if (m_pwMainLoop) {
        pw_loop_destroy(m_pwMainLoop);
    }
}

bool PipeWireCore::init(int fd)
{
    m_pwMainLoop = pw_loop_new(nullptr);
    pw_loop_enter(m_pwMainLoop);

    QSocketNotifier *notifier = new QSocketNotifier(pw_loop_get_fd(m_pwMainLoop), QSocketNotifier::Read, this);
    connect(notifier, &QSocketNotifier::activated, this, [this] {
        int result = pw_loop_iterate(m_pwMainLoop, 0);
        if (result < 0)
            qCWarning(PIPEWIRE_LOGGING) << "pipewire_loop_iterate failed: " << spa_strerror(result);
    });

    m_pwContext = pw_context_new(m_pwMainLoop, nullptr, 0);
    if (!m_pwContext) {
        qCWarning(PIPEWIRE_LOGGING) << "Failed to create PipeWire context";
        m_error = i18n("Failed to create PipeWire context");
        return false;
    }

    if (fd > 0) {
        m_pwCore = pw_context_connect_fd(m_pwContext, fd, nullptr, 0);
    } else {
        m_pwCore = pw_context_connect(m_pwContext, nullptr, 0);
    }
    if (!m_pwCore) {
        m_error = i18n("Failed to connect to PipeWire");
        qCWarning(PIPEWIRE_LOGGING) << "error:" << m_error << fd;
        return false;
    }

    if (pw_loop_iterate(m_pwMainLoop, 0) < 0) {
        qCWarning(PIPEWIRE_LOGGING) << "Failed to start main PipeWire loop";
        m_error = i18n("Failed to start main PipeWire loop");
        return false;
    }

    pw_core_add_listener(m_pwCore, &m_coreListener, &s_pwCoreEvents, this);
    return true;
}

QSharedPointer<PipeWireCore> PipeWireCore::fetch(int fd)
{
    static QThreadStorage<QHash<int, QWeakPointer<PipeWireCore>>> global;
    QSharedPointer<PipeWireCore> ret = global.localData().value(fd).toStrongRef();
    if (!ret) {
        ret.reset(new PipeWireCore);
        if (ret->init(fd)) {
            global.localData().insert(fd, ret);
        }
    }
    return ret;
}

QString PipeWireCore::error() const
{
    return m_error;
}
