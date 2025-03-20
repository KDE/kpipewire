/*
    SPDX-FileCopyrightText: 2024 Noah Davis <noahadvs@gmail.com>
    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "encoder_p.h"

/**
 * A software encoder that uses the FFmpeg GIF encoder.
 */
class GifEncoder : public SoftwareEncoder
{
public:
    GifEncoder(PipeWireProduce *produce);

    bool initialize(const QSize &size) override;
    std::pair<int, int> encodeFrame(int maximumFrames) override;

protected:
    int percentageToAbsoluteQuality(const std::optional<quint8> &quality) override;
};
