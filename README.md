# PolyTrack

Polyphonic guitar-to-MIDI for a normal pickup, plus a keyboard-chord harmonizer. Windows
standalone app and VST3. Version 0.3.2.

Insert it on a guitar track: your guitar passes through, and what you play comes out of the
plug-in's MIDI output, chords included, with velocity and pitch bend. Route that MIDI to any
synth.

## What it does

**Guitar tracker** (normal mono pickup, no special hardware)
- Polyphonic note detection, up to 6 notes, D2 (drop D) to E6, with velocity and pitch bend.
- Settings: input, input gain, noise gate, sensitivity (how weak a chord note may be), max
  notes (1 = mono), lowest note, velocity range, pitch bend on/off, OCTAVES (Strict drops
  notes that are only an upper partial of a lower string; Allow keeps them).

**MIDI out**
- Single channel, or Per Note (MPE-style: one channel per string, its own pitch bend each).
- Bend range (sent to the synth as RPN 0), transpose, fixed or dynamic velocity, velocity curve.
- Hold (notes keep sounding after the string stops), Infinite sustain (until the next pick).
- Sustain pedal (CC 64 in) holds notes. MIDI Thru passes incoming MIDI to the output.
- Env CC: sends the guitar's level as a CC (filter or volume swells on a synth).
- MIDI LEARN: click LEARN, touch a control on screen, then move a knob or press a footswitch
  on your controller. CLEAR MAP removes all mappings. Mappings are saved with the session.
- PANIC sends all-notes-off.

**Harmonizer** (keyboard chords, up to 12 voices)
- A single-note input (a voice on a mic, or a guitar lead) is re-pitched to every note you hold.
- Chord From: MIDI keys (a keyboard, or the on-screen piano), the guitar tracker (sing while
  strumming and the voices follow the chord), or both.
- Voices, formant shift, detune, stereo spread, attack, release, level. Voices keep the
  singer's tone (pitch-synchronous resynthesis).

**Output**: Dry (the input passed through), Synth (a small built-in synth playing the MIDI
output, so you can hear the tracking without routing MIDI anywhere), Master, in/out meters.

**REC INPUT** records the inputs to `Documents\PolyTrack Recordings\take-*.wav`.

## Latency

Measured on synthetic plucked strings through a magnetic-pickup model at 48 kHz, from the pick
to the note-on, before your audio buffer:

| | |
|---|---|
| Single notes, typical | 10 ms |
| High notes (D5 and up) | 6 - 8 ms |
| Mid notes (D4 - B4) | 10 ms |
| Low strings (D2 - D3) | 20 - 32 ms |
| Chords, typical note | ~40 ms |
| Harmonizer | about 2 periods of the input pitch (~9 ms at 220 Hz) |

No tracker can be instant: it has to hear a cycle or two of a note, and one cycle of low E
lasts 12 ms. Chords from a single pickup are harder than single notes, because the strings'
partials overlap. Test set: all 21 single notes found; 49 of 54 notes over 12 strummed chord
shapes. Doubled octaves inside a chord are the weak spot. These numbers come from synthetic
strings, so real guitars will differ.

## Using it

### VST3 (two plug-ins)

- **PolyTrack** - an audio effect with MIDI out. Put it as an insert on the guitar's audio
  track. The guitar passes through (Dry knob) and the notes go out of its MIDI output. Then
  set a MIDI or instrument track's MIDI input to PolyTrack (some hosts call it "Event
  Output") and enable monitoring or record-arm on it.
- **PolyTrack Instrument** - the same plug-in as an instrument with a side-chain input, for
  hosts that only take MIDI from instruments. Load it on an instrument track, activate its
  side-chain, send the guitar track to that side-chain, and take its MIDI output as above.

Example: audio track -> insert PolyTrack; MIDI track -> set its MIDI input to PolyTrack.

Use a small audio buffer (128 samples or less at 48 kHz) for the lowest delay.

### Standalone app

In Settings pick the audio interface, a MIDI input (keyboard or controller) and a MIDI output.
To send MIDI to another program on Windows, install a virtual MIDI port driver and pick that
port as the output. Aim for -30 to -12 dBFS on the input level line while playing (raise
Input Gain if needed).

## Building (Windows)

Needs Visual Studio 2019 Build Tools (C++), Git, and PowerShell.

    git clone --recursive https://github.com/Gwojtowicz/PolyTrack.git
    cd PolyTrack
    powershell -ExecutionPolicy Bypass -File scripts\apply-patches.ps1
    cd iPlug2\Dependencies\IPlug; bash ./download-vst3-sdk.sh v3.8.1_build_84; cd ..\..\..
    cd PolyTrack
    MSBuild.exe PolyTrack.sln /t:PolyTrack-app  /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v142
    MSBuild.exe PolyTrack.sln /t:PolyTrack-vst3 /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v142

`apply-patches` puts the bundled iPlug2 changes (`patches/iPlug2.patch`) on the pinned iPlug2
submodule; iPlug2's own script downloads the VST3 SDK (MIT, needs Git Bash). The VST3's post-build copy into `C:\Program Files\Common Files\VST3` fails without
admin rights; the plug-in is still built (`build-win\PolyTrack.vst3`). Build the app target
on its own if MSBuild skips it after that failed copy.

The instrument version is the same source built with `PT_INSTRUMENT=1` into its own folder:

    MSBuild.exe projects\PolyTrack-vst3.vcxproj /p:SolutionDir=%CD%\ /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v142 /p:EXTRA_RELEASE_DEFS=PT_INSTRUMENT=1 /p:OutDir=%CD%\build-win\vst3i\ /p:IntDir=%CD%\build-win\vst3i\int\

Rename its `PolyTrack.vst3` to `PolyTrack Instrument.vst3`; it has its own plug-in ID, so both
can be installed side by side.

Only Windows is built and tested. The macOS/iOS/web project files are the unmodified iPlug2
template.

## Source

- `PolyTrack/dsp/PolyTracker.h` - the polyphonic tracker: multi-resolution spectra (16 / 32 / 64 ms
  windows every 2 ms), whitening, iterative estimation with partial cancellation, octave
  checks, attack mode (faster windows right after a pick), note state machine. Tuning
  constants are in `TrackerTuning`.
- `PolyTrack/dsp/Harmonizer.h` - pitch detector + PSOLA voices.
- `PolyTrack/dsp/NoteOutput.h` - MIDI output rules (channels, hold, sustain, bends, RPN).
- `PolyTrack/dsp/MonitorSynth.h` - the built-in synth.
- `PolyTrack/PolyTrack.cpp/.h`, `PolyTrack/PTControls.h` - plug-in, UI, MIDI learn, recorder.
- `PolyTrack/tests/` - the test harness behind the numbers above:
  - `trk.cpp` plays synthetic strings (optional pickup model) through the tracker and reports
    accuracy and latency. Build with `b.bat trk`, run `trk.exe -20 0.5 0.5 0 1`
    (level dBFS, sensitivity, pick brightness, noisy input, pickup model). `sweep.ps1` compares
    tuning settings.
  - `pthost.cpp` is a minimal VST3 host that runs a built plug-in on a raw float file and
    prints its MIDI output (`build.bat`).

## License

MIT, see `LICENSE`. Third-party components (iPlug2, the VST3 SDK, RtAudio, RtMidi, NanoVG,
Roboto and others) are listed in `ThirdPartyNotices.txt`.
