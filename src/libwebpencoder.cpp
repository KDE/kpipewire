/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>
    SPDX-FileCopyrightText: 2023 Marco Martin <mart@kde.org>
    SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
    SPDX-FileCopyrightText: 2024 Noah Davis <noahadvs@gmail.com>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "libwebpencoder_p.h"

#include "pipewireproduce_p.h"

#include <QSize>
#include <QThread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/pixfmt.h>
}

#include "logging_record.h"

LibWebPEncoder::LibWebPEncoder(PipeWireProduce *produce)
    : SoftwareEncoder(produce)
{
}

bool LibWebPEncoder::initialize(const QSize &size)
{
    createFilterGraph(size);

    auto codec = avcodec_find_encoder_by_name("libwebp");
    if (!codec) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "libwebp codec not found";
        return false;
    }

    m_avCodecContext = avcodec_alloc_context3(codec);
    if (!m_avCodecContext) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not allocate video codec context";
        return false;
    }

    Q_ASSERT(!size.isEmpty());
    m_avCodecContext->width = size.width();
    m_avCodecContext->height = size.height();
    m_avCodecContext->pix_fmt = AV_PIX_FMT_YUVA420P;
    m_avCodecContext->time_base = AVRational{1, 1000};

    AVDictionary *options = nullptr;

    applyEncodingPreference(options);

    if (int result = avcodec_open2(m_avCodecContext, codec, &options); result < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not open codec" << av_err2str(result);
        return false;
    }

    return true;
}

int LibWebPEncoder::percentageToAbsoluteQuality(const std::optional<quint8> &quality)
{
    return quality.value_or(-1); // Already 0-100. -1 resets to default.
}

void LibWebPEncoder::applyEncodingPreference([[maybe_unused]] AVDictionary *options)
{
}
