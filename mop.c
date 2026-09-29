/*
 * Copyright (C) M. Glargaard, aka graybox
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

/*
 * opal2play.c
 *
 * Simple MIDI -> AdLib/OPL3 in OP3-mode player using opal-opl and OSS /dev/dsp or tinyalsa.
 * 
 * # Single note
 * ./opal2play -b mop.bnk -p 1 -n 60 -d 1500
 * ./opal2play -r 00000101030F050001030F000000000001040D0700020400000001010000 -n 60 -d 1500
 * 
 * where p is program (instrument),  n note and d duration, r is the 30 byte instruction
 * 
 * # C major chord (C4, E4, G4)
 * ./opal2play -b mop.bnk -p 1 -n "60,64,67" -d 1500
 *
 * # G7 chord (G3, B3, D4, F4)
 * ./opal2play -b mop.bnk -p 1 -n "55,59,62,65" -d 1500
 *
 * # Raw hex with chord
 * ./opal2play -r 00000101030F050001030F000000000001040D0700020400000001010000 -n "60,64,67" -d 1500
 *
 * Usage:
 *     	opal2play song.mid [instruments.bnk]
 *
 * Supports:
 *     	MIDI format 0/1
 *     	running status
 *     	note on/off
 *     	program change
 *     	controller 7  - volume
 *     	controller 11 - expression
 *     	controller 64 - sustain
 *     	controller 120/123 - all notes off
 *     	pitch bend
 *     	tempo
 *
 * Instrument bank:
 *     	Standard AdLib BNK format.
 *
 * opal:
 * 	   	https://github.com/RealBitdancer/opal (MIT)
 * tml.h:
 * 		https://github.com/schellingb/TinySoundFont (MIT)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>

#define TML_IMPLEMENTATION
#include "tml.h"

#include "opal/opal.h"
#include "opal/opal.c"

typedef int Bits;

static Opal chip;

static void adlib_init(uint32_t rate) {
    opalInit(&chip, (int)rate);
}

static void adlib_write(uint32_t reg, uint8_t val) {
    opalWriteReg(&chip, (uint16_t)reg, val);
}

static void adlib_getsample(int16_t *buf, Bits n) {
    for (Bits i = 0; i < n; i++) {
        int16_t l, r;
        opalSample(&chip, &l, &r);
        buf[2 * i]     = l;
        buf[2 * i + 1] = r;
    }
}

//18 voices
#define BANK(v)    (((v) >= 9) << 8)
#define MODR(v, b) (BANK(v) + (b) + mod_base[(v) % 9])
#define CARR(v, b) (BANK(v) + (b) + car_base[(v) % 9])
#define CHR(v, b)  (BANK(v) + (b) + ((v) % 9))

#include "audio.h"

#if defined(__UCLIBC__) && (__GNUC__ < 4)
#define copysignf(x, y) ((float)copysign((double)(x), (double)(y)))
#endif

#define SAMPLE_RATE     48000	//44100
#define MAX_VOICES 		18
#define MAX_PROGRAMS    256
#define AUDIO_BUFFER    1024	//1024
#define PERC_BASE		163	// Percussion instruments start at this index in the bank
#define SYNTH_CHANNELS 	2	// physical output is always stereo

#define EV_NOTE_ON      1
#define EV_NOTE_OFF     2
#define EV_PROGRAM      3
#define EV_CONTROL      4
#define EV_PITCH        5
#define EV_TEMPO        6

typedef struct {
    uint64_t tick;
    uint8_t type;
    uint8_t channel;
    uint8_t a;
    uint8_t b;
    uint32_t tempo;
} MIDI_EVENT;

typedef struct {
    uint8_t data[30];
} BNK_INSTRUMENT;

typedef struct {
    BNK_INSTRUMENT inst[MAX_PROGRAMS];
    int count;
} BANK;

typedef struct {
    int active;
    int released;
    int channel;
    int note;
    int velocity;
    unsigned long age;
    int prog;
} VOICE;

static MIDI_EVENT *events;
static BANK bank;
static VOICE voices[MAX_VOICES];

static uint8_t program[16];
static uint8_t volume[16];
static uint8_t expression[16];
static uint8_t sustain[16];
static uint8_t pan[16];	//0-127

static int pitch[16];

static unsigned long voice_age;

extern const unsigned char _binary_mop_bnk_start[];
extern const unsigned char _binary_mop_bnk_end[];

static uint8_t percussion_program(int note) {
    if (note < 35 || note > 81)
        return PERC_BASE; // fallback to first drum
    return (uint8_t)(PERC_BASE + (note - 35));
}

static int is_percussion_channel(int ch) {	// Helper to check if a channel is percussion
    return ch == 9; // MIDI channel 10
}

/*
 * OPL2 register operator bases.
 * For channel 0:
 *     modulator = 0
 *     carrier   = 3
 * Channel 1:
 *     modulator = 1
 *     carrier   = 4
 *
 * etc.
 */

static const uint8_t mod_base[9] = {
    0, 1, 2,
    8, 9, 10,
    16, 17, 18
};

static const uint8_t car_base[9] = {
    3, 4, 5,
    11, 12, 13,
    19, 20, 21
};

// Little/big endian helpers
static uint16_t le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// BNK
/*
 * Standard AdLib BNK:
 *
 * header:
 *     1  major
 *     1  minor
 *     6  "ADLIB-"
 *     2  numUsed
 *     2  numInstruments
 *     4  offsetName
 *     4  offsetData
 *     8  reserved
 *
 * name entry:
 *     2  index
 *     1  flags
 *     9  name
 *
 * packed timbre:
 *     2  percussive / voice
 *    13  modulator OPLREGS
 *    13  carrier OPLREGS
 *     1  modulator waveform
 *     1  carrier waveform
 *
 * total = 30 bytes.
 */

#define BNK_HEADER_SIZE 28
#define BNK_NAME_SIZE   12
#define BNK_INST_SIZE   30

static int load_bnk_memory(const uint8_t *buf, size_t size) {
    const uint8_t *hdr;
    const uint8_t *names;
    const uint8_t *data;
    uint16_t num_instruments;
    uint32_t offset_name;
    uint32_t offset_data;
    size_t names_size;
    size_t data_size;
    size_t i;
    int count = 0;

    if (size < BNK_HEADER_SIZE)
        return -1;

    hdr = buf;

    if (memcmp(hdr + 2, "ADLIB-", 6) != 0)
        return -1;

    num_instruments = le16(hdr + 10);
    offset_name = le32(hdr + 12);
    offset_data = le32(hdr + 16);

    names_size = (size_t)num_instruments * BNK_NAME_SIZE;
    data_size = (size_t)num_instruments * BNK_INST_SIZE;

    if ((size_t)offset_name > size ||
        names_size > size - (size_t)offset_name)
        return -1;

    if ((size_t)offset_data > size ||
        data_size > size - (size_t)offset_data)
        return -1;

    names = buf + offset_name;
    data = buf + offset_data;

    for (i = 0; i < num_instruments && count < MAX_PROGRAMS; i++) {

        const uint8_t *entry;
        uint16_t midi_index;
        entry = names + i * BNK_NAME_SIZE;
        midi_index = le16(entry);

        if (midi_index >= num_instruments)
            continue;

        // HMI version 0.0 banks such as melodic.bnk have valid instruments with flags == 0
        if ((hdr[0] == 0 && hdr[1] == 0) || entry[2] != 0) {
			if (count >= MAX_PROGRAMS)
				break;
            memcpy(bank.inst[count].data, data + (size_t)midi_index * BNK_INST_SIZE, BNK_INST_SIZE);
            count++;
        }
    }
    bank.count = count;
    return count ? 0 : -1;
}

static int load_bnk(const char *filename) {
    FILE *fp;
    uint8_t *buf;
    long len;
    int ret;
    fp = fopen(filename, "rb");
    if (!fp) {
        perror(filename);
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) < 0) {
        fclose(fp);
        return -1;
    }
    len = ftell(fp);
    if (len < 0) {
        fclose(fp);
        return -1;
    }
    rewind(fp);
    buf = malloc((size_t)len);
    if (!buf) {
        fclose(fp);
        return -1;
    }
    if (fread(buf, 1, (size_t)len, fp) != (size_t)len) {
        free(buf);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    ret = load_bnk_memory(buf, (size_t)len);
    free(buf);
    return ret;
}

// OPL instrument
/*
 * OPLREGS offsets inside each 13-byte operator:
 *
 * 0  ksl
 * 1  multiple
 * 2  feedback
 * 3  attack
 * 4  sustain
 * 5  eg
 * 6  decay
 * 7  release
 * 8  total level
 * 9  am
 * 10 vib
 * 11 ksr
 * 12 con
 *
 * BNK:
 *
 * 0..1       percussion / voice
 * 2..14      modulator
 * 15..27     carrier
 * 28         mod waveform
 * 29         carrier waveform
 */

static uint8_t opl_reg20(const uint8_t *o) {
    return (uint8_t)(
        ((o[9]  ? 1 : 0) << 7) |
        ((o[10] ? 1 : 0) << 6) |
        ((o[5]  ? 1 : 0) << 5) |
        ((o[11] ? 1 : 0) << 4) |
        (o[1] & 0x0f));
}

static uint8_t opl_reg40(const uint8_t *o) {
    return (uint8_t)(((o[0] & 3) << 6) | (o[8] & 0x3f));
}

static uint8_t opl_reg60(const uint8_t *o) {
    return (uint8_t)(((o[3] & 0x0f) << 4) | (o[6] & 0x0f));
}

static uint8_t opl_reg80(const uint8_t *o) {
    return (uint8_t)(((o[4] & 0x0f) << 4) | (o[7] & 0x0f));
}

static uint8_t opl_regc0(const uint8_t *o) {
    /* BNK "con" is inverted:
     * 0    -> OPL connection bit 1
     * other -> OPL connection bit 0
     */
    return (uint8_t)(((o[2] & 7) << 1) | (o[12] ? 0 : 1));
}

static int pan_to_stereo_mode(uint8_t pan_val) {
    if (pan_val < 48)
        return 1; // left only
    if (pan_val > 80)
        return 2; // right only
    return 3;     // center (both)
}

static void load_opl_instrument(int voice, int prog, int channel) {
	(void)channel;
    const uint8_t *p;
    const uint8_t *mod;
    const uint8_t *car;
		
    if (prog < 0 || prog >= bank.count)
        prog = 0;

    p = bank.inst[prog].data;

    mod = p + 2;
    car = p + 15;

    adlib_write(MODR(voice, 0x20), opl_reg20(mod));
    adlib_write(MODR(voice, 0x40), opl_reg40(mod));
    adlib_write(MODR(voice, 0x60), opl_reg60(mod));
    adlib_write(MODR(voice, 0x80), opl_reg80(mod));
    adlib_write(MODR(voice, 0xe0), p[28] & 7);

    adlib_write(CARR(voice, 0x20), opl_reg20(car));
    adlib_write(CARR(voice, 0x40), opl_reg40(car));
    adlib_write(CARR(voice, 0x60), opl_reg60(car));
    adlib_write(CARR(voice, 0x80), opl_reg80(car));
    adlib_write(CARR(voice, 0xe0), p[29] & 7);


	uint8_t con;
    con = opl_regc0(mod);
    
    // Add stereo select bits based on channel pan
    int mode = pan_to_stereo_mode(pan[channel]);
    // Clear old stereo bits (4 and 5)
    con &= ~0x30;	//needed?
	if (mode & 1) con |= 0x10; // left
	if (mode & 2) con |= 0x20; // right
	adlib_write(CHR(voice, 0xc0), con);
}

// OPL note frequency
static void note_frequency(int note, int bend, int *fnum, int *block) {
    double semitones;
    double freq;
    int b;
    int f;

    semitones = ((double)note - 69.0) / 12.0;

    // Pitch bend range = +/- 2 semitones
    semitones += ((double)bend / 8192.0) * (2.0 / 12.0);
    freq = 440.0 * pow(2.0, semitones);

	// opl frequency:  f = fnum * 2^block * INTFREQU / 2^20
    // Choose the lowest block that gives a valid 10-bit fnum
    b = 0;

    for (;;) {
        double x;
        x = freq * 1048576.0 / (49715.9027777778 * (double)(1 << b));
        f = (int)(x + 0.5);
        if (f <= 1023 || b >= 7)
            break;

        b++;
    }

    if (f < 0)
        f = 0;

    if (f > 1023)
        f = 1023;

    *fnum = f;
    *block = b;
}

static void opl_note(int voice, int note, int bend, int on) {
    int fnum;
    int block;

    note_frequency(note, bend, &fnum, &block);
    adlib_write(CHR(voice, 0xa0), fnum & 0xff);
    adlib_write(CHR(voice, 0xb0), ((fnum >> 8) & 3) | ((block & 7) << 2) | (on ? 0x20 : 0));
}

// Volume
static void set_volume(int voice) {
    const uint8_t *p;
    const uint8_t *car;
    int level;
    int amp;

    p = bank.inst[voices[voice].prog].data;
    car = p + 15;

    if (is_percussion_channel(voices[voice].channel)) {
        // Percussion: ignore channel volume/expression, use velocity only,
        // with a slight boost to match melodic instruments.
        int vel = voices[voice].velocity; // 0..127
        // Map 1..127 -> ~64..127 to keep drums present even at low velocity
        amp = 60 + (vel * 63) / 127;
    } else {
        amp = volume[voices[voice].channel];
        amp = amp * expression[voices[voice].channel] / 127;
        amp = amp * voices[voice].velocity / 127;
    }
    // Convert amplitude to additional TL attenuation
    level = car[8] & 63;
    level += (127 - amp) * 63 / 127;

    if (level > 63)
        level = 63;

	adlib_write(CARR(voice, 0x40), ((car[0] & 3) << 6) | level);
}

// Voices
static int find_voice(int channel, int note) {
    int i;

    for (i = 0; i < MAX_VOICES; i++) {
        if (voices[i].active &&
            voices[i].channel == channel &&
            voices[i].note == note)
            return i;
    }
    return -1;
}

static void voice_free(int v) {
    opl_note(v, voices[v].note, pitch[voices[v].channel], 0);
    voices[v].active = 0;
    voices[v].released = 0;
    voices[v].age = ++voice_age;   // age now = time of release
}

static int alloc_voice(void) {
    int i, best = -1;

    // free voice that has been silent longest
    for (i = 0; i < MAX_VOICES; i++)
        if (!voices[i].active && (best < 0 || voices[i].age < voices[best].age))
            best = i;
    if (best >= 0)
        return best;
        
    // debug
    // static unsigned steals;
	// steals++; fprintf(stderr, "steal #%u\n", steals);

    // steal: sustain-held voices first, then the oldest
    for (i = 0; i < MAX_VOICES; i++)
        if (best < 0 ||
            voices[i].released > voices[best].released ||
            (voices[i].released == voices[best].released &&
             voices[i].age < voices[best].age))
            best = i;

    voice_free(best);
    return best;
}

static void note_on(int channel, int note, int velocity) {
    int v;
	int prog;

    v = find_voice(channel, note);

    if (v >= 0) {
        opl_note(v, note, pitch[channel], 0);
        voices[v].active = 0;
    }

    v = alloc_voice();
 
	if (is_percussion_channel(channel)) {
		// Ignore program[channel]
		prog = percussion_program(note);
    } else {
        prog = program[channel];
        if (prog >= bank.count)
            prog = 0;
    }
    load_opl_instrument(v, prog, channel);

    voices[v].active = 1;
    voices[v].released = 0;
    voices[v].channel = channel;
    voices[v].note = note;
    voices[v].velocity = velocity;
    voices[v].age = ++voice_age;
    voices[v].prog = prog;
    
    set_volume(v);
    opl_note(v, note, pitch[channel], 1);
}

static void note_off(int channel, int note) {
    int v;
    v = find_voice(channel, note);

    if (v < 0)
        return;

    if (sustain[channel] >= 64) {
        voices[v].released = 1;
        return;
    }
    opl_note(v, note, pitch[channel], 0);
    voices[v].active = 0;
}

static void release_sustain(int channel) {
    int i;

    for (i = 0; i < MAX_VOICES; i++) {
        if (voices[i].active && voices[i].channel == channel && voices[i].released) {
            opl_note(i, voices[i].note, pitch[channel], 0);
            voices[i].active = 0;
        }
    }
}

static void all_notes_off(int channel) {
    int i;
    for (i = 0; i < MAX_VOICES; i++) {
        if (voices[i].active && voices[i].channel == channel) {
            opl_note(i, voices[i].note, pitch[channel], 0);
            voices[i].active = 0;
        }
    }
}

static void set_pitch(int channel, int value) {
    int i;
    pitch[channel] = value - 8192;
    for (i = 0; i < MAX_VOICES; i++) {
        if (voices[i].active && voices[i].channel == channel) {
            opl_note(i, voices[i].note, pitch[channel], 1);
        }
    }
}

static void process_event(const struct tml_message *m) {
    int ch = m->channel;
    if (ch > 15) return;

    switch (m->type) {
    case TML_NOTE_ON:
        if (m->velocity)
            note_on(ch, m->key, m->velocity);
        else
            note_off(ch, m->key);
        break;
    case TML_NOTE_OFF:
        note_off(ch, m->key);
        break;
    case TML_PROGRAM_CHANGE:
        if (!is_percussion_channel(ch)) {
            program[ch] = m->program;
            if (program[ch] >= bank.count)
                program[ch] = 0;
        }
        break;
    case TML_CONTROL_CHANGE:
        switch (m->control) {
        case 7:  volume[ch] = m->control_value; break;
        case 10: pan[ch] = m->control_value; break;
        case 11: expression[ch] = m->control_value; break;
        case 64:
            sustain[ch] = m->control_value;
            if (m->control_value < 64)
                release_sustain(ch);
            break;
        case 120:
        case 123:
            all_notes_off(ch);
            break;
        }
        break;
    case TML_PITCH_BEND:
        set_pitch(ch, m->pitch_bend);   // already 0..16383
        break;
    }
}

// Initial state
static void init_state(void) {
    int i;

    memset(voices, 0, sizeof(voices));
	
    for (i = 0; i < 16; i++) {
        program[i] = 0;
        volume[i] = 127;
        expression[i] = 127;
        sustain[i] = 0;
        pitch[i] = 0;
        pan[i] = 64; // default center
    }
}

static void render_samples(uint64_t samples) {
    while (samples) {
        int16_t buffer[AUDIO_BUFFER * 2];
        size_t n = samples > AUDIO_BUFFER ? AUDIO_BUFFER : (size_t)samples;

        adlib_getsample(buffer, n);
        if (audio_write(buffer, (int)n) < 0) {
            perror("audio_write");
            exit(1);
        }
        samples -= n;
    }
}

static void play(tml_message *msg) {
    uint64_t rendered = 0;

    for (; msg; msg = msg->next) {
        uint64_t target = (uint64_t)msg->time * SAMPLE_RATE / 1000;
        if (target > rendered) {
            render_samples(target - rendered);
            rendered = target;
        }
        process_event(msg);
    }
    render_samples((uint64_t)SAMPLE_RATE * 2);   // release tails
}

static int parse_note_list(const char *str, int *notes, int max_notes) {
    int count = 0;
    while (*str && count < max_notes) {
        while (*str == ',' || *str == ' ') str++; // skip separators
        if (!*str) break;

        char *end;
        long val = strtol(str, &end, 10);
        if (end == str) break; // no digit found

        notes[count++] = (int)val;
        str = end;
    }
    return count;
}

static void test_play_chord(const uint8_t inst_data[30], const int *notes, int note_count, int duration_ms) {
    if (note_count <= 0 || note_count > MAX_VOICES) {
        fprintf(stderr, "Invalid chord: %d notes (max %d)\n", note_count, MAX_VOICES);
        return;
    }

    // 1. Copy instrument to bank slot 0
    memcpy(bank.inst[0].data, inst_data, 30);
    bank.count = 1;

    // 2. Chip & Mode Init
    adlib_init(SAMPLE_RATE);

    adlib_write(0x105, 0x01); // OPL3 mode
    adlib_write(0x01, 0x20);  // Waveform Select
    adlib_write(0xBD, 0x00);  // Melodic mode

    // 3. Channel & Voice state
    init_state();
    volume[0] = 127;
    expression[0] = 127;
    pan[0] = 64; // Center
    pitch[0] = 0;

    // 4. Allocate voices and key on each note
    for (int i = 0; i < note_count; i++) {
        int v = i; // Use voices 0,1,2,... for chord notes

        voices[v].active = 1;
        voices[v].released = 0;
        voices[v].channel = 0;
        voices[v].note = notes[i];
        voices[v].velocity = 127;
        voices[v].prog = 0;

        load_opl_instrument(v, 0, 0);
        set_volume(v);
        opl_note(v, notes[i], 0, 1);
    }

    // 5. Audio loop: Hold duration
    uint64_t hold_frames = ((uint64_t)duration_ms * SAMPLE_RATE) / 1000;
    uint64_t release_frames = ((uint64_t)500 * SAMPLE_RATE) / 1000;
    uint64_t total_frames = hold_frames + release_frames;
    uint64_t rendered = 0;
    int keyed_off = 0;

    while (rendered < total_frames) {
        if (!keyed_off && rendered >= hold_frames) {
            // Key off all chord voices
            for (int i = 0; i < note_count; i++) {
                opl_note(i, notes[i], 0, 0);
            }
            keyed_off = 1;
        }

        uint64_t remaining = total_frames - rendered;
        size_t n = remaining > AUDIO_BUFFER ? AUDIO_BUFFER : (size_t)remaining;

        int16_t buffer[AUDIO_BUFFER * 2];
        adlib_getsample(buffer, (Bits)n);

        if (audio_write(buffer, (int)n) < 0) {
            perror("audio_write");
            exit(1);
        }

        rendered += n;
    }

    // Deactivate all voices
    for (int i = 0; i < note_count; i++) {
        voices[i].active = 0;
    }
}

static void test_play_instrument(const uint8_t inst_data[30], int note, int duration_ms) {
    // 1. Copy the instrument timbre into slot 0
    memcpy(bank.inst[0].data, inst_data, 30);
    bank.count = 1;

    // 2. Set channel 0 controller state
    init_state();
    volume[0] = 127;
    expression[0] = 127;
    pan[0] = 64; // use either 0 or 127 _NOT_ 63
    pitch[0] = 0;

	// 3. Setup voice 0
    voices[0].active = 1;
    voices[0].released = 0;
    voices[0].channel = 0;
    voices[0].note = note;
    voices[0].velocity = 127;	// Full velocity
    voices[0].prog = 0;

    // 4. Program OPL registers & set volume attenuation
    load_opl_instrument(0, 0, 0);
    set_volume(0);

    // 5. Key on
    opl_note(0, note, 0, 1);
    
    // 6. Audio loop: Note sustain duration
    uint64_t hold_frames = ((uint64_t)duration_ms * SAMPLE_RATE) / 1000;
    while (hold_frames > 0) {
        int16_t buffer[AUDIO_BUFFER * 2];
        size_t n = hold_frames > AUDIO_BUFFER ? AUDIO_BUFFER : (size_t)hold_frames;
        
        adlib_getsample(buffer, (Bits)n);
        if (audio_write(buffer, (int)n) < 0) {
            perror("audio_write");
            exit(1);
        }
        hold_frames -= n;
    }

    // 7. Key off
    opl_note(0, note, 0, 0);

    // 8. Audio loop: Release tail (500 ms)
    uint64_t release_frames = ((uint64_t)500 * SAMPLE_RATE) / 1000;
    while (release_frames > 0) {
        int16_t buffer[AUDIO_BUFFER * 2];
        size_t n = release_frames > AUDIO_BUFFER ? AUDIO_BUFFER : (size_t)release_frames;
        
        adlib_getsample(buffer, (Bits)n);
        if (audio_write(buffer, (int)n) < 0) {
            perror("audio_write");
            exit(1);
        }
        release_frames -= n;
    }
    voices[0].active = 0;
}

static int parse_hex_byte(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_raw_hex(const char *str, uint8_t out[30]) {
    if (strlen(str) < 60) return -1;
    for (int i = 0; i < 30; i++) {
        int hi = parse_hex_byte(str[i * 2]);
        int lo = parse_hex_byte(str[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

// Main
int main(int argc, char **argv) {
	
	int chord_notes[9];
	int chord_count = 0;
	
	int test_mode = 0;
    int test_note = 60;        // Default Middle C (C4)
    int test_duration = 1000;  // 1 second sustain
    int test_prog = -1;
    const char *bnk_file = NULL;
    uint8_t raw_data[30];
    int have_raw = 0;

    // Check for CLI testing flags
    for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-n") && i + 1 < argc) {
			const char *arg = argv[++i];
			
			// Check if it's a chord (contains comma) or single note
			if (strchr(arg, ',')) {
				chord_count = parse_note_list(arg, chord_notes, 9);
			} else {
				test_note = atoi(arg);
				chord_count = 0; // Single note mode
			}
			test_mode = 1;
			
		} else if (!strcmp(argv[i], "-d") && i + 1 < argc) {
			test_duration = atoi(argv[++i]);
			test_mode = 1;
			
		} else if (!strcmp(argv[i], "-p") && i + 1 < argc) {
			test_prog = atoi(argv[++i]);
			test_mode = 1;
			
		} else if (!strcmp(argv[i], "-b") && i + 1 < argc) {
			bnk_file = argv[++i];
			test_mode = 1;
			
		} else if (!strcmp(argv[i], "-r") && i + 1 < argc) {
			if (parse_raw_hex(argv[++i], raw_data) == 0) {
				have_raw = 1;
				test_mode = 1;
			} else {
				fprintf(stderr, "Invalid hex string (must be 60 hex characters for 30 bytes)\n");
				return 1;
			}
		}
	}
    
	if (test_mode) {
		if (audio_init(SAMPLE_RATE, 2) < 0) {
			fprintf(stderr, "Unable to initialize audio\n");
			return 1;
		}
		adlib_init(SAMPLE_RATE);
		adlib_write(0x105, 0x01); // OPL3 mode

		if (have_raw || (bnk_file && test_prog >= 0)) {
			uint8_t raw_inst[30];
			if (have_raw) {
				memcpy(raw_inst, raw_data, 30);
			} else {
				if (load_bnk(bnk_file) < 0 || test_prog >= bank.count) {
					fprintf(stderr, "Could not load program %d from bank %s\n", test_prog, bnk_file);
					audio_close();
					return 1;
				}
				memcpy(raw_inst, bank.inst[test_prog].data, 30);
			}

			if (chord_count > 0) {
				test_play_chord(raw_inst, chord_notes, chord_count, test_duration);
			} else {
				// Single note fallback
				test_play_instrument(raw_inst, test_note, test_duration);
			}
		}

		audio_close();
		return 0;
    } else {

	if (argc < 2 || argc > 3) {
		fprintf(stderr,	"usage: %s file.mid [instruments.bnk]\n", argv[0]);
		return 1;
	}

	if (argc == 3) {
		if (load_bnk(argv[2]) < 0) {
			fprintf(stderr, "Could not load bank %s\n", argv[2]);
			return 1;
		}
	} else {
		size_t size;
		size = (size_t)(_binary_mop_bnk_end - _binary_mop_bnk_start);

		if (load_bnk_memory(_binary_mop_bnk_start, size) < 0) {
			fprintf(stderr, "Could not load internal bank\n");
			return 1;
		}
	}
		
	tml_message *song = tml_load_filename(argv[1]);
	if (!song) {
		fprintf(stderr, "Could not load MIDI %s\n", argv[1]);
		return 1;
	}	

    if (audio_init(SAMPLE_RATE, 2) < 0) {
		fprintf(stderr, "Unable to initialize audio\n");
		return 1;
	}

    init_state();

    adlib_init(SAMPLE_RATE);
	adlib_write(0x105, 0x01);

    play(song);

    audio_close();
    free(events);
    tml_free(song);
    return 0;
	}	
}
