# Slow Sippin — plugin (AU · VST3 · standalone app)

An extreme time-stretch instrument for textures, pads and drones. It makes a short sample last for minutes without changing its pitch.

It's a native C++/JUCE plugin for macOS 12 or later (including Sonoma 14.6). It's a universal build for Apple Silicon and Intel Macs, and comes in three forms:

| Form | Use it in | Installed to |
|---|---|---|
| **Audio Unit** | Logic Pro, GarageBand, MainStage, Ableton Live, … | `/Library/Audio/Plug-Ins/Components` |
| **VST3** | Ableton Live, Reaper, Bitwig, Cubase, Studio One, FL Studio, … | `/Library/Audio/Plug-Ins/VST3` |
| **Standalone app** | on its own, with no DAW | `/Applications/Slow Sippin.app` |

## Install

1. Download **`Slow-Sippin-1.0.0-macOS.pkg`** from the repository's [Releases](../../releases/tag/plugin-latest) (the "plugin-latest" pre-release).
2. The first time, **right-click the .pkg → Open → Open**. The installer isn't notarized by Apple, so a plain double-click is refused.
3. Click through the installer. It installs all three forms.
4. In your DAW, rescan plugins if it doesn't pick it up automatically. Slow Sippin is an **instrument** (synth), so put it on an instrument or MIDI track.

The installer doesn't mark the files as downloaded, so there's no "damaged" warning. If you install from the manual zip instead, run this once:
`xattr -cr "/Library/Audio/Plug-Ins/Components/Slow Sippin.component" "/Library/Audio/Plug-Ins/VST3/Slow Sippin.vst3" "/Applications/Slow Sippin.app"`

## Playing it

There's no Render step: the stretch is computed live, so every control can be changed while it plays.

- **Load a sample:** drag it onto the window, or click **+ SAMPLE** (WAV, AIFF, MP3, FLAC; also OGG, M4A and CAF on macOS).
- **Region:** drag on the source waveform to select part of the sound. Click once to set a start point; double-click to clear the selection.
- **AUDITION** plays the original sample or region.
- **PLAY / STOP** (or Space) plays the stretched sound. **MIDI notes play it too**: polyphonic with 8 voices, transposed relative to C3 when KEY TRACK is on, with velocity, sustain pedal, and pitch bend of ±2 semitones.
- **The output panel** draws the stretched sound as you hear it. Click anywhere on it to set the start point, which jumps there if playing. The thin lines on the source waveform show where each voice is reading.
- **RENDER & SAVE** renders the whole one-shot (region × stretch, plus reverb tail) in the background, with a progress bar and **CANCEL**. It saves WAV or AIFF at 16-bit (dithered), 24-bit or 32-bit float.
  - Renders can be any length up to **12 hours**. They stream to disk, so an hour-long file needs very little memory, but it does need free disk space: about 2.5 GB for an hour of 24-bit stereo at 48 kHz, freed again once the file is saved.
  - WAV files over 4 GB are written as RF64. AIFF can't exceed 4 GB, so use WAV for very long 32-bit renders.

### Controls

| | |
|---|---|
| **Stretch** | How much longer the sound becomes. The slider goes from 1× to 1000×; the number box accepts up to 1,000,000×. |
| **Pitch** | Transposition in semitones, independent of stretch. The slider covers ±48; the box accepts ±60. |
| **Weave** | Grains overlapping at each moment (1–16). 1 is broken and pulsing; higher values are smoother and glassier. |
| **Scatter** | 0 % keeps the real phase: a coherent stretch where attacks keep their shape. 100 % invents the phase for the full texture. |
| **Drift** | Each grain reads from a wandering position, up to ± this many ms, so the texture breathes. |
| **Reverb / Size** | Internal reverb: how much reverb is added on top of the sound, and its decay time (RT60). The dry sound stays at full level. |
| **Gain** | Output level. |
| **Attack / Release** | Fade-in and fade-out of each note. STOP always fades quickly. |
| **Normalize** | Brings the level up to a safe maximum: it measures the stretched sound's peak in the background, and a limiter catches chords. |
| **Mono → stereo** | Builds wide stereo from a mono sample using two independent phase passes. |
| **Reverse** | Plays the stretched sound backwards. |
| **Loop** | Off: a one-shot lasting region × stretch. On: the region loops seamlessly, for endless drones. |
| **Key track** | MIDI notes transpose the sound. Off: every key plays at the Pitch setting. |
| **Window** | Moves the selected region across the sample (±100 % = its full travel). Drop an LFO on the waveform to scrub. |

### Effects (EFFECTS tab)

The right-hand panel has two tabs. **EFFECTS** has two effect slots, **SLOT A** and **SLOT B**. Pick any two of:

| Effect | Controls |
|---|---|
| **Chorus** | Rate, Depth: three slowly detuned voices |
| **Dub Echo** | Time, Feedback (above 100 % it self-oscillates, safely saturated), Tone. It runs through a tape-style loop: darkening tone filter, saturation, gentle wow, and pitch glide when the time changes |
| **Filter** | Cutoff, Resonance, LP > BP > HP (morphs from low-pass through band-pass to high-pass) |
| **Spectral** | Blur (smears the spectrum over time), Shift (moves partials ±24 semitones), Thin (keeps only the strongest partials) |

Picking the effect that's already in the other slot swaps the two. Only the controls of the two chosen effects are shown.

- **ROUTING:** **SERIES** feeds A into B (e.g. filter → dub echo). **PARALLEL** runs them side by side, mixed by **A / B BALANCE**.
- **FX MIX** is the dry/wet for the effects. At 0 they're bypassed, and the sound is bit-identical to dry.
- **The XY pad plays the two parameters you choose.** Use the **X →** and **Y →** menus under the pad to pick any parameter of the two chosen effects, or A / B Balance in parallel mode. The default is X = Filter Cutoff, Y = Echo Feedback.
  - Moving the puck sweeps each parameter across its full range. The pad's edges show what each axis plays and its value right now.
  - A parameter played by the pad gets a red **X** or **Y** badge, and its own slider dims. The box shows what the pad is playing. Set the axis to OFF to hand the parameter back to its slider.
  - Double-click the pad to centre it.
- Drop an LFO on the pad (choose X, Y or both) to move it automatically. You can also route LFOs to the effect parameters themselves.
- Effects come after the stretch and before the reverb.

### Modulation: 8 LFOs

The **MODULATION** strip along the bottom holds 8 LFOs. Each has a shape (click the name: sine, triangle, saw up, saw down, square, S&H, wander) and a rate (0.01–20 Hz).

- **Drag an LFO's coloured header** onto any control to route it there. It can also go on the **XY pad** (choose X, Y or both) or the **source waveform** (moves the window).
- Each route shows as a **chip** on its target.
  - **Drag the chip up/down** to set the depth (Shift for fine control).
  - **Double-click** it to remove it; **right-click** to invert or remove.
- A control being modulated shows a ring travelling along its slider, so you can see where the LFO is pushing it.
- One LFO can drive many controls, and one control can take several LFOs.
- Depth is relative to the control's full range: 100 % sweeps the whole range.

**Scrubbing with the WINDOW.** The WINDOW control (ENGINE tab) moves your selected region across the sample: ±100 % is its full travel, bouncing off both ends. Drop an LFO on the source waveform and the region window travels with it while playing, which scrubs the sample under the LFO's direction. The red region on the waveform moves live, and dashed lines mark where you drew it. WINDOW needs a selected region to move; it does nothing when the whole sample is selected.

**Render & Save** includes the effects and the LFO movement, so the file sounds like what you hear. The LFOs start from zero at the beginning of the file, and the file runs on until echo and reverb tails have died away.

The routes, the LFO settings and the open tab are saved with your project.

Every control is independent: changing one never moves another; only an LFO you routed moves anything. Sliders ignore the scroll wheel and trackpad, so scrolling can't change a value by accident. Normalize is the one deliberate interaction: it re-measures the level when the engine controls change, keeping the output at the same safe maximum.

Everything is automatable from the DAW. The project remembers the sample (by file path), the region, the start point and the export settings.

## Building from source

```sh
cmake -S plugin -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build --config Release -j
./build/EngineTests
```

JUCE 8.0.15 is fetched automatically. Pass `-DSS_JUCE_DIR=/path/to/JUCE` to use a local copy. On Linux the build produces the VST3 and standalone versions.

Every push that touches `plugin/` runs `.github/workflows/build-mac.yml` on a macOS runner. That job builds, tests, signs, validates the AU with `auval`, packages the .pkg, and updates the `plugin-latest` release.

## Inside

- `Source/StretchEngine.*` is the streaming spectral stretcher: phase vocoder with identity phase locking blended into random phase, peak-region pitch shifting, and drift. It also contains the reverb impulse and the FFT convolution. It has no JUCE dependency, and `Tests/EngineTests.cpp` tests it.
- `Source/Effects.*`: chorus, filter, dub echo and spectral effects, with the two-slot chain (series or parallel) and FX MIX. No JUCE dependency; tested in `Tests/EngineTests.cpp`.
- `Source/Modulation.h`: the 8 LFOs and the routing table. LFOs are pure functions of time, so the render threads, which work about 100 ms ahead of what you hear, evaluate them at exactly the moment their audio will play.
- `Source/ModUI.h`: LFO tiles, route chips and the XY pad. `makeFxParams` in `Source/Offline.h` applies the pad's axis mapping, so playback and export use the same rules.
- `Source/VoiceEngine.*` holds the 8 polyphonic voices. Each voice renders about 100 ms ahead on background threads into a lock-free FIFO, so the bursty FFT work never lands on the audio thread.
- `Source/Offline.*` contains the Normalize analyzer, the file export and the WAV/AIFF writer.
- `Source/PluginProcessor.*` handles parameters, MIDI, transport, audition, reverb, the limiter and saved state. `Source/PluginEditor.*` and `Source/Look.h` are the UI.

JUCE is used under its AGPLv3 / commercial licence terms. The fonts (Archivo, IBM Plex Mono) are under the SIL Open Font License; see `Resources/fonts`.
