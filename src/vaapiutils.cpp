/*
    SPDX-FileCopyrightText: 2023 Marco Martin <mart@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "vaapiutils_p.h"
#include <logging_record.h>

#include <QDir>

extern "C" {
#include <fcntl.h>
#include <unistd.h>
#include <va/va_drm.h>
#include <xf86drm.h>
}

VaapiUtils::VaapiUtils()
{
    int max_devices = drmGetDevices2(0, nullptr, 0);
    if (max_devices <= 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "drmGetDevices2() has not found any devices (errno=" << -max_devices << ")";
        return;
    }

    std::vector<drmDevicePtr> devices(max_devices);
    int ret = drmGetDevices2(0, devices.data(), max_devices);
    if (ret < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "drmGetDevices2() returned an error " << ret;
        return;
    }

    for (const drmDevicePtr &device : devices) {
        if (device->available_nodes & (1 << DRM_NODE_RENDER)) {
            QByteArray fullPath = device->nodes[DRM_NODE_RENDER];
            if (supportsH264(fullPath)) {
                m_devicePath = fullPath;
                break;
            }
            break;
        }
    }

    drmFreeDevices(devices.data(), ret);

    if (m_devicePath.isEmpty()) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "DRM device not found";
    }
}

VaapiUtils::~VaapiUtils()
{
}

bool VaapiUtils::supportsProfile(VAProfile profile)
{
    if (m_devicePath.isEmpty()) {
        return false;
    }
    bool ret = false;

    int drmFd = -1;

    VADisplay vaDpy = openDevice(&drmFd, m_devicePath);
    if (!vaDpy) {
        return false;
    }

    ret = supportsProfile(profile, vaDpy, m_devicePath);

    closeDevice(&drmFd, vaDpy);

    return ret;
}

bool VaapiUtils::supportsH264(const QByteArray &path) const
{
    if (path.isEmpty()) {
        return false;
    }
    bool ret = false;

    int drmFd = -1;

    VADisplay vaDpy = openDevice(&drmFd, path);
    if (!vaDpy) {
        return false;
    }

    ret = supportsProfile(VAProfileH264ConstrainedBaseline, vaDpy, path) || supportsProfile(VAProfileH264Main, vaDpy, path)
        || supportsProfile(VAProfileH264High, vaDpy, path);

    querySizeConstraints(vaDpy);

    /**
     * If FFPEG fails to import a buffer with modifiers, it silently goes into
     * of importing as linear, which looks to us like it works, but obviously results in
     * a messed up image. At the time of writing the Intel iHD driver does not
     *
     * Manually blacklist drivers which are known to fail import
     *
     * 10/7/23 - FFmpeg 2.6 with intel-media-driver 23.2.3-1
     */
    const bool blackListed = QByteArray(vaQueryVendorString(vaDpy)).startsWith("Intel iHD driver");

    const bool disabledByEnvVar = qEnvironmentVariableIntValue("KPIPEWIRE_NO_MODIFIERS_FOR_ENCODING") > 0;

    if (blackListed || disabledByEnvVar) {
        m_supportsHardwareModifiers = false;
    }

    closeDevice(&drmFd, vaDpy);

    return ret;
}

QByteArray VaapiUtils::devicePath()
{
    return m_devicePath;
}

QSize VaapiUtils::minimumSize() const
{
    return m_minSize;
}

QSize VaapiUtils::maximumSize() const
{
    return m_maxSize;
}

bool VaapiUtils::supportsHardwareModifiers() const
{
    return m_supportsHardwareModifiers;
}

VADisplay VaapiUtils::openDevice(int *fd, const QByteArray &path)
{
    VADisplay vaDpy;

    if (path.isEmpty()) {
        return NULL;
    }

    *fd = open(path.data(), O_RDWR);
    if (*fd < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "VAAPI: Failed to open device" << path;
        return NULL;
    }

    vaDpy = vaGetDisplayDRM(*fd);
    if (!vaDpy) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "VAAPI: Failed to initialize DRM display";
        return NULL;
    }

    if (vaDisplayIsValid(vaDpy) == 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Invalid VA display";
        vaTerminate(vaDpy);
        return NULL;
    }

    int major, minor;
    VAStatus va_status = vaInitialize(vaDpy, &major, &minor);

    if (va_status != VA_STATUS_SUCCESS) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "VAAPI: Failed to initialize display";
        return NULL;
    }

    qCWarning(PIPEWIRERECORD_LOGGING) << "VAAPI: Display initialized";

    qCWarning(PIPEWIRERECORD_LOGGING) << "VAAPI: API version" << major << "." << minor;

    const char *driver = vaQueryVendorString(vaDpy);

    qCWarning(PIPEWIRERECORD_LOGGING) << "VAAPI:" << driver << "in use for device" << path;

    return vaDpy;
}

void VaapiUtils::closeDevice(int *fd, VADisplay dpy)
{
    vaTerminate(dpy);
    if (*fd < 0) {
        return;
    }

    close(*fd);
    *fd = -1;
}

bool VaapiUtils::supportsProfile(VAProfile profile, VADisplay dpy, const QByteArray &path)
{
    uint32_t ret = rateControlForProfile(profile, VAEntrypointEncSlice, dpy, path);

    if (ret & VA_RC_CBR || ret & VA_RC_CQP || ret & VA_RC_VBR) {
        return true;
    } else {
        ret = rateControlForProfile(profile, VAEntrypointEncSliceLP, dpy, path);

        if (ret & VA_RC_CBR || ret & VA_RC_CQP || ret & VA_RC_VBR) {
            return true;
        }
    }

    return false;
}

uint32_t VaapiUtils::rateControlForProfile(VAProfile profile, VAEntrypoint entrypoint, VADisplay dpy, const QByteArray &path)
{
    VAStatus va_status;
    VAConfigAttrib attrib[1];
    attrib->type = VAConfigAttribRateControl;

    va_status = vaGetConfigAttributes(dpy, profile, entrypoint, attrib, 1);

    switch (va_status) {
    case VA_STATUS_SUCCESS:
        return attrib->value;
    case VA_STATUS_ERROR_UNSUPPORTED_PROFILE:
        qCWarning(PIPEWIRERECORD_LOGGING) << "VAAPI: profile" << profile << "is not supported by the device" << path;
        return 0;
    case VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT:
        qCWarning(PIPEWIRERECORD_LOGGING) << "VAAPI: entrypoint" << entrypoint << "of profile" << profile << "is not supported by the device" << path;
        return 0;
    default:
        qCWarning(PIPEWIRERECORD_LOGGING) << "VAAPI: Fail to get RC attribute from the" << profile << entrypoint << "of the device" << path;
        return 0;
    }
}

void VaapiUtils::querySizeConstraints(VADisplay dpy) const
{
    VAConfigID config;
    if (auto status = vaCreateConfig(dpy, VAProfileH264ConstrainedBaseline, VAEntrypointEncSlice, nullptr, 0, &config); status != VA_STATUS_SUCCESS) {
        return;
    }

    VASurfaceAttrib attrib[8];
    uint32_t attribCount = 8;

    auto status = vaQuerySurfaceAttributes(dpy, config, attrib, &attribCount);
    if (status == VA_STATUS_SUCCESS) {
        for (uint32_t i = 0; i < attribCount; ++i) {
            switch (attrib[i].type) {
            case VASurfaceAttribMinWidth:
                m_minSize.setWidth(attrib[i].value.value.i);
                break;
            case VASurfaceAttribMinHeight:
                m_minSize.setHeight(attrib[i].value.value.i);
                break;
            case VASurfaceAttribMaxWidth:
                m_maxSize.setWidth(attrib[i].value.value.i);
                break;
            case VASurfaceAttribMaxHeight:
                m_maxSize.setHeight(attrib[i].value.value.i);
                break;
            default:
                break;
            }
        }
    }

    vaDestroyConfig(dpy, config);
}
