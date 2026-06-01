// SPDX-FileCopyrightText: 2022 Aleix Pol i Gonzalez <aleixpol@kde.org>
// SPDX-License-Identifier: Apache-2.0

#include "dmabufhandler.h"
#include "glhelpers.h"
#include "rendernodecontext_p.h"
#include <fcntl.h>
#include <gbm.h>
#include <logging_dmabuf.h>
#include <unistd.h>

struct DmaBufHandlerPrivate {
    ~DmaBufHandlerPrivate()
    {
        if (gbmDevice) {
            gbm_device_destroy(gbmDevice);
        }
        if (drmFd >= 0) {
            close(drmFd);
        }
    }

    bool eglInitialized = false;
    qint32 drmFd = -1;
    gbm_device *gbmDevice = nullptr;

    struct EGLStruct {
        EGLDisplay display = EGL_NO_DISPLAY;
        EGLContext context = EGL_NO_CONTEXT;
    };
    EGLStruct egl;
};

DmaBufHandler::DmaBufHandler()
    : d(std::make_unique<DmaBufHandlerPrivate>())
{
}

DmaBufHandler::~DmaBufHandler() = default;

void DmaBufHandler::setupEgl()
{
    if (d->eglInitialized) {
        return;
    }

    const auto renderContext = RenderNodeResolver::resolveForCurrentSession();
    d->egl.display = renderContext.eglDisplay;

    // Use eglGetPlatformDisplayEXT() to get the display pointer
    // if the implementation supports it.
    if (!epoxy_has_egl_extension(d->egl.display, "EGL_EXT_platform_base") || !epoxy_has_egl_extension(d->egl.display, "EGL_MESA_platform_gbm")) {
        qCWarning(PIPEWIREDMABUF_LOGGING) << "One of required EGL extensions is missing";
        return;
    }

    if (d->egl.display == EGL_NO_DISPLAY) {
        d->egl.display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, (void *)EGL_DEFAULT_DISPLAY, nullptr);
    }
    if (d->egl.display == EGL_NO_DISPLAY) {
        const QByteArray renderNode = renderContext.renderNode;
        if (renderNode.isEmpty()) {
            qCWarning(PIPEWIREDMABUF_LOGGING) << "Failed to resolve a render node for the current session";
            return;
        }
        d->drmFd = open(renderNode.constData(), O_RDWR);

        if (d->drmFd < 0) {
            qCWarning(PIPEWIREDMABUF_LOGGING) << "Failed to open drm render node" << renderNode << "with error: " << strerror(errno);
            return;
        }

        d->gbmDevice = gbm_create_device(d->drmFd);

        if (!d->gbmDevice) {
            qCWarning(PIPEWIREDMABUF_LOGGING) << "Cannot create GBM device: " << strerror(errno);
            return;
        }
        d->egl.display = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_MESA, d->gbmDevice, nullptr);
    }

    if (d->egl.display == EGL_NO_DISPLAY) {
        qCWarning(PIPEWIREDMABUF_LOGGING) << "Error during obtaining EGL display: " << GLHelpers::formatEGLError(eglGetError());
        return;
    }

    EGLint major = 0;
    EGLint minor = 0;
    if (eglInitialize(d->egl.display, &major, &minor) == EGL_FALSE) {
        qCWarning(PIPEWIREDMABUF_LOGGING) << "Error during eglInitialize: " << GLHelpers::formatEGLError(eglGetError());
        return;
    }

    if (eglBindAPI(EGL_OPENGL_API) == EGL_FALSE) {
        qCWarning(PIPEWIREDMABUF_LOGGING) << "bind OpenGL API failed";
        return;
    }

    EGLConfig configs{};
    auto createConfig = [&] {
        static const EGLint configAttribs[] = {
            EGL_SURFACE_TYPE,
            EGL_WINDOW_BIT,
            EGL_RED_SIZE,
            8,
            EGL_GREEN_SIZE,
            8,
            EGL_BLUE_SIZE,
            8,
            EGL_RENDERABLE_TYPE,
            EGL_OPENGL_BIT,
            EGL_CONFIG_CAVEAT,
            EGL_NONE,
            EGL_NONE,
        };

        EGLint count = 333;
        if (eglChooseConfig(d->egl.display, configAttribs, &configs, 1, &count) == EGL_FALSE) {
            qCWarning(PIPEWIREDMABUF_LOGGING) << "choose config failed";
            return false;
        }
        qCWarning(PIPEWIREDMABUF_LOGGING) << "eglChooseConfig returned this many configs:" << count;
        return true;
    };

    bool b = createConfig();
    static const EGLint configAttribs[] = {EGL_CONTEXT_OPENGL_DEBUG, EGL_TRUE, EGL_NONE};
    Q_ASSERT(configs);
    d->egl.context = eglCreateContext(d->egl.display, b ? configs : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, configAttribs);

    Q_ASSERT(b);
    Q_ASSERT(d->egl.context);
    if (d->egl.context == EGL_NO_CONTEXT) {
        qCWarning(PIPEWIREDMABUF_LOGGING) << "Couldn't create EGL context: " << GLHelpers::formatEGLError(eglGetError());
        return;
    }

    qCDebug(PIPEWIREDMABUF_LOGGING) << "Egl initialization succeeded";
    qCDebug(PIPEWIREDMABUF_LOGGING) << QStringLiteral("EGL version: %1.%2").arg(major).arg(minor);

    d->eglInitialized = true;
}

GLenum closestGLType(const QImage &image)
{
    switch (image.format()) {
    case QImage::Format_RGB888:
        return GL_RGB;
    case QImage::Format_BGR888:
        return GL_BGR;
    case QImage::Format_RGB32:
    case QImage::Format_RGBX8888:
    case QImage::Format_RGBA8888:
    case QImage::Format_RGBA8888_Premultiplied:
        return GL_RGBA;
    default:
        qDebug() << "cannot convert QImage format to GLType" << image.format();
        return GL_RGBA;
    }
}

bool DmaBufHandler::downloadFrame(QImage &qimage, const PipeWireFrame &frame)
{
    Q_ASSERT(frame.dmabuf);
    const QSize streamSize = {frame.dmabuf->width, frame.dmabuf->height};
    Q_ASSERT(qimage.size() == streamSize);
    setupEgl();
    if (!d->eglInitialized) {
        return false;
    }

    if (!eglMakeCurrent(d->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, d->egl.context)) {
        qCWarning(PIPEWIREDMABUF_LOGGING) << "Failed to make context current" << GLHelpers::formatEGLError(eglGetError());
        return false;
    }
    EGLImageKHR image =
        GLHelpers::createImage(d->egl.display, *frame.dmabuf, PipeWireSourceStream::spaVideoFormatToDrmFormat(frame.format), qimage.size(), d->gbmDevice);

    if (image == EGL_NO_IMAGE_KHR) {
        qCWarning(PIPEWIREDMABUF_LOGGING) << "Failed to record frame: Error creating EGLImageKHR - " << GLHelpers::formatEGLError(eglGetError());
        return false;
    }

    GLHelpers::initDebugOutput();
    // create GL 2D texture for framebuffer
    GLuint texture = 0;
    GLuint fbo = 0;
    glGenTextures(1, &texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, texture);

    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

    auto releaseResources = qScopeGuard([&]() {
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &texture);
        eglDestroyImageKHR(d->egl.display, image);
    });

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        return false;
    }

    glReadPixels(0, 0, frame.dmabuf->width, frame.dmabuf->height, closestGLType(qimage), GL_UNSIGNED_BYTE, qimage.bits());
    return true;
}
