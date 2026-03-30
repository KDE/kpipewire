/*
    SPDX-FileCopyrightText: 2026 David Edmundson <davidedmundson@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include "encoder_p.h"

/**
 * A hardware encoder that uses Vulkan Video to encode to H264.
 *
 * It imports DMA-BUF frames via DRM_PRIME and uses a Vulkan-based
 * filter graph to remain zero-copy on the GPU.
 */
class H264VulkanEncoder : public HardwareEncoder
{
public:
    H264VulkanEncoder(H264Profile profile, PipeWireProduce *produce);

    bool initialize(const QSize &size) override;

protected:
    int percentageToAbsoluteQuality(const std::optional<quint8> &quality) override;

private:
    H264Profile m_profile = H264Profile::Main;
};
