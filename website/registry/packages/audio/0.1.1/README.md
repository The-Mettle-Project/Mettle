# audio

Sound for Mettle, written in pure Mettle: decoders, encoders, resampling,
a low-latency game mixer, and playback and capture devices.

- **Decode:** WAV (PCM, float, A-law, mu-law, IMA and Microsoft ADPCM, RF64),
  FLAC, Ogg Vorbis, Ogg Opus, MP3 (and MPEG Layers I and II)
- **Encode:** WAV, FLAC (levels 0-8, as libFLAC)
- **Process:** sample format conversion, channel mixing (ITU-R BS.775
  downmix), a polyphase windowed-sinc resampler (speexdsp quality levels)
- **Mix:** many voices with gain, pan, pitch, fades, loops, groups and a
  master gain; lock-free between the game thread and the audio thread
- **Devices:** PulseAudio and PipeWire, then ALSA, on Linux; WASAPI on
  Windows; playback and capture

Every decoder seeks sample exactly, never crashes on bad input (all are
fuzzed under bounds checks), and is compared with the reference decoders.
See [docs/formats.md](docs/formats.md).

## Quick start

```mettle
import "audio/audio";

fn main() -> int32 {
  var m: AudioMixer;
  audio_mixer_init(&m, 48000, 2, 64);
  var dev: AudioDevice;
  if (audio_device_open_mixer(&dev, &m, 256) != AUDIO_OK) { return 1; }
  audio_device_start(&dev);

  var shot: AudioPcm;
  audio_load_for_mixer("shot.wav", 48000, 1, &shot);
  audio_mixer_play(&m, &shot, 1.0, 0.0, false);    // gain, pan, loop

  var music: AudioPlayer;
  audio_player_open(&music, "theme.ogg", 48000, 2, true);
  audio_mixer_play_stream(&m, &audio_player_fill, (rawptr)&music, 0.6, 1, 1000.0);
  // ... the game runs ...
  audio_device_close(&dev);
  return 0;
}
```

Loading and saving:

```mettle
var pcm: AudioPcm;
audio_load_file("in.mp3", &pcm);         // any format, found from the data
var out: AudioBuf;
audio_wav_encode(&pcm, &out);
audio_file_write("out.wav", out.data, out.len);
```

The whole API is in [docs/api.md](docs/api.md).

## Build and test

With mbuild:

```bash
mbuild build
mbuild test              # debug
mbuild test -r           # release
mbuild -p safe test      # every memory access bounds checked
mbuild fuzz 300          # long fuzz run under bounds checks
mbuild bench             # speed against ffmpeg (needs ffmpeg)
mbuild testdata          # remake tests/data (needs ffmpeg)
```

Examples: `examples/convert` (`convert IN OUT [LEVEL]`: any format to WAV
or FLAC), `examples/info` (what is in sound files) and `examples/play`
(streams a file of any format to the speakers, or plays a beep).

On Linux the program links libc and loads `libpulse-simple.so.0` or
`libasound.so.2` when a device is opened, so it runs (without sound) where
they are missing. On Windows it links ole32.

## Layout

```
src/audio.mettle     the one import
src/core/            status codes, AudioPcm, buffers, bit readers and writer, CRCs, MD5, files, math
src/dsp/             sample conversion, channel mixing, resampling
src/codec/           wav, flac, flac_enc, ogg, vorbis*, opus*, mp3*, stream (format detection, AudioStream, AudioPlayer)
src/mix/             the mixer and the lock-free ring buffer
src/dev/             devices: the front end, Linux (PulseAudio, ALSA) and Windows (WASAPI) back ends
tests/               one test program per part, the fuzz test, and the test files
bench/, examples/, tools/
```

## Status and limits

- The Windows back end compiles but has not been run on Windows yet.
- PipeWire is used through its PulseAudio service (present on PipeWire
  desktops); there is no native PipeWire back end.
- Ogg files with more than one stream (chained files) play their first
  stream. Ogg FLAC is not read.
- FLAC with 32-bit samples and stereo side coding (33-bit side channel) is
  not read.
- Opus: lost packets are concealed simply rather than as libopus does, and
  channel mapping family 3 is not read.

## License

Apache-2.0. See [LICENSE](LICENSE).
