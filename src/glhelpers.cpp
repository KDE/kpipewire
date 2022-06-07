/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "glhelpers.h"
#include <QDebug>
#include <QList>
#include <QVersionNumber>
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <libdrm/drm_fourcc.h>
#include <logging.h>
#include <mutex>

namespace GLHelpers
{

void initDebugOutputOnce()
{
    auto callback = [](GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar *message, const GLvoid *userParam) {
        Q_UNUSED(source)
        Q_UNUSED(severity)
        Q_UNUSED(userParam)
        while (length && std::isspace(message[length - 1])) {
            --length;
        }

        switch (type) {
        case GL_DEBUG_TYPE_ERROR:
        case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
            qCWarning(PIPEWIRE_LOGGING, "%#x: %.*s", id, length, message);
            break;

        case GL_DEBUG_TYPE_OTHER:
        case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
        case GL_DEBUG_TYPE_PORTABILITY:
        case GL_DEBUG_TYPE_PERFORMANCE:
        default:
            qCDebug(PIPEWIRE_LOGGING, "%#x: %.*s", id, length, message);
            break;
        }
    };
    glDebugMessageCallback(callback, nullptr);
    glEnable(GL_DEBUG_OUTPUT);
}

static std::once_flag initDebugOnce;
void initDebugOutput()
{
    if (!PIPEWIRE_LOGGING().isDebugEnabled()) {
        return;
    }

    std::call_once(initDebugOnce, initDebugOutputOnce);
}

#define ENUM_STRING(x) case x: return #x;

QByteArray formatGLError(GLenum err)
{
    switch (err) {
    ENUM_STRING(GL_NO_ERROR)
    ENUM_STRING(GL_INVALID_ENUM)
    ENUM_STRING(GL_INVALID_VALUE)
    ENUM_STRING(GL_INVALID_OPERATION)
    ENUM_STRING(GL_STACK_OVERFLOW)
    ENUM_STRING(GL_STACK_UNDERFLOW)
    ENUM_STRING(GL_OUT_OF_MEMORY)
    default:
        return QByteArray("0x") + QByteArray::number(err, 16);
    }
}

QByteArray formatEGLError(GLenum err)
{
    switch (err) {
    ENUM_STRING(EGL_BAD_DISPLAY)
    ENUM_STRING(EGL_BAD_CONTEXT)
    ENUM_STRING(EGL_BAD_PARAMETER)
    ENUM_STRING(EGL_BAD_MATCH)
    ENUM_STRING(EGL_BAD_ACCESS)
    ENUM_STRING(EGL_BAD_ALLOC)
    default:
        return QByteArray("0x") + QByteArray::number(err, 16);
    }
}

bool hasEglExtension(const QByteArray &name)
{
    return eglExtensions().contains(name);
}

QList<QByteArray> eglExtensions()
{
    const char *clientExtensionsCString = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    const QByteArray clientExtensionsString = QByteArray::fromRawData(clientExtensionsCString, qstrlen(clientExtensionsCString));
    if (clientExtensionsString.isEmpty()) {
        // If eglQueryString() returned NULL, the implementation doesn't support
        // EGL_EXT_client_extensions. Expect an EGL_BAD_DISPLAY error.
        qWarning() << "No client extensions defined! " << GLHelpers::formatGLError(eglGetError());
        return {};
    }

    return clientExtensionsString.split(' ');
}

EGLImage createImage(EGLDisplay display, const QVector<DmaBufPlane> &planes, uint32_t format, const QSize &size)
{
    const bool hasModifiers = planes[0].modifier != DRM_FORMAT_MOD_INVALID;

    QVector<EGLint> attribs;
    attribs << EGL_WIDTH << size.width() << EGL_HEIGHT << size.height() << EGL_LINUX_DRM_FOURCC_EXT << EGLint(format)

            << EGL_DMA_BUF_PLANE0_FD_EXT << planes[0].fd << EGL_DMA_BUF_PLANE0_OFFSET_EXT << EGLint(planes[0].offset) << EGL_DMA_BUF_PLANE0_PITCH_EXT
            << EGLint(planes[0].stride);

    if (hasModifiers) {
        attribs << EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT << EGLint(planes[0].modifier & 0xffffffff) << EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT
                << EGLint(planes[0].modifier >> 32);
    }

    if (planes.count() > 1) {
        attribs << EGL_DMA_BUF_PLANE1_FD_EXT << planes[1].fd << EGL_DMA_BUF_PLANE1_OFFSET_EXT << EGLint(planes[1].offset) << EGL_DMA_BUF_PLANE1_PITCH_EXT
                << EGLint(planes[1].stride);

        if (hasModifiers) {
            attribs << EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT << EGLint(planes[1].modifier & 0xffffffff) << EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT
                    << EGLint(planes[1].modifier >> 32);
        }
    }

    if (planes.count() > 2) {
        attribs << EGL_DMA_BUF_PLANE2_FD_EXT << planes[2].fd << EGL_DMA_BUF_PLANE2_OFFSET_EXT << EGLint(planes[2].offset) << EGL_DMA_BUF_PLANE2_PITCH_EXT
                << EGLint(planes[2].stride);

        if (hasModifiers) {
            attribs << EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT << EGLint(planes[2].modifier & 0xffffffff) << EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT
                    << EGLint(planes[2].modifier >> 32);
        }
    }

    if (planes.count() > 3) {
        attribs << EGL_DMA_BUF_PLANE3_FD_EXT << planes[3].fd << EGL_DMA_BUF_PLANE3_OFFSET_EXT << EGLint(planes[3].offset) << EGL_DMA_BUF_PLANE3_PITCH_EXT
                << EGLint(planes[3].stride);

        if (hasModifiers) {
            attribs << EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT << EGLint(planes[3].modifier & 0xffffffff) << EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT
                    << EGLint(planes[3].modifier >> 32);
        }
    }

    attribs << EGL_NONE;

    static auto eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    Q_ASSERT(eglCreateImageKHR);

    EGLImage ret = eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, (EGLClientBuffer) nullptr, attribs.data());
    if (ret == EGL_NO_IMAGE_KHR) {
        qCWarning(PIPEWIRE_LOGGING) << "invalid image" << GLHelpers::formatEGLError(eglGetError());
    }
    //     Q_ASSERT(ret);
    return ret;
}
}
