// Harmonizer.h -- keyboard-chord vocal/guitar harmonizer (our own implementation).
//
// A monophonic input (a voice or a single-note guitar line) is re-pitched to every note held on
// a MIDI keyboard (or every note the guitar tracker hears), up to 12 voices.
//
//  * MonoPitch: YIN pitch detector on a ~16 kHz decimated copy (4 ms hop).
//  * PSOLA resynthesis: pitch marks one period apart on the input; each voice overlap-adds
//    two-period Hann grains at ITS period. The grain shape (formants) is kept, so a voice still
//    sounds like the singer. Formant shift resamples the grains.
//  * Latency is about two periods of the CURRENT input pitch (~9 ms at 220 Hz, ~4.5 ms at
//    440 Hz): each grain is taken from the newest fully-recorded pitch period.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "PolyTracker.h" // Decimator, MidiToHz

namespace polytrack
{

class MonoPitch
{
public:
  void Init(double sampleRate, float fMin = 65.0f, float fMax = 1100.0f)
  {
    mSR = sampleRate;
    const int D = std::max(1, (int)std::lround(sampleRate / 8000.0)); // voice pitch: 8 kHz is plenty
    mDec.Init(D);
    mFa = (float)(sampleRate / D);
    mTauMin = std::max(2, (int)(mFa / fMax));
    mTauMax = (int)(mFa / fMin) + 2;
    mW = mTauMax;
    mHop = std::max(16, (int)std::lround(mFa * 0.004));
    mRing.assign(kRing, 0.0f);
    mD.assign(mTauMax + 2, 0.0f);
    mPos = 0;
    mCount = 0;
    mVoiced = false;
    mPeriod = (float)(sampleRate / 150.0);
    mLevel = 0.0f;
  }

  void Process(const float* in, int n)
  {
    for (int i = 0; i < n; i++)
    {
      const float a = std::fabs(in[i]);
      mLevel = std::max(a, mLevel * 0.9995f);
      float y;
      if (!mDec.Push(in[i], y))
        continue;
      mRing[mPos] = y;
      mPos = (mPos + 1) & (kRing - 1);
      if (++mCount >= mHop)
      {
        mCount = 0;
        _Estimate();
      }
    }
  }

  bool Voiced() const { return mVoiced; }
  float PeriodSamples() const { return mPeriod; } // at the input sample rate
  float Hz() const { return (float)(mSR / mPeriod); }
  float Level() const { return mLevel; }

private:
  static constexpr int kRing = 2048;

  void _Estimate()
  {
    // difference function over the newest W samples against lags up to tauMax
    const int N = mW + mTauMax;
    const int start = (mPos - N) & (kRing - 1);
    mBuf.resize(N);
    float energy = 0.0f;
    for (int i = 0; i < N; i++)
    {
      mBuf[i] = mRing[(start + i) & (kRing - 1)];
      energy += mBuf[i] * mBuf[i];
    }
    if (energy / N < 1e-11f)
    {
      mVoiced = false;
      return;
    }
    mD[0] = 1.0f;
    float running = 0.0f;
    for (int tau = 1; tau <= mTauMax; tau++)
    {
      float d = 0.0f;
      const float* x = mBuf.data();
      const float* y = mBuf.data() + tau;
      for (int j = 0; j < mW; j++)
      {
        const float df = x[j] - y[j];
        d += df * df;
      }
      running += d;
      mD[tau] = running > 0.0f ? d * tau / running : 1.0f; // cumulative mean normalised
    }
    int best = -1;
    for (int tau = mTauMin; tau < mTauMax; tau++)
    {
      if (mD[tau] < 0.15f)
      {
        while (tau + 1 < mTauMax && mD[tau + 1] < mD[tau])
          tau++;
        best = tau;
        break;
      }
    }
    if (best < 0)
    {
      // no clear dip: take the global minimum if it's still fairly periodic
      float mn = 1e9f;
      for (int tau = mTauMin; tau < mTauMax; tau++)
        if (mD[tau] < mn)
        {
          mn = mD[tau];
          best = tau;
        }
      if (mn > 0.35f)
      {
        mVoiced = false;
        return;
      }
    }
    float t = (float)best;
    if (best > 1 && best < mTauMax)
    {
      const float a = mD[best - 1], b = mD[best], c = mD[best + 1];
      const float den = a - 2.0f * b + c;
      if (den > 0.0f)
        t += std::clamp(0.5f * (a - c) / den, -0.5f, 0.5f);
    }
    const float period = t * (float)(mSR / mFa);
    // smooth small changes, jump on big ones (new note)
    if (mVoiced && std::fabs(period / mPeriod - 1.0f) < 0.03f)
      mPeriod = 0.5f * mPeriod + 0.5f * period;
    else
      mPeriod = period;
    mVoiced = true;
  }

  double mSR = 48000.0;
  float mFa = 16000.0f;
  Decimator mDec;
  int mTauMin = 15, mTauMax = 250, mW = 250, mHop = 64;
  std::vector<float> mRing, mBuf, mD;
  int mPos = 0, mCount = 0;
  bool mVoiced = false;
  float mPeriod = 320.0f;
  float mLevel = 0.0f;
};

struct HarmonizerSettings
{
  int maxVoices = 8;        // 1..12
  float formantSemis = 0.0f; // -12..12
  float detuneCents = 6.0f;  // per-voice random spread
  float spread = 0.6f;       // stereo spread 0..1
  float attackMs = 10.0f;
  float releaseMs = 120.0f;
  float unvoicedGain = 0.5f; // consonants / breath on the harmony voices
  float gateDb = -60.0f;     // below this input level the harmony is silent
};

class Harmonizer
{
public:
  static constexpr int kMaxVoices = 12;

  void Init(double sampleRate)
  {
    mSR = sampleRate;
    mPitch.Init(sampleRate);
    mIn.assign(kRing, 0.0f);
    mOutL.assign(kRing, 0.0f);
    mOutR.assign(kRing, 0.0f);
    mT = 0;
    mNextMark = 0.0;
    mMarks.fill({-1000000, 1.0f});
    mMarkHead = 0;
    for (auto& v : mVoices)
      v = Voice();
    std::mt19937 rng(77);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (int i = 0; i < kMaxVoices; i++)
      mDetuneRand[i] = u(rng);
  }

  void SetSettings(const HarmonizerSettings& s) { mSet = s; }

  // Chord notes (MIDI keys or tracked guitar notes).
  void NoteOn(int note, int velocity)
  {
    // retrigger an existing voice for this note
    for (auto& v : mVoices)
      if (v.active && v.note == note)
      {
        v.held = true;
        v.vel = velocity / 127.0f;
        return;
      }
    // count held voices
    int held = 0;
    for (auto& v : mVoices)
      if (v.active && v.held)
        held++;
    if (held >= std::clamp(mSet.maxVoices, 1, kMaxVoices))
    {
      // steal the oldest held voice
      int oldest = -1;
      for (int i = 0; i < kMaxVoices; i++)
        if (mVoices[i].active && mVoices[i].held && (oldest < 0 || mVoices[i].age > mVoices[oldest].age))
          oldest = i;
      if (oldest >= 0)
        mVoices[oldest].held = false;
    }
    int slot = -1;
    for (int i = 0; i < kMaxVoices; i++)
      if (!mVoices[i].active)
      {
        slot = i;
        break;
      }
    if (slot < 0)
    {
      // all busy (some releasing): take the quietest releasing voice
      float lo = 2.0f;
      for (int i = 0; i < kMaxVoices; i++)
        if (!mVoices[i].held && mVoices[i].env < lo)
        {
          lo = mVoices[i].env;
          slot = i;
        }
      if (slot < 0)
        return;
    }
    Voice& v = mVoices[slot];
    v = Voice();
    v.active = true;
    v.held = true;
    v.note = note;
    v.vel = velocity / 127.0f;
    v.nextSyn = (double)mT;
    v.slot = slot;
    _UpdatePans();
  }

  void NoteOff(int note)
  {
    for (auto& v : mVoices)
      if (v.active && v.held && v.note == note)
        v.held = false;
  }

  void AllOff()
  {
    for (auto& v : mVoices)
      v.held = false;
  }

  bool InputVoiced() const { return mPitch.Voiced(); }
  float InputHz() const { return mPitch.Hz(); }
  int ActiveVoices() const
  {
    int n = 0;
    for (auto& v : mVoices)
      n += v.active ? 1 : 0;
    return n;
  }

  // in: mono source. Adds the harmony into outL/outR (scaled by gain).
  void Process(const float* in, float* outL, float* outR, int n, float gain)
  {
    mPitch.Process(in, n);
    const bool voiced = mPitch.Voiced();
    const float period = std::clamp(mPitch.PeriodSamples(), 16.0f, (float)(mSR / 50.0));
    const float unvPeriod = (float)(mSR / 200.0);
    const bool gateOpen = mPitch.Level() > DbToLin(mSet.gateDb);
    const float aCoef = 1.0f - std::exp(-1.0f / (0.001f * std::max(1.0f, mSet.attackMs) * (float)mSR));
    const float rCoef = 1.0f - std::exp(-1.0f / (0.001f * std::max(5.0f, mSet.releaseMs) * (float)mSR));
    const float formant = std::pow(2.0f, mSet.formantSemis / 12.0f);

    for (int i = 0; i < n; i++)
    {
      const int64_t t = mT;
      mIn[t & (kRing - 1)] = in[i];

      // analysis marks, one (input) period apart. Voiced marks sit on each period's main
      // peak (searched +-P/4 around the expected spot, a quarter period after it), so every
      // grain is centred on one pulse.
      if (mNextMark < (double)t - 2.0 * period)
        mNextMark = (double)t;
      if (voiced)
      {
        const float q = 0.25f * period;
        if ((double)t >= mNextMark + q)
        {
          const int64_t a = (int64_t)std::floor(mNextMark - q), b = t;
          int64_t best = a;
          float bv = -1e30f;
          for (int64_t k = a; k <= b; k++)
          {
            const float v = mIn[k & (kRing - 1)];
            if (v > bv)
            {
              bv = v;
              best = k;
            }
          }
          mMarkHead = (mMarkHead + 1) % kMarks;
          mMarks[mMarkHead] = {best, period, true};
          mNextMark = (double)best + period;
        }
      }
      else if ((double)t >= mNextMark)
      {
        mMarkHead = (mMarkHead + 1) % kMarks;
        mMarks[mMarkHead] = {(int64_t)std::llround(mNextMark), unvPeriod, false};
        mNextMark += unvPeriod;
      }

      // synthesis
      for (auto& v : mVoices)
      {
        if (!v.active)
          continue;
        const float target = v.held ? v.vel : 0.0f;
        v.env += (target - v.env) * (v.held ? aCoef : rCoef);
        v.age++;
        if (!v.held && v.env < 1e-4f)
        {
          v.active = false;
          continue;
        }
        // newest mark whose two-period grain is fully recorded
        const Mark* m = _NewestCompleteMark(t);
        if (!m)
          continue;
        const float Pa = m->period;
        const float Ho = Pa / formant; // output half-width
        if ((double)t + Ho + 1.0 < v.nextSyn)
          continue; // not yet time for this voice's next grain
        // exact fractional placement keeps the pitch exact; if we fell behind, restart now
        const double s = (v.nextSyn < (double)t + Ho - 1.0) ? (double)t + Ho : v.nextSyn;
        const float detune = std::pow(2.0f, mDetuneRand[v.slot] * mSet.detuneCents / 1200.0f);
        const float Pt = m->voiced ? (float)(mSR / (MidiToHz((float)v.note) * detune)) : Pa;
        const float g = (gateOpen ? 1.0f : 0.0f) * v.env * (m->voiced ? 1.0f : mSet.unvoicedGain)
                        * std::min(2.0f, 1.35f * std::sqrt(Pt / Ho)); // pulse-like grains add in power
        if (g > 0.0f)
          _AddGrain(*m, s, Ho, t, g * v.panL, g * v.panR);
        v.nextSyn = s + Pt;
      }

      const int64_t o = t & (kRing - 1);
      outL[i] += gain * mOutL[o];
      outR[i] += gain * mOutR[o];
      mOutL[o] = 0.0f;
      mOutR[o] = 0.0f;
      mT++;
    }
  }

private:
  static constexpr int kRing = 1 << 15;
  static constexpr int kMarks = 128;

  struct Mark
  {
    int64_t pos;
    float period;
    bool voiced = false;
  };

  struct Voice
  {
    bool active = false;
    bool held = false;
    int note = 60;
    float vel = 1.0f;
    float env = 0.0f;
    double nextSyn = 0.0;
    int64_t age = 0;
    int slot = 0;
    float panL = 0.707f, panR = 0.707f;
  };

  const Mark* _NewestCompleteMark(int64_t t) const
  {
    for (int k = 0; k < kMarks; k++)
    {
      const Mark& m = mMarks[(mMarkHead - k + kMarks) % kMarks];
      if (m.pos < -100000)
        return nullptr;
      if ((double)m.pos + m.period <= (double)t)
        return &m;
    }
    return nullptr;
  }

  void _AddGrain(const Mark& m, double s, float Ho, int64_t now, float gL, float gR)
  {
    const int h = (int)std::ceil(Ho);
    const int64_t sc = (int64_t)std::floor(s);
    const float frac = (float)(s - (double)sc);
    for (int j = -h; j <= h; j++)
    {
      const float x = ((float)j - frac) / Ho; // position in the grain, -1..1
      if (x <= -1.0f || x >= 1.0f)
        continue;
      const float w = 0.5f + 0.5f * std::cos(3.14159265f * x);
      const double src = (double)m.pos + (double)x * m.period; // formant: grain resampled
      const int64_t si = (int64_t)std::floor(src);
      const float sf = (float)(src - (double)si);
      const float a = mIn[si & (kRing - 1)], b = mIn[(si + 1) & (kRing - 1)];
      const float v = (a + (b - a) * sf) * w;
      if (sc + j < now)
        continue; // already played (only the window's ~0 edge)
      const int64_t o = (sc + j) & (kRing - 1);
      mOutL[o] += v * gL;
      mOutR[o] += v * gR;
    }
  }

  void _UpdatePans()
  {
    // spread the active voices across the stereo field by pitch (low left .. high right)
    std::array<int, kMaxVoices> idx{};
    int n = 0;
    for (int i = 0; i < kMaxVoices; i++)
      if (mVoices[i].active && mVoices[i].held)
        idx[n++] = i;
    std::sort(idx.begin(), idx.begin() + n, [&](int a, int b) { return mVoices[a].note < mVoices[b].note; });
    for (int k = 0; k < n; k++)
    {
      const float pos = n > 1 ? (float)k / (n - 1) * 2.0f - 1.0f : 0.0f; // -1..1
      const float p = pos * std::clamp(mSet.spread, 0.0f, 1.0f);
      const float ang = (p + 1.0f) * 0.25f * 3.14159265f;
      mVoices[idx[k]].panL = std::cos(ang);
      mVoices[idx[k]].panR = std::sin(ang);
    }
  }

  double mSR = 48000.0;
  HarmonizerSettings mSet;
  MonoPitch mPitch;
  std::vector<float> mIn, mOutL, mOutR;
  int64_t mT = 0;
  double mNextMark = 0.0;
  std::array<Mark, kMarks> mMarks{};
  int mMarkHead = 0;
  std::array<Voice, kMaxVoices> mVoices{};
  std::array<float, kMaxVoices> mDetuneRand{};
};

} // namespace polytrack
