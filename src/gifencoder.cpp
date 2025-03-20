/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>
    SPDX-FileCopyrightText: 2023 Marco Martin <mart@kde.org>
    SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
    SPDX-FileCopyrightText: 2024 Noah Davis <noahadvs@gmail.com>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "gifencoder_p.h"

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

using namespace Qt::StringLiterals;

GifEncoder::GifEncoder(PipeWireProduce *produce)
    : SoftwareEncoder(produce)
{
}

bool GifEncoder::initialize(const QSize &size)
{
    m_filterGraphToParse = u"split[v1][v2];[v1]palettegen=stats_mode=single[palette];[v2][palette]paletteuse=new=1:dither=sierra2_4a"_s;
    createFilterGraph(size);

    auto codec = avcodec_find_encoder_by_name("gif");
    if (!codec) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "gif codec not found";
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
    m_avCodecContext->pix_fmt = AV_PIX_FMT_PAL8;
    m_avCodecContext->time_base = AVRational{1, 1000};

    AVDictionary *options = nullptr;
    if (int result = avcodec_open2(m_avCodecContext, codec, &options); result < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not open codec" << av_err2str(result);
        return false;
    }

    return true;
}

std::pair<int, int> GifEncoder::encodeFrame(int maximumFrames)
{
    auto level = av_log_get_level();
    // Gif encoder spams the console when generating palettes unless you do this.
    av_log_set_level(AV_LOG_ERROR);
    auto ret = SoftwareEncoder::encodeFrame(maximumFrames);
    av_log_set_level(level);
    return ret;
}

int GifEncoder::percentageToAbsoluteQuality([[maybe_unused]] const std::optional<quint8> &quality)
{
    return -1; // Not possible to set quality
}
