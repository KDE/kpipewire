#pragma once

#include <QByteArray>

#include <epoxy/egl.h>

struct RenderNodeContext {
    EGLDisplay eglDisplay = EGL_NO_DISPLAY;
    QByteArray primaryNode;
    QByteArray renderNode;

    bool hasEglDisplay() const
    {
        return eglDisplay != EGL_NO_DISPLAY;
    }

    bool hasRenderNode() const
    {
        return !renderNode.isEmpty();
    }
};

class RenderNodeResolver
{
public:
    static RenderNodeContext resolveForCurrentSession();
    static QByteArray renderNodeForDevicePath(const QByteArray &devicePath);
};
