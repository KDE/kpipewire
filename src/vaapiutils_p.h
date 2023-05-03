/*
    SPDX-FileCopyrightText: 2023 Marco Martin <mart@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QByteArray>
#include <QSize>

extern "C" {
#include <libavfilter/avfilter.h>
#include <va/va.h>
}

#undef av_err2str
// The one provided by libav fails to compile on GCC due to passing data from the function scope outside
char *av_err2str(int errnum);

class VaapiUtils
{
public:
    VaapiUtils();
    ~VaapiUtils();

    void init(const QSize &size);

    bool isValid() const;

    AVBufferRef *drmContext() const;
    AVBufferRef *drmFramesContext() const;

    bool supportsProfile(VAProfile profile);

private:
    static VADisplay openDevice(int *fd, const QByteArray &path);
    static void closeDevice(int *fd, VADisplay dpy);
    bool supportsH264(const QByteArray &path) const;
    static bool supportsProfile(VAProfile profile, VADisplay dpy, const QByteArray &path);
    static uint32_t rateControlForProfile(VAProfile profile, VAEntrypoint entrypoint, VADisplay dpy, const QByteArray &path);

    QByteArray m_devicePath;

    AVBufferRef *m_drmContext = nullptr;
    AVBufferRef *m_drmFramesContext = nullptr;
};
