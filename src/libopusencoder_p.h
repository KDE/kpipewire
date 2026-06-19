/*
    SPDX-FileCopyrightText: 2026 Khudoberdi <xudoyberdi0410@gmail.com>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include "audioencoder_p.h"

/**
 * An audio encoder that uses libopus to encode to Opus.
 */
class LibOpusEncoder : public AudioEncoder
{
public:
    LibOpusEncoder(PipeWireProduce *produce);

    bool initialize(int inputCount, bool globalHeader) override;
};
