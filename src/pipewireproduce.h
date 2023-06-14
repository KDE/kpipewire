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
#include <QQueue>
#include <QRunnable>
#include <QThread>
#include <QWaitCondition>

#include <functional>
#include <optional>
#include <thread>

#include "pipewirebaseencodedstream.h"
#include "pipewiresourcestream.h"

struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

struct AVFilterContext;
struct AVFilterGraph;

class CustomAVFrame;
class Encoder;
class PipeWireReceiveEncodedThread;

class PipeWireProduce : public QObject
{
    Q_OBJECT
public:
    PipeWireProduce(PipeWireBaseEncodedStream::Encoder encoderType, uint nodeId, uint fd, const std::optional<Fraction> &framerate);
    ~PipeWireProduce() override;

    virtual void initialize();

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
    virtual void aboutToEncode(PipeWireFrame &frame)
    {
        Q_UNUSED(frame);
    }

    void deactivate();

    const uint m_nodeId;
    QScopedPointer<PipeWireSourceStream> m_stream;
    QString m_error;

    PipeWireBaseEncodedStream::Encoder m_encoderType;
    QByteArray m_encoderName;
    std::unique_ptr<Encoder> m_encoder;

    uint m_fd;
    std::optional<Fraction> m_frameRate;

    struct {
        QImage texture;
        std::optional<QPoint> position;
        QPoint hotspot;
        bool dirty = false;
    } m_cursor;

    std::jthread m_passthroughThread;
    std::jthread m_outputThread;

    std::atomic_bool m_deactivated = false;

Q_SIGNALS:
    void producedFrames();

private:
    void initFiltersVaapi();
    void initFiltersSoftware();
};
