#!/usr/bin/env python3
"""Compares two raw stereo float32 renders (mdmfxref and the full-system emulation, mdProbe wav:): finds the lag and the gain
(sign included) that fit best from the first sound on, then reports the correlation and the residual.
    mfx_compare.py ours.raw ref.raw [window_frames=40000] [skip_frames=0] [max_lag=400]
"""
import sys
import numpy as np

def load(path):
    a = np.fromfile(path, dtype='<f4')
    return a[0::2].astype(np.float64), a[1::2].astype(np.float64)

def onset(x, thr=2e-4):
    i = np.flatnonzero(np.abs(x) > thr)
    return int(i[0]) if len(i) else -1

def main():
    ours, ref = load(sys.argv[1]), load(sys.argv[2])
    win = int(sys.argv[3]) if len(sys.argv) > 3 else 40000
    skip = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    maxlag = int(sys.argv[5]) if len(sys.argv) > 5 else 400
    for ch, name in ((0, 'L'), (1, 'R')):
        a, b = ours[ch], ref[ch]
        oa, ob = onset(a), onset(b)
        if oa < 0 or ob < 0: print(name, 'silent'); continue
        n = min(win, len(a) - oa - maxlag - 1, len(b) - ob - maxlag - 1)
        sb = b[ob + skip: ob + skip + n]
        best = None
        for lag in range(-maxlag, maxlag + 1):
            start = oa + skip + lag
            if start < 0: continue
            sa = a[start: start + n]
            c = float(sa @ sb) / (np.sqrt(float(sa @ sa) * float(sb @ sb)) or 1.0)
            if best is None or abs(c) > abs(best[0]): best = (c, lag)
        c, lag = best
        sa = a[oa + skip + lag: oa + skip + lag + n]
        g = float(sa @ sb) / float(sb @ sb)
        res = sa - g * sb
        print('%s: onset ours %d ref %d, lag %d, correlation %.6f, gain ours/ref %.5f, residual %.1f dB re signal, max |diff| %.6f (peak %.4f)' %
              (name, oa, ob, lag, c, g, 20 * np.log10(np.sqrt(np.mean(res ** 2)) / (abs(g) * np.sqrt(np.mean(sb ** 2))) + 1e-15), np.max(np.abs(res)), np.max(np.abs(sa))))

main()
