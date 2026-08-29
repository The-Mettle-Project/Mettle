# Video player

A desktop video player written in Mettle. It reads AVI, MP4 and MOV files,
decodes H.264 and Motion JPEG video with AAC and PCM audio, keeps the picture
locked to the sound card's own clock, and draws to a window through GDI.

Everything below the operating system is Mettle: the container parsers, the
H.264 decoder, the AAC decoder, the baseline JPEG decoder, the transforms, the
colour conversion, and the playback clock. The only outside calls are Win32
ones for the window, the blit, and the wave device, plus the NVIDIA driver when
the GPU is available to convert colour.

## Building

```bat
examples\video_player\build.bat
```

That produces `player.exe` and `vptool.exe`. To build by hand:

```bat
bin\mettle.exe --build --release --linker internal examples\video_player\player.mettle -o examples\video_player\player.exe
```

## Playing something

```bat
examples\video_player\player.exe clip.mov
```

Containers: AVI, MP4 and MOV, including fragmented MP4 where the samples live
in `moof` boxes rather than in a sample table. Video: H.264 Main and High
profile with CABAC, and Motion JPEG at any chroma sampling plus greyscale.
Audio: AAC LC, and PCM.

An ordinary MP4 from a camera or a download plays directly.

## Setting it as the default player

```powershell
examples\video_player\install-player.ps1
```

That registers the player for `.mp4`, `.mov`, `.m4v` and `.avi` under HKCU, with
no administrator rights. Windows does not let a program make itself the
default, so pick it once from Open with, or from Settings, Apps, Default apps.
`install-player.ps1 -Unregister` removes every key it wrote.

## Controls

| Key | Action |
|-----|--------|
| space, K | play and pause |
| left, right | seek 5 seconds |
| J, L | seek 30 seconds |
| `,` `.` | step one frame, which also pauses |
| home, end | jump to the start or the end |
| up, down, wheel | volume |
| M | mute |
| F, F11, double click | fullscreen |
| click the bar | seek, and drag to scrub |
| esc, Q | leave fullscreen, then quit |

The overlay appears when the mouse moves and hides after two and a half
seconds. It stays up while paused.

## How it fits together

| File | What it holds |
|------|---------------|
| [`player.mettle`](player.mettle) | window, message loop, clock, drawing, overlay, input |
| [`source.mettle`](source.mettle) | one media interface over both containers |
| [`avi.mettle`](avi.mettle) | RIFF and AVI parsing, chunk index |
| [`mp4.mettle`](mp4.mettle) | ISO base media parsing, sample tables and fragments |
| [`jpeg.mettle`](jpeg.mettle) | baseline JPEG decoder, from markers to BGRA pixels |
| [`audio.mettle`](audio.mettle) | waveOut ring buffer and the audio clock |
| [`h264.mettle`](h264.mettle) | H.264 bitstream: NAL units, SPS, PPS, slice headers |
| [`h264_decode.mettle`](h264_decode.mettle) | CABAC engine, macroblock layer, residuals, transforms |
| [`h264_predict.mettle`](h264_predict.mettle) | intra prediction, 4x4 and 16x16 and chroma |
| [`h264_inter.mettle`](h264_inter.mettle) | motion compensation, six tap luma, bilinear chroma |
| [`h264_pslice.mettle`](h264_pslice.mettle) | P slice syntax, weighting, inter reconstruction |
| [`h264_bslice.mettle`](h264_bslice.mettle) | B slice syntax, spatial and temporal direct modes |
| [`h264_deblock.mettle`](h264_deblock.mettle) | deblocking filter, boundary strength and edges |
| [`h264_frame.mettle`](h264_frame.mettle) | picture order, reference lists, DPB, frame assembly |
| [`aac.mettle`](aac.mettle) | AudioSpecificConfig and the bit reader |
| [`aac_decode.mettle`](aac_decode.mettle) | AAC LC: Huffman, scalefactors, TNS, IMDCT, windows |
| [`aac_tables.mettle`](aac_tables.mettle) | generated Huffman codebooks and scalefactor bands |
| [`h264_gpu.mettle`](h264_gpu.mettle) | optional CUDA colour conversion, loaded at run time |
| [`video_kernels.mettle`](video_kernels.mettle) | the GPU kernel, compiled to PTX |
| [`vptool.mettle`](vptool.mettle) | command line tool used to test the other modules |

### The containers

AVI is indexed by walking the `movi` list and recording every chunk, so a file
with a damaged or missing `idx1` still plays.

MP4 and MOV are read from `moov`: `stts`, `ctts`, `stsc`, `stsz`, `stco` and
`co64` give decode times, composition times, offsets and sizes, `stss` gives
the sync samples, and `elst` gives the presentation shift. When the file is
fragmented, the same table is built instead from every `moof`, reading `tfhd`,
`tfdt` and `trun` with the defaults from `trex`. Samples are sorted into
display order, so a stream with B-frames presents in the right sequence.

MOV files often store PCM one audio frame per sample, which would mean a
four-byte read per frame. Adjacent samples are merged into buffers of about
eight kilobytes when the index is built.

### The decoder

`jpeg.mettle` handles baseline sequential JPEG: DQT, DHT, SOF0, SOF1, DRI, SOS,
restart markers, and any sampling factor. A stream that omits its Huffman
tables, which some Motion JPEG writers do, falls back to the tables in Annex K
of the standard. The inverse DCT is the integer row and column algorithm with
13 bit constants, so the output tracks libjpeg to within a rounding step.
Chroma is upsampled with the triangle filter, which is what libjpeg calls fancy
upsampling.

The decoder holds its buffers across frames. Decoding frame 4952 of a file
allocates nothing that frame 1 did not already allocate.

### Speed

720p decode was taken from 9.3 ms a frame to 4.8 ms, and the whole path from
compressed bytes to BGRA pixels from 11.0 ms a frame to 5.3 ms, measured over
300 frames of a 1280x720 file. Every step was checked by hashing all three
decoded planes against the decoder as it stood before, on four different files:
the output is identical, not merely close.

Measuring was most of the work, and twice it said something other than what the
code looked like it was doing. A per-macroblock timer put 45 per cent of decode
in a branch that did nothing. A call-count profiler put 60 per cent in a blend
whose real cost was a tenth of that, because disabling inlining had turned a
one-line clamp into a million function calls a frame. What held up was
switching a whole stage off in an ordinary optimized build and taking the
difference, and timing one loop on its own.

What that pointed at, in the order it mattered:

- **Prediction was two thirds of decode**, and most of it was bulk byte
  movement done a byte at a time. Copying a reference block, averaging two
  predictions, and the quarter-sample chroma filter now work through 64-bit
  words: eight pixels per operation, no SIMD instruction set required. The
  rounded average of two byte planes is
  `(a | b) - (((a ^ b) & 0xFEFEFEFEFEFEFEFE) >> 1)`, which is exact, not an
  approximation. On its own that is 4.6x on the blend and 5.8x on the copy.
- **Weighted prediction** ran every pixel through an `int32` buffer: motion
  compensation wrote bytes, a pass widened them, a pass weighted them, and a
  pass narrowed them back. It is one pass now, and the weight itself is a
  256-entry table built when the weight changes rather than a multiply and two
  compares per pixel. This stream uses weighted prediction on nearly every
  macroblock, which is why it was worth finding.
- **Deblocking** was 30 per cent of decode and is now 11. Boundary strength is
  derived once per macroblock edge instead of once per four-sample segment, the
  chroma pass reuses the luma answer, and an inter macroblock with no
  coefficients and one motion vector cannot have a nonzero strength on any
  interior edge, so those edges are skipped without being examined.
- **Motion compensation** worked in `int32` through a padded copy of the
  reference block and chose between the sixteen half-sample cases inside the
  pixel loop. It reads the reference plane in place now, works in bytes, and
  picks the case once per block. When the macroblock has no residual the
  prediction is written straight into the picture.
- **The residual path** cleared 664 integers per macroblock whether or not a
  block was coded, and each block cleared its own coefficients again. Only
  coded blocks are cleared now.
- **The macroblock record** shrank from 680 bytes to 424 by holding motion
  vectors and differences as `int16`. Three thousand six hundred of them are
  written and read back per frame.
- **Temporal direct mode** did a division and a reference-list search per 8x8
  block. Both depend only on the slice, so both are a table now.
- **CABAC** reads through a 64-bit cache instead of a bit at a time, keeps the
  context state and its most probable bit in one byte, and renormalises with a
  table lookup. On this file that was worth almost nothing, because the stream
  spends about seven thousand bins a frame; on a high-bitrate stream it is the
  part that matters.

Decoding stays linear in macroblocks as the picture grows: 0.58 microseconds
per macroblock at 1280x720, 0.51 at 1920x1080, and 0.51 at 3840x2160. A 4K
frame costs 16.4 ms and a 1080p frame 4.1 ms, decode only.

What is left is the part that needs real vector instructions. Prediction now
runs at about 1.4 cycles per pixel operation, which is close to what scalar
code can do; ffmpeg decodes the same file in 1.15 ms a frame using hand-written
AVX2. The compiler's own auto-vectorizer does fire on these loops when they are
written as flat loops over row pointers, but its byte kernel carries about 90 ns
of setup, so on a 16-pixel row it loses to the scalar code it replaces. The
64-bit word arithmetic above is what was available in the meantime.

### Seeking

A seek has to decode from the last keyframe forward, which in this file is up
to 158 frames. Two things made that cheaper than it looks. Frames whose
`nal_ref_idc` is zero are never referenced by anything, so when one lands
before the frame being seeked to it is skipped without being decoded at all;
in this file that is 45 per cent of the frames in a group. And the frames
passed over on the way are no longer converted to BGRA, only the one being
seeked to and the few after it that the ring will want.

Measured over 20 seeks spread across the file: 332 ms each on average and
1121 ms at worst before, 198 ms and 531 ms after. `vptool h264seek` is that
measurement, and its second argument turns the skipping off so the two can be
compared.

Skipping is only done when the stream numbers its pictures with
`pic_order_cnt_type` 0, where the order count of a non-reference picture feeds
nothing that comes after it. Under the other two types it decodes everything.

### Colour on the GPU

The conversion from YUV to BGRA is the one part of playback that is
embarrassingly parallel, so it is offered to the GPU. `video_kernels.mettle` is
an ordinary Mettle kernel compiled to PTX:

```bat
mettle -O --emit-ptx video_kernels.mettle -o video_kernels.ptx
```

`h264_gpu.mettle` loads `nvcuda.dll` with `LoadLibrary` at run time and calls
the driver through function pointers, so the player has no link-time
dependency on CUDA and runs unchanged on a machine with no NVIDIA card. When
the driver, the card and `video_kernels.ptx` are all present the conversion
moves to the GPU; when any of them is missing it stays on the CPU and says so.

The frame ring is allocated as page-locked memory when the GPU is in use, which
roughly doubles the speed of reading the finished picture back. The conversion
is issued asynchronously and waited on only when the player reaches for that
frame, so it overlaps the decode of the frames after it.

Measured over 300 frames at 1280x720: 1.63 ms a frame on the CPU, 0.52 ms on an
RTX 5060 Ti, and the two outputs agree on every one of the 921,600 pixels in
every frame. `vptool h264gpucheck` is that comparison.

### The clock

When the file carries audio, the sound card is the master clock. Video frames
are chosen from `waveOutGetPosition`, so the picture follows what you are
hearing and cannot drift away from it. A file with no audio track runs on
`QueryPerformanceCounter` instead.

Motion JPEG has no inter-frame prediction, so every frame stands alone. Seeking
is a lookup in the sample index and one decode, and a machine that falls behind
skips straight to the frame that is due.

## Testing

`vptool` drives the modules without a window:

```bash
vptool.exe info  clip.mov              # container, codecs, duration, index
vptool.exe frame clip.mov 40 out.ppm   # decode one frame
vptool.exe bench clip.mov              # decode every frame, hash the pixels
vptool.exe jpeg  photo.jpg out.ppm     # decode a still
vptool.exe audio clip.mov 5            # audio clock against the wall clock
vptool.exe mp4info clip.mp4            # tracks, formats, sample counts
vptool.exe mp4samples clip.mp4 0 100   # offset, size, dts, pts, sync per sample
vptool.exe h264info clip.mp4 400       # H.264 parameter sets and slice headers
vptool.exe h264bench clip.mp4 300      # decode timing, and where the colour goes
vptool.exe h264gpucheck clip.mp4 40    # GPU colour against CPU colour, pixel by pixel
```

What was measured:

- Decoder output compared against libjpeg on 4:2:0 stills: largest difference 2
  of 255, mean 0.018. On 4:4:4 the output is identical. Against ffmpeg's own
  decode of the same frames, plane for plane: largest difference 3 of 255, from
  the two decoders rounding their inverse DCTs differently.
- The MP4 sample index compared against `ffprobe -show_packets` on a plain file
  and on a fragmented one: 5071 samples, every offset, size, presentation time
  and keyframe flag identical.
- Frames decoded under `--build`, `--release` and `-s --release`, with every
  pixel hashed: the same in all three.
- The H.264 slice headers read out of the reference clip compared against
  `ffprobe -show_frames`: 400 of 400 slice types identical, which also confirms
  the parameter sets, since a wrong field length would misalign every header
  after it.
- 1918 truncated and byte-corrupted files through the decoder, both demuxers
  and the H.264 header parser under `--safe`: no crash, no read outside a
  buffer, no hang.
- Playback rate measured by screenshotting the window and matching against an
  ffmpeg frame index: 153 frames in 6.364 s at 24 fps on the audio clock,
  84 in 3.351 s at 25 fps on the wall clock.
- 1280x720 decode costs 4.8 ms a frame in a release build, and 5.3 ms including
  colour conversion on the GPU, so a 24 fps file uses about an eighth of one
  core.
- The optimized decoder against the one before it, over 300 frames with all
  three planes hashed: identical output, 1.92x faster on decode and 2.09x over
  the whole path. The same check on a second H.264 file, on three re-encoded
  clips at 320x180, 640x360 and 1280x720, and on the Motion JPEG path:
  identical in every case.
- Decode against ffmpeg on one thread, same files: 1.9 ms a frame against
  0.95 at 720p, 4.2 against 1.5 at 1080p, 16.4 against 5.2 at 4K. The gap is
  hand-written AVX2.
- The GPU colour path against the CPU one, 40 frames of 921,600 pixels: no
  pixel differs.

## Limits

- Windows only. The window, the blit, and the audio device are Win32.
- H.264 is Main and High profile with CABAC, 4:2:0 only. CAVLC streams, other
  chroma formats, and interlaced content are refused by name rather than
  decoded wrongly.
- Progressive JPEG is rejected. Motion JPEG does not use it.
- Decoding runs on the UI thread. It is fast enough for 720p and 1080p on one
  core, and a 4K file would want a decode thread.
- The GPU is used for colour conversion only. Entropy decoding and macroblock
  reconstruction are serial by construction and stay on the CPU.
