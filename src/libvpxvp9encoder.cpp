/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>
    SPDX-FileCopyrightText: 2023 Marco Martin <mart@kde.org>
    SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
    SPDX-FileCopyrightText: 2023 Noah Davis <noahadvs@gmail.com>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "libvpxvp9encoder_p.h"

#include "pipewireproduce_p.h"

#include <QSize>
#include <QThread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/opt.h>
}

#include "logging_record.h"

LibVpxVp9Encoder::LibVpxVp9Encoder(PipeWireProduce *produce)
    : SoftwareEncoder(produce)
{
}

bool LibVpxVp9Encoder::initialize(const QSize &size)
{
    createFilterGraph(size);

    auto codec = avcodec_find_encoder_by_name("libvpx-vp9");
    if (!codec) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "libvpx-vp9 codec not found";
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
    m_avCodecContext->pix_fmt = AV_PIX_FMT_YUV420P;
    m_avCodecContext->time_base = AVRational{1, 1000};

    AVDictionary *options = buildEncodingOptions();
    maybeLogOptions(options);

    const auto area = size.width() * size.height();
    // m_avCodecContext->framerate is not set, so we use m_produce->maxFramerate() instead.
    const auto maxFramerate = m_produce->maxFramerate();
    const auto fps = qreal(maxFramerate.numerator) / std::max(quint32(1), maxFramerate.denominator);

    m_avCodecContext->gop_size = fps * 2;

    setQuality(m_quality);

    if (int result = avcodec_open2(m_avCodecContext, codec, &options); result < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not open codec" << av_err2str(result);
        return false;
    }

    return true;
}


void LibVpxVp9Encoder::setQuality(std::optional<quint8> quality)
{
    SoftwareEncoder::setQuality(quality);
    // AVCodecContext::priv_data is where the encoder specific options are.
    // Naturally, you can't use the C API of the private data object and using
    // a private data object directly to set options seems like a bad idea.
    // However, you can still set the options by setting options on the
    // AVCodecContext object with the AV_OPT_SEARCH_CHILDREN search flag.
    if (!m_avCodecContext) {
        return;
    }
    // Lower crf is higher quality. Max 0, min 63. libvpx-vp9 doesn't use global_quality.
    int crf = 31;
    if (m_quality) {
        constexpr int MinQuality = 63;
        crf = std::max(1, int(MinQuality - (quality.value() / 100.0) * MinQuality));
    }
    // libvpx-vp9 crf takes an int
    av_opt_set_int(m_avCodecContext, "qmin", std::clamp(crf / 2, 0, crf), AV_OPT_SEARCH_CHILDREN);
    av_opt_set_int(m_avCodecContext, "qmax", std::clamp(int(crf * 1.5), crf, 63), AV_OPT_SEARCH_CHILDREN);
    av_opt_set_int(m_avCodecContext, "crf", crf, AV_OPT_SEARCH_CHILDREN);
}

AVDictionary *LibVpxVp9Encoder::buildEncodingOptions()
{
    AVDictionary *options = SoftwareEncoder::buildEncodingOptions();

    // We're probably capturing a screen
    av_dict_set(&options, "tune-content", "screen", 0);

    // 0-4 are for Video-On-Demand with the good or best deadline.
    // Don't use best, it's not worth it.
    // 5-8 are for streaming with the realtime deadline.
    // Lower is higher quality.
    int cpuUsed = 5 + std::max(1, int(3 - std::round(m_quality.value_or(50) / 100.0 * 3)));
    av_dict_set_int(&options, "cpu-used", cpuUsed, 0);
    av_dict_set(&options, "deadline", "realtime", 0);

    // The value is interpreted as being equivalent to log2(realNumberOfColumns),
    // so 3 is 8 columns. 6 is the max amount of columns. 2 is the max amount of rows.
    av_dict_set(&options, "tile-columns", "6", 0);
    av_dict_set(&options, "tile-rows", "2", 0);

    av_dict_set(&options, "row-mt", "1", 0);
    av_dict_set(&options, "frame-parallel", "1", 0);

    return options;
}
