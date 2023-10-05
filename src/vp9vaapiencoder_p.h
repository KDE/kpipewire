/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>
    SPDX-FileCopyrightText: 2023 Marco Martin <mart@kde.org>
    SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
    SPDX-FileCopyrightText: 2023 Noah Davis <noahadvs@gmail.com>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "encoder_p.h"

/**
 * A hardware encoder that uses VAAPI to encode to VP9.
 */
class Vp9VAAPIEncoder : public HardwareEncoder
{
public:
    Vp9VAAPIEncoder(PipeWireProduce *produce);

    bool initialize(const QSize &size) override;

protected:
    int percentageToAbsoluteQuality(const std::optional<quint8> &quality) override;
};
