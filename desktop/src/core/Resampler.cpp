// From shnolk's Monomodule (https://github.com/shnolk/monomodule, src/core/dsp/, commit eb5cfee), AGPL-3.0, unchanged.
#include "Resampler.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace mnm::dsp {

namespace {
// floor(a / b) for b > 0 and any sign of a
int64_t floorDiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

bool wholeNumber(double r) { return r >= 1.0 && std::abs(r - std::round(r)) < 1e-6; }

double besselI0(double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 64; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}
} // namespace

// ---- RateMap ----

void RateMap::set(double numRate, double denRate)
{
    m_exact = wholeNumber(numRate) && wholeNumber(denRate);
    if (m_exact) {
        const int64_t a = int64_t(std::llround(numRate)), b = int64_t(std::llround(denRate)), g = std::gcd(a, b);
        m_num = a / g; m_den = b / g;
    }
    m_ratio = static_cast<long double>(numRate) / static_cast<long double>(denRate);
}

RateMap::Position RateMap::at(int64_t m) const
{
    Position r;
    if (m_exact) {
        const int64_t p = m * m_num;
        r.index = floorDiv(p, m_den);
        r.phase = p - r.index * m_den;
        r.frac = double(r.phase) / double(m_den);
        return r;
    }
    const long double p = static_cast<long double>(m) * m_ratio, f = std::floor(p);
    r.index = int64_t(f);
    r.frac = double(p - f);
    return r;
}

// ---- SincKernel ----

void SincKernel::design(double inRate, double outRate)
{
    // Kaiser design for 100 dB: beta = 0.1102 (A - 8.7); length (A - 8) / (2.285 * transition in radians/sample)
    constexpr double kAttenuationDb = 100.0, kPass = 0.4535, kStop = 0.5465;
    const double lower = std::min(inRate, outRate);
    const double cutoff = 0.5 * lower;                                   // Hz: the lower Nyquist
    const double transition = (kStop - kPass) * lower;                   // Hz
    const double beta = 0.1102 * (kAttenuationDb - 8.7);
    const double length = (kAttenuationDb - 8.0) / (2.285 * 2.0 * 3.14159265358979323846 * transition / inRate);
    m_half = std::max(2, int(std::ceil(length / 2.0)));
    const int points = 2 * m_half * kTableSteps + 1;
    m_table.assign(size_t(points) + 1, 0.0f);                            // + a zero guard for the interpolation
    const double scale = 2.0 * cutoff / inRate, i0Beta = besselI0(beta);
    for (int j = 0; j < points; ++j) {
        const double d = double(j) / kTableSteps - m_half;               // offset in input samples
        const double x = d / m_half;
        const double w = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - x * x))) / i0Beta;
        const double t = 3.14159265358979323846 * scale * d;
        m_table[size_t(j)] = float(scale * (t == 0.0 ? 1.0 : std::sin(t) / t) * w);
    }
    // every phase's taps, when there are few: output m lies at m * in / out input samples, a fraction r / (out / g)
    constexpr int64_t kMaxPhases = 4096;
    m_phases.clear();
    m_phaseCount = 0;
    if (wholeNumber(inRate) && wholeNumber(outRate)) {
        const int64_t a = int64_t(std::llround(inRate)), b = int64_t(std::llround(outRate)), phases = b / std::gcd(a, b);
        if (phases <= kMaxPhases) {
            m_phases.resize(size_t(phases) * size_t(numTaps()));
            for (int64_t r = 0; r < phases; ++r) taps(double(r) / double(phases), m_phases.data() + size_t(r) * size_t(numTaps()));
            m_phaseCount = size_t(phases);
        }
    }
}

void SincKernel::taps(double frac, float* out) const
{
    // tap k weighs input sample i - H + 1 + k at offset d = k - H + 1 - frac from the output position; its table
    // coordinate is (d + H) * steps = (k + 1 - frac) * steps
    const int n = numTaps();
    double sum = 0.0;
    for (int k = 0; k < n; ++k) {
        const double pos = (double(k) + 1.0 - frac) * kTableSteps;
        const int j = int(pos);
        const double t = pos - j;
        const double v = double(m_table[size_t(j)]) * (1.0 - t) + double(m_table[size_t(j) + 1]) * t;
        out[k] = float(v);
        sum += v;
    }
    const float g = sum != 0.0 ? float(1.0 / sum) : 1.0f;
    for (int k = 0; k < n; ++k) out[k] *= g;
}

// ---- History ----

void History::prepare(int capacity)
{
    m_cap = 1;
    while (m_cap < capacity) m_cap <<= 1;
    m_mask = m_cap - 1;
    m_buf.assign(size_t(2 * m_cap), 0.0f);
    m_end = 0;
}

void History::clear()
{
    std::fill(m_buf.begin(), m_buf.end(), 0.0f);
    m_end = 0;
}

void History::push(const float* x, int n)
{
    for (int k = 0; k < n; ++k, ++m_end) {
        const auto i = size_t(m_end & m_mask);
        m_buf[i] = x[k];
        m_buf[i + size_t(m_cap)] = x[k];
    }
}

void History::pushZeros(int n)
{
    for (int k = 0; k < n; ++k, ++m_end) {
        const auto i = size_t(m_end & m_mask);
        m_buf[i] = 0.0f;
        m_buf[i + size_t(m_cap)] = 0.0f;
    }
}

float History::dot(int64_t first, const float* taps, int n) const
{
    if (first < 0) {   // before the stream started: zeros
        const int skip = int(std::min<int64_t>(n, -first));
        taps += skip; n -= skip; first += skip;
    }
    const float* x = m_buf.data() + (first & m_mask);
    float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;   // four independent sums: a vectorisable loop, not one long chain
    int k = 0;
    for (; k + 4 <= n; k += 4) {
        a0 += taps[k] * x[k];
        a1 += taps[k + 1] * x[k + 1];
        a2 += taps[k + 2] * x[k + 2];
        a3 += taps[k + 3] * x[k + 3];
    }
    for (; k < n; ++k) a0 += taps[k] * x[k];
    return (a0 + a1) + (a2 + a3);
}

} // namespace mnm::dsp
