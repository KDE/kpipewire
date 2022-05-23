/*
    SPDX-FileCopyrightText: 2018-2020 Red Hat Inc
    SPDX-FileCopyrightText: 2020 Aleix Pol Gonzalez <aleixpol@kde.org>
    SPDX-FileContributor: Jan Grulich <jgrulich@redhat.com>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#pragma once

#include <QHash>
#include <QObject>
#include <QSharedPointer>
#include <QSize>
#include <optional>

#include <pipewire/pipewire.h>
#include <spa/param/format-utils.h>
#include <spa/param/props.h>
#include <spa/param/video/format-utils.h>

#include <kpipewire_export.h>

#undef Status

namespace KWin
{
class AbstractEglBackend;
class GLTexture;
}
class PipeWireCore;

typedef void *EGLDisplay;

struct DmaBufPlane {
    int fd; ///< The dmabuf file descriptor
    uint32_t offset; ///< The offset from the start of buffer
    uint32_t stride; ///< The distance from the start of a row to the next row in bytes
    uint64_t modifier = 0; ///< The layout modifier
};

struct Fraction {
    const quint32 numerator;
    const quint32 denominator;
};

struct PipeWireSourceStreamPrivate;

class KPIPEWIRE_EXPORT PipeWireSourceStream : public QObject
{
    Q_OBJECT
public:
    explicit PipeWireSourceStream(QObject *parent);
    ~PipeWireSourceStream();

    static void onStreamParamChanged(void *data, uint32_t id, const struct spa_pod *format);
    static void onStreamStateChanged(void *data, pw_stream_state old, pw_stream_state state, const char *error_message);

    Fraction framerate() const;
    uint nodeId();
    QString error() const;

    QSize size() const;
    bool createStream(uint nodeid, int fd);
    void setActive(bool active);

    void handleFrame(struct pw_buffer *buffer);
    void process();

    bool setAllowDmaBuf(bool allowed);
    std::optional<std::chrono::nanoseconds> currentPresentationTimestamp() const;

Q_SIGNALS:
    void streamReady();
    void startStreaming();
    void stopStreaming();
    void streamParametersChanged();
    void dmabufTextureReceived(const QVector<DmaBufPlane> &planes, uint32_t format);
    void imageTextureReceived(const QImage &image);

private:
    void coreFailed(const QString &errorMessage);
    QScopedPointer<PipeWireSourceStreamPrivate> d;
};

Q_DECLARE_METATYPE(QVector<DmaBufPlane>);
