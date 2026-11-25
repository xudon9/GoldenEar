# Golden Ear — Product requirements

## Goal

Golden Ear is a local Windows listening lab. Listeners use bundled or imported lossless tracks to find out which MP3 settings they can distinguish from the source under their current listening conditions.

## Session flow

1. Make bundled public-domain and CC0 recordings available by default and let listeners hide them in the repertoire. Allow listeners to import FLAC or WAV tracks and remove references without changing the source files. Save imported references and bundled-track visibility across launches.
2. Choose a test type, a difficulty mode and one or more MP3 profiles. Offer an optional Practice mode for any test type. Keep this configuration fixed until the session ends.
3. Shuffle tracks by default, avoiding immediate repeats when more than one track is available. Offer a setting to keep the current track for new trials and sessions. Randomize which side is lossless and, in ABX, independently randomize whether X is A or B.
4. Play, pause, seek, switch between versions or listen again before answering. Select listening sources and submit answers using distinct mouse controls or shortcuts. Let the listener skip an unanswered trial. Offer manual track selection in Repertoire throughout the session. A manual selection replaces an unanswered trial at the same quality or opens the next trial after an answer.
5. Reveal the correct match or the lossless side immediately after submission. Show the MP3 profile with the reveal. Let the listener review the answer before starting the next trial. By default, finishing starts a fresh session with the same settings and resets results. Allow the listener to turn off automatic restart and keep results on screen until the next session.

## Test types

| Type | Answer | Feedback and result |
| --- | --- | --- |
| ABX | X matches A or B | Reveal the match and the lossless side. Report accuracy against 50% chance. |
| Identify Hi-Fi | A or B is the lossless reference | Reveal the lossless side. Report accuracy against 50% chance. |
| Preference | A or B sounds better | Reveal the lossless side and report how often it was preferred. |

## Difficulty

| Preset | Sample rate | Constant bitrate |
| --- | ---: | ---: |
| 1 | 16 kHz | 32 kbps |
| 2 | 22.05 kHz | 56 kbps |
| 3 | 32 kHz | 96 kbps |
| 4 | 44.1 kHz | 160 kbps |
| 5 | 48 kHz | 320 kbps |

- **Random** draws uniformly and independently from enabled profiles. Skip draws a new trial. Choosing a different song before answering keeps the current profile.
- **Ladder** starts with the lowest enabled profile and advances after twelve scored answers at each level. The quota makes above-chance results attainable even with all five profiles enabled. Skip gives a fresh trial at the current level. Change song keeps the level and selects a different track. The highest enabled level stays active until the listener finishes. Preset order is an estimate of difficulty. Actual audibility depends on the source and listening conditions.

## Playback and controls

- Decode the first audio stream to a common stereo floating-point playback format. Encode MP3 using LAME at the selected sample rate and bitrate, then decode it into the same format. Use a seekable MP3 with a Xing/Info header so the decoder can compensate for encoder delay and end padding. Use the shared valid frame range, match whole-track RMS, and apply shared peak headroom.
- Keep a single playback clock and queue. Switch sources at the same frame using a short crossfade without restarting playback or changing the play/pause state. A, B and X share the same gain, playback position and transport controls. Shared Windows audio output may resample for the selected hardware. Results reflect this complete playback chain.
- Default controls are J/K/L to listen to A/B/X, semicolon to toggle A/B, Space for play/pause, comma/period to seek five seconds, and 1/2 to answer A/B. After an answer, the play/pause shortcut advances to the next trial and starts playback as soon as the audio is ready. Save personalized keys and reject duplicates. Give mouse access to the same actions and provide a clickable seek bar.
- Let listeners drag across the waveform or mark two playhead positions to loop a clip. Highlight the range and repeat it on the shared playback clock for every sample. Keep the loop for consecutive trials using the same track and provide a clear-loop control.
- Prepare audio off the UI thread. While a trial is available to play, encode the likely next trial’s MP3 to a temporary file. Reuse it when the track and profile match. Report missing files, unsupported inputs, unavailable FFmpeg and device failures with recovery guidance. Support tracks up to 15 minutes and keep one prepared pair in memory.

## Statistics

- Show scored answers, correct count and accuracy by MP3 profile. For ABX and Identify Hi-Fi, use a one-sided anytime-valid p-value for performance above 50% guessing. Compute the running maximum of a mixture of Bernoulli likelihood ratios for success rates 0.60, 0.70, 0.80, 0.90 and 0.99. Take its reciprocal, multiply by the number of enabled profiles for family-wise error control, and cap at 1. Show “Above chance” at adjusted p ≤ 0.05 and “Inconclusive” otherwise. This supports checking results continuously and stopping whenever the listener chooses. [Ramdas et al., Game-Theoretic Statistics and Safe Anytime-Valid Inference](https://arxiv.org/abs/2210.01948)
- Preference includes the share of answers favoring the lossless version. Preference and objective identification describe different outcomes. An inconclusive score leaves discrimination ability unresolved.
- Scores describe the current session, using its music and current listening setup. For inference, the null is that correctness is at most 50% on each scored trial given its profile and past results. Record skipped and replaced trials separately from submitted answers.

## Interface and delivery

- Build a native C++20 Windows desktop application at the repository root, with an offline audio path and FFmpeg with `libmp3lame` as a runtime dependency. Keep personal audio local. Native file selection supports multiple files.
- Use a listening console with a dominant A/B/X selector, clear playback and response states, and persistent session results. In a blind session, reveal codec assignments after submission. In Practice mode, label the lossless source and the MP3 profile on each sample before answering. Show practice answer counts by profile and use them for Ladder advancement. Make all trial actions available with the mouse.
- Group appearance, language, shortcuts, and session settings under Preferences. Follow the Windows app theme by default, provide light and dark overrides, and honor Windows contrast themes.
- Provide English, Simplified Chinese, and Spanish for the app interface. Default to the first supported Windows display language, with English as the fallback. Save manual language choices and apply them immediately.
- Verify both difficulty schedules, scoring and early stopping, real audio conversion and alignment, and a native build. Check the transition across A/B/X for position continuity and report any hardware playback limitations encountered during verification.
