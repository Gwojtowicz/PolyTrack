#define PT_TRACE
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <chrono>
#include "PolyTracker.h"
using namespace polytrack;
static const char* NN(int n) { static char b[8]; static const char* nm[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"}; snprintf(b, 8, "%s%d", nm[n % 12], n / 12 - 1); return b; }
int main(int argc, char** argv)
{
  FILE* f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); size_t n = ftell(f) / 8; fseek(f, 0, SEEK_SET);
  std::vector<float> st(n * 2); fread(st.data(), 4, n * 2, f); fclose(f);
  float g = DbToLin(argc > 2 ? (float)atof(argv[2]) : 12.0f);
  double t0 = argc > 3 ? atof(argv[3]) : 0, t1 = argc > 4 ? atof(argv[4]) : 99;
  std::vector<float> x(n); for (size_t i = 0; i < n; i++) x[i] = st[2 * i] * g;
  PolyTracker t; t.Init(48000); TrackerSettings s; s.sensitivity = 0.4f; t.SetSettings(s);
  std::vector<NoteEvent> ev; const int B = 128;
  auto c0 = std::chrono::high_resolution_clock::now(); double worst = 0;
  for (size_t i = 0; i < n; i += B)
  {
    double ms = i * 1000.0 / 48000; t.mTraceEvents = ms >= t0 * 1000 && ms <= t1 * 1000;
    ev.clear();
    auto b0 = std::chrono::high_resolution_clock::now();
    t.Process(&x[i], (int)std::min<size_t>(B, n - i), ev);
    double us = std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - b0).count();
    worst = std::max(worst, us / (B / 48.0) / 10.0);
    for (auto& e : ev) if (t.mTraceEvents && e.type != NoteEvent::kBend) printf("%8.1f %s %s\n", (i + e.offset) / 48.0, e.type ? "ON " : "OFF", NN(e.note));
  }
  double tot = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - c0).count();
  printf("tracker CPU avg %.2f%% worst block %.1f%%\n", tot / (n / 48.0) * 100, worst);
#ifdef PT_PROF
  printf("fft %.0f whiten %.0f noise/gate %.0f estimate %.0f hyster %.0f states %.0f us total\n", gProf[0], gProf[1], gProf[2], gProf[3], gProf[4], gProf[5]);
#endif
}
