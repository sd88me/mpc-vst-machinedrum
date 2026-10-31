// From shnolk's Monomodule (https://github.com/shnolk/monomodule, src/core/dsp/, commit eb5cfee), AGPL-3.0, unchanged.
// Band-limited sample-rate conversion between the engine's 44.1 kHz and a host's rate (issue #3). The emulated DSP only
// ever runs at the Monomachine's 44.1 kHz; at any other host rate its outputs, and the FX machines' inputs, cross this
// converter. Three parts, used in pull form by the plugin processor and the preview player:
//
//   RateMap     where output sample m lies on the input's timeline: m * inRate / outRate, as an index and a fraction.
//               For whole-number rates it is computed in integers, so the engine frames rendered and the frames the
//               converter consumes can never drift apart. (1.0.0 rounded the frames per block up and dropped the
//               unconsumed fraction at every block boundary: the rhythmic crackle and the slight sharpness at
//               48/96 kHz.)
//   SincKernel  the filter: a Kaiser-windowed sinc cut at the lower rate's Nyquist, flat within 0.0001 dB up to
//               0.4535 x the lower rate (20 kHz at 44.1 kHz) and 100 dB down from 0.5465 x it (24.1 kHz). What it lets
//               through between the two only aliases into that same 20-24.1 kHz band, as with the transition band of
//               the hardware's own DAC filter. For an output at input position i + frac it gives the 2H taps that
//               weigh input samples i-H+1 .. i+H.
//   History     a sample history addressed by absolute index (a mirrored power-of-two ring, so every tap window is
//               one contiguous run), and its dot product with a tap set.
#pragma once
#include <cstdint>
#include <vector>

namespace mnm::dsp {

class RateMap {
public:
    // floor(position), position - floor, and for whole-number rates the fraction's numerator over the reduced output
    // rate (its "phase": the same phase always means the same taps), else -1
    struct Position { int64_t index = 0; double frac = 0.0; int64_t phase = -1; };
    // position(m) = m * numRate / denRate
    void set(double numRate, double denRate);
    Position at(int64_t m) const;   // m may be negative
    int64_t floorAt(int64_t m) const { return at(m).index; }
    int64_t ceilAt(int64_t m) const { const auto p = at(m); return p.frac > 0.0 ? p.index + 1 : p.index; }

private:
    bool m_exact = true;
    int64_t m_num = 1, m_den = 1;
    long double m_ratio = 1.0L;
};

class SincKernel {
public:
    static constexpr int kTableSteps = 512;   // table points per input sample (linear interpolation: < -110 dB)
    // Builds the filter for inRate -> outRate; allocates, so not on the audio thread. For whole-number rates whose
    // output positions take few distinct fractions (160 at 44.1 -> 48 kHz, 640 at 44.1 -> 192 kHz) the taps of every
    // fraction are worked out here, once.
    void design(double inRate, double outRate);
    bool ready() const { return m_half > 0; }
    int halfTaps() const { return m_half; }   // H, in input samples
    int numTaps() const { return 2 * m_half; }
    // out[k] (k < numTaps) weighs input sample i - H + 1 + k for an output at input position i + frac, 0 <= frac < 1.
    // The taps are normalised to sum to 1 (exact DC gain at every fraction).
    void taps(double frac, float* out) const;
    // The taps for a position from the RateMap of the same rates (in, out): the stored set for its phase, else worked
    // out into `scratch` (numTaps floats). Either way the same values as taps(p.frac).
    const float* taps(const RateMap::Position& p, float* scratch) const
    {
        if (p.phase >= 0 && size_t(p.phase) < m_phaseCount) return m_phases.data() + size_t(p.phase) * size_t(numTaps());
        taps(p.frac, scratch);
        return scratch;
    }

private:
    int m_half = 0;
    std::vector<float> m_table;    // the windowed sinc at offsets (j / kTableSteps) - H input samples, j = 0 .. 2*H*steps
    std::vector<float> m_phases;   // taps of phase r at r * numTaps, r < m_phaseCount
    size_t m_phaseCount = 0;
};

class History {
public:
    // Room for at least `capacity` samples; empties the history (every sample reads 0) and restarts it at index 0.
    void prepare(int capacity);
    void clear();
    void push(const float* x, int n);   // appends n samples at the next absolute indices
    void pushZeros(int n);
    int64_t end() const { return m_end; }   // one past the newest sample
    int capacity() const { return m_cap; }
    // Sum of taps[k] * sample(first + k), k < n. Samples before index 0 read 0; the caller keeps the window within
    // the last capacity() samples.
    float dot(int64_t first, const float* taps, int n) const;

private:
    std::vector<float> m_buf;   // 2 x capacity: sample i is stored at (i & mask) and (i & mask) + capacity
    int m_cap = 0;
    int64_t m_mask = 0, m_end = 0;
};

} // namespace mnm::dsp
