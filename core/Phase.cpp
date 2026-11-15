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
// удары в полосе: максимумы сглаженной огибающей (±60 мс) выше 10 % от 99-го процентиля; время удара — начало атаки (25 % пика)
std::vector<double> hitsOf (const Band& B)
{
    std::vector<double> r; const int n = (int) B.e.size(); if (n < 100) return r;
    const int w = juce::jmax (1, (int) std::lround (0.004 * B.rate)), win = (int) std::lround (0.06 * B.rate), back = (int) std::lround (0.04 * B.rate);
    std::vector<double> s ((size_t) n, 0.0); double acc = 0;
    for (int i = 0; i < n; ++i) { acc += B.e[(size_t) i]; if (i - 2 * w - 1 >= 0) acc -= B.e[(size_t) (i - 2 * w - 1)]; if (i - w >= 0) s[(size_t) (i - w)] = acc; }
    auto srt = s; std::nth_element (srt.begin(), srt.begin() + (long) (srt.size() * 99 / 100), srt.end());
    const double thr = 0.1 * srt[srt.size() * 99 / 100];
    if (thr <= 0) return r;
    for (int i = win; i + win < n; ++i)
    {
        if (s[(size_t) i] < thr) continue;
        bool mx = true; for (int k = i - win; k <= i + win && mx; ++k) if (s[(size_t) k] > s[(size_t) i] || (s[(size_t) k] == s[(size_t) i] && k < i)) mx = false;
        if (! mx) continue;
        int o = i; while (o > i - back && s[(size_t) o] > 0.25 * s[(size_t) i]) --o;
        if (s[(size_t) juce::jmax (0, i - back)] > 0.5 * s[(size_t) i]) continue;    // не атака (гул)
        r.push_back (o / B.rate);
    }
    return r;
}

void movedBars (const std::vector<double>& oldG, const std::vector<double>& newG, const std::vector<double>& oldBars, double beat, std::vector<int>& fixedBars)
{
    fixedBars.clear();
    auto barOf = [&] (double t) { int b = 0; for (size_t k = 0; k < oldBars.size(); ++k) if (oldBars[k] <= t + 1e-6) b = (int) k; return b + 1; };
    auto far = [beat] (const std::vector<double>& a, double x) { auto it = std::lower_bound (a.begin(), a.end(), x); double best = 1e9;
        if (it != a.end()) best = *it - x; if (it != a.begin()) best = juce::jmin (best, x - *(it - 1)); return best > 0.12 * beat; };
    auto mark = [&] (double tm) { const int bb = barOf (tm); if (std::find (fixedBars.begin(), fixedBars.end(), bb) == fixedBars.end()) fixedBars.push_back (bb); };
    const double lo = juce::jmax (oldG.front(), newG.front()) - 0.5 * beat, hi = juce::jmin (oldG.back(), newG.back()) + 0.5 * beat;
    for (double x : newG) if (x > lo && x < hi && far (oldG, x)) mark (x);
    for (double x : oldG) if (x > lo && x < hi && far (newG, x)) mark (x);
    std::sort (fixedBars.begin(), fixedBars.end());
}
} // namespace

juce::StringArray& steadyFitDebug() { static juce::StringArray s; return s; }

SteadyFit fitSteadyGrid (const juce::AudioBuffer<float>& d, double sr, std::vector<double>& grid, std::vector<double>& bars, std::vector<int>& fixedBars)
{
    const auto B = bandsOf (d, sr);
    return fitSteadyGridHits (hitsOf (B.snare), hitsOf (B.kick), grid, bars, fixedBars);
}

SteadyFit fitSteadyGridHits (const std::vector<double>& sn, const std::vector<double>& kk, std::vector<double>& grid, std::vector<double>& bars, std::vector<int>& fixedBars)
{
    SteadyFit F; fixedBars.clear(); steadyFitDebug().clear();
    if (grid.size() < 32) return F;
    // начальный период — медиана долей трекера (лишняя доля её не сдвигает), фаза — первая доля
    std::vector<double> dd; for (size_t i = 0; i + 1 < grid.size(); ++i) dd.push_back (grid[i + 1] - grid[i]);
    std::nth_element (dd.begin(), dd.begin() + (long) dd.size() / 2, dd.end());
    double m = dd[dd.size() / 2], t0 = grid[grid.size() / 2];
    std::vector<double> hits (sn); hits.insert (hits.end(), kk.begin(), kk.end()); std::sort (hits.begin(), hits.end());
    if (hits.size() < 40) return F;
    // фаза восьмых: удары на решётке восьмых от t0; МНК t = c + n·(m/2), выбросы > 30 мс — прочь; от середины песни наружу,
    // чтобы небольшая ошибка начального периода не путала номера восьмых на краях
    const double mid = t0, span = (hits.back() - hits.front()) / 2;
    for (int it = 0; it < 16; ++it)
    {
        const double reach = juce::jmin (span + 1.0, 12.0 * std::pow (1.6, juce::jmin (it, 11)));
        double sx = 0, sy = 0, sxx = 0, sxy = 0; int cnt = 0;
        for (double h : hits)
        {
            if (std::abs (h - mid) > reach) continue;
            const double nn = std::round ((h - t0) / (m / 2)), pred = t0 + nn * (m / 2);
            if (std::abs (h - pred) > (it < 12 ? 0.03 : 0.012)) continue;   // последние проходы — строже (R14 AFTP: период ближе к истине)
            sx += nn; sy += h; sxx += nn * nn; sxy += nn * h; ++cnt;
        }
        if (cnt < 16) return F;
        const double den = cnt * sxx - sx * sx; if (std::abs (den) < 1e-9) return F;
        const double half = (cnt * sxy - sx * sy) / den, c = (sy - half * sx) / cnt;
        m = 2 * half; t0 = c;
    }
    // проверка: окна 8 с — медиана отклонения малого (если малого мало — всех ударов) ≤ 5 мс, и в окне большинство ударов на решётке
    const auto& chk = sn.size() >= 40 ? sn : hits;
    double worst = 0; int windows = 0, bad = 0, used = 0, unclear = 0;
    for (double w0 = chk.front(); w0 < chk.back(); w0 += 8.0)
    {
        std::vector<double> res; int all = 0;
        for (double h : chk) if (h >= w0 && h < w0 + 8.0)
        { ++all; const double nn = std::round ((h - t0) / (m / 2)), rr = h - (t0 + nn * (m / 2)); if (std::abs (rr) <= 0.03) res.push_back (rr); }
        if (all < 4) continue;
        ++windows;
        { auto r2 = res; std::sort (r2.begin(), r2.end()); steadyFitDebug().add (juce::String (w0, 1) + " s: hits " + juce::String (all) + ", on grid " + juce::String ((int) res.size()) + (r2.empty() ? juce::String() : ", median " + juce::String (r2[r2.size() / 2] * 1000, 1) + " ms")); }
        if ((int) res.size() * 2 < all) { ++unclear; continue; }      // сбивка шестнадцатыми / триолями — окно не судит
        std::nth_element (res.begin(), res.begin() + (long) res.size() / 2, res.end());
        const double md = std::abs (res[res.size() / 2]) * 1000;
        const bool edgeW = w0 == chk.front() || w0 + 8.0 >= chk.back();
        if (! edgeW) worst = juce::jmax (worst, md);
        used += (int) res.size();
        // R14 AFTP (sound 16.11): вступление (такты 1–4) малый играет свободнее (+4…+13 мс от ровной сетки), остальная песня ±2 мс —
        // первое и последнее окна судятся мягче (≤ 15 мс), остальные — ≤ 5 мс
        if (md > (edgeW ? 15.0 : 5.0)) ++bad;   // край в worst не идёт
    }
    F.period = m; F.windows = windows; F.worstWindowMs = worst; F.hits = used;
    if (windows < 4 || bad > 0 || unclear * 4 > windows) { if (unclear * 4 > windows) F.worstWindowMs = juce::jmax (F.worstWindowMs, 30.0); return F; }
    // фаза — по долям трекера: медиана отклонения долей от прямой (только близкие, < 30 мс) — как сетка была до сбоя
    std::vector<double> off;
    for (double g : grid) { const double nn = std::round ((g - t0) / m), rr = g - (t0 + nn * m); if (std::abs (rr) < 0.03) off.push_back (rr); }
    if (off.size() * 2 < grid.size()) return F;      // доли трекера не на долях прямой (например, на «и») — не трогаем
    std::nth_element (off.begin(), off.begin() + (long) off.size() / 2, off.end());
    t0 += off[off.size() / 2];
    // ровная сетка от первой до последней доли трекера
    const double k0 = std::round ((grid.front() - t0) / m), k1 = std::round ((grid.back() - t0) / m);
    std::vector<double> g2; for (double k = k0; k <= k1 + 0.5; k += 1) g2.push_back (t0 + k * m);
    // такт в долях — по тактам трекера (медиана), фаза такта — голосованием тактов трекера до первого сбоя (взято у studio 0.9.100)
    std::vector<double> b2;
    if (! bars.empty())
    {
        int Bt = 4;
        if (bars.size() > 2) { std::vector<double> bd; for (size_t i = 1; i < bars.size(); ++i) bd.push_back (bars[i] - bars[i - 1]);
            std::nth_element (bd.begin(), bd.begin() + (long) bd.size() / 2, bd.end()); Bt = juce::jlimit (2, 12, (int) std::lround (bd[bd.size() / 2] / m)); }
        double firstBad = 1e18;
        for (double g : grid) { const double x = (g - t0) / m; if (std::abs (x - std::round (x)) > 0.15) { firstBad = g; break; } }
        std::vector<int> votes ((size_t) Bt, 0);
        for (double b : bars) { if (b >= firstBad && b != bars.front()) break; const long k = std::lround ((b - t0) / m) - (long) k0; ++votes[(size_t) (((k % Bt) + Bt) % Bt)]; }
        const int ph = (int) (std::max_element (votes.begin(), votes.end()) - votes.begin());
        for (size_t q = (size_t) ph; q < g2.size(); q += (size_t) Bt) b2.push_back (g2[q]);
    }
    movedBars (grid, g2, bars, m, fixedBars);
    grid = g2; if (! b2.empty()) bars = b2;
    F.steady = true; F.phase = t0;
    return F;
}

bool regularizeGrid (std::vector<double>& grid, std::vector<double>& bars, std::vector<int>& fixedBars)
{
    fixedBars.clear();
    const int n = (int) grid.size();
    if (n < 16) return false;
    std::vector<double> d ((size_t) n - 1);
    for (int i = 0; i + 1 < n; ++i) d[(size_t) i] = grid[(size_t) i + 1] - grid[(size_t) i];
    // опорная доля — медиана интервалов в ±32 долях (лишняя доля размазана по ~25 интервалам, медиану не сдвигает)
    auto medAround = [&] (int c)
    {
        std::vector<double> w;
        for (int k = juce::jmax (0, c - 32); k <= juce::jmin (n - 2, c + 32); ++k) w.push_back (d[(size_t) k]);
        std::nth_element (w.begin(), w.begin() + (long) w.size() / 2, w.end());
        return w[w.size() / 2];
    };
    std::vector<double> mref ((size_t) n - 1);
    for (int i = 0; i + 1 < n; ++i) mref[(size_t) i] = medAround (i);
    // плохие интервалы: (а) сама доля короче 0,9 / длиннее 1,15 опорной;
    // (б) окно в W долей вмещает на ≥ 0,6 доли больше или меньше, чем W (лишняя или пропущенная доля, размазанная по участку)
    std::vector<char> bad ((size_t) n - 1, 0);
    for (int i = 0; i + 1 < n; ++i) if (d[(size_t) i] < 0.9 * mref[(size_t) i] || d[(size_t) i] > 1.15 * mref[(size_t) i]) bad[(size_t) i] = 1;
    for (int W : { 8, 16, 32, 48 })
        for (int i = 0; i + W < n; ++i)
        {
            const double m = mref[(size_t) (i + W / 2)];
            if (std::abs ((grid[(size_t) (i + W)] - grid[(size_t) i]) / m - W) >= 0.6)
                for (int k = i; k < i + W; ++k) bad[(size_t) k] = 1;
        }
    auto barOf = [&] (double t) { int b = 0; for (size_t k = 0; k < bars.size(); ++k) if (bars[k] <= t + 1e-6) b = (int) k; return b + 1; };
    std::vector<double> out; out.push_back (grid[0]);
    bool changed = false;
    int i = 0;
    while (i + 1 < n)
    {
        if (! bad[(size_t) i]) { out.push_back (grid[(size_t) i + 1]); ++i; continue; }
        int j = i; while (j + 1 < n && bad[(size_t) j]) ++j;          // доли i..j: интервалы i..j-1 плохие
        const double a = grid[(size_t) i], b = grid[(size_t) j];
        // доля — медиана хороших интервалов рядом (вне участка)
        // доля — только из участков ДО и ПОСЛЕ сбоя (по 16 хороших интервалов), не из окна со сбоем (sound 15.11 21:50)
        std::vector<double> w;
        for (int k = i - 1, c = 0; k >= 0 && c < 16; --k) if (! bad[(size_t) k]) { w.push_back (d[(size_t) k]); ++c; }
        for (int k = j, c = 0; k < n - 1 && c < 16; ++k) if (! bad[(size_t) k]) { w.push_back (d[(size_t) k]); ++c; }
        double m = mref[(size_t) i];
        if (w.size() >= 4) { std::nth_element (w.begin(), w.begin() + (long) w.size() / 2, w.end()); m = w[w.size() / 2]; }
        const int k = juce::jmax (1, (int) std::lround ((b - a) / m));
        if (k != j - i)        // число долей сходится — доли трекера не трогаем (живой темп: они идут за ударами)
        {
            for (int q = 1; q < k; ++q) out.push_back (a + (b - a) * q / k);
            out.push_back (b);
            changed = true;
            for (int bb = barOf (a); bb <= barOf (b); ++bb) if (fixedBars.empty() || fixedBars.back() < bb) fixedBars.push_back (bb);
        }
        else for (int q = i + 1; q <= j; ++q) out.push_back (grid[(size_t) q]);
        i = j;
    }
    if (! changed) return false;
    {   // автору — только такты, где доля реально сдвинулась (больше 12 % доли) или исчезла / появилась
        fixedBars.clear();
        std::vector<double> all (d); std::nth_element (all.begin(), all.begin() + (long) all.size() / 2, all.end());
        const double m = all[all.size() / 2];
        auto mark = [&] (double tm) { const int bb = barOf (tm); if (std::find (fixedBars.begin(), fixedBars.end(), bb) == fixedBars.end()) fixedBars.push_back (bb); };
        auto far = [m] (const std::vector<double>& a, double x) { auto it = std::lower_bound (a.begin(), a.end(), x); double best = 1e9;
            if (it != a.end()) best = *it - x; if (it != a.begin()) best = juce::jmin (best, x - *(it - 1)); return best > 0.12 * m; };
        for (double x : out)  if (far (grid, x)) mark (x);
        for (double x : grid) if (far (out, x))  mark (x);
        std::sort (fixedBars.begin(), fixedBars.end());
    }
    if (! bars.empty())
    {
        int k0 = 0; for (int q = 0; q < (int) out.size(); ++q) if (std::abs (out[(size_t) q] - bars[0]) < std::abs (out[(size_t) k0] - bars[0])) k0 = q;
        std::vector<double> b2; for (size_t q = (size_t) k0; q < out.size(); q += 4) b2.push_back (out[q]);
        bars = b2;
    }
    grid = out;
    return true;
}

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
