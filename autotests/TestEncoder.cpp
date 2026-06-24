// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// SPDX-FileCopyrightText: 2026 Arjen Hiemstra <ahiemstra@heimr.nl>

#include <QtTest>

#include <functional>
#include <memory>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "encoder_p.h"
#include "gifencoder_p.h"
#include "h264vaapiencoder_p.h"
#include "libopenh264encoder_p.h"
#include "libvpxencoder_p.h"
#include "libvpxvp9encoder_p.h"
#include "libwebpencoder_p.h"
#include "libx264encoder_p.h"
#include "pipewirebaseencodedstream.h"
#include "pipewireproduce_p.h"
#include "vaapiutils_p.h"

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
};

// This is a pretty simple smoke test that verifies all the encoders can
// initialize properly. This verifies that things like filter chains are correct.
class TestEncoder : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        m_produce = std::make_unique<TestProduce>();
    }

    void testEncoder_data()
    {
        QTest::addColumn<std::shared_ptr<Encoder>>("encoder");
        QTest::addColumn<QByteArray>("avcodecEncoder");

        QTest::addRow("h264_vaapi_main") << std::shared_ptr<Encoder>(new H264VAAPIEncoder(Encoder::H264Profile::Main, m_produce.get())) << "h264_vaapi"_ba;
        QTest::addRow("h264_vaapi_baseline") << std::shared_ptr<Encoder>(new H264VAAPIEncoder(Encoder::H264Profile::Baseline, m_produce.get()))
                                             << "h264_vaapi"_ba;
        QTest::addRow("h264_vaapi_high") << std::shared_ptr<Encoder>(new H264VAAPIEncoder(Encoder::H264Profile::High, m_produce.get())) << "h264_vaapi"_ba;
        //
        QTest::addRow("x264_main") << std::shared_ptr<Encoder>(new LibX264Encoder(Encoder::H264Profile::Main, m_produce.get())) << "libx264"_ba;
        QTest::addRow("x264_baseline") << std::shared_ptr<Encoder>(new LibX264Encoder(Encoder::H264Profile::Baseline, m_produce.get())) << "libx264"_ba;
        QTest::addRow("x264_high") << std::shared_ptr<Encoder>(new LibX264Encoder(Encoder::H264Profile::High, m_produce.get())) << "libx264"_ba;

        QTest::addRow("openh264_main") << std::shared_ptr<Encoder>(new LibOpenH264Encoder(Encoder::H264Profile::Main, m_produce.get())) << "libopenh264"_ba;
        QTest::addRow("openh264_baseline") << std::shared_ptr<Encoder>(new LibOpenH264Encoder(Encoder::H264Profile::Baseline, m_produce.get()))
                                           << "libopenh264"_ba;
        QTest::addRow("openh264_high") << std::shared_ptr<Encoder>(new LibOpenH264Encoder(Encoder::H264Profile::High, m_produce.get())) << "libopenh264"_ba;

        QTest::addRow("vp8") << std::shared_ptr<Encoder>(new LibVpxEncoder(m_produce.get())) << "libvpx"_ba;
        QTest::addRow("vp9") << std::shared_ptr<Encoder>(new LibVpxVp9Encoder(m_produce.get())) << "libvpx-vp9"_ba;

        QTest::addRow("gif") << std::shared_ptr<Encoder>(new GifEncoder(m_produce.get())) << "gif"_ba;
        QTest::addRow("webp") << std::shared_ptr<Encoder>(new LibWebPEncoder(m_produce.get())) << "libwebp"_ba;
    }

    void testEncoder()
    {
        QFETCH(std::shared_ptr<Encoder>, encoder);
        QFETCH(QByteArray, avcodecEncoder);

        if (!avcodec_find_encoder_by_name(avcodecEncoder.data())) {
            QSKIP("Skipping because the encoder was not found");
        }

        if (avcodecEncoder.contains("vaapi") && VaapiUtils::instance()->devicePath().isEmpty()) {
            QSKIP("Skipping because hardware encoding is not supported on this device");
        }

        QVERIFY(encoder->initialize(QSize(512, 512)));
    }

    // A mid-stream source resize is handled by PipeWireProduce::reconfigureStream(),
    // which discards the encoder and creates a fresh one for the new size. Verify
    // every encoder sets up cleanly when (re)created at a second, different size.
    void testReinitializeAtNewSize()
    {
        struct Case {
            const char *name;
            QByteArray avcodecEncoder;
            std::function<std::shared_ptr<Encoder>()> make;
        };
        const std::vector<Case> cases = {
            {"x264",
             "libx264"_ba,
             [this] {
                 return std::shared_ptr<Encoder>(new LibX264Encoder(Encoder::H264Profile::Main, m_produce.get()));
             }},
            {"openh264",
             "libopenh264"_ba,
             [this] {
                 return std::shared_ptr<Encoder>(new LibOpenH264Encoder(Encoder::H264Profile::Main, m_produce.get()));
             }},
            {"vp8",
             "libvpx"_ba,
             [this] {
                 return std::shared_ptr<Encoder>(new LibVpxEncoder(m_produce.get()));
             }},
            {"vp9",
             "libvpx-vp9"_ba,
             [this] {
                 return std::shared_ptr<Encoder>(new LibVpxVp9Encoder(m_produce.get()));
             }},
            {"h264_vaapi",
             "h264_vaapi"_ba,
             [this] {
                 return std::shared_ptr<Encoder>(new H264VAAPIEncoder(Encoder::H264Profile::Main, m_produce.get()));
             }},
        };

        bool ranAny = false;
        for (const auto &testCase : cases) {
            if (!avcodec_find_encoder_by_name(testCase.avcodecEncoder.data())) {
                continue;
            }
            if (testCase.avcodecEncoder.contains("vaapi") && VaapiUtils::instance()->devicePath().isEmpty()) {
                continue;
            }
            // Build at one size, then build a fresh encoder at a different size,
            // exactly as the mid-stream rebuild does.
            QVERIFY2(testCase.make()->initialize(QSize(512, 512)), testCase.name);
            QVERIFY2(testCase.make()->initialize(QSize(1280, 720)), testCase.name);
            ranAny = true;
        }

        if (!ranAny) {
            QSKIP("No usable encoders available to test");
        }
    }

private:
    std::unique_ptr<TestProduce> m_produce;
};

QTEST_MAIN(TestEncoder)

#include "TestEncoder.moc"
