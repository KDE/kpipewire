/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once
#include "pipewireproduce.h"
#include <QRunnable>

struct gbm_device;
struct AVFormatContext;
class PipeWireProduce;

class PipeWireRecordProduce : public PipeWireProduce
{
    Q_OBJECT
public:
    PipeWireRecordProduce(const QByteArray &encoder, uint nodeId, uint fd, const QString &output);

    void processFrame(const PipeWireFrame &frame) override;
    void processPacket(AVPacket *packet) override;
    int64_t framePts(const std::optional<std::chrono::nanoseconds> &presentationTimestamp) override;
    void aboutToEncode(QImage &image) override;
    bool setupFormat() override;
    void cleanup() override;

private:
    const QString m_output;
    AVFormatContext *m_avFormatContext = nullptr;
};

struct PipeWireRecordPrivate {
    QString m_output;
};
