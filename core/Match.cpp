#include "Match.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace dzod
{
namespace arranger
{
namespace
{
const double kOct[10] = { 31.5, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };

double midMean (const Spectrum& s)
{
    double sum = 0, w = 0;
    for (int k = 0; k < kGrid; ++k) { const double f = gridHz (k); if (f >= 100 && f <= 8000) { sum += s[(size_t) k]; w += 1; } }
    return w > 0 ? sum / w : 0;
}

// как scipy.ndimage.gaussian_filter1d (mode = nearest, truncate = 4)
Spectrum gaussian (const Spectrum& x, double sigma)
{
    const int r = (int) std::lround (4 * sigma);
    std::vector<double> k ((size_t) (2 * r + 1));
    double ks = 0;
    for (int i = -r; i <= r; ++i) { k[(size_t) (i + r)] = std::exp (-0.5 * i * i / (sigma * sigma)); ks += k[(size_t) (i + r)]; }
    Spectrum y {};
    for (int n = 0; n < kGrid; ++n)
    {
        double s = 0;
        for (int i = -r; i <= r; ++i) s += k[(size_t) (i + r)] * x[(size_t) juce::jlimit (0, kGrid - 1, n + i)];
        y[(size_t) n] = s / ks;
    }
    return y;
}

double interpLogF (const Spectrum& c, double f)
{
    if (f <= gridHz (0)) return c[0];
    if (f >= gridHz (kGrid - 1)) return c[(size_t) (kGrid - 1)];
    const double k = 12.0 * std::log2 (f / 20.0);
    const int i = (int) std::floor (k);
    const double t = k - i;
    return c[(size_t) i] * (1 - t) + c[(size_t) juce::jmin (kGrid - 1, i + 1)] * t;
}

struct Biquad
{
    double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
    double process (double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};
} // namespace

double gridHz (int k) { return 20.0 * std::pow (2.0, k / 12.0); }

Spectrum ltas (const juce::AudioBuffer<float>& audio, double sr, juce::int64 from, juce::int64 to)
{
    const int order = 13, N = 1 << order, hop = N / 2;
    const juce::int64 total = audio.getNumSamples();
    if (to < 0 || to > total) to = total;
    from = juce::jlimit ((juce::int64) 0, total, from);
    Spectrum out; out.fill (-200.0);
    if (to - from < 1024) return out;

    juce::dsp::FFT fft (order);
    std::vector<float> win ((size_t) N), buf ((size_t) (2 * N));
    for (int i = 0; i < N; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) N);
    std::vector<double> psd ((size_t) (N / 2 + 1), 0.0);
    int frames = 0;
    // длинный участок — не больше ~400 окон, равномерно
    const juce::int64 span = to - from;
    const juce::int64 nWin = juce::jmax ((juce::int64) 1, (span - N) / hop + 1);
    const juce::int64 step = juce::jmax ((juce::int64) 1, nWin / 400);
    for (juce::int64 w = 0; w < nWin; w += step)
    {
        const juce::int64 s0 = from + w * hop;
        for (int c = 0; c < audio.getNumChannels(); ++c)
        {
            std::fill (buf.begin(), buf.end(), 0.0f);
            const float* x = audio.getReadPointer (c);
            for (int i = 0; i < N; ++i) { const juce::int64 j = s0 + i; buf[(size_t) i] = j < to ? x[j] * win[(size_t) i] : 0.0f; }
            fft.performFrequencyOnlyForwardTransform (buf.data(), true);
            for (int i = 0; i <= N / 2; ++i) psd[(size_t) i] += (double) buf[(size_t) i] * buf[(size_t) i];
        }
        ++frames;
    }
    for (auto& p : psd) p /= juce::jmax (1, frames);
    const double binHz = sr / N, nyq = sr / 2;
    for (int k = 0; k < kGrid; ++k)
    {
        const double f = gridHz (k);
        double hi = juce::jmin (f * std::pow (2.0, 1.0 / 6), nyq), lo = juce::jmin (f * std::pow (2.0, -1.0 / 6), hi * 0.9);
        int b0 = (int) std::ceil (lo / binHz), b1 = (int) std::floor (hi / binHz);
        double v = 0;
        if (b1 >= b0) { for (int b = b0; b <= b1; ++b) v += psd[(size_t) juce::jlimit (0, N / 2, b)]; v /= (b1 - b0 + 1); }
        else
        {
            const double bf = f / binHz; const int b = juce::jlimit (0, N / 2 - 1, (int) bf); const double t = bf - b;
            v = psd[(size_t) b] * (1 - t) + psd[(size_t) (b + 1)] * t;
        }
        out[(size_t) k] = 10.0 * std::log10 (v + 1e-20);
    }
    return out;
}

Spectrum normalised (const Spectrum& s)
{
    Spectrum r = s; const double m = midMean (s);
    for (auto& v : r) v -= m;
    return r;
}

Spectrum eqCurve (const Spectrum& piece, const Spectrum& reference, double strength, double limitDb)
{
    const auto a = normalised (piece), b = normalised (reference);
    Spectrum d {};
    for (int k = 0; k < kGrid; ++k) d[(size_t) k] = b[(size_t) k] - a[(size_t) k];
    d = gaussian (d, 4.0);                                                           // ~2/3 октавы
    const double m = midMean (d);
    for (auto& v : d) v = limitDb * std::tanh ((v - m) / limitDb);
    // самый низ и самый верх не трогаем: у Suno там часто пусто, усиливать нечего
    // и полосы, где у куска почти нет сигнала (на 60 дБ ниже самой громкой полосы)
    Spectrum band {}, present {};
    double bmax = -1e9;
    for (int k = 0; k < kGrid; ++k) { band[(size_t) k] = a[(size_t) k] + 10.0 * std::log10 (gridHz (k)); bmax = juce::jmax (bmax, band[(size_t) k]); }
    for (int k = 0; k < kGrid; ++k) present[(size_t) k] = juce::jlimit (0.0, 1.0, (band[(size_t) k] - (bmax - 60.0)) / 12.0);
    present = gaussian (present, 2.0);
    for (int k = 0; k < kGrid; ++k)
    {
        const double f = gridHz (k);
        const double edge = juce::jlimit (0.0, 1.0, std::log2 (f / 25.0)) * juce::jlimit (0.0, 1.0, std::log2 (17000.0 / f) / 0.6);
        d[(size_t) k] *= edge * present[(size_t) k] * strength;
    }
    return d;
}

std::array<double, 10> octaveTable (const Spectrum& c)
{
    std::array<double, 10> r {};
    for (int i = 0; i < 10; ++i) r[(size_t) i] = interpLogF (c, kOct[i]);
    return r;
}

void applyEq (juce::AudioBuffer<float>& audio, double sr, const Spectrum& curveDb)
{
    bool flat = true; for (auto v : curveDb) if (std::abs (v) > 0.01) flat = false;
    if (flat || audio.getNumSamples() == 0) return;
    // 1) импульсная характеристика: нужная АЧХ на сетке БПФ, нулевая фаза, окно Блэкмана на 8191 отвод
    const int L = 8191, half = L / 2, dOrder = 14, D = 1 << dOrder;
    juce::dsp::FFT dfft (dOrder);
    std::vector<float> spec ((size_t) (2 * D), 0.0f);
    const double nyq = sr / 2;
    for (int i = 0; i <= D / 2; ++i)
    {
        const double f = juce::jmin ((double) i * sr / D, nyq * 0.98);
        spec[(size_t) (2 * i)] = (float) std::pow (10.0, interpLogF (curveDb, juce::jmax (f, 20.0)) / 20.0);
        spec[(size_t) (2 * i + 1)] = 0.0f;
    }
    dfft.performRealOnlyInverseTransform (spec.data());                             // spec[0..D) — вещественный отклик
    std::vector<float> h ((size_t) L);
    for (int n = -half; n <= half; ++n)
    {
        const double w = 0.42 + 0.5 * std::cos (juce::MathConstants<double>::pi * n / (half + 1)) + 0.08 * std::cos (2 * juce::MathConstants<double>::pi * n / (half + 1));
        h[(size_t) (n + half)] = (float) (spec[(size_t) ((n + D) % D)] * w);
    }
    // 2) свёртка блоками через БПФ (перекрытие со сложением), «same»: без задержки
    const int order = 15, F = 1 << order, B = F - L + 1;
    juce::dsp::FFT fft (order);
    std::vector<float> H ((size_t) (2 * F), 0.0f);
    std::copy (h.begin(), h.end(), H.begin());
    fft.performRealOnlyForwardTransform (H.data());
    const int n = audio.getNumSamples();
    std::vector<float> out ((size_t) (n + L), 0.0f), blk ((size_t) (2 * F));
    for (int c = 0; c < audio.getNumChannels(); ++c)
    {
        std::fill (out.begin(), out.end(), 0.0f);
        const float* x = audio.getReadPointer (c);
        for (int s = 0; s < n; s += B)
        {
            std::fill (blk.begin(), blk.end(), 0.0f);
            const int m = juce::jmin (B, n - s);
            std::copy (x + s, x + s + m, blk.begin());
            fft.performRealOnlyForwardTransform (blk.data());
            for (int i = 0; i <= F / 2; ++i)
            {
                const float ar = blk[(size_t) (2 * i)], ai = blk[(size_t) (2 * i + 1)], br = H[(size_t) (2 * i)], bi = H[(size_t) (2 * i + 1)];
                blk[(size_t) (2 * i)] = ar * br - ai * bi; blk[(size_t) (2 * i + 1)] = ar * bi + ai * br;
            }
            fft.performRealOnlyInverseTransform (blk.data());
            for (int i = 0; i < m + L - 1 && s + i < (int) out.size(); ++i) out[(size_t) (s + i)] += blk[(size_t) i];
        }
        float* y = audio.getWritePointer (c);
        for (int i = 0; i < n; ++i) y[i] = out[(size_t) (i + half)];
    }
}

double lufs (const juce::AudioBuffer<float>& audio, double sr, juce::int64 from, juce::int64 to)
{
    const juce::int64 total = audio.getNumSamples();
    if (to < 0 || to > total) to = total;
    from = juce::jlimit ((juce::int64) 0, total, from);
    const juce::int64 n = to - from;
    const int W = (int) std::lround (0.4 * sr), H = (int) std::lround (0.1 * sr);
    if (n < W) return -70.0;
    // K-взвешивание: полка +4 дБ на 1,5 кГц и срез ниже 38 Гц (формулы BS.1770 для любой частоты)
    const double A = std::pow (10.0, 4.0 / 40), pi = juce::MathConstants<double>::pi;
    double w0 = 2 * pi * 1500.0 / sr, al = std::sin (w0) / (2 * (1 / std::sqrt (2.0))), c = std::cos (w0);
    const double a0s = (A + 1) - (A - 1) * c + 2 * std::sqrt (A) * al;
    const Biquad shelfProto { A * ((A + 1) + (A - 1) * c + 2 * std::sqrt (A) * al) / a0s, -2 * A * ((A - 1) + (A + 1) * c) / a0s,
                              A * ((A + 1) + (A - 1) * c - 2 * std::sqrt (A) * al) / a0s, 2 * ((A - 1) - (A + 1) * c) / a0s,
                              ((A + 1) - (A - 1) * c - 2 * std::sqrt (A) * al) / a0s };
    w0 = 2 * pi * 38.0 / sr; al = std::sin (w0) / (2 * 0.5); c = std::cos (w0);
    const double a0h = 1 + al;
    const Biquad hpProto { (1 + c) / 2 / a0h, -(1 + c) / a0h, (1 + c) / 2 / a0h, -2 * c / a0h, (1 - al) / a0h };
    std::vector<double> cs ((size_t) n + 1, 0.0);
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
    {
        Biquad s = shelfProto, hp = hpProto; double acc = 0;
        const float* x = audio.getReadPointer (ch);
        for (juce::int64 i = 0; i < n; ++i) { const double y = hp.process (s.process (x[from + i])); acc += y * y; cs[(size_t) i + 1] += acc; }
    }
    std::vector<double> blocks;
    for (juce::int64 s0 = 0; s0 + W <= n; s0 += H) blocks.push_back (-0.691 + 10 * std::log10 ((cs[(size_t) (s0 + W)] - cs[(size_t) s0]) / W + 1e-12));
    std::vector<double> g; for (double b : blocks) if (b > -70) g.push_back (b);
    if (g.empty()) return -70.0;
    auto meanPow = [] (const std::vector<double>& v) { double s = 0; for (double b : v) s += std::pow (10.0, b / 10); return 10 * std::log10 (s / v.size()); };
    const double rel = meanPow (g) - 10;
    std::vector<double> g2; for (double b : g) if (b > rel) g2.push_back (b);
    return meanPow (g2.empty() ? g : g2);
}

double peakDb (const juce::AudioBuffer<float>& a)
{
    float m = 0; for (int c = 0; c < a.getNumChannels(); ++c) m = juce::jmax (m, a.getMagnitude (c, 0, a.getNumSamples()));
    return 20 * std::log10 (juce::jmax (m, 1e-9f));
}

juce::String Key::name() const
{
    static const char* names[12] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    if (tonic < 0) return juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x94"));
    return juce::String (names[tonic]) + (minor ? " minor" : " major");
}

float Key::similarity (const Key& o) const
{
    double ma = 0, mb = 0; for (int i = 0; i < 12; ++i) { ma += chroma[(size_t) i]; mb += o.chroma[(size_t) i]; } ma /= 12; mb /= 12;
    double sab = 0, saa = 0, sbb = 0;
    for (int i = 0; i < 12; ++i) { const double x = chroma[(size_t) i] - ma, y = o.chroma[(size_t) i] - mb; sab += x * y; saa += x * x; sbb += y * y; }
    return saa > 0 && sbb > 0 ? (float) (sab / std::sqrt (saa * sbb)) : 1.0f;
}

bool Key::compatibleWith (const Key& o) const
{
    if (tonic < 0 || o.tonic < 0) return true;
    if (tonic == o.tonic) return true;
    const int rel = minor ? (tonic + 3) % 12 : (tonic + 9) % 12;
    if (o.minor != minor && o.tonic == rel) return true;
    // RIGOR 04B -> 04C: детектор назвал F major / C# major (сходство нот 0,27), автор на слух — тональность одна (28.10).
    // Куски одной песни делят ноты, даже когда профили «врут»; тревога — только если наборы нот почти не совпадают.
    if (confidence < 0.5f || o.confidence < 0.5f) return true;
    return similarity (o) > 0.0f;
}

Key detectKey (const std::vector<const juce::AudioBuffer<float>*>& harm, double sr, double fromSec, double toSec)
{
    Key k;
    if (harm.empty()) return k;
    juce::int64 n = 0; for (auto* b : harm) n = juce::jmax (n, (juce::int64) b->getNumSamples());
    juce::int64 s0 = (juce::int64) (fromSec * sr), s1 = toSec < 0 ? n : juce::jmin (n, (juce::int64) (toSec * sr));
    s0 = juce::jlimit ((juce::int64) 0, n, s0);
    const int order = 14, N = 1 << order, hop = N / 2;
    if (s1 - s0 < N) return k;
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) (2 * N)), win ((size_t) N);
    for (int i = 0; i < N; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * i / N);
    double chroma[12] = {};
    const juce::int64 nWin = (s1 - s0 - N) / hop + 1, step = juce::jmax ((juce::int64) 1, nWin / 300);
    for (juce::int64 w = 0; w < nWin; w += step)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        const juce::int64 a = s0 + w * hop;
        for (auto* b : harm)
            for (int c = 0; c < b->getNumChannels(); ++c)
            {
                const float* x = b->getReadPointer (c);
                for (int i = 0; i < N; ++i) if (a + i < b->getNumSamples()) buf[(size_t) i] += x[a + i] * win[(size_t) i];
            }
        fft.performFrequencyOnlyForwardTransform (buf.data(), true);
        double frame[12] = {}, tot = 0;
        for (int i = 1; i <= N / 2; ++i)
        {
            const double f = i * sr / N; if (f < 65 || f > 2100) continue;
            const double m = buf[(size_t) i];
            const int pc = ((int) std::lround (69 + 12 * std::log2 (f / 440.0)) % 12 + 12) % 12;
            frame[pc] += m; tot += m;
        }
        if (tot > 1e-6) for (int p = 0; p < 12; ++p) chroma[p] += frame[p] / tot;
    }
    static const double maj[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
    static const double mnr[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
    auto corr = [] (const double* x, const double* p, int rot)
    {
        double mx = 0, mp = 0; for (int i = 0; i < 12; ++i) { mx += x[i]; mp += p[i]; } mx /= 12; mp /= 12;
        double sxy = 0, sxx = 0, syy = 0;
        for (int i = 0; i < 12; ++i) { const double a = x[(i + rot) % 12] - mx, b = p[i] - mp; sxy += a * b; sxx += a * a; syy += b * b; }
        return sxx > 0 && syy > 0 ? sxy / std::sqrt (sxx * syy) : 0.0;
    };
    double best = -2;
    for (int r = 0; r < 12; ++r)
    {
        const double cm = corr (chroma, maj, r), cn = corr (chroma, mnr, r);
        if (cm > best) { best = cm; k.tonic = r; k.minor = false; }
        if (cn > best) { best = cn; k.tonic = r; k.minor = true; }
    }
    k.confidence = (float) juce::jmax (0.0, best);
    double cs = 0; for (double c : chroma) cs += c;
    for (int p = 0; p < 12; ++p) k.chroma[(size_t) p] = (float) (cs > 0 ? chroma[p] / cs : 0);
    if (best < 0.3) k.tonic = -1;
    return k;
}
} // namespace arranger
} // namespace dzod
