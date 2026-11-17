// Harmonizer test: synthetic vowel (glottal pulses through formant resonators) -> one key at a time,
// writes output for numpy pitch analysis. Also a chord render.
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cmath>
#include <chrono>
#include "Harmonizer.h"
using namespace polytrack;
static const double SR = 48000.0;

struct Reso { float a1, a2, b0, y1 = 0, y2 = 0; void Set(float f, float bw) { float r = std::exp(-3.14159f * bw / SR); a1 = 2 * r * std::cos(2 * 3.14159f * f / SR); a2 = -r * r; b0 = 1 - r; } float Tick(float x) { float y = b0 * x + a1 * y1 + a2 * y2; y2 = y1; y1 = y; return y; } };

static std::vector<float> Vowel(float f0, int n, float vib = 0.0f)
{
  std::vector<float> v(n);
  Reso r1, r2, r3; r1.Set(700, 110); r2.Set(1220, 120); r3.Set(2600, 160); // "ah"
  double ph = 0;
  for (int i = 0; i < n; i++)
  {
    float f = f0 * (1.0f + vib * std::sin(2 * 3.14159f * 5.5f * i / SR));
    ph += f / SR;
    float g = 0;
    if (ph >= 1) { ph -= 1; g = 1; }
    float x = g;
    v[i] = 0.05f * (r1.Tick(x) * 1.0f + r2.Tick(x) * 0.5f + r3.Tick(x) * 0.25f);
  }
  return v;
}

int main(int argc, char** argv)
{
  const int N = (int)(1.5 * SR);
  float f0 = argc > 1 ? (float)atof(argv[1]) : 200.0f;
  auto in = Vowel(f0, N, argc > 2 ? (float)atof(argv[2]) : 0.0f);
  FILE* fi = fopen("h_in.raw", "wb"); fwrite(in.data(), 4, N, fi); fclose(fi);
  int keys[] = {48, 55, 60, 64, 67, 72, 79};
  for (int key : keys)
  {
    Harmonizer h; h.Init(SR);
    HarmonizerSettings s; s.detuneCents = 0; s.spread = 0; h.SetSettings(s);
    h.NoteOn(key, 127);
    std::vector<float> L(N, 0.0f), R(N, 0.0f);
    for (int i = 0; i < N; i += 128) h.Process(&in[i], &L[i], &R[i], std::min(128, N - i), 1.0f);
    char nm[32]; snprintf(nm, sizeof nm, "h_%d.raw", key);
    FILE* f = fopen(nm, "wb"); fwrite(L.data(), 4, N, f); fclose(f);
  }
  // chord + timing
  {
    Harmonizer h; h.Init(SR);
    HarmonizerSettings s; s.maxVoices = 12; h.SetSettings(s);
    int chord[] = {48, 55, 60, 64, 67, 71, 74, 77, 81, 84, 88, 91};
    for (int k : chord) h.NoteOn(k, 100);
    std::vector<float> L(N, 0.0f), R(N, 0.0f);
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < N; i += 128) h.Process(&in[i], &L[i], &R[i], std::min(128, N - i), 1.0f);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
    printf("12 voices: %.1f ms for %.1f s audio = %.2f%% of a core\n", ms, N / SR, ms / (N / SR * 1000) * 100);
    FILE* f = fopen("h_chord.raw", "wb"); for (int i = 0; i < N; i++) { fwrite(&L[i], 4, 1, f); fwrite(&R[i], 4, 1, f); } fclose(f);
  }
  return 0;
}
