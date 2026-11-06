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
 * mop.c
 *
 * Simple MIDI -> AdLib/OPL3 in OP3-mode player using opal-opl and OSS /dev/dsp or tinyalsa.
 *
 * # Single note
 * ./mop -b mop.bnk -p 1 -n 60 -d 1500
 * ./mop -b GENMIDI.op2 -p 0 -n 60 -d 1500
 * ./mop -r 00000101030F050001030F000000000001040D0700020400000001010000 -n 60 -d 1500
 *
 * where p is program (instrument),  n note and d duration, r is a raw instrument:
 * 30 bytes (AdLib BNK) or 36 bytes (OP2) as hex
 *
 * # C major chord (C4, E4, G4)
 * ./mop -b mop.bnk -p 1 -n "60,64,67" -d 1500
 *
 * # G7 chord (G3, B3, D4, F4)
 * ./mop -b mop.bnk -p 1 -n "55,59,62,65" -d 1500
 *
 * # Raw hex with chord
 * ./mop -r 00000101030F050001030F000000000001040D0700020400000001010000 -n "60,64,67" -d 1500
 *
 * Usage:
 *     	mop song.mid [instruments.bnk|instruments.op2]
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
 *     	DMX OP2 (GENMIDI) format, detected by its "#OPL_II#" header.
 *     	Standard AdLib BNK format (leave out with -DNO_BNK).
 *     	Both are converted at load time to one internal format (INSTR).
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
    // buffered: writes are spaced like on real hardware, immediate writes sound uneven
    opalWriteRegBuffered(&chip, (uint16_t)reg, val);
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
#define AUDIO_BUFFER    1024	//1024
#define SYNTH_CHANNELS 	2	// physical output is always stereo

/*
 * A second voice only starts when more than this many voices are free and
 * silent, so a busy song keeps voices for new notes instead of starting second
 * voices that the next notes steal again.
 */
#ifndef SECOND_VOICE_RESERVE
#define SECOND_VOICE_RESERVE 1
#endif

/*
 * Internal instrument format.
 *
 * Every bank format is converted to this at load time, so the synth code
 * only knows one layout. It follows OP2 (DMX GENMIDI), which can describe
 * everything a BNK instrument can: a BNK instrument is an INSTR with one
 * voice, no fixed note and a base note offset of 0.
 */
#define INST_FIXED   0x01	// always play inst->note
#define INST_DOUBLE  0x04	// play both voices

typedef struct {
    uint8_t mod[5];		// register values: 0x20, 0x40 (KSL|TL), 0x60, 0x80, 0xE0
    uint8_t car[5];
    uint8_t c0;			// feedback/connection, stereo bits clear
    int8_t  offset;		// base note offset in semitones
} OPVOICE;

typedef struct {
    uint8_t flags;
    uint8_t fine;		// 128 = no detune; detunes the second voice like a pitch bend
    uint8_t note;		// fixed note
    OPVOICE v[2];
} INSTR;

#define NUM_MELODIC  128
#define NUM_PERC     47	// GM percussion notes 35..81
#define PERC_FIRST   35

#define BANK_BNK     1
#define BANK_OP2     2

typedef struct {
    INSTR melodic[NUM_MELODIC];
    INSTR perc[NUM_PERC];
    int type;			// BANK_BNK or BANK_OP2
} BANK;

typedef struct {
    int active;
    int released;
    int channel;
    int note;			// MIDI key, used to match note off
    int velocity;
    unsigned long age;
    const INSTR *ins;
    int sub;			// 0 = first voice, 1 = second voice of a double-voice instrument
} VOICE;

static BANK bank;
static VOICE voices[MAX_VOICES];

static uint8_t program[16];
static uint8_t volume[16];
static uint8_t expression[16];
static uint8_t sustain[16];
static uint8_t pan[16];	//0-127

static int pitch[16];

static unsigned long voice_age;

// The embedded bank. The Makefile copies $(BANK) to bank.bin first, so the
// symbol names stay the same whichever bank is embedded.
extern const unsigned char _binary_bank_bin_start[];
extern const unsigned char _binary_bank_bin_end[];


static const INSTR *percussion_instr(int note) {
    if (note < PERC_FIRST || note >= PERC_FIRST + NUM_PERC)
        return &bank.perc[0]; // fallback to first drum
    return &bank.perc[note - PERC_FIRST];
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

#ifndef NO_BNK
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
 *
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
 *
 * Instrument n of the bank (in name-table order) is MIDI program n for
 * n < 128, and drum note 35 + (n - BNK_PERC_BASE) for n >= BNK_PERC_BASE.
 * Missing entries fall back to instrument 0.
 */

#define BNK_HEADER_SIZE 28
#define BNK_NAME_SIZE   12
#define BNK_INST_SIZE   30
#define BNK_MAX_INST    256
#define BNK_PERC_BASE   163	// Percussion instruments start at this index in the bank

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

static void bnk_operator(const uint8_t *o, uint8_t wave, uint8_t r[5]) {
    r[0] = opl_reg20(o);
    r[1] = opl_reg40(o);
    r[2] = opl_reg60(o);
    r[3] = opl_reg80(o);
    r[4] = wave & 7;
}

// Convert one 30-byte BNK timbre
static void bnk_to_instr(const uint8_t *p, INSTR *in) {
    memset(in, 0, sizeof(*in));
    in->fine = 128;
    bnk_operator(p + 2, p[28], in->v[0].mod);
    bnk_operator(p + 15, p[29], in->v[0].car);
    in->v[0].c0 = opl_regc0(p + 2);
}

static int load_bnk_memory(const uint8_t *buf, size_t size) {
    const uint8_t *hdr;
    const uint8_t *names;
    const uint8_t *data;
    const uint8_t *list[BNK_MAX_INST];
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

    for (i = 0; i < num_instruments && count < BNK_MAX_INST; i++) {

        const uint8_t *entry;
        uint16_t midi_index;
        entry = names + i * BNK_NAME_SIZE;
        midi_index = le16(entry);

        if (midi_index >= num_instruments)
            continue;

        // HMI version 0.0 banks such as melodic.bnk have valid instruments with flags == 0
        if ((hdr[0] == 0 && hdr[1] == 0) || entry[2] != 0)
            list[count++] = data + (size_t)midi_index * BNK_INST_SIZE;
    }
    if (!count)
        return -1;

    for (i = 0; i < NUM_MELODIC; i++)
        bnk_to_instr(list[(int)i < count ? i : 0], &bank.melodic[i]);
    for (i = 0; i < NUM_PERC; i++) {
        int n = BNK_PERC_BASE + (int)i;
        // BNK drums have no fixed note: they play at the MIDI note, as before
        bnk_to_instr(list[n < count ? n : 0], &bank.perc[i]);
    }
    bank.type = BANK_BNK;
    return 0;
}
#endif /* NO_BNK */

// OP2
/*
 * DMX OP2 (GENMIDI), as described on the Doom Wiki GENMIDI page:
 *
 * header:
 *     8  "#OPL_II#"
 *
 * 175 instruments of 36 bytes (0-127 melodic, 128-174 drum notes 35-81):
 *     2  flags (bit 0 fixed pitch, bit 2 double voice)
 *     1  fine tuning of the second voice (128 = none, works like pitch bend)
 *     1  fixed note
 *    16  voice 1
 *    16  voice 2
 *
 * voice:
 *     6  modulator: 0x20, 0x60, 0x80, waveform, KSL, level
 *     1  feedback/connection (0xC0)
 *     6  carrier, same layout
 *     1  unused
 *     2  base note offset, signed
 *
 * 175 names of 32 bytes follow; they are not used, and the Makefile strips
 * them from the embedded bank.
 */

#define OP2_HEADER_SIZE 8
#define OP2_INST_SIZE   36
#define OP2_NUM_INST    (NUM_MELODIC + NUM_PERC)
#define OP2_DATA_SIZE   (OP2_HEADER_SIZE + OP2_NUM_INST * OP2_INST_SIZE)	// 6308

static void op2_operator(const uint8_t *o, uint8_t r[5]) {
    r[0] = o[0];
    r[1] = (uint8_t)((o[4] & 0xc0) | (o[5] & 0x3f));
    r[2] = o[1];
    r[3] = o[2];
    r[4] = o[3] & 7;
}

static void op2_voice(const uint8_t *p, OPVOICE *v) {
    int off = (int16_t)le16(p + 14);

    op2_operator(p, v->mod);
    op2_operator(p + 7, v->car);
    v->c0 = p[6] & 0x0f;
    if (off < -127) off = -127;
    if (off > 127) off = 127;
    v->offset = (int8_t)off;
}

// Convert one 36-byte OP2 instrument
static void op2_to_instr(const uint8_t *p, INSTR *in) {
    in->flags = (uint8_t)(le16(p) & (INST_FIXED | INST_DOUBLE));
    in->fine = p[2];
    in->note = p[3];
    op2_voice(p + 4, &in->v[0]);
    op2_voice(p + 20, &in->v[1]);
}

static int load_op2_memory(const uint8_t *buf, size_t size) {
    int i;
    const uint8_t *p = buf + OP2_HEADER_SIZE;

    if (size < OP2_DATA_SIZE)
        return -1;

    for (i = 0; i < NUM_MELODIC; i++, p += OP2_INST_SIZE)
        op2_to_instr(p, &bank.melodic[i]);
    for (i = 0; i < NUM_PERC; i++, p += OP2_INST_SIZE)
        op2_to_instr(p, &bank.perc[i]);
    bank.type = BANK_OP2;
    return 0;
}

static int load_bank_memory(const uint8_t *buf, size_t size) {
    if (size >= OP2_HEADER_SIZE && memcmp(buf, "#OPL_II#", OP2_HEADER_SIZE) == 0)
        return load_op2_memory(buf, size);
#ifndef NO_BNK
    return load_bnk_memory(buf, size);
#else
    return -1;
#endif
}

/*
 * Bank index as used by -p: 0-127 melodic, then the drums from the
 * format's own drum base (128 for OP2, 163 for BNK).
 */
static const INSTR *bank_index(int idx) {
    int perc_base = bank.type == BANK_OP2 ? NUM_MELODIC : 163;	// 163 = BNK_PERC_BASE

    if (idx >= 0 && idx < NUM_MELODIC)
        return &bank.melodic[idx];
    if (idx >= perc_base && idx < perc_base + NUM_PERC)
        return &bank.perc[idx - perc_base];
    return NULL;
}

static int load_bank(const char *filename) {
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
    ret = load_bank_memory(buf, (size_t)len);
    free(buf);
    return ret;
}

// OPL instrument
static int pan_to_stereo_mode(uint8_t pan_val) {
    if (pan_val < 48)
        return 1; // left only
    if (pan_val > 80)
        return 2; // right only
    return 3;     // center (both)
}

static void load_opl_instrument(int voice) {
    const OPVOICE *ov = &voices[voice].ins->v[voices[voice].sub];
    int channel = voices[voice].channel;
    uint8_t con;
    int mode;

    adlib_write(MODR(voice, 0x20), ov->mod[0]);
    adlib_write(MODR(voice, 0x40), ov->mod[1]);
    adlib_write(MODR(voice, 0x60), ov->mod[2]);
    adlib_write(MODR(voice, 0x80), ov->mod[3]);
    adlib_write(MODR(voice, 0xe0), ov->mod[4]);

    adlib_write(CARR(voice, 0x20), ov->car[0]);
    adlib_write(CARR(voice, 0x40), ov->car[1]);
    adlib_write(CARR(voice, 0x60), ov->car[2]);
    adlib_write(CARR(voice, 0x80), ov->car[3]);
    adlib_write(CARR(voice, 0xe0), ov->car[4]);

    // Add stereo select bits based on channel pan
    con = ov->c0 & 0x0f;
    mode = pan_to_stereo_mode(pan[channel]);
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

/*
 * Key a voice on or off at its instrument's pitch:
 * the MIDI key (or the fixed note) plus the voice's base note offset,
 * plus the channel pitch bend. The second voice of a double-voice
 * instrument is also detuned by the fine tuning byte, 1/64 semitone per
 * step from 128. The Doom Wiki only says it works "like a pitch bend"
 * (0x82 = +256); the scale is taken from the bank data: Overdriven Guitar
 * has offsets -12/+2 and fine 6, so 1/64 puts its second voice 9 cents
 * above the octave, where 1/32 would put it 1.8 semitones flat.
 *
 * Notes outside 0..127 fold back by octaves, as DMX folds notes outside
 * its range (large negative offsets reach below 0).
 */
static void voice_key(int v, int on) {
    const VOICE *vc = &voices[v];
    const INSTR *in = vc->ins;
    int note = (in->flags & INST_FIXED) ? in->note : vc->note;
    int bend = pitch[vc->channel];

    note += in->v[vc->sub].offset;
    if (vc->sub)
        bend += ((int)in->fine - 128) * 64;	// 8192 = 2 semitones
    while (note < 0)
        note += 12;
    while (note > 127)
        note -= 12;
    opl_note(v, note, bend, on);
}

// Volume
static uint8_t scale_level(uint8_t reg40, int att) {
    int level = (reg40 & 63) + att;

    if (level > 63)
        level = 63;
    return (uint8_t)((reg40 & 0xc0) | level);
}

/*
 * DMX plays both carriers of a double-voice instrument at the level of the
 * first voice's carrier (Doom Wiki), and OP2 banks are made for that.
 */
static void set_volume(int voice) {
    const OPVOICE *ov = &voices[voice].ins->v[voices[voice].sub];
    const OPVOICE *ov0 = &voices[voice].ins->v[0];
    int amp;
    int att;

    if (is_percussion_channel(voices[voice].channel) && bank.type == BANK_BNK) {
        // BNK percussion: ignore channel volume/expression, use velocity only,
        // with a slight boost to match melodic instruments.
        // OP2 drums are loud enough and follow the channel like other notes.
        int vel = voices[voice].velocity; // 0..127
        // Map 1..127 -> ~64..127 to keep drums present even at low velocity
        amp = 60 + (vel * 63) / 127;
    } else {
        amp = volume[voices[voice].channel];
        amp = amp * expression[voices[voice].channel] / 127;
        amp = amp * voices[voice].velocity / 127;
    }
    // Convert amplitude to additional TL attenuation
    att = (127 - amp) * 63 / 127;

    adlib_write(CARR(voice, 0x40), scale_level((uint8_t)((ov->car[1] & 0xc0) | (ov0->car[1] & 0x3f)), att));
    // Additive connection: the modulator is heard directly, so scale it too
    if (ov->c0 & 1)
        adlib_write(MODR(voice, 0x40), scale_level(ov->mod[1], att));
}

// Voices
#ifdef MOP_STATS
static unsigned long stat_cut, stat_steal2, stat_dropped, stat_hot, stat_faded;
#endif

/*
 * How loud a voice is right now, read from the chip: the lowest
 * attenuation of its audible operators, in 0.1875 dB steps
 * (0 = loudest, >= 511 = silent). Opal documents egOut for this use.
 * OPL voice v is Opal channel v.
 */
#define ATTEN_SILENT 511
#define ATTEN_QUIET  320	// 60 dB down: reusing the voice will not click

static int voice_atten(int v) {
    const OpalChannel *c = &chip.chan[v];
    int a = chip.op[c->opSlot[1]].egOut;	// carrier

    if (!voices[v].ins)
        return ATTEN_SILENT;
    if ((voices[v].ins->v[voices[v].sub].c0 & 1) && chip.op[c->opSlot[0]].egOut < a)
        a = chip.op[c->opSlot[0]].egOut;	// additive: the modulator is heard too
    return a < ATTEN_SILENT ? a : ATTEN_SILENT;
}

/*
 * Fade out what a voice still plays before reusing it. Starting a note on
 * a voice that can still be heard restarts the waveform and clicks.
 * Set the fastest release rate (from full level to -60 dB in 1.6 ms),
 * key off, then hold the following writes back by about 2 ms: the write
 * buffer spaces writes OPAL_WRITEBUF_DELAY chip samples apart, so
 * repeating the key-off write FADE_WRITES times is a delay.
 * At most FADE_MAX fades per instant, so a chord is delayed by at most
 * about 6 ms.
 */
#define FADE_WRITES 50
#define FADE_MAX    3
static int fades_now;	// fades since audio was last rendered

static void fade_voice(int v) {
    const OPVOICE *ov = &voices[v].ins->v[voices[v].sub];
    int i;

    if (fades_now >= FADE_MAX)
        return;
    fades_now++;
#ifdef MOP_STATS
    stat_faded++;
#endif
    adlib_write(MODR(v, 0x80), (uint8_t)(ov->mod[3] | 0x0f));
    adlib_write(CARR(v, 0x80), (uint8_t)(ov->car[3] | 0x0f));
    for (i = 0; i < FADE_WRITES; i++)
        voice_key(v, 0);
}

// Is voice a a better choice to (re)use than voice b: quieter, or as quiet and older
static int quieter(int a, int b) {
    int la = voice_atten(a), lb = voice_atten(b);

    if (la != lb)
        return la > lb;
    return voices[a].age < voices[b].age;
}

static void voice_free(int v) {
    if (voices[v].ins)
        voice_key(v, 0);
    voices[v].active = 0;
    voices[v].released = 0;
    voices[v].age = ++voice_age;   // age now = time of release
}

/*
 * Find a voice: the quietest free voice, so a new note does not start on
 * a voice whose release can still be heard (that clicks). If none is free
 * and may_steal is set, steal one: second voices of double-voice
 * instruments first (the note goes on with its first voice), then
 * sustain-held voices, then the oldest.
 * Returns -1 if none is free and may_steal is 0.
 */
static int alloc_voice(int may_steal) {
    int i, best = -1;

    for (i = 0; i < MAX_VOICES; i++)
        if (!voices[i].active && (best < 0 || quieter(i, best)))
            best = i;
    if (best >= 0 || !may_steal)
        return best;

    for (i = 0; i < MAX_VOICES; i++) {
        const VOICE *a = &voices[i];
        const VOICE *b;

        if (best < 0) {
            best = i;
            continue;
        }
        b = &voices[best];
        if (a->sub != b->sub) {
            if (a->sub > b->sub)
                best = i;
        } else if (a->released != b->released) {
            if (a->released > b->released)
                best = i;
        } else if (a->age < b->age) {
            // by age: a note still in its attack reads as quiet
            best = i;
        }
    }

#ifdef MOP_STATS
    if (voices[best].sub)
        stat_steal2++;
    else
        stat_cut++;
#endif

    voice_free(best);
    return best;
}

// Free voices that are silent enough to start a note on without a click
static int quiet_voices(void) {
    int i, n = 0;

    for (i = 0; i < MAX_VOICES; i++)
        n += !voices[i].active && voice_atten(i) >= ATTEN_QUIET;
    return n;
}

static void note_on(int channel, int note, int velocity) {
    const INSTR *ins;
    int i, n;

    // retrigger: release whatever still plays this key
    for (i = 0; i < MAX_VOICES; i++)
        if (voices[i].active && voices[i].channel == channel && voices[i].note == note)
            voice_free(i);

    if (is_percussion_channel(channel))
        ins = percussion_instr(note);	// Ignore program[channel]
    else
        ins = &bank.melodic[program[channel]];

    /*
     * The second voice of a double-voice instrument is optional: it only
     * takes a free voice and never steals one. Without a free voice the
     * note plays with its first voice alone.
     */
    n = (ins->flags & INST_DOUBLE) ? 2 : 1;
    for (i = 0; i < n; i++) {
        int v;

        if (i == 1 && quiet_voices() <= SECOND_VOICE_RESERVE) {
#ifdef MOP_STATS
            stat_dropped++;
#endif
            break;
        }
        v = alloc_voice(i == 0);

        if (v < 0) {
#ifdef MOP_STATS
            stat_dropped++;
#endif
            break;
        }
        if (voice_atten(v) < ATTEN_QUIET) {
#ifdef MOP_STATS
            stat_hot++;
#endif
            fade_voice(v);	// still uses the old instrument and note
        }
        voices[v].active = 1;
        voices[v].released = 0;
        voices[v].channel = channel;
        voices[v].note = note;
        voices[v].velocity = velocity;
        voices[v].age = ++voice_age;
        voices[v].ins = ins;
        voices[v].sub = i;

        load_opl_instrument(v);
        set_volume(v);
        voice_key(v, 1);
    }
}

static void note_off(int channel, int note) {
    int i;

    for (i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].active || voices[i].released ||
            voices[i].channel != channel || voices[i].note != note)
            continue;
        if (sustain[channel] >= 64)
            voices[i].released = 1;
        else
            voice_free(i);
    }
}

static void release_sustain(int channel) {
    int i;

    for (i = 0; i < MAX_VOICES; i++)
        if (voices[i].active && voices[i].channel == channel && voices[i].released)
            voice_free(i);
}

static void all_notes_off(int channel) {
    int i;

    for (i = 0; i < MAX_VOICES; i++)
        if (voices[i].active && voices[i].channel == channel)
            voice_free(i);
}

static void set_pitch(int channel, int value) {
    int i;
    pitch[channel] = value - 8192;
    for (i = 0; i < MAX_VOICES; i++)
        if (voices[i].active && voices[i].channel == channel)
            voice_key(i, 1);
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
        if (!is_percussion_channel(ch))
            program[ch] = m->program & (NUM_MELODIC - 1);
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
        fades_now = 0;
        if (audio_write(buffer, (int)n) < 0) {
            perror("audio_write");
            exit(1);
        }
        samples -= n;
    }
}

#define MAX_F2_TRACKS 256
static unsigned int track_boundary_ms[MAX_F2_TRACKS];
static int track_boundary_count = 0;
static void reset_channels(void) {
    int i;
    for (i = 0; i < MAX_VOICES; i++)
        if (voices[i].active)
            voice_free(i);   // keys off + clears the voice as in the allocator
    init_state();            // program/volume/expression/sustain/pitch/pan back to defaults
}

static void play(tml_message *msg) {
    uint64_t rendered = 0;
    
	int boundary_idx = 0;

    for (; msg; msg = msg->next) {
		while (boundary_idx < track_boundary_count && msg->time >= track_boundary_ms[boundary_idx]) {
			reset_channels();
			boundary_idx++;
		}

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

static void chip_init(void) {
    adlib_init(SAMPLE_RATE);
    adlib_write(0x105, 0x01); // OPL3 mode
    adlib_write(0x104, 0x00); // no 4-operator channels
    adlib_write(0x01, 0x20);  // waveform select (needed on OPL2, harmless on OPL3)
    adlib_write(0xbd, 0x00);  // melodic mode
}

/*
 * Test mode: play one instrument as a single note or a chord through the
 * normal note_on/note_off path, so double voices, fixed notes and voice
 * allocation behave exactly as in playback.
 */
static void test_play(const INSTR *ins, const int *notes, int note_count, int duration_ms) {
    int i;

    if (note_count <= 0 || note_count > MAX_VOICES) {
        fprintf(stderr, "Invalid chord: %d notes (max %d)\n", note_count, MAX_VOICES);
        return;
    }

    // The instrument goes into melodic slot 0, played on channel 0
    bank.melodic[0] = *ins;
    init_state();
    chip_init();

    for (i = 0; i < note_count; i++)
        note_on(0, notes[i], 127);
    render_samples(((uint64_t)duration_ms * SAMPLE_RATE) / 1000);

    for (i = 0; i < note_count; i++)
        note_off(0, notes[i]);
    render_samples(((uint64_t)500 * SAMPLE_RATE) / 1000);   // release tail
}

static int parse_hex_byte(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parse a raw instrument: 60 hex chars (30-byte BNK) or 72 (36-byte OP2)
static int parse_raw_hex(const char *str, INSTR *out) {
    uint8_t raw[OP2_INST_SIZE];
    size_t len = strlen(str);
    size_t n = len / 2;

    if (len % 2 || (n != OP2_INST_SIZE
#ifndef NO_BNK
                    && n != BNK_INST_SIZE
#endif
                    ))
        return -1;
    for (size_t i = 0; i < n; i++) {
        int hi = parse_hex_byte(str[i * 2]);
        int lo = parse_hex_byte(str[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        raw[i] = (uint8_t)((hi << 4) | lo);
    }
#ifndef NO_BNK
    if (n == BNK_INST_SIZE) {
        bnk_to_instr(raw, out);
        return 0;
    }
#endif
    op2_to_instr(raw, out);
    return 0;
}

// Function that finds the raw MIDI data inside a RIFF container
unsigned char* unwrap_riff_midi(unsigned char* data, unsigned int* size) {

    if (*size < 20) return data;


    if (memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "RMID", 4) == 0){
        unsigned int offset = 12;
        
        while (offset + 8 < *size) {
            unsigned int chunk_size = data[offset + 4] | 
                                      (data[offset + 5] << 8) | 
                                      (data[offset + 6] << 16) | 
                                      (data[offset + 7] << 24);
            
            if (memcmp(data + offset, "data", 4) == 0) {
                if (offset + 8 + chunk_size <= *size) {
                    *size = chunk_size;
                    return data + offset + 8;
                }
            }
            offset += 8 + ((chunk_size + 1) & ~1);
        }
    }
    return data;
}

static int load_embedded_bank(void) {
	size_t size = (size_t)(_binary_bank_bin_end - _binary_bank_bin_start);

	if (load_bank_memory(_binary_bank_bin_start, size) < 0) {
		fprintf(stderr, "Could not load internal bank\n");
		return -1;
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
    INSTR raw_inst;
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
			if (parse_raw_hex(argv[++i], &raw_inst) == 0) {
				have_raw = 1;
				test_mode = 1;
			} else {
				fprintf(stderr, "Invalid hex string (must be 60 hex characters for a 30-byte BNK "
				                "instrument or 72 for a 36-byte OP2 instrument)\n");
				return 1;
			}
		}
	}
    
	if (test_mode) {
		const INSTR *ins = NULL;

		if (have_raw) {
			ins = &raw_inst;
		} else if (test_prog >= 0) {
			// -p without -b uses the built-in bank
			if (bnk_file ? load_bank(bnk_file) < 0 : load_embedded_bank() < 0) {
				fprintf(stderr, "Could not load bank %s\n", bnk_file ? bnk_file : "(built-in)");
				return 1;
			}
			ins = bank_index(test_prog);
			if (!ins) {
				fprintf(stderr, "No program %d in bank %s\n", test_prog, bnk_file ? bnk_file : "(built-in)");
				return 1;
			}
			raw_inst = *ins;	// test_play overwrites melodic slot 0
			ins = &raw_inst;
		}

		if (ins) {
			if (audio_init(SAMPLE_RATE, 2) < 0) {
				fprintf(stderr, "Unable to initialize audio\n");
				return 1;
			}
			if (chord_count > 0)
				test_play(ins, chord_notes, chord_count, test_duration);
			else
				test_play(ins, &test_note, 1, test_duration);
			audio_close();
		}
		return 0;
    } else {

	if (argc < 2 || argc > 3) {
		fprintf(stderr,	"usage: %s file.mid [instruments.bnk|instruments.op2]\n", argv[0]);
		return 1;
	}

	if (argc == 3) {
		if (load_bank(argv[2]) < 0) {
			fprintf(stderr, "Could not load bank %s\n", argv[2]);
			return 1;
		}
	} else if (load_embedded_bank() < 0) {
		return 1;
	}
	// RIFF (little-endian) data guard
	FILE* f = fopen(argv[1], "rb");
	if (!f) {
		fprintf(stderr, "Could not open file %s\n", argv[1]);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	unsigned int file_size = ftell(f);
	fseek(f, 0, SEEK_SET);

	unsigned char* file_buffer = (unsigned char*)malloc(file_size);
	if (!file_buffer) {
		fprintf(stderr, "Could not allocate memory.\n");
		fclose(f);
		return 1;
	}

	size_t bytes_read = fread(file_buffer, 1, file_size, f);
	fclose(f);
	
	if (bytes_read != file_size) {
		fprintf(stderr, "Could not read the entire file (read %zu of %u bytes).\n", bytes_read, file_size);
		free(file_buffer);
		return 1;
	}

	// RMID-unpack
	unsigned int midi_size = file_size;
	unsigned char* raw_midi_ptr = unwrap_riff_midi(file_buffer, &midi_size);
	
	tml_message* song = NULL;

	// check MIDI format and read
	if (midi_size >= 14 && memcmp(raw_midi_ptr, "MThd", 4) == 0)  {
		unsigned short midi_format = (raw_midi_ptr[8] << 8) | raw_midi_ptr[9];
		unsigned short tracks = (raw_midi_ptr[10] << 8) | raw_midi_ptr[11];

		if (midi_format > 2) {
			fprintf(stderr, "%s: MIDI format %u unsupported\n", argv[1], midi_format);
			free(file_buffer);
			return 1;
		}

		if (midi_format == 2) {
			unsigned char* track_ptr = raw_midi_ptr + 14;
			unsigned int accumulated_time_ms = 0;
			tml_message* last_msg = NULL;

			// We create a temporary buffer for a valid 1-track MIDI file in memory
			// A MThd header (14 bytes) + a MTrk spor
			unsigned char* tmp_midi = (unsigned char*)malloc(midi_size);
			if (!tmp_midi) {
				fprintf(stderr, "Could not allocate temporary memory for Format 2.\n");
				free(file_buffer);
				return 1;
			}

			// Copy the original MThd header into our temporary buffer
			memcpy(tmp_midi, raw_midi_ptr, 14);
			// Force the format to be Format 1 (or 0) and the number of tracks to be 1
			tmp_midi[8] = 0; tmp_midi[9] = 1;   // Format 1
			tmp_midi[10] = 0; tmp_midi[11] = 1; // 1 Track

			for (unsigned short i = 0; i < tracks; i++) {
				if (track_ptr + 8 > raw_midi_ptr + midi_size) break;
				if (memcmp(track_ptr, "MTrk", 4) != 0) break;

				unsigned int track_len = (track_ptr[4] << 24) | (track_ptr[5] << 16) | 
				                         (track_ptr[6] << 8)  | track_ptr[7];
				
				if (track_ptr + 8 + track_len > raw_midi_ptr + midi_size) break;

				// Copy the track into the temporary MIDI file right after the header
				memcpy(tmp_midi + 14, track_ptr, 8 + track_len);
				unsigned int tmp_midi_size = 14 + 8 + track_len;

				// Let tml.h load this single track
				tml_message* track_song = tml_load_memory(tmp_midi, tmp_midi_size);

				if (track_song) {
					tml_message* curr = track_song;
					tml_message* prev = NULL;

					// Add the accumulated time from the previous tracks to all events in this track
					while (curr) {
						curr->time += accumulated_time_ms;
						prev = curr;
						curr = curr->next;
					}

					if (prev) {
						accumulated_time_ms = prev->time;
						if (track_boundary_count < MAX_F2_TRACKS)
							track_boundary_ms[track_boundary_count++] = accumulated_time_ms;
					}

					// Chain the track together with the main list (song)
					if (!song) {
						song = track_song;
					} else if (last_msg) {
						last_msg->next = track_song;
					}
					
					if (prev) {
						last_msg = prev;
					}
				}

				// Go to the next track in the original file
				track_ptr += 8 + track_len;
			}

			free(tmp_midi);
		} 
		else {
			// --- FORMAT 0 and 1 PROCESSING (Standard tml.h loading) ---
			song = tml_load_memory(raw_midi_ptr, midi_size);
		}
	}
	free(file_buffer);

	if (!song) {
		fprintf(stderr, "Could not load MIDI data.\n");
		return 1;
	}
	
    if (audio_init(SAMPLE_RATE, 2) < 0) {
		fprintf(stderr, "Unable to initialize audio\n");
		return 1;
	}

    init_state();
    chip_init();

    play(song);

    audio_close();
    tml_free(song);	// file_buffer was already freed after loading
#ifdef MOP_STATS
    fprintf(stderr, "notes cut: %lu, second voices stolen: %lu, second voices not started: %lu, "
            "voices reused while audible: %lu (faded: %lu)\n",
            stat_cut, stat_steal2, stat_dropped, stat_hot, stat_faded);
#endif
    return 0;
	}	
}
