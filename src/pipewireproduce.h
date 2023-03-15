/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QObject>
#include <epoxy/egl.h>

extern "C" {
#include <pipewire/pipewire.h>
#include <spa/param/format-utils.h>
#include <spa/param/props.h>
#include <spa/param/video/format-utils.h>
}

#include <QFile>
#include <QImage>
#include <QMutex>
#include <QPoint>
#include <QRunnable>
#include <QThread>
#include <QWaitCondition>

#include <functional>
#include <optional>

#include "dmabufhandler.h"
#include "pipewirebaseencodedstream.h"
#include "pipewiresourcestream.h"

struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct AVFormatContext;
struct AVPacket;
class CustomAVFrame;
class PipeWireReceiveEncodedThread;

#undef av_err2str
// The one provided by libav fails to compile on GCC due to passing data from the function scope outside
char *av_err2str(int errnum);

class PipeWireProduce : public QObject
{
    Q_OBJECT
public:
    PipeWireProduce(const QByteArray &encoder, uint nodeId, uint fd);
    ~PipeWireProduce() override;

    QString error() const
    {
        return m_error;
    }
    virtual int64_t framePts(const PipeWireFrame &frame) = 0;

    virtual void processPacket(AVPacket *packet) = 0;
    virtual bool setupFormat()
    {
        return true;
    }
    virtual void cleanup()
    {
    }

    void stateChanged(pw_stream_state state);
    friend class PipeWireProduceThread;
    void setupStream();
    virtual void processFrame(const PipeWireFrame &frame);
    void render(const PipeWireFrame &frame);
    virtual void aboutToEncode(QImage &frame)
    {
        Q_UNUSED(frame);
    }

    AVCodecContext *m_avCodecContext = nullptr;
    const AVCodec *m_codec = nullptr;
    const uint m_nodeId;
    QScopedPointer<PipeWireSourceStream> m_stream;
    QString m_error;

    QWaitCondition m_bufferNotEmpty;
    const QByteArray m_encoder;

    struct {
        QImage texture;
        std::optional<QPoint> position;
        QPoint hotspot;
        bool dirty = false;
    } m_cursor;
    QImage m_frameWithoutMetadataCursor;
    DmaBufHandler m_dmabufHandler;
    uint m_lastKeyFrame = 0;
    int64_t m_lastPts = -1;
    QAtomicInt m_deactivated = false;
    PipeWireReceiveEncodedThread *m_writeThread = nullptr;
    QMutex m_readyToWrite;
};

class PipeWireProduceThread : public QThread
{
    Q_OBJECT
public:
    PipeWireProduceThread(const QByteArray &encoder, uint nodeId, uint fd, PipeWireBaseEncodedStream *base)
        : m_nodeId(nodeId)
        , m_fd(fd)
        , m_encoder(encoder)
        , m_base(base)
    {
    }
    void run() override;
    void deactivate();

Q_SIGNALS:
    void errorFound(const QString &error);

protected:
    friend class PipeWireProduce;
    const uint m_nodeId;
    const uint m_fd;
    PipeWireProduce *m_producer = nullptr;
    const QByteArray m_encoder;
    PipeWireBaseEncodedStream *const m_base;
};

class PipeWireReceiveEncodedThread : public QRunnable
{
public:
    PipeWireReceiveEncodedThread(AVCodecContext *avCodecContext, PipeWireProduce *producer);
    ~PipeWireReceiveEncodedThread();

    void run() override;

private:
    friend class PipeWireProduce;
    QAtomicInt m_active = true;
    AVPacket *m_packet;
    AVCodecContext *const m_avCodecContext;
    PipeWireProduce *const m_producer;
};

struct PipeWireEncodedStreamPrivate {
    uint m_nodeId = 0;
    std::optional<uint> m_fd;
    bool m_active = false;
    QByteArray m_encoder;
    std::unique_ptr<PipeWireProduceThread> m_recordThread;
    bool m_produceThreadFinished = true;
};
