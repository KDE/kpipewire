/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QByteArray>

typedef unsigned int GLenum;

namespace GLHelpers
{

void initDebugOutput();
QByteArray formatGLError(GLenum err);

QList<QByteArray> eglExtensions();
bool hasEglExtension(const QByteArray &name);

}
