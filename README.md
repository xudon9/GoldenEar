# Golden Ear

Golden Ear is a native Windows application for blind listening tests. Use the bundled lossless recordings or import FLAC or WAV tracks, choose ABX, Identify Hi-Fi or Preference, and compare the source with MP3, AAC, LDAC, aptX HD, or SBC. [Product requirements](PRD.md)

## Build and run

Use Windows C11 and C++20 compilers and CMake. Put `ffmpeg.exe` on `PATH` or set `GOLDEN_EAR_FFMPEG` to its full executable path. Use an FFmpeg build that includes `libmp3lame`, AAC, aptX HD, and SBC. Golden Ear builds the LDAC encoder and decoder from the included sources. [FFmpeg encoder documentation](https://ffmpeg.org/ffmpeg-codecs.html) · [LDAC sources and licenses](third_party/ldac/README.md)

From the repository root, configure and build with Visual Studio and the Desktop development with C++ workload.

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\Release\golden-ear.exe
```

With MinGW, configure using `cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release`. The executable is `build\golden-ear.exe`. If FFmpeg is on `PATH` at configure time, CTest also runs encode/decode and alignment checks for every preset.

The build copies the recordings and their credits to `tracks` and the LDAC license and notice files to `licenses` next to `golden-ear.exe`. Keep those folders with the executable when distributing it.

## Run a test

1. Start with the bundled recordings or add your own using **Import audio**. In **Repertoire**, clear **Show bundled tracks** to use only your imports. The app saves this choice, your imported paths, and your test settings in `%LOCALAPPDATA%\GoldenEar\settings.txt`. `GOLDEN_EAR_HOME` overrides that directory.
2. Choose a test type, select **Random** or **Ladder**, pick one **Compression codec**, and enable one or more of its bitrate presets. For Ladder, set **Trials per level** before starting. The default is 12. Ladder moves up after that many answers at each bitrate and finishes after the final answer at the highest enabled bitrate. Only submitted answers count toward the preset.
3. Start a session. Use the play icon below the waveform to begin and the A, B or X buttons to switch while listening. The curved arrows jump backward or forward five seconds. Submit an answer using the separate A/B answer buttons. **Choose song** opens Repertoire, and **Random song** picks a different track immediately. Before an answer, either action replaces the trial at the same quality. After an answer, it opens the next trial on the new track. **Skip** discards the unanswered trial.
4. **Back to setup** ends the current test and keeps its results on screen while you choose a new test type. **Finish and restart** starts a fresh session with the same settings and resets the results. To hide the restart action, turn off **Restart after finishing** in **Preferences → Session**. Objective modes show per-profile accuracy and an anytime-valid p-value adjusted across the enabled profiles. Preference mode shows how often you preferred the lossless version.

**Shuffle tracks** in **Preferences → Session** selects a different track for new trials when several are available. Turn it off to keep the current track until you choose another one.

Consecutive trials with the same track, codec, and bitrate reuse the decoded audio in memory. Golden Ear keeps up to 128 MiB of compressed audio for the life of the app, evicting the least recently used entries. Returning to a cached track and preset skips encoding. The next trial’s audio is fetched or encoded in the background while you listen. Use **Preferences → Session → Clear audio cache** to free the memory used by saved conversions.

For a guided comparison, turn on **Practice mode** before starting a session. Each sample shows **Lossless source** or its codec and preset, including X in ABX. Practice results show how many questions you answered at each quality.

## Bluetooth presets

Bluetooth presets encode and decode the track locally at 48 kHz. You can listen on any Windows audio output. The scenario labels illustrate where you might encounter each setting. Actual settings depend on the devices and connection, and the tests use a fixed preset throughout each trial.

| Codec | Bitrate | Example |
| --- | ---: | --- |
| AAC-LC | 256 kbps target | Phone and earbuds |
| AAC-LC | 320 kbps target | Higher-rate AAC pair |
| LDAC | 330 kbps | Crowded metro |
| LDAC | 660 kbps | Busy café |
| LDAC | 990 kbps | Quiet study room |
| aptX HD | 576 kbps | Hi-fi at home |
| SBC | 237 kbps | Older car stereo |
| SBC | 345 kbps | Home speaker |

Sony’s Best Effort mode can move among LDAC bitrates as the link changes. aptX HD uses a fixed 576 kbps at 48 kHz. SBC uses the Bluetooth A2DP medium- and high-quality joint-stereo configurations. AAC uses the FFmpeg AAC-LC encoder and will differ from specific devices’ encoders. [Sony LDAC](https://www.sony.co.jp/en/Products/LDAC/) · [Qualcomm aptX HD](https://www.aptx.com/aptx-hd) · [Bluetooth A2DP specification](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/a2dp_v1-4-1_1752513648/A2DP_v1.4.1/out/en/index-en.html)

## Repeat a clip

Drag across the waveform to select a passage. You can also place the playhead at each boundary and use **Set loop start** and **Set loop end**, while playing or paused. Golden Ear highlights the selected range and repeats it at the same positions for A, B, and X. Use **Clear loop** to resume playback through the whole track. The loop stays selected between trials on the same song and clears when the song changes.

## Keyboard controls

| Default key | Action |
| --- | --- |
| J / K / L | Listen to A / B / X |
| `;` | Toggle A/B |
| Space | Play or pause. After answering, start playing the next trial |
| `,` / `.` | Seek backward / forward five seconds |
| 1 / 2 | Submit A / B |

Use **Preferences → Shortcuts** to rebind the keys. Tab moves between controls, Enter activates the focused control, and Space toggles a focused checkbox. Use the arrow keys within a group of radio buttons. Tracks up to 15 minutes are supported. Playback uses shared Windows audio output, and the app shows its stream sample rate during each trial.

## Bundled recordings

- Bach’s *Goldberg Variations*, Aria and Variation 1, performed by Kimiko Ishizaka. Both recordings are CC0 and use 24-bit / 96 kHz FLAC.
- Beethoven’s *String Quartet No. 6*, first movement, performed by the Musopen String Quartet. The recording is public domain and uses 24-bit / 48 kHz FLAC.

See the [recording credits and licenses](assets/tracks/LICENSE.md) for sources.

## Preferences

**Appearance** follows the Windows app mode by default. Choose **Light** or **Dark** to override it. Windows contrast themes still apply automatically.

**Language** offers English, Simplified Chinese, and Spanish. **Automatic** is the default and uses the first supported language in your Windows display language preferences, with English as the fallback. Appearance, language, and shortcut changes apply immediately and are saved for future launches.
