/*
    Render a PipeWire stream into a QtQuick scene as a standard Item
    SPDX-FileCopyrightText: 2020 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "pipewiresourceitem.h"
#include "glhelpers.h"
#include "logging.h"
#include "pipewiresourcestream.h"
#include "pwhelpers.h"

#include <QGuiApplication>
#include <QOpenGLContext>
#include <QOpenGLTexture>
#include <QPainter>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSocketNotifier>
#include <QThread>
#include <memory>
#include <qpa/qplatformnativeinterface.h>

#include <EGL/eglext.h>
#include <fcntl.h>
#include <libdrm/drm_fourcc.h>
#include <unistd.h>

static void pwInit()
{
    pw_init(nullptr, nullptr);
}
Q_COREAPP_STARTUP_FUNCTION(pwInit);

class PipeWireSourceItemPrivate
{
public:
    std::weak_ptr<QOpenGLTexture> m_sharedGlTex;

    uint m_nodeId = 0;
    std::optional<uint> m_fd;
    std::function<QSGTexture *()> m_createNextTexture;
    std::unique_ptr<PipeWireSourceStream> m_stream;
    bool m_allowDmaBuf = true;
    bool m_ready = false;

    struct {
        QImage texture;
        std::optional<QPoint> position;
        QPoint hotspot;
        bool dirty = false;
    } m_cursor;
    std::optional<QRegion> m_damage;
    QRectF m_paintedRect;
};

PipeWireSourceItem::PipeWireSourceItem(QQuickItem *parent)
    : QQuickItem(parent)
    , d(new PipeWireSourceItemPrivate)
{
    setFlag(ItemHasContents, true);
    connect(this, &PipeWireSourceItem::streamSizeChanged, this, &PipeWireSourceItem::updatePaintedRect);
    connect(this, &PipeWireSourceItem::widthChanged, this, &PipeWireSourceItem::updatePaintedRect);
    connect(this, &PipeWireSourceItem::heightChanged, this, &PipeWireSourceItem::updatePaintedRect);
}

PipeWireSourceItem::~PipeWireSourceItem()
{
    if (d->m_fd) {
        close(*d->m_fd);
    }
}

void PipeWireSourceItem::itemChange(QQuickItem::ItemChange change, const QQuickItem::ItemChangeData &data)
{
    switch (change) {
    case ItemVisibleHasChanged:
        if (!isVisible()) {
            setReady(false);
        }
        if (d->m_stream) {
            d->m_stream->setActive(isVisible());
        }
        break;
    default:
        break;
    }

    QQuickItem::itemChange(change, data);
}

void PipeWireSourceItem::setFd(uint fd)
{
    if (fd == d->m_fd)
        return;

    if (d->m_fd) {
        close(*d->m_fd);
    }
    d->m_fd = fd;
    refresh();
    Q_EMIT fdChanged(fd);
}

void PipeWireSourceItem::resetFd()
{
    if (!d->m_fd.has_value()) {
        return;
    }

    setReady(false);
    close(*d->m_fd);
    d->m_fd.reset();
    d->m_stream.reset(nullptr);
    d->m_createNextTexture = [] {
        return nullptr;
    };
    Q_EMIT streamSizeChanged();
}

void PipeWireSourceItem::refresh()
{
    setReady(false);

    if (!isComponentComplete()) {
        return;
    }

    if (d->m_nodeId == 0) {
        d->m_stream.reset(nullptr);
        Q_EMIT streamSizeChanged();

        d->m_createNextTexture = [] {
            return nullptr;
        };
    } else {
        d->m_stream.reset(new PipeWireSourceStream(this));
        d->m_stream->setAllowDmaBuf(d->m_allowDmaBuf);
        Q_EMIT streamSizeChanged();
        connect(d->m_stream.get(), &PipeWireSourceStream::streamParametersChanged, this, &PipeWireSourceItem::streamSizeChanged);
        connect(d->m_stream.get(), &PipeWireSourceStream::streamParametersChanged, this, &PipeWireSourceItem::usingDmaBufChanged);

        const bool created = d->m_stream->createStream(d->m_nodeId, d->m_fd.value_or(0));
        if (!created || !d->m_stream->error().isEmpty()) {
            d->m_stream.reset(nullptr);
            d->m_nodeId = 0;
            return;
        }
        d->m_stream->setActive(isVisible());

        connect(d->m_stream.get(), &PipeWireSourceStream::frameReceived, this, &PipeWireSourceItem::processFrame);
        connect(d->m_stream.get(), &PipeWireSourceStream::stateChanged, this, &PipeWireSourceItem::stateChanged);
    }
    Q_EMIT stateChanged();
}

void PipeWireSourceItem::setNodeId(uint nodeId)
{
    if (nodeId == d->m_nodeId)
        return;

    d->m_nodeId = nodeId;
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
    if (Q_UNLIKELY(!d->m_createNextTexture)) {
        return node;
    }

    auto texture = d->m_createNextTexture();
    if (!texture) {
        delete node;
        return nullptr;
    }

    auto pwNode = static_cast<PipeWireRenderNode *>(node);
    if (!pwNode) {
        pwNode = new PipeWireRenderNode;
    }

    QSGImageNode *screenNode = pwNode->screenNode(window());
    screenNode->setTexture(texture);
    screenNode->setOwnsTexture(true);
    const auto rect = calculatePaintedRect(texture->textureSize());
    screenNode->setRect(rect);

    if (!d->m_cursor.position.has_value() || d->m_cursor.texture.isNull()) {
        pwNode->discardCursor();
    } else {
        QSGImageNode *cursorNode = pwNode->cursorNode(window());
        if (d->m_cursor.dirty || !cursorNode->texture()) {
            cursorNode->setTexture(window()->createTextureFromImage(d->m_cursor.texture));
            cursorNode->setOwnsTexture(true);
            d->m_cursor.dirty = false;
        }
        const qreal scale = qreal(rect.width()) / texture->textureSize().width();
        cursorNode->setRect(QRectF{rect.topLeft() + (d->m_cursor.position.value() * scale), d->m_cursor.texture.size() * scale});
        Q_ASSERT(cursorNode->texture());
    }

    if (!d->m_damage || d->m_damage->isEmpty()) {
        pwNode->discardDamage();
    } else {
        auto *damageNode = pwNode->damageNode(window());
        QImage damageImage(texture->textureSize(), QImage::Format_RGBA64_Premultiplied);
        damageImage.fill(Qt::transparent);
        QPainter p(&damageImage);
        p.setBrush(Qt::red);
        for (auto rect : *d->m_damage) {
            p.drawRect(rect);
        }
        damageNode->setTexture(window()->createTextureFromImage(damageImage));
        damageNode->setOwnsTexture(true);
        damageNode->setRect(rect);
        Q_ASSERT(damageNode->texture());
    }
    return pwNode;
}

QString PipeWireSourceItem::error() const
{
    return d->m_stream->error();
}

void PipeWireSourceItem::processFrame(const PipeWireFrame &frame)
{
    d->m_damage = frame.damage;

    if (frame.cursor) {
        d->m_cursor.position = frame.cursor->position;
        d->m_cursor.hotspot = frame.cursor->hotspot;
        if (!frame.cursor->texture.isNull()) {
            d->m_cursor.dirty = true;
            d->m_cursor.texture = frame.cursor->texture;
        }
    } else {
        d->m_cursor.position = std::nullopt;
        d->m_cursor.hotspot = {};
    }

    if (frame.dmabuf) {
        updateTextureDmaBuf(*frame.dmabuf, frame.format);
    } else if (frame.dataFrame) {
        updateTextureImage(frame.dataFrame);
    }

    if (window() && window()->isVisible()) {
        update();
    }
}

void PipeWireSourceItem::updateTextureDmaBuf(const DmaBufAttributes &attribs, spa_video_format format)
{
    if (!window()) {
        qCWarning(PIPEWIRE_LOGGING) << "Window not available" << this;
        return;
    }

    const auto openglContext = static_cast<QOpenGLContext *>(window()->rendererInterface()->getResource(window(), QSGRendererInterface::OpenGLContextResource));
    if (!openglContext || !d->m_stream) {
        qCWarning(PIPEWIRE_LOGGING) << "need a window and a context" << window();
        return;
    }

    d->m_createNextTexture = [this, format, attribs]() -> QSGTexture * {
        const EGLDisplay display = static_cast<EGLDisplay>(QGuiApplication::platformNativeInterface()->nativeResourceForIntegration("egldisplay"));
        const auto size = d->m_stream->size();

        EGLImageKHR image = GLHelpers::createImage(display, attribs, PipeWireSourceStream::spaVideoFormatToDrmFormat(format), size, nullptr);
        if (image == EGL_NO_IMAGE_KHR) {
            QMetaObject::invokeMethod(
                d->m_stream.get(),
                [this, format, attribs]() {
                    d->m_stream->renegotiateModifierFailed(format, attribs.modifier);
                },
                Qt::QueuedConnection);
            return nullptr;
        }

        // One raw QOpenGLTexture is used for all QSGTextures
        auto sharedTex = d->m_sharedGlTex.lock();
        if (!sharedTex) {
            auto raw = std::make_shared<QOpenGLTexture>(QOpenGLTexture::Target2D);
            bool created = raw->create();
            Q_ASSERT(created);
            sharedTex = raw;
            d->m_sharedGlTex = sharedTex;
        }

        GLHelpers::initDebugOutput();
        sharedTex->bind();
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)image);
        sharedTex->setWrapMode(QOpenGLTexture::ClampToEdge);
        sharedTex->setMinMagFilters(QOpenGLTexture::Linear, QOpenGLTexture::Linear);
        sharedTex->release();
        sharedTex->setSize(size.width(), size.height());

        const int textureId = sharedTex->textureId();
        const QQuickWindow::CreateTextureOption textureOption =
            (format == SPA_VIDEO_FORMAT_ARGB || format == SPA_VIDEO_FORMAT_BGRA) ? QQuickWindow::TextureHasAlphaChannel : QQuickWindow::TextureIsOpaque;
        QSGTexture *tex = QNativeInterface::QSGOpenGLTexture::fromNative(textureId, window(), size, textureOption);

        QObject::connect(
            tex,
            &QObject::destroyed,
            tex,
            [sharedTex, image, display]() {
                // sharedTex is captured so it gets destroyed when the texture replaces and the lambda goes out of scope
                if (image != EGL_NO_IMAGE_KHR) {
                    eglDestroyImageKHR(display, image);
                }
            },
            Qt::DirectConnection);

        return tex;
    };

    setReady(true);
}

void PipeWireSourceItem::updateTextureImage(const std::shared_ptr<PipeWireFrameData> &data)
{
    if (!window()) {
        qCWarning(PIPEWIRE_LOGGING) << "pass";
        return;
    }

    d->m_createNextTexture = [this, data] {
        return window()->createTextureFromImage(data->toImage(), QQuickWindow::TextureIsOpaque);
    };

    setReady(true);
}

void PipeWireSourceItem::componentComplete()
{
    QQuickItem::componentComplete();
    if (d->m_nodeId != 0) {
        refresh();
    }
}

PipeWireSourceItem::StreamState PipeWireSourceItem::state() const
{
    if (!d->m_stream) {
        return StreamState::Unconnected;
    }
    switch (d->m_stream->state()) {
    case PW_STREAM_STATE_ERROR:
        return StreamState::Error;
    case PW_STREAM_STATE_UNCONNECTED:
        return StreamState::Unconnected;
    case PW_STREAM_STATE_CONNECTING:
        return StreamState::Connecting;
    case PW_STREAM_STATE_PAUSED:
        return StreamState::Paused;
    case PW_STREAM_STATE_STREAMING:
        return StreamState::Streaming;
    default:
        return StreamState::Error;
    }
}

uint PipeWireSourceItem::fd() const
{
    return d->m_fd.value_or(0);
}

uint PipeWireSourceItem::nodeId() const
{
    return d->m_nodeId;
}

QSize PipeWireSourceItem::streamSize() const
{
    if (!d->m_stream) {
        return QSize();
    }
    return d->m_stream->size();
}

bool PipeWireSourceItem::usingDmaBuf() const
{
    return d->m_stream && d->m_stream->usingDmaBuf();
}

bool PipeWireSourceItem::allowDmaBuf() const
{
    return d->m_stream && d->m_stream->allowDmaBuf();
}

void PipeWireSourceItem::setAllowDmaBuf(bool allowed)
{
    d->m_allowDmaBuf = allowed;
    if (d->m_stream) {
        d->m_stream->setAllowDmaBuf(allowed);
    }
}

void PipeWireSourceItem::setReady(bool ready)
{
    if (d->m_ready != ready) {
        d->m_ready = ready;
        Q_EMIT readyChanged();
    }
}

bool PipeWireSourceItem::isReady() const
{
    return d->m_ready;
}

void PipeWireSourceItem::setPaintedRect(const QRectF &rect)
{
    if (rect == d->m_paintedRect) {
        return;
    }

    d->m_paintedRect = rect;
    Q_EMIT paintedRectChanged();
}

QRectF PipeWireSourceItem::paintedRect() const
{
    return d->m_paintedRect;
}

QRect PipeWireSourceItem::calculatePaintedRect(const QSize &size) const
{
    if (size.isNull()) {
        return {};
    }

    const auto bounding = boundingRect().toRect();
    QRect rect({0, 0}, size.scaled(bounding.size(), Qt::KeepAspectRatio));
    rect.moveCenter(bounding.center());
    return rect;
}

void PipeWireSourceItem::updatePaintedRect()
{
    if (!d->m_stream) {
        setPaintedRect(QRectF());
        return;
    }

    setPaintedRect(calculatePaintedRect(d->m_stream->size()));
}

#include "moc_pipewiresourceitem.cpp"
