# Formats

What each format reader and writer handles, the standard it follows, and
how it is checked. Test files are made by `tools/make_testdata.sh` with
ffmpeg and the reference encoders (libFLAC-compatible flac, libvorbis,
libopus, libmp3lame, shine); `mbuild testdata` remakes them, and also makes
ffmpeg's float decodes (`*.f32`, not kept in git) that the lossy decoders
are compared with.

Every decoder is fuzzed (`tests/fuzz_test.mettle`, and a loop in each
codec's own test) under the bounds-checked `safe` build, and every test
passes in debug, release and safe builds.

## WAV

Standards: Microsoft RIFF/WAVE (Multimedia Programming Interface and Data
Specifications 1.0), WAVE_FORMAT_EXTENSIBLE, EBU Tech 3306 (RF64), ITU-T
G.711 (A-law, mu-law), IMA ADPCM (IMA Digital Audio Focus and Technical
Working Groups, 1992), Microsoft ADPCM.

- Reads: PCM 8, 16, 24 and 32 bits (with any valid-bits count), IEEE float
  32 and 64, A-law, mu-law, IMA ADPCM, Microsoft ADPCM, inside RIFF, RF64 or
  BW64, plain or WAVE_FORMAT_EXTENSIBLE. A file cut short plays what is
  there. Up to 8 channels.
- Writes: PCM and float; WAVE_FORMAT_EXTENSIBLE for more than 2 channels,
  a speaker mask or fewer valid bits than the container; RF64 past 4 GB.
- Checked: every sample format against the 16-bit file; A-law, mu-law and
  Microsoft ADPCM bit exact against ffmpeg; IMA ADPCM within a few steps
  (the 1992 rule adds shifted steps where ffmpeg multiplies); write and read
  back of every format with 1 to 8 channels; streaming and seeking.

## FLAC

Standard: RFC 9639.

- Decodes: every block size, 1 to 8 channels, 4 to 32 bits per sample (a
  side channel of 32-bit audio needs 33 bits and is not read), all four
  channel codings, constant, verbatim, fixed (orders 0-4) and LPC (orders
  1-32) subframes, wasted bits, both Rice codings and escaped partitions,
  fixed and variable block size streams, the seek table. Each frame header
  is checked with its CRC-8 and each frame with its CRC-16; a bad frame is
  skipped and the decoder finds the next one. The MD5 of the output can be
  checked against STREAMINFO. Seeking is sample exact.
- Encodes: levels 0-8 with libFLAC's settings (block size, LPC order,
  partition order, stereo search, apodization windows), the streamable
  subset, STREAMINFO with MD5, a seek table (a point every 10 s) and a
  Vorbis comment block.
- Checked: decoding ffmpeg-made files at levels 0, 5, 8 and 12, 24-bit,
  mono, 6 channels and 8 kHz, by their MD5; seeking against a full decode;
  encoding at every level and many depths and channel counts, then decoding
  (MD5 checked); ffmpeg decodes our files to the same samples.

## Ogg

Standard: RFC 3533. Pages are checked with their CRC-32; a bad page is
skipped and the reader finds the next. Packets that span pages are joined;
a gap in page numbers drops the packet in progress. Only the first logical
stream of a file is read (chained files play their first part).

## Vorbis

Standard: Vorbis I specification (Xiph.Org).

- All of it: codebooks (lookup types 1 and 2), floor 0 and floor 1,
  residue 0, 1 and 2, channel coupling, both block sizes and the windows.
  1 to 8 channels, in WAV order.
- The first and last packets are trimmed by the granule positions, as the
  spec says. Seeking is sample exact: a bisection over page granules, then
  decoding forward.
- Checked against ffmpeg's decode of files from libvorbis (quality 0, 4,
  10, mono, 6 channels, 8 kHz, 48 kHz) and from ffmpeg's own encoder: same
  frame counts, about 136 dB signal to error.

## Opus

Standards: RFC 6716 (with the RFC 8251 updates), RFC 7845 (Ogg Opus).

- CELT, SILK and hybrid modes, every frame size (2.5 to 60 ms), frame
  packing codes 0-3, mode switches with redundant CELT frames, in-band FEC
  frames skipped, stereo; multistream for channel mapping families 0, 1, 2
  and 255 (up to 8 channels, converted from Vorbis to WAV order). All the
  RFC 8251 decoder updates. Output is always 48 kHz float.
- OpusHead pre-skip, output gain and end trimming by granule position are
  applied. Seeking is sample exact: a granule bisection, then 80 ms of
  pre-roll as RFC 7845 says.
- CELT is the float decoder of libopus; SILK is libopus's fixed-point
  decoder and its resampler to 48 kHz, so SILK output is bit exact.
- Checked against libopus's float decode (`*.lib.f32`) of files from
  libopus: CELT at 96 and 256 kb/s with 2.5 ms and 60 ms frames, 6-channel
  multistream, SILK narrowband and mediumband, hybrid, stereo SILK with FEC,
  and a file switching hybrid -> SILK -> CELT. Frame counts are exact; SILK
  is identical and the rest are 130 to 154 dB signal to error.
- Lost packets are concealed simply (shaped noise for CELT, silence for
  SILK) instead of libopus's pitch-based concealment, so files with lost
  packets differ from libopus there. Channel mapping family 3 (ambisonics
  with a demixing matrix) is not read.

## MP3 (MPEG audio)

Standards: ISO/IEC 11172-3 (MPEG-1 audio), ISO/IEC 13818-3 (MPEG-2 lower
sampling rates), and MPEG-2.5.

- Layer III with long, short and mixed blocks, M/S and intensity stereo,
  the bit reservoir; Layers I and II too. All MPEG-1, MPEG-2 and MPEG-2.5
  rates.
- ID3v2 tags at the start and ID3v1, APEv2 and Lyrics tags at the end are
  skipped. The Xing/Info and LAME tags give the exact length: the encoder
  delay (plus the 529-sample decoder delay) and the padding are removed, as
  ffmpeg does. Garbage between frames is skipped (a frame counts only when
  the next ones match it).
- Seeking is sample exact: from an index of frame positions, decoding a few
  frames early so the bit reservoir and filters are full.
- Checked against ffmpeg's decode of files from LAME (CBR 128 and 320,
  VBR, joint and plain stereo, MPEG-2 mono, MPEG-2.5 8 kHz, sharp clicks for
  short blocks), shine and ffmpeg's Layer II encoder: same frame counts,
  about 120 dB signal to error.
