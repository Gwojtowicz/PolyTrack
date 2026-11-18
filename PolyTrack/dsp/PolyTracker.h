// PolyTracker.h -- polyphonic guitar-to-note tracker for a normal (mono) pickup.
//
// Method (our own implementation of published multi-pitch ideas):
//  1. Decimate the input to ~16 kHz (guitar partials worth using sit below ~5 kHz).
//  2. Every 2 ms take three spectra with asymmetric (fast-decay) windows: 64 / 32 / 16 ms,
//     zero-padded 2x. Each partial is read from the SHORTEST window that can still separate
//     it from the neighbouring semitone, so upper partials (and high notes) react quickly.
//  3. Whiten: subtract the local spectral envelope so only tonal peaks count (a pick's
//     broadband burst scores ~0).
//  4. Score every note by a weighted sum of its harmonic amplitudes, pick the best, check it
//     for octave / twelfth errors, remove its partials from a residual spectrum and repeat
//     (iterative estimation + cancellation) up to the note limit.
//  5. A per-note state machine turns frame detections into note on / off / pitch-bend events
//     with hysteresis, re-pick detection and bend tracking.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>
#ifdef PT_TRACE
#include <cstdio>
#endif
#ifdef PT_PROF
#include <chrono>
namespace polytrack { inline double gProf[8] = {}; inline double ProfNow() { return std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now().time_since_epoch()).count(); } }
#define PROF_MARK(i) { double _t = ProfNow(); gProf[i] += _t - _pt; _pt = _t; }
#else
#define PROF_MARK(i)
#endif

#include "PTFft.h"

namespace polytrack
{

// Tuning knobs (fixed in the plugin; the test harness may override them before Init()).
struct TrackerTuning
{
  // 0.3.1: 0.25 -> 0.15 (a pick reaches full weight sooner: ~3 ms faster, chord ghosts shorter)
  float fall[3] = {0.15f, 0.15f, 0.15f}; // newest fraction of each window that falls to zero
  float needK = 33.6f;                    // window choice: lobe width vs semitone (bigger = longer windows)
  int needFrames = 2;                     // agreeing frames before a note-on
  int emergeFrames = 20;                  // max frames a candidate waits for a lower string after a pick
  int gapTol = 0;                         // missed frames tolerated while counting towards a note-on
  float fastRel = 0.5f;                   // 1-frame note-on when salience >= this x the strongest note (>1 = off)
  int fastPresent = 4;                    // ...and this many of its first 4 partials are present
  float fastCents = 25.0f;                // ...and its pitch is within this many cents
  bool harmDrop = false;                  // estimator replaces an upper partial by its emerging lower string
  // 0.3.2: 8 (right after a pick, partials only need separating from the note's own neighbours:
  // D4 is read from the 16 ms window instead of the 64 ms one -> single notes median 18 -> 10 ms)
  float attackK = 8.0f;                   // window rule right after a pick (0 = off; smaller = faster windows)
  int attackFrames = 10;                  // frames after a pick that use attackK
  int blend = 0;                          // 1: always take the stronger of the normal / attackK readings
  int attackMaxOn = 99;                   // attack mode only while at most this many notes are sounding
  float hopMs = 2.0f;                     // analysis interval
  int repickOwn = 1;                      // re-pick on the note's own (1-5 kHz, semitone-resolved) level
  float repickLoHz = 1000.0f;
  float repickFloorDb = -90.0f;
  float repickBelowPeakDb = 12.0f;
  int maskFrames = 20;                    // (2 ms frames) after a pick, a still-loud note...
  int maskMiss = 8;                       // ...may be missed this many frames before release
  int highPartials = 1;                   // strict octaves also drop 9th..24th partials of a lower string
  int repickWait = 8;                     // (2 ms frames) a level re-pick waits this long for a competing new note
  float repickRel = 0.5f;                 // ...and the estimator found it at >= this x the strongest note
  int repickAge = 20;                     // (2 ms frames) no level re-pick before this age
  float repickDb = 6.0f;                  // a ringing note whose level jumps this much at a pick is re-picked
  float velCalDb = 2.0f;                  // note level -> dBFS calibration (see _NoteLevelDb; measured with tests/seq.cpp)
  int velRes = -1;                        // window for the note level (-1 = attack-mode choice, 0 = fastest)
  int retAge = 40;                      // a note that ended less than this (2 ms frames) ago...
  int retExtra = 6;                       // ...needs this many extra frames to return (no flicker)
  float onsetDb = 5.0f;                   // level jump that counts as a pick
  int blockMax = 100000;                  // an upper-partial block lasts at most this (2 ms frames) without a pick
};
inline TrackerTuning gTune;

inline float MidiToHz(float n) { return 440.0f * std::pow(2.0f, (n - 69.0f) / 12.0f); }
inline float HzToMidi(float f) { return 69.0f + 12.0f * std::log2(f / 440.0f); }
inline float DbToLin(float db) { return std::pow(10.0f, db / 20.0f); }
inline float LinToDb(float x) { return 20.0f * std::log10(std::max(x, 1e-12f)); }

// Windowed-sinc FIR decimator (integer factor).
class Decimator
{
public:
  void Init(int factor)
  {
    mD = std::max(1, factor);
    const int taps = 24 * mD + 1;
    mH.assign(taps, 0.0f);
    const double fc = 0.38 / mD; // cycles / input sample (-6 dB point)
    const int M = taps / 2;
    double sum = 0.0;
    for (int k = 0; k < taps; k++)
    {
      const double t = k - M;
      const double sinc = (t == 0.0) ? 2.0 * fc : std::sin(2.0 * 3.14159265358979 * fc * t) / (3.14159265358979 * t);
      const double w = 0.42 - 0.5 * std::cos(2.0 * 3.14159265358979 * k / (taps - 1))
                       + 0.08 * std::cos(4.0 * 3.14159265358979 * k / (taps - 1));
      mH[k] = (float)(sinc * w);
      sum += mH[k];
    }
    for (auto& h : mH)
      h = (float)(h / sum);
    mBuf.assign(taps * 2, 0.0f);
    mPos = 0;
    mPhase = 0;
  }

  int Factor() const { return mD; }
  int GroupDelayInput() const { return (int)mH.size() / 2; }

  // Returns true when an output sample is produced.
  bool Push(float x, float& out)
  {
    const int taps = (int)mH.size();
    mBuf[mPos] = x;
    mBuf[mPos + taps] = x;
    mPos = (mPos + 1) % taps;
    if (++mPhase < mD)
      return false;
    mPhase = 0;
    if (mD == 1)
    {
      out = x;
      return true;
    }
    const float* b = &mBuf[mPos]; // oldest..newest
    float acc = 0.0f;
    for (int k = 0; k < taps; k++)
      acc += b[k] * mH[taps - 1 - k];
    out = acc;
    return true;
  }

private:
  int mD = 1;
  std::vector<float> mH;
  std::vector<float> mBuf;
  int mPos = 0;
  int mPhase = 0;
};

struct TrackerSettings
{
  float gateDb = -72.0f;      // partials below this level are ignored (dBFS, after input gain)
  float sensitivity = 0.5f;   // 0..1: higher = weaker notes in a chord are accepted
  int maxNotes = 6;           // 1 = mono
  int lowNote = 38;           // D2 (drop D) by default
  int highNote = 88;          // E6
  float velRangeDb = 30.0f;   // dynamic range mapped onto velocity 1..127
  float velTopDb = -10.0f;    // pick level (dBFS, after input gain) that gives velocity 127
  bool bendTracking = true;
  bool strictOctaves = true;  // reject notes that are only upper partials of a lower string
};

struct NoteEvent
{
  enum Type
  {
    kOff = 0,
    kOn = 1,
    kBend = 2
  };
  int type;
  int note;
  int velocity; // on only
  float cents;  // bend only (deviation from the note)
  int offset;   // sample offset inside the processed block
};

class PolyTracker
{
public:
  static constexpr int kMaxNote = 128;
  static constexpr int kNumRes = 3;

  void Init(double sampleRate)
  {
    mSR = sampleRate;
    const int D = std::max(1, (int)std::lround(sampleRate / 16000.0));
    mDec.Init(D);
    mFa = (float)(sampleRate / D);
    mHop = std::max(8, (int)std::lround(mFa * 0.001 * gTune.hopMs)); // 2 ms (gTune.hopMs)
    // every timer below was tuned in 2 ms frames: scale counts and per-frame smoothing
    mFS = std::max(1, (int)std::lround(2.0 / gTune.hopMs));
    kSettleFrames = 25 * mFS;
    mHistN = 6 * mFS;
    mKeep06 = std::pow(0.6f, 1.0f / mFS);
    mDecPeak = std::pow(0.9995f, 1.0f / mFS);
    mDecRes = std::pow(0.995f, 1.0f / mFS);
    const int sizes[kNumRes] = {256, 512, 1024};
    for (int r = 0; r < kNumRes; r++)
    {
      Res& R = mRes[r];
      R.n = sizes[r];
      R.p = 2 * R.n;
      R.fft.Init(R.p);
      R.win.assign(R.n, 0.0f);
      // asymmetric window: slow sin^2 rise over 75 %, fast fall over the newest 25 %
      const int rise = (int)(R.n * (1.0f - gTune.fall[r]));
      const int fall = R.n - rise;
      double sum = 0.0;
      for (int i = 0; i < R.n; i++)
      {
        double w;
        if (i < rise)
          w = std::sin(0.5 * 3.14159265358979 * (i + 0.5) / rise);
        else
          w = std::cos(0.5 * 3.14159265358979 * (i - rise + 0.5) / fall);
        w *= w;
        R.win[i] = (float)w;
        sum += w;
      }
      R.norm = (float)(2.0 / sum);
      R.buf.assign(R.p / 2, {0.0f, 0.0f});
      R.in.assign(R.n, 0.0f);
      R.mn.assign(R.p / 2, 0.0f);
      R.suf.assign(R.p / 2, 0.0f);
      R.mag.assign(R.p / 2, 0.0f);
      R.wht.assign(R.p / 2, 0.0f);
      R.ton.assign(R.p / 2, 0.0f);
      R.res.assign(R.p / 2, 0.0f);
      R.pre.assign(R.p / 2 + 1, 0.0);
      R.pre2.assign(R.p / 2 + 1, 0.0);
    }
    mRing.assign(kRing, 0.0f);
    mRingPos = 0;
    mHopCount = 0;
    mK = gTune.needK;
    mTabSel = 0;
    mBlend = gTune.blend != 0 && gTune.attackK > 0.0f;
    _BuildTables();
    Reset();
  }

  void Reset()
  {
    for (auto& s : mNotes)
      s = NoteState();
    std::fill(mRing.begin(), mRing.end(), 0.0f);
    mLevelDb = -120.0f;
  }

  void SetSettings(const TrackerSettings& s) { mSet = s; }
  const TrackerSettings& Settings() const { return mSet; }

  bool IsOn(int note) const { return note >= 0 && note < kMaxNote && mNotes[note].on; }
  float Cents(int note) const { return mNotes[note].centsSm; }
  float LevelDb() const { return mLevelDb; }
  float DebugSal(int n) const { return _Salience(MidiToHz((float)n), kWhite); }
  float DebugPartial(float f) const { return _Partial(f, kWhite); }
  float DebugRaw(float f) const { return _Partial(f, kRaw); }
  const std::array<bool, kMaxNote>& LastDetected() const { return mLastDet; }

  // Process one block of input. Events get sample offsets within the block.
  void Process(const float* in, int nFrames, std::vector<NoteEvent>& events)
  {
    for (int i = 0; i < nFrames; i++)
    {
      float y;
      if (!mDec.Push(in[i], y))
        continue;
      mRing[mRingPos] = y;
      mRingPos = (mRingPos + 1) & (kRing - 1);
      if (++mHopCount >= mHop)
      {
        mHopCount = 0;
        _Analyze(i, events);
      }
    }
  }

private:
  static constexpr int kRing = 2048;

  struct Res
  {
    int n = 0; // window length
    int p = 0; // padded FFT length
    Fft fft;
    std::vector<float> win;
    float norm = 1.0f;
    std::vector<std::complex<float>> buf;
    std::vector<float> in, mn, suf;
    std::vector<float> mag; // raw magnitude
    std::vector<float> wht; // whitened (tonal peaks only)
    std::vector<float> ton; // tonal part, level kept (magnitude above 3x the local floor)
    std::vector<float> res; // residual of wht during estimation
    std::vector<double> pre;
    std::vector<double> pre2;
  };

  struct NoteState
  {
    bool on = false;
    int seen = 0; // consecutive frames detected while off
    int gap = 0;  // missed frames inside the current `seen` run
    int miss = 0; // consecutive frames not detected while on
    int weak = 0; // consecutive frames below the release level while on
    int age = 0;  // frames since note-on
    float salPeak = 0.0f;
    std::array<float, 12> hist{}; // recent saliences (first mHistN used)
    float trackHz = 0.0f;
    float centsSm = 0.0f;
    float sentCents = 0.0f;
    int velocity = 0;
    int offAge = 1000;
    bool relabelled = false;
    float salRes = 0.0f;
    int harm = 0; // consecutive frames judged an upper partial of a sounding lower note
    bool harmBlock = false;
    std::array<float, 12> lvlHist{}; // recent note levels (dB), for re-pick detection
    float lvlPeak = -200.0f;
    int repickWait = 0; // frames left before a pending level re-pick fires
  };
  int kSettleFrames = 25; // ~50 ms (scaled to the analysis interval in Init)

  int ResFor(float f) const { return ResForK(f, mK); }
  int ResForK(float f, float K) const
  {
    // the window's main lobe (~2 bins half-width) must be narrower than a semitone at f
    // (K = needK); right after a pick a smaller K picks faster windows (attack mode)
    const float need = K * mFa / std::max(f, 1.0f);
    for (int r = 0; r < kNumRes; r++)
      if ((float)mRes[r].n >= need)
        return r;
    return kNumRes - 1;
  }

  enum ESpec
  {
    kRaw,
    kWhite,
    kResidual,
    kTonal
  };

  // Precomputed partial lookups for every MIDI note (same rules as _Partial()).
  struct PTab
  {
    int8_t r = -1; // resolution, -1 = out of range
    bool interp = false, lowEdge = false, highEdge = false;
    int16_t k = 0, k0 = 0, k1 = 0;
    float fr = 0.0f;
  };
  static constexpr int kMaxH = 16;
  static constexpr int kCancelH = 40;
  PTab mTabs[2][kMaxNote][kMaxH + 1]; // [0] normal windows, [1] attack (faster) windows
  float mW[kMaxNote][kMaxH + 1];
  int mNH[kMaxNote];
  float mK = 33.6f;  // current window rule
  int mTabSel = 0;   // which table _PartialT uses
  bool mBlend = false;

  void _BuildTables()
  {
    for (int sel = 0; sel < 2; sel++)
    for (int n = 0; n < kMaxNote; n++)
    {
      const float f0 = MidiToHz((float)n);
      mNH[n] = _NumHarm(f0);
      const float K = sel == 0 ? gTune.needK : (gTune.attackK > 0.0f ? gTune.attackK : gTune.needK);
      for (int h = 1; h <= kMaxH; h++)
      {
        PTab& t = mTabs[sel][n][h];
        t = PTab();
        mW[n][h] = _Weight(f0, h);
        const float f = f0 * h;
        const int r = ResForK(f, K);
        const Res& R = mRes[r];
        const float bin = f * R.p / mFa;
        if (bin < 1.0f || bin > R.p / 2 - 3)
          continue;
        t.r = (int8_t)r;
        const float w = bin * 0.0145f;
        if (w < 1.0f)
        {
          t.interp = true;
          t.k = (int16_t)(int)bin;
          t.fr = bin - (int)bin;
          t.lowEdge = t.fr - w < 0.0f;
          t.highEdge = t.fr + w > 1.0f;
        }
        else
        {
          t.k0 = (int16_t)std::max(1, (int)std::floor(bin - w));
          t.k1 = (int16_t)std::min(R.p / 2 - 2, (int)std::ceil(bin + w));
        }
      }
    }
  }

  float _PartialT(int n, int h, ESpec spec) const
  {
    if (mBlend && spec != kRaw)
      return std::max(_PartialTab(mTabs[0][n][h], spec), _PartialTab(mTabs[1][n][h], spec));
    return _PartialTab(mTabs[mTabSel][n][h], spec);
  }

  float _PartialTab(const PTab& t, ESpec spec) const
  {
    if (t.r < 0)
      return 0.0f;
    const std::vector<float>& m = _Spec(mRes[t.r], spec);
    if (t.interp)
    {
      float v = m[t.k] + (m[t.k + 1] - m[t.k]) * t.fr;
      if (t.lowEdge)
        v = std::max(v, m[t.k]);
      if (t.highEdge)
        v = std::max(v, m[t.k + 1]);
      return v;
    }
    float best = 0.0f;
    for (int k = t.k0; k <= t.k1; k++)
      best = std::max(best, m[k]);
    return best;
  }

  float _SalienceT(int n, ESpec spec) const
  {
    float s = 0.0f;
    for (int h = 1; h <= mNH[n]; h++)
      s += mW[n][h] * _PartialT(n, h, spec);
    return s;
  }

  const std::vector<float>& _Spec(const Res& R, ESpec s) const { return s == kRaw ? R.mag : s == kWhite ? R.wht : s == kTonal ? R.ton : R.res; }

  // Amplitude at f (within +-25 cents, interpolated when that is less than a bin).
  float _Partial(float f, ESpec spec, int forceRes = -1) const
  {
    if (forceRes >= 0)
      return _PartialR(f, spec, forceRes);
    const int r = ResFor(f);
    if (mBlend && spec != kRaw)
    {
      const int ra = ResForK(f, gTune.attackK);
      if (ra != r)
        return std::max(_PartialR(f, spec, r), _PartialR(f, spec, ra));
    }
    return _PartialR(f, spec, r);
  }

  float _PartialR(float f, ESpec spec, int r) const
  {
    const Res& R = mRes[r];
    const std::vector<float>& m = _Spec(R, spec);
    const float bin = f * R.p / mFa;
    if (bin < 1.0f || bin > R.p / 2 - 3)
      return 0.0f;
    const float w = bin * 0.0145f;
    if (w < 1.0f)
    {
      const int k = (int)bin;
      const float fr = bin - k;
      float v = m[k] + (m[k + 1] - m[k]) * fr;
      // allow a little mistuning: also look at the next bins within the tolerance
      if (fr - w < 0.0f)
        v = std::max(v, m[k]);
      if (fr + w > 1.0f)
        v = std::max(v, m[k + 1]);
      return v;
    }
    const int k0 = std::max(1, (int)std::floor(bin - w));
    const int k1 = std::min(R.p / 2 - 2, (int)std::ceil(bin + w));
    float best = 0.0f;
    for (int k = k0; k <= k1; k++)
      best = std::max(best, m[k]);
    return best;
  }

  static float _MinRange(const std::vector<float>& v, int a, int b)
  {
    float m = v[a];
    for (int k = a + 1; k <= b; k++)
      m = std::min(m, v[k]);
    return m;
  }

  static float _Weight(float f0, int h) { return (f0 + 52.0f) / (h * f0 + 320.0f); }

  int _NumHarm(float f0) const { return std::max(1, std::min(16, (int)(4500.0f / f0))); }

  float _Salience(float f0, ESpec spec) const
  {
    const int H = _NumHarm(f0);
    float s = 0.0f;
    for (int h = 1; h <= H; h++)
      s += _Weight(f0, h) * _Partial(f0 * h, spec);
    return s;
  }

  // Count harmonics of the lower candidate (f0 / div) that are NOT shared with f0 and are present.
  int _ExtraEvidence(float f0, int div, float ref, int* tested) const
  {
    const float fl = f0 / div;
    int count = 0, n = 0;
    for (int h = 1; h <= 8 && n < 4; h++)
    {
      if (h % div == 0)
        continue; // shared with the upper candidate
      if (fl * h > 4500.0f)
        break;
      n++;
      if (_Partial(fl * h, kResidual) > ref)
        count++;
    }
    *tested = n;
    return count;
  }

  // Is note n just an upper partial (2nd..6th) of a lower string? Returns that lower note or -1.
  // Every partial of an octave-up note is shared with the lower note, so the only evidence for
  // a real upper note is a partial clearly louder than the lower note's neighbouring partials.
  // requireOn: only lower notes that are already sounding count (used to drop sounding ghosts).
  int _HarmonicOf(int n, const std::array<bool, kMaxNote>& det, bool requireOn) const
  {
    static constexpr int kIv[7] = {12, 19, 24, 28, 31, 34, 36}; // partials 2..8
    const float f0 = MidiToHz((float)n);
    const float pn = _Partial(f0, kWhite);
    if (pn <= 0.0f)
      return -1;
    const int lo = std::max(0, mSet.lowNote);
    for (int i = 0; i < 7; i++)
    {
      const int m = n - kIv[i], d = i + 2;
      if (m < lo)
        break;
      const float fm = f0 / d;
      bool lower = mNotes[m].on;
      if (!lower && !requireOn)
      {
        if (det[m])
          lower = true;
        else
        {
          // the lower string is still emerging (attack): its unshared partials and at least
          // a trace of its fundamental are already there
          int got = 0, tested = 0;
          for (int h = 1; h <= 7 && tested < 4; h++)
          {
            if (h % d == 0 || fm * h > 4500.0f)
              continue;
            tested++;
            if (_Partial(fm * h, kWhite) > 0.15f * pn)
              got++;
          }
          lower = tested >= 2 && got * 4 >= tested * 3 && _Partial(fm, kWhite) > 0.05f * pn;
        }
      }
      if (!lower)
        continue;
      const float a = _Partial(fm * (d - 1), kWhite);
      const float b = fm * (d + 1) < 4500.0f ? _Partial(fm * (d + 1), kWhite) : a;
      const float expect = std::max(0.5f * (a + b), 1e-9f);
      if (pn < 2.0f * expect)
        return m;
    }
    // higher partials (9th..24th) of a sounding lower string: they fall between semitones
    // (the 11th is a quarter tone off), and a pickup's resonance makes them loud enough to
    // read as high notes. Same test: not clearly louder than that string's neighbouring partials.
    if (gTune.highPartials)
      for (int m = lo; m <= n - 37; m++)
      {
        const bool sounding = mNotes[m].on || (!requireOn && det[m]);
        if (!sounding)
          continue;
        const float fm = mNotes[m].on && mNotes[m].trackHz > 0.0f ? mNotes[m].trackHz : MidiToHz((float)m);
        const int d = (int)std::lround(f0 / fm);
        if (d < 9 || d > 24 || std::fabs(1200.0f * std::log2(f0 / (d * fm))) > 60.0f)
          continue;
        const float a = _Partial(fm * (d - 1), kWhite);
        const float b = _Partial(fm * (d + 1), kWhite);
        const float expect = std::max(0.5f * (a + b), 1e-9f);
        if (pn < 2.0f * expect)
          return m;
      }
    return -1;
  }

  // Right after a pick, a low string's upper partials show up before its fundamental can be
  // resolved. Hold a candidate while the fast window shows energy at one of its subharmonics
  // whose note isn't sounding yet (at most ~40 ms; then the normal tests decide).
  bool _LowerEmerging(int n) const
  {
    if (mSinceOnset >= gTune.emergeFrames * mFS)
      return false;
    static constexpr int kIv[5] = {12, 19, 24, 28, 31};
    const float f0 = MidiToHz((float)n);
    const float pn = _Partial(f0, kRaw, 0);
    const int lo = std::max(0, mSet.lowNote);
    for (int i = 0; i < 5; i++)
    {
      const int m = n - kIv[i];
      if (m < lo)
        break;
      // the fast window can't tell the subharmonic from the note's own lobe below ~130 Hz apart
      if (mNotes[m].on || f0 - f0 / (i + 2) < 130.0f)
        continue;
      if (_Partial(f0 / (i + 2), kRaw, 0) > 0.25f * pn)
        return true;
    }
    return false;
  }

  void _Cancel(float f0)
  {
    // Remove the note's partials from every resolution's residual (spectral smoothness:
    // remove at most ~the local average so a shared partial keeps what another note adds).
    // cancel well past the 16 partials used for scoring: a low string's upper partials
    // (boosted by a pickup's resonance) would otherwise be left to form high ghost notes
    const int H = std::max(1, std::min(kCancelH, (int)(4500.0f / f0)));
    float amp[kCancelH + 2] = {};
    for (int h = 1; h <= H; h++)
      amp[h] = _Partial(f0 * h, kResidual);
    for (int h = 1; h <= H; h++)
    {
      const float a = amp[h];
      if (a <= 0.0f)
        continue;
      // expected amplitude from the neighbouring partials (excluding this one): energy above
      // that belongs to another note sharing the partial (octaves, fifths) and is kept
      float expect;
      if (h == 1)
        expect = H > 1 ? std::max(amp[2], a * 0.5f) : a;
      else if (h == H)
        expect = amp[h - 1];
      else
        expect = 0.5f * (amp[h - 1] + amp[h + 1]);
      const float q = std::min(1.0f, std::max(1.1f * expect / a, 0.75f));
      for (int r = 0; r < kNumRes; r++)
      {
        Res& R = mRes[r];
        const float bin = f0 * h * R.p / mFa;
        if (bin < 1.0f || bin > R.p / 2 - 6)
          continue;
        // find the partial's peak within the +-25 cent tolerance, then scale its whole lobe
        // (out to the valleys on each side) so no shoulders are left for other candidates
        const float w = std::max(1.0f, bin * 0.0145f);
        const int k0 = std::max(1, (int)std::floor(bin - w));
        const int k1 = std::min(R.p / 2 - 2, (int)std::ceil(bin + w));
        int kp = k0;
        for (int k = k0; k <= k1; k++)
          if (R.res[k] > R.res[kp])
            kp = k;
        if (R.res[kp] <= 0.0f)
          continue;
        int a0 = kp, a1 = kp;
        while (a0 > 1 && kp - a0 < 10 && R.res[a0 - 1] > 0.0f && R.res[a0 - 1] <= R.res[a0])
          a0--;
        while (a1 < R.p / 2 - 2 && a1 - kp < 10 && R.res[a1 + 1] > 0.0f && R.res[a1 + 1] <= R.res[a1])
          a1++;
        for (int k = a0; k <= a1; k++)
          R.res[k] *= (1.0f - q);
      }
    }
  }

  // Refine a note's frequency from its partials (parabolic peak interpolation on the raw
  // spectrum), searching within +-range semitones of the guess. Returns 0 if nothing usable.
  float _RefineHz(float guessHz, float rangeSemis) const
  {
    // one estimate per partial, then keep the frequency that the most (weighted) partials agree
    // on: a single neighbouring note's partial can't drag the pitch away on its own
    float est[6], wt[6];
    int ne = 0;
    const int H = std::min(6, _NumHarm(guessHz));
    const float span = std::pow(2.0f, rangeSemis / 12.0f) - 1.0f;
    for (int h = 1; h <= H; h++)
    {
      const float f = guessHz * h;
      const int r = ResFor(f);
      const Res& R = mRes[r];
      const float bin = f * R.p / mFa;
      const float w = std::max(1.0f, bin * span);
      const int k0 = std::max(1, (int)std::floor(bin - w));
      const int k1 = std::min(R.p / 2 - 2, (int)std::ceil(bin + w));
      int kp = k0;
      for (int k = k0; k <= k1; k++)
        if (R.mag[k] > R.mag[kp])
          kp = k;
      if (kp == k0 || kp == k1 || R.wht[kp] <= 0.0f)
        continue; // not a real peak inside the range
      const float ym = std::log(R.mag[kp - 1] + 1e-12f), y0 = std::log(R.mag[kp] + 1e-12f),
                  yp = std::log(R.mag[kp + 1] + 1e-12f);
      const float den = ym - 2.0f * y0 + yp;
      const float dlt = (den < 0.0f) ? std::clamp(0.5f * (ym - yp) / den, -0.5f, 0.5f) : 0.0f;
      est[ne] = (kp + dlt) * mFa / R.p / h;
      wt[ne] = R.wht[kp] * std::sqrt((float)h);
      ne++;
    }
    if (ne == 0)
      return 0.0f;
    int best = -1, bestCount = 0;
    float bestSupport = 0.0f;
    for (int i = 0; i < ne; i++)
    {
      float support = 0.0f;
      int count = 0;
      for (int j = 0; j < ne; j++)
        if (std::fabs(1200.0f * std::log2(est[j] / est[i])) < 20.0f)
        {
          support += wt[j];
          count++;
        }
      if (support > bestSupport)
      {
        bestSupport = support;
        best = i;
        bestCount = count;
      }
    }
    // need two agreeing partials unless the note only has one usable partial
    if (bestCount < 2 && H >= 2)
      return 0.0f;
    float sum = 0.0f, ws = 0.0f;
    for (int j = 0; j < ne; j++)
      if (std::fabs(1200.0f * std::log2(est[j] / est[best])) < 20.0f)
      {
        sum += est[j] * wt[j];
        ws += wt[j];
      }
    return sum / ws;
  }

  void _Analyze(int offset, std::vector<NoteEvent>& events)
  {
#ifdef PT_PROF
    double _pt = ProfNow();
#endif
    // 1. spectra + whitening (the long 64 ms window changes slowly: refresh it every other hop)
    mFrame++;
    for (int r = 0; r < kNumRes; r++)
    {
      Res& R = mRes[r];
      if (kSkipLong && r == kNumRes - 1 && (mFrame & 1) && mFrame > 2)
      {
        R.res = R.wht;
        continue;
      }
      const int start = (mRingPos - R.n) & (kRing - 1);
      for (int i = 0; i < R.n; i++)
        R.in[i] = mRing[(start + i) & (kRing - 1)] * R.win[i];
      R.fft.ForwardReal(R.in.data(), R.n, R.buf.data());
      PROF_MARK(0)
      const int half = R.p / 2;
      for (int k = 0; k < half; k++)
      {
        const float re = R.buf[k].real(), im = R.buf[k].imag();
        R.mag[k] = std::sqrt(re * re + im * im) * R.norm;
      }
      // spectral floor = smoothed local minimum (the valleys between partials), so dense chord
      // spectra don't raise it the way a local mean would. Running min over 2M+1 bins in O(N)
      // (van Herk / Gil-Werman: block prefix and suffix minima).
      const int M = 6, Wn = 2 * M + 1;
      {
        std::vector<float>& pre = R.mn;  // prefix minima within blocks of Wn
        std::vector<float>& suf = R.suf; // suffix minima within blocks of Wn
        for (int k = 0; k < half; k++)
          pre[k] = (k % Wn == 0) ? R.mag[k] : std::min(pre[k - 1], R.mag[k]);
        for (int k = half - 1; k >= 0; k--)
          suf[k] = (k == half - 1 || (k + 1) % Wn == 0) ? R.mag[k] : std::min(suf[k + 1], R.mag[k]);
        for (int k = 0; k < half; k++)
        {
          const int a = std::max(0, k - M), b = std::min(half - 1, k + M);
          // [a, b] spans at most two blocks: suffix of a's block + prefix of b's block
          R.res[k] = (a / Wn == b / Wn) ? ((a % Wn == 0) ? pre[b] : (b == half - 1 || (b + 1) % Wn == 0) ? suf[a] : _MinRange(R.mag, a, b))
                                        : std::min(suf[a], pre[b]);
        }
      }
      R.pre[0] = 0.0;
      R.pre2[0] = 0.0;
      for (int k = 0; k < half; k++)
      {
        R.pre[k + 1] = R.pre[k] + R.res[k];
        R.pre2[k + 1] = R.pre2[k] + R.mag[k];
      }
      float specMax = 0.0f;
      for (int k = 1; k < half; k++)
        specMax = std::max(specMax, R.mag[k]);
      const float minFloor = specMax * 0.005f; // -46 dB: same behaviour at any input level
      for (int k = 0; k < half; k++)
      {
        const int a = std::max(0, k - M), b = std::min(half, k + M + 1);
        const float fl = std::max((float)((R.pre[b] - R.pre[a]) / (b - a)), minFloor);
        // broad envelope (+-1/6 octave) for tilt compression: a low note's many partials then
        // outweigh one loud upper partial, without boosting tiny peaks in clean regions
        const int W = std::max(12, (int)(0.12f * k));
        const int ea = std::max(0, k - W), eb = std::min(half, k + W + 1);
        const float env = std::max((float)((R.pre2[eb] - R.pre2[ea]) / (eb - ea)), 1e-9f);
        const float above = R.mag[k] - 3.0f * fl;
        R.wht[k] = above > 0.0f ? above * std::exp2(-0.67f * std::log2(env)) : 0.0f;
        R.ton[k] = std::max(0.0f, above);
      }
      R.res = R.wht;
      PROF_MARK(1)
    }
    // level of the newest 16 ms
    {
      float e = 0.0f;
      for (int i = 0; i < 256; i++)
      {
        const float x = mRing[(mRingPos - 1 - i) & (kRing - 1)];
        e += x * x;
      }
      mLevelDb = 10.0f * std::log10(e / 256.0f + 1e-20f) + 3.0f; // ~peak of a sine
      float minRecent = mLevelDb;
      for (int i = 0; i < mHistN; i++)
        minRecent = std::min(minRecent, mLevelHist[i]);
      for (int i = mHistN - 1; i > 0; i--)
        mLevelHist[i] = mLevelHist[i - 1];
      mLevelHist[0] = mLevelDb;
      mOnset = (mLevelDb - minRecent) > gTune.onsetDb;
      mSinceOnset = mOnset ? 0 : std::min(mSinceOnset + 1, 1000);
    }
    // attack mode: right after a pick the slow window hasn't built its peaks yet, so read the
    // partials from faster windows (they only need to separate the note's own partials)
    {
      int sounding = 0;
      for (const auto& s : mNotes)
        sounding += s.on ? 1 : 0;
      const bool attack = gTune.attackK > 0.0f && !mBlend && mSinceOnset < gTune.attackFrames * mFS && sounding <= gTune.attackMaxOn;
      mK = attack ? gTune.attackK : gTune.needK;
      mTabSel = attack ? 1 : 0;
    }

    // gate: absolute (noise) and relative to the loudest partial in this frame (so weak
    // leftovers behave the same at any input level)
    float frameMax = 0.0f;
    {
      const Res& R = mRes[kNumRes - 1];
      const int k0 = (int)(70.0f * R.p / mFa), k1 = (int)(5000.0f * R.p / mFa);
      for (int k = k0; k < k1; k++)
        frameMax = std::max(frameMax, R.mag[k]);
    }
    const float relGateDb = -30.0f + 10.0f * (0.5f - std::clamp(mSet.sensitivity, 0.0f, 1.0f)); // -35..-25 dB
    const float gate = std::max(DbToLin(mSet.gateDb), frameMax * DbToLin(relGateDb));
    const float sens = std::clamp(mSet.sensitivity, 0.0f, 1.0f);
    const float relToFirst = 0.30f - 0.24f * sens; // weakest accepted note vs strongest

    // 2. iterative estimation on the whitened spectrum
    std::array<bool, kMaxNote> det{};
    std::array<float, kMaxNote> detSal{};
    std::array<int8_t, kMaxNote> detPres{};
    std::array<bool, kMaxNote> detEst{}; // found by the estimator itself (not kept by hysteresis)
    float firstSal = 0.0f;
    const int lo = std::max(0, mSet.lowNote), hi = std::min(kMaxNote - 1, mSet.highNote);
    const int maxNotes = std::clamp(mSet.maxNotes, 1, 8);
    PROF_MARK(2)
    // saliences only fall as notes are cancelled, so last iteration's values are upper bounds:
    // re-score just the leader until it stays on top (lazy greedy, same result, far cheaper)
    std::array<float, kMaxNote> bound{};
    for (int n = lo; n <= hi; n++)
      bound[n] = _SalienceT(n, kResidual);
    for (int j = 0; j < maxNotes; j++)
    {
      int best = -1;
      for (int guard = 0; guard < 64; guard++)
      {
        int top = -1, second = -1;
        for (int n = lo; n <= hi; n++)
        {
          if (det[n])
            continue;
          if (top < 0 || bound[n] > bound[top])
          {
            second = top;
            top = n;
          }
          else if (second < 0 || bound[n] > bound[second])
            second = n;
        }
        if (top < 0 || bound[top] <= 0.0f)
          break;
        if (j > 0)
          bound[top] = _SalienceT(top, kResidual);
        if (second < 0 || bound[top] >= bound[second] || j == 0)
        {
          best = top;
          break;
        }
      }
      if (best < 0)
        break;
      // octave / twelfth error check: prefer the lower candidate if its own partials exist
      for (int guard = 0; guard < 3; guard++)
      {
        const float f0 = MidiToHz((float)best);
        const float ref = 0.15f * std::max(_Partial(f0, kResidual), _Partial(2.0f * f0, kResidual));
        bool moved = false;
        const int drops[2] = {12, 19};
        const int divs[2] = {2, 3};
        for (int d = 0; d < 2 && !moved; d++)
        {
          const int m = best - drops[d];
          if (m < lo || det[m])
            continue;
          int tested = 0;
          const int got = _ExtraEvidence(f0, divs[d], std::max(ref, gate), &tested);
          // the lower note's own fundamental must be there too: in chords the other odd
          // partials are often just other chord tones (a triad = partials 3 and 5 of a root)
          const float fl = f0 / divs[d];
          // (a magnetic pickup often leaves a low string's fundamental weaker than its 2nd
          // partial: when every unshared partial is there, a weak fundamental is enough)
          const float fk = (mSet.strictOctaves && got == tested && tested >= 3) ? 0.5f : 2.0f;
          const bool fund = _Partial(fl, kResidual) > fk * std::max(ref, gate)
                            && (divs[d] == 2 || _Partial(2.0f * fl, kResidual) > std::max(ref, gate));
          if (fund && tested >= 2 && got * 4 >= tested * 3)
          {
            best = m;
            moved = true;
          }
        }
        // (strict) an upper partial of a lower string whose fundamental is still forming in the
        // slow window: take the lower string now instead of letting its partial take a slot
        if (!moved && gTune.harmDrop && mSet.strictOctaves)
        {
          const int m = _HarmonicOf(best, det, false);
          if (m >= lo && !det[m])
          {
            best = m;
            moved = true;
          }
        }
        if (!moved)
          break;
      }
      const float f0 = MidiToHz((float)best);
      const float s = _SalienceT(best, kResidual);
      float strongest = 0.0f, strongestRaw = 0.0f;
      int present = 0;
      const int H = mNH[best];
      for (int h = 1; h <= H; h++)
      {
        strongest = std::max(strongest, _PartialT(best, h, kResidual));
        strongestRaw = std::max(strongestRaw, _PartialT(best, h, kRaw));
      }
      const int firstH = std::min(H, 4);
      for (int h = 1; h <= firstH; h++)
        if (_PartialT(best, h, kResidual) > 0.12f * strongest)
          present++;
      if (j == 0)
        firstSal = s;
      // a real string note shows several of its first partials; a lone peak does not count
      const bool ok = strongestRaw > gate && s >= relToFirst * firstSal && (present >= 2 || firstH < 2);
#ifdef PT_TRACE
      if (mTrace)
      {
        printf("  it%d note %d sal %.3g (rel %.2f) present %d raw %.2g ok %d | p:", j, best, s, firstSal > 0 ? s / firstSal : 0, present, strongestRaw, ok);
        for (int h = 1; h <= std::min(H, 8); h++) printf(" %.2g", _Partial(f0 * h, kResidual));
        printf("\n");
      }
#endif
      if (!ok)
        break;
      det[best] = true;
      detSal[best] = s;
      detPres[best] = (int8_t)present;
      detEst[best] = true;
      _Cancel(f0);
    }

    PROF_MARK(3)
    // hysteresis: a sounding note stays detected with less evidence (chord partial overlap
    // makes the iterative pick drop it for a frame or two)
    for (int n = lo; n <= hi; n++)
    {
      NoteState& S = mNotes[n];
      if (!S.on || det[n])
        continue;
      // young notes get a stricter bar, so attack ghosts still die quickly
      const bool young = S.age < kSettleFrames;
      const float relK = young ? 0.7f : 0.4f, resK = young ? 0.5f : 0.25f;
      const float f0 = S.trackHz > 0.0f ? S.trackHz : MidiToHz((float)n);
      const float s = _Salience(f0, kResidual);
      float raw = 0.0f;
      for (int h = 1; h <= std::min(4, _NumHarm(f0)); h++)
        raw = std::max(raw, _Partial(f0 * h, kRaw));
      if (raw > gate && s >= relK * relToFirst * firstSal && s >= resK * S.salRes)
      {
        det[n] = true;
        detSal[n] = s;
        _Cancel(f0);
      }
    }
    PROF_MARK(4)
    mLastDet = det;
    // 3. note state machine
    // credible new notes: counting towards a note-on (passed every check last frame) or just started
    int newDetCount = 0;
    for (int n = lo; n <= hi; n++)
      newDetCount += ((det[n] && !mNotes[n].on && mNotes[n].seen > 0) || (mNotes[n].on && mNotes[n].age < gTune.repickWait * mFS)) ? 1 : 0;
    for (int n = lo; n <= hi; n++)
    {
      NoteState& S = mNotes[n];
      if (!S.on)
      {
        S.offAge = std::min(S.offAge + 1, 1000);
        // a frame without evidence: a chord's overlapping partials can hide a real note for a
        // frame, so a short gap pauses the count instead of restarting it
        auto miss = [&]() {
          if (S.seen > 0 && S.gap < gTune.gapTol)
            S.gap++;
          else
          {
            S.seen = 0;
            S.gap = 0;
          }
        };
        mWhy[n] = 1;
        if (!det[n])
        {
          miss();
          continue;
        }
        mWhy[n] = 2;
        // suppressed if an active note is bent onto this pitch
        bool bent = false;
        if (mSet.bendTracking)
          for (int m = std::max(lo, n - 3); m <= std::min(hi, n + 3); m++)
            if (m != n && mNotes[m].on && mNotes[m].age > kSettleFrames && mNotes[m].trackHz > 0.0f
                && std::fabs(HzToMidi(mNotes[m].trackHz) - (float)n) < 0.6f)
              bent = true;
        if (bent)
        {
          S.seen = 0;
          continue;
        }
        // the spectral peak must sit on this note (fake peaks from unresolved low partials don't).
        // Nearest note, not +-30 cents: a low string's first frames read sharp in the long window
        // (its attack sits on the window's falling edge), which cost ~6 ms on every low note.
        const float f0 = MidiToHz((float)n);
        const float hz = _RefineHz(f0, 0.5f);
        mWhy[n] = 3;
        if (hz <= 0.0f || (int)std::lround(HzToMidi(hz)) != n)
        {
          miss();
          continue;
        }
        // an upper partial of a lower string (pickups make these loud) is not a note
        if (mSet.strictOctaves)
        {
          mWhy[n] = _HarmonicOf(n, det, false) >= 0 ? 4 : (S.harmBlock && !mOnset && S.offAge < gTune.blockMax * mFS) ? 5 : _LowerEmerging(n) ? 6 : 0;
          if (mWhy[n])
          {
            S.seen = 0;
            continue;
          }
        }
        mWhy[n] = 7;
        S.harmBlock = false;
        // a note that only just ended needs longer evidence to return (no flicker), unless the
        // string was picked again
        // (2 frames = 4 ms of agreement; low strings used to need 3)
        const bool returning = S.offAge < gTune.retAge * mFS && !mOnset;
        const int need = gTune.needFrames + (returning ? gTune.retExtra * mFS : 0);
        // strong, complete, in-tune evidence needs only one frame
        const bool fast = !returning && detPres[n] >= gTune.fastPresent && detSal[n] >= gTune.fastRel * firstSal
                          && std::fabs(1200.0f * std::log2(hz / f0)) < gTune.fastCents;
        S.gap = 0;
        if (++S.seen >= need || fast)
          _NoteOn(n, detSal[n], offset, events);
      }
      else
      {
        S.age++;
        // follow the pitch (bends / vibrato)
        const float guess = S.trackHz > 0.0f ? S.trackHz : MidiToHz((float)n);
        const float hz = _RefineHz(guess, S.age < 3 * mFS ? 0.5f : 1.0f);
        if (hz > 0.0f && std::fabs(HzToMidi(hz) - (float)n) < 24.0f)
          S.trackHz = mKeep06 * S.trackHz + (1.0f - mKeep06) * hz;
        S.centsSm = 100.0f * (HzToMidi(S.trackHz) - (float)n);
        // early correction: the attack was labelled a semitone off; move it to the right note
        if (!S.relabelled && S.age >= 3 * mFS && S.age <= kSettleFrames && std::fabs(S.centsSm) > 55.0f)
        {
          const int m = (int)std::lround(HzToMidi(S.trackHz));
          if (m != n && m >= lo && m <= hi && !mNotes[m].on && det[m])
          {
#ifdef PT_TRACE
            if (mTraceEvents) printf("  [relabel %d -> %d cents %.0f]\n", n, m, S.centsSm);
#endif
            const int vel = S.velocity;
            const float tr = S.trackHz;
            _NoteOff(n, offset, events);
            _NoteOnVel(m, S.salPeak, vel, offset, events);
            mNotes[m].relabelled = true;
            mNotes[m].trackHz = tr;
            mNotes[m].age = S.age;
            continue;
          }
        }
        const float salNow = _Salience(S.trackHz, kWhite);
        // re-pick: big rise against the recent minimum
        float recentMin = salNow;
        for (int i = 0; i < mHistN; i++)
          recentMin = std::min(recentMin, S.hist[i]);
        for (int i = mHistN - 1; i > 0; i--)
          S.hist[i] = S.hist[i - 1];
        S.hist[0] = salNow;
        S.salPeak = std::max(S.salPeak * mDecPeak, salNow);
        // ...or the note's real level (not the flattened spectrum, where a 10 dB harder pick
        // looks like ~3 dB) jumps against its recent minimum at a pick
        // (floored: a damped note has no tonal part left, and -inf would let any trace count)
        const float lvl = std::max(gTune.repickFloorDb, gTune.repickOwn ? _NoteOwnLevelDb(n) : _NoteLevelDb(n));
        float lvlMin = lvl;
        for (int i = 0; i < mHistN; i++)
          lvlMin = std::min(lvlMin, S.lvlHist[i]);
        for (int i = mHistN - 1; i > 0; i--)
          S.lvlHist[i] = S.lvlHist[i - 1];
        S.lvlHist[0] = lvl;
        S.lvlPeak = std::max(S.lvlPeak, lvl);
        // (not during the note's own attack: the fast window is still filling for ~30 ms; and a
        // real re-pick comes back near the note's earlier level, leakage from another pick doesn't)
        const bool levelRepick = gTune.repickDb > 0.0f && S.age > gTune.repickAge * mFS && lvl > lvlMin + gTune.repickDb
                                 && lvl > S.lvlPeak - gTune.repickBelowPeakDb && detEst[n] && detSal[n] >= gTune.repickRel * firstSal;
        // a level re-pick waits a few frames: if another new note shows up meanwhile, the jump was
        // that note's pick leaking into this one (a damped note followed by a new one), not a re-pick
        bool fireLevelRepick = false;
        if (S.repickWait > 0)
        {
          if (newDetCount > 0)
            S.repickWait = 0;
          else if (--S.repickWait == 0)
            fireLevelRepick = true;
        }
        else if (S.age > 8 * mFS && mOnset && det[n] && levelRepick)
        {
          if (gTune.repickWait <= 0)
            fireLevelRepick = true;
          else
            S.repickWait = gTune.repickWait * mFS;
        }
        if (fireLevelRepick || (S.age > 8 * mFS && mOnset && det[n] && salNow > 3.0f * recentMin && recentMin > 0.0f))
        {
          S.repickWait = 0;
#ifdef PT_TRACE
          if (mTraceEvents) printf("  [repick %d sal %.3g min %.3g lvl %.1f min %.1f level %.1f]\n", n, salNow, recentMin, lvl, lvlMin, mLevelDb);
#endif
          _NoteOff(n, offset, events);
          _NoteOn(n, salNow, offset, events);
          continue;
        }
        S.miss = det[n] ? 0 : S.miss + 1;
        if (det[n])
          S.salRes = std::max(S.salRes * mDecRes, detSal[n]);
        const float relOff = 0.10f - 0.06f * sens;
        S.weak = (salNow < relOff * S.salPeak || _Partial(S.trackHz, kRaw) + _Partial(2 * S.trackHz, kRaw) < gate) ? S.weak + 1 : 0;
        // a note that turns out to be an upper partial of a sounding lower string goes
        S.harm = (mSet.strictOctaves && _HarmonicOf(n, det, true) >= 0) ? S.harm + 1 : 0;
        if (S.harm >= (S.age < kSettleFrames ? 2 : 6) * mFS)
        {
          _NoteOff(n, offset, events);
          S.harmBlock = true; // stays out until the next pick (no on/off churn)
          continue;
        }
        // release when the estimator stops finding the note (judging by the full spectrum would
        // keep ghosts made of other notes' partials alive)
        // a note that is still loud (weak == 0) and only lost by the estimator right after another
        // string's pick (its attack masks ringing notes) gets longer before it is released
        const int missLimit = (S.weak == 0 && mSinceOnset < gTune.maskFrames * mFS) ? gTune.maskMiss * mFS : 8 * mFS;
        if (S.weak >= 5 * mFS || S.miss >= missLimit)
        {
#ifdef PT_TRACE
          if (mTraceEvents) printf("  [off %d weak %d miss %d]\n", n, S.weak, S.miss);
#endif
          _NoteOff(n, offset, events);
          continue;
        }
        if (mSet.bendTracking && std::fabs(S.centsSm - S.sentCents) >= 3.0f)
        {
          S.sentCents = S.centsSm;
          events.push_back({NoteEvent::kBend, n, 0, S.centsSm, offset});
        }
      }
    }
    PROF_MARK(5)
  }

  // How hard the note was picked, in dBFS (about the peak level of that note played alone): its
  // own partials, read from the fast attack-mode windows (they only need to separate the note's
  // own partials, and they have caught the attack by the time the note goes on). The whitened
  // spectrum the tracker uses is flattened on purpose, so it can't be used for this.
  float _NoteLevelDb(int n) const
  {
    // every partial up to ~5 kHz: a low string's level sits mostly in its upper partials
    // (a pickup boosts them), so its first few alone would read it far too quiet
    float e = 0.0f;
    const float f0 = MidiToHz((float)n);
    const int H = std::max(1, std::min(kCancelH, (int)(5000.0f / f0)));
    for (int h = 1; h <= H; h++)
    {
      const float f = f0 * h;
      const float a = _PartialR(f, kRaw, gTune.velRes >= 0 ? gTune.velRes : ResForK(f, gTune.attackK > 0.0f ? gTune.attackK : gTune.needK));
      e += a * a;
    }
    return 10.0f * std::log10(e + 1e-20f) + gTune.velCalDb;
  }

  // Level of this note only, for re-pick detection: its partials from 1 to 5 kHz read from
  // windows that separate semitones (there those are still the fast 16/32 ms ones), so a
  // neighbouring note's pick doesn't leak in the way it does in the attack-mode windows.
  float _NoteOwnLevelDb(int n) const
  {
    float e = 0.0f;
    const float f0 = MidiToHz((float)n);
    const int H = std::max(1, std::min(kCancelH, (int)(5000.0f / f0)));
    for (int h = 1; h <= H; h++)
    {
      const float f = f0 * h;
      if (f < gTune.repickLoHz)
        continue;
      const int r = ResForK(f, gTune.needK);
      if (r > 1)
        continue;
      const float a = _PartialR(f, kTonal, r);
      e += a * a;
    }
    return 10.0f * std::log10(e + 1e-20f);
  }

  void _NoteOn(int n, float sal, int offset, std::vector<NoteEvent>& events)
  {
    // fixed scale: velTopDb -> 127, velRangeDb below it -> 1 (the same pick always gives the
    // same velocity; 0.3.x compared with a reference that faded in ~70 ms, so most notes were 127)
    const float db = _NoteLevelDb(n) - mSet.velTopDb;
    const float range = std::max(6.0f, mSet.velRangeDb);
    const int vel = std::clamp((int)std::lround(127.0f * (1.0f + db / range)), 1, 127);
    _NoteOnVel(n, sal, vel, offset, events);
  }

  void _NoteOnVel(int n, float sal, int vel, int offset, std::vector<NoteEvent>& events)
  {
    NoteState& S = mNotes[n];
    S.on = true;
    S.seen = 0;
    S.miss = 0;
    S.weak = 0;
    S.age = 0;
    S.salPeak = sal;
    S.hist.fill(sal);
    S.lvlHist.fill(std::max(gTune.repickFloorDb, gTune.repickOwn ? _NoteOwnLevelDb(n) : _NoteLevelDb(n)));
    S.lvlPeak = S.lvlHist[0];
    S.trackHz = MidiToHz((float)n);
    S.centsSm = 0.0f;
    S.sentCents = 0.0f;
    S.velocity = vel;
    S.relabelled = false;
    S.salRes = sal;
    S.harm = 0;
    events.push_back({NoteEvent::kOn, n, vel, 0.0f, offset});
  }

  void _NoteOff(int n, int offset, std::vector<NoteEvent>& events)
  {
    NoteState& S = mNotes[n];
    S.on = false;
    S.seen = 0;
    S.offAge = 0;
    S.trackHz = 0.0f;
    events.push_back({NoteEvent::kOff, n, 0, 0.0f, offset});
  }

  double mSR = 48000.0;
  float mFa = 16000.0f;
  int mHop = 32;
  int64_t mFrame = 0;
  static constexpr bool kSkipLong = false;
  Decimator mDec;
  Res mRes[kNumRes];
  std::vector<float> mRing;
  int mRingPos = 0;
  int mHopCount = 0;
  TrackerSettings mSet;
  std::array<NoteState, kMaxNote> mNotes{};
  float mLevelDb = -120.0f;
  std::array<bool, kMaxNote> mLastDet{};
public:
  bool mTrace = false;
  bool mTraceEvents = false;
  // why each note isn't on yet (test harness): 1 no detection, 2 bent, 3 pitch, 4 upper partial, 5 blocked, 6 lower emerging, 7 counting
  std::array<int8_t, kMaxNote> mWhy{};
private:
  std::array<float, 12> mLevelHist{};
  bool mOnset = false;
  int mSinceOnset = 1000; // analysis frames since the last pick
  int mFS = 1;       // analysis frames per 2 ms
  int mHistN = 6;
  float mKeep06 = 0.6f, mDecPeak = 0.9995f, mDecRes = 0.995f;
};

} // namespace polytrack
