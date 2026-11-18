// clang-format off
// Need to import these two in this order.
#include "PolyTrack.h"
#include "IPlug_include_in_plug_src.h"
// clang-format on

#include <algorithm>
#include <cmath>
#include <ctime>

#if IPLUG_EDITOR
  #include "IControls.h"
#endif

#ifdef OS_WIN
  #include <shlobj.h>
#endif

using namespace polytrack;

namespace
{
const char* kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

void NoteName(int n, WDL_String& s) { s.SetFormatted(8, "%s%d", kNoteNames[((n % 12) + 12) % 12], n / 12 - 1); }

void OffOrDb(double v, WDL_String& s)
{
  if (v <= -59.9)
    s.Set("Off");
  else
    s.SetFormatted(16, "%.1f dB", v);
}

std::atomic<bool> gPanic{false};
} // namespace

PolyTrack::PolyTrack(const InstanceInfo& info)
: Plugin(info, MakeConfig(kNumParams, kNumPresets))
{
  for (auto& a : mCCMap)
    a.store(-1);
  mRecRing.assign((size_t)1 << 21, 0.0f); // 2^20 stereo frames (~21 s at 48 kHz) between UI drains

  // tracker
  GetParam(kTrackSource)->InitEnum("Track Input", kSrcIn1, {"In 1", "In 2", "In 1+2"});
  GetParam(kInGain)->InitDouble("Input Gain", 12.0, -12.0, 48.0, 0.5, "dB");
  GetParam(kGate)->InitDouble("Noise Gate", -72.0, -100.0, -30.0, 1.0, "dB");
  GetParam(kSens)->InitDouble("Sensitivity", 40.0, 0.0, 100.0, 1.0, "%");
  GetParam(kMaxNotes)->InitInt("Max Notes", 6, 1, 6);
  GetParam(kLowNote)->InitInt("Lowest Note", 38, 28, 52);
  GetParam(kLowNote)->SetDisplayFunc([](double v, WDL_String& s) { NoteName((int)v, s); });
  GetParam(kBendTrack)->InitBool("Pitch Bend", true);
  GetParam(kStrictOct)->InitEnum("Octaves", 0, {"Strict", "Allow"});
  GetParam(kVelRange)->InitDouble("Velocity Range", 30.0, 12.0, 48.0, 1.0, "dB");
  GetParam(kVelTop)->InitDouble("Velocity Top", -10.0, -40.0, 0.0, 0.5, "dB");
  // MIDI out
  GetParam(kMidiOn)->InitBool("MIDI Out", true);
  GetParam(kChanMode)->InitEnum("Channels", 0, {"Single", "Per Note"});
  GetParam(kChannel)->InitInt("Channel", 1, 1, 16);
  GetParam(kBendRange)->InitInt("Bend Range", 2, 1, 48, "st");
  GetParam(kTranspose)->InitInt("Transpose", 0, -24, 24, "st");
  GetParam(kVelFixed)->InitInt("Fixed Velocity", 0, 0, 127);
  GetParam(kVelFixed)->SetDisplayFunc([](double v, WDL_String& s) {
    if (v < 0.5)
      s.Set("Dynamic");
    else
      s.SetFormatted(8, "%d", (int)v);
  });
  GetParam(kVelCurve)->InitDouble("Velocity Curve", 1.0, 0.3, 3.0, 0.01, "", 0, "", IParam::ShapePowCurve(1.6));
  GetParam(kHold)->InitDouble("Hold", 0.0, 0.0, 2000.0, 1.0, "ms", 0, "", IParam::ShapePowCurve(2.5));
  GetParam(kInfinite)->InitBool("Infinite Sustain", false);
  GetParam(kThru)->InitBool("MIDI Thru", true);
  GetParam(kEnvCC)->InitInt("Envelope CC", 0, 0, 119);
  GetParam(kEnvCC)->SetDisplayFunc([](double v, WDL_String& s) {
    if (v < 0.5)
      s.Set("Off");
    else
      s.SetFormatted(8, "CC %d", (int)v);
  });
  // harmonizer
  GetParam(kHarmOn)->InitBool("Harmonizer", true);
  GetParam(kHarmSource)->InitEnum("Harmony Input", kSrcIn2, {"In 1", "In 2", "In 1+2"});
  GetParam(kChordSource)->InitEnum("Chord From", kChordKeys, {"MIDI Keys", "Guitar", "Keys+Guitar"});
  GetParam(kHarmVoices)->InitInt("Voices", 8, 1, 12);
  GetParam(kFormant)->InitDouble("Formant", 0.0, -12.0, 12.0, 0.1, "st");
  GetParam(kDetune)->InitDouble("Detune", 6.0, 0.0, 30.0, 0.1, "ct");
  GetParam(kSpread)->InitDouble("Stereo Spread", 60.0, 0.0, 100.0, 1.0, "%");
  GetParam(kHarmAttack)->InitDouble("Attack", 10.0, 1.0, 300.0, 1.0, "ms", 0, "", IParam::ShapePowCurve(2.0));
  GetParam(kHarmRelease)->InitDouble("Release", 120.0, 10.0, 2000.0, 1.0, "ms", 0, "", IParam::ShapePowCurve(2.0));
  GetParam(kHarmLevel)->InitDouble("Harmony Level", 0.0, -60.0, 12.0, 0.1, "", 0, "", IParam::ShapeLinear(), IParam::kUnitDB, OffOrDb);
  // output
  GetParam(kDryLevel)->InitDouble("Dry Level", -6.0, -60.0, 12.0, 0.1, "", 0, "", IParam::ShapeLinear(), IParam::kUnitDB, OffOrDb);
  GetParam(kSynthLevel)->InitDouble("Synth Level", -12.0, -60.0, 12.0, 0.1, "", 0, "", IParam::ShapeLinear(), IParam::kUnitDB, OffOrDb);
  GetParam(kMaster)->InitDouble("Master", 0.0, -24.0, 12.0, 0.1, "dB");

#if IPLUG_EDITOR
  mMakeGraphicsFunc = [&]() { return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS); };

  mLayoutFunc = [&](IGraphics* g) {
    if (g->NControls())
      return; // the window scales uniformly; no relayout needed

    g->AttachCornerResizer(EUIResizerMode::Scale, false);
    g->LoadFont("Roboto-Regular", ROBOTO_FN);
    g->AttachPanelBackground(ptui::kBg);

    const IText labelText(12.f, ptui::kDim, "Roboto-Regular");
    const IText valueText(12.f, ptui::kInk, "Roboto-Regular");
    const IVStyle style = DEFAULT_STYLE.WithDrawShadows(false)
                            .WithColor(kBG, COLOR_TRANSPARENT)
                            .WithColor(kFG, IColor(255, 44, 48, 58))
                            .WithColor(kPR, IColor(255, 64, 70, 84))
                            .WithColor(kFR, ptui::kPanelEdge)
                            .WithColor(kHL, IColor(60, 255, 255, 255))
                            .WithColor(kX1, ptui::kAccent)
                            .WithLabelText(labelText)
                            .WithValueText(valueText)
                            .WithRoundness(0.3f)
                            .WithFrameThickness(1.f);
    const IVStyle tabStyle = style.WithColor(kPR, IColor(255, 120, 72, 30)).WithValueText(valueText.WithSize(11.f));
    const IVStyle btnStyle = style.WithColor(kFG, IColor(255, 48, 52, 62)).WithLabelText(valueText.WithSize(13.f));

    const IRECT b = g->GetBounds();
    const float M = 12.f, G = 10.f;

    // header
    const IRECT header = b.GetFromTop(64.f).GetPadded(-M);
    g->AttachControl(new ITextControl(header.GetFromLeft(260.f).GetFromTop(26.f), "POLYTRACK",
                                      IText(26.f, ptui::kAccent, "Roboto-Regular", EAlign::Near)));
    g->AttachControl(new ITextControl(header.GetFromLeft(420.f).GetFromBottom(16.f),
                                      "guitar to MIDI  +  keyboard-chord harmonizer",
                                      IText(12.f, ptui::kDim, "Roboto-Regular", EAlign::Near)));
    WDL_String ver;
    ver.SetFormatted(32, "v%s", PLUG_VERSION_STR);
    g->AttachControl(new ITextControl(header.GetFromRight(60.f).GetFromBottom(16.f), ver.Get(),
                                      IText(12.f, ptui::kDim, "Roboto-Regular", EAlign::Far)));
    {
      IRECT btns = header.GetFromRight(470.f).GetReducedFromRight(70.f).GetMidVPadded(15.f);
      const float bw = 92.f;
      g->AttachControl(new IVButtonControl(btns.GetFromLeft(bw), [this](IControl* c) {
                         SplashClickActionFunc(c);
                         if (mRecording.load())
                           _StopRecording();
                         else
                           _StartRecording();
                       }, "REC INPUT", btnStyle), kCtrlTagRec);
      g->AttachControl(new IVButtonControl(btns.GetFromLeft(2 * bw + 8).GetFromRight(bw), [this](IControl* c) {
                         SplashClickActionFunc(c);
                         const bool armed = !mLearnArmed.load();
                         mLearnParam.store(-1);
                         mLearnArmed.store(armed);
                       }, "MIDI LEARN", btnStyle), kCtrlTagLearn);
      g->AttachControl(new IVButtonControl(btns.GetFromLeft(3 * bw + 16).GetFromRight(bw), [this](IControl* c) {
                         SplashClickActionFunc(c);
                         for (auto& a : mCCMap)
                           a.store(-1);
                         mLearnArmed.store(false);
                       }, "CLEAR MAP", btnStyle));
      g->AttachControl(new IVButtonControl(btns.GetFromLeft(4 * bw + 24).GetFromRight(bw), [this](IControl* c) {
                         SplashClickActionFunc(c);
                         gPanic.store(true);
                       }, "PANIC", btnStyle));
    }

    // panels
    const IRECT body = b.GetReducedFromTop(70.f).GetFromTop(392.f).GetHPadded(-M);
    const float avail = body.W() - 3 * G;
    const float fr[4] = {0.245f, 0.28f, 0.30f, 0.175f};
    IRECT cols[4];
    float x = body.L;
    for (int i = 0; i < 4; i++)
    {
      cols[i] = IRECT(x, body.T, x + avail * fr[i], body.B);
      x += avail * fr[i] + G;
    }
    const char* titles[4] = {"GUITAR TRACKER", "MIDI OUT", "HARMONIZER", "OUTPUT"};
    for (int i = 0; i < 4; i++)
      g->AttachControl(new ptui::PanelControl(cols[i], titles[i]));

    auto inner = [](const IRECT& r) { return r.GetReducedFromTop(32.f).GetPadded(-8.f); };
    auto knob = [&](const IRECT& r, int p, const char* label) {
      g->AttachControl(new IVKnobControl(r.GetPadded(-2.f), p, label, style, true));
    };
    auto toggle = [&](const IRECT& r, int p, const char* label) {
      g->AttachControl(new IVToggleControl(r.GetPadded(-3.f), p, label, style, "OFF", "ON"));
    };
    auto tabs = [&](const IRECT& r, int p, std::initializer_list<const char*> opts, const char* label) {
      g->AttachControl(new IVTabSwitchControl(r.GetPadded(-2.f), p, std::vector<const char*>(opts), label, tabStyle));
    };

    // GUITAR TRACKER
    {
      IRECT r = inner(cols[0]);
      tabs(r.GetFromTop(52.f), kTrackSource, {"In 1", "In 2", "In 1+2"}, "Input");
      IRECT gr = r.GetReducedFromTop(58.f);
      const int R = 3, C = 3;
      knob(gr.GetGridCell(0, 0, R, C), kInGain, "Input Gain");
      knob(gr.GetGridCell(0, 1, R, C), kGate, "Noise Gate");
      knob(gr.GetGridCell(0, 2, R, C), kSens, "Sensitivity");
      knob(gr.GetGridCell(1, 0, R, C), kMaxNotes, "Max Notes");
      knob(gr.GetGridCell(1, 1, R, C), kLowNote, "Lowest Note");
      knob(gr.GetGridCell(1, 2, R, C), kVelRange, "Vel Range");
      knob(gr.GetGridCell(2, 0, R, C), kVelTop, "Vel Top");
      toggle(gr.GetGridCell(2, 1, R, C).GetMidVPadded(26.f), kBendTrack, "Pitch Bend");
      tabs(gr.GetGridCell(2, 2, R, C).GetMidVPadded(26.f), kStrictOct, {"Strict", "Allow"}, "Octaves");
    }
    // MIDI OUT
    {
      IRECT r = inner(cols[1]);
      IRECT top = r.GetFromTop(52.f);
      toggle(top.GetGridCell(0, 0, 1, 4), kMidiOn, "MIDI Out");
      tabs(top.GetGridCell(0, 1, 1, 4).Union(top.GetGridCell(0, 2, 1, 4)), kChanMode, {"Single", "Per Note"}, "Channels");
      toggle(top.GetGridCell(0, 3, 1, 4), kThru, "Thru");
      IRECT gr = r.GetReducedFromTop(58.f);
      const int R = 3, C = 4;
      knob(gr.GetGridCell(0, 0, R, C), kChannel, "Channel");
      knob(gr.GetGridCell(0, 1, R, C), kBendRange, "Bend Range");
      knob(gr.GetGridCell(0, 2, R, C), kTranspose, "Transpose");
      knob(gr.GetGridCell(0, 3, R, C), kVelFixed, "Fixed Vel");
      knob(gr.GetGridCell(1, 0, R, C), kVelCurve, "Vel Curve");
      knob(gr.GetGridCell(1, 1, R, C), kHold, "Hold");
      knob(gr.GetGridCell(1, 2, R, C), kEnvCC, "Env CC");
      toggle(gr.GetGridCell(1, 3, R, C).GetMidVPadded(26.f), kInfinite, "Infinite");
      g->AttachControl(new ITextControl(gr.GetGridCell(2, 0, R, 1).GetPadded(-4.f),
                                        "Per Note = MPE (one channel per string)",
                                        IText(11.f, ptui::kDim, "Roboto-Regular")));
    }
    // HARMONIZER
    {
      IRECT r = inner(cols[2]);
      IRECT top = r.GetFromTop(52.f);
      toggle(top.GetGridCell(0, 0, 1, 4), kHarmOn, "Harmonizer");
      tabs(top.GetGridCell(0, 1, 1, 4).Union(top.GetGridCell(0, 3, 1, 4)), kHarmSource, {"In 1", "In 2", "In 1+2"}, "Voice Input");
      tabs(r.GetReducedFromTop(56.f).GetFromTop(50.f), kChordSource, {"MIDI Keys", "Guitar", "Keys+Guitar"}, "Chord From");
      IRECT gr = r.GetReducedFromTop(112.f);
      const int R = 2, C = 4;
      knob(gr.GetGridCell(0, 0, R, C), kHarmVoices, "Voices");
      knob(gr.GetGridCell(0, 1, R, C), kFormant, "Formant");
      knob(gr.GetGridCell(0, 2, R, C), kDetune, "Detune");
      knob(gr.GetGridCell(0, 3, R, C), kSpread, "Spread");
      knob(gr.GetGridCell(1, 0, R, C), kHarmAttack, "Attack");
      knob(gr.GetGridCell(1, 1, R, C), kHarmRelease, "Release");
      knob(gr.GetGridCell(1, 2, R, C), kHarmLevel, "Level");
    }
    // OUTPUT
    {
      IRECT r = inner(cols[3]);
      IRECT kn = r.GetFromLeft(r.W() * 0.62f);
      knob(kn.GetGridCell(0, 0, 3, 1), kDryLevel, "Dry");
      knob(kn.GetGridCell(1, 0, 3, 1), kSynthLevel, "Synth");
      knob(kn.GetGridCell(2, 0, 3, 1), kMaster, "Master");
      g->AttachControl(new IVMeterControl<2>(r.GetFromRight(r.W() * 0.36f), "", style.WithShowLabel(false),
                                             EDirection::Vertical, {"In", "Out"}, 0, IVMeterControl<2>::EResponse::Log, -72.f, 6.f),
                       kCtrlTagMeter);
    }

    // notes + status line, keyboard
    const IRECT lines = b.GetReducedFromTop(470.f).GetFromTop(46.f).GetHPadded(-M);
    g->AttachControl(new ITextControl(lines.GetFromTop(24.f), "", IText(16.f, ptui::kInk, "Roboto-Regular", EAlign::Near)),
                     kCtrlTagNotes);
    g->AttachControl(new ITextControl(lines.GetFromBottom(20.f), "", IText(12.f, ptui::kDim, "Roboto-Regular", EAlign::Near)),
                     kCtrlTagStatus);
    const IRECT kb = b.GetFromBottom(b.H() - 522.f).GetPadded(-M).GetReducedFromTop(0.f);
    g->AttachControl(new ptui::KeyboardControl(kb, 28, 96, &mTrackedMask, &mKeyMask), kCtrlTagKeyboard);
    // open a bit smaller than the design size so it fits high-DPI laptop screens; the corner
    // handle scales the whole UI
    g->Resize(PLUG_WIDTH, PLUG_HEIGHT, 0.85f);
  };
#endif
}

PolyTrack::~PolyTrack()
{
  if (mRecFile)
    _StopRecording();
}

#if IPLUG_EDITOR
void PolyTrack::OnParentWindowResize(int width, int height)
{
  if (GetUI())
    GetUI()->Resize(PLUG_WIDTH, PLUG_HEIGHT, (float)width / PLUG_WIDTH, false); // uniform scale
}

void PolyTrack::OnParamChangeUI(int paramIdx, EParamSource source)
{
  if (source == kUI && mLearnArmed.load())
    mLearnParam.store(paramIdx);
}
#endif

// State layout: 0.1.0 = kNumParams0 doubles, 'PTm1' + CC map. 0.1.1+ = 'PTs2', count, that many
// doubles, then 'PTm1' + CC map. Parameters are only ever appended.
static constexpr int kNumParams0 = kStrictOct;

bool PolyTrack::SerializeState(IByteChunk& chunk) const
{
  int tag = 'PTs2', count = kNumParams;
  chunk.Put(&tag);
  chunk.Put(&count);
  bool ok = true;
  for (int i = 0; i < kNumParams; i++)
  {
    double v = GetParam(i)->Value();
    ok &= chunk.Put(&v) > 0;
  }
  int magic = 'PTm1';
  chunk.Put(&magic);
  for (const auto& a : mCCMap)
  {
    int v = a.load();
    chunk.Put(&v);
  }
  return ok;
}

int PolyTrack::UnserializeState(const IByteChunk& chunk, int startPos)
{
  int pos = startPos, tag = 0, count = kNumParams0;
  const int pt = chunk.Get(&tag, pos);
  if (pt > 0 && tag == 'PTs2')
  {
    pos = chunk.Get(&count, pt);
    if (pos < 0)
      return pos;
  }
  ENTER_PARAMS_MUTEX
  for (int i = 0; i < count && pos >= 0; i++)
  {
    double v = 0.0;
    pos = chunk.Get(&v, pos);
    if (pos >= 0 && i < kNumParams)
      GetParam(i)->Set(v);
  }
  LEAVE_PARAMS_MUTEX
  if (pos < 0)
    return pos;
  OnParamReset(kPresetRecall);
  int magic = 0;
  const int p2 = chunk.Get(&magic, pos);
  if (p2 > 0 && magic == 'PTm1')
  {
    pos = p2;
    for (auto& a : mCCMap)
    {
      int v = -1;
      const int p3 = chunk.Get(&v, pos);
      if (p3 < 0)
        break;
      pos = p3;
      a.store((v >= 0 && v < kNumParams) ? v : -1);
    }
  }
  return pos;
}

#if IPLUG_DSP
void PolyTrack::OnReset()
{
  const double sr = GetSampleRate();
  mTracker.Init(sr);
  mNoteOut.Init(sr);
  mHarm.Init(sr);
  mSynth.Init(sr);
  const int bs = std::max(512, GetBlockSize());
  mTrackIn.assign(bs, 0.0f);
  mHarmIn.assign(bs, 0.0f);
  mOutL.assign(bs, 0.0f);
  mOutR.assign(bs, 0.0f);
  mInMixL.assign(bs, 0.0);
  mInMixR.assign(bs, 0.0);
  mEvents.reserve(512);
  mMidiOut.reserve(4096);
  mMidiOutIn.reserve(1024);
  mThruQueue.reserve(1024);
  mMeterSender.Reset(sr);
  mTrackedMask.Clear();
  mEnvLast = -1;
  mRecSR = (int)std::lround(sr);
}

void PolyTrack::_ReadSettings()
{
  TrackerSettings ts;
  ts.gateDb = (float)GetParam(kGate)->Value();
  ts.sensitivity = (float)(GetParam(kSens)->Value() / 100.0);
  ts.maxNotes = GetParam(kMaxNotes)->Int();
  ts.lowNote = GetParam(kLowNote)->Int();
  ts.highNote = 88;
  ts.velRangeDb = (float)GetParam(kVelRange)->Value();
  ts.velTopDb = (float)GetParam(kVelTop)->Value();
  ts.bendTracking = GetParam(kBendTrack)->Bool();
  ts.strictOctaves = GetParam(kStrictOct)->Int() == 0;
  mTracker.SetSettings(ts);

  OutputSettings os;
  os.perNoteChannels = GetParam(kChanMode)->Int() == 1;
  os.channel = os.perNoteChannels ? 0 : GetParam(kChannel)->Int() - 1;
  os.bendRange = GetParam(kBendRange)->Int();
  os.transpose = GetParam(kTranspose)->Int();
  os.fixedVelocity = GetParam(kVelFixed)->Int();
  os.velCurve = (float)GetParam(kVelCurve)->Value();
  os.holdMs = (float)GetParam(kHold)->Value();
  os.infinite = GetParam(kInfinite)->Bool();
  os.bends = ts.bendTracking;
  os.maxChannels = std::max(1, ts.maxNotes);
  mNoteOut.SetSettings(os, mMidiOutIn);
  mTranspose = os.transpose;

  HarmonizerSettings hs;
  hs.maxVoices = GetParam(kHarmVoices)->Int();
  hs.formantSemis = (float)GetParam(kFormant)->Value();
  hs.detuneCents = (float)GetParam(kDetune)->Value();
  hs.spread = (float)(GetParam(kSpread)->Value() / 100.0);
  hs.attackMs = (float)GetParam(kHarmAttack)->Value();
  hs.releaseMs = (float)GetParam(kHarmRelease)->Value();
  mHarm.SetSettings(hs);

  const int cs = GetParam(kChordSource)->Int();
  const bool fromKeys = cs == kChordKeys || cs == kChordBoth;
  const bool fromGuitar = cs == kChordGuitar || cs == kChordBoth;
  if (mHarmFromGuitar && !fromGuitar)
  {
    // stop guitar-driven voices that won't get their note-off any more
    for (int n = 0; n < 128; n++)
      if (mTracker.IsOn(n) && !mKeyMask.Get(n))
        mHarm.NoteOff(n);
  }
  mHarmFromKeys = fromKeys;
  mHarmFromGuitar = fromGuitar;
  mThru = GetParam(kThru)->Bool();
  mEnvCC = GetParam(kEnvCC)->Int();

  const bool midiOn = GetParam(kMidiOn)->Bool();
  if (mMidiOn && !midiOn)
  {
    // turning MIDI out off: release what's sounding on the external synth first
    std::vector<RawMidi>& v = mMidiOutIn;
    mNoteOut.AllNotesOff(0, v);
    _SendRaw(v);
    v.clear();
  }
  mMidiOn = midiOn;
}

void PolyTrack::ProcessMidiMsg(const IMidiMsg& msg)
{
  const IMidiMsg::EStatusMsg st = msg.StatusMsg();
  if (st == IMidiMsg::kControlChange)
  {
    const int cc = msg.mData1, val = msg.mData2;
    if (mLearnArmed.load() && mLearnParam.load() >= 0)
    {
      mCCMap[cc].store(mLearnParam.load());
      mLearnArmed.store(false);
      mLearnedCC.store(cc);
      return;
    }
    const int p = mCCMap[cc].load();
    if (p >= 0 && p < kNumParams)
    {
      const bool isBool = GetParam(p)->Type() == IParam::kTypeBool;
      mParamQueue.Push({p, isBool ? (val >= 64 ? 1.0 : 0.0) : val / 127.0});
      return; // mapped controls are consumed
    }
    if (cc == 64)
      mNoteOut.SetPedal(val >= 64, msg.mOffset, mMidiOutIn);
  }
  else if (st == IMidiMsg::kNoteOn && msg.Velocity() > 0)
  {
    mKeyMask.Set(msg.NoteNumber(), true);
    if (mHarmFromKeys)
      mHarm.NoteOn(msg.NoteNumber(), msg.Velocity());
  }
  else if (st == IMidiMsg::kNoteOff || (st == IMidiMsg::kNoteOn && msg.Velocity() == 0))
  {
    mKeyMask.Set(msg.NoteNumber(), false);
    if (!(mHarmFromGuitar && mTracker.IsOn(msg.NoteNumber())))
      mHarm.NoteOff(msg.NoteNumber());
  }
  if (mThru && mThruQueue.size() < mThruQueue.capacity())
    mThruQueue.push_back(msg);
}

void PolyTrack::_SendRaw(const std::vector<RawMidi>& v)
{
  for (const RawMidi& r : v)
    SendMidiMsg(IMidiMsg(r.offset, r.status, r.d1, r.d2));
}

void PolyTrack::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
  _ReadSettings();

  if (gPanic.exchange(false))
  {
    mNoteOut.AllNotesOff(0, mMidiOutIn);
    for (int c = 0; c < 16; c++)
    {
      mMidiOutIn.push_back({0, (uint8_t)(0xB0 | c), 64, 0});
      mMidiOutIn.push_back({0, (uint8_t)(0xB0 | c), 123, 0});
    }
    mHarm.AllOff();
    mSynth.AllOff();
    mKeyMask.Clear();
  }

  int nIn = NInChansConnected();
  const sample* in1 = inputs[0];
  const sample* in2 = nIn > 1 ? inputs[1] : inputs[0];
#if PT_SIDECHAIN_IN
  // VST3 instrument: the guitar arrives on the side-chain (channels 2-3) in most hosts, or on the
  // main input in hosts that feed instruments audio. Use whatever is connected, summed.
  {
    const bool m0 = IsChannelConnected(ERoute::kInput, 0), m1 = IsChannelConnected(ERoute::kInput, 1);
    const bool s0 = IsChannelConnected(ERoute::kInput, 2), s1 = IsChannelConnected(ERoute::kInput, 3);
    if (s0)
    {
      if ((int)mInMixL.size() < nFrames)
      {
        mInMixL.resize(nFrames);
        mInMixR.resize(nFrames);
      }
      const sample* mL = m0 ? inputs[0] : nullptr;
      const sample* mR = m1 ? inputs[1] : mL;
      const sample* sL = inputs[2];
      const sample* sR = s1 ? inputs[3] : sL;
      for (int i = 0; i < nFrames; i++)
      {
        mInMixL[i] = (mL ? mL[i] : 0.0) + sL[i];
        mInMixR[i] = (mR ? mR[i] : 0.0) + sR[i];
      }
      in1 = mInMixL.data();
      in2 = mInMixR.data();
      nIn = 2;
    }
    else
      nIn = m1 ? 2 : 1;
  }
#endif
  const int chunk = (int)mTrackIn.size();
  for (int s = 0; s < nFrames; s += chunk)
  {
    const int n = std::min(chunk, nFrames - s);
    _Process(in1 + s, in2 + s, outputs[0] + s, outputs[1] + s, n);
  }

  // meters: tracker input (after gain) and the output
  {
    const float g = DbToLin((float)GetParam(kInGain)->Value());
    const int src = GetParam(kTrackSource)->Int();
    mMeterIn.resize(nFrames);
    for (int i = 0; i < nFrames; i++)
    {
      const double a = in1[i], b = in2[i];
      mMeterIn[i] = g * (src == kSrcIn1 ? a : src == kSrcIn2 ? b : 0.5 * (a + b));
    }
    sample* ptrs[2] = {mMeterIn.data(), outputs[0]};
    mMeterSender.ProcessBlock(ptrs, nFrames, kCtrlTagMeter, 2);
  }

  // recorder
  if (mRecording.load())
  {
    const int64_t frames = (int64_t)mRecRing.size() / 2;
    int64_t w = mRecWrite.load();
    for (int i = 0; i < nFrames; i++, w++)
    {
      const int64_t k = (w % frames) * 2;
      mRecRing[k] = (float)in1[i];
      mRecRing[k + 1] = (float)(nIn > 1 ? in2[i] : 0.0);
    }
    mRecWrite.store(w);
  }
}

void PolyTrack::_Process(const sample* in1, const sample* in2, sample* outL, sample* outR, int n)
{
  const float gIn = DbToLin((float)GetParam(kInGain)->Value());
  const int tSrc = GetParam(kTrackSource)->Int();
  const int hSrc = GetParam(kHarmSource)->Int();
  for (int i = 0; i < n; i++)
  {
    const float a = (float)in1[i], b = (float)in2[i];
    mTrackIn[i] = gIn * (tSrc == kSrcIn1 ? a : tSrc == kSrcIn2 ? b : 0.5f * (a + b));
    mHarmIn[i] = hSrc == kSrcIn1 ? a : hSrc == kSrcIn2 ? b : 0.5f * (a + b);
  }

  // 1. track
  mEvents.clear();
  mTracker.Process(mTrackIn.data(), n, mEvents);

  // 2. MIDI out
  mMidiOut.clear();
  mMidiOut.insert(mMidiOut.end(), mMidiOutIn.begin(), mMidiOutIn.end());
  mMidiOutIn.clear();
  mNoteOut.BeginBlock(n, mMidiOut);
  for (const NoteEvent& e : mEvents)
  {
    mNoteOut.Event(e, mMidiOut);
    if (e.type == NoteEvent::kOn)
    {
      mTrackedMask.Set(e.note, true);
      if (mHarmFromGuitar)
        mHarm.NoteOn(e.note + mTranspose, e.velocity);
    }
    else if (e.type == NoteEvent::kOff)
    {
      mTrackedMask.Set(e.note, false);
      if (mHarmFromGuitar && !mKeyMask.Get(e.note + mTranspose))
        mHarm.NoteOff(e.note + mTranspose);
    }
  }
  mNoteOut.EndBlock(n);

  // envelope follower -> CC
  if (mEnvCC > 0)
  {
    float pk = 0.0f;
    for (int i = 0; i < n; i++)
      pk = std::max(pk, std::fabs(mTrackIn[i]));
    const float gate = (float)GetParam(kGate)->Value();
    const float db = LinToDb(pk);
    const float target = std::clamp((db - gate) / (-6.0f - gate), 0.0f, 1.0f) * 127.0f;
    mEnvSmooth += (target - mEnvSmooth) * (target > mEnvSmooth ? 0.6f : 0.15f);
    mEnvCounter += n;
    const int v = (int)std::lround(mEnvSmooth);
    if (mEnvCounter >= (int)(GetSampleRate() * 0.005) && v != mEnvLast)
    {
      mEnvCounter = 0;
      mEnvLast = v;
      const int ch = GetParam(kChanMode)->Int() == 1 ? 0 : GetParam(kChannel)->Int() - 1;
      mMidiOut.push_back({n - 1, (uint8_t)(0xB0 | ch), (uint8_t)mEnvCC, (uint8_t)v});
    }
  }

  // built-in synth mirrors what goes out (after hold / sustain / transpose)
  {
    int* chanNote = mChanNote;
    const bool perNote = GetParam(kChanMode)->Int() == 1;
    const float range = (float)GetParam(kBendRange)->Int() * 100.0f;
    for (const RawMidi& r : mMidiOut)
    {
      const int type = r.status & 0xF0, ch = r.status & 0x0F;
      if (type == 0x90 && r.d2 > 0)
      {
        mSynth.NoteOn(r.d1, r.d2);
        chanNote[ch] = r.d1;
      }
      else if (type == 0x80 || (type == 0x90 && r.d2 == 0))
        mSynth.NoteOff(r.d1);
      else if (type == 0xE0)
      {
        const float cents = ((float)((r.d2 << 7) | r.d1) - 8192.0f) / 8191.0f * range;
        if (perNote)
        {
          if (chanNote[ch] >= 0)
            mSynth.Bend(chanNote[ch], cents);
        }
        else
          mSynth.BendAll(cents);
      }
    }
  }

  if (mMidiOn)
    _SendRaw(mMidiOut);
  else
  {
    // keep RPN/CC housekeeping quiet; nothing goes out
  }
  for (const IMidiMsg& m : mThruQueue)
    SendMidiMsg(m);
  mThruQueue.clear();

  // 3. audio
  std::fill(mOutL.begin(), mOutL.begin() + n, 0.0f);
  std::fill(mOutR.begin(), mOutR.begin() + n, 0.0f);
  const float dry = _DbOrOff(GetParam(kDryLevel)->Value());
  if (dry > 0.0f)
    for (int i = 0; i < n; i++)
    {
      const float d = dry * (float)(in1[i] + (in2 != in1 ? in2[i] : 0.0));
      mOutL[i] += d;
      mOutR[i] += d;
    }
  mSynth.Process(mOutL.data(), mOutR.data(), n, _DbOrOff(GetParam(kSynthLevel)->Value()));
  if (GetParam(kHarmOn)->Bool())
    mHarm.Process(mHarmIn.data(), mOutL.data(), mOutR.data(), n, _DbOrOff(GetParam(kHarmLevel)->Value()));
  const float master = DbToLin((float)GetParam(kMaster)->Value());
  for (int i = 0; i < n; i++)
  {
    outL[i] = master * mOutL[i];
    outR[i] = master * mOutR[i];
  }

  mInLevelDb.store(mTracker.LevelDb());
  mVoiceHz.store(mHarm.InputVoiced() ? mHarm.InputHz() : 0.0f);
  mVoices.store(mHarm.ActiveVoices());
}

void PolyTrack::OnIdle()
{
  mMeterSender.TransmitData(*this);

  ParamChange pc;
  while (mParamQueue.Pop(pc))
    SetParameterValue(pc.idx, pc.normalized);

  _DrainRecording();
  _UpdateStatus();
}
#endif

void PolyTrack::_UpdateStatus()
{
  IGraphics* ui = GetUI();
  if (!ui)
    return;

  // notes line
  std::string notes = "Hearing: ";
  int count = 0;
  for (int n = 0; n < 128; n++)
    if (mTrackedMask.Get(n))
    {
      WDL_String s;
      NoteName(n, s);
      notes += s.Get();
      notes += "  ";
      count++;
    }
  if (!count)
    notes += "-";
  const float hz = mVoiceHz.load();
  char buf[160];
  if (GetParam(kHarmOn)->Bool())
  {
    if (hz > 0.0f)
      snprintf(buf, sizeof buf, "      Voice: %.0f Hz   Harmony voices: %d", hz, mVoices.load());
    else
      snprintf(buf, sizeof buf, "      Voice: -   Harmony voices: %d", mVoices.load());
    notes += buf;
  }

  // status line
  std::string status;
  const float lvl = mInLevelDb.load();
  snprintf(buf, sizeof buf, "Tracker input %.0f dBFS (aim for -30 to -12 while playing).  ", std::max(lvl, -99.0f));
  status += buf;
  if (mLearnArmed.load())
  {
    const int p = mLearnParam.load();
    if (p < 0)
      status += "MIDI LEARN: click or move a control on screen...";
    else
    {
      snprintf(buf, sizeof buf, "MIDI LEARN: now move a knob or press a footswitch on your controller for '%s'", GetParam(p)->GetName());
      status += buf;
    }
  }
  else if (mLearnedCC.load() >= 0 || mLearnShowFrames > 0)
  {
    const int cc = mLearnedCC.exchange(-1);
    if (cc >= 0)
    {
      const int p = mCCMap[cc].load();
      snprintf(mLearnMsg, sizeof mLearnMsg, "Mapped CC %d to '%s'.", cc, p >= 0 ? GetParam(p)->GetName() : "?");
      mLearnShowFrames = 150;
    }
    status += mLearnMsg;
    mLearnShowFrames--;
  }
  else if (mRecording.load())
  {
    snprintf(buf, sizeof buf, "RECORDING %.1f s -> %s", (double)mRecFrames / std::max(1, mRecSR), mRecPath.c_str());
    status += buf;
  }
  else if (!mRecPath.empty())
  {
    status += "Last recording: ";
    status += mRecPath;
  }

  if (auto* c = ui->GetControlWithTag(kCtrlTagNotes))
    if (notes != mLastNotes)
    {
      mLastNotes = notes;
      c->As<ITextControl>()->SetStr(notes.c_str());
      c->SetDirty(false);
    }
  if (auto* c = ui->GetControlWithTag(kCtrlTagStatus))
    if (status != mLastStatus)
    {
      mLastStatus = status;
      c->As<ITextControl>()->SetStr(status.c_str());
      c->SetDirty(false);
    }
  if (auto* c = ui->GetControlWithTag(kCtrlTagRec))
    c->As<IVButtonControl>()->SetLabelStr(mRecording.load() ? "STOP REC" : "REC INPUT");
  if (auto* c = ui->GetControlWithTag(kCtrlTagLearn))
    c->As<IVButtonControl>()->SetLabelStr(mLearnArmed.load() ? "LEARNING..." : "MIDI LEARN");
}

// ---- input recorder ----

void PolyTrack::_StartRecording()
{
  if (mRecFile)
    return;
  std::string dir;
#ifdef OS_WIN
  wchar_t docs[MAX_PATH] = {};
  if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs)))
  {
    std::wstring wdir = std::wstring(docs) + L"\\PolyTrack Recordings";
    CreateDirectoryW(wdir.c_str(), nullptr);
    char utf8[MAX_PATH * 3] = {};
    WideCharToMultiByte(CP_UTF8, 0, wdir.c_str(), -1, utf8, sizeof utf8, nullptr, nullptr);
    dir = utf8;
  }
#endif
  if (dir.empty())
    return;
  const std::time_t now = std::time(nullptr);
  std::tm tmv{};
#ifdef OS_WIN
  localtime_s(&tmv, &now);
#else
  localtime_r(&now, &tmv);
#endif
  char name[64];
  std::strftime(name, sizeof name, "take-%Y%m%d-%H%M%S.wav", &tmv);
  mRecPath = dir + "\\" + name;
#ifdef OS_WIN
  wchar_t wpath[MAX_PATH * 2] = {};
  MultiByteToWideChar(CP_UTF8, 0, mRecPath.c_str(), -1, wpath, MAX_PATH * 2);
  mRecFile = _wfopen(wpath, L"wb");
#else
  mRecFile = fopen(mRecPath.c_str(), "wb");
#endif
  if (!mRecFile)
    return;
  unsigned char header[44] = {};
  fwrite(header, 1, 44, mRecFile); // placeholder, filled at stop
  mRecFrames = 0;
  mRecRead = mRecWrite.load();
  mRecording.store(true);
}

void PolyTrack::_DrainRecording()
{
  if (!mRecFile)
    return;
  const int64_t frames = (int64_t)mRecRing.size() / 2;
  const int64_t w = mRecWrite.load();
  if (w - mRecRead > frames)
  {
    mRecDropped += (w - mRecRead) - frames;
    mRecRead = w - frames;
  }
  while (mRecRead < w)
  {
    const int64_t k = mRecRead % frames;
    const int64_t run = std::min(w - mRecRead, frames - k);
    fwrite(&mRecRing[(size_t)(k * 2)], sizeof(float) * 2, (size_t)run, mRecFile);
    mRecRead += run;
    mRecFrames += run;
  }
}

void PolyTrack::_StopRecording()
{
  if (!mRecFile)
    return;
  mRecording.store(false);
  _DrainRecording();
  auto put32 = [](unsigned char* p, uint32_t v) {
    p[0] = v & 255;
    p[1] = (v >> 8) & 255;
    p[2] = (v >> 16) & 255;
    p[3] = (v >> 24) & 255;
  };
  auto put16 = [](unsigned char* p, uint16_t v) {
    p[0] = v & 255;
    p[1] = (v >> 8) & 255;
  };
  const uint32_t dataBytes = (uint32_t)(mRecFrames * 8);
  unsigned char h[44];
  memcpy(h, "RIFF", 4);
  put32(h + 4, 36 + dataBytes);
  memcpy(h + 8, "WAVEfmt ", 8);
  put32(h + 16, 16);
  put16(h + 20, 3); // IEEE float
  put16(h + 22, 2);
  put32(h + 24, (uint32_t)mRecSR);
  put32(h + 28, (uint32_t)mRecSR * 8);
  put16(h + 32, 8);
  put16(h + 34, 32);
  memcpy(h + 36, "data", 4);
  put32(h + 40, dataBytes);
  fseek(mRecFile, 0, SEEK_SET);
  fwrite(h, 1, 44, mRecFile);
  fclose(mRecFile);
  mRecFile = nullptr;
}
