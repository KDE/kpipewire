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
#include <QRunnable>
#include <QSGImageNode>
#include <QSGRendererInterface>
#include <QSocketNotifier>
#include <QThread>
#include <QtGui/qvulkanfunctions.h>
#include <QtGui/qvulkaninstance.h>
#include <qpa/qplatformnativeinterface.h>
#include <vulkan/vulkan.h>

#include <EGL/eglext.h>
#if QT_CONFIG(vulkan)
#ifndef VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT
#define VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT static_cast<VkImageTiling>(1000158000)
#endif
#endif
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
    uint m_nodeId = 0;
    std::optional<uint> m_fd;
    std::function<QSGTexture *()> m_createNextTexture;
    std::unique_ptr<PipeWireSourceStream> m_stream;
    std::unique_ptr<QOpenGLTexture> m_texture;

    EGLImage m_image = nullptr;
    bool m_needsRecreateTexture = false;
    bool m_allowDmaBuf = true;
    bool m_ready = false;

    // Vulkan wrapping of dmabuf
    VkImage m_vkImage = VK_NULL_HANDLE;
    VkDeviceMemory m_vkMemory = VK_NULL_HANDLE;
    VkDevice m_vkDevice = VK_NULL_HANDLE;
    QVulkanInstance *m_vkInstance = nullptr; // not owned

    struct {
        QImage texture;
        std::optional<QPoint> position;
        QPoint hotspot;
        bool dirty = false;
    } m_cursor;
    std::optional<QRegion> m_damage;
    QRectF m_paintedRect;
};

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
            eglDestroyImageKHR(eglGetCurrentDisplay(), m_image);
        }

        delete m_texture;
    }

private:
    const EGLImageKHR m_image;
    QOpenGLTexture *m_texture;
};

class DiscardVulkanImageRunnable : public QRunnable
{
public:
    DiscardVulkanImageRunnable(VkDevice device, VkImage image, VkDeviceMemory memory, PFN_vkDestroyImage pfnDestroyImage, PFN_vkFreeMemory pfnFreeMemory)
        : m_device(device)
        , m_image(image)
        , m_memory(memory)
        , m_pfnDestroyImage(pfnDestroyImage)
        , m_pfnFreeMemory(pfnFreeMemory)
    {
    }

    void run() override
    {
        if (m_device == VK_NULL_HANDLE)
            return;
        if (m_image != VK_NULL_HANDLE && m_pfnDestroyImage) {
            m_pfnDestroyImage(m_device, m_image, nullptr);
        }
        if (m_memory != VK_NULL_HANDLE && m_pfnFreeMemory) {
            m_pfnFreeMemory(m_device, m_memory, nullptr);
        }
    }

private:
    VkDevice m_device;
    VkImage m_image;
    VkDeviceMemory m_memory;
    PFN_vkDestroyImage m_pfnDestroyImage;
    PFN_vkFreeMemory m_pfnFreeMemory;
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
    case ItemSceneChange:
        d->m_needsRecreateTexture = true;
        releaseResources();
        break;
    default:
        break;
    }

    QQuickItem::itemChange(change, data);
}

void PipeWireSourceItem::releaseResources()
{
    if (window() && (d->m_image || d->m_texture)) {
        window()->scheduleRenderJob(new DiscardEglPixmapRunnable(d->m_image, d->m_texture.release()), QQuickWindow::NoStage);
        d->m_image = EGL_NO_IMAGE_KHR;
    }
    // Release Vulkan resources if any
    if (window() && (d->m_vkImage != VK_NULL_HANDLE || d->m_vkMemory != VK_NULL_HANDLE)) {
        QVulkanInstance *inst =
            static_cast<QVulkanInstance *>(window()->rendererInterface()->getResource(window(), QSGRendererInterface::VulkanInstanceResource));
        auto f = inst ? inst->functions() : nullptr;
        PFN_vkDestroyImage pfnDestroyImage = nullptr;
        PFN_vkFreeMemory pfnFreeMemory = nullptr;
        if (f && d->m_vkDevice != VK_NULL_HANDLE) {
            pfnDestroyImage = reinterpret_cast<PFN_vkDestroyImage>(f->vkGetDeviceProcAddr(d->m_vkDevice, "vkDestroyImage"));
            pfnFreeMemory = reinterpret_cast<PFN_vkFreeMemory>(f->vkGetDeviceProcAddr(d->m_vkDevice, "vkFreeMemory"));
        }
        window()->scheduleRenderJob(new DiscardVulkanImageRunnable(d->m_vkDevice, d->m_vkImage, d->m_vkMemory, pfnDestroyImage, pfnFreeMemory),
                                    QQuickWindow::NoStage);
        d->m_vkImage = VK_NULL_HANDLE;
        d->m_vkMemory = VK_NULL_HANDLE;
        d->m_vkDevice = VK_NULL_HANDLE;
    }
}

void PipeWireSourceItem::invalidateSceneGraph()
{
    if (d->m_image != EGL_NO_IMAGE_KHR) {
        eglDestroyImageKHR(eglGetCurrentDisplay(), d->m_image);
        d->m_image = EGL_NO_IMAGE_KHR;
    }
    d->m_texture.reset();
    if (d->m_vkImage != VK_NULL_HANDLE || d->m_vkMemory != VK_NULL_HANDLE) {
        // Destruction of Vulkan resources should happen on the render thread via releaseResources
        // Here we just clear local references; releaseResources will schedule proper cleanup.
    }
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
        releaseResources();
        d->m_stream.reset(nullptr);
        Q_EMIT streamSizeChanged();

        d->m_createNextTexture = [] {
            return nullptr;
        };
    } else {
        d->m_stream.reset(new PipeWireSourceStream(this));
        // Prefer zero-copy via dmabuf on OpenGL and Vulkan; fall back on others
        bool allowDmabuf = d->m_allowDmaBuf;
        if (window()) {
            const auto api = window()->rendererInterface()->graphicsApi();
            if (api != QSGRendererInterface::OpenGL && api != QSGRendererInterface::Vulkan) {
                allowDmabuf = false;
            }
        }
        d->m_stream->setAllowDmaBuf(allowDmabuf);
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

    // Ensure we have an active RHI-based scene graph and a valid stream
    void *rhi = window()->rendererInterface()->getResource(window(), QSGRendererInterface::RhiResource);
    if (!rhi || !d->m_stream) {
        qCWarning(PIPEWIRE_LOGGING) << "Need a window and an RHI context" << window();
        return;
    }

    const auto api = window()->rendererInterface()->graphicsApi();

    if (api == QSGRendererInterface::Vulkan) {
        d->m_createNextTexture = [this, format, attribs]() -> QSGTexture * {
            QVulkanInstance *inst =
                static_cast<QVulkanInstance *>(window()->rendererInterface()->getResource(window(), QSGRendererInterface::VulkanInstanceResource));
            VkDevice device = reinterpret_cast<VkDevice>(window()->rendererInterface()->getResource(window(), QSGRendererInterface::DeviceResource));
            VkPhysicalDevice phys =
                reinterpret_cast<VkPhysicalDevice>(window()->rendererInterface()->getResource(window(), QSGRendererInterface::PhysicalDeviceResource));
            auto renegotiate = [&]() -> QSGTexture * {
                QMetaObject::invokeMethod(
                    d->m_stream.get(),
                    [this, format, attribs]() {
                        d->m_stream->renegotiateModifierFailed(format, attribs.modifier);
                    },
                    Qt::QueuedConnection);
                return nullptr;
            };
            if (!inst || device == VK_NULL_HANDLE || phys == VK_NULL_HANDLE) {
                qCWarning(PIPEWIRE_LOGGING) << "Vulkan resources not available";
                return renegotiate();
            }

            const auto size = d->m_stream->size();
            if (attribs.planes.size() != 1 || attribs.modifier == DRM_FORMAT_MOD_INVALID) {
                return renegotiate();
            }

            auto f = inst->functions();

            auto spaToVk = [](spa_video_format f) -> VkFormat {
                switch (f) {
                case SPA_VIDEO_FORMAT_BGRA:
                case SPA_VIDEO_FORMAT_ARGB:
                    return VK_FORMAT_B8G8R8A8_UNORM;
                case SPA_VIDEO_FORMAT_RGBA:
                case SPA_VIDEO_FORMAT_ABGR:
                    return VK_FORMAT_R8G8B8A8_UNORM;
                case SPA_VIDEO_FORMAT_BGRx:
                case SPA_VIDEO_FORMAT_RGBx:
                    return VK_FORMAT_B8G8R8A8_UNORM;
                default:
                    return VK_FORMAT_UNDEFINED;
                }
            };

            VkFormat vkFormat = spaToVk(format);
            if (vkFormat == VK_FORMAT_UNDEFINED) {
                return renegotiate();
            }

            VkSubresourceLayout planeLayout{
                .offset = attribs.planes[0].offset,
                .rowPitch = attribs.planes[0].stride,
            };

            VkImageDrmFormatModifierExplicitCreateInfoEXT drmInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
                .drmFormatModifier = attribs.modifier,
                .drmFormatModifierPlaneCount = 1,
                .pPlaneLayouts = &planeLayout,
            };

            VkExternalMemoryImageCreateInfo extMemInfo{
                .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                .pNext = &drmInfo,
                .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
            };

            VkImageCreateInfo ci{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .pNext = &extMemInfo,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = vkFormat,
                .extent = {static_cast<uint32_t>(size.width()), static_cast<uint32_t>(size.height()), 1u},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
            };

            VkImage image = VK_NULL_HANDLE;
            auto pfnCreateImage = reinterpret_cast<PFN_vkCreateImage>(f->vkGetDeviceProcAddr(device, "vkCreateImage"));
            auto pfnGetImageMemReq = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(f->vkGetDeviceProcAddr(device, "vkGetImageMemoryRequirements"));
            auto pfnAllocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(f->vkGetDeviceProcAddr(device, "vkAllocateMemory"));
            auto pfnBindImageMemory = reinterpret_cast<PFN_vkBindImageMemory>(f->vkGetDeviceProcAddr(device, "vkBindImageMemory"));
            auto pfnDestroyImage = reinterpret_cast<PFN_vkDestroyImage>(f->vkGetDeviceProcAddr(device, "vkDestroyImage"));
            auto pfnFreeMemory = reinterpret_cast<PFN_vkFreeMemory>(f->vkGetDeviceProcAddr(device, "vkFreeMemory"));
            if (!pfnCreateImage || !pfnGetImageMemReq || !pfnAllocateMemory || !pfnBindImageMemory) {
                qCWarning(PIPEWIRE_LOGGING) << "Failed to resolve required Vulkan device functions";
                return renegotiate();
            }

            if (pfnCreateImage(device, &ci, nullptr, &image) != VK_SUCCESS || image == VK_NULL_HANDLE) {
                qCWarning(PIPEWIRE_LOGGING) << "vkCreateImage failed for dmabuf";
                return renegotiate();
            }

            VkMemoryRequirements memReq{};
            pfnGetImageMemReq(device, image, &memReq);
            VkPhysicalDeviceMemoryProperties memProps{};
            f->vkGetPhysicalDeviceMemoryProperties(phys, &memProps);
            uint32_t memTypeIndex = UINT32_MAX;
            for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
                if ((memReq.memoryTypeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                    memTypeIndex = i;
                    break;
                }
            }
            if (memTypeIndex == UINT32_MAX) {
                qCWarning(PIPEWIRE_LOGGING) << "No suitable memory type for dmabuf image";
                if (pfnDestroyImage)
                    pfnDestroyImage(device, image, nullptr);
                return renegotiate();
            }

            VkImportMemoryFdInfoKHR importInfo{
                .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                .fd = attribs.planes[0].fd,
            };

            VkMemoryAllocateInfo ai{
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .pNext = &importInfo,
                .allocationSize = memReq.size,
                .memoryTypeIndex = memTypeIndex,
            };

            VkDeviceMemory memory = VK_NULL_HANDLE;
            if (pfnAllocateMemory(device, &ai, nullptr, &memory) != VK_SUCCESS || memory == VK_NULL_HANDLE) {
                qCWarning(PIPEWIRE_LOGGING) << "vkAllocateMemory failed for dmabuf";
                if (pfnDestroyImage)
                    pfnDestroyImage(device, image, nullptr);
                return renegotiate();
            }

            if (pfnBindImageMemory(device, image, memory, 0) != VK_SUCCESS) {
                qCWarning(PIPEWIRE_LOGGING) << "vkBindImageMemory failed";
                if (pfnFreeMemory)
                    pfnFreeMemory(device, memory, nullptr);
                if (pfnDestroyImage)
                    pfnDestroyImage(device, image, nullptr);
                return renegotiate();
            }

            if (d->m_vkImage != VK_NULL_HANDLE || d->m_vkMemory != VK_NULL_HANDLE) {
                window()->scheduleRenderJob(new DiscardVulkanImageRunnable(device, d->m_vkImage, d->m_vkMemory, pfnDestroyImage, pfnFreeMemory),
                                            QQuickWindow::NoStage);
            }
            d->m_vkDevice = device;
            d->m_vkImage = image;
            d->m_vkMemory = memory;

            QQuickWindow::CreateTextureOption textureOption =
                (format == SPA_VIDEO_FORMAT_ARGB || format == SPA_VIDEO_FORMAT_BGRA) ? QQuickWindow::TextureHasAlphaChannel : QQuickWindow::TextureIsOpaque;
            return QNativeInterface::QSGVulkanTexture::fromNative(image, VK_IMAGE_LAYOUT_GENERAL, window(), size, textureOption);
        };

        setReady(true);
        return;
    }

    if (api == QSGRendererInterface::OpenGL) {
        d->m_createNextTexture = [this, format, attribs]() -> QSGTexture * {
            const EGLDisplay display = static_cast<EGLDisplay>(QGuiApplication::platformNativeInterface()->nativeResourceForIntegration("egldisplay"));
            if (d->m_image) {
                eglDestroyImageKHR(display, d->m_image);
            }
            const auto size = d->m_stream->size();
            d->m_image = GLHelpers::createImage(display, attribs, PipeWireSourceStream::spaVideoFormatToDrmFormat(format), size, nullptr);
            if (d->m_image == EGL_NO_IMAGE_KHR) {
                QMetaObject::invokeMethod(
                    d->m_stream.get(),
                    [this, format, attribs]() {
                        d->m_stream->renegotiateModifierFailed(format, attribs.modifier);
                    },
                    Qt::QueuedConnection);
                return nullptr;
            }
            if (!d->m_texture) {
                d->m_texture.reset(new QOpenGLTexture(QOpenGLTexture::Target2D));
                bool created = d->m_texture->create();
                Q_ASSERT(created);
            }

            GLHelpers::initDebugOutput();
            d->m_texture->bind();

            glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)d->m_image);

            d->m_texture->setWrapMode(QOpenGLTexture::ClampToEdge);
            d->m_texture->setMinMagFilters(QOpenGLTexture::Linear, QOpenGLTexture::Linear);
            d->m_texture->release();
            d->m_texture->setSize(size.width(), size.height());

            int textureId = d->m_texture->textureId();
            QQuickWindow::CreateTextureOption textureOption =
                format == SPA_VIDEO_FORMAT_ARGB || format == SPA_VIDEO_FORMAT_BGRA ? QQuickWindow::TextureHasAlphaChannel : QQuickWindow::TextureIsOpaque;
            return QNativeInterface::QSGOpenGLTexture::fromNative(textureId, window(), size, textureOption);
        };

        setReady(true);
        return;
    }
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
