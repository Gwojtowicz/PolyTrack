// NoteOutput.h -- turns tracker note events into outgoing MIDI: transpose, velocity shaping,
// hold / infinite sustain / sustain pedal, single-channel or per-note-channel (MPE-style)
// output with pitch bend, and RPN pitch-bend-range setup.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "PolyTracker.h"

namespace polytrack
{

struct RawMidi
{
  int offset;
  uint8_t status, d1, d2;
};

struct OutputSettings
{
  bool perNoteChannels = false; // false: everything on `channel`; true: MPE lower zone (ch 2..)
  int channel = 0;              // 0-based, single-channel mode (MPE master is channel 0)
  int bendRange = 2;            // semitones (send matching value to the synth)
  int transpose = 0;            // semitones
  int fixedVelocity = 0;        // 0 = dynamic
  float velCurve = 1.0f;        // out = 127 * (v/127)^curve
  float holdMs = 0.0f;          // extra time a note keeps sounding after the string stops
  bool infinite = false;        // notes ring until the next pick
  bool bends = true;
  int maxChannels = 6;          // per-note mode: channels 2..(1+maxChannels)
};

class NoteOutput
{
public:
  void Init(double sampleRate)
  {
    mSR = sampleRate;
    Reset();
  }

  void Reset()
  {
    for (auto& n : mNotes)
      n = Out();
    mChanBusy.fill(-1);
    mNextChan = 0;
    mPedal = false;
    mRpnPending = true;
    mLastSingleBend = 8192;
  }

  // Changing channel layout or transpose while notes sound would strand them: flush first.
  void SetSettings(const OutputSettings& s, std::vector<RawMidi>& out)
  {
    const bool layout = s.perNoteChannels != mSet.perNoteChannels || s.channel != mSet.channel
                        || s.transpose != mSet.transpose || s.maxChannels != mSet.maxChannels;
    const bool range = s.bendRange != mSet.bendRange;
    if (layout)
      AllNotesOff(0, out);
    mSet = s;
    if (layout || range)
      mRpnPending = true;
  }

  void SetPedal(bool down, int offset, std::vector<RawMidi>& out)
  {
    if (mPedal && !down)
    {
      mPedal = false;
      for (int n = 0; n < 128; n++)
        if (mNotes[n].sounding && !mNotes[n].physical && !mSet.infinite)
          _Release(n, offset, out);
    }
    mPedal = down;
  }

  void AllNotesOff(int offset, std::vector<RawMidi>& out)
  {
    for (int n = 0; n < 128; n++)
      if (mNotes[n].sounding)
        _Release(n, offset, out);
  }

  // Call once per block before Event(); emits RPNs when needed and processes hold timers.
  void BeginBlock(int nFrames, std::vector<RawMidi>& out)
  {
    if (mRpnPending)
    {
      mRpnPending = false;
      if (mSet.perNoteChannels)
        for (int c = 1; c <= std::clamp(mSet.maxChannels, 1, 15); c++)
          _Rpn(c, out);
      else
        _Rpn(mSet.channel, out);
    }
    const int64_t holdSamples = (int64_t)(mSet.holdMs * 0.001 * mSR);
    for (int n = 0; n < 128; n++)
    {
      Out& o = mNotes[n];
      if (!o.sounding || o.physical || mSet.infinite || mPedal)
        continue;
      const int64_t due = o.releasedAt + holdSamples;
      if (due < mTime + nFrames)
        _Release(n, (int)std::clamp<int64_t>(due - mTime, 0, nFrames - 1), out);
    }
  }

  void EndBlock(int nFrames) { mTime += nFrames; }

  void Event(const NoteEvent& e, std::vector<RawMidi>& out)
  {
    if (e.note < 0 || e.note > 127)
      return;
    Out& o = mNotes[e.note];
    switch (e.type)
    {
      case NoteEvent::kOn:
      {
        // a new pick ends infinite sustain on every note that isn't physically ringing
        if (mSet.infinite)
          for (int n = 0; n < 128; n++)
            if (n != e.note && mNotes[n].sounding && !mNotes[n].physical)
              _Release(n, e.offset, out);
        if (o.sounding)
          _Release(e.note, e.offset, out); // re-pick of a held note
        const int outNote = std::clamp(e.note + mSet.transpose, 0, 127);
        int vel = mSet.fixedVelocity > 0 ? mSet.fixedVelocity
                                         : (int)std::lround(127.0 * std::pow(e.velocity / 127.0, (double)std::max(0.1f, mSet.velCurve)));
        vel = std::clamp(vel, 1, 127);
        int ch = mSet.channel;
        if (mSet.perNoteChannels)
          ch = _AllocChannel(e.note, e.offset, out);
        o.sounding = true;
        o.physical = true;
        o.outNote = outNote;
        o.channel = ch;
        if (mSet.perNoteChannels)
          _Bend(ch, 0.0f, e.offset, out); // centre the channel before the note
        out.push_back({e.offset, (uint8_t)(0x90 | ch), (uint8_t)outNote, (uint8_t)vel});
        mLastNote = e.note;
        break;
      }
      case NoteEvent::kOff:
      {
        if (!o.sounding)
          return;
        o.physical = false;
        o.releasedAt = mTime + e.offset;
        if (mSet.holdMs <= 0.0f && !mSet.infinite && !mPedal)
          _Release(e.note, e.offset, out);
        break;
      }
      case NoteEvent::kBend:
      {
        if (!o.sounding || !mSet.bends)
          return;
        if (mSet.perNoteChannels)
          _Bend(o.channel, e.cents, e.offset, out);
        else if (e.note == mLastNote)
          _Bend(mSet.channel, e.cents, e.offset, out, true); // single channel: follow the newest note
        break;
      }
    }
  }

  // Note currently sounding at the output (after hold), for display.
  bool Sounding(int note) const { return mNotes[note].sounding; }

private:
  struct Out
  {
    bool sounding = false;
    bool physical = false;
    int outNote = 0;
    int channel = 0;
    int64_t releasedAt = 0;
  };

  void _Release(int n, int offset, std::vector<RawMidi>& out)
  {
    Out& o = mNotes[n];
    if (!o.sounding)
      return;
    out.push_back({offset, (uint8_t)(0x80 | o.channel), (uint8_t)o.outNote, 0});
    if (mSet.perNoteChannels && o.channel >= 1 && o.channel < 16 && mChanBusy[o.channel] == n)
      mChanBusy[o.channel] = -1;
    o.sounding = false;
    o.physical = false;
  }

  int _AllocChannel(int note, int offset, std::vector<RawMidi>& out)
  {
    const int nCh = std::clamp(mSet.maxChannels, 1, 15);
    for (int k = 0; k < nCh; k++)
    {
      const int c = 1 + (mNextChan + k) % nCh;
      if (mChanBusy[c] < 0)
      {
        mChanBusy[c] = note;
        mNextChan = (mNextChan + k + 1) % nCh;
        return c;
      }
    }
    // all busy: steal round-robin
    const int c = 1 + mNextChan % nCh;
    if (mChanBusy[c] >= 0)
      _Release(mChanBusy[c], offset, out);
    mChanBusy[c] = note;
    mNextChan = (mNextChan + 1) % nCh;
    return c;
  }

  void _Bend(int ch, float cents, int offset, std::vector<RawMidi>& out, bool single = false)
  {
    const float range = (float)std::max(1, mSet.bendRange) * 100.0f;
    const int v = std::clamp((int)std::lround(8192.0f + 8191.0f * std::clamp(cents / range, -1.0f, 1.0f)), 0, 16383);
    if (single)
    {
      if (v == mLastSingleBend)
        return;
      mLastSingleBend = v;
    }
    out.push_back({offset, (uint8_t)(0xE0 | ch), (uint8_t)(v & 0x7F), (uint8_t)(v >> 7)});
  }

  void _Rpn(int ch, std::vector<RawMidi>& out)
  {
    const uint8_t s = (uint8_t)(0xB0 | ch);
    out.push_back({0, s, 101, 0});
    out.push_back({0, s, 100, 0});
    out.push_back({0, s, 6, (uint8_t)std::clamp(mSet.bendRange, 1, 96)});
    out.push_back({0, s, 38, 0});
    out.push_back({0, s, 101, 127});
    out.push_back({0, s, 100, 127});
  }

  double mSR = 48000.0;
  OutputSettings mSet;
  std::array<Out, 128> mNotes{};
  std::array<int, 16> mChanBusy{};
  int mNextChan = 0;
  bool mPedal = false;
  bool mRpnPending = true;
  int mLastNote = -1;
  int mLastSingleBend = 8192;
  int64_t mTime = 0;
};

} // namespace polytrack
