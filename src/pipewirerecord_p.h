/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QFile>
#include <QRunnable>
#include <QThread>
#include <QWaitCondition>
#include <functional>

#include <epoxy/egl.h>
#include <pipewire/pipewire.h>
#include <spa/param/format-utils.h>
#include <spa/param/props.h>
#include <spa/param/video/format-utils.h>

class PipeWireSourceStream;
struct DmaBufPlane;
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

private:
    friend class PipeWireRecordProduceThread;
    void setupEGL();
    void setupStream();
    void updateTextureDmaBuf(const QVector<DmaBufPlane> &plane, uint32_t format);
    void updateTextureImage(const QImage &image);

    AVCodecContext *m_avCodecContext = nullptr;
    const AVCodec *m_codec = nullptr;
    AVFormatContext *m_avFormatContext = nullptr;
    const QString m_output;
    const uint m_nodeId;
    QScopedPointer<PipeWireSourceStream> m_stream;

    struct EGLStruct {
        EGLDisplay display = EGL_NO_DISPLAY;
        EGLContext context = EGL_NO_CONTEXT;
    };

    bool m_eglInitialized = false;
    qint32 m_drmFd = 0; // for GBM buffer mmap
    gbm_device *m_gbmDevice = nullptr; // for passed GBM buffer retrieval

    EGLStruct m_egl;
    PipeWireRecordWriteThread *m_writeThread;
    QWaitCondition m_bufferNotEmpty;
    const QByteArray m_encoder;

    QScopedPointer<CustomAVFrame> m_frame;
};

class PipeWireRecordProduceThread : public QThread
{
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
    bool m_lastRecordThreadFinished = true;
    QByteArray m_encoder;
};
