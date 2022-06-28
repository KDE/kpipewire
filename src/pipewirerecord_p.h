/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QFile>
#include <QImage>
#include <QPoint>
#include <QRunnable>
#include <QThread>
#include <QWaitCondition>

#include <functional>
#include <optional>

#include <epoxy/egl.h>
#include <pipewire/pipewire.h>
#include <spa/param/format-utils.h>
#include <spa/param/props.h>
#include <spa/param/video/format-utils.h>

#include "pipewiresourcestream.h"

struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct AVFormatContext;
struct AVPacket;
class CustomAVFrame;
struct gbm_device;

class PipeWireRecordWriteThread : public QRunnable
{
public:
    PipeWireRecordWriteThread(QWaitCondition *notEmpty, AVFormatContext *avFormatContext, AVCodecContext *avCodecContext);
    ~PipeWireRecordWriteThread();

    void run() override;
    void drain();

private:
    QAtomicInt m_active = true;
    AVPacket *m_packet;
    AVFormatContext *const m_avFormatContext;
    AVCodecContext *const m_avCodecContext;
    QWaitCondition *const m_bufferNotEmpty;
};

class PipeWireRecordProduce : public QObject
{
public:
    PipeWireRecordProduce(const QByteArray &encoder, uint nodeId, uint fd, const QString &output);
    ~PipeWireRecordProduce() override;

    void finish();
    QString error() const
    {
        return m_error;
    }

private:
    friend class PipeWireRecordProduceThread;
    void setupEGL();
    void setupStream();
    void processFrame(const PipeWireFrame &frame);
    void updateTextureDmaBuf(const DmaBufAttributes &plane, spa_video_format format);
    void updateTextureImage(const QImage &image);
    void render();

    AVCodecContext *m_avCodecContext = nullptr;
    const AVCodec *m_codec = nullptr;
    AVFormatContext *m_avFormatContext = nullptr;
    const QString m_output;
    const uint m_nodeId;
    QScopedPointer<PipeWireSourceStream> m_stream;
    QString m_error;

    struct EGLStruct {
        EGLDisplay display = EGL_NO_DISPLAY;
        EGLContext context = EGL_NO_CONTEXT;
    };

    bool m_eglInitialized = false;
    qint32 m_drmFd = 0; // for GBM buffer mmap
    gbm_device *m_gbmDevice = nullptr; // for passed GBM buffer retrieval

    EGLStruct m_egl;
    PipeWireRecordWriteThread *m_writeThread = nullptr;
    QWaitCondition m_bufferNotEmpty;
    const QByteArray m_encoder;

    QScopedPointer<CustomAVFrame> m_frame;

    struct {
        QImage texture;
        std::optional<QPoint> position;
        QPoint hotspot;
        bool dirty = false;
    } m_cursor;
    QImage m_frameWithoutMetadataCursor;
};

class PipeWireRecordProduceThread : public QThread
{
    Q_OBJECT
public:
    PipeWireRecordProduceThread(const QByteArray &encoder, uint nodeId, uint fd, const QString &output)
        : m_nodeId(nodeId)
        , m_fd(fd)
        , m_output(output)
        , m_encoder(encoder)
    {
    }
    void run() override;
    void deactivate();

Q_SIGNALS:
    void errorFound(const QString &error);

private:
    const uint m_nodeId;
    const uint m_fd;
    const QString m_output;
    PipeWireRecordProduce *m_producer = nullptr;
    const QByteArray m_encoder;
};

struct PipeWireRecordPrivate {
    uint m_nodeId = 0;
    uint m_fd = 0;
    bool m_active = false;
    QString m_output;
    PipeWireRecordProduceThread *m_recordThread = nullptr;
    bool m_produceThreadFinished = true;
    QByteArray m_encoder;
};
