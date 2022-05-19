/*
    SPDX-FileCopyrightText: 2020 Aleix Pol Gonzalez <aleixpol@kde.org>
    SPDX-FileContributor: Jan Grulich <jgrulich@redhat.com>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#pragma once

#include <QObject>
#include <pipewire/pipewire.h>

class PipeWireCore : public QObject
{
    Q_OBJECT
public:
    PipeWireCore();

    static void onCoreError(void *data, uint32_t id, int seq, int res, const char *message);

    ~PipeWireCore();

    bool init();
    QString error() const;

    pw_core *operator*() const { return m_pwCore; };
    static QSharedPointer<PipeWireCore> self();

private:
    pw_core *m_pwCore = nullptr;
    pw_context *m_pwContext = nullptr;
    pw_loop *m_pwMainLoop = nullptr;
    spa_hook m_coreListener;
    QString m_error;

    static pw_core_events s_pwCoreEvents;

Q_SIGNALS:
    void pipewireFailed(const QString &message);
};
