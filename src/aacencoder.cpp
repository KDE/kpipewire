/*
    SPDX-FileCopyrightText: 2026 Khudoberdi <xudoyberdi0410@gmail.com>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "aacencoder_p.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

#include "audioconstants_p.h"
#include "logging_record.h"

AacEncoder::AacEncoder(PipeWireProduce *produce)
    : AudioEncoder(produce)
{
}

bool AacEncoder::initialize(int inputCount, bool globalHeader)
{
    auto codec = avcodec_find_encoder_by_name("aac");
    if (!codec) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "aac codec not found";
        return false;
    }

    m_avCodecContext = avcodec_alloc_context3(codec);
    if (!m_avCodecContext) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not allocate audio codec context";
        return false;
    }

    static const AVChannelLayout stereoLayout = AV_CHANNEL_LAYOUT_STEREO;

    m_avCodecContext->sample_fmt = AV_SAMPLE_FMT_FLTP;
    m_avCodecContext->sample_rate = AudioSampleRate;
    av_channel_layout_copy(&m_avCodecContext->ch_layout, &stereoLayout);
    m_avCodecContext->time_base = AVRational{1, AudioSampleRate};
    m_avCodecContext->bit_rate = qualityToBitrate(m_quality);

    if (globalHeader) {
        m_avCodecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (int result = avcodec_open2(m_avCodecContext, codec, nullptr); result < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not open codec" << av_err2str(result);
        return false;
    }

    return createFilterGraph(inputCount, AV_SAMPLE_FMT_FLTP, stereoLayout, AudioSampleRate);
}
