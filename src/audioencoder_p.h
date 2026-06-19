/*
    SPDX-FileCopyrightText: 2026 Khudoberdi <xudoyberdi0410@gmail.com>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <mutex>
#include <optional>

#include <QList>
#include <QObject>

#include "pipewireproduce_p.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavfilter/avfilter.h"
#include "libavutil/channel_layout.h"
}

#undef av_err2str
// The one provided by libav fails to compile on GCC due to passing data from the function scope outside
char *av_err2str(int errnum);

class PipeWireProduce;

/**
 * Base class for objects that encapsulate audio encoder logic and state.
 */
class AudioEncoder : public QObject
{
    Q_OBJECT
public:
    /**
     * Constructor.
     *
     * @param produce The PipeWireProduce instance that owns this encoder.
     */
    AudioEncoder(PipeWireProduce *produce);
    ~AudioEncoder() override;

    /**
     * Initialize and setup the encoder.
     *
     * @param inputCount The number of audio inputs that will be mixed together.
     * @param globalHeader Whether the container format requires global codec headers.
     *
     * @return true if initialization was succesful, false if not.
     */
    virtual bool initialize(int inputCount, bool globalHeader) = 0;
    /**
     * Pass an audio frame to libav for filtering.
     *
     * @param input The index of the input the frame belongs to.
     * @param frame The frame to process.
     *
     * @note This method will be called on its own thread.
     */
    bool filterFrame(int input, AVFrame *frame);
    /**
     * Signal the end of stream for an input, so the filter graph can drain.
     *
     * @param input The index of the input that ended.
     */
    void endInput(int input);
    /**
     * Get the next finished frames from the libav filter chain and queue them for encoding.
     *
     * @param maximumFrames The maximum number of frames that can be queued for encoding.
     *
     * @return A pair with the number of frames removed from the filter chain as first entry
     *         and the number of frames queued for encoding as the second entry.
     *
     * @note This method will be called on its own thread.
     */
    std::pair<int, int> encodeFrame(int maximumFrames);
    /**
     * Get the next encoded packets from libav and pass them to PipeWireProduce.
     *
     * @return The number of encoded packets that were received.
     *
     * @note This method will be called on its own thread.
     */
    int receivePacket();
    /**
     * End encoding and perform any necessary cleanup.
     */
    void finish();

    /**
     * Return the AVCodecContext for this encoder.
     */
    AVCodecContext *avCodecContext() const;

    /**
     * Set the quality level, from 0 (lowest) to 100 (highest).
     *
     * Internally this will be converted to a bit rate.
     */
    void setQuality(std::optional<quint8> quality);

protected:
    /**
     * Create a filter graph that mixes the inputs and converts to the encoder's format.
     *
     * @param inputCount The number of audio inputs to create buffer sources for.
     * @param outputFormat The sample format the encoder expects.
     * @param layout The channel layout of the stream.
     * @param sampleRate The sample rate of the stream.
     */
    bool createFilterGraph(int inputCount, AVSampleFormat outputFormat, const AVChannelLayout &layout, int sampleRate);

    int64_t qualityToBitrate(const std::optional<quint8> &quality) const;

    PipeWireProduce *m_produce;

    AVCodecContext *m_avCodecContext = nullptr;
    std::mutex m_avCodecMutex;

    AVFilterGraph *m_avFilterGraph = nullptr;
    // Serializes all access to the filter graph, frames are pushed in and
    // pulled out from different threads.
    std::mutex m_avFilterGraphMutex;
    QList<AVFilterContext *> m_inputFilters;
    AVFilterContext *m_outputFilter = nullptr;

    std::optional<quint8> m_quality;
};
