// Tracker harness: Karplus-Strong guitar chords -> PolyTracker, reports accuracy and latency.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <vector>
#include "PolyTracker.h"

using namespace polytrack;
static const double SR = 48000.0;
static float g_lpc = 0.5f;
static bool g_noisy = false;
static int g_pickup = 0; // 1: magnetic pickup model (velocity sensing + weak-fundamental high-pass + resonant top)

// Pickup model: a magnetic pickup senses string velocity (+6 dB/oct), the pickup/cable forms a
// resonant low-pass (~3.5 kHz), and small speaker-less DI rigs often high-pass the lows. Net
// effect on low strings: the 2nd and 3rd harmonics come out louder than the fundamental.
static void ApplyPickup(std::vector<float>& b)
{
  if (!g_pickup) return;
  float prev = 0, hp = 0, hpx = 0;
  const float a = (float)std::exp(-2.0 * 3.14159265 * 120.0 / SR);
  // resonant 2-pole low-pass at 3.5 kHz, Q 2
  const double w = 2 * 3.14159265 * 3500.0 / SR, q = 2.0, al = std::sin(w) / (2 * q), cw = std::cos(w);
  const double b0 = (1 - cw) / 2 / (1 + al), b1 = (1 - cw) / (1 + al), a1 = -2 * cw / (1 + al), a2 = (1 - al) / (1 + al);
  double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  for (auto& x : b)
  {
    float v = (x - prev) * 8.0f; // velocity
    prev = x;
    hp = a * (hp + v - hpx); hpx = v; // 120 Hz high-pass
    double y = b0 * hp + b1 * x1 + b0 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = hp; y2 = y1; y1 = y;
    x = (float)y;
  }
}



struct KS
{
  std::vector<float> d;
  int len = 0, pos = 0;
  float frac = 0, apz = 0, apc = 0, prev = 0, gain = 0.996f, bright = 0.5f;
  void Pluck(float hz, float amp, std::mt19937& rng, float pickPos = 0.18f)
  {
    double total = SR / hz - 0.5; // loop filter adds half a sample
    len = (int)total;
    frac = (float)(total - len);
    if (frac < 0.1f) { len--; frac += 1.0f; }
    apc = (1 - frac) / (1 + frac);
    d.assign(len + 2, 0.0f);
    std::uniform_real_distribution<float> u(-1, 1);
    std::vector<float> ex(len);
    float lp = 0;
    float lp2 = 0; for (int i = 0; i < len; i++) { lp += g_lpc * (u(rng) - lp); lp2 += g_lpc * (lp - lp2); ex[i] = g_lpc < 0.5f ? lp2 : lp; }
    int pp = std::max(1, (int)(pickPos * len));
    if (g_noisy)
      for (int i = 0; i < len; i++) d[i] = amp * (ex[i] - (i >= pp ? ex[i - pp] : 0.0f));
    else
    {
      // physical pluck: triangle displacement peaking at the pick point, smoothed (pick width), little noise
      std::vector<float> tri(len);
      for (int i = 0; i < len; i++) tri[i] = i < pp ? (float)i / pp : (float)(len - i) / (len - pp);
      float l = 0; for (int pass = 0; pass < 2; pass++) for (int i = 0; i < len; i++) { l += (0.25f + g_lpc) * (tri[i] - l); tri[i] = l; }
      float mean = 0; for (float v : tri) mean += v; mean /= len;
      for (int i = 0; i < len; i++) d[i] = amp * 2.0f * ((tri[i] - mean) + 0.05f * ex[i]);
    }
    pos = 0; apz = 0; prev = 0;
    // per-note decay: lower strings ring longer
    gain = (float)std::pow(0.001, 1.0 / (SR * (hz < 150 ? 3.0 : hz < 400 ? 2.0 : 1.2)) * (SR / hz));
  }
  float Tick()
  {
    if (len == 0) return 0;
    float x = d[pos];
    float y = 0.5f * (x + prev);
    prev = x;
    // allpass fractional delay
    float ap = apc * y + apz;
    apz = y - apc * ap;
    d[pos] = ap * gain;
    pos = (pos + 1) % len;
    return x;
  }
  void Mute() { len = 0; }
};

static const char* NoteName(int n)
{
  static char buf[8];
  static const char* nm[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
  snprintf(buf, sizeof buf, "%s%d", nm[n % 12], n / 12 - 1);
  return buf;
}

struct Result { int longExtra = 0; int correct = 0, missed = 0, extra = 0; double latSum = 0; int latN = 0; double latMax = 0; };

static std::vector<double> g_latS, g_latC;
static double Pct(std::vector<double> v, double p) { if (v.empty()) return 0; std::sort(v.begin(), v.end()); return v[(size_t)std::min<double>(v.size() - 1, p * (v.size() - 1) + 0.5)]; }

// Plays `notes` together (strum spread ms between strings), runs `durS` seconds; reports.
static void RunChord(const char* label, std::vector<int> notes, float peakDb, float strumMs, Result& tot, bool verbose,
                     int maxNotes = 6, float sens = 0.5f)
{
  std::mt19937 rng(1234 + notes[0] * 7 + (int)notes.size());
  PolyTracker t;
  t.Init(SR);
  TrackerSettings s; s.maxNotes = maxNotes; s.sensitivity = sens;
  t.SetSettings(s);
  std::vector<KS> str(notes.size());
  const int pre = (int)(0.3 * SR), dur = (int)(1.2 * SR);
  std::vector<float> buf(pre + dur, 0.0f);
  float amp = DbToLin(peakDb);
  for (size_t k = 0; k < notes.size(); k++)
  {
    int start = pre + (int)(k * strumMs * 1e-3 * SR);
    str[k].Pluck(MidiToHz((float)notes[k]), amp, rng);
    for (int i = start; i < (int)buf.size(); i++) buf[i] += str[k].Tick();
  }
  ApplyPickup(buf);
  if (g_pickup) { float pk = 1e-9f; for (float v : buf) pk = std::max(pk, std::fabs(v)); for (auto& v : buf) v *= amp / pk; }
  // noise floor ~ -92 dBFS
  std::normal_distribution<float> g(0, DbToLin(-95));
  for (auto& x : buf) x += g(rng);
  std::vector<NoteEvent> ev;
  std::set<int> want(notes.begin(), notes.end());
  std::set<int> gotOn;
  std::vector<double> onTime(128, -1);
  const int B = 128;
  std::vector<NoteEvent> all;
  std::vector<double> allT;
  for (int i = 0; i < (int)buf.size(); i += B)
  {
    ev.clear();
    const char* tr = getenv("TRACE");
    const double ms = ((i + B) - pre) * 1000.0 / SR;
#ifdef PT_TRACE
    float ta = 44, tb = 52;
    if (const char* e = getenv("TRACEMS")) sscanf(e, "%f,%f", &ta, &tb);
    t.mTrace = tr && !strcmp(tr, label) && ms > ta && ms < tb;
    if (t.mTrace) printf("---- block ending %.1f ms\n", ms);
#endif
    t.Process(&buf[i], std::min(B, (int)buf.size() - i), ev);
    for (auto& e : ev) { all.push_back(e); allT.push_back((i + e.offset) / SR); }
    if (tr && !strcmp(tr, label) && ms > -5 && ms < 110)
    {
      // per note: '#' on, digit = reason it isn't on yet (1 no det 3 pitch 4 partial 6 emerging 7 counting)
      printf("%6.1f ms ", ms);
      for (int n : notes) printf(" %s:%c", NoteName(n), t.IsOn(n) ? '#' : (char)('0' + t.mWhy[n]));
      printf("\n");
    }
  }
  std::string onStr;
  int k = 0;
  int early = 0;
  for (size_t i = 0; i < all.size(); i++)
  {
    auto& e = all[i];
    if (e.type != NoteEvent::kOn) continue;
    // a chord tone that comes on before its own string was picked is a ghost (another string's
    // partial), not a fast detection
    size_t si = 0; for (; si < notes.size(); si++) if (notes[si] == e.note) break;
    const double pluckT = pre / SR + (si < notes.size() ? si * strumMs * 1e-3 : 0.0);
    if (si < notes.size() && allT[i] < pluckT) { early++; continue; }
    if (onTime[e.note] < 0) onTime[e.note] = allT[i];
    gotOn.insert(e.note);
    char b[48]; snprintf(b, sizeof b, " %s@%.0f(v%d)", NoteName(e.note), (allT[i] - pre / SR) * 1000, e.velocity);
    if (k++ < 14) onStr += b;
  }
  Result r;
  for (int n : want)
  {
    if (gotOn.count(n))
    {
      r.correct++;
      size_t idx = 0; for (; idx < notes.size(); idx++) if (notes[idx] == n) break;
      double lat = (onTime[n] - (pre / SR + idx * strumMs * 1e-3)) * 1000;
      r.latSum += lat; r.latN++; r.latMax = std::max(r.latMax, lat);
      (notes.size() > 1 ? g_latC : g_latS).push_back(lat);
    }
    else r.missed++;
  }
  for (int n : gotOn) if (!want.count(n)) r.extra++;
  r.extra += early;
  {
    std::vector<double> onAt(128, -1), dsum(128, 0);
    for (size_t i = 0; i < all.size(); i++)
    {
      auto& e = all[i];
      if (e.type == NoteEvent::kOn) onAt[e.note] = allT[i];
      if (e.type == NoteEvent::kOff && onAt[e.note] >= 0) { dsum[e.note] += allT[i] - onAt[e.note]; onAt[e.note] = -1; }
    }
    double end = buf.size() / SR;
    std::string le;
    for (int n = 0; n < 128; n++)
    {
      if (onAt[n] >= 0) dsum[n] += end - onAt[n];
      if (!want.count(n) && dsum[n] >= 0.05) { r.longExtra++; char b[24]; snprintf(b, sizeof b, " %s:%.0fms", NoteName(n), dsum[n] * 1000); le += b; }
    }
    if (verbose && !le.empty()) printf("   long extras:%s\n", le.c_str());
  }
  tot.longExtra += r.longExtra;
  tot.correct += r.correct; tot.missed += r.missed; tot.extra += r.extra; tot.latSum += r.latSum; tot.latN += r.latN;
  tot.latMax = std::max(tot.latMax, r.latMax);
  if (verbose)
  {
    std::string w; for (int n : notes) { w += NoteName(n); w += " "; }
    printf("%-14s want[%s] ok %d miss %d extra %d lat avg %.1f max %.1f |%s\n", label, w.c_str(), r.correct, r.missed, r.extra,
           r.latN ? r.latSum / r.latN : 0, r.latMax, onStr.c_str());
  }
}

static void ParseTuneEnv()
{
  // tuning overrides: FALL="a,b,c" NEEDK=x FRAMES=n EMERGE=n
  if (const char* e = getenv("FALL")) sscanf(e, "%f,%f,%f", &gTune.fall[0], &gTune.fall[1], &gTune.fall[2]);
  if (const char* e = getenv("NEEDK")) gTune.needK = (float)atof(e);
  if (const char* e = getenv("FRAMES")) gTune.needFrames = atoi(e);
  if (const char* e = getenv("EMERGE")) gTune.emergeFrames = atoi(e);
  if (const char* e = getenv("GAP")) gTune.gapTol = atoi(e);
  if (const char* e = getenv("HDROP")) gTune.harmDrop = atoi(e) != 0;
  if (const char* e = getenv("ATTK")) gTune.attackK = (float)atof(e);
  if (const char* e = getenv("ATTF")) gTune.attackFrames = atoi(e);
  if (const char* e = getenv("BLEND")) gTune.blend = atoi(e);
  if (const char* e = getenv("ATTMAX")) gTune.attackMaxOn = atoi(e);
  if (const char* e = getenv("HOP")) gTune.hopMs = (float)atof(e);
  if (const char* e = getenv("RETAGE")) gTune.retAge = atoi(e);
  if (const char* e = getenv("RETX")) gTune.retExtra = atoi(e);
  if (const char* e = getenv("ONSETDB")) gTune.onsetDb = (float)atof(e);
  if (const char* e = getenv("BLOCKMAX")) gTune.blockMax = atoi(e);
  if (const char* e = getenv("FASTREL")) gTune.fastRel = (float)atof(e);
  if (const char* e = getenv("VELCAL")) gTune.velCalDb = (float)atof(e);
  if (const char* e = getenv("VELRES")) gTune.velRes = atoi(e);
  if (const char* e = getenv("REPICK")) gTune.repickDb = (float)atof(e);
  if (const char* e = getenv("REPAGE")) gTune.repickAge = atoi(e);
  if (const char* e = getenv("REPOWN")) gTune.repickOwn = atoi(e);
  if (const char* e = getenv("REPLO")) gTune.repickLoHz = (float)atof(e);
  if (const char* e = getenv("REPFLOOR")) gTune.repickFloorDb = (float)atof(e);
  if (const char* e = getenv("REPPEAK")) gTune.repickBelowPeakDb = (float)atof(e);
  if (const char* e = getenv("REPREL")) gTune.repickRel = (float)atof(e);
  if (const char* e = getenv("REPWAIT")) gTune.repickWait = atoi(e);
  if (const char* e = getenv("HIGHP")) gTune.highPartials = atoi(e);
  if (const char* e = getenv("MASKF")) gTune.maskFrames = atoi(e);
  if (const char* e = getenv("MASKM")) gTune.maskMiss = atoi(e);
  if (const char* e = getenv("FASTPRES")) gTune.fastPresent = atoi(e);
  if (const char* e = getenv("FASTCENTS")) gTune.fastCents = (float)atof(e);
}

#ifndef TRK_NO_MAIN
int main(int argc, char** argv)
{
  float level = argc > 1 ? (float)atof(argv[1]) : -40.0f;
  float sens = argc > 2 ? (float)atof(argv[2]) : 0.5f;
  if (argc > 3) g_lpc = (float)atof(argv[3]);
  if (argc > 4) g_noisy = atoi(argv[4]) != 0;
  if (argc > 5) g_pickup = atoi(argv[5]);
  bool verbose = getenv("BRIEF") == nullptr;
  ParseTuneEnv();
  Result single, chords;
  printf("== single notes (peak %.0f dBFS) ==\n", level);
  int sn[] = {38, 40, 43, 45, 47, 50, 52, 55, 57, 59, 62, 64, 67, 69, 71, 74, 76, 79, 81, 84, 88};
  for (int n : sn) RunChord(NoteName(n), {n}, level, 0, single, verbose, 6, sens);
  printf("== chords (strummed 8 ms/string) ==\n");
  struct C { const char* name; std::vector<int> n; } cs[] = {
    {"E maj open", {40, 47, 52, 56, 59, 64}}, {"A min open", {45, 52, 57, 60, 64}}, {"C maj open", {48, 52, 55, 60, 64}},
    {"G maj open", {43, 47, 50, 55, 59, 67}}, {"D maj open", {50, 57, 62, 66}},     {"E5 power", {40, 47, 52}},
    {"A5 power", {45, 52, 57}},              {"F barre", {41, 48, 53, 57, 60, 65}}, {"Dyad 3rds", {64, 68}},
    {"Em7 open", {40, 47, 50, 55, 59, 64}},  {"B min barre", {47, 54, 59, 62, 66}}, {"high triad", {72, 76, 79}},
  };
  for (auto& c : cs) RunChord(c.name, c.n, level, 8, chords, verbose, 6, sens);
  printf("\nSINGLE: LONG EXTRAS %d | ok %d miss %d extra %d avg lat %.1f ms max %.1f\n", single.longExtra, single.correct, single.missed, single.extra,
         single.latN ? single.latSum / single.latN : 0, single.latMax);
  printf("CHORDS: LONG EXTRAS %d | ok %d miss %d extra %d avg lat %.1f ms max %.1f\n", chords.longExtra, chords.correct, chords.missed, chords.extra,
         chords.latN ? chords.latSum / chords.latN : 0, chords.latMax);
  printf("MEDIAN: single %.0f (low 6: %.0f avg) | chords median %.0f p75 %.0f\n", Pct(g_latS, 0.5),
         (g_latS.size() >= 6 ? (g_latS[0] + g_latS[1] + g_latS[2] + g_latS[3] + g_latS[4] + g_latS[5]) / 6 : 0), Pct(g_latC, 0.5), Pct(g_latC, 0.75));
  return 0;
}
#endif
