// Sequence harness: one continuous take through ONE tracker (like real playing), so it tests what
// trk.cpp can't: notes that follow and stop each other, repeated picks, palm mutes, ringing
// arpeggios, and velocity against how hard each note was picked.
//
//   seq.exe [pickup=1] [verbose=0]      tuning env vars as trk.exe (FALL, ATTK, ...)
#define TRK_NO_MAIN
#include "trk.cpp"
#include <algorithm>
#include <cmath>

struct SeqNote
{
  int note;
  double t;   // pick time (s)
  double dur; // when the string is damped (s after the pick)
  float db;   // pick strength: peak level in dBFS (single note at this level)
};

struct SeqStats
{
  int expected = 0, ok = 0, missed = 0, extra = 0, longExtra = 0, hang = 0, wrongVel = 0;
  double latSum = 0;
  std::vector<double> lat;
  std::vector<std::pair<float, int>> vel; // (pick dB, velocity)
  void Add(const SeqStats& o)
  {
    expected += o.expected; ok += o.ok; missed += o.missed; extra += o.extra; longExtra += o.longExtra; hang += o.hang;
    latSum += o.latSum; lat.insert(lat.end(), o.lat.begin(), o.lat.end()); vel.insert(vel.end(), o.vel.begin(), o.vel.end());
  }
};

// velocity vs pick level: slope (velocity per dB), correlation, and the velocity spread
static void VelFit(const std::vector<std::pair<float, int>>& v, double& slope, double& r)
{
  slope = r = 0;
  if (v.size() < 3) return;
  double mx = 0, my = 0;
  for (auto& p : v) { mx += p.first; my += p.second; }
  mx /= v.size(); my /= v.size();
  double sxx = 0, syy = 0, sxy = 0;
  for (auto& p : v) { sxx += (p.first - mx) * (p.first - mx); syy += (p.second - my) * (p.second - my); sxy += (p.first - mx) * (p.second - my); }
  if (sxx > 0) slope = sxy / sxx;
  if (sxx > 0 && syy > 0) r = sxy / std::sqrt(sxx * syy);
}

static float g_pickupGain = 1.0f; // makes a pickup-model note peak at its nominal level

static void RunSeq(const char* label, std::vector<SeqNote> seq, SeqStats& tot, bool verbose)
{
  std::sort(seq.begin(), seq.end(), [](const SeqNote& a, const SeqNote& b) { return a.t < b.t; });
  std::mt19937 rng(777 + (int)seq.size());
  const int pre = (int)(0.3 * SR);
  double end = 0;
  for (auto& s : seq) end = std::max(end, s.t + s.dur);
  std::vector<float> buf(pre + (int)((end + 0.6) * SR), 0.0f);
  // each note is rendered alone (through the pickup model) and scaled so ITS output peaks at s.db:
  // "how loud that note came out of the guitar", which is what velocity should follow
  for (auto& s : seq)
  {
    KS k;
    k.Pluck(MidiToHz((float)s.note), 1.0f, rng);
    const int a = pre + (int)(s.t * SR), stop = pre + (int)((s.t + s.dur) * SR);
    const int last = std::min((int)buf.size(), stop + (int)(0.15 * SR));
    std::vector<float> one(std::max(0, last - a), 0.0f);
    // fretting hand lifts / palm mutes: the string dies away over ~30 ms (T60), with the decay
    // easing in over 5 ms. (Cutting the loop gain at once made a click that sounded like a pick.)
    const double rateEnd = std::log(0.001) / (0.03 * SR), easeN = 0.005 * SR;
    double env = 1.0;
    for (int i = a; i < last; i++)
    {
      if (i >= stop)
        env *= std::exp(rateEnd * std::min(1.0, (i - stop) / easeN));
      one[i - a] = (float)(k.Tick() * env);
    }
    ApplyPickup(one);
    // level = energy over the first 50 ms (+3 dB, so a sine reads its peak): loudness, not the
    // peak (a low string through a pickup is spiky; its peak says little about how loud it is)
    double e2 = 0;
    const int nE = std::min((int)one.size(), (int)(0.05 * SR));
    for (int i = 0; i < nE; i++) e2 += (double)one[i] * one[i];
    const float lvl = (float)std::sqrt(2.0 * e2 / std::max(1, nE));
    const float sc = DbToLin(s.db) / std::max(lvl, 1e-9f);
    for (int i = a; i < last; i++) buf[i] += one[i - a] * sc;
  }
  std::normal_distribution<float> g(0, DbToLin(-95));
  for (auto& x : buf) x += g(rng);

  PolyTracker t;
  t.Init(SR);
  { TrackerSettings ts; ts.strictOctaves = g_strict != 0; if (const char* e = getenv("SENS")) ts.sensitivity = (float)atof(e); t.SetSettings(ts); }
  struct Ev { NoteEvent e; double ts; };
  std::vector<Ev> evs;
  std::vector<NoteEvent> ev;
  for (int i = 0; i < (int)buf.size(); i += 128)
  {
    ev.clear();
#ifdef PT_TRACE
    { const double ms = (i - pre) * 1000.0 / SR; float a = 1e9f, b = -1e9f; if (const char* e = getenv("TRACEMS")) sscanf(e, "%f,%f", &a, &b);
      const char* tl = getenv("TRACE"); t.mTraceEvents = tl && !strcmp(tl, label) && ms >= a && ms <= b; if (t.mTraceEvents) printf("-- %.1f ms level %.1f\n", ms, t.LevelDb()); }
#endif
    t.Process(&buf[i], std::min(128, (int)buf.size() - i), ev);
    for (auto& e : ev) evs.push_back({e, (i + e.offset - pre) / SR});
  }

  SeqStats st;
  st.expected = (int)seq.size();
  std::vector<bool> used(evs.size(), false);
  std::string log;
  for (size_t si = 0; si < seq.size(); si++)
  {
    const SeqNote& s = seq[si];
    int hit = -1;
    for (size_t j = 0; j < evs.size(); j++)
      if (!used[j] && evs[j].e.type == NoteEvent::kOn && evs[j].e.note == s.note && evs[j].ts >= s.t - 0.002 && evs[j].ts <= s.t + 0.12)
      { hit = (int)j; break; }
    char b[64];
    if (hit < 0)
    {
      st.missed++;
      snprintf(b, sizeof b, " [%s MISS]", NoteName(s.note));
      log += b;
      continue;
    }
    used[hit] = true;
    st.ok++;
    const double lat = (evs[hit].ts - s.t) * 1000;
    st.latSum += lat;
    st.lat.push_back(lat);
    st.vel.push_back({s.db, evs[hit].e.velocity});
    // note-off: should come within ~80 ms of the damping (or at the next pick of the same note)
    double off = 1e9;
    for (size_t j = hit + 1; j < evs.size(); j++)
      if (evs[j].e.note == s.note && (evs[j].e.type == NoteEvent::kOff || evs[j].e.type == NoteEvent::kOn)) { off = evs[j].ts; break; }
    double nextSame = 1e9;
    for (size_t k = si + 1; k < seq.size(); k++) if (seq[k].note == s.note) { nextSame = seq[k].t; break; }
    const double due = std::min(s.t + s.dur, nextSame);
    if (off > due + 0.08 && off < 1e8 && nextSame > s.t + s.dur + 0.08) st.hang++;
    snprintf(b, sizeof b, " %s@%.0f(v%d,%.0fdB)", NoteName(s.note), lat, evs[hit].e.velocity, s.db);
    if (verbose) log += b;
  }
  // unmatched note-ons = wrong notes; ones that last >= 50 ms are the audible ones
  for (size_t j = 0; j < evs.size(); j++)
  {
    if (used[j] || evs[j].e.type != NoteEvent::kOn) continue;
    st.extra++;
    double off = evs.back().ts;
    for (size_t k = j + 1; k < evs.size(); k++)
      if (evs[k].e.note == evs[j].e.note && evs[k].e.type != NoteEvent::kBend) { off = evs[k].ts; break; }
    if (off - evs[j].ts >= 0.05) st.longExtra++;
    if (verbose) { char b[48]; snprintf(b, sizeof b, " +%s@%.0f(%.0fms)", NoteName(evs[j].e.note), evs[j].ts * 1000, (off - evs[j].ts) * 1000); log += b; }
  }
  double slope, r;
  VelFit(st.vel, slope, r);
  printf("%-16s notes %2d ok %2d miss %d wrong %2d (long %d) hang %d | lat avg %4.1f | vel/dB %.2f r %.2f\n", label,
         st.expected, st.ok, st.missed, st.extra, st.longExtra, st.hang, st.ok ? st.latSum / st.ok : 0.0, slope, r);
  if (verbose && !log.empty()) printf("   %s\n", log.c_str());
  tot.Add(st);
}

int main(int argc, char** argv)
{
  g_pickup = argc > 1 ? atoi(argv[1]) : 1;
  const bool verbose = argc > 2 && atoi(argv[2]) != 0;
  ParseTuneEnv();
  // pickup calibration: an E3 picked at -20 dB peaks at -20 dBFS after the pickup model
  if (g_pickup)
  {
    std::mt19937 rng(5);
    KS k; k.Pluck(MidiToHz(52), DbToLin(-20), rng);
    std::vector<float> b((int)(0.5 * SR));
    for (auto& x : b) x = k.Tick();
    ApplyPickup(b);
    float pk = 1e-9f; for (float x : b) pk = std::max(pk, std::fabs(x));
    g_pickupGain = DbToLin(-20) / pk;
  }
  std::mt19937 rng(42);
  std::uniform_real_distribution<float> u(0, 1);
  auto lv = [&](float lo, float hi) { return lo + (hi - lo) * u(rng); };
  SeqStats tot;

  // 1. dynamics: the same notes picked at 5 strengths, in random order
  {
    std::vector<SeqNote> s;
    const float dbs[] = {-36, -30, -24, -18, -12};
    const int notes[] = {40, 45, 52, 59, 64, 69, 76};
    double t = 0;
    for (int rep = 0; rep < 2; rep++)
      for (int n : notes)
      {
        std::vector<float> d(dbs, dbs + 5);
        std::shuffle(d.begin(), d.end(), rng);
        for (float db : d) { s.push_back({n, t, 0.40, db}); t += 0.5; }
      }
    RunSeq("dynamics", s, tot, verbose);
  }
  // 2. scale on one string, eighths and sixteenths at 120 bpm (each note stops the last)
  {
    const int sc[] = {55, 57, 59, 60, 62, 64, 66, 67, 66, 64, 62, 60, 59, 57, 55};
    for (double step : {0.25, 0.125})
    {
      std::vector<SeqNote> s;
      double t = 0;
      for (int rep = 0; rep < 2; rep++)
        for (int n : sc) { s.push_back({n, t, step * 0.95, lv(-24, -16)}); t += step; }
      RunSeq(step > 0.2 ? "scale 8ths" : "scale 16ths", s, tot, verbose);
    }
  }
  // 3. low-string scale (E minor pentatonic), eighths
  {
    const int sc[] = {40, 43, 45, 47, 50, 52, 50, 47, 45, 43, 40};
    std::vector<SeqNote> s;
    double t = 0;
    for (int rep = 0; rep < 2; rep++)
      for (int n : sc) { s.push_back({n, t, 0.24, lv(-24, -16)}); t += 0.25; }
    RunSeq("low pentatonic", s, tot, verbose);
  }
  // 4. repeated picking of one note (each pick must give a new note-on)
  {
    std::vector<SeqNote> s;
    for (int i = 0; i < 12; i++) s.push_back({57, i * 0.15, 0.14, lv(-24, -14)});
    for (int i = 0; i < 12; i++) s.push_back({45, 2.0 + i * 0.15, 0.14, lv(-24, -14)});
    RunSeq("repeated notes", s, tot, verbose);
  }
  // 5. palm-muted chugs on low E with a few accents
  {
    std::vector<SeqNote> s;
    const int riff[] = {40, 40, 40, 43, 40, 40, 45, 40, 40, 40, 43, 40, 47, 45, 43, 40};
    for (int i = 0; i < 16; i++) s.push_back({riff[i], i * 0.125, 0.08, lv(-22, -14)});
    RunSeq("palm mute riff", s, tot, verbose);
  }
  // 6. arpeggios that ring (notes overlap)
  {
    std::vector<SeqNote> s;
    const int arps[3][6] = {{48, 52, 55, 60, 64, 67}, {45, 52, 57, 60, 64, 69}, {43, 47, 50, 55, 59, 67}};
    double t = 0;
    for (auto& a : arps) { for (int n : a) { s.push_back({n, t, 1.1 - (t - std::floor(t / 1.4) * 1.4), lv(-24, -16)}); t += 0.18; } t += 0.32; }
    RunSeq("ringing arpeggio", s, tot, verbose);
  }
  // 7. lead line: random melody, mixed lengths and strengths
  {
    std::vector<SeqNote> s;
    double t = 0;
    int n = 64;
    for (int i = 0; i < 40; i++)
    {
      n = std::clamp(n + (int)std::lround(lv(-5, 5)), 52, 84);
      const double d = 0.1 + 0.3 * u(rng);
      s.push_back({n, t, d * 0.95, lv(-30, -14)});
      t += d;
    }
    RunSeq("lead line", s, tot, verbose);
  }
  double slope, r;
  VelFit(tot.vel, slope, r);
  std::sort(tot.lat.begin(), tot.lat.end());
  printf("TOTAL: notes %d ok %d miss %d wrong %d (long %d) hang %d | lat avg %.1f median %.0f | vel/dB %.2f r %.2f\n", tot.expected,
         tot.ok, tot.missed, tot.extra, tot.longExtra, tot.hang, tot.ok ? tot.latSum / tot.ok : 0.0,
         tot.lat.empty() ? 0.0 : tot.lat[tot.lat.size() / 2], slope, r);
  return 0;
}
