// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// SPDX-FileCopyrightText: 2026 Khudoberdi <xudoyberdi0410@gmail.com>

#include <QtTest>

#include <array>
#include <cmath>
#include <limits>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

#include "aacencoder_p.h"
#include "audioencoder_p.h"
#include "libopusencoder_p.h"
#include "pipewirebaseencodedstream.h"
#include "pipewireproduce_p.h"

using namespace Qt::StringLiterals;

class TestProduce : public PipeWireProduce
{
public:
    TestProduce()
        : PipeWireProduce(PipeWireBaseEncodedStream::Encoder::NoEncoder, 0, 0, 0, Fraction{.numerator = 1, .denominator = 24})
    {
    }

    void processPacket([[maybe_unused]] AVPacket *packet) override
    {
    }

    void processAudioPacket(AVPacket *packet) override
    {
        packets.append(std::make_pair(packet->pts, packet->duration));
    }

    QList<std::pair<int64_t, int64_t>> packets;
};

class TestAudioEncoder : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        m_produce = std::make_unique<TestProduce>();
    }

    // A simple smoke test that verifies the audio encoders can initialize
    // properly. This verifies that things like filter chains are correct.
    void testEncoderInit_data()
    {
        QTest::addColumn<std::shared_ptr<AudioEncoder>>("encoder");
        QTest::addColumn<QByteArray>("avcodecEncoder");

        QTest::addRow("opus") << std::shared_ptr<AudioEncoder>(new LibOpusEncoder(m_produce.get())) << "libopus"_ba;
        QTest::addRow("aac") << std::shared_ptr<AudioEncoder>(new AacEncoder(m_produce.get())) << "aac"_ba;
    }

    void testEncoderInit()
    {
        QFETCH(std::shared_ptr<AudioEncoder>, encoder);
        QFETCH(QByteArray, avcodecEncoder);

        if (!avcodec_find_encoder_by_name(avcodecEncoder.data())) {
            QSKIP("Skipping because the encoder was not found");
        }

        QVERIFY(encoder->initialize(2, false));
    }

    // Run actual samples through the full filter and encode pipeline,
    // verifying that arbitrarily sized input frames are rechunked to the
    // encoder's frame size and that the tail is drained at end of stream.
    void testEncodePipeline()
    {
        if (!avcodec_find_encoder_by_name("libopus")) {
            QSKIP("Skipping because the encoder was not found");
        }

        TestProduce produce;
        LibOpusEncoder encoder(&produce);
        QVERIFY(encoder.initialize(1, false));

        const int frameSize = encoder.avCodecContext()->frame_size;
        QVERIFY(frameSize > 0);

        static const AVChannelLayout stereoLayout = AV_CHANNEL_LAYOUT_STEREO;
        constexpr std::array sampleCounts = {441, 512, 1000};

        int64_t pts = 0;

        for (int round = 0; round < 4; ++round) {
            for (auto sampleCount : sampleCounts) {
                auto frame = av_frame_alloc();
                QVERIFY(frame);

                frame->format = AV_SAMPLE_FMT_FLT;
                frame->sample_rate = 48000;
                frame->nb_samples = sampleCount;
                av_channel_layout_copy(&frame->ch_layout, &stereoLayout);
                frame->pts = pts;
                pts += sampleCount;

                QCOMPARE(av_frame_get_buffer(frame, 0), 0);

                auto samples = reinterpret_cast<float *>(frame->data[0]);
                for (int i = 0; i < sampleCount * 2; ++i) {
                    samples[i] = std::sin(i * 0.01f);
                }

                QVERIFY(encoder.filterFrame(0, frame));
                av_frame_free(&frame);

                encoder.encodeFrame(std::numeric_limits<int>::max());
                encoder.receivePacket();
            }
        }

        // Push a large burst, like the gap filling in PipeWireProduce does, to
        // verify that frames beyond the codec's two-frame send queue are not
        // dropped.
        {
            const int burstSamples = frameSize * 10;
            auto frame = av_frame_alloc();
            QVERIFY(frame);

            frame->format = AV_SAMPLE_FMT_FLT;
            frame->sample_rate = 48000;
            frame->nb_samples = burstSamples;
            av_channel_layout_copy(&frame->ch_layout, &stereoLayout);
            frame->pts = pts;
            pts += burstSamples;

            QCOMPARE(av_frame_get_buffer(frame, 0), 0);

            auto samples = reinterpret_cast<float *>(frame->data[0]);
            for (int i = 0; i < burstSamples * 2; ++i) {
                samples[i] = std::sin(i * 0.01f);
            }

            QVERIFY(encoder.filterFrame(0, frame));
            av_frame_free(&frame);

            encoder.encodeFrame(std::numeric_limits<int>::max());
            encoder.receivePacket();
        }

        encoder.endInput(0);
        encoder.encodeFrame(std::numeric_limits<int>::max());
        encoder.finish();
        while (encoder.receivePacket() > 0) { }

        QVERIFY(!produce.packets.isEmpty());

        // The encoder may shift the start by its initial padding, but the
        // first packet should still be near zero.
        QVERIFY(qAbs(produce.packets.first().first) <= frameSize);

        int64_t previousPts = std::numeric_limits<int64_t>::min();
        int64_t totalDuration = 0;
        for (const auto &[packetPts, packetDuration] : produce.packets) {
            QVERIFY(packetPts > previousPts);
            previousPts = packetPts;
            totalDuration += packetDuration;
        }

        QVERIFY(qAbs(totalDuration - pts) <= frameSize);
    }

private:
    std::unique_ptr<TestProduce> m_produce;
};

QTEST_MAIN(TestAudioEncoder)

#include "TestAudioEncoder.moc"
