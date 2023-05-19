/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Copyright (C) 2023-2023  The DOSBox Staging Team
 *  Copyright (C) 2002-2021  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifndef DOSBOX_CAPTURE_H
#define DOSBOX_CAPTURE_H

#include "render.h"

#include <string>

std::string capture_generate_filename(const char* type, const char* ext);

FILE* CAPTURE_CreateFile(const char* type, const char* ext);

void CAPTURE_AddAudioData(const uint32_t sample_rate, const uint32_t num_sample_frames,
                          const int16_t* sample_frames);

void CAPTURE_AddFrame(const RenderedImage_t image, const float frames_per_second);

void CAPTURE_AddMidiData(const bool sysex, const size_t len, const uint8_t* data);

void CAPTURE_StartVideoCapture();
void CAPTURE_StopVideoCapture();

bool CAPTURE_IsCapturingAudio();
bool CAPTURE_IsCapturingImage();
bool CAPTURE_IsCapturingMidi();
bool CAPTURE_IsCapturingOpl();
bool CAPTURE_IsCapturingVideo();

#endif
