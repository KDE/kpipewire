/*
    SPDX-FileCopyrightText: 2023 Marco Martin <mart@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QByteArray>
#include <QSize>

extern "C" {
#include <va/va.h>
}

class VaapiUtils
{
public:
    VaapiUtils();
    ~VaapiUtils();

    bool supportsProfile(VAProfile profile);

    QByteArray devicePath();

    QSize minimumSize() const;
    QSize maximumSize() const;

    bool supportsHardwareModifiers() const;

private:
    static VADisplay openDevice(int *fd, const QByteArray &path);
    static void closeDevice(int *fd, VADisplay dpy);
    bool supportsH264(const QByteArray &path) const;
    void querySizeConstraints(VADisplay dpy) const;
    static bool supportsProfile(VAProfile profile, VADisplay dpy, const QByteArray &path);
    static uint32_t rateControlForProfile(VAProfile profile, VAEntrypoint entrypoint, VADisplay dpy, const QByteArray &path);

    QByteArray m_devicePath;

    mutable bool m_supportsHardwareModifiers = true;
    mutable QSize m_minSize;
    mutable QSize m_maxSize = QSize{std::numeric_limits<int>::max(), std::numeric_limits<int>::max()};
};
