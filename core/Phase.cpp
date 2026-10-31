#include "Phase.h"
#include <algorithm>
#include <cmath>

namespace dzod::arranger
{
namespace
{
// огибающая энергии полосы: моно -> полосовой фильтр (2 каскада) -> квадрат, шаг 1 мс (сумма)
struct Band
{
    std::vector<float> e; double rate = 1000;      // точек в секунду
    double at (double t, double halfWin) const
    {
        const int a = juce::jmax (0, (int) ((t - halfWin) * rate)), b = juce::jmin ((int) e.size(), (int) ((t + halfWin) * rate) + 1);
        double s = 0; for (int i = a; i < b; ++i) s += e[(size_t) i];
        return s;
    }
};

Band band (const juce::AudioBuffer<float>& in, double sr, double lo, double hi)
{
    const int n = in.getNumSamples();
    std::vector<float> x ((size_t) n);
    for (int c = 0; c < in.getNumChannels(); ++c) { auto* p = in.getReadPointer (c); for (int i = 0; i < n; ++i) x[(size_t) i] += p[i] / (float) in.getNumChannels(); }
    juce::IIRFilter f[4];
    if (hi > 0)
    {
        const double fc = std::sqrt (lo * hi), q = fc / (hi - lo);
        for (auto& k : f) k.setCoefficients (juce::IIRCoefficients::makeBandPass (sr, fc, q));
    }
    else for (auto& k : f) k.setCoefficients (juce::IIRCoefficients::makeHighPass (sr, lo, 0.707));
    for (int k = 0; k < 2; ++k) f[k].processSamples (x.data(), n);
    Band b; const int hop = juce::jmax (1, (int) (sr / 1000));
    b.rate = sr / hop;                      // точная частота точек (44,1 кГц: шаг 44 отсчёта — не ровно 1 мс, иначе к концу песни сдвиг 0,3 с)
    b.e.assign ((size_t) (n / hop + 1), 0.0f);
    for (int i = 0; i < n; ++i) b.e[(size_t) (i / hop)] += x[(size_t) i] * x[(size_t) i];
    return b;
}

double db (double e) { return 10.0 * std::log10 (e + 1e-12); }

struct Bands { Band kick, snare, crash; };
Bands bandsOf (const juce::AudioBuffer<float>& d, double sr) { return { band (d, sr, 40, 90), band (d, sr, 180, 280), band (d, sr, 6000, 0) }; }

PhaseCheck measure (const Bands& B, const std::vector<double>& grid, const std::vector<double>& bars)
{
    PhaseCheck r; r.checked = true;
    if (grid.size() < 8) return r;
    double kOn = 0, kOff = 0, sOn = 0, sOff = 0;
    for (size_t i = 0; i + 1 < grid.size(); ++i)
    {
        const double d = grid[i + 1] - grid[i], w = juce::jmin (0.03, d / 8);
        kOn += B.kick.at (grid[i], w); kOff += B.kick.at (grid[i] + d / 2, w);
        sOn += B.snare.at (grid[i], w); sOff += B.snare.at (grid[i] + d / 2, w);
    }
    r.kickOnDb = db (kOn); r.kickOffDb = db (kOff); r.snareOnDb = db (sOn); r.snareOffDb = db (sOff);
    // по тактам: бочка на долях сильнее, чем между ними; на первой доле — бочка или тарелка
    int barsN = 0, kickOnBars = 0, startBars = 0;
    for (size_t b = 0; b + 1 < bars.size(); ++b)
    {
        double on = 0, off = 0;
        for (size_t i = 0; i + 1 < grid.size(); ++i)
        {
            if (grid[i] < bars[b] - 1e-3 || grid[i] >= bars[b + 1] - 1e-3) continue;
            const double d = grid[i + 1] - grid[i], w = juce::jmin (0.03, d / 8);
            on += B.kick.at (grid[i], w); off += B.kick.at (grid[i] + d / 2, w);
        }
        if (on + off <= 0) continue;
        ++barsN; if (on > off) ++kickOnBars;
        const double d = (bars[b + 1] - bars[b]) / 4, w = juce::jmin (0.03, d / 8);
        const double k0 = B.kick.at (bars[b], w) + B.crash.at (bars[b], w) * 4;
        double kRest = 0; for (int q = 1; q < 4; ++q) kRest += B.kick.at (bars[b] + q * d, w) + B.crash.at (bars[b] + q * d, w) * 4;
        if (k0 >= kRest / 3) ++startBars;
    }
    r.kickOnBeatsPct = barsN > 0 ? 100.0 * kickOnBars / barsN : 0;
    r.kickOnBarStartPct = barsN > 0 ? 100.0 * startBars / barsN : 0;
    return r;
}

int nearestIndex (const std::vector<double>& g, double t)
{
    int best = 0; for (int i = 0; i < (int) g.size(); ++i) if (std::abs (g[(size_t) i] - t) < std::abs (g[(size_t) best] - t)) best = i;
    return best;
}
} // namespace

PhaseCheck measurePhase (const juce::AudioBuffer<float>& d, double sr, const std::vector<double>& grid, const std::vector<double>& bars)
{
    return measure (bandsOf (d, sr), grid, bars);
}

PhaseCheck checkPhase (const juce::AudioBuffer<float>& d, double sr, std::vector<double>& grid, std::vector<double>& bars, double musicStart)
{
    const auto B = bandsOf (d, sr);
    auto r = measure (B, grid, bars);
    if (grid.size() < 8) return r;

    // 1) доли: бочка между долями на 3+ дБ сильнее, а малый на долях не слабее — сетка на полдоли мимо
    if (r.kickOffDb - r.kickOnDb > 3.0 && r.snareOnDb >= r.snareOffDb - 1.0)
    {
        std::vector<double> g2;
        if (grid[0] - (grid[1] - grid[0]) / 2 >= 0) g2.push_back (grid[0] - (grid[1] - grid[0]) / 2);
        for (size_t i = 0; i + 1 < grid.size(); ++i) g2.push_back ((grid[i] + grid[i + 1]) / 2);
        grid = g2; r.halfShifted = true;
    }

    // 1б) сетка начинается позже музыки (у Stemmer первая доля бывает пропущена) — достроить назад до начала музыки
    while (grid.size() >= 2 && grid[0] - (grid[1] - grid[0]) >= musicStart - 0.03 && grid[0] - (grid[1] - grid[0]) >= -0.03)
        grid.insert (grid.begin(), juce::jmax (0.0, grid[0] - (grid[1] - grid[0])));

    // 2) такты: доля начала такта — где бочка + тарелка (тарелка весит больше: crash на «раз»)
    std::vector<double> s (grid.size(), 0.0), kick (grid.size()), crash (grid.size());
    for (size_t i = 0; i < grid.size(); ++i)
    {
        const double d = i + 1 < grid.size() ? grid[i + 1] - grid[i] : grid[i] - grid[i - 1], w = juce::jmin (0.03, d / 8);
        kick[i] = db (B.kick.at (grid[i], w)); crash[i] = db (B.crash.at (grid[i], w));
    }
    auto med = [] (std::vector<double> v) { std::sort (v.begin(), v.end()); return v[v.size() / 2]; };
    const double mk = med (kick), mc = med (crash);
    for (size_t i = 0; i < grid.size(); ++i) s[i] = juce::jlimit (-20.0, 20.0, kick[i] - mk) + 1.5 * juce::jlimit (-20.0, 20.0, crash[i] - mc);
    double S[4] = { 0, 0, 0, 0 }; int N[4] = { 0, 0, 0, 0 };
    for (size_t i = 0; i < grid.size(); ++i) { S[i % 4] += s[i]; ++N[i % 4]; }
    for (int k = 0; k < 4; ++k) S[k] = N[k] > 0 ? S[k] / N[k] : -1e9;
    int best = 0; for (int k = 1; k < 4; ++k) if (S[k] > S[best]) best = k;
    for (int k = 0; k < 4; ++k) r.phaseScore[k] = S[k];
    double second = -1e9; for (int k = 0; k < 4; ++k) if (k != best) second = juce::jmax (second, S[k]);
    r.barPhase = best; r.barMarginDb = S[best] - second; r.ambiguous = r.barMarginDb < 1.0;
    const int current = bars.empty() ? -1 : nearestIndex (grid, bars[0]) % 4;
    const bool currentOnGrid = ! bars.empty() && std::abs (grid[(size_t) nearestIndex (grid, bars[0])] - bars[0]) < 0.02;
    if (r.halfShifted || ! currentOnGrid || (best != current && S[best] - (current >= 0 ? S[current] : -1e9) > 1.5))
    {
        std::vector<double> b2; for (size_t i = (size_t) best; i < grid.size(); i += 4) b2.push_back (grid[i]);
        bars = b2; r.barChanged = true;
    }
    else r.barPhase = current;
    const auto after = measure (B, grid, bars);
    r.kickOnBeatsPct = after.kickOnBeatsPct; r.kickOnBarStartPct = after.kickOnBarStartPct;
    return r;
}
} // namespace dzod::arranger
