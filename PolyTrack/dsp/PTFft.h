// PTFft.h -- small radix-2 FFT used by the tracker (power-of-two sizes only).
// ForwardReal() transforms N real samples with an N/2-point complex FFT plus a split step.
#pragma once

#include <cmath>
#include <complex>
#include <vector>

namespace polytrack
{

class Fft
{
public:
  // n = real transform size
  void Init(int n)
  {
    mN = n;
    const int h = n / 2;
    mTw.resize(h / 2 > 0 ? h / 2 : 1);
    for (int i = 0; i < h / 2; i++)
      mTw[i] = std::polar(1.0f, (float)(-2.0 * 3.14159265358979323846 * i / h));
    mSplit.resize(h);
    for (int k = 0; k < h; k++)
      mSplit[k] = std::polar(1.0f, (float)(-2.0 * 3.14159265358979323846 * k / n));
    mRev.resize(h);
    int bits = 0;
    while ((1 << bits) < h)
      bits++;
    for (int i = 0; i < h; i++)
    {
      int r = 0;
      for (int b = 0; b < bits; b++)
        if (i & (1 << b))
          r |= 1 << (bits - 1 - b);
      mRev[i] = r;
    }
    mZ.resize(h);
  }

  int Size() const { return mN; }

  // x: n real samples (only the first `used` are non-zero). out: n/2 complex bins (0..n/2-1).
  void ForwardReal(const float* x, int used, std::complex<float>* out)
  {
    const int h = mN / 2;
    for (int k = 0; k < h; k++)
    {
      const int i0 = 2 * k, i1 = 2 * k + 1;
      mZ[k] = {i0 < used ? x[i0] : 0.0f, i1 < used ? x[i1] : 0.0f};
    }
    _Complex(mZ.data(), h);
    // split: X[k] = E[k] + W^k O[k], E = (Z[k] + conj(Z[h-k]))/2, O = (Z[k] - conj(Z[h-k]))/(2i)
    for (int k = 0; k < h; k++)
    {
      const std::complex<float> a = mZ[k];
      const std::complex<float> b = std::conj(mZ[(h - k) & (h - 1)]);
      const std::complex<float> e = 0.5f * (a + b);
      const std::complex<float> d = 0.5f * (a - b);
      const std::complex<float> o(d.imag(), -d.real()); // d / i
      out[k] = e + mSplit[k] * o;
    }
  }

private:
  void _Complex(std::complex<float>* x, int n) const
  {
    for (int i = 0; i < n; i++)
      if (i < mRev[i])
        std::swap(x[i], x[mRev[i]]);
    float* f = reinterpret_cast<float*>(x);
    for (int len = 2; len <= n; len <<= 1)
    {
      const int half = len >> 1;
      const int step = n / len;
      for (int i = 0; i < n; i += len)
      {
        for (int j = 0; j < half; j++)
        {
          const float wr = mTw[j * step].real(), wi = mTw[j * step].imag();
          float* u = f + 2 * (i + j);
          float* v = f + 2 * (i + j + half);
          const float vr = v[0] * wr - v[1] * wi;
          const float vi = v[0] * wi + v[1] * wr;
          v[0] = u[0] - vr;
          v[1] = u[1] - vi;
          u[0] += vr;
          u[1] += vi;
        }
      }
    }
  }

  int mN = 0;
  std::vector<std::complex<float>> mTw, mSplit, mZ;
  std::vector<int> mRev;
};

} // namespace polytrack
