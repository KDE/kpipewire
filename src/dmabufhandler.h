/*
    SPDX-FileCopyrightText: 2022 Aleix Pol i Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#ifndef DMABUFHANDLER_H
#define DMABUFHANDLER_H

#include "kpipewiredmabuf_export.h"
#include "pipewiresourcestream.h"
#include <QImage>
#include <memory>

struct DmaBufHandlerPrivate;

class KPIPEWIREDMABUF_EXPORT DmaBufHandler
{
public:
    DmaBufHandler();
    ~DmaBufHandler();

    bool downloadFrame(QImage &image, const PipeWireFrame &frame);

private:
    void setupEgl();
    std::unique_ptr<DmaBufHandlerPrivate> d;
};

#endif // DMABUFHANDLER_H
