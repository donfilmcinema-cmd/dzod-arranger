// DZOD Stemmer core - см. DzodStemmer.h. Код анализа и выравнивания перенесён из DZOD Drummer 1.0.18 (StemAnalysis.cpp) без изменений в расчётах:
// выровненные файлы и снятая партия совпадают побайтно (тест DZODSmokeTest «Stemmer core»).
#include "DzodStemmer.h"
#include "Mp3Writer.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <iostream>
#include <charconv>
#include <string>

namespace dzod
{
namespace stemmer
{
using Vec = std::vector<float>;
using VecD = std::vector<double>;

float percentile (std::vector<float> v, double p)
{
    if (v.empty()) return 0.0f;
    const double pos = juce::jlimit (0.0, 1.0, p / 100.0) * (double) (v.size() - 1);
    const size_t i = (size_t) pos; std::nth_element (v.begin(), v.begin() + (long) i, v.end());
    const float a = v[i]; if (i + 1 >= v.size()) return a;
    const float b = *std::min_element (v.begin() + (long) i + 1, v.end());
    return a + (b - a) * (float) (pos - (double) i);
}

// положение времени t на сетке долей (дробный индекс)
double gridPos (double t, const std::vector<double>& grid)
{
    if (t <= grid.front()) return 0.0; if (t >= grid.back()) return (double) grid.size() - 1;
    const auto it = std::upper_bound (grid.begin(), grid.end(), t); const size_t i = (size_t) (it - grid.begin());
    return (double) (i - 1) + (t - grid[i - 1]) / (grid[i] - grid[i - 1]);
}


namespace
{
constexpr int kNfft = 1024, kOrder = 10, kBands = 40;
constexpr double kPi = 3.14159265358979323846;

//==============================================================================
// мелкие численные помощники
Vec medfilt (const Vec& x, int k)
{
    const int n = (int) x.size(), h = k / 2; Vec out ((size_t) n), win; win.reserve ((size_t) k);
    std::multiset<float> ms; std::multiset<float>::iterator mid;
    // простой вариант: окно с нулями по краям, как в scipy.signal.medfilt
    Vec pad ((size_t) (n + 2 * h), 0.0f); std::copy (x.begin(), x.end(), pad.begin() + h);
    for (int i = 0; i < k; ++i) ms.insert (pad[(size_t) i]);
    mid = std::next (ms.begin(), h);
    for (int i = 0; i < n; ++i)
    {
        out[(size_t) i] = *mid;
        if (i + 1 >= n) break;
        const float add = pad[(size_t) (i + k)], rem = pad[(size_t) i];
        ms.insert (add); if (add < *mid) --mid;
        if (rem <= *mid) ++mid;
        ms.erase (ms.lower_bound (rem));
    }
    return out;
}

double interp (double x, const VecD& xs, const VecD& ys)
{
    if (xs.empty()) return 0.0;
    if (x <= xs.front()) return ys.front();
    if (x >= xs.back()) return ys.back();
    const auto it = std::upper_bound (xs.begin(), xs.end(), x); const size_t i = (size_t) (it - xs.begin());
    const double f = (x - xs[i - 1]) / (xs[i] - xs[i - 1]);
    return ys[i - 1] + (ys[i] - ys[i - 1]) * f;
}
double interpIdx (double x, const VecD& ys)       // xs = 0,1,2...
{
    if (ys.empty()) return 0.0;
    if (x <= 0) return ys.front(); if (x >= (double) ys.size() - 1) return ys.back();
    const size_t i = (size_t) x; const double f = x - (double) i; return ys[i] + (ys[i + 1] - ys[i]) * f;
}
std::vector<int> findPeaks (const Vec& x, const Vec* heightVec, float heightConst, int distance, float prominence = 0.0f, int promWin = 0)
{
    std::vector<int> pk;
    for (int i = 1; i + 1 < (int) x.size(); ++i)
        if (x[(size_t) i] > x[(size_t) i - 1] && x[(size_t) i] >= x[(size_t) i + 1] && x[(size_t) i] >= (heightVec ? (*heightVec)[(size_t) i] : heightConst))
        {
            if (prominence > 0.0f)
            {
                float lmin = x[(size_t) i], rmin = x[(size_t) i];
                for (int j = i - 1; j >= juce::jmax (0, i - promWin) && x[(size_t) j] <= x[(size_t) i]; --j) lmin = juce::jmin (lmin, x[(size_t) j]);
                for (int j = i + 1; j < juce::jmin ((int) x.size(), i + promWin) && x[(size_t) j] <= x[(size_t) i]; ++j) rmin = juce::jmin (rmin, x[(size_t) j]);
                if (x[(size_t) i] - juce::jmax (lmin, rmin) < prominence) continue;
            }
            pk.push_back (i);
        }
    if (distance > 1 && pk.size() > 1)        // как в scipy: сильные пики вытесняют соседей
    {
        std::vector<int> order (pk.size()); std::iota (order.begin(), order.end(), 0);
        std::sort (order.begin(), order.end(), [&] (int a, int b) { return x[(size_t) pk[(size_t) a]] > x[(size_t) pk[(size_t) b]]; });
        std::vector<char> keep (pk.size(), 1);
        for (int o : order)
        {
            if (! keep[(size_t) o]) continue;
            for (int j = o - 1; j >= 0 && pk[(size_t) o] - pk[(size_t) j] < distance; --j) keep[(size_t) j] = 0;
            for (int j = o + 1; j < (int) pk.size() && pk[(size_t) j] - pk[(size_t) o] < distance; ++j) keep[(size_t) j] = 0;
        }
        std::vector<int> out; for (size_t i = 0; i < pk.size(); ++i) if (keep[i]) out.push_back (pk[i]);
        pk.swap (out);
    }
    return pk;
}

Vec normOdf (const Vec& x)
{
    const Vec m = medfilt (x, 201); Vec o (x.size()); double s = 0, s2 = 0;
    for (size_t i = 0; i < x.size(); ++i) { o[i] = juce::jmax (0.0f, x[i] - m[i]); s += o[i]; s2 += (double) o[i] * o[i]; }
    const double mean = s / (double) juce::jmax ((size_t) 1, x.size()), sd = std::sqrt (juce::jmax (1e-18, s2 / (double) juce::jmax ((size_t) 1, x.size()) - mean * mean));
    for (auto& v : o) v = (float) (v / (sd + 1e-9));
    return o;
}

//==============================================================================
bool loadAudio (const juce::File& f, juce::AudioBuffer<float>& buf, double& sr, juce::String& err)
{
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (r == nullptr) { err = ru ("Не удалось открыть файл: ") + f.getFileName(); return false; }
    if (r->lengthInSamples > (juce::int64) (r->sampleRate * 60.0 * 20.0)) { err = ru ("Файл длиннее 20 минут: ") + f.getFileName(); return false; }
    buf.setSize ((int) juce::jmax (1u, juce::jmin (2u, r->numChannels)), (int) r->lengthInSamples);
    r->read (&buf, 0, (int) r->lengthInSamples, 0, true, true); sr = r->sampleRate; return true;
}
// Перевод в другую частоту дискретизации без сдвига во времени
juce::AudioBuffer<float> resampled (const juce::AudioBuffer<float>& in, double from, double to)
{
    const double ratio = from / to;
    const int outLen = (int) ((double) in.getNumSamples() / ratio);
    juce::AudioBuffer<float> out (in.getNumChannels(), outLen); out.clear();
    for (int c = 0; c < in.getNumChannels(); ++c)
    {
        juce::WindowedSincInterpolator sinc;
        const int latency = (int) sinc.getBaseLatency();                 // фильтр запаздывает на столько входных отсчётов: добавляем их в конец и срезаем в начале
        const int lead = (int) std::ceil ((double) latency / ratio);
        std::vector<float> src ((size_t) in.getNumSamples() + (size_t) latency + 64, 0.0f), dst ((size_t) outLen + (size_t) lead, 0.0f);
        std::copy (in.getReadPointer (c), in.getReadPointer (c) + in.getNumSamples(), src.begin());
        const int want = juce::jmin ((int) dst.size(), (int) ((double) (src.size() - 8) / ratio));
        sinc.process (ratio, src.data(), dst.data(), want);
        // точное запаздывание в выходных отсчётах дробное: остаток меньше одного отсчёта
        const int skip = juce::roundToInt ((double) latency / ratio), count = juce::jmin (outLen, want - skip);
        if (count > 0) std::copy (dst.begin() + skip, dst.begin() + skip + count, out.getWritePointer (c));
    }
    return out;
}

Vec toMono (const juce::AudioBuffer<float>& b)
{
    Vec m ((size_t) b.getNumSamples());
    for (int c = 0; c < b.getNumChannels(); ++c) { auto* p = b.getReadPointer (c); for (size_t i = 0; i < m.size(); ++i) m[i] += p[i] / (float) b.getNumChannels(); }
    return m;
}

struct Feat           // покадровые признаки одного файла
{
    double sr = 44100, fr = 344.5; int hop = 128, n = 0;
    VecD t;                       // время центра кадра
    Vec odf[6];                   // потоки по полосам: low, lm, mid, crk, hi, all
    Vec band;                     // энергия в 40 полосах: band[b * n + frame]
    Vec hiDb;                     // уровень выше 6 кГц, дБ
};
const double kBandLo[5] = { 30, 110, 300, 2000, 7000 }, kBandHi[5] = { 110, 300, 2000, 6000, 16000 };

void computeFeat (const Vec& y, double sr, Feat& F, bool full)
{
    F.sr = sr; F.hop = juce::jmax (16, juce::roundToInt (sr / 344.53)); F.fr = sr / F.hop;
    F.n = juce::jmax (0, ((int) y.size() - kNfft) / F.hop + 1);
    F.t.resize ((size_t) F.n); for (int k = 0; k < F.n; ++k) F.t[(size_t) k] = (k * (double) F.hop + kNfft / 2.0) / sr;
    for (auto& o : F.odf) o.assign ((size_t) F.n, 0.0f);
    if (full) { F.band.assign ((size_t) kBands * (size_t) F.n, 0.0f); F.hiDb.assign ((size_t) F.n, -90.0f); }
    juce::dsp::FFT fft (kOrder); Vec win (kNfft), buf (2 * kNfft), mag (kNfft / 2 + 1), prevL (kNfft / 2 + 1, 0.0f);
    double wsum = 0; for (int i = 0; i < kNfft; ++i) { win[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2 * kPi * i / kNfft)); wsum += win[(size_t) i]; }
    const double binHz = sr / kNfft; const int nb = kNfft / 2 + 1;
    int bLo[6], bHi[6], cnt[6];
    for (int b = 0; b < 6; ++b)
    {
        const double lo = b < 5 ? kBandLo[b] : 30.0, hi = b < 5 ? kBandHi[b] : 16000.0;
        bLo[b] = (int) std::ceil (lo / binHz); bHi[b] = juce::jmin (nb, (int) std::ceil (hi / binHz)); cnt[b] = juce::jmax (1, bHi[b] - bLo[b]);
    }
    std::vector<int> bandOf ((size_t) nb, -1); const int hiBin = (int) std::ceil (6000.0 / binHz);
    for (int i = 0; i < nb; ++i)
    {
        const double f = i * binHz; if (f < 35.0 || f >= 16000.0) continue;
        bandOf[(size_t) i] = juce::jlimit (0, kBands - 1, (int) (std::log (f / 35.0) / std::log (16000.0 / 35.0) * kBands));
    }
    for (int k = 0; k < F.n; ++k)
    {
        const float* src = y.data() + (size_t) k * (size_t) F.hop;
        for (int i = 0; i < kNfft; ++i) buf[(size_t) i] = src[i] * win[(size_t) i];
        std::fill (buf.begin() + kNfft, buf.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform (buf.data(), true);
        double hi = 0;
        for (int i = 0; i < nb; ++i)
        {
            const float m = buf[(size_t) i] / (float) wsum; mag[(size_t) i] = m;
            if (full) { const float p = m * m; if (bandOf[(size_t) i] >= 0) F.band[(size_t) bandOf[(size_t) i] * (size_t) F.n + (size_t) k] += p; if (i >= hiBin) hi += p; }
        }
        if (full) F.hiDb[(size_t) k] = (float) (10.0 * std::log10 (hi + 1e-9));
        for (int i = 0; i < nb; ++i)
        {
            const float L = std::log1p (200.0f * mag[(size_t) i]);
            if (k > 0) { const float d = juce::jmax (0.0f, L - prevL[(size_t) i]); if (d > 0) for (int b = full ? 0 : 5; b < 6; ++b) if (i >= bLo[b] && i < bHi[b]) F.odf[b][(size_t) k] += d / (float) cnt[b]; }
            prevL[(size_t) i] = L;
        }
    }
}

// автокорреляция через БПФ
Vec autocorr (const Vec& x)
{
    int order = 1; while ((1 << order) < (int) x.size() * 2) ++order;
    const int N = 1 << order; juce::dsp::FFT fft (order); Vec b ((size_t) (2 * N), 0.0f);
    std::copy (x.begin(), x.end(), b.begin());
    fft.performRealOnlyForwardTransform (b.data());
    for (int i = 0; i < N; ++i) { const float re = b[(size_t) (2 * i)], im = b[(size_t) (2 * i + 1)]; b[(size_t) (2 * i)] = re * re + im * im; b[(size_t) (2 * i + 1)] = 0.0f; }
    fft.performRealOnlyInverseTransform (b.data());
    Vec a (x.size()); for (size_t i = 0; i < x.size(); ++i) a[i] = b[i];
    return a;
}

void savgol11 (VecD& x)
{
    static const double c[11] = { -36, 9, 44, 69, 84, 89, 84, 69, 44, 9, -36 };
    if (x.size() < 11) return; VecD o (x);
    for (size_t i = 5; i + 5 < x.size(); ++i) { double s = 0; for (int k = -5; k <= 5; ++k) s += c[k + 5] * x[(size_t) ((int) i + k)]; o[i] = s / 429.0; }
    x.swap (o);
}

Vec spectrum (const Vec& y, int start, int len, int order, juce::dsp::FFT& fft)
{
    const int N = 1 << order; Vec b ((size_t) (2 * N), 0.0f);
    for (int i = 0; i < len && i < N; ++i) { const int s = start + i; if (s >= 0 && s < (int) y.size()) b[(size_t) i] = y[(size_t) s] * (float) (0.5 - 0.5 * std::cos (2 * kPi * i / (len - 1))); }
    fft.performFrequencyOnlyForwardTransform (b.data(), true); b.resize ((size_t) (N / 2 + 1)); return b;
}


// Выпрямление сетки: для каждой доли - прямая по соседям ±w (без самой доли, с отбрасыванием выбросов);
// доля дальше tol от прямой ставится на неё. Медленный разгон сохраняется, местные «вилки» уходят.
void straighten (VecD& bt, int w, double tol)
{
    const int n = (int) bt.size(); if (n < 2 * w + 1) return;
    for (int pass = 0; pass < 2; ++pass)
    {
        VecD out (bt);
        for (int i = w; i < n - w; ++i)                       // только с соседями с обеих сторон: края (начало и конец сетки) не трогаются
        {
            const int a = i - w, b = i + w;
            std::vector<char> use ((size_t) (b - a + 1), 1); use[(size_t) (i - a)] = 0;
            double k = 0, c = 0;
            for (int it = 0; it < 3; ++it)
            {
                double sx = 0, sy = 0, sxx = 0, sxy = 0, m = 0;
                for (int j = a; j <= b; ++j) if (use[(size_t) (j - a)]) { sx += j; sy += bt[(size_t) j]; sxx += (double) j * j; sxy += j * bt[(size_t) j]; m += 1; }
                if (m < 4) break;
                k = (m * sxy - sx * sy) / juce::jmax (1e-12, m * sxx - sx * sx); c = (sy - k * sx) / m;
                for (int j = a; j <= b; ++j) if (j != i) use[(size_t) (j - a)] = std::abs (bt[(size_t) j] - (k * j + c)) < tol;
            }
            const double fit = k * i + c;
            if (std::abs (bt[(size_t) i] - fit) > tol) out[(size_t) i] = fit;
        }
        bt.swap (out);
    }
}

// Темп без подсказки: автокорреляция кривой атак по всей музыке, кандидаты 60-200 BPM (доля, 2, 4, 8 долей),
// между темпом и его половиной / удвоением выбирает мягкое предпочтение ~120 BPM (лог-гаусс).
double estimateTempo (const Vec& o, int a0, int a1, double FR)
{
    Vec seg (o.begin() + juce::jmax (0, a0), o.begin() + juce::jmin ((int) o.size(), a1 + 1));
    if (seg.size() < 64) return 120.0;
    const Vec a = autocorr (seg); const float a00 = a[0] + 1e-9f;
    auto at = [&] (double lag) { const int l = (int) lag; if (l + 1 >= (int) a.size()) return 0.0; return (a[(size_t) l] + (a[(size_t) l + 1] - a[(size_t) l]) * (lag - l)) / a00; };
    double best = -1e18, bestB = 120.0;
    for (double b = 60.0; b <= 200.0; b += 0.25)
    {
        static const double mult[4] = { 1, 2, 4, 8 }, wt[4] = { 1, 0.8, 0.8, 0.5 }; double s = 0;
        for (int m = 0; m < 4; ++m) s += wt[m] * at (60.0 / b * mult[m] * FR);
        s += 0.6 * at (30.0 / b * FR);                                    // восьмые: отличает темп от 2/3 и 3/2
        const double oct = std::log2 (b / 120.0), prior = std::exp (-0.5 * (oct / 0.6) * (oct / 0.6));
        if (s * prior > best) { best = s * prior; bestB = b; }
    }
    return bestB;
}

// рабочие данные анализа (как было в Drummer: StemAnalyzer::Impl)
struct Work
{
    Job job;
    double sr = 44100; Vec drums, fullMono; Feat fd;
    VecD grid;                       // время каждой доли в исходных файлах
    double musicStart = 0, bpmMin = 0, bpmMed = 0, bpmMax = 0;
    std::vector<int> pk;             // кадры атак
    Vec kick, body, stick;           // уровни по типам для каждой атаки
    std::vector<std::pair<float, float>> pitchTail;   // высота и «звон» ударов с корпусом
    double snarePitch = 190;
    struct Hi { double t; float sustain, level; }; std::vector<Hi> hi;
};
} // namespace

struct Stemmer::Audio { double sr = 44100; Vec fullMono; };

bool Stemmer::loadAudio (const juce::File& f, juce::AudioBuffer<float>& buf, double& sr, juce::String& err) { return dzod::stemmer::loadAudio (f, buf, sr, err); }
juce::AudioBuffer<float> Stemmer::resample (const juce::AudioBuffer<float>& in, double from, double to) { return resampled (in, from, to); }

//==============================================================================
bool Stemmer::analyse (const Job& job, Progress progress)
{
    ready = false; error.clear(); audio.reset(); jobData = job; result = {}; Work work; auto& I = work; I.job = job;
    auto say = [&] (float p, const char* s) { return progress == nullptr || progress (p, ru (s)); };
    if (! say (0.02f, "Чтение файлов")) return false;
    juce::AudioBuffer<float> bd, bf; double srD = 0, srF = 0;
    if (! loadAudio (job.drums, bd, srD, error) || ! loadAudio (job.full, bf, srF, error)) return false;
    if (std::abs (srD - srF) > 1.0) { error = ru ("У барабанного стема и полной песни разная частота дискретизации"); return false; }
    I.sr = srD; I.drums = toMono (bd); I.fullMono = toMono (bf); bd.setSize (0, 0); bf.setSize (0, 0);
    if (I.drums.size() < (size_t) (I.sr * 8)) { error = ru ("Запись слишком короткая"); return false; }

    if (! say (0.08f, "Анализ барабанного стема")) return false;
    computeFeat (I.drums, I.sr, I.fd, true);
    if (! say (0.22f, "Анализ полной песни")) return false;
    Feat ff; computeFeat (I.fullMono, I.sr, ff, false);
    const int n = juce::jmin (I.fd.n, ff.n); const double FR = I.fd.fr; const auto& t = I.fd.t;

    // --- где звучит музыка
    std::vector<char> act ((size_t) n, 0);
    {
        const int w = (int) (I.sr * 0.5), step = (int) (I.sr * 0.1); VecD env; double mx = -200;
        for (size_t s = 0; s + (size_t) w <= I.fullMono.size(); s += (size_t) step) { double e = 0; for (int i = 0; i < w; i += 4) e += (double) I.fullMono[s + (size_t) i] * I.fullMono[s + (size_t) i]; e = 10 * std::log10 (e * 4 / w + 1e-10); env.push_back (e); mx = juce::jmax (mx, e); }
        for (int k = 0; k < n; ++k) { const double e = interpIdx ((t[(size_t) k] - 0.25) / 0.1, env); act[(size_t) k] = e > mx - 40; }
    }
    int a0 = 0, a1 = n - 1; while (a0 < n && ! act[(size_t) a0]) ++a0; while (a1 > 0 && ! act[(size_t) a1]) --a1;
    if (a1 - a0 < (int) (FR * 20)) { error = ru ("В полной песне слишком мало звука"); return false; }
    I.musicStart = t[(size_t) a0];

    // --- общий детектор атак: барабаны + полная песня (чтобы не терять сетку там, где барабаны молчат)
    Vec od = normOdf (I.fd.odf[5]), os = normOdf (ff.odf[5]), o ((size_t) n);
    for (int k = 0; k < n; ++k) o[(size_t) k] = od[(size_t) k] + 0.5f * os[(size_t) k];

    // --- кривая темпа: окна 12 с, шаг 2 с, путь Витерби без скачков
    if (! say (0.34f, "Поиск темпа")) return false;
    // темп без подсказки (projectBpm <= 0, анализ версии для Arrange): грубый поиск 60-200 по всей песне
    const double bpm0 = job.projectBpm > 1.0 ? job.projectBpm : estimateTempo (o, a0, a1, FR);
    result.tempoEstimate = bpm0;
    VecD bp; for (double b = bpm0 * 0.86; b < bpm0 * 1.20; b += 0.25) bp.push_back (b);
    VecD cs; for (double c = t[(size_t) a0] + 6; c < t[(size_t) a1] - 6; c += 2.0) cs.push_back (c);
    if (cs.size() < 3) { error = ru ("Запись слишком короткая для поиска темпа"); return false; }
    std::vector<VecD> Sc (cs.size(), VecD (bp.size(), 0.0));
    for (size_t i = 0; i < cs.size(); ++i)
    {
        const int f0 = juce::jmax (0, (int) ((cs[i] - 6) * FR)), f1 = juce::jmin (n, (int) ((cs[i] + 6) * FR));
        Vec seg (o.begin() + f0, o.begin() + f1); Vec a = autocorr (seg); const float a00 = a[0] + 1e-9f;
        for (size_t j = 0; j < bp.size(); ++j)
        {
            static const double mult[4] = { 1, 2, 4, 8 }, wt[4] = { 1, 0.8, 0.8, 0.5 }; double s = 0;
            for (int m = 0; m < 4; ++m) { const double lag = 60.0 / bp[j] * mult[m] * FR; const int l = (int) lag; if (l + 1 < (int) a.size()) s += wt[m] * (a[(size_t) l] + (a[(size_t) l + 1] - a[(size_t) l]) * (lag - l)) / a00; }
            Sc[i][j] = s;
        }
    }
    std::vector<VecD> D (cs.size(), VecD (bp.size())); std::vector<std::vector<int>> Bk (cs.size(), std::vector<int> (bp.size(), 0)); D[0] = Sc[0];
    for (size_t i = 1; i < cs.size(); ++i)
        for (size_t j = 0; j < bp.size(); ++j)
        {
            double best = -1e18; int bi = 0;
            for (size_t q = 0; q < bp.size(); ++q) { const double v = D[i - 1][q] - 0.04 * std::abs (bp[j] - bp[q]); if (v > best) { best = v; bi = (int) q; } }
            D[i][j] = Sc[i][j] + best; Bk[i][j] = bi;
        }
    VecD tempo (cs.size()); { int j = (int) (std::max_element (D.back().begin(), D.back().end()) - D.back().begin()); for (size_t i = cs.size(); i-- > 0;) { tempo[i] = bp[(size_t) j]; j = Bk[i][(size_t) j]; } }

    // --- отслеживание долей с переменным периодом
    if (! say (0.44f, "Сетка долей")) return false;
    Vec ls ((size_t) n, 0.0f); { float g[15]; for (int i = 0; i < 15; ++i) g[i] = std::exp (-0.5f * (float) ((i - 7) * (i - 7)) / 9.0f); for (int k = 0; k < n; ++k) { float s = 0; for (int i = 0; i < 15; ++i) { const int q = k + i - 7; if (q >= 0 && q < n) s += o[(size_t) q] * g[i]; } ls[(size_t) k] = s; } }
    VecD cum ((size_t) n, 0.0); std::vector<int> back ((size_t) n, -1); VecD pf ((size_t) n);
    for (int k = 0; k < n; ++k) pf[(size_t) k] = 60.0 / interp (t[(size_t) k], cs, tempo) * FR;
    for (int i = 0; i < n; ++i)
    {
        const double p = pf[(size_t) i]; const int lo = (int) (p * 0.86), hi = (int) (p * 1.16);
        if (i < hi) { cum[(size_t) i] = ls[(size_t) i]; continue; }
        double best = -1e18; int bj = lo;
        for (int off = lo; off <= hi; ++off) { const double lg = std::log (off / p); const double v = cum[(size_t) (i - off)] - 500.0 * lg * lg; if (v > best) { best = v; bj = off; } }
        cum[(size_t) i] = ls[(size_t) i] + best; back[(size_t) i] = i - bj;
    }
    std::vector<int> bfr; { const int tail = (int) (pf.back() * 2); int i = (int) (std::max_element (cum.end() - tail, cum.end()) - cum.begin()); while (i >= 0) { bfr.push_back (i); i = back[(size_t) i]; } std::reverse (bfr.begin(), bfr.end()); }
    VecD ref (bfr.size()); std::vector<char> ok (bfr.size(), 0);
    for (size_t i = 0; i < bfr.size(); ++i)
    {
        const int x = bfr[i], lo = juce::jmax (0, x - 9), hi = juce::jmin (n, x + 10); int k = lo; for (int q = lo; q < hi; ++q) if (ls[(size_t) q] > ls[(size_t) k]) k = q;
        ref[i] = t[(size_t) k]; ok[i] = ls[(size_t) k] > 0.8f && act[(size_t) x];
    }
    int first = -1, last = -1; for (int i = 0; i < (int) ok.size(); ++i) if (ok[(size_t) i]) { if (first < 0) first = i; last = i; }
    if (first < 0 || last - first < 16) { error = ru ("Не удалось найти доли. Проверьте темп проекта."); return false; }
    VecD bt; { VecD xi, yi; for (int i = first; i <= last; ++i) if (ok[(size_t) i]) { xi.push_back (i); yi.push_back (ref[(size_t) i]); } for (int i = first; i <= last; ++i) bt.push_back (interp (i, xi, yi)); }
    savgol11 (bt);
    if (job.straightenGrid) straighten (bt, 8, 0.010);
    const double p0 = (bt[8] - bt[0]) / 8.0, p1 = (bt.back() - bt[bt.size() - 9]) / 8.0, dur = (double) I.fullMono.size() / I.sr;
    I.grid.clear(); for (int k = (int) (bt[0] / p0); k >= 1; --k) I.grid.push_back (bt[0] - p0 * k);
    I.grid.insert (I.grid.end(), bt.begin(), bt.end());
    for (int k = 1; k <= (int) ((dur - bt.back()) / p1); ++k) I.grid.push_back (bt.back() + p1 * k);
    { VecD loc; for (size_t i = 8; i < bt.size(); ++i) loc.push_back (60.0 / ((bt[i] - bt[i - 8]) / 8.0)); std::sort (loc.begin(), loc.end()); I.bpmMin = loc.front(); I.bpmMax = loc.back(); I.bpmMed = loc[loc.size() / 2]; }

    // --- атаки барабанов
    if (! say (0.55f, "Поиск ударов")) return false;
    const int nd = I.fd.n; Vec odm ((size_t) nd, 0.0f);
    for (int b = 0; b < 5; ++b)
    {
        const Vec m = medfilt (I.fd.odf[b], 301); Vec x ((size_t) nd); for (int k = 0; k < nd; ++k) x[(size_t) k] = juce::jmax (0.0f, I.fd.odf[b][(size_t) k] - m[(size_t) k]);
        const float p = percentile (x, 99.5) + 1e-9f; for (int k = 0; k < nd; ++k) odm[(size_t) k] += x[(size_t) k] / p;
    }
    { Vec sm ((size_t) nd, 0.0f); static const float h[5] = { 0.0f, 0.2f, 0.4f, 0.2f, 0.0f }; for (int k = 2; k + 2 < nd; ++k) for (int i = 0; i < 5; ++i) sm[(size_t) k] += odm[(size_t) (k + i - 2)] * h[i]; odm.swap (sm); }
    Vec thr = medfilt (odm, ((int) (FR * 0.4)) | 1); for (auto& v : thr) v = v * 1.5f + 0.12f;
    I.pk = findPeaks (odm, &thr, 0.0f, (int) (0.045 * FR));
    I.pk.erase (std::remove_if (I.pk.begin(), I.pk.end(), [&] (int i) { return i <= 20 || i >= nd - 60; }), I.pk.end());
    if (I.pk.size() < 40) { error = ru ("В барабанном стеме слишком мало ударов"); return false; }

    // --- спектр каждой атаки: 40 полос в момент удара и 40 полос «хвоста»
    const int np = (int) I.pk.size(), dim = 2 * kBands; Vec V ((size_t) np * (size_t) dim);
    auto seg = [&] (int b, int i, int a, int c) { double s = 0; int cnt = 0; for (int q = juce::jmax (0, i + a); q < juce::jmin (nd, i + c); ++q) { s += I.fd.band[(size_t) b * (size_t) nd + (size_t) q]; ++cnt; } return cnt ? s / cnt : 0.0; };
    for (int r = 0; r < np; ++r)
        for (int b = 0; b < kBands; ++b)
        {
            const int i = I.pk[(size_t) r]; const double pre = seg (b, i, -14, -5);
            V[(size_t) r * dim + (size_t) b] = (float) std::sqrt (juce::jmax (0.0, seg (b, i, -2, 12) - pre));
            V[(size_t) r * dim + (size_t) (kBands + b)] = (float) std::sqrt (juce::jmax (0.0, seg (b, i, 20, 55) - 0.5 * pre));
        }
    { const float s = percentile (V, 99.9) + 1e-12f; for (auto& v : V) v /= s; }

    // --- разложение на типы ударов (NMF, расходимость Кульбака-Лейблера), начальные образцы заданы по смыслу
    if (! say (0.66f, "Распознавание типов ударов")) return false;
    const int K = 7; VecD cen (kBands); for (int b = 0; b < kBands; ++b) cen[(size_t) b] = 35.0 * std::pow (16000.0 / 35.0, (b + 0.5) / kBands);
    Vec W ((size_t) K * dim, 0.01f), H ((size_t) np * K, 0.1f);
    auto setW = [&] (int k, bool tail, double lo, double hi, float v) { for (int b = 0; b < kBands; ++b) if (cen[(size_t) b] >= lo && cen[(size_t) b] < hi) W[(size_t) k * dim + (size_t) (tail ? kBands + b : b)] = v; };
    setW (0, false, 35, 180, 1.0f); setW (0, false, 1500, 6000, 0.15f);
    setW (1, false, 110, 300, 0.6f); setW (1, false, 300, 1500, 1.0f); setW (1, false, 1500, 6000, 0.4f);
    setW (2, false, 6000, 16000, 1.0f); setW (2, false, 1500, 6000, 0.4f);
    setW (3, false, 300, 6000, 1.0f);
    setW (4, true, 35, 300, 1.0f); setW (5, true, 300, 6000, 1.0f); setW (6, true, 6000, 16000, 1.0f); setW (6, true, 300, 6000, 0.3f);
    Vec R ((size_t) np * dim), num;
    for (int it = 0; it < 300; ++it)
    {
        if (it % 60 == 0 && ! say (0.66f + 0.2f * it / 300.0f, "Распознавание типов ударов")) return false;
        auto ratio = [&] { for (int r = 0; r < np; ++r) for (int d = 0; d < dim; ++d) { float s = 1e-9f; for (int k = 0; k < K; ++k) s += H[(size_t) r * K + (size_t) k] * W[(size_t) k * dim + (size_t) d]; R[(size_t) r * dim + (size_t) d] = V[(size_t) r * dim + (size_t) d] / s; } };
        ratio();
        for (int k = 0; k < K; ++k)
        {
            float wsum = 1e-9f; for (int d = 0; d < dim; ++d) wsum += W[(size_t) k * dim + (size_t) d];
            for (int r = 0; r < np; ++r) { float s = 0; for (int d = 0; d < dim; ++d) s += R[(size_t) r * dim + (size_t) d] * W[(size_t) k * dim + (size_t) d]; H[(size_t) r * K + (size_t) k] *= s / wsum; }
        }
        ratio();
        for (int k = 0; k < K; ++k)
        {
            float hsum = 1e-9f; for (int r = 0; r < np; ++r) hsum += H[(size_t) r * K + (size_t) k];
            for (int d = 0; d < dim; ++d) { float s = 0; for (int r = 0; r < np; ++r) s += H[(size_t) r * K + (size_t) k] * R[(size_t) r * dim + (size_t) d]; W[(size_t) k * dim + (size_t) d] *= s / hsum; }
        }
    }
    // роли компонент определяются по получившейся форме спектра
    std::vector<int> kickBody, kickSub, snareC, stickC;
    for (int k = 0; k < K; ++k)
    {
        double att = 1e-12, tail = 0, f[5] = { 0, 0, 0, 0, 0 };
        for (int b = 0; b < kBands; ++b)
        {
            const double a = W[(size_t) k * dim + (size_t) b], c = cen[(size_t) b]; att += a; tail += W[(size_t) k * dim + (size_t) (kBands + b)];
            f[c < 110 ? 0 : c < 300 ? 1 : c < 1500 ? 2 : c < 6000 ? 3 : 4] += a;
        }
        for (auto& v : f) v /= att;
        if (tail / att > 5.0) continue;
        if (f[0] > 0.2 && f[1] > 0.15) kickBody.push_back (k);
        else if (f[0] > 0.6) kickSub.push_back (k);
        else if (f[0] < 0.1 && f[1] + f[2] > 0.6) snareC.push_back (k);
        else if (f[2] + f[3] > 0.7 && f[4] < 0.3) stickC.push_back (k);
    }
    if (kickBody.empty()) kickBody = kickSub;
    auto level = [&] (const std::vector<int>& cs2)
    {
        Vec v ((size_t) np, 0.0f); if (cs2.empty()) return v;
        for (int k : cs2) { float a = 0; for (int b = 0; b < kBands; ++b) a += W[(size_t) k * dim + (size_t) b]; for (int r = 0; r < np; ++r) v[(size_t) r] += H[(size_t) r * K + (size_t) k] * a; }
        Vec pos; for (float x : v) if (x > 0) pos.push_back (x); const float p = percentile (pos, 92) + 1e-12f; for (auto& x : v) x /= p; return v;
    };
    I.kick = level (kickBody); I.body = level (snareC); I.stick = level (stickC);
    if (kickBody.empty() && snareC.empty()) { error = ru ("Не удалось различить бочку и малый в этом стеме"); return false; }

    // --- высота и «звон» ударов с корпусом: отличаем малый от томов и от корпуса бочки
    if (! say (0.88f, "Малый и томы")) return false;
    juce::dsp::FFT fftP (14); const double sc = I.sr / 44100.0, binHz = I.sr / 16384.0;
    const int lo = (int) std::ceil (70.0 / binHz), hiB = (int) (420.0 / binHz);
    I.pitchTail.assign ((size_t) np, { 0.0f, 0.0f }); Vec sp;
    for (int r = 0; r < np; ++r)
    {
        if (I.body[(size_t) r] <= 0.3f) continue;
        const int s = (int) (t[(size_t) I.pk[(size_t) r]] * I.sr) - (int) (100 * sc);
        Vec X = spectrum (I.drums, s, (int) (4410 * sc), 14, fftP), X2 = spectrum (I.drums, s + (int) (3500 * sc), (int) (7500 * sc), 14, fftP);
        int pkb = lo; double e1 = 0, e2 = 0; for (int b = lo; b <= hiB; ++b) { if (X[(size_t) b] > X[(size_t) pkb]) pkb = b; e1 += (double) X[(size_t) b] * X[(size_t) b]; e2 += (double) X2[(size_t) b] * X2[(size_t) b]; }
        I.pitchTail[(size_t) r] = { (float) (pkb * binHz), (float) (std::sqrt (e2) / (std::sqrt (e1) + 1e-9)) }; if (I.pitchTail[(size_t) r].first >= 150.0f) sp.push_back (I.pitchTail[(size_t) r].first);
    }
    I.snarePitch = sp.empty() ? 190.0 : percentile (sp, 50);

    // --- тарелки: отдельный детектор верхних частот
    {
        const auto& e = I.fd.hiDb; Vec sm ((size_t) nd), d ((size_t) nd, 0.0f);
        for (int k = 0; k < nd; ++k) sm[(size_t) k] = (e[(size_t) juce::jmax (0, k - 1)] + e[(size_t) k] + e[(size_t) juce::jmin (nd - 1, k + 1)]) / 3.0f;
        for (int k = 1; k < nd; ++k) d[(size_t) k] = juce::jmax (0.0f, sm[(size_t) k] - sm[(size_t) k - 1]);
        auto hp = findPeaks (d, nullptr, 2.5f, (int) (0.05 * FR), 2.0f, (int) (0.4 * FR));
        auto mean = [&] (int a, int b) { double s = 0; for (int q = a; q < b; ++q) s += e[(size_t) q]; return (float) (s / juce::jmax (1, b - a)); };
        for (int i : hp)
        {
            if (i <= 30 || i >= nd - 240) continue;
            float mx = e[(size_t) i]; for (int q = i; q < i + 10; ++q) mx = juce::jmax (mx, e[(size_t) q]);
            const float pre = mean (i - 20, i - 5); I.hi.push_back ({ t[(size_t) i], mean (i + 150, i + 230) - pre, mx });
        }
    }
    // результат: сетка и удары - в Analysis, звук для выравнивания - в Audio
    result.sampleRate = I.sr; result.projectBpm = bpm0; if (jobData.projectBpm <= 1.0) jobData.projectBpm = bpm0; result.grid = I.grid; result.musicStart = I.musicStart;
    result.bpmMin = I.bpmMin; result.bpmMedian = I.bpmMed; result.bpmMax = I.bpmMax; result.snarePitch = I.snarePitch;
    result.hits.resize (I.pk.size());
    for (size_t r = 0; r < I.pk.size(); ++r)
        result.hits[r] = { I.fd.t[(size_t) I.pk[r]], I.kick[r], I.body[r], I.stick[r], I.pitchTail[r].first, I.pitchTail[r].second };
    for (auto& h : I.hi) result.hi.push_back ({ h.t, h.sustain, h.level });
    audio = std::make_shared<Audio>(); audio->sr = I.sr; audio->fullMono = std::move (I.fullMono);
    result.autoShift = chooseShift();
    ready = true;
    return say (1.0f, "Готово");
}


//==============================================================================
Placement Stemmer::placement (int shift, int barShift) const
{
    // привязка: индекс доли сетки + off = номер доли проекта
    const auto& grid = result.grid; Placement P;
    if (grid.size() < 2) return P;
    const double beat = 60.0 / result.projectBpm, meanGap = (grid.back() - grid.front()) / (double) (grid.size() - 1);
    int d0 = 0; const int ph = ((shift % 4) + 4) % 4;
    for (int j = 0; j < (int) grid.size(); ++j) if (j % 4 == ph && grid[(size_t) j] >= result.musicStart - 0.6 * meanGap) { d0 = j; break; }
    const int Kbar = juce::jmax (0, juce::roundToInt (grid[(size_t) d0] / (4.0 * beat)) + barShift);
    P.d0 = d0; P.bar = Kbar; P.off = Kbar * 4 - d0;
    P.songStartSec = grid[(size_t) d0]; P.placedAtSec = Kbar * 4 * beat; P.projectBar = Kbar + 1;
    return P;
}

double Stemmer::srcTime (double q, int off) const       // доля проекта -> время в исходном файле
{
    const auto& grid = result.grid;
    const double j = q - off, n = (double) grid.size();
    if (j < 0) return grid[0] + j * (grid[1] - grid[0]);
    if (j > n - 1) return grid.back() + (j - (n - 1)) * (grid.back() - grid[grid.size() - 2]);
    return interpIdx (j, grid);
}

// первая доля такта: бочка на «раз», малый не на «раз», сбивки заканчиваются у тактовой черты
int Stemmer::chooseShift() const
{
    const auto& grid = result.grid; const auto& atk = result.hits; const double musicStart = result.musicStart, snarePitch = result.snarePitch;
    double bestScore = -1e9; int best = 0;
    for (int sh = 0; sh < 4; ++sh)
    {
        double k0 = 0, kq = 1e-9, s0 = 0, sq = 1e-9; std::map<int, std::vector<int>> hits;
        for (size_t r = 0; r < atk.size(); ++r)
        {
            const int step = juce::roundToInt ((gridPos (atk[r].t, grid) - sh) * 4.0); if (step < 0) continue; const int s = step % 16;
            if (atk[r].kick > 0.3f && s % 4 == 0) { kq += 1; if (s == 0) k0 += 1; }
            const bool sn = atk[r].body > 0.45f && atk[r].pitch >= 0.8f * (float) snarePitch;
            if (sn && s % 4 == 0) { sq += 1; if (s == 0) s0 += 1; }
            if (atk[r].body > 0.3f && atk[r].pitch >= 95.0f) hits[step / 16].push_back (s);
        }
        int runs = 0, endAtBar = 0;
        for (auto& hv : hits)
        {
            auto u = hv.second; std::sort (u.begin(), u.end()); u.erase (std::unique (u.begin(), u.end()), u.end());
            for (size_t a = 0; a < u.size();) { size_t b = a; while (b + 1 < u.size() && u[b + 1] - u[b] <= 2) ++b; if (b - a + 1 >= 4) { ++runs; if (u[b] >= 12) ++endAtBar; } a = b + 1; }
        }
        // песня обычно начинается с сильной доли: первая доля такта рядом с началом музыки
        double startBonus = 0.0; const double gap = (grid.back() - grid.front()) / (double) (grid.size() - 1);
        for (int j = sh; j < (int) grid.size(); j += 4) { if (std::abs (grid[(size_t) j] - musicStart) < 0.35 * gap) startBonus = 0.5; if (grid[(size_t) j] > musicStart + gap) break; }
        const double score = k0 / kq - s0 / sq + (runs >= 2 ? 2.0 * endAtBar / runs : 0.0) + startBonus;
        if (juce::SystemStats::getEnvironmentVariable ("DZOD_DEBUG", {}).isNotEmpty()) std::cout << "shift " << sh << " kick0 " << k0 << "/" << kq << " snare0 " << s0 << "/" << sq << " runs " << runs << " endAtBar " << endAtBar << " score " << score << std::endl;
        if (score > bestScore) { bestScore = score; best = sh; }
    }
    return best;
}


//==============================================================================
// WSOLA по карте времени: сдвиги окон считаются один раз по опорному звуку (полная песня / сумма стемов)
// и применяются ко всем стемам одинаково - стемы между собой не расходятся. Высота не меняется.
bool Stemmer::planStretch (const std::vector<float>& mono, double sr, double durOut, const std::function<double (int)>& srcSecOfFrame,
                           StretchPlan& plan, const std::function<bool (float)>& progress)
{
    const int N = StretchPlan::N, Hs = StretchPlan::Hs, DLT = 600, PAD = StretchPlan::PAD, DEC = 4;
    const int nfr = juce::jmax (1, (int) (durOut * sr / Hs));
    // уменьшенная копия для поиска лучшего совмещения окон
    Vec dec ((mono.size() + 2 * PAD) / DEC + N, 0.0f);
    for (size_t i = 0; i + DEC <= mono.size(); i += DEC) { float s = 0; for (int q = 0; q < DEC; ++q) s += mono[i + (size_t) q]; dec[(i + PAD) / DEC] = s / DEC; }
    std::vector<juce::int64> X ((size_t) nfr); juce::int64 prev = -1; const juce::int64 maxC = (juce::int64) mono.size() + PAD - DLT - N;
    const int nT = N / DEC, nL = DLT / DEC;
    for (int m = 0; m < nfr; ++m)
    {
        if (m % 2000 == 0 && progress && ! progress ((float) m / nfr)) return false;
        juce::int64 c = (juce::int64) (srcSecOfFrame (m) * sr); c = juce::jlimit ((juce::int64) (-PAD + DLT), maxC, c) + PAD; juce::int64 x = c;
        if (prev >= 0)
        {
            if (std::abs (c - (prev + Hs)) <= 2) x = prev + Hs;
            else
            {
                const float* tgt = dec.data() + (prev + Hs) / DEC; float pkv = 0; for (int i = 0; i < nT; ++i) pkv = juce::jmax (pkv, std::abs (tgt[i]));
                if (pkv > 1e-4f)
                {
                    const float* reg = dec.data() + (c - DLT) / DEC; float best = -1e30f; int bl = nL;
                    for (int l = 0; l <= 2 * nL; ++l) { float s = 0; const float* r = reg + l; for (int i = 0; i < nT; ++i) s += r[i] * tgt[i]; if (s > best) { best = s; bl = l; } }
                    x = c - DLT + (juce::int64) bl * DEC;
                }
            }
        }
        X[(size_t) m] = x; prev = x;
    }
    plan.X.swap (X); plan.frames = nfr;
    return true;
}

juce::AudioBuffer<float> Stemmer::applyStretch (const juce::AudioBuffer<float>& in, const StretchPlan& plan)
{
    const int N = StretchPlan::N, Hs = StretchPlan::Hs, PAD = StretchPlan::PAD, nfr = plan.frames;
    Vec win ((size_t) N); for (int i = 0; i < N; ++i) win[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2 * kPi * i / N));
    const int ch = in.getNumChannels(), len = in.getNumSamples(); juce::AudioBuffer<float> out (ch, nfr * Hs + N); out.clear();
    for (int c = 0; c < ch; ++c)
    {
        const float* src = in.getReadPointer (c); float* dst = out.getWritePointer (c);
        for (int m = 0; m < nfr; ++m)
        {
            const juce::int64 s0 = plan.X[(size_t) m] - PAD; float* d = dst + (size_t) m * Hs;
            for (int i = 0; i < N; ++i) { const juce::int64 s = s0 + i; if (s >= 0 && s < len) d[i] += src[s] * win[(size_t) i]; }
        }
    }
    return out;
}

//==============================================================================
// Выравнивание: WSOLA по карте времени. Сдвиги окон считаются один раз по полной песне
// и применяются ко всем стемам одинаково, чтобы стемы не разошлись между собой.
bool Stemmer::renderAligned (int shift, int barShift, const juce::File& outFolder, bool mp3, juce::StringArray& written, juce::String& errors, Progress progress)
{
    if (! ready || audio == nullptr) return false;
    const auto P = placement (shift, barShift); const int off = P.off;
    const double sr = audio->sr, beat = 60.0 / jobData.projectBpm;
    const auto& mono = audio->fullMono; const double dur = (double) mono.size() / sr;
    const double durOut = ((double) result.grid.size() - 1 + off) * beat + (dur - result.grid.back()) + 0.5;
    StretchPlan plan;
    if (! planStretch (mono, sr, durOut, [&] (int m) { return srcTime ((double) m * StretchPlan::Hs / sr / beat, off); }, plan,
                       [&] (float f) { return progress == nullptr || progress (0.3f * f, ru ("Расчёт выравнивания")); }))
        return false;
    juce::Array<juce::File> files; files.add (jobData.full); files.add (jobData.drums); for (auto& f : jobData.others) files.addIfNotAlreadyThere (f);
    outFolder.createDirectory(); written.clear();
    const juce::String suffix = "_aligned_" + juce::String (juce::roundToInt (jobData.projectBpm)) + "bpm";
    const double outRate = 48000.0;
    for (int fi = 0; fi < files.size(); ++fi)
    {
        if (progress && ! progress (0.3f + 0.7f * fi / files.size(), ru ("Выравнивание: ") + files[fi].getFileNameWithoutExtension())) return false;
        juce::AudioBuffer<float> in; double fsr = 0; juce::String err;
        if (! loadAudio (files[fi], in, fsr, err) || std::abs (fsr - sr) > 1.0) { errors << ru ("Пропущен файл (не читается или другая частота): ") << files[fi].getFileName() << "\n"; continue; }
        const int ch = in.getNumChannels(); juce::AudioBuffer<float> out = applyStretch (in, plan);
        // песня целиком остаётся стерео, отдельные стемы сводятся в моно
        const bool stereo = files[fi] == jobData.full && ch >= 2;
        juce::AudioBuffer<float> mix (stereo ? 2 : 1, out.getNumSamples());
        if (stereo) { mix.copyFrom (0, 0, out, 0, 0, out.getNumSamples()); mix.copyFrom (1, 0, out, 1, 0, out.getNumSamples()); }
        else
        {
            mix.clear();
            for (int c = 0; c < ch; ++c) mix.addFrom (0, 0, out, c, 0, out.getNumSamples(), 1.0f / (float) ch);
        }
        out.setSize (0, 0);
        if (std::abs (sr - outRate) > 1.0) mix = resampled (mix, sr, outRate);

        const auto base = files[fi].getFileNameWithoutExtension() + suffix;
        outFolder.getChildFile (base + ".wav").deleteFile(); outFolder.getChildFile (base + ".mp3").deleteFile();     // прошлый результат в другом формате не должен путаться под рукой
        const auto target = outFolder.getChildFile (base + (mp3 ? ".mp3" : ".wav"));
        bool ok = false;
        if (mp3) ok = writeMp3 (target, mix, outRate, 320, true);
        else
        {
            juce::WavAudioFormat wav; std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (target), outRate, (unsigned int) mix.getNumChannels(), 24, {}, 0));
            ok = w != nullptr && w->writeFromAudioSampleBuffer (mix, 0, mix.getNumSamples());
        }
        if (! ok) { errors << ru ("Не удалось записать: ") << target.getFullPathName() << "\n"; continue; }
        written.add (target.getFullPathName());
    }
    if (progress) progress (1.0f, ru ("Готово"));
    return ! written.isEmpty();
}


//==============================================================================
// Arrange: роли стемов, такты, части песни, растяжка куска
juce::String Stemmer::roleFromFileName (const juce::String& fileName)
{
    const auto n = fileName.toLowerCase();
    if (n.contains ("backing") || n.contains ("back vocal") || n.contains ("backvocal") || n.contains ("bv")) return "backs";
    if (n.contains ("vocal") || n.contains ("vox")) return "vocals";
    if (n.contains ("drum")) return "drums";
    if (n.contains ("percussion")) return "percussion";
    if (n.contains ("bass")) return "bass";
    if (n.contains ("guitar")) return "guitar";
    if (n.contains ("keyboard") || n.contains ("piano") || n.contains ("keys")) return "keys";
    if (n.contains ("synth")) return "synth";
    if (n.contains ("string")) return "strings";
    if (n.contains ("brass")) return "brass";
    if (n.contains ("woodwind")) return "woodwinds";
    if (n.contains ("fx")) return "fx";
    return "other";
}

namespace
{
float cosSim (const float* a, const float* b, int n)
{
    double ab = 0, aa = 1e-12, bb = 1e-12;
    for (int i = 0; i < n; ++i) { ab += (double) a[i] * b[i]; aa += (double) a[i] * a[i]; bb += (double) b[i] * b[i]; }
    return (float) (ab / std::sqrt (aa * bb));
}
}

bool Stemmer::analyseSections (const juce::Array<RoleFile>& stems, Progress progress)
{
    if (! ready || result.grid.size() < 12) { error = ru ("Сначала нужен анализ сетки"); return false; }
    auto say = [&] (float p, const char* t) { return progress == nullptr || progress (p, ru (t)); };
    const auto& grid = result.grid; const double sr = result.sampleRate;
    // такты: от первой сильной доли после начала музыки, по 4 доли
    const auto P = placement (result.autoShift, 0);
    std::vector<int> barIdx; for (int j = P.d0; j + 4 < (int) grid.size(); j += 4) barIdx.push_back (j);
    const int nb = (int) barIdx.size(); if (nb < 8) { error = ru ("Слишком мало тактов"); return false; }
    result.bars.clear(); for (int j : barIdx) result.bars.push_back (grid[(size_t) j]);
    auto barStart = [&] (int b) { return grid[(size_t) barIdx[(size_t) b]]; };
    auto barEnd = [&] (int b) { return grid[(size_t) barIdx[(size_t) b] + 4]; };

    // --- громкость каждой роли по тактам и хрома гармонических стемов
    std::map<juce::String, std::vector<float>> levelDb;          // роль -> дБ по тактам
    std::map<juce::String, std::vector<float>> timbre;           // роль -> 8 полос по тактам
    std::vector<float> chroma ((size_t) nb * 12, 0.0f);
    const int order = 13, NF = 1 << order, hop = NF / 2; juce::dsp::FFT fft (order);
    Vec win ((size_t) NF); for (int i = 0; i < NF; ++i) win[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2 * kPi * i / NF));
    int done = 0;
    for (auto& st : stems)
    {
        if (! say (0.05f + 0.75f * done++ / juce::jmax (1, stems.size()), "Части песни: стемы")) return false;
        juce::AudioBuffer<float> b; double fsr = 0; juce::String err;
        if (! loadAudio (st.file, b, fsr, err)) continue;
        if (std::abs (fsr - sr) > 1.0) b = resampled (b, fsr, sr);
        const Vec m = toMono (b); b.setSize (0, 0);
        auto& L = levelDb[st.role]; if (L.empty()) L.assign ((size_t) nb, 0.0f);
        for (int k = 0; k < nb; ++k)
        {
            const size_t a = (size_t) juce::jmax (0.0, barStart (k) * sr), e = juce::jmin (m.size(), (size_t) (barEnd (k) * sr)); double q = 0;
            for (size_t i = a; i < e; ++i) q += (double) m[i] * m[i];
            L[(size_t) k] += (float) (q / (double) juce::jmax ((size_t) 1, e - a));          // мощность; в дБ ниже
        }
        const bool harmonic = ! (st.role == "drums" || st.role == "percussion" || st.role == "fx");
        auto& Tb = timbre[st.role]; if (Tb.empty()) Tb.assign ((size_t) nb * 8, 0.0f);
        // хрома (55-2000 Гц, лог-сжатие) и тембр: 8 полос 40 Гц - 16 кГц
        Vec buf ((size_t) NF * 2);
        for (int k = 0; k < nb; ++k)
        {
            const int a = (int) (barStart (k) * sr), e = (int) (barEnd (k) * sr);
            for (int s0 = a; s0 + NF <= e && s0 + NF <= (int) m.size(); s0 += hop)
            {
                std::fill (buf.begin(), buf.end(), 0.0f);
                for (int i = 0; i < NF; ++i) buf[(size_t) i] = m[(size_t) (s0 + i)] * win[(size_t) i];
                fft.performFrequencyOnlyForwardTransform (buf.data(), true);
                for (int i = 1; i < NF / 2; ++i)
                {
                    const double f = i * sr / NF;
                    if (f >= 40.0 && f < 16000.0) Tb[(size_t) k * 8 + (size_t) juce::jlimit (0, 7, (int) (std::log2 (f / 40.0) / std::log2 (400.0) * 8.0))] += buf[(size_t) i] * buf[(size_t) i];
                    if (! harmonic || f < 55.0 || f > 2000.0) continue;
                    const int pc = ((int) std::lround (12.0 * std::log2 (f / 440.0)) % 12 + 12 + 9) % 12;      // 0 = C
                    chroma[(size_t) k * 12 + (size_t) pc] += std::log1p (buf[(size_t) i]);
                }
            }
        }
    }
    if (! say (0.85f, "Части песни: границы")) return false;
    for (auto& kv : levelDb) for (auto& v : kv.second) v = (float) (10.0 * std::log10 (v + 1e-12));
    std::vector<float> total ((size_t) nb, 0.0f);
    for (int k = 0; k < nb; ++k) { double pw = 0; for (auto& kv : levelDb) pw += std::pow (10.0, kv.second[(size_t) k] / 10.0); total[(size_t) k] = (float) (10.0 * std::log10 (pw + 1e-12)); }

    // --- рисунок барабанов: 16 шагов x (бочка, корпус) + тарелки
    std::vector<float> drumPat ((size_t) nb * 48, 0.0f);
    {
        std::vector<double> bt (result.bars); bt.push_back (barEnd (nb - 1));
        auto place = [&] (double t, int& bar, int& step)
        {
            const auto it = std::upper_bound (bt.begin(), bt.end(), t); bar = (int) (it - bt.begin()) - 1;
            if (bar < 0 || bar >= nb) return false;
            const double gp = gridPos (t, grid) - (double) barIdx[(size_t) bar]; step = juce::jlimit (0, 15, (int) std::lround (gp * 4.0)); return true;
        };
        for (auto& h : result.hits) { int b, st; if (place (h.t, b, st)) { drumPat[(size_t) b * 48 + (size_t) st] += h.kick; drumPat[(size_t) b * 48 + 16 + (size_t) st] += h.body; } }
        for (auto& h : result.hi) { int b, st; if (place (h.t, b, st)) drumPat[(size_t) b * 48 + 32 + (size_t) st] += 1.0f; }
    }
    // --- сходство тактов
    // тембр: дБ полос, каждая полоса - z-оценка по тактам песни
    const int td = 8 * (int) timbre.size(); std::vector<float> tz ((size_t) nb * (size_t) juce::jmax (1, td), 0.0f);
    {
        int o = 0;
        for (auto& kv : timbre)
        {
            for (int d = 0; d < 8; ++d)
            {
                double m1 = 0, m2 = 0; for (int k = 0; k < nb; ++k) { const double v = 10.0 * std::log10 (kv.second[(size_t) k * 8 + (size_t) d] + 1e-12); tz[(size_t) k * td + (size_t) (o + d)] = (float) v; m1 += v; m2 += v * v; }
                m1 /= nb; const double sdv = std::sqrt (juce::jmax (1e-9, m2 / nb - m1 * m1));
                for (int k = 0; k < nb; ++k) { auto& v = tz[(size_t) k * td + (size_t) (o + d)]; v = (float) ((v - m1) / sdv); }
            }
            o += 8;
        }
    }
    float wC = 0.35f, wD = 0.2f, wT = 0.3f, wE = 0.15f;
    {
        juce::StringArray w; w.addTokens (juce::SystemStats::getEnvironmentVariable ("DZOD_SECW", {}), ",", "");
        if (w.size() == 4) { wC = w[0].getFloatValue(); wD = w[1].getFloatValue(); wT = w[2].getFloatValue(); wE = w[3].getFloatValue(); }
    }
    auto roleVec = [&] (int k, std::vector<float>& v) { v.clear(); for (auto& kv : levelDb) v.push_back (kv.second[(size_t) k]); };
    std::vector<float> S ((size_t) nb * nb);
    std::vector<float> va, vb;
    for (int i = 0; i < nb; ++i)
        for (int j = i; j < nb; ++j)
        {
            roleVec (i, va); roleVec (j, vb); double d = 0; for (size_t q = 0; q < va.size(); ++q) d += std::abs (juce::jmax (-70.0f, va[q]) - juce::jmax (-70.0f, vb[q]));
            d /= (double) juce::jmax ((size_t) 1, va.size());
            const float sim = wC * cosSim (&chroma[(size_t) i * 12], &chroma[(size_t) j * 12], 12) + wD * cosSim (&drumPat[(size_t) i * 48], &drumPat[(size_t) j * 48], 48)
                            + wT * (td > 0 ? 0.5f + 0.5f * cosSim (&tz[(size_t) i * td], &tz[(size_t) j * td], td) : 0.5f) + wE * (float) std::exp (-d / 6.0);
            S[(size_t) i * nb + j] = S[(size_t) j * nb + i] = sim;
        }
    // --- новизна: шахматное ядро 2 x 4 такта
    const int Lk = juce::jlimit (2, 8, juce::SystemStats::getEnvironmentVariable ("DZOD_SECK", "4").getIntValue()); std::vector<float> nov ((size_t) nb, 0.0f);
    const double thrK = juce::SystemStats::getEnvironmentVariable ("DZOD_SECT", "0.3").getDoubleValue();
    for (int i = Lk; i <= nb - Lk; ++i)
    {
        double v = 0;
        for (int a = -Lk; a < Lk; ++a) for (int b = -Lk; b < Lk; ++b) { const double w = ((a < 0) == (b < 0)) ? 1.0 : -1.0; v += w * S[(size_t) (i + a) * nb + (i + b)]; }
        nov[(size_t) i] = (float) juce::jmax (0.0, v) * (i % 4 == 0 ? 1.15f : i % 2 == 0 ? 1.05f : 1.0f);
    }
    if (juce::SystemStats::getEnvironmentVariable ("DZOD_DEBUG", {}).isNotEmpty()) { std::cout << "nov:"; for (int i = 0; i < nb; ++i) std::cout << " " << i << ":" << juce::String (nov[(size_t) i], 1); std::cout << std::endl; }
    double mean = 0, sd = 0; for (float v : nov) mean += v; mean /= nb; for (float v : nov) sd += (v - mean) * (v - mean); sd = std::sqrt (sd / nb);
    // --- повторы: диагонали матрицы сходства (блок из 4 тактов повторяется через L тактов) - начала и концы повторов тоже границы
    {
        const double alpha = juce::SystemStats::getEnvironmentVariable ("DZOD_SECR", "0.5").getDoubleValue();
        if (alpha > 0.0)
        {
            double m1 = 0, m2 = 0; int c = 0;
            for (int i = 0; i < nb; ++i) for (int j = i + 8; j < nb; ++j) { const double v = S[(size_t) i * nb + j]; m1 += v; m2 += v * v; ++c; }
            m1 /= juce::jmax (1, c); const double sdS = std::sqrt (juce::jmax (1e-12, m2 / juce::jmax (1, c) - m1 * m1));
            const double thrR = m1 + juce::SystemStats::getEnvironmentVariable ("DZOD_SECRT", "0.5").getDoubleValue() * sdS;
            const int W = 4; std::vector<float> rep ((size_t) nb + 1, 0.0f);
            for (int L = 8; L + W < nb; ++L)
            {
                int runStart = -1;
                for (int i = 0; i + L + W <= nb + 1; ++i)
                {
                    bool high = false;
                    if (i + L + W <= nb) { double d = 0; for (int k = 0; k < W; ++k) d += S[(size_t) (i + k) * nb + (i + L + k)]; high = d / W > thrR; }
                    if (high && runStart < 0) runStart = i;
                    if (! high && runStart >= 0)
                    {
                        const int len = i - runStart; const int endBar = i - 1 + W;
                        if (len >= 4)
                            for (int b : { runStart, runStart + L, endBar, endBar + L }) if (b > 0 && b < nb) rep[(size_t) b] += (float) len;
                        runStart = -1;
                    }
                }
            }
            float rmax = 0; for (float v : rep) rmax = juce::jmax (rmax, v);
            double nm = 0; for (float v : nov) nm += v; nm /= juce::jmax (1, nb);
            if (rmax > 0) for (int i = 0; i < nb; ++i) nov[(size_t) i] = (float) (nov[(size_t) i] + alpha * nm * 3.0 * rep[(size_t) i] / rmax);
            mean = 0; sd = 0; for (float v : nov) mean += v; mean /= nb; for (float v : nov) sd += (v - mean) * (v - mean); sd = std::sqrt (sd / nb);
        }
    }
    std::vector<int> cuts { 0 };
    {
        std::vector<int> order; for (int i = 1; i < nb; ++i) if (nov[(size_t) i] > mean + thrK * sd && nov[(size_t) i] >= nov[(size_t) i - 1] && (i + 1 >= nb || nov[(size_t) i] >= nov[(size_t) i + 1])) order.push_back (i);
        std::sort (order.begin(), order.end(), [&] (int a, int b) { return nov[(size_t) a] > nov[(size_t) b]; });
        std::vector<int> keep; for (int i : order) { bool ok = i >= 4 && i <= nb - 2; for (int k : keep) ok = ok && std::abs (k - i) >= 4; if (ok) keep.push_back (i); }
        cuts.insert (cuts.end(), keep.begin(), keep.end()); std::sort (cuts.begin(), cuts.end());
    }
    cuts.push_back (nb);
    // --- части, группы, названия
    struct Seg { int a, b; float level, vocal; int group; };
    std::vector<Seg> segs;
    const auto vit = levelDb.find ("vocals"); float vocMax = -200; if (vit != levelDb.end()) for (float v : vit->second) vocMax = juce::jmax (vocMax, v);
    for (size_t c = 0; c + 1 < cuts.size(); ++c)
    {
        Seg g { cuts[c], cuts[c + 1], 0, -200, (int) c };
        double pw = 0, vp = 0; for (int k = g.a; k < g.b; ++k) { pw += std::pow (10.0, total[(size_t) k] / 10.0); if (vit != levelDb.end()) vp += std::pow (10.0, vit->second[(size_t) k] / 10.0); }
        g.level = (float) (10.0 * std::log10 (pw / (g.b - g.a) + 1e-12)); g.vocal = (float) (10.0 * std::log10 (vp / (g.b - g.a) + 1e-12));
        segs.push_back (g);
    }
    auto segSim = [&] (const Seg& x, const Seg& y)
    {
        const int n = juce::jmin (x.b - x.a, y.b - y.a); double s2 = 0; for (int k = 0; k < n; ++k) s2 += S[(size_t) (x.a + k) * nb + (y.a + k)]; return s2 / juce::jmax (1, n);
    };
    if (juce::SystemStats::getEnvironmentVariable ("DZOD_DEBUG", {}).isNotEmpty())
        for (size_t i = 0; i < segs.size(); ++i) { std::cout << "seg " << segs[i].a + 1 << "-" << segs[i].b << ":"; for (size_t j = 0; j < segs.size(); ++j) std::cout << " " << juce::String (segSim (segs[i], segs[j]), 2); std::cout << std::endl; }
    // повторы: порог относительно сходства частей этой песни (у плотного металла сходство сжато в 0,55-0,9)
    double ms = 0, ms2 = 0; int mc = 0;
    for (size_t i = 0; i < segs.size(); ++i) for (size_t j = 0; j < i; ++j) { const double v = segSim (segs[i], segs[j]); ms += v; ms2 += v * v; ++mc; }
    const double mMean = mc ? ms / mc : 0.7, mSd = mc ? std::sqrt (juce::jmax (0.0, ms2 / mc - mMean * mMean)) : 0.05;
    const double groupThr = juce::jmax (0.74, mMean + 1.0 * mSd);
    for (size_t i = 0; i < segs.size(); ++i)
    {
        double bestS = groupThr; int bestG = -1;
        for (size_t j = 0; j < i; ++j) { const double v = segSim (segs[i], segs[j]); if (v > bestS) { bestS = v; bestG = segs[j].group; } }
        if (bestG >= 0) segs[i].group = bestG;
    }
    // названия: Intro / Outro - края без вокала; остальное - буквы по повторам (A, B, C...), припев - догадка:
    // повторяющаяся часть с вокалом, самая громкая. Автор переименовывает.
    const float vocThr = juce::jmax (-50.0f, vocMax - 18.0f);
    auto hasVoc = [&] (const Seg& g) { return vit != levelDb.end() && g.vocal > vocThr; };
    std::map<int, int> groupCount; std::map<int, double> groupLevel;
    for (auto& g : segs) { ++groupCount[g.group]; groupLevel[g.group] += g.level; }
    int chorusGroup = -1; double best = -1e9;
    for (auto& g : segs) if (hasVoc (g) && groupCount[g.group] >= 2) { const double sc = groupLevel[g.group] / groupCount[g.group]; if (sc > best) { best = sc; chorusGroup = g.group; } }
    result.sections.clear(); std::map<int, juce::String> letterOf; std::map<int, int> seen; char next = 'A';
    int firstVoc = -1, lastVoc = -1; for (int i = 0; i < (int) segs.size(); ++i) if (hasVoc (segs[(size_t) i])) { if (firstVoc < 0) firstVoc = i; lastVoc = i; }
    for (int i = 0; i < (int) segs.size(); ++i)
    {
        const auto& g = segs[(size_t) i]; juce::String name;
        if (! hasVoc (g) && firstVoc >= 0 && i < firstVoc) name = "Intro";
        else if (! hasVoc (g) && firstVoc >= 0 && i > lastVoc) name = "Outro";
        else
        {
            if (! letterOf.count (g.group)) letterOf[g.group] = juce::String::charToString ((juce::juce_wchar) (next <= 'Z' ? next++ : 'Z'));
            name = (g.group == chorusGroup ? juce::String ("Chorus ") : juce::String ("Part ")) + letterOf[g.group];
            if (groupCount[g.group] >= 2) name << juce::String (++seen[g.group]);
            if (! hasVoc (g)) name << " (instr)";
        }
        Section sct; sct.startBar = g.a; sct.endBar = g.b; sct.startSec = barStart (g.a); sct.endSec = g.b < nb ? barStart (g.b) : barEnd (nb - 1);
        sct.label = name; sct.group = g.group; sct.vocals = hasVoc (g); sct.levelDb = g.level;
        result.sections.push_back (sct);
    }
    return say (1.0f, "Готово");
}

bool Stemmer::pieceMap (double srcFromSec, double srcToSec, double targetBpm, std::function<double (int)>& srcSecOfFrame, double& durOutSec,
                        double& snappedFromSec, double& snappedToSec) const
{
    const auto& grid = result.grid;
    if (! ready || grid.size() < 2 || targetBpm <= 1.0 || srcToSec <= srcFromSec) return false;
    const int j0 = juce::jlimit (0, (int) grid.size() - 1, (int) std::lround (gridPos (srcFromSec, grid)));
    const int j1 = juce::jlimit (j0 + 1, (int) grid.size() - 1, (int) std::lround (gridPos (srcToSec, grid)));
    snappedFromSec = grid[(size_t) j0]; snappedToSec = grid[(size_t) j1];
    const double beat = 60.0 / targetBpm, sr = result.sampleRate; durOutSec = (j1 - j0) * beat;
    const std::vector<double> g (grid);
    srcSecOfFrame = [g, j0, beat, sr] (int m)
    {
        const double q = (double) m * Stemmer::StretchPlan::Hs / sr / beat + j0, n = (double) g.size();
        if (q > n - 1) return g.back() + (q - (n - 1)) * (g.back() - g[g.size() - 2]);
        return interpIdx (q, g);
    };
    return true;
}

//==============================================================================
juce::File Stemmer::writeManifest (const Job& job, const juce::StringArray& alignedFiles, int projectBar, double placedAtSec,
                                   const juce::File& folder, const juce::String& writer)
{
    // для DZOD Studio - список выровненных стемов (группа Stems). Пишется ПОСЛЕДНИМ, после всех аудиофайлов.
    auto roleOf = [&job] (const juce::File& f)
    {
        const auto n = f.getFileName();
        if (job.full != juce::File() && n.startsWith (job.full.getFileNameWithoutExtension() + "_aligned_")) return juce::String ("full");
        if (job.drums != juce::File() && n.startsWith (job.drums.getFileNameWithoutExtension() + "_aligned_")) return juce::String ("drums");
        return juce::String ("other");
    };
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    juce::Array<juce::var> list;
    for (auto& path : alignedFiles)
    {
        const juce::File f (path);
        if (! f.existsAsFile()) continue;
        auto* o = new juce::DynamicObject();
        o->setProperty ("file", f.getRelativePathFrom (folder).replaceCharacter ('\\', '/'));
        o->setProperty ("role", roleOf (f));
        o->setProperty ("name", f.getFileNameWithoutExtension().upToFirstOccurrenceOf ("_aligned_", false, false));
        if (std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f)); r != nullptr)
        {
            o->setProperty ("sampleRate", r->sampleRate);
            o->setProperty ("channels", (int) r->numChannels);
            o->setProperty ("lengthSamples", (juce::int64) r->lengthInSamples);
        }
        o->setProperty ("startSec", 0.0);                           // все стемы - с начала проекта, с нуля
        list.add (juce::var (o));
    }
    auto* root = new juce::DynamicObject();
    root->setProperty ("format", 1);
    root->setProperty ("writer", writer);
    root->setProperty ("writtenAt", juce::Time::getCurrentTime().toISO8601 (true));
    root->setProperty ("projectBpm", job.projectBpm);
    root->setProperty ("songStartsAtBar", projectBar);
    root->setProperty ("songStartSec", placedAtSec);
    root->setProperty ("analysis", "dzod_stemmer.json");
    root->setProperty ("stems", list);
    const auto manifest = folder.getChildFile ("dzod_stems.json");
    manifest.replaceWithText (juce::JSON::toString (juce::var (root)), false, false, "\n");
    return manifest;
}

//==============================================================================
// dzod_stemmer.json - связь с Composer: сетка, удары и привязка. Числа - с полной точностью (снятая партия совпадает побайтно).
namespace
{
juce::var num (double v)            // строкой: кратчайшая запись, которая читается обратно в то же число; не зависит от языка системы
{
    char b[64]; const auto r = std::to_chars (b, b + sizeof (b), v);
    return juce::var (juce::String (b, (size_t) (r.ptr - b)));
}
double numOf (const juce::var& v)
{
    if (! v.isString()) return (double) v;
    const auto t = v.toString().toStdString(); double x = 0; std::from_chars (t.data(), t.data() + t.size(), x); return x;
}
}

bool Stemmer::writeAnalysis (const juce::File& target, int shift, int barShift, const juce::String& writer) const
{
    if (! ready) return false;
    const auto P = placement (shift, barShift);
    auto* root = new juce::DynamicObject();
    root->setProperty ("format", 1);
    root->setProperty ("writer", writer);
    root->setProperty ("writtenAt", juce::Time::getCurrentTime().toISO8601 (true));
    root->setProperty ("drumsFile", jobData.drums.getFileName());
    root->setProperty ("fullFile", jobData.full.getFileName());
    root->setProperty ("sampleRate", num (result.sampleRate));
    root->setProperty ("projectBpm", num (result.projectBpm));
    root->setProperty ("musicStart", num (result.musicStart));
    root->setProperty ("bpmMin", num (result.bpmMin)); root->setProperty ("bpmMedian", num (result.bpmMedian)); root->setProperty ("bpmMax", num (result.bpmMax));
    root->setProperty ("snarePitch", num (result.snarePitch));
    root->setProperty ("autoShift", result.autoShift);
    root->setProperty ("shift", shift);                          // выбранная привязка (autoShift + сдвиг долей)
    root->setProperty ("barShift", barShift);
    root->setProperty ("songStartsAtBar", P.projectBar);
    root->setProperty ("songStartSec", num (P.placedAtSec));
    root->setProperty ("sourceStartSec", num (P.songStartSec));
    juce::Array<juce::var> g; for (double x : result.grid) g.add (num (x));
    root->setProperty ("grid", g);
    juce::Array<juce::var> hs;
    for (auto& h : result.hits) { juce::Array<juce::var> a { num (h.t), num (h.kick), num (h.body), num (h.stick), num (h.pitch), num (h.tail) }; hs.add (a); }
    root->setProperty ("hitsColumns", "t kick body stick pitch tail");
    root->setProperty ("hits", hs);
    juce::Array<juce::var> hh;
    for (auto& h : result.hi) { juce::Array<juce::var> a { num (h.t), num (h.sustain), num (h.level) }; hh.add (a); }
    root->setProperty ("cymbalsColumns", "t sustain level");
    root->setProperty ("cymbals", hh);
    root->setProperty ("tempoEstimate", num (result.tempoEstimate));
    if (! result.bars.empty()) { juce::Array<juce::var> b; for (double x : result.bars) b.add (num (x)); root->setProperty ("bars", b); }
    if (! result.sections.empty())
    {
        juce::Array<juce::var> ss;
        for (auto& sc : result.sections)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("label", sc.label); o->setProperty ("startBar", sc.startBar); o->setProperty ("endBar", sc.endBar);
            o->setProperty ("startSec", num (sc.startSec)); o->setProperty ("endSec", num (sc.endSec));
            o->setProperty ("group", sc.group); o->setProperty ("vocals", sc.vocals); o->setProperty ("levelDb", num (sc.levelDb));
            ss.add (juce::var (o));
        }
        root->setProperty ("sections", ss);
    }
    return target.replaceWithText (juce::JSON::toString (juce::var (root), true), false, false, "\n");
}

bool Stemmer::readAnalysis (const juce::File& source)
{
    ready = false; error.clear(); audio.reset(); result = {};
    const auto v = juce::JSON::parse (source);
    if (! v.isObject() || (int) v["format"] != 1) { error = ru ("Не удалось прочитать анализ: ") + source.getFileName(); return false; }
    result.sampleRate = numOf (v["sampleRate"]); result.projectBpm = numOf (v["projectBpm"]); result.musicStart = numOf (v["musicStart"]);
    result.bpmMin = numOf (v["bpmMin"]); result.bpmMedian = numOf (v["bpmMedian"]); result.bpmMax = numOf (v["bpmMax"]);
    result.snarePitch = numOf (v["snarePitch"]); result.autoShift = (int) v["autoShift"];
    if (auto* g = v["grid"].getArray()) for (auto& x : *g) result.grid.push_back (numOf (x));
    if (auto* hs = v["hits"].getArray())
        for (auto& a : *hs)
            if (a.size() >= 6) result.hits.push_back ({ numOf (a[0]), (float) numOf (a[1]), (float) numOf (a[2]), (float) numOf (a[3]), (float) numOf (a[4]), (float) numOf (a[5]) });
    if (auto* hh = v["cymbals"].getArray())
        for (auto& a : *hh)
            if (a.size() >= 3) result.hi.push_back ({ numOf (a[0]), (float) numOf (a[1]), (float) numOf (a[2]) });
    result.tempoEstimate = numOf (v["tempoEstimate"]);
    if (auto* b = v["bars"].getArray()) for (auto& x : *b) result.bars.push_back (numOf (x));
    if (auto* ss = v["sections"].getArray())
        for (auto& o : *ss)
        {
            Section sc; sc.label = o["label"].toString(); sc.startBar = (int) o["startBar"]; sc.endBar = (int) o["endBar"];
            sc.startSec = numOf (o["startSec"]); sc.endSec = numOf (o["endSec"]); sc.group = (int) o["group"]; sc.vocals = (bool) o["vocals"]; sc.levelDb = (float) numOf (o["levelDb"]);
            result.sections.push_back (sc);
        }
    jobData = {}; jobData.projectBpm = result.projectBpm;
    jobData.drums = source.getSiblingFile (v["drumsFile"].toString()); jobData.full = source.getSiblingFile (v["fullFile"].toString());
    if (result.grid.size() < 2 || result.projectBpm <= 1.0) { error = ru ("В анализе нет сетки долей"); return false; }
    ready = true;
    return true;
}
} // namespace stemmer
} // namespace dzod
