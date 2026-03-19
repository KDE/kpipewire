/*
    SPDX-FileCopyrightText: 2026

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include "encoder_p.h"

class H264NVENCEncoder : public SoftwareEncoder
{
public:
    H264NVENCEncoder(H264Profile profile, PipeWireProduce *produce);
    ~H264NVENCEncoder() override;

    bool initialize(const QSize &size) override;
    bool filterFrame(const PipeWireFrame &frame) override;

protected:
    int percentageToAbsoluteQuality(const std::optional<quint8> &quality) override;
    AVDictionary *buildEncodingOptions() override;

private:
    H264Profile m_profile;
    AVBufferRef *m_cudaDeviceContext = nullptr;
};
