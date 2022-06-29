/*
    Render a PipeWire stream into a QtQuick scene as a standard Item
    SPDX-FileCopyrightText: 2020 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QImage>
#include <QQuickItem>
#include <functional>
#include <optional>

#include <pipewire/pipewire.h>
#include <spa/param/format-utils.h>
#include <spa/param/props.h>
#include <spa/param/video/format-utils.h>

#include <kpipewire_export.h>

struct DmaBufAttributes;
class PipeWireSourceStream;
struct PipeWireFrame;
class QSGTexture;
class QOpenGLTexture;
typedef void *EGLImage;

class PipeWireSourceItemPrivate;

class KPIPEWIRE_EXPORT PipeWireSourceItem : public QQuickItem
{
    Q_OBJECT
    /// Specify the pipewire node id that we want to play
    Q_PROPERTY(uint nodeId READ nodeId WRITE setNodeId NOTIFY nodeIdChanged)
    Q_PROPERTY(uint fd READ fd WRITE setFd NOTIFY fdChanged)
public:
    PipeWireSourceItem(QQuickItem *parent = nullptr);
    ~PipeWireSourceItem() override;

    QSGNode *updatePaintNode(QSGNode *node, UpdatePaintNodeData *data) override;
    Q_SCRIPTABLE QString error() const;

    void setNodeId(uint nodeId);
    uint nodeId() const;

    void setFd(uint fd);
    uint fd() const;

    void componentComplete() override;
    void releaseResources() override;

Q_SIGNALS:
    void nodeIdChanged(uint nodeId);
    void fdChanged(uint fd);

private:
    void itemChange(ItemChange change, const ItemChangeData &data) override;
    void processFrame(const PipeWireFrame &frame);
    void updateTextureDmaBuf(const DmaBufAttributes &attribs, spa_video_format format);
    void updateTextureImage(const QImage &image);
    void refresh();

    QScopedPointer<PipeWireSourceItemPrivate> d;
};
