// Writes a stereo test input: L = guitar (KS) sequence, R = vowel. Prints the expected note timeline.
#define main trk_main
#include "trk.cpp"
#undef main

struct Reso { float a1, a2, b0, y1 = 0, y2 = 0; void Set(float f, float bw) { float r = std::exp(-3.14159f * bw / (float)SR); a1 = 2 * r * std::cos(2 * 3.14159f * f / (float)SR); a2 = -r * r; b0 = 1 - r; } float Tick(float x) { float y = b0 * x + a1 * y1 + a2 * y2; y2 = y1; y1 = y; return y; } };

int main(int argc, char** argv)
{
  const char* path = argc > 1 ? argv[1] : "in.f32";
  float lvl = argc > 2 ? (float)atof(argv[2]) : -30.0f;
  const int N = (int)(4.5 * SR);
  std::vector<float> L(N, 0.0f), R(N, 0.0f);
  std::mt19937 rng(9);
  struct Hit { double t; std::vector<int> notes; double dur; };
  std::vector<Hit> hits = {{0.20, {40, 47, 52}, 0.7}, {1.00, {64}, 0.25}, {1.30, {67}, 0.25}, {1.60, {71}, 0.35}, {2.10, {45, 52, 57, 60, 64}, 1.2}, {3.50, {52}, 0.6}};
  for (auto& h : hits)
  {
    printf("guitar @%.2fs:", h.t);
    for (size_t k = 0; k < h.notes.size(); k++)
    {
      KS s; s.Pluck(MidiToHz((float)h.notes[k]), DbToLin(lvl), rng);
      int st = (int)((h.t + k * 0.008) * SR), en = (int)((h.t + h.dur) * SR);
      for (int i = st; i < N; i++)
      {
        float v = s.Tick();
        if (i > en) v *= std::exp(-(i - en) / (0.01f * (float)SR)); // palm mute
        L[i] += v;
      }
      printf(" %s", NoteName(h.notes[k]));
    }
    printf("\n");
  }
  // vowel 200 Hz on the right from 2.0 s to 4.0 s
  Reso r1, r2, r3; r1.Set(700, 110); r2.Set(1220, 120); r3.Set(2600, 160);
  double ph = 0;
  for (int i = (int)(2.0 * SR); i < (int)(4.0 * SR); i++)
  {
    ph += 200.0 / SR; float g = 0; if (ph >= 1) { ph -= 1; g = 1; }
    R[i] = 0.05f * (r1.Tick(g) + 0.5f * r2.Tick(g) + 0.25f * r3.Tick(g));
  }
  std::normal_distribution<float> nz(0, DbToLin(-95));
  FILE* f = fopen(path, "wb");
  for (int i = 0; i < N; i++) { float a = L[i] + nz(rng), b = R[i] + nz(rng); fwrite(&a, 4, 1, f); fwrite(&b, 4, 1, f); }
  fclose(f);
  return 0;
}
