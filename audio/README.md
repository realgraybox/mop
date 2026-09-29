# audio
Single-file audio abstraction (audio.c).
OSS and TinyALSA backends.
Compile-time backend selection.

Contract:
    Signed 16-bit little-endian PCM
    Interleaved stereo (2 channels)
    frames is the number of stereo frames, not bytes

Backend selection
Control backends at compile time:

    -DAUDIO_HAVE_OSS – enable OSS
    -DAUDIO_HAVE_TINYALSA – enable TinyALSA

If both are enabled, OSS is tried first; TinyALSA is the fallback.
