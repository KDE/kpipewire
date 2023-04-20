/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QObject>
#include <epoxy/egl.h>

extern "C" {
#include <libavfilter/avfilter.h>
#include <pipewire/pipewire.h>
#include <spa/param/format-utils.h>
#include <spa/param/props.h>
#include <spa/param/video/format-utils.h>
#include <va/va.h>
#include <va/va_drm.h>
}

#include <QFile>
#include <QImage>
#include <QMutex>
#include <QPoint>
#include <QQueue>
#include <QRunnable>
#include <QThread>
#include <QWaitCondition>

#include <functional>
#include <optional>

#include "dmabufhandler.h"
#include "pipewirebaseencodedstream.h"
#include "pipewiresourcestream.h"
#include "vaapiutils_p.h"

struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

struct AVFilterContext;
struct AVFilterGraph;

class CustomAVFrame;
class PipeWireReceiveEncodedThread;

#undef av_err2str
// The one provided by libav fails to compile on GCC due to passing data from the function scope outside
char *av_err2str(int errnum);

struct PipeWireRecordFrame {
    QImage image;
    std::optional<int> sequential;
    std::optional<std::chrono::nanoseconds> presentationTimestamp;
};

class PipeWireProduce : public QObject
{
    Q_OBJECT
public:
    PipeWireProduce(PipeWireBaseEncodedStream::Encoder encoder, uint nodeId, uint fd, const std::optional<Fraction> &framerate);
    ~PipeWireProduce() override;

    QString error() const
    {
        return m_error;
    }

    Fraction maxFramerate() const;
    void setMaxFramerate(const Fraction &framerate);

    virtual int64_t framePts(const std::optional<std::chrono::nanoseconds> &presentationTimestamp)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(presentationTimestamp.value()).count();
    }

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
    void render(const QImage &image, const PipeWireFrame &frame);
    virtual void aboutToEncode(QImage &frame)
    {
        Q_UNUSED(frame);
    }

    AVCodecContext *m_avCodecContext = nullptr;
    const AVCodec *m_codec = nullptr;
    const uint m_nodeId;
    QScopedPointer<PipeWireSourceStream> m_stream;
    QString m_error;

    PipeWireBaseEncodedStream::Encoder m_encoder;
    QByteArray m_encoderName;

    struct {
        QImage texture;
        std::optional<QPoint> position;
        QPoint hotspot;
        bool dirty = false;
    } m_cursor;
    DmaBufHandler m_dmabufHandler;
    QAtomicInt m_deactivated = false;
    PipeWireReceiveEncodedThread *m_writeThread = nullptr;

    QMutex m_framesMutex;
    QQueue<PipeWireRecordFrame> m_frames;
    QAtomicInt m_processing = false;
    void enqueueFrame(const PipeWireRecordFrame &frame);
    PipeWireRecordFrame dequeueFrame(int *remaining);

    AVFilterGraph *m_avFilterGraph;
    AVFilterContext *m_bufferFilter;
    AVFilterContext *m_formatFilter;
    AVFilterContext *m_outputFilter;
    AVFilterContext *m_inputFormatFilter;
    AVFilterContext *m_uploadFilter;

    VaapiUtils m_vaapi;

Q_SIGNALS:
    void producedFrames();

private:
    void initFiltersVaapi();
    void initFiltersSoftware();
};

class PipeWireProduceThread : public QThread
{
    Q_OBJECT
public:
    PipeWireProduceThread(PipeWireBaseEncodedStream::Encoder encoder, uint nodeId, uint fd, PipeWireBaseEncodedStream *base)
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
    const PipeWireBaseEncodedStream::Encoder m_encoder;
    PipeWireBaseEncodedStream *const m_base;
};

class PipeWireReceiveEncoded : public QObject
{
public:
    PipeWireReceiveEncoded(PipeWireProduce *produce, AVCodecContext *avCodecContext);
    ~PipeWireReceiveEncoded();

    void addFrame();

private:
    QAtomicInt m_active = true;
    AVPacket *m_packet;
    AVCodecContext *const m_avCodecContext;
    PipeWireProduce *const m_produce;
    struct SwsContext *sws_context = nullptr;
    int64_t m_lastPts = -1;
    uint m_lastKeyFrame = 0;
    QSize m_lastReceivedSize;
};

class PipeWireReceiveEncodedThread : public QThread
{
    Q_OBJECT
public:
    PipeWireReceiveEncodedThread(PipeWireProduce *produce, AVCodecContext *avCodecContext);

    void run() override;

private:
    PipeWireProduce *const m_produce;
    AVCodecContext *const m_avCodecContext;
};

struct PipeWireEncodedStreamPrivate {
    uint m_nodeId = 0;
    std::optional<uint> m_fd;
    std::optional<Fraction> m_maxFramerate;
    bool m_active = false;
    PipeWireBaseEncodedStream::Encoder m_encoder;
    std::unique_ptr<PipeWireProduceThread> m_recordThread;
    bool m_produceThreadFinished = true;
};
