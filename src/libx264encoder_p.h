/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>
    SPDX-FileCopyrightText: 2023 Marco Martin <mart@kde.org>
    SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "encoder_p.h"

/**
 * A software encoder that uses libx264 to encode to H264.
 */
class LibX264Encoder : public SoftwareEncoder
{
public:
    LibX264Encoder(H264Profile profile, PipeWireProduce *produce);

    bool initialize(const QSize &size) override;

protected:
    int percentageToAbsoluteQuality(const std::optional<quint8> &quality) override;
    AVDictionary *buildEncodingOptions() override;

private:
    H264Profile m_profile = H264Profile::Main;
};
