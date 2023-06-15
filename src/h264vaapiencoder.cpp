/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>
    SPDX-FileCopyrightText: 2023 Marco Martin <mart@kde.org>
    SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "h264vaapiencoder.h"

H264VAAPIEncoder::H264VAAPIEncoder(H264Profile profile, PipeWireProduce *produce)
    : HardwareEncoder(produce)
    , m_profile(profile)
{
}

bool H264VAAPIEncoder::initialize(const QSize &size)
{
    return true;
}
