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
- **Instrument banks** — standard AdLib BNK format, parsed directly into
  OPL2 operator registers.

## Building

```sh
View the build options in Makefile. OSS only, TinyALSA only or both is possible. If both are compiled in via -DAUDIO_HAVE_OSS -DAUDIO_HAVE_TINYALSA - the OSS is tried first with TinyALSA as fallback.
```
## Usage

```sh
# Play a MIDI file with an external instrument bank
./mop song.mid instruments.bnk

# Play a MIDI file using the built-in bank
./mop song.mid
```

### Instrument test mode

```sh
# Single note: program 1 from sc3.bnk, note 60 (middle C), 1500ms
./mop -b sc3.bnk -p 1 -n 60 -d 1500

# C major chord (C4, E4, G4)
./mop -b sc3.bnk -p 1 -n "60,64,67" -d 1500

# G7 chord (G3, B3, D4, F4)
./mop -b sc3.bnk -p 1 -n "55,59,62,65" -d 1500

# Raw 30-byte AdLib instrument, as hex, with a chord
./mop -r 00000101030F050001030F000000000001040D0700020400000001010000 -n "60,64,67" -d 1500
```

| Flag | Meaning |
|------|---------|
| `-b <file>` | Load instrument bank from a `.bnk` file |
| `-p <n>` | Program (instrument) number from the bank |
| `-r <hex>` | Raw 30-byte AdLib instrument definition (60 hex chars) |
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

See each project's repository for full license text and attribution
requirements.

## Acknowledgments

- Shayde of Reality, for the original public-domain Opal core
- [RealBitdancer](https://github.com/RealBitdancer/opal) for the C11 port
- [schellingb](https://github.com/schellingb/TinySoundFont) for tml.h
