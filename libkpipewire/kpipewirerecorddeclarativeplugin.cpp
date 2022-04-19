/*
 *   SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>
 *
 *   SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "kpipewirerecorddeclarativeplugin.h"

#include "pipewirerecord.h"
#include <qqml.h>

void KPipewireRecordDeclarativePlugin::registerTypes(const char * /*uri*/)
{
    const char * uri = "org.kde.pipewire.record";
    qmlRegisterType<PipeWireRecord>(uri, 0, 1, "PipeWireRecord");
}
