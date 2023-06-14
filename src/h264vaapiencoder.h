/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>
    SPDX-FileCopyrightText: 2023 Marco Martin <mart@kde.org>
    SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "encoder.h"

/**
 * A hardware encoder that uses VAAPI to encode to H264.
 */
class H264VAAPIEncoder : public HardwareEncoder
{
public:
    enum class Profile { Baseline, Main, High };

    H264VAAPIEncoder(Profile profile, PipeWireProduce *produce);

    bool initialize(const QSize &size) override;

private:
    Profile m_profile = Profile::Main;
};
