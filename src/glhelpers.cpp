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

QByteArray formatGLError(GLenum err)
{
    switch (err) {
    case GL_NO_ERROR:
        return "GL_NO_ERROR";
    case GL_INVALID_ENUM:
        return "GL_INVALID_ENUM";
    case GL_INVALID_VALUE:
        return "GL_INVALID_VALUE";
    case GL_INVALID_OPERATION:
        return "GL_INVALID_OPERATION";
    case GL_STACK_OVERFLOW:
        return "GL_STACK_OVERFLOW";
    case GL_STACK_UNDERFLOW:
        return "GL_STACK_UNDERFLOW";
    case GL_OUT_OF_MEMORY:
        return "GL_OUT_OF_MEMORY";
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

}
