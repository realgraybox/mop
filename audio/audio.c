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
 //audio.c
#define _XOPEN_SOURCE 500
#define _POSIX_C_SOURCE 200809L

#include "audio.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef AUDIO_HAVE_OSS
	#include <sys/ioctl.h>
	#include <sys/soundcard.h>
#endif
#ifdef AUDIO_HAVE_TINYALSA
	#include "asoundlib.h"
#endif

//at main.c compile time include pcm.c (and pcm.h)
//and audio.c

static enum audio_backend current_backend = AUDIO_BACKEND_NONE;

static int audio_rate;
static int audio_channels;

#ifdef AUDIO_HAVE_OSS
static int oss_fd = -1;
#endif

#ifdef AUDIO_HAVE_TINYALSA
static struct pcm *tiny_pcm;
#endif

#ifdef AUDIO_HAVE_OSS
static int audio_init_oss(int rate, int channels);
static int audio_write_oss(const int16_t *pcm, int frames);
static void audio_close_oss(void);
#endif

#ifdef AUDIO_HAVE_TINYALSA
static int audio_init_tinyalsa(int rate, int channels);
static int audio_write_tinyalsa(const int16_t *pcm, int frames);
static void audio_close_tinyalsa(void);
#endif

enum audio_backend audio_get_backend(void) {
    return current_backend;
}

int audio_init_backend(enum audio_backend backend, int rate, int channels) {
    if (rate <= 0 || channels <= 0)
        return -1;

    audio_close();
#ifdef AUDIO_HAVE_OSS
    if (backend == AUDIO_BACKEND_OSS)
        return audio_init_oss(rate, channels);
#endif
#ifdef AUDIO_HAVE_TINYALSA
    if (backend == AUDIO_BACKEND_TINYALSA)
        return audio_init_tinyalsa(rate, channels);
#endif
    return -1;
}

int audio_init(int rate, int channels) {
    /*
     * Choose the desired policy here. This version tries OSS first,
     * then TinyALSA.
     */
#ifdef AUDIO_HAVE_OSS
    if (audio_init_backend(AUDIO_BACKEND_OSS, rate, channels) == 0) 
        return 0;
#endif
#if defined AUDIO_HAVE_OSS && AUDIO_HAVE_TINYALSA
    fprintf(stderr, "Audio: OSS unavailable; trying TinyALSA\n");
#endif
#ifdef AUDIO_HAVE_TINYALSA
    if (audio_init_backend(AUDIO_BACKEND_TINYALSA, rate, channels) == 0)
        return 0;
#endif
    current_backend = AUDIO_BACKEND_NONE;
    return -1;

}

int audio_write(const int16_t *pcm, int frames) {
    if (!pcm || frames <= 0)
        return -1;

    switch (current_backend) {
#ifdef AUDIO_HAVE_OSS
    case AUDIO_BACKEND_OSS:
        return audio_write_oss(pcm, frames);
#endif
#ifdef AUDIO_HAVE_TINYALSA
    case AUDIO_BACKEND_TINYALSA:
        return audio_write_tinyalsa(pcm, frames);
#endif
    default:
        return -1;
    }
}

void audio_close(void) {
    switch (current_backend) {
#ifdef AUDIO_HAVE_OSS
    case AUDIO_BACKEND_OSS:
        audio_close_oss();
        break;
#endif
#ifdef AUDIO_HAVE_TINYALSA
    case AUDIO_BACKEND_TINYALSA:
        audio_close_tinyalsa();
        break;
#endif
    default:
        break;
    }

    current_backend = AUDIO_BACKEND_NONE;
    audio_rate = 0;
    audio_channels = 0;
}

//OSS backend
#ifdef AUDIO_HAVE_OSS
static int write_all(int fd, const void *data, size_t bytes) {
    const unsigned char *p = data;

    while (bytes != 0) {
        ssize_t n = write(fd, p, bytes);

        if (n > 0) {
            p += n;
            bytes -= (size_t)n;
            continue;
        }

        if (n < 0 && errno == EINTR)
            continue;

        return -1;
    }

    return 0;
}

static int audio_init_oss(int rate, int channels) {
    int fd;
    int format = AFMT_S16_LE;
    int actual_channels = channels;
    int actual_rate = rate;

    fd = open("/dev/dsp", O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "OSS: open /dev/dsp: %s\n", strerror(errno));
        return -1;
    }

    if (ioctl(fd, SNDCTL_DSP_RESET, 0) < 0)
        goto fail;

    if (ioctl(fd, SNDCTL_DSP_SETFMT, &format) < 0)
        goto fail;

    if (format != AFMT_S16_LE) {
        fprintf(stderr, "OSS: device rejected S16_LE\n");
        goto fail;
    }

    if (ioctl(fd, SNDCTL_DSP_CHANNELS, &actual_channels) < 0)
        goto fail;

    if (actual_channels != channels) {
        fprintf(stderr, "OSS: requested %d channels, got %d\n",
                channels, actual_channels);
        goto fail;
    }

    if (ioctl(fd, SNDCTL_DSP_SPEED, &actual_rate) < 0)
        goto fail;

    /*
     * Decide whether rate clamping is acceptable. If the synthesizer
     * cannot tolerate a different rate, reject it instead.
     */
    if (actual_rate != rate) {
        fprintf(stderr, "OSS: requested %d Hz, got %d Hz\n",
                rate, actual_rate);
    }

    oss_fd = fd;
    audio_rate = actual_rate;
    audio_channels = actual_channels;
    current_backend = AUDIO_BACKEND_OSS;

    fprintf(stderr, "Audio: OSS %d Hz, %d channel(s), S16_LE\n",
            audio_rate, audio_channels);

    return 0;

fail:
    fprintf(stderr, "OSS: unsupported audio configuration\n");
    close(fd);
    return -1;
}

static int audio_write_oss(const int16_t *pcm, int frames) {
    size_t bytes;

    if (oss_fd < 0)
        return -1;

    bytes = (size_t)frames * (size_t)audio_channels * sizeof(*pcm);

    return write_all(oss_fd, pcm, bytes);
}

static void audio_close_oss(void) {
    if (oss_fd >= 0) {
		(void)ioctl(oss_fd, SNDCTL_DSP_SYNC, 0);
        close(oss_fd);
        oss_fd = -1;
    }
}
#endif
//TinyALSA backend
#ifdef AUDIO_HAVE_TINYALSA
static int audio_init_tinyalsa(int rate, int channels) {
    struct pcm_config config = {
        .channels = (unsigned int)channels,
        .rate = (unsigned int)rate,
        .format = PCM_FORMAT_S16_LE,
        .period_size = 1024,
        .period_count = 4,
        .start_threshold = 0,
        .stop_threshold = 0,
        .silence_threshold = 0
    };

    tiny_pcm = pcm_open(0, 0, PCM_OUT, &config);
    if (!tiny_pcm) {
        fprintf(stderr, "TinyALSA: pcm_open returned NULL\n");
        return -1;
    }

    if (!pcm_is_ready(tiny_pcm)) {
        fprintf(stderr, "TinyALSA: %s\n", pcm_get_error(tiny_pcm));
        pcm_close(tiny_pcm);
        tiny_pcm = NULL;
        return -1;
    }

    audio_rate = rate;
    audio_channels = channels;
    current_backend = AUDIO_BACKEND_TINYALSA;

    fprintf(stderr, "Audio: TinyALSA %d Hz, %d channel(s), S16_LE\n",
            rate, channels);

    return 0;
}

static int audio_write_tinyalsa(const int16_t *pcm, int frames) {
    unsigned int bytes;
    int rc;

    if (!tiny_pcm)
        return -1;

    bytes = (unsigned int)((size_t)frames * (size_t)audio_channels * sizeof(*pcm));

    rc = pcm_write(tiny_pcm, pcm, bytes);
    if (rc < 0) {
        fprintf(stderr, "TinyALSA: pcm_write: %s\n", pcm_get_error(tiny_pcm));
        return -1;
    }

    return 0;
}

static void audio_close_tinyalsa(void) {
    if (tiny_pcm) {
        pcm_close(tiny_pcm);
        tiny_pcm = NULL;
    }
}
#endif
