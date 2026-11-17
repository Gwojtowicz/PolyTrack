// MonitorSynth.h -- a small built-in polyphonic synth so the tracked notes can be heard without
// routing MIDI anywhere (anti-aliased saw through a gentle low-pass, follows pitch bends).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace polytrack
{

class MonitorSynth
{
public:
  void Init(double sampleRate)
  {
    mSR = sampleRate;
    for (auto& v : mV)
      v = Voice();
  }

  void NoteOn(int note, int vel)
  {
    Voice* slot = nullptr;
    for (auto& v : mV)
      if (v.active && v.note == note)
        slot = &v;
    if (!slot)
      for (auto& v : mV)
        if (!v.active)
        {
          slot = &v;
          break;
        }
    if (!slot)
    {
      slot = &mV[0];
      for (auto& v : mV)
        if (v.env < slot->env)
          slot = &v;
    }
    slot->active = true;
    slot->held = true;
    slot->note = note;
    slot->vel = vel / 127.0f;
    slot->cents = 0.0f;
  }

  void NoteOff(int note)
  {
    for (auto& v : mV)
      if (v.active && v.note == note)
        v.held = false;
  }

  void Bend(int note, float cents)
  {
    for (auto& v : mV)
      if (v.active && v.note == note)
        v.cents = cents;
  }

  void BendAll(float cents)
  {
    for (auto& v : mV)
      if (v.active)
        v.cents = cents;
  }

  void AllOff()
  {
    for (auto& v : mV)
      v.held = false;
  }

  void Process(float* L, float* R, int n, float gain)
  {
    if (gain <= 0.0f)
    {
      for (auto& v : mV)
        if (!v.held)
          v.active = false;
      return;
    }
    const float aC = 1.0f - std::exp(-1.0f / (0.003f * (float)mSR));
    const float rC = 1.0f - std::exp(-1.0f / (0.15f * (float)mSR));
    const float lpC = 1.0f - std::exp(-2.0f * 3.14159265f * 2200.0f / (float)mSR);
    for (auto& v : mV)
    {
      if (!v.active)
        continue;
      const float f = 440.0f * std::pow(2.0f, ((float)v.note + v.cents / 100.0f - 69.0f) / 12.0f);
      const float dt = f / (float)mSR;
      for (int i = 0; i < n; i++)
      {
        v.env += ((v.held ? v.vel : 0.0f) - v.env) * (v.held ? aC : rC);
        v.ph += dt;
        if (v.ph >= 1.0f)
          v.ph -= 1.0f;
        float s = 2.0f * v.ph - 1.0f;
        // polyBLEP
        if (v.ph < dt)
        {
          const float t = v.ph / dt;
          s -= t + t - t * t - 1.0f;
        }
        else if (v.ph > 1.0f - dt)
        {
          const float t = (v.ph - 1.0f) / dt;
          s -= t * t + t + t + 1.0f;
        }
        v.lp += (s - v.lp) * lpC;
        const float y = v.lp * v.env * gain * 0.25f;
        L[i] += y;
        R[i] += y;
      }
      if (!v.held && v.env < 1e-4f)
        v.active = false;
    }
  }

private:
  struct Voice
  {
    bool active = false, held = false;
    int note = 60;
    float vel = 1.0f, env = 0.0f, ph = 0.0f, lp = 0.0f, cents = 0.0f;
  };
  double mSR = 48000.0;
  std::array<Voice, 16> mV{};
};

} // namespace polytrack
