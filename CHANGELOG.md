# PolyTrack changelog

## 0.3.2 (2026-11-17)
Faster single notes. Measured on test strings through a pickup model, -20 dBFS (0.3.1 -> 0.3.2):
single notes median 18 -> 10 ms, average 18.0 -> 14.9 ms, worst (low D) 38 -> 32 ms, low
strings (D2-D3) 29 -> 24 ms, mid notes (D4-B4) 12-18 -> 10 ms; extra notes 3 -> 1. Chords:
about the same speed (median 40 -> 42 ms), 2 more chord notes found, ghosts 30 -> 24.
- Attack mode: for ~20 ms after a pick, each partial is read from the fastest window that can
  separate it from the note's own neighbouring partials, instead of one that separates
  semitones. A note's low partials no longer wait for the 64 ms window to fill (D4: 16 ms
  window instead of 64 ms).
- Test harness: a chord tone that comes on before its own string is picked now counts as a
  ghost (it is another string's partial); 0.3.1's chord numbers counted those as fast notes.

## 0.3.1 (2026-11-17)
Lower latency. Measured on test strings through a pickup model, -20 dBFS (0.3.0 -> 0.3.1):
single notes avg 21.1 -> 18.0 ms, low strings (D2-D3) 32 -> 29 ms, chords median 40 -> 38 ms,
chord notes found 45 -> 49 of 54, slowest chord note 632 -> 220 ms. Without the pickup model
single notes are 18.6 -> 16.1 ms (worst 32 -> 26 ms).
- The analysis windows give the newest audio more weight (falling edge 25% -> 15% of the
  window), so a pick counts at full strength sooner.
- A note whose first four partials are all present, in tune and at least half as strong as the
  loudest note goes on after one analysis frame instead of two. This mostly helps chord tones
  that used to be held back for a long time.
- Short chord ghosts: more of them, but much shorter (total time 1.26 -> 0.67 s over the test
  chords).

## 0.3.0 (2026-11-17)
PolyTrack is an audio effect again, with MIDI out: insert it on the guitar track, the guitar
passes through and the notes go out its MIDI output (like a MIDI send).
- PolyTrack.vst3: audio effect ("Fx|Analyzer"), stereo in/out, MIDI in/out. Same plugin ID
  as 0.1.x/0.2.0.
- New PolyTrack Instrument.vst3: the 0.2.0 instrument + side-chain version, now under its own
  name and plugin ID so both can be installed. Use it if a host won't list an effect's MIDI
  output as a MIDI track input.
- No DSP changes.

## 0.2.0 (2026-11-17)
Fix: MIDI out did nothing in some DAWs, pitch bends came out wrong, lower latency.
- The VST3 is now an INSTRUMENT with a side-chain input, on the assumption that the DAW only took
  MIDI from instrument plugins. (Wrong: 0.3.0 went back to an audio effect, whose MIDI output
  the DAW does take; the instrument stays available as PolyTrack Instrument.) Feed the guitar to
  its side-chain (or main input in hosts that give instruments audio); see README.
  The standalone app is unchanged (stereo in/out).
- VST3 pitch-bend output was broken (shared iPlug2 code scaled the -1..+1 bend by 16383 without
  centring it): "no bend" went out as full bend DOWN, so with Pitch Bend on every note sounded
  ~2 semitones flat. Bends now send the real 14-bit value.
- Tracker: a new note needs 2 agreeing frames on every string (low strings needed 3), and the
  pitch check accepts the nearest note instead of +-30 cents (a low string's first frames read
  sharp). Measured (test strings through a pickup model, -20 dBFS): single notes avg 23.2 ->
  21.0 ms, worst (low D) 44 -> 38 ms; chords avg 64.6 -> 50.2 ms; short extra blips in chords
  16 -> 21 over 12 chords, single notes unchanged. Without the pickup model: low D 42 -> 32 ms.

## 0.1.1 (2026-11-16)
Fix: higher-octave ghost notes from real pickups (a pickup makes a low string's upper partials
louder than its fundamental).
- Octave-down check accepts a weak fundamental when the lower note's other partials are all there.
- A low note's partials are removed up to 4.5 kHz (was 16 partials), so its treble partials no
  longer turn into high ghost notes.
- New OCTAVES switch (Tracker panel). STRICT (default): a note that is only an upper partial
  (2nd-8th) of a lower string is not sent; high notes wait up to ~40 ms after a pick while a
  lower string is still emerging; a removed ghost stays out until the next pick. ALLOW: old
  behaviour (keeps doubled octaves in chords, more ghosts).
- Saved states are versioned; 0.1.0 states still load (OCTAVES = Strict).
Measured (test strings through a pickup model, -20 dBFS): single-note extra notes 32 -> 4,
lingering ghosts 17 -> 2; chords extra notes 106 -> 19, lingering 38 -> 1; latency unchanged
(avg ~23 ms single notes). Cost: some doubled octaves in chords are dropped in STRICT.

## 0.1.0 (2026-11-14)
First build: polyphonic guitar tracker, MIDI out (single / per-note channels, hold, infinite
sustain, sustain pedal, thru, envelope CC, MIDI learn), 12-voice keyboard-chord harmonizer,
built-in monitor synth, input recorder. Standalone + VST3.
Measured: tracker CPU ~6 % of a core (worst 128-sample block ~20 %), whole plugin with the
harmonizer ~8 % (worst block ~27 %).
