#include "rendernodecontext_p.h"

#include <QGuiApplication>
#include <qpa/qplatformnativeinterface.h>
#include <vector>

extern "C" {
#include <xf86drm.h>
}

QByteArray RenderNodeResolver::renderNodeForDevicePath(const QByteArray &devicePath)
{
    if (devicePath.isEmpty()) {
        return {};
    }

    int maxDevices = drmGetDevices2(0, nullptr, 0);
    if (maxDevices <= 0) {
        return {};
    }

    std::vector<drmDevicePtr> devices(maxDevices);
    const int ret = drmGetDevices2(0, devices.data(), maxDevices);
    if (ret < 0) {
        return {};
    }

    QByteArray renderNode;
    for (const drmDevicePtr &device : devices) {
        const bool hasPrimary = device->available_nodes & (1 << DRM_NODE_PRIMARY);
        const bool hasRender = device->available_nodes & (1 << DRM_NODE_RENDER);

        if (hasPrimary && devicePath == device->nodes[DRM_NODE_PRIMARY]) {
            if (hasRender) {
                renderNode = device->nodes[DRM_NODE_RENDER];
            }
            break;
        }
        if (hasRender && devicePath == device->nodes[DRM_NODE_RENDER]) {
            renderNode = device->nodes[DRM_NODE_RENDER];
            break;
        }
    }

    drmFreeDevices(devices.data(), ret);
    return renderNode;
}

RenderNodeContext RenderNodeResolver::resolveForCurrentSession()
{
    RenderNodeContext context;

    auto *platformInterface = QGuiApplication::platformNativeInterface();
    if (!platformInterface) {
        return context;
    }

    context.eglDisplay = static_cast<EGLDisplay>(platformInterface->nativeResourceForIntegration("egldisplay"));
    if (context.eglDisplay == EGL_NO_DISPLAY) {
        return context;
    }

    if (!epoxy_has_egl_extension(context.eglDisplay, "EGL_EXT_device_query")) {
        return context;
    }

    EGLAttrib deviceAttrib = 0;
    if (!eglQueryDisplayAttribEXT(context.eglDisplay, EGL_DEVICE_EXT, &deviceAttrib) || deviceAttrib == 0) {
        return context;
    }

#ifdef EGL_DRM_RENDER_NODE_FILE_EXT
    if (const auto renderNode = eglQueryDeviceStringEXT(reinterpret_cast<EGLDeviceEXT>(deviceAttrib), EGL_DRM_RENDER_NODE_FILE_EXT)) {
        if (renderNode[0] != '\0') {
            context.renderNode = QByteArray(renderNode);
        }
    }
#endif

#ifdef EGL_DRM_DEVICE_FILE_EXT
    if (const auto primaryNode = eglQueryDeviceStringEXT(reinterpret_cast<EGLDeviceEXT>(deviceAttrib), EGL_DRM_DEVICE_FILE_EXT)) {
        if (primaryNode[0] != '\0') {
            context.primaryNode = QByteArray(primaryNode);
            if (context.renderNode.isEmpty()) {
                context.renderNode = renderNodeForDevicePath(context.primaryNode);
            }
        }
    }
#endif

    return context;
}
