# Video player

A desktop video player written in Mettle. It reads AVI, MP4 and MOV files,
decodes Motion JPEG video and PCM audio, keeps the picture locked to the sound
card's own clock, and draws to a window through GDI.

Everything below the operating system is Mettle: the container parsers, the
baseline JPEG decoder, the inverse DCT, the chroma upsampler, the colour
conversion, and the playback clock. The only outside calls are Win32 ones for
the window, the blit, and the wave device.

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
in `moof` boxes rather than in a sample table. Video: Motion JPEG, at any
chroma sampling, plus greyscale. Audio: PCM.

H.264 and AAC are not decoded yet, so a typical MP4 from a camera or a
download needs one conversion first:

```bash
ffmpeg -i input.mp4 -c:v mjpeg -q:v 5 -pix_fmt yuvj420p -c:a pcm_s16le -ar 44100 -ac 2 clip.avi
```

`-q:v` runs from 2 (large files, near transparent) to 31 (small, blocky). 5 is
a good default.

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
- 1390 truncated and byte-corrupted files through the decoder and both
  demuxers under `--safe`: no crash, no read outside a buffer, no hang.
- Playback rate measured by screenshotting the window and matching against an
  ffmpeg frame index: 153 frames in 6.364 s at 24 fps on the audio clock,
  84 in 3.351 s at 25 fps on the wall clock.
- 1280x720 decode costs 6.7 ms a frame in a release build, so a 24 fps file
  uses about a sixth of one core.

## Limits

- Windows only. The window, the blit, and the audio device are Win32.
- H.264 and AAC are not decoded yet. A file carrying them opens, reports what
  it holds, and says what to convert.
- Progressive JPEG is rejected. Motion JPEG does not use it.
- Decoding runs on the UI thread. It is fast enough for 720p and 1080p on one
  core, and a 4K file would want a decode thread.
