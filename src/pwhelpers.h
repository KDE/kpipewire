/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include "pipewiresourcestream.h"
#include <QByteArray>
#include <epoxy/egl.h>
#include <kpipewire_export.h>

typedef unsigned int GLenum;

namespace PWHelpers
{

KPIPEWIRE_EXPORT QImage SpaBufferToQImage(const uchar *data, int width, int height, qsizetype bytesPerLine, spa_video_format format);

}
