/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QByteArray>
#include <kpipewire_export.h>

typedef unsigned int GLenum;

namespace GLHelpers
{

KPIPEWIRE_EXPORT void initDebugOutput();
KPIPEWIRE_EXPORT QByteArray formatGLError(GLenum err);

KPIPEWIRE_EXPORT QList<QByteArray> eglExtensions();
KPIPEWIRE_EXPORT bool hasEglExtension(const QByteArray &name);
}
