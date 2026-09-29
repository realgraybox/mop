/*
 * Copyright (C) 2026 M. Glargaard, aka graybox
 *
 * This software is provided "as-is", without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would
 *    be appreciated but is not required.
 *
 * 2. Altered source versions must be plainly marked as such, and must not
 *    be misrepresented as being the original software.
 *
 * 3. This notice may not be removed or altered from any source distribution.
 */
 //audio.h
#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

enum audio_backend {
	AUDIO_BACKEND_NONE = 0,
    AUDIO_BACKEND_OSS,
    AUDIO_BACKEND_TINYALSA
};

/*
 * samples is the number of interleaved frames, not int16_t elements.
 *
 * For stereo:
 *   samples = 100 means 200 int16_t values.
 */
int  audio_init(int rate, int channels);
int  audio_init_backend(enum audio_backend backend, int rate, int channels);
int  audio_write(const int16_t *pcm, int samples);
void audio_close(void);

enum audio_backend audio_get_backend(void);

#endif /* AUDIO_H */
