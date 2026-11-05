# Slow Sippin'

An extreme time-stretch instrument for textures, pads and drones. It can make a
short sample last for minutes without changing its pitch.

Slow Sippin is a standalone macOS app for macOS 12 Monterey or later, including Sonoma 14.6. It runs fully offline and is built with Electron. Rendering runs in background threads, one per channel. Open and Save use the normal Mac dialogs.

## Install on macOS

1. Unzip the build for your Mac:
   - `Slow-Sippin-1.0.0-mac-apple-silicon.zip` for M1/M2/M3/M4 Macs
   - `Slow-Sippin-1.0.0-mac-intel.zip` for Intel Macs

   To check which you have, go to  → About This Mac and look at "Chip".
2. Drag **Slow Sippin.app** into **Applications**.
3. **First launch only.** The app isn't signed with an Apple developer certificate, so macOS blocks it at first (often saying it "is damaged"). Open **Terminal** and paste:

   ```sh
   xattr -cr "/Applications/Slow Sippin.app" && codesign --force --deep --sign - "/Applications/Slow Sippin.app"
   ```

   This removes the download quarantine and signs the app locally on your Mac. Then open it normally.

You can also open a sample by dropping it on the Dock icon, or from Finder with Open With → Slow Sippin.

### Keyboard shortcuts

| | |
|---|---|
| ⌘O | Load sample… |
| ⌘E | Export… |
| ⌘R | Render / cancel |
| ⌘P or Space | Play / stop |
| ⌘L | Audition |
| ⌘D | Clear selection |

### Building the app yourself

Requires Node.js 20 or later.

```sh
npm install
npm start                       # run without packaging
npm run dist:mac                # → dist/*.zip for Apple Silicon and Intel
npm run dist:mac -- arm64       # one architecture only
```

On a Mac the build is ad-hoc signed automatically. An app you build yourself isn't quarantined, so it opens without the Terminal step.

The same files also run in a browser (`python3 -m http.server`, then open `http://localhost:8000`). In that case, Download saves through the browser.

## Workflow

1. **Load a sample.** Drag and drop it anywhere, click **+ SAMPLE**, or press ⌘O. WAV, AIFF / AIF and FLAC load at their native sample rate. MP3 also loads at its native rate in most browsers.
2. **Choose a region (optional).** Drag on the source waveform to select part of the sound. Click once to set a start point. Double-click to clear.
3. **Listen first.** **AUDITION** plays the whole sample, or only the region.
4. **Set the controls, then press RENDER.** A progress bar and time estimate appear. Press **CANCEL** to stop.
5. **Play and check.** Click anywhere on the rendered waveform to start playback from that point. **Space** toggles play and stop.
6. **Export.** Choose WAV or AIFF and 16-bit (dithered), 24-bit or 32-bit float, then press **DOWNLOAD** (or ⌘E). A Save dialog opens, defaulting to your Music folder. The export includes the reverb tail.

## Controls

| Control | What it does | Needs a new render? |
|---|---|---|
| **Stretch** | How much longer the sound becomes. The slider goes from 1× to 1000× (logarithmic). The number box accepts up to 1,000,000×. | yes |
| **Pitch** | Transposition in semitones (±48 on the slider, ±60 in the box). Independent of stretch. | yes |
| **Weave** | Number of grains overlapping at each moment (1–16). At 1 the sound is broken and pulsing. Higher values sound smooth and glassy. | yes |
| **Scatter** | How much of the phase is invented. 0 % keeps the real phase: a coherent phase vocoder in which attacks keep their shape. 100 % gives the full smeared texture. | yes |
| **Drift** | The read position wanders by up to ± this many ms. Movement combines slow and medium curves with a per-grain jitter. | yes |
| **Mono → stereo** | Turns a mono source into wide stereo using two independent phase passes. | yes |
| **Reverb / Size** | Internal reverb: wet amount and decay time (RT60). Applied after the stretch. | no, heard at once |
| **Gain** | Output level, applied after Normalize. | no |
| **Normalize** | Brings the peak to −0.5 dBFS, measured after the reverb. | no |
| **Reverse** | Plays the stretched sound backwards. The reverb still rings after it. | no |

Stretch, Pitch, Weave, Scatter, Drift and Mono → stereo are marked with a red dot. When one of them changes after a render, the RENDER button gets a ring and the hint tells you to render again.

Tips:
- Double-click a slider to reset it.
- Arrow keys in a number box nudge the value. Hold Shift for steps ×10.
- The number boxes accept `49,25` as well as `49.25`.
- Settings are remembered between visits.

## How the engine works

`js/engine.js` is an STFT engine in the phase-vocoder and PaulStretch family. The grain size is about 170 ms (8192 samples at 44.1/48 kHz) with a Hann window.

For each output grain:

1. **Read.** The read position is `outputPosition / stretch`, plus any drift offset. The windowed frame is FFT'd.
2. **Phase.** With Scatter below 100 %, a second frame N/8 later gives each bin's true frequency. Phases are propagated from that frequency and locked around each spectral peak to the real analysis phase (identity phase locking). Scatter then adds a random offset of up to ±π. At 100 % the phases are fully random.
3. **Pitch.** Each peak and its surrounding bins move to the transposed frequency (peak-region shifting), so pitch never depends on stretch.
4. **Write.** The inverse FFT is windowed and overlap-added with hop `N / weave`. Level compensation blends coherent and incoherent overlap-add gain, so loudness stays steady as Weave and Scatter change.

Output is limited to about 64 M samples (≈ 11½ minutes of stereo at 48 kHz) to keep memory reasonable. Choose a shorter region for very large stretches.

## Files

```
electron/main.js     macOS app: window, menu, app:// file serving, Open/Save dialogs, Finder "Open With"
electron/preload.js  the narrow bridge between the page and macOS
scripts/build-mac.js packages, signs and zips the .app (arm64 + x64)
build/icon.icns      app icon
fonts/               Archivo + IBM Plex Mono (SIL Open Font License), bundled for offline use
index.html       layout
css/style.css    styling (responsive down to phone width)
js/engine.js     FFT, StretchJob, reverb impulse generator
js/worker.js     runs one StretchJob per channel off the main thread
js/codec.js      WAV / AIFF / AIFC parsing, MP3 / FLAC rate sniffing, WAV / AIFF writing
js/app.js        UI, region selection, playback graph, render orchestration, export
```
