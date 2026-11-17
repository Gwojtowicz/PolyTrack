import numpy as np, sys
SR = 48000
def f0_of(x):
    # harmonic-sum pitch estimate on a 0.5 s steady segment
    seg = x[int(0.6*SR):int(1.1*SR)]
    seg = seg * np.hanning(len(seg))
    S = np.abs(np.fft.rfft(seg, 1 << 19)); f = np.fft.rfftfreq(1 << 19, 1/SR)
    best, bf = 0, 0
    for c in np.arange(60, 1400, 0.25):
        idx = [int(round(c*h*len(S)*2/SR)) for h in range(1, 6) if c*h < 5000]
        s = sum(S[i] for i in idx) / len(idx)
        if s > best: best, bf = s, c
    return bf

def formants(x):
    seg = x[int(0.6*SR):int(1.1*SR)]
    S = np.abs(np.fft.rfft(seg*np.hanning(len(seg)), 1 << 16)); f = np.fft.rfftfreq(1 << 16, 1/SR)
    # spectral envelope: max in 150 Hz bins then smooth
    env = np.array([S[(f >= a) & (f < a+150)].max() for a in range(100, 3500, 150)])
    return 100 + 150*np.argmax(env[:7]) + 75

inp = np.fromfile('h_in.raw', np.float32)
print('input f0 %.2f Hz, env peak ~%d Hz' % (f0_of(inp), formants(inp)))
for key in [48, 55, 60, 64, 67, 72, 79]:
    y = np.fromfile('h_%d.raw' % key, np.float32)
    want = 440 * 2 ** ((key - 69) / 12)
    got = f0_of(y)
    cents = 1200*np.log2(got/want)
    lvl = 20*np.log10(np.sqrt(np.mean(y[int(0.6*SR):int(1.1*SR)]**2)) / np.sqrt(np.mean(inp[int(0.6*SR):int(1.1*SR)]**2)))
    print('key %d want %.1f got %.2f (%+.1f c) level %+.1f dB env peak ~%d Hz' % (key, want, got, cents, lvl, formants(y)))
# latency: first output above -40 dB rel steady vs input onset
y = np.fromfile('h_60.raw', np.float32)
thr = np.abs(y[int(0.6*SR):]).max() * 0.1
print('first output sample above 10%% of steady peak: %.1f ms' % (np.argmax(np.abs(y) > thr) / SR * 1000))
