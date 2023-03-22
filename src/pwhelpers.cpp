/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "pwhelpers.h"
#include "logging.h"

QImage::Format SpaToQImageFormat(quint32 format)
{
    switch (format) {
    case SPA_VIDEO_FORMAT_BGRx:
    case SPA_VIDEO_FORMAT_BGRA:
        return QImage::Format_RGBA8888_Premultiplied; // TODO: Add BGR to QImage
    case SPA_VIDEO_FORMAT_BGR:
        return QImage::Format_BGR888;
    case SPA_VIDEO_FORMAT_RGBx:
        return QImage::Format_RGBX8888;
    case SPA_VIDEO_FORMAT_RGB:
        return QImage::Format_RGB888;
    case SPA_VIDEO_FORMAT_RGBA:
        return QImage::Format_RGBA8888_Premultiplied;
    default:
        qCWarning(PIPEWIRE_LOGGING) << "unknown spa format" << format;
        return QImage::Format_RGB32;
    }
}

QImage PWHelpers::SpaBufferToQImage(const uchar *data, int width, int height, qsizetype bytesPerLine, spa_video_format format)
{
    switch (format) {
    case SPA_VIDEO_FORMAT_BGRx:
    case SPA_VIDEO_FORMAT_BGRA: {
        // This is needed because QImage does not support BGRA
        // This is obviously a much slower path, it makes sense to avoid it as much as possible
        return QImage(data, width, height, bytesPerLine, SpaToQImageFormat(format)).rgbSwapped();
    }
    case SPA_VIDEO_FORMAT_BGR:
    case SPA_VIDEO_FORMAT_RGBx:
    case SPA_VIDEO_FORMAT_RGB:
    case SPA_VIDEO_FORMAT_RGBA:
    default:
        return QImage(data, width, height, bytesPerLine, SpaToQImageFormat(format));
    }
}
