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

    m_avCodecContext->global_quality = percentageToAbsoluteQuality(m_quality);
    AVDictionary *options = buildEncodingOptions();
    maybeLogOptions(options);

    if (int result = avcodec_open2(m_avCodecContext, codec, &options); result < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not open codec" << av_err2str(result);
        return false;
    }

    return true;
}

int LibWebPEncoder::percentageToAbsoluteQuality(const std::optional<quint8> &quality)
{
    // AVCodecContext::global_quality uses the scale of MPEG-1/2/4 qscale.
    // FFmpeg will convert it to a 0-100 scale internally.
    // 100 is maximum quality and 75 is default quality.
    // FFmpeg will use the default quality if global_quality is -1.
    return quality.has_value() ? quality.value() * FF_QP2LAMBDA : -1;
}

AVDictionary *LibWebPEncoder::buildEncodingOptions()
{
    AVDictionary *options = SoftwareEncoder::buildEncodingOptions();

    /* Many of libwebp's options aren't exposed directly through FFmpeg options,
     * so we need to use presets to change the options.
     *
     * AVCodecContext::compression_level is ignored when using presets.
     * AVCodecContext::global_quality is still respected when using presets.
     *
     * See the link below to see what the presets actually do:
     * https://github.com/webmproject/libwebp/blob/ed0fa1cb07b7ea473286df4908fff5f82aee8406/src/enc/config_enc.c#L63-L96
     *
     * Icon and text presets are basically the same, except text has segments=2.
     * We're using the text preset since screen content will often have lots of text and icons.
     *
     * The default for segments is 4, but official docs say it's effectively
     * capped at 2 unless using the low-memory option (not exposed by FFmpeg).
     * I'm not 100% sure if it's true that segments are capped at 2 without the
     * low-memory option since I don't see any code in libwebp that does that.
     */
    av_dict_set(&options, "preset", "text", 0);
    return options;
}
