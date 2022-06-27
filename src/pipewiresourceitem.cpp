/*
    Render a PipeWire stream into a QtQuick scene as a standard Item
    SPDX-FileCopyrightText: 2020 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "pipewiresourceitem.h"
#include "glhelpers.h"
#include "logging.h"
#include "pipewiresourcestream.h"

#include <QGuiApplication>
#include <QOpenGLContext>
#include <QOpenGLTexture>
#include <QPainter>
#include <QQuickWindow>
#include <QRunnable>
#include <QSGImageNode>
#include <QSocketNotifier>
#include <QThread>
#include <qpa/qplatformnativeinterface.h>

#include <EGL/eglext.h>
#include <fcntl.h>
#include <libdrm/drm_fourcc.h>

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QtPlatformHeaders/QEGLNativeContext>
#endif

static void pwInit()
{
    pw_init(nullptr, nullptr);
}
Q_COREAPP_STARTUP_FUNCTION(pwInit);

class DiscardEglPixmapRunnable : public QRunnable
{
public:
    DiscardEglPixmapRunnable(EGLImageKHR image, QOpenGLTexture *texture)
        : m_image(image)
        , m_texture(texture)
    {
    }

    void run() override
    {
        if (m_image != EGL_NO_IMAGE_KHR) {
            static auto eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
            eglDestroyImageKHR(eglGetCurrentDisplay(), m_image);
        }

        delete m_texture;
    }

private:
    const EGLImageKHR m_image;
    QOpenGLTexture *m_texture;
};

PipeWireSourceItem::PipeWireSourceItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);

    connect(this, &QQuickItem::visibleChanged, this, [this]() {
        setEnabled(isVisible());
        if (m_stream)
            m_stream->setActive(isVisible());
    });
}

PipeWireSourceItem::~PipeWireSourceItem()
{
}

void PipeWireSourceItem::itemChange(QQuickItem::ItemChange change, const QQuickItem::ItemChangeData &data)
{
    switch (change) {
    case ItemVisibleHasChanged:
        setEnabled(isVisible());
        if (m_stream)
            m_stream->setActive(isVisible() && data.boolValue && isComponentComplete());
        break;
    case ItemSceneChange:
        m_needsRecreateTexture = true;
        releaseResources();
        break;
    default:
        break;
    }
}

void PipeWireSourceItem::releaseResources()
{
    if (window()) {
        window()->scheduleRenderJob(new DiscardEglPixmapRunnable(m_image, m_texture.take()), QQuickWindow::NoStage);
        m_image = EGL_NO_IMAGE_KHR;
    }
}

void PipeWireSourceItem::setFd(uint fd)
{
    if (fd == m_fd)
        return;

    m_fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    refresh();
    Q_EMIT fdChanged(fd);
}

void PipeWireSourceItem::refresh()
{
    setEnabled(false);

    if (!isComponentComplete()) {
        return;
    }

    if (m_nodeId == 0) {
        m_stream.reset(nullptr);
        m_createNextTexture = [] {
            return nullptr;
        };
    } else {
        m_stream.reset(new PipeWireSourceStream(this));
        m_stream->createStream(m_nodeId, m_fd);
        if (!m_stream->error().isEmpty()) {
            m_stream.reset(nullptr);
            m_nodeId = 0;
            return;
        }
        m_stream->setActive(isVisible() && isComponentComplete());

        connect(m_stream.data(), &PipeWireSourceStream::dmabufTextureReceived, this, &PipeWireSourceItem::updateTextureDmaBuf);
        connect(m_stream.data(), &PipeWireSourceStream::imageTextureReceived, this, &PipeWireSourceItem::updateTextureImage);
        connect(m_stream.data(), &PipeWireSourceStream::cursorChanged, this, &PipeWireSourceItem::moveCursor);
        connect(m_stream.data(), &PipeWireSourceStream::noVisibleFrame, this, [this] {
            if (window()->isVisible()) {
                update();
            }
        });
    }
}

void PipeWireSourceItem::setNodeId(uint nodeId)
{
    if (nodeId == m_nodeId)
        return;

    m_nodeId = nodeId;
    refresh();
    Q_EMIT nodeIdChanged(nodeId);
}

class PipeWireRenderNode : public QSGNode
{
public:
    QSGImageNode *screenNode(QQuickWindow *window)
    {
        if (!m_screenNode) {
            m_screenNode = window->createImageNode();
            appendChildNode(m_screenNode);
        }
        return m_screenNode;
    }
    QSGImageNode *cursorNode(QQuickWindow *window)
    {
        if (!m_cursorNode) {
            m_cursorNode = window->createImageNode();
            appendChildNode(m_cursorNode);
        }
        return m_cursorNode;
    }

    QSGImageNode *damageNode(QQuickWindow *window)
    {
        if (!m_damageNode) {
            m_damageNode = window->createImageNode();
            appendChildNode(m_damageNode);
        }
        return m_damageNode;
    }

    void discardCursor()
    {
        if (m_cursorNode) {
            removeChildNode(m_cursorNode);
            delete m_cursorNode;
            m_cursorNode = nullptr;
        }
    }

    void discardDamage()
    {
        if (m_damageNode) {
            removeChildNode(m_damageNode);
            delete m_damageNode;
            m_damageNode = nullptr;
        }
    }

private:
    QSGImageNode *m_screenNode = nullptr;
    QSGImageNode *m_cursorNode = nullptr;
    QSGImageNode *m_damageNode = nullptr;
};

QSGNode *PipeWireSourceItem::updatePaintNode(QSGNode *node, QQuickItem::UpdatePaintNodeData *)
{
    if (Q_UNLIKELY(!m_createNextTexture)) {
        return node;
    }

    auto texture = m_createNextTexture();
    if (!texture) {
        delete node;
        return nullptr;
    }

    QSGImageNode *screenNode;
    auto pwNode = dynamic_cast<PipeWireRenderNode *>(node);
    if (!pwNode) {
        delete node;
        pwNode = new PipeWireRenderNode;
        screenNode = window()->createImageNode();
        screenNode->setOwnsTexture(true);
        pwNode->appendChildNode(screenNode);
    } else {
        screenNode = static_cast<QSGImageNode *>(pwNode->childAtIndex(0));
    }
    screenNode->setTexture(texture);

    const auto br = boundingRect().toRect();
    QRect rect({0, 0}, texture->textureSize().scaled(br.size(), Qt::KeepAspectRatio));
    rect.moveCenter(br.center());
    screenNode->setRect(rect);

    if (!m_cursor.position.has_value() || m_cursor.texture.isNull()) {
        pwNode->discardCursor();
    } else {
        QSGImageNode *cursorNode = pwNode->cursorNode(window());
        if (m_cursor.dirty) {
            cursorNode->setTexture(window()->createTextureFromImage(m_cursor.texture));
            m_cursor.dirty = false;
        }
        const qreal scale = qreal(rect.width()) / texture->textureSize().width();
        cursorNode->setRect(QRectF{rect.topLeft() + (m_cursor.position.value() * scale), m_cursor.texture.size() * scale});
        Q_ASSERT(cursorNode->texture());
    }

    if (m_stream->damage().isEmpty()) {
        pwNode->discardDamage();
    } else {
        auto *damageNode = pwNode->damageNode(window());
        QImage damageImage(texture->textureSize(), QImage::Format_RGBA64_Premultiplied);
        damageImage.fill(Qt::transparent);
        QPainter p(&damageImage);
        p.setBrush(Qt::red);
        for (auto rect : m_stream->damage()) {
            p.drawRect(rect);
        }
        damageNode->setTexture(window()->createTextureFromImage(damageImage));
        damageNode->setRect(rect);
        Q_ASSERT(damageNode->texture());
    }
    return pwNode;
}

QString PipeWireSourceItem::error() const
{
    return m_stream->error();
}

void PipeWireSourceItem::updateTextureDmaBuf(const QVector<DmaBufPlane> &planes, spa_video_format format)
{
    static auto s_glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!s_glEGLImageTargetTexture2DOES) {
        qCWarning(PIPEWIRE_LOGGING) << "glEGLImageTargetTexture2DOES is not available" << window();
        return;
    }
    if (!window()) {
        qCWarning(PIPEWIRE_LOGGING) << "Window not available" << this;
        return;
    }

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    const auto openglContext = window()->openglContext();
#else
    const auto openglContext = static_cast<QOpenGLContext *>(window()->rendererInterface()->getResource(window(), QSGRendererInterface::OpenGLContextResource));
#endif
    if (!openglContext || !m_stream) {
        qCWarning(PIPEWIRE_LOGGING) << "need a window and a context" << window();
        return;
    }

    m_createNextTexture = [this, format, planes]() -> QSGTexture * {
        const EGLDisplay display = static_cast<EGLDisplay>(QGuiApplication::platformNativeInterface()->nativeResourceForIntegration("egldisplay"));
        if (m_image) {
            static auto eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
            eglDestroyImageKHR(display, m_image);
        }
        const auto size = m_stream->size();
        const EGLContext context = static_cast<EGLContext>(QGuiApplication::platformNativeInterface()->nativeResourceForIntegration("eglcontext"));
        m_image = GLHelpers::createImage(display, context, planes, PipeWireSourceStream::spaVideoFormatToDrmFormat(format), size);
        if (m_image == EGL_NO_IMAGE_KHR) {
            m_stream->renegotiateModifierFailed(format, planes.constFirst().modifier);
            return nullptr;
        }
        if (!m_texture) {
            m_texture.reset(new QOpenGLTexture(QOpenGLTexture::Target2D));
            bool created = m_texture->create();
            Q_ASSERT(created);
        }

        GLHelpers::initDebugOutput();
        m_texture->bind();

        s_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)m_image);

        m_texture->setWrapMode(QOpenGLTexture::ClampToEdge);
        m_texture->setMinMagFilters(QOpenGLTexture::Linear, QOpenGLTexture::Linear);
        m_texture->release();
        m_texture->setSize(size.width(), size.height());

        int textureId = m_texture->textureId();
        QQuickWindow::CreateTextureOption textureOption =
            format == SPA_VIDEO_FORMAT_ARGB || format == SPA_VIDEO_FORMAT_BGRA ? QQuickWindow::TextureHasAlphaChannel : QQuickWindow::TextureIsOpaque;
        setEnabled(true);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        return window()->createTextureFromNativeObject(QQuickWindow::NativeObjectTexture, &textureId, 0 /*a vulkan thing?*/, size, textureOption);
#else
        return QNativeInterface::QSGOpenGLTexture::fromNative(textureId, window(), size, textureOption);
#endif
    };
    if (window()->isVisible()) {
        update();
    }
}

void PipeWireSourceItem::updateTextureImage(const QImage &image)
{
    if (!window()) {
        qCWarning(PIPEWIRE_LOGGING) << "pass";
        return;
    }

    m_createNextTexture = [this, image] {
        setEnabled(true);
        return window()->createTextureFromImage(image, QQuickWindow::TextureIsOpaque);
    };
    if (window()->isVisible())
        update();
}

void PipeWireSourceItem::moveCursor(const std::optional<QPoint> &position, const QPoint &hotspot, const QImage &texture)
{
    // we don't need to schedule a new repaint because we should be getting a new texture anyway
    m_cursor.position = position;
    m_cursor.hotspot = hotspot;
    if (!texture.isNull()) {
        m_cursor.dirty = true;
        m_cursor.texture = texture;
    }
}

void PipeWireSourceItem::componentComplete()
{
    QQuickItem::componentComplete();
    if (m_nodeId != 0) {
        refresh();
    }
}
