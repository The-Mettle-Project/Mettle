# audio API

Everything is in one import:

```mettle
import "audio/audio";
```

All names start with `audio_`, `Audio` or `AUDIO_`. Functions that can fail
return a status code: `AUDIO_OK` (0) or a negative `AUDIO_E_*` value.
`audio_error_text(rc)` gives a short message. Nothing crashes on bad input:
a damaged file gives an error (or, for a damaged frame in the middle of a
stream, that frame is skipped).

## Sound in memory: AudioPcm

```mettle
struct AudioPcm {
  data: uint8*;     // frames * channels samples, interleaved (L R L R ...)
  frames: int64;
  channels: int32;
  rate: int32;      // frames per second
  fmt: int32;       // AUDIO_U8, AUDIO_S16, AUDIO_S24, AUDIO_S32, AUDIO_F32, AUDIO_F64
  bits: int32;      // bits of real data per sample (16 for a 16-bit file)
  mask: uint32;     // speaker layout (WAVE_FORMAT_EXTENSIBLE bits), 0 = usual
}
```

| Function | What it does |
|---|---|
| `audio_pcm_alloc(p, frames, channels, rate, fmt)` | zeroed room for a sound |
| `audio_pcm_free(p)` | frees the samples |
| `audio_pcm_convert(src, fmt, dst)` | copy in another sample format |
| `audio_pcm_channels(src, channels, dst)` | float copy with another channel count (ITU-R BS.775 downmix) |
| `audio_pcm_resample(src, rate, quality, dst)` | float copy at another rate, same length in time, no delay |
| `audio_fmt_size(fmt)` | bytes per sample |

Channels are always in WAV order: FL FR FC LFE BL BR SL SR (as the speaker
mask bits say). Every decoder converts its format's own order to this.

## Loading and saving whole files

| Function | What it does |
|---|---|
| `audio_load_file(path, pcm)` | any supported format; WAV and FLAC keep their integer format, others give `AUDIO_F32` |
| `audio_load(data, size, pcm)` | the same from memory |
| `audio_load_for_mixer(path, rate, channels, pcm)` | float, at the mixer's rate and channel count |
| `audio_wav_decode(data, size, pcm)` / `audio_wav_encode(pcm, out)` | WAV |
| `audio_flac_decode(data, size, pcm, check_md5)` / `audio_flac_encode(pcm, level, out)` | FLAC; level 0 (fastest) to 8 (smallest), 5 is usual |
| `audio_probe(data, size)` | `AUDIO_FMT_WAV`, `_FLAC`, `_VORBIS`, `_OPUS`, `_MP3` or `_NONE` |
| `audio_file_read(path, buf)` / `audio_file_write(path, data, size)` | whole files |

Encoders write into an `AudioBuf` (`data`, `len`); free it with
`audio_buf_free`.

```mettle
var pcm: AudioPcm;
if (audio_load_file("voice.ogg", &pcm) != AUDIO_OK) { ... }
var out: AudioBuf;
var s16: AudioPcm;
audio_pcm_convert(&pcm, AUDIO_S16, &s16);
audio_flac_encode(&s16, 5, &out);
audio_file_write("voice.flac", out.data, out.len);
```

## Streaming: AudioStream

For long sounds (music), decode a block at a time instead of all at once.

| Function | What it does |
|---|---|
| `audio_stream_open_file(s, path)` / `audio_stream_open(s, data, size)` | finds the format, reads the headers into `s.info` |
| `audio_stream_read(s, out, frames)` | up to `frames` interleaved float frames; 0 at the end |
| `audio_stream_seek(s, frame)` | sample exact in every format |
| `audio_stream_tell(s)` | the current frame |
| `audio_stream_close(s)` | frees it |

`s.info` holds `channels`, `rate`, `frames` (-1 if the file does not say),
the stream's own `fmt` and `bits`, and the speaker `mask`.

Each format also has its own decoder with the same shape
(`audio_wav_*`, `audio_flac_*`, `audio_vorbis_*`, `audio_opus_*`,
`audio_mp3_*`: `open`, `next`, `seek`, `close`), if you want the blocks as
the decoder makes them.

## The mixer: AudioMixer

For games: many sounds at once, with no locks between the game thread and
the audio thread. Gain, pan and fades are ramped per sample, so they never
click.

```mettle
var m: AudioMixer;
audio_mixer_init(&m, 48000, 2, 64);                 // rate, channels, voices
var shot: AudioPcm;
audio_load_for_mixer("shot.wav", 48000, 1, &shot);  // mono, so it can be panned
var v: int64 = audio_mixer_play(&m, &shot, 0.8, -0.3, false);   // gain, pan, loop
audio_mixer_set_pitch(&m, v, 1.1);
```

| Function | What it does |
|---|---|
| `audio_mixer_play(m, pcm, gain, pan, loop)` | plays an `AUDIO_F32` sound of 1 or 2 channels at any rate; returns a handle or -1 |
| `audio_mixer_play_ex(m, pcm, gain, pan, pitch, loop, group, fade_ms)` | with pitch, a group (0-15) and a fade-in |
| `audio_mixer_play_stream(m, fill, user, gain, group, fade_ms)` | plays from a fill function on the audio thread |
| `audio_mixer_stop(m, v, fade_ms)` | stops, with a fade-out |
| `audio_mixer_pause` / `audio_mixer_resume` | |
| `audio_mixer_set_gain(m, v, gain, ms)` / `audio_mixer_set_pan(m, v, pan, ms)` | moves smoothly over `ms` |
| `audio_mixer_set_pitch(m, v, pitch)` / `audio_mixer_set_loop(m, v, on)` / `audio_mixer_seek(m, v, frame)` | |
| `audio_mixer_set_group_gain(m, group, gain, ms)` | music, effects, speech ... |
| `audio_mixer_set_master(m, gain, ms)` / `audio_mixer_set_limiter(m, on)` | soft limiting instead of hard clipping |
| `audio_mixer_playing(m, v)` / `audio_mixer_active(m)` | |
| `audio_mixer_mix(m, out, frames)` | the audio thread's call: fills interleaved floats |

A handle stays unique: one whose voice has ended does nothing, even after
its voice slot is used again. Sounds passed to `play` must stay alive while
they play.

Music through the mixer, decoded on the audio thread and converted to the
mixer's rate and channels:

```mettle
var music: AudioPlayer;
audio_player_open(&music, "theme.ogg", 48000, 2, true);       // looping
audio_mixer_play_stream(&m, &audio_player_fill, (rawptr)&music, 0.7, 1, 500.0);
```

## Devices: AudioDevice

| Function | What it does |
|---|---|
| `audio_device_open_mixer(d, m, period)` | the default output at the mixer's rate, playing the mixer |
| `audio_device_open(d, playback, name, rate, channels, period, cb, user)` | any device; `cb(user, buf, frames, channels)` runs on the device thread |
| `audio_device_start(d)` / `audio_device_stop(d)` / `audio_device_close(d)` | |
| `audio_device_status(d)` | `AUDIO_E_DEVICE` once the device failed (unplugged) |
| `audio_device_backend(d)` | `AUDIO_DEV_PULSE`, `AUDIO_DEV_ALSA` or `AUDIO_DEV_WASAPI` |
| `audio_device_latency_ms(d)` | about how much is buffered |

```mettle
var dev: AudioDevice;
audio_device_open_mixer(&dev, &m, 256);   // 256 frames: about 5 ms at 48 kHz
audio_device_start(&dev);
...
audio_device_close(&dev);
```

On Linux, PulseAudio (which PipeWire also serves) is tried first, then ALSA;
a name such as `"hw:0,0"` or `"alsa:NAME"` picks ALSA. Both libraries are
loaded when a device is opened, so a program without them still runs and
gets `AUDIO_E_DEVICE`. On Windows the device is WASAPI in shared mode.

For capture, open with `playback = false`; the callback receives each block
of recorded frames. `AudioRing` (`audio_ring_init`, `_write`, `_read`,
`_count`) moves them to another thread without locks.

## Lower level

- `AudioResampler`: `audio_resampler_init(r, channels, in_rate, out_rate, quality)`,
  `audio_resample(r, in, in_frames, &used, out, out_cap)`; quality 0-10.
- `audio_channel_matrix` and `audio_mix_channels`: channel mixing of float blocks.
- `audio_convert_samples(src, sfmt, dst, dfmt, n)`: any sample format to any other.
- Readers for format work: `AudioRd` (bytes), `AudioBits` (bits, most
  significant first), `AudioBitsL` (least significant first), `AudioOgg`
  (Ogg pages and packets), the CRCs and MD5.
