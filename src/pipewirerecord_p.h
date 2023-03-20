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

#include "dmabufhandler.h"
#include "pipewiresourcestream.h"

struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct AVFormatContext;
struct AVPacket;
class CustomAVFrame;
class PipeWireRecordProduce;
struct gbm_device;

class PipeWireRecordWrite : public QObject
{
public:
    PipeWireRecordWrite(PipeWireRecordProduce *produce, AVFormatContext *avFormatContext, AVCodecContext *avCodecContext);
    ~PipeWireRecordWrite();

    void addFrame(const QImage &image, std::optional<int> sequential, std::optional<std::chrono::nanoseconds> presentationTimestamp);

private:
    QAtomicInt m_active = true;
    AVPacket *m_packet;
    AVFormatContext *const m_avFormatContext;
    AVCodecContext *const m_avCodecContext;
    struct SwsContext *sws_context = nullptr;
    int64_t m_lastPts = -1;
    uint m_lastKeyFrame = 0;
    QSize m_lastReceivedSize;
};

class PipeWireRecordWriteThread : public QThread
{
public:
    PipeWireRecordWriteThread(PipeWireRecordProduce *produce, AVFormatContext *avFormatContext, AVCodecContext *avCodecContext);

    void run() override;
    void drain();

private:
    PipeWireRecordProduce *const m_produce;
    AVFormatContext *const m_avFormatContext;
    AVCodecContext *const m_avCodecContext;
};

class PipeWireRecordProduce : public QObject
{
    Q_OBJECT
public:
    PipeWireRecordProduce(const QByteArray &encoder, uint nodeId, uint fd, const QString &output);
    ~PipeWireRecordProduce() override;

    QString error() const
    {
        return m_error;
    }

Q_SIGNALS:
    void producedFrame(const QImage &image, std::optional<int> sequential, std::optional<std::chrono::nanoseconds> presentationTimestamp);

private:
    friend class PipeWireRecordProduceThread;
    void setupStream();
    void processFrame(const PipeWireFrame &frame);
    void updateTextureImage(const QImage &image, const PipeWireFrame &frame);
    void render(const PipeWireFrame &frame);
    void stateChanged(pw_stream_state state);

    AVCodecContext *m_avCodecContext = nullptr;
    const AVCodec *m_codec = nullptr;
    AVFormatContext *m_avFormatContext = nullptr;
    const QString m_output;
    const uint m_nodeId;
    QScopedPointer<PipeWireSourceStream> m_stream;
    QString m_error;

    PipeWireRecordWriteThread *m_writeThread = nullptr;
    const QByteArray m_encoder;

    struct {
        QImage texture;
        std::optional<QPoint> position;
        QPoint hotspot;
        bool dirty = false;
    } m_cursor;
    QImage m_frameWithoutMetadataCursor;
    DmaBufHandler m_dmabufHandler;
    QAtomicInt m_deactivated = false;
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
    std::optional<uint> m_fd;
    bool m_active = false;
    QString m_output;
    std::unique_ptr<PipeWireRecordProduceThread> m_recordThread;
    bool m_produceThreadFinished = true;
    QByteArray m_encoder;
};
