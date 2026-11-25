# Golden Ear

Golden Ear is a native Windows application for blind listening tests. Use the bundled lossless recordings or import FLAC or WAV tracks, choose ABX, Identify Hi-Fi or Preference, and compare each lossless track with an MP3 encoded on demand. [Product requirements](PRD.md)

## Build and run

Use a Windows C++20 compiler and CMake. FFmpeg must include `libmp3lame`. Put `ffmpeg.exe` on `PATH` or set `GOLDEN_EAR_FFMPEG` to its full executable path. [FFmpeg encoder documentation](https://ffmpeg.org/ffmpeg-codecs.html#libmp3lame)

From the repository root, configure and build with Visual Studio and the Desktop development with C++ workload.

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\Release\golden-ear.exe
```

With MinGW, configure using `cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release`. The executable is `build\golden-ear.exe`. If FFmpeg is on `PATH` at configure time, CTest also runs the real encode/decode and alignment checks for all five profiles.

The build copies the recordings and their credits into a `tracks` folder next to `golden-ear.exe`. Keep the folder with the executable when moving or distributing it.

## Run a test

1. Start with the bundled recordings or add your own using **Import audio**. In **Repertoire**, choose **Hide bundled tracks** to use only your imports. The app saves this choice, your imported paths, and your test settings in `%LOCALAPPDATA%\GoldenEar\settings.txt`. `GOLDEN_EAR_HOME` overrides that directory.
2. Choose a test type, select **Random** or **Ladder**, and enable any combination of MP3 profiles. Ladder advances after every twelve scored answers until it reaches the highest enabled level.
3. Start a session. Use **Play** to begin and the A, B or X buttons to switch while listening. Submit an answer using the separate A/B answer buttons. **Change song** keeps the profile and **Skip** discards the trial. Neither counts as an answer.
4. Finish whenever you like. Results stay visible until you start another session. Objective modes show per-profile accuracy and an anytime-valid p-value adjusted across the enabled profiles. Preference mode shows how often you preferred the lossless version.

## Repeat a clip

Drag across the waveform to select a passage. You can also place the playhead at each boundary and use **Set loop start** and **Set loop end**, while playing or paused. Golden Ear highlights the selected range and repeats it at the same positions for A, B, and X. Use **Clear loop** to resume playback through the whole track. The loop stays selected between trials on the same song and clears when the song changes.

## Keyboard controls

| Default key | Action |
| --- | --- |
| J / K / L | Listen to A / B / X |
| `;` | Toggle A/B |
| Space | Play or pause |
| `,` / `.` | Seek backward / forward five seconds |
| 1 / 2 | Submit A / B |

Use **Preferences → Shortcuts** to rebind the keys. Tab moves between controls and Enter activates the focused button. Tracks up to 15 minutes are supported. Playback uses shared Windows audio output, and the app shows its stream sample rate during each trial.

## Bundled recordings

- Bach’s *Goldberg Variations*, Aria and Variation 1, performed by Kimiko Ishizaka. Both recordings are CC0 and use 24-bit / 96 kHz FLAC.
- Beethoven’s *String Quartet No. 6*, first movement, performed by the Musopen String Quartet. The recording is public domain and uses 24-bit / 48 kHz FLAC.

See the [recording credits and licenses](assets/tracks/LICENSE.md) for sources.

## Preferences

**Appearance** follows the Windows app mode by default. Choose **Light** or **Dark** to override it. Windows contrast themes still apply automatically.

**Language** offers English, Simplified Chinese, and Spanish. **Automatic** is the default and uses the first supported language in your Windows display language preferences, with English as the fallback. Appearance, language, and shortcut changes apply immediately and are saved for future launches.
