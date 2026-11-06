

https://github.com/user-attachments/assets/87c7bb41-bece-4cca-9077-bf52b81d98b6

# mop

A small, self-contained MIDI player for Linux that renders General MIDI
files through OPL3 FM synthesis — the classic AdLib / Sound Blaster sound
of DOS-era games, not sampled/wavetable playback.

It's built to be tiny: a static, uClibc-linked build comes in at around
**65 KB**.

## Why

Most MIDI players either need a full audio/synth stack, or lean on
wavetable soundfonts to get General MIDI sound (which is a different
sound entirely — more like an SB16 AWE than a plain AdLib/SB16). This
project goes the other way: a minimal MIDI parser feeding a
cycle-accurate OPL3 emulator, with no dependencies beyond the two
libraries below and your audio device.

## Features

- MIDI file format 0 and 1
- DMX `.op2` (GENMIDI) banks, including double-voice instruments, fixed-pitch
  drums, base note offsets and fine tuning
- Standard AdLib `.bnk` instrument banks (melodic and percussion)
- Note on/off, program change, pitch bend, tempo changes
- Controllers: 7 (volume), 11 (expression), 64 (sustain), 120/123 (all notes off)
- Full 18-voice OPL3 polyphony (both register banks), with
  least-recently-released voice stealing
- Stereo panning via controller 10
- OSS `/dev/dsp` and tinyalsa output
- A `-r`/`-n`/`-p`/`-d`/`-b` test mode for auditioning single instruments
  or chords without a MIDI file

## How it works

- **MIDI parsing** — [tml.h](https://github.com/schellingb/TinySoundFont)
  (the MIDI-loader half of TinySoundFont). Header-only, MIT licensed.
  Events arrive pre-sorted with tempo already resolved to millisecond
  timestamps.
- **FM synthesis** — [Opal](https://github.com/RealBitdancer/opal), a
  cycle-accurate OPL2/OPL3 (YMF262) emulator in C11. The core is public
  domain (ported from the Reality Adlib Tracker 2 core by Shayde), the
  C API/resampler wrapper is MIT.
- **Instrument banks** — DMX OP2 (GENMIDI) and standard AdLib BNK. Both are
  converted at load time into one internal format (OPL register values per
  voice), so the synth only knows one layout. The format is detected by the
  `#OPL_II#` header. The OP2 loader follows the
  [Doom Wiki GENMIDI page](https://doomwiki.org/wiki/GENMIDI).
- **Double-voice instruments** — the second voice is optional: it only takes
  a free OPL voice and never steals one, and only when more than
  `SECOND_VOICE_RESERVE` voices are free. When stealing, second voices go
  first, so under load a note loses its second voice instead of another
  note being cut off.

## Building

View the build options in Makefile. OSS only, TinyALSA only or both is possible. If both are compiled in via -DAUDIO_HAVE_OSS -DAUDIO_HAVE_TINYALSA - the OSS is tried first with TinyALSA as fallback.

```sh
# Built-in bank: mop.bnk (default)
make

# Built-in bank: DMXOPL3 (OP2). The names are stripped when embedding.
make clean && make BANK=banks/GENMIDI.op2
```

Extra `CFLAGS` options:

| Define | Effect |
|--------|--------|
| `-DNO_BNK` | Leave out the BNK loader (OP2 only; the built-in bank must be `.op2`) |
| `-DMOP_STATS` | Print notes cut, second voices stolen and second voices not started after playback |
| `-DSECOND_VOICE_RESERVE=n` | Start a second voice only when more than n voices are free (default 2) |
## Usage

```sh
# Play a MIDI file with an external instrument bank (.bnk or .op2)
./mop song.mid instruments.bnk
./mop song.mid banks/GENMIDI.op2

# Play a MIDI file using the built-in bank
./mop song.mid
```

### Instrument test mode

```sh
# Single note: program 1 from mop.bnk, note 60 (middle C), 1500ms
./mop -b mop.bnk -p 1 -n 60 -d 1500

# Program 0 from an OP2 bank, or from the built-in bank when -b is left out
./mop -b banks/GENMIDI.op2 -p 0 -n 60 -d 1500
./mop -p 0 -n 60 -d 1500

# C major chord (C4, E4, G4)
./mop -b mop.bnk -p 1 -n "60,64,67" -d 1500

# G7 chord (G3, B3, D4, F4)
./mop -b mop.bnk -p 1 -n "55,59,62,65" -d 1500

# Raw 30-byte AdLib instrument, as hex, with a chord
./mop -r 00000101030F050001030F000000000001040D0700020400000001010000 -n "60,64,67" -d 1500
```

| Flag | Meaning |
|------|---------|
| `-b <file>` | Load instrument bank from a `.bnk` or `.op2` file (default: built-in) |
| `-p <n>` | Instrument number: 0-127 melodic; drums from 128 (OP2) or 163 (BNK) |
| `-r <hex>` | Raw instrument: 30-byte AdLib (60 hex chars) or 36-byte OP2 (72 hex chars) |
| `-n <note[,note...]>` | Note (or comma-separated chord) as a MIDI note number |
| `-d <ms>` | Note duration in milliseconds |

## Limitations

- No SysEx or meta-event handling beyond tempo (lyrics/text/markers are
  parsed but discarded, since `tml.h` doesn't surface them)
- 18-voice polyphony ceiling — dense arrangements will still steal voices
- No General MIDI loudness normalization; playback volume follows
  whatever each file's CC7/velocity data specifies

## License

`mop.c` and the rest of this project's own source are licensed as stated in the source code (zlib license).

Bundled/linked dependencies carry their own licenses:

- [tml.h](https://github.com/schellingb/TinySoundFont) — MIT
- [Opal](https://github.com/RealBitdancer/opal) — MIT (C API/wrapper),
  public domain (emulation core)
- `banks/GENMIDI.op2` — [DMXOPL3 by Sneakernets](https://github.com/sneakernets/DMXOPL),
  MIT (see `banks/DMXOPL-LICENSE`)

See each project's repository for full license text and attribution
requirements.

## Acknowledgments

- Shayde of Reality, for the original public-domain Opal core
- [RealBitdancer](https://github.com/RealBitdancer/opal) for the C11 port
- [schellingb](https://github.com/schellingb/TinySoundFont) for tml.h
- DMXOPL3 by Sneakernets for the OP2 bank
