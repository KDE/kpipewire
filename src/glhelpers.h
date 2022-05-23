/*
    SPDX-FileCopyrightText: 2020-2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#pragma once

#include <QByteArray>

typedef unsigned int GLenum;

namespace GLHelpers
{

QByteArray formatGLError(GLenum err);

QList<QByteArray> eglExtensions();
bool hasEglExtension(const QByteArray &name);

}
