# MrSample

Chromatic single-sample player module for [Schwung](https://github.com/charlesvestal/schwung).

Mirrors Ableton Move's Sampler workflow: load one audio file, play it polyphonically
across the keyboard with pitch tracking, AHDSR amp envelope, multimode filter, and
filter LFO.

## Features

- **Formats**: WAV, MP3, FLAC, AIFF (case-insensitive extension)
- **Chromatic playback**: root note + transpose + fine tune, 8-voice polyphony (up to 16)
- **Amp envelope**: AHDSR, AHD one-shot, or Loop
- **Filter**: LP / BP / HP state-variable, env-amount, LFO modulation
- **Loop points** with crossfade, auto-populated from WAV `smpl` chunk when present
- **Sample start** knob with live `wav_position` preview

## Build

Requires Docker (for cross-compilation to ARM64).

```
./scripts/build.sh
```

Produces `dist/mrsample/` and `dist/mrsample-module.tar.gz`.

## Install on Move

After building:

```
./scripts/install.sh
```

The module is installed to `/data/UserData/schwung/modules/sound_generators/mrsample/`.

Restart Schwung to load it.

## License

MIT. Bundled `dr_mp3.h` and `dr_flac.h` by David Reid are public-domain / MIT-0
(see headers in `src/dsp/`).
