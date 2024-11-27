/*
    SPDX-FileCopyrightText: 2024 Noah Davis <noahadvs@gmail.com>
    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "encoder_p.h"

/**
 * A software encoder that uses libwebp to encode to animated WebP.
 */
class LibWebPEncoder : public SoftwareEncoder
{
public:
    LibWebPEncoder(PipeWireProduce *produce);

    bool initialize(const QSize &size) override;

protected:
    int percentageToAbsoluteQuality(const std::optional<quint8> &quality) override;
    void applyEncodingPreference(AVDictionary *options) override;
};
