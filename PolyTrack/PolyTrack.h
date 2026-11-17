#pragma once

#include <array>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "IPlug_include_in_plug_hdr.h"
#include "IPlugQueue.h"
#include "ISender.h"

#include "dsp/Harmonizer.h"
#include "dsp/MonitorSynth.h"
#include "dsp/NoteOutput.h"
#include "dsp/PolyTracker.h"
#include "PTControls.h"

const int kNumPresets = 1;

enum EParams
{
  // tracker
  kTrackSource = 0,
  kInGain,
  kGate,
  kSens,
  kMaxNotes,
  kLowNote,
  kBendTrack,
  kVelRange,
  // MIDI out
  kMidiOn,
  kChanMode,
  kChannel,
  kBendRange,
  kTranspose,
  kVelFixed,
  kVelCurve,
  kHold,
  kInfinite,
  kThru,
  kEnvCC,
  // harmonizer
  kHarmOn,
  kHarmSource,
  kChordSource,
  kHarmVoices,
  kFormant,
  kDetune,
  kSpread,
  kHarmAttack,
  kHarmRelease,
  kHarmLevel,
  // output
  kDryLevel,
  kSynthLevel,
  kMaster,
  kStrictOct, // added 0.1.1 (saved states from 0.1.0 have one parameter fewer)
  kNumParams
};

enum ECtrlTags
{
  kCtrlTagMeter = 0,
  kCtrlTagStatus,
  kCtrlTagNotes,
  kCtrlTagLearn,
  kCtrlTagRec,
  kCtrlTagKeyboard
};

enum ESource
{
  kSrcIn1 = 0,
  kSrcIn2,
  kSrcSum
};

enum EChordSource
{
  kChordKeys = 0,
  kChordGuitar,
  kChordBoth
};

using namespace iplug;
using namespace igraphics;

class PolyTrack final : public Plugin
{
public:
  PolyTrack(const InstanceInfo& info);
  ~PolyTrack();

#if IPLUG_EDITOR
  void OnParentWindowResize(int width, int height) override;
  bool OnHostRequestingSupportedViewConfiguration(int width, int height) override { return true; }
  void OnParamChangeUI(int paramIdx, EParamSource source) override;
#endif

#if IPLUG_DSP
  void ProcessBlock(sample** inputs, sample** outputs, int nFrames) override;
  void ProcessMidiMsg(const IMidiMsg& msg) override;
  void OnReset() override;
  void OnIdle() override;
#endif

  bool SerializeState(IByteChunk& chunk) const override;
  int UnserializeState(const IByteChunk& chunk, int startPos) override;

private:
  struct ParamChange
  {
    int idx;
    double normalized;
  };

  void _ReadSettings();
  void _Process(const sample* in1, const sample* in2, sample* outL, sample* outR, int n);
  void _SendRaw(const std::vector<polytrack::RawMidi>& v);
  void _StartRecording();
  void _StopRecording();
  void _DrainRecording();
  void _UpdateStatus();
  static float _DbOrOff(double db) { return db <= -59.9 ? 0.0f : polytrack::DbToLin((float)db); }

  polytrack::PolyTracker mTracker;
  polytrack::NoteOutput mNoteOut;
  polytrack::Harmonizer mHarm;
  polytrack::MonitorSynth mSynth;

  std::vector<float> mTrackIn, mHarmIn, mOutL, mOutR;
  std::vector<sample> mInMixL, mInMixR; // main input + side-chain (VST3 instrument)
  std::vector<polytrack::NoteEvent> mEvents;
  std::vector<polytrack::RawMidi> mMidiOut;   // audio thread
  std::vector<polytrack::RawMidi> mMidiOutIn; // produced inside ProcessMidiMsg
  std::vector<IMidiMsg> mThruQueue;

  int mTranspose = 0;
  bool mHarmFromKeys = true, mHarmFromGuitar = false;
  bool mMidiOn = true, mThru = true;
  int mEnvCC = 0, mEnvLast = -1, mEnvCounter = 0;
  float mEnvSmooth = 0.0f;

  // display
  ptui::NoteMask mTrackedMask, mKeyMask;
  std::atomic<float> mInLevelDb{-120.0f};
  std::atomic<float> mVoiceHz{0.0f};
  std::atomic<int> mVoices{0};
  IPeakSender<2> mMeterSender;
  std::vector<sample> mMeterIn;
  std::string mLastNotes, mLastStatus;
  char mLearnMsg[160] = {};
  int mLearnShowFrames = 0;
  int mChanNote[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

  // MIDI learn: CC number -> parameter index
  std::array<std::atomic<int>, 128> mCCMap;
  std::atomic<bool> mLearnArmed{false};
  std::atomic<int> mLearnParam{-1};
  std::atomic<int> mLearnedCC{-1}; // set by the audio thread when a mapping is made
  IPlugQueue<ParamChange> mParamQueue{256};

  // input recorder (for tuning the tracker with real playing)
  std::vector<float> mRecRing;
  std::atomic<int64_t> mRecWrite{0};
  int64_t mRecRead = 0;
  std::atomic<bool> mRecording{false};
  FILE* mRecFile = nullptr;
  int64_t mRecFrames = 0;
  std::string mRecPath;
  int mRecSR = 48000;
  std::atomic<int64_t> mRecDropped{0};
};
