/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>
    SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QObject>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavfilter/avfilter.h"
}

#undef av_err2str
// The one provided by libav fails to compile on GCC due to passing data from the function scope outside
char *av_err2str(int errnum);

struct PipeWireFrame;
class PipeWireProduce;

class Encoder : public QObject
{
    Q_OBJECT
public:
    Encoder(PipeWireProduce *produce);
    ~Encoder() override;

    virtual bool initialize(const QSize &size) = 0;
    virtual void filterFrame(const PipeWireFrame &frame) = 0;
    virtual void encodeFrame();
    virtual void receivePacket();

    AVCodecContext *avCodecContext() const;

protected:
    PipeWireProduce *m_produce;

    AVCodecContext *m_avCodecContext = nullptr;
    AVFilterGraph *m_avFilterGraph = nullptr;
    AVFilterContext *m_inputFilter = nullptr;
    AVFilterContext *m_outputFilter = nullptr;
};

class SoftwareEncoder : public Encoder
{
public:
    SoftwareEncoder(PipeWireProduce *produce);

    void filterFrame(const PipeWireFrame &frame) override;
};

class HardwareEncoder : public Encoder
{
public:
    HardwareEncoder(PipeWireProduce *produce);

    void filterFrame(const PipeWireFrame &frame) override;

protected:
    QByteArray checkVaapi(const QSize &size);
    bool createDrmContext(const QByteArray &path, const QSize &size);

    AVBufferRef *m_drmContext;
    AVBufferRef *m_drmFramesContext;
};
