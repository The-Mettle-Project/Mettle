# References

What audio follows and where its techniques come from. No code was copied
from any GPL or LGPL project; the permissive references below were studied
and their methods written again in Mettle.

## Standards

| Area | Standard |
|---|---|
| WAV | Microsoft RIFF/WAVE and WAVE_FORMAT_EXTENSIBLE; EBU Tech 3306 (RF64); ITU-T G.711; IMA ADPCM (1992); Microsoft ADPCM |
| FLAC | RFC 9639 |
| Ogg | RFC 3533 |
| Vorbis | Vorbis I specification (Xiph.Org) |
| Opus | RFC 6716, RFC 8251, RFC 7845 |
| MP3 | ISO/IEC 11172-3, ISO/IEC 13818-3, MPEG-2.5 |
| MD5 | RFC 1321 |
| Downmix | ITU-R BS.775 |

## Implementations studied, and what was taken

### WAV and the sample formats
- **dr_wav** (public domain / MIT-0): which odd WAV files to accept (cut-short
  data, RF64/BW64, extensible sub-formats), and the ADPCM block layouts.
- **miniaudio** (public domain / MIT-0): the sample conversion rules
  (integers through a full-scale 32-bit form). We scale float to integer by
  2^(bits-1) and clip, so integer -> float -> integer gives back the same
  samples.

### FLAC
- **libFLAC** (BSD-3): the encoder's compression levels (block size, LPC
  order, partition order, stereo search, apodization windows tukey(0.5),
  partial_tukey(2) and punchout_tukey(3)), LPC order selection from the
  expected bits per residual, coefficient quantization with the error
  carried forward, the precision limit that keeps the predictor in 32 bits,
  and choosing the fixed predictor from residual sums in one pass.
- **dr_flac** (public domain / MIT-0): decoding Rice codes straight from a
  64-bit bit cache with a table for the leading zeros, and refilling the
  cache a word at a time.
- **ffmpeg** (behaviour only): picking the stereo coding from estimates
  below level 7 instead of coding all four choices; used as the reference
  decoder for our output.

### Ogg and Vorbis
- **stb_vorbis** (public domain / MIT): building codewords, the 10-bit fast
  Huffman table with a binary search for longer codes, floor 1 line drawing
  and the deferred floor, inverse coupling, the windows, and the fast inverse
  MDCT (from Sporer, Brandenburg and Edler's filter bank paper). One fix
  against it: the first floor 1 values are read with `ilog(range - 1)` bits
  as the spec says.
- **libvorbis** (BSD-3): rejecting incomplete Huffman trees, the
  single-entry codebook case.
- **ffmpeg** (behaviour only): the channel order table and end trimming
  checked against its output.
- Seeking: a granule bisection over Ogg pages, after the idea in
  stb_vorbis, then a scan back from the next page's granule.

### Opus
- **libopus** (BSD-3), the reference decoder of RFC 6716: the float CELT
  decoder (band energies, bit allocation, PVQ and cwrs, spreading, TF,
  intensity and dual stereo, anti-collapse, the MDCT through kiss_fft with
  radix 2/3/4/5, the comb post-filter, de-emphasis, and the approximate
  `celt_exp2`), the fixed-point SILK decoder (NLSF, LTP, LPC synthesis and
  stability, stereo prediction) with its IIR/FIR resampler, and the frame
  glue, redundancy and multistream layout of opus_decoder.c and
  opus_multistream_decoder.c. Every table was checked byte for byte against
  an installed libopus.

### MP3
- **minimp3** (CC0): the frame structure, packed Huffman tables and decoder,
  scale factors, the x^(4/3) table, reordering, alias reduction, IMDCT-36/12
  through 9- and 3-point DCTs, polyphase synthesis with its fast 32-point
  DCT-II, the Layer I/II decoder, the sync rule (the next frames must
  match), the bit reservoir, and the Xing/LAME and ID3 handling of
  minimp3_ex. One change: the bit cache is refilled before reading linbits.
- **ffmpeg** (behaviour only): which LAME tags give the delay and padding,
  and the start and end trimming formula.

### Resampling, mixing, devices
- **speexdsp** (BSD-3): the resampler: quality levels 0-10 (filter length,
  pass band, Kaiser window), the ratio reduced by its greatest common
  divisor, narrowing and stretching the filter when going down in rate, and
  the oversampled filter read with cubic interpolation. We use an exact
  filter row per output phase when the reduced output rate allows it (it
  does for 44.1 <-> 48 kHz).
- **miniaudio** (public domain / MIT-0): the device model (a thread per
  device calling back for fixed-size periods), PulseAudio before ALSA on
  Linux, WASAPI shared mode with event-driven buffering and
  AUTOCONVERTPCM on Windows, and loading the system libraries at run time.
- The mixer's lock-free command and completion queues, per-sample gain and
  pan ramps, and generation-checked voice handles are the usual game audio
  engine design (as in miniaudio's engine and FMOD/Wwise-style APIs).

## Speed (a rough comparison)

`mbuild bench` makes two-minute stereo 44.1 kHz test files and times
audio's `audio_load` on each (release build, file already in memory, best
of five runs). It then runs `ffmpeg -benchmark -threads 1 -i FILE -f null -`
once per file and prints ffmpeg's wall time. The two do not measure the same
work: ffmpeg's time includes reading and probing the file, and its read,
decode and write steps run on separate threads that overlap; audio's time
includes growing and copying the output buffer. Treat the numbers as a
rough guide, not a like-for-like benchmark. On the development machine:

| Format | audio | ffmpeg |
|---|---|---|
| FLAC | 140 ms | 137 ms |
| MP3 192k | 205 ms | 154 ms |
| Vorbis q5 | 203 ms | 124 ms |

Opus was measured decode-only against libopus on the same packets (60 s
files): 96 kb/s stereo 0.17 s against 0.10 s, 6 channels 0.31 s against
0.18 s, speech (hybrid) 0.13 s against 0.12 s, 8 kHz mono 0.05 s against
0.03 s. libopus uses SIMD; audio does not yet.

FLAC encoding at level 5 took 0.27 s against ffmpeg's 0.16 s (measured the
same rough way), with files about 0.9% larger on this noise-heavy test
signal. Resampling 30 s of stereo from 44.1 to 48 kHz takes 37 ms at
quality 4 and 86 ms at quality 10.
