/*
    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>
    SPDX-FileCopyrightText: 2023 Marco Martin <mart@kde.org>
    SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "encoder.h"
Encoder::Encoder(PipeWireProduce *produce)
    : QObject(nullptr)
    , m_produce(produce)
{
}

Encoder::~Encoder()
{
}

void Encoder::encodeFrame()
{
}

void Encoder::receivePacket()
{
}

void Encoder::finish()
{
SoftwareEncoder::SoftwareEncoder(PipeWireProduce *produce)
    : Encoder(produce)
{
}

void SoftwareEncoder::filterFrame(const PipeWireFrame &frame)
{
}

bool SoftwareEncoder::createFilterGraph(const QSize &size)
{
}

HardwareEncoder::HardwareEncoder(PipeWireProduce *produce)
    : Encoder(produce)
{
}

HardwareEncoder::~HardwareEncoder()
{
}

void HardwareEncoder::filterFrame(const PipeWireFrame &frame)
{
}

}
