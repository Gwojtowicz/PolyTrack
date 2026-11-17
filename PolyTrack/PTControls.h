// PTControls.h -- PolyTrack UI pieces.
#pragma once

#include <atomic>
#include <cstdint>

#include "IControl.h"

using namespace iplug;
using namespace igraphics;

namespace ptui
{
const IColor kBg(255, 20, 22, 27);
const IColor kPanel(255, 30, 33, 40);
const IColor kPanelEdge(255, 52, 56, 66);
const IColor kInk(255, 226, 229, 236);
const IColor kDim(255, 140, 146, 158);
const IColor kAccent(255, 255, 150, 60);  // tracked guitar notes
const IColor kAccent2(255, 80, 200, 230); // harmony keys

// 128-bit note mask shared between the audio thread (writer) and the UI (reader).
struct NoteMask
{
  std::atomic<uint64_t> w[2]{{0}, {0}};
  void Set(int n, bool on)
  {
    if (n < 0 || n > 127)
      return;
    const uint64_t bit = 1ull << (n & 63);
    if (on)
      w[n >> 6].fetch_or(bit, std::memory_order_relaxed);
    else
      w[n >> 6].fetch_and(~bit, std::memory_order_relaxed);
  }
  bool Get(int n) const { return (w[n >> 6].load(std::memory_order_relaxed) >> (n & 63)) & 1ull; }
  void Clear()
  {
    w[0].store(0);
    w[1].store(0);
  }
};

// Piano strip: orange = notes the guitar tracker hears, blue = held harmony keys.
// Clicking a key plays it as a harmony key (MIDI note to the plugin).
class KeyboardControl : public IControl
{
public:
  KeyboardControl(const IRECT& bounds, int lo, int hi, const NoteMask* tracked, const NoteMask* keys)
  : IControl(bounds)
  , mLo(lo)
  , mHi(hi)
  , mTracked(tracked)
  , mKeys(keys)
  {
  }

  static bool IsBlack(int n)
  {
    const int k = n % 12;
    return k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
  }

  int NumWhite() const
  {
    int c = 0;
    for (int n = mLo; n <= mHi; n++)
      c += IsBlack(n) ? 0 : 1;
    return c;
  }

  IRECT KeyRect(int note) const
  {
    const float ww = mRECT.W() / NumWhite();
    int wi = 0;
    for (int n = mLo; n < note; n++)
      wi += IsBlack(n) ? 0 : 1;
    if (!IsBlack(note))
      return IRECT(mRECT.L + wi * ww, mRECT.T, mRECT.L + (wi + 1) * ww, mRECT.B);
    const float x = mRECT.L + wi * ww;
    return IRECT(x - ww * 0.32f, mRECT.T, x + ww * 0.32f, mRECT.T + mRECT.H() * 0.6f);
  }

  void Draw(IGraphics& g) override
  {
    uint64_t sig[4] = {mTracked->w[0].load(), mTracked->w[1].load(), mKeys->w[0].load(), mKeys->w[1].load()};
    std::copy(sig, sig + 4, mLastSig);
    for (int pass = 0; pass < 2; pass++)
    {
      for (int n = mLo; n <= mHi; n++)
      {
        const bool black = IsBlack(n);
        if ((pass == 0) == black)
          continue;
        const IRECT r = KeyRect(n);
        const bool t = mTracked->Get(n), k = mKeys->Get(n) || n == mMouseNote;
        IColor c = black ? IColor(255, 28, 30, 36) : IColor(255, 214, 218, 226);
        if (t && k)
          c = IColor::LinearInterpolateBetween(ptui::kAccent, ptui::kAccent2, 0.5f);
        else if (t)
          c = ptui::kAccent;
        else if (k)
          c = ptui::kAccent2;
        g.FillRect(c, r.GetPadded(black ? 0.f : -0.5f));
        g.DrawRect(IColor(255, 10, 11, 14), r);
        if (!black && n % 12 == 0)
        {
          WDL_String s;
          s.SetFormatted(8, "C%d", n / 12 - 1);
          g.DrawText(IText(11.f, IColor(255, 90, 94, 104), nullptr, EAlign::Center, EVAlign::Bottom), s.Get(),
                     r.GetPadded(-2.f));
        }
      }
    }
  }

  bool IsDirty() override
  {
    const uint64_t sig[4] = {mTracked->w[0].load(), mTracked->w[1].load(), mKeys->w[0].load(), mKeys->w[1].load()};
    for (int i = 0; i < 4; i++)
      if (sig[i] != mLastSig[i])
        return true;
    return IControl::IsDirty();
  }

  int NoteAt(float x, float y) const
  {
    for (int n = mLo; n <= mHi; n++)
      if (IsBlack(n) && KeyRect(n).Contains(x, y))
        return n;
    for (int n = mLo; n <= mHi; n++)
      if (!IsBlack(n) && KeyRect(n).Contains(x, y))
        return n;
    return -1;
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    mMouseNote = NoteAt(x, y);
    if (mMouseNote >= 0)
    {
      IMidiMsg m;
      m.MakeNoteOnMsg(mMouseNote, 100, 0);
      GetDelegate()->SendMidiMsgFromUI(m);
    }
    SetDirty(false);
  }

  void OnMouseUp(float x, float y, const IMouseMod& mod) override
  {
    if (mMouseNote >= 0)
    {
      IMidiMsg m;
      m.MakeNoteOffMsg(mMouseNote, 0);
      GetDelegate()->SendMidiMsgFromUI(m);
    }
    mMouseNote = -1;
    SetDirty(false);
  }

private:
  int mLo, mHi;
  const NoteMask* mTracked;
  const NoteMask* mKeys;
  int mMouseNote = -1;
  uint64_t mLastSig[4] = {};
};

// Rounded panel with a title.
class PanelControl : public IControl
{
public:
  PanelControl(const IRECT& bounds, const char* title)
  : IControl(bounds)
  , mTitle(title)
  {
    mIgnoreMouse = true;
  }
  void Draw(IGraphics& g) override
  {
    g.FillRoundRect(kPanel, mRECT, 8.f);
    g.DrawRoundRect(kPanelEdge, mRECT, 8.f);
    g.DrawText(IText(15.f, kAccent, "Roboto-Regular", EAlign::Near, EVAlign::Middle), mTitle.Get(),
               mRECT.GetFromTop(28.f).GetHPadded(-12.f));
  }

private:
  WDL_String mTitle;
};
} // namespace ptui
