/*
    SPDX-FileCopyrightText: 2026 Khudoberdi <xudoyberdi0410@gmail.com>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

// The audio pipeline uses a single fixed format end to end: the capture
// streams negotiate it (PipeWire resamples and remixes whatever the device
// produces) and the encoders and muxer are configured to match. Keeping the
// values here ensures the capture and encode sides never drift apart.
constexpr int AudioSampleRate = 48000;
constexpr int AudioChannels = 2;
