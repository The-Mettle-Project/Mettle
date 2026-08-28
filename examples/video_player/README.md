# Video player

A desktop video player written in Mettle. It decodes Motion JPEG video and PCM
audio out of an AVI container, keeps the picture locked to the sound card's own
clock, and draws to a window through GDI.

Everything below the operating system is Mettle: the container parser, the
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

The player reads Motion JPEG in AVI. Convert any source video once with ffmpeg:

```bash
ffmpeg -i input.mp4 -c:v mjpeg -q:v 5 -pix_fmt yuvj420p -c:a pcm_s16le -ar 44100 -ac 2 clip.avi
```

```bat
examples\video_player\player.exe clip.avi
```

`-q:v` runs from 2 (large files, near transparent) to 31 (small, blocky). 5 is
a good default. 4:2:0, 4:2:2 and 4:4:4 chroma all decode, as does greyscale.

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
| [`jpeg.mettle`](jpeg.mettle) | baseline JPEG decoder, from markers to BGRA pixels |
| [`avi.mettle`](avi.mettle) | RIFF and AVI parsing, chunk index, frame and sample reads |
| [`audio.mettle`](audio.mettle) | waveOut ring buffer and the audio clock |
| [`vptool.mettle`](vptool.mettle) | command line tool used to test the three modules |

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
is a lookup in the chunk index and one decode, and a machine that falls behind
skips straight to the frame that is due rather than decoding the ones it
missed.

## Testing

`vptool` drives the modules without a window:

```bash
vptool.exe info  clip.avi              # streams, duration, index
vptool.exe frame clip.avi 40 out.ppm   # decode one frame
vptool.exe bench clip.avi              # decode every frame, hash the pixels
vptool.exe jpeg  photo.jpg out.ppm     # decode a still
vptool.exe audio clip.avi 5            # audio clock against the wall clock
```

What was measured on this decoder:

- Output compared against libjpeg on 4:2:0 stills: largest difference 2 of 255,
  mean 0.018. On 4:4:4 the output is identical.
- Compared against ffmpeg's own decode of the same frames, plane for plane:
  largest difference 3 of 255, from the two decoders rounding their inverse DCTs
  differently.
- 749 frames decoded under `--build`, `--release`, and `-s --release`. Every
  pixel hashes the same in all three.
- 598 truncated and byte-corrupted files through the decoder and the demuxer
  under `--safe`: no crash, no read outside a buffer, no hang.
- 1280x720 decode costs 6.7 ms a frame in a release build, so a 24 fps file
  uses about a sixth of one core.

## Limits

- Windows only. The window, the blit, and the audio device are Win32.
- Motion JPEG in AVI. There is no H.264 decoder here, which is why other
  formats go through ffmpeg first.
- Progressive JPEG is rejected. Motion JPEG does not use it.
- Audio must be PCM. A file whose audio is compressed plays as video alone.
- Decoding runs on the UI thread. It is fast enough for 720p and 1080p on one
  core, and a 4K file would want a decode thread.
