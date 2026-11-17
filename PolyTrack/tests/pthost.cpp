// Offline VST3 host for PolyTrack: stereo input, MIDI notes in, prints MIDI out, writes audio.
// pthost.exe <plugin.vst3> <in.f32 stereo interleaved> <out.f32 stereo interleaved> [block] [sr]
// env NOTES="ms:note:vel;..."  (vel 0 = note off)   env PARAMS="id=plain;..."   env QUIET=1
#define INIT_CLASS_IID
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <algorithm>

#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

class Host : public IHostApplication
{
public:
  tresult PLUGIN_API getName(String128 name) override { name[0] = 'h'; name[1] = 0; return kResultOk; }
  tresult PLUGIN_API createInstance(TUID, TUID, void** obj) override { *obj = nullptr; return kResultFalse; }
  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
  {
    if (FUnknownPrivate::iidEqual(iid, IHostApplication::iid) || FUnknownPrivate::iidEqual(iid, FUnknown::iid)) { *obj = this; return kResultOk; }
    *obj = nullptr; return kNoInterface;
  }
  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }
};

static std::vector<char> ReadAll(const char* p)
{
  std::vector<char> v; FILE* f = fopen(p, "rb"); if (!f) return v;
  fseek(f, 0, SEEK_END); v.resize(ftell(f)); fseek(f, 0, SEEK_SET); fread(v.data(), 1, v.size(), f); fclose(f); return v;
}

static const char* NN(int n) { static char b[8]; static const char* nm[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"}; snprintf(b, 8, "%s%d", nm[n % 12], n / 12 - 1); return b; }

int main(int argc, char** argv)
{
  if (argc < 4) { fprintf(stderr, "usage\n"); return 2; }
  HMODULE dll = LoadLibraryA(argv[1]);
  if (!dll) { fprintf(stderr, "LoadLibrary failed %lu\n", GetLastError()); return 3; }
  using InitFn = bool (*)();
  if (auto init = (InitFn)GetProcAddress(dll, "InitDll")) init();
  using GetFactory = IPluginFactory* (*)();
  IPluginFactory* factory = ((GetFactory)GetProcAddress(dll, "GetPluginFactory"))();
  IComponent* component = nullptr;
  for (int i = 0; i < factory->countClasses(); i++)
  {
    PClassInfo info; factory->getClassInfo(i, &info);
    if (strcmp(info.category, kVstAudioEffectClass) == 0) { factory->createInstance(info.cid, IComponent::iid, (void**)&component); break; }
  }
  if (!component) return 4;
  Host host;
  component->initialize(&host);
  IAudioProcessor* processor = nullptr;
  component->queryInterface(IAudioProcessor::iid, (void**)&processor);
  IEditController* ctrl = nullptr;
  component->queryInterface(IEditController::iid, (void**)&ctrl);
  if (std::string(argv[2]) == "params")
  {
    for (int32 i = 0; ctrl && i < ctrl->getParameterCount(); i++)
    {
      ParameterInfo pi; ctrl->getParameterInfo(i, pi);
      char t[256] = {}; for (int k = 0; k < 255 && pi.title[k]; k++) t[k] = (char)pi.title[k];
      printf("%u\t%s\n", (unsigned)pi.id, t);
    }
    printf("event buses in %d out %d\n", component->getBusCount(kEvent, kInput), component->getBusCount(kEvent, kOutput));
    return 0;
  }
  std::map<int, double> setParams;
  if (const char* ps = getenv("PARAMS"))
  {
    std::string s(ps); size_t pos = 0;
    while (pos < s.size()) { size_t semi = s.find(';', pos); if (semi == std::string::npos) semi = s.size(); std::string it = s.substr(pos, semi - pos); size_t eq = it.find('='); if (eq != std::string::npos) setParams[atoi(it.c_str())] = atof(it.c_str() + eq + 1); pos = semi + 1; }
  }
  struct NoteIn { double ms; int note, vel; };
  std::vector<NoteIn> notesIn;
  if (const char* ns = getenv("NOTES"))
  {
    std::string s(ns); size_t pos = 0;
    while (pos < s.size()) { size_t semi = s.find(';', pos); if (semi == std::string::npos) semi = s.size(); std::string it = s.substr(pos, semi - pos); double ms; int n, v; if (sscanf(it.c_str(), "%lf:%d:%d", &ms, &n, &v) == 3) notesIn.push_back({ms, n, v}); pos = semi + 1; }
  }
  const bool quiet = getenv("QUIET") != nullptr;
  const int B = argc > 4 ? atoi(argv[4]) : 128;
  const double SR = argc > 5 ? atof(argv[5]) : 48000.0;

  SpeakerArrangement inArr[1] = {SpeakerArr::kStereo}, outArr[1] = {SpeakerArr::kStereo};
  processor->setBusArrangements(inArr, 1, outArr, 1);
  component->activateBus(kAudio, kInput, 0, true);
  component->activateBus(kAudio, kOutput, 0, true);
  for (int b = 0; b < component->getBusCount(kEvent, kInput); b++) component->activateBus(kEvent, kInput, b, true);
  for (int b = 0; b < component->getBusCount(kEvent, kOutput); b++) component->activateBus(kEvent, kOutput, b, true);
  ProcessSetup setup{kRealtime, kSample64, B, SR};
  processor->setupProcessing(setup);
  component->setActive(true);
  processor->setProcessing(true);

  std::vector<char> raw = ReadAll(argv[2]);
  const size_t n = raw.size() / sizeof(float) / 2;
  const float* x = (const float*)raw.data();
  std::vector<float> out(n * 2);
  std::vector<double> i0(B), i1(B), o0(B), o1(B);
  double* ip[2] = {i0.data(), i1.data()}; double* op[2] = {o0.data(), o1.data()};
  AudioBusBuffers inBus, outBus;
  inBus.numChannels = 2; inBus.channelBuffers64 = ip; outBus.numChannels = 2; outBus.channelBuffers64 = op;
  LARGE_INTEGER f0, t0, t1; QueryPerformanceFrequency(&f0); double worst = 0, total = 0;
  int nOn = 0, nOff = 0, nBend = 0, nCC = 0;
  for (size_t pos = 0; pos < n; pos += B)
  {
    const int len = (int)std::min<size_t>(B, n - pos);
    for (int s = 0; s < len; s++) { i0[s] = x[2 * (pos + s)]; i1[s] = x[2 * (pos + s) + 1]; }
    ParameterChanges changes;
    if (pos == 0)
      for (auto& kv : setParams) { int32 qi = 0; if (auto* q = changes.addParameterData((ParamID)kv.first, qi)) { int32 pi = 0; q->addPoint(0, ctrl->plainParamToNormalized((ParamID)kv.first, kv.second), pi); } }
    EventList inEv, outEv;
    for (auto& ni : notesIn)
    {
      const double smp = ni.ms * 0.001 * SR;
      if (smp >= pos && smp < pos + len)
      {
        Event e{}; e.busIndex = 0; e.sampleOffset = (int32)(smp - pos);
        if (ni.vel > 0) { e.type = Event::kNoteOnEvent; e.noteOn.pitch = (int16)ni.note; e.noteOn.velocity = ni.vel / 127.f; e.noteOn.channel = 0; e.noteOn.noteId = -1; }
        else { e.type = Event::kNoteOffEvent; e.noteOff.pitch = (int16)ni.note; e.noteOff.velocity = 0; e.noteOff.channel = 0; e.noteOff.noteId = -1; }
        inEv.addEvent(e);
      }
    }
    ProcessData d;
    d.inputParameterChanges = pos == 0 ? &changes : nullptr;
    d.processMode = kRealtime; d.symbolicSampleSize = kSample64; d.numSamples = len;
    d.numInputs = 1; d.numOutputs = 1; d.inputs = &inBus; d.outputs = &outBus;
    d.inputEvents = &inEv; d.outputEvents = &outEv;
    QueryPerformanceCounter(&t0);
    processor->process(d);
    QueryPerformanceCounter(&t1);
    const double dt = (double)(t1.QuadPart - t0.QuadPart) / f0.QuadPart;
    total += dt; worst = std::max(worst, dt / (len / SR));
    for (int s = 0; s < len; s++) { out[2 * (pos + s)] = (float)o0[s]; out[2 * (pos + s) + 1] = (float)o1[s]; }
    for (int32 k = 0; k < outEv.getEventCount(); k++)
    {
      Event e; outEv.getEvent(k, e);
      const double ms = (pos + e.sampleOffset) * 1000.0 / SR;
      if (e.type == Event::kNoteOnEvent) { nOn++; if (!quiet) printf("%8.1f ms  ON  ch%-2d %-4s vel %d\n", ms, e.noteOn.channel + 1, NN(e.noteOn.pitch), (int)(e.noteOn.velocity * 127 + 0.5f)); }
      else if (e.type == Event::kNoteOffEvent) { nOff++; if (!quiet) printf("%8.1f ms  OFF ch%-2d %-4s\n", ms, e.noteOff.channel + 1, NN(e.noteOff.pitch)); }
      else if (e.type == Event::kLegacyMIDICCOutEvent)
      {
        const int cc = e.midiCCOut.controlNumber;
        if (cc == kPitchBend) { nBend++; }
        else { nCC++; if (!quiet && nCC < 40) printf("%8.1f ms  CC  ch%-2d #%d = %d\n", ms, e.midiCCOut.channel + 1, cc, e.midiCCOut.value); }
      }
    }
  }
  printf("events: %d on, %d off, %d bends, %d CC | CPU avg %.2f%% worst block %.1f%% of real time\n", nOn, nOff, nBend, nCC,
         100.0 * total / (n / SR), 100.0 * worst);
  FILE* f = fopen(argv[3], "wb"); fwrite(out.data(), 4, out.size(), f); fclose(f);
  processor->setProcessing(false); component->setActive(false); component->terminate();
  return 0;
}
