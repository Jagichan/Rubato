# Rubato
Rubato is a premium Windows utility designed to give your MIDI a pulse.

# Rubato  (version 1.4.0)
*Because music happens between the lines.*

Designed and Engineered : O.Jagannatha Rao (JagiChan)

Coded using Claude AI : Model Sonnet 5.5

Splash screen, icons and some text write-up including this readme was created using ChatGPT.

Acknowledgements : Many thanks to OpenAI & Anthropic for providing such a powerful AI development platforms. https://www.anthropic.com/ and https://openai.com/

**License:** Open source GNU licensed (see LICENSE, https://www.gnu.org/licenses/gpl-3.0.txt). Clone it, fork it, rip it apart, rebuild it.
If you find a bug, open an issue. If you want to add a feature, just do it and inform me and I'll merge it or else you can wait for me to implement it.

**Source code:** The whole thing is on GitHub: https://github.com/Jagichan/Rubato/

If you like it: https://buymeacoffee.com/jagichan

Native Win32 (C++) MIDI processor: multi-track view with live controller graphs, per-track effect chains in an
"MFX Chain" dialog, instrument presets, and real-time playback (winmm).  Effects are a **non-destructive preview**
until you press **Apply All FXs**.

## Build
Open `Rubato.sln` in Visual Studio 2019 / 2022 / 2026 (workload "Desktop development with C++"), choose
**Release | x64**, Ctrl+Shift+B, run with Ctrl+F5.  Output: `bin\x64\Release\Rubato.exe` (static CRT).
Command line: `build.bat [Release|Debug] [x64|x86]`.

## Using it
1. **Open** a MIDI file (or drag it onto the window).  A format 0 file (all channels in one track) is split into a
   conductor track plus one track per channel, so every instrument gets its own strip.  Each track is a coloured strip with its notes; the coloured
   graphs on top are pitch bend, expression (CC11), modulation (CC1), breath (CC2), volume (CC7) and velocity.
2. Select a track and press its **FX** button (or double-click the strip, or Ctrl+E): the **MFX Chain** dialog opens.
   Open the folder icon to pick an instrument preset (Wind, Reed, Brass, Strings, Plucked, Keys and voice, General),
   or press **Insert** to add single effects.  Click a parameter box to read what it does in the box below.
3. **Play** to listen - playback restarts by itself when you change a parameter.  **Bypass FX** = A/B with the original.
4. **Apply All FXs** bakes the chain into the song (Undo available).  **Save As** exports what you see and hear.

### Track list columns
| Column | | 
|---|---|
| Mute / Solo | buttons, work while playing |
| Ch / Patch | channel and General MIDI instrument name.  

**Click the Patch box** to pick another instrument from the 128 General MIDI sounds (16 families); it is heard at once and saved with Save As.  
Orange = changed from the file, "As in the file" undoes it |
| FX | opens the chain dialog (blue = effects active) |
| Vol / Pan | sliders: drag with the mouse, double-click resets (orange = changed from the file) |

Drag the **dividers in the column header** to resize the columns.  The chain dialog can be resized too.

### Timeline
* Click the ruler or a strip: start marker.  **Drag on the ruler**: select a part of the song (blue).
* **Play** then plays only the selection and **Loop** repeats it.  Switch **Loop** off while it plays and the music
  continues to the end of the song.  Click the ruler (or Playback > Clear) to remove the selection.
* Scrolling: wheel = up / down through the tracks, **Shift + wheel** or a **touch-pad sideways swipe** = along the time line,
  Ctrl + wheel (or pinch) = zoom.
* In the chain dialog **From selection** fills the From/To bars of an effect with the selection.

### MFX Chain columns
| Column | Meaning |
|--------|---|
| N | order in the chain (top to bottom) |
| Mute | click: bypass this effect |
| FX | effect name |
| All | x = the effect works on everything on the track |
| Exclude | x = work on everything **except** bar From to bar To |
| Use | x = work **only** on bar From to bar To |
| From / To | first / last bar (inclusive) |

## Effects
| Effect | What it does |
|---|---|
| Transpose | shift notes (drums can be skipped) |
| Quantizer | grid, strength, start offset, shift onward (swing), duration grid, reduce duration |
| Humanize | bell-curve random start (ms), velocity and length |
| Velocity | scale + offset |
| Phrase Finder | finds the phrases (min / max length and pause in **sixteenth notes**, sensitivity); the effects below it use them; phrase starts are marked red on the strip |
| Human Pitch / Mod / Expr | drift, vibrato, **scoop-in** (which notes: none / phrase starts / long notes approached from below / every note), **glide** into leaps, fall-off, modulation bell, expression wobble |
| Breath & Phrase Dynamics | per-note breath pulse, phrase arc, **breath gap** with loudness dip, velocity arc, turbulence; writes CC11 and/or CC2 |
| Strum | spreads chord notes in time (down / up / alternate) with velocity taper |

## How the presets were tuned
`flute_sample_raw / humanized` and `guitar_sample_raw / humanized` were measured note by note:
* **Flute**: starts almost untouched (RMS 0.8 ms); phrase-end notes shortened by exactly 80 ticks (~76 ms) = breath gap;
  expression: peak at the note start, relax by 17-28 % after ~60 ms, swell toward the next note, V-shaped dip to ~39 in
  the breath gap; velocities 73-115 following a phrase arc; **-61 cent scoop-in on long notes approached from below**,
  recovering in ~60 ms; controller resolution ~8 ms.  (The -200 cent spikes once per bar are already in the raw file.)
* **Guitar**: pick dip of about -60 cents on every note (~45 ms), long fret glides of -100...-150 cents into upward
  leaps, modulation bells (up to ~115) starting ~40 % into long notes and fading by the end, velocities pulled toward 96.
The two "matches the reference sample" presets reproduce these.  Presets for other instruments are built from other reference files and
behaviours plus playing practice (sax scoops and fall-offs, violin vibrato and portamento, accordion bellows ...).

## Layout
    Rubato.sln, LICENSE, build.bat, icon.png
    Rubato/        midi_core.h  song model, SMF reader/writer, tempo map     (no Windows code)
                   midi_fx.h    effects, phrase detection, chains, ranges   (no Windows code)
                   player.h     winmm playback (selection loop, live volume/pan)
                   main.cpp     window, multi-track view, MFX Chain dialog, presets, About
                   Rubato.rc, rubato.ico   application icon and version information
    samples/       the reference files used to train the presets.
    Presets/       (created next to Rubato.exe when you save a preset; *.rps text files)
