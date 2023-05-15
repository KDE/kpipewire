/*
    SPDX-FileCopyrightText: 2023 Marco Martin <mart@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "vaapiutils_p.h"
#include <logging_record.h>

#include <QDir>

extern "C" {
#include <fcntl.h>
#include <libavformat/avformat.h>
#include <unistd.h>
#include <va/va_drm.h>
#include <xf86drm.h>
}

#undef av_err2str
// The one provided by libav fails to compile on GCC due to passing data from the function scope outside
char str[AV_ERROR_MAX_STRING_SIZE];
char *av_err2str(int errnum)
{
    return av_make_error_string(str, AV_ERROR_MAX_STRING_SIZE, errnum);
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
    av_buffer_unref(&m_drmContext);
    av_buffer_unref(&m_drmFramesContext);
}

bool VaapiUtils::isValid() const
{
    return !m_devicePath.isEmpty() && m_drmContext != nullptr && m_drmFramesContext != nullptr;
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

    closeDevice(&drmFd, vaDpy);

    return ret;
}

AVBufferRef *VaapiUtils::drmContext() const
{
    return m_drmContext;
}

AVBufferRef *VaapiUtils::drmFramesContext() const
{
    return m_drmFramesContext;
}

void VaapiUtils::init(const QSize &size)
{
    if (m_devicePath.isEmpty()) {
        return;
    }

    if (m_drmContext) {
        av_buffer_unref(&m_drmContext);
    }
    if (m_drmFramesContext) {
        av_buffer_unref(&m_drmFramesContext);
    }

    int err = av_hwdevice_ctx_create(&m_drmContext, AV_HWDEVICE_TYPE_DRM, m_devicePath.data(), NULL, AV_HWFRAME_MAP_READ);
    if (err < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to create DRM device. Error" << av_err2str(err);
        return;
    }

    m_drmFramesContext = av_hwframe_ctx_alloc(m_drmContext);
    if (!m_drmFramesContext) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to create DRM frames context";
        return;
    }

    auto framesContext = reinterpret_cast<AVHWFramesContext *>(m_drmFramesContext->data);
    framesContext->format = AV_PIX_FMT_DRM_PRIME;
    framesContext->sw_format = AV_PIX_FMT_0BGR;
    framesContext->width = size.width();
    framesContext->height = size.height();

    if (auto result = av_hwframe_ctx_init(m_drmFramesContext); result < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed initializing DRM frames context" << av_err2str(result);
        av_buffer_unref(&m_drmFramesContext);
    }
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
