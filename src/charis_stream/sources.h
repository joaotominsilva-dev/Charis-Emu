// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>

#include "charis_stream/host.h"

namespace CharisStream {

// Windows: the main monitor captured on the GPU (ddagrab), scaled there and encoded by NVENC.
std::unique_ptr<VideoSource> MakeScreenVideoSource();
// Windows: everything the PC plays (WASAPI loopback), encoded as Opus.
std::unique_ptr<AudioSource> MakeLoopbackAudioSource();

} // namespace CharisStream
