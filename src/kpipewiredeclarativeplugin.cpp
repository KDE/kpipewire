/*
 *   SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>
 *
 *   SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "kpipewiredeclarativeplugin.h"

#include "pipewiresourceitem.h"
#include "pipewirerecord.h"
#include "screencasting.h"
#include "screencastingrequest.h"

void KPipewireDeclarativePlugin::registerTypes(const char *uri)
{
    qmlRegisterType<PipeWireSourceItem>(uri, 0, 1, "PipeWireSourceItem");
    qmlRegisterType<ScreencastingRequest>(uri, 0, 1, "ScreencastingRequest");
    qmlRegisterUncreatableType<Screencasting>(uri, 0, 1, "Screencasting", "Use PipeWireSourceItem");
}
