#pragma once
// DZOD Arranger — подгонка звука к образцу (перенос расчёта DZOD Unifier 1.4 с Python на C++).
//   ltas      — усреднённый спектр в 1/3 октавы на сетке 20 Гц…20 кГц с шагом 1/12 октавы (121 точка), дБ;
//   eqCurve   — сглаженная разница с образцом, не больше ±limit дБ, без самого низа/верха и полос без сигнала;
//   applyEq   — линейно-фазовый КИХ-фильтр (8191 отвод), без сдвига во времени;
//   lufs      — интегральная громкость ITU-R BS.1770 (K-взвешивание, стробирование);
//   detectKey — тональность по хромаграмме (профили Крумхансла), для предупреждения о несовпадении соседних кусков.
// Только JUCE juce_dsp (БПФ) и juce_audio_basics. Без окна.
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <vector>

namespace dzod
{
namespace arranger
{
constexpr int kGrid = 121;                       // 20 Гц * 2^(k/12), k = 0..120
using Spectrum = std::array<double, kGrid>;

double gridHz (int k);
// Спектр участка [from, to) отсчётов (все каналы суммируются). to < 0 — до конца.
Spectrum ltas (const juce::AudioBuffer<float>& audio, double sampleRate, juce::int64 from = 0, juce::int64 to = -1);
// Спектр, приведённый к среднему уровню в полосе 100 Гц…8 кГц (сравниваем форму, а не громкость).
Spectrum normalised (const Spectrum&);
// Кривая коррекции «кусок -> образец», дБ на сетке. strength 0..1, limit — предел, дБ.
Spectrum eqCurve (const Spectrum& piece, const Spectrum& reference, double strength, double limitDb);
// Значения кривой на октавных частотах 31,5…16 000 Гц (для отчёта).
std::array<double, 10> octaveTable (const Spectrum& curve);
// Применить кривую ко всем каналам (на месте).
void applyEq (juce::AudioBuffer<float>& audio, double sampleRate, const Spectrum& curveDb);
// Громкость, LUFS (-70, если тишина). Участок [from, to) отсчётов.
double lufs (const juce::AudioBuffer<float>& audio, double sampleRate, juce::int64 from = 0, juce::int64 to = -1);
double peakDb (const juce::AudioBuffer<float>& audio);

struct Key
{
    int tonic = -1;          // 0 = C … 11 = B, -1 — не определена
    bool minor = false;
    float confidence = 0;    // корреляция с профилем, 0..1
    std::array<float, 12> chroma {};   // доли нот C…B (нормированы) — сравнение соседних кусков
    juce::String name() const;              // «A minor», «C major», «—»
    // те же ноты: та же тоника (мажор/минор у квинтовых аккордов не различить), относительная тональность или похожий набор нот
    bool compatibleWith (const Key&) const;   // тревога только при сходстве нот <= 0 (оба уверены)
    float similarity (const Key&) const;    // корреляция хромы, -1..1
};
// Тональность по гармоническим стемам (суммируются). [from, to) — секунды, to < 0 — до конца.
Key detectKey (const std::vector<const juce::AudioBuffer<float>*>& harmonic, double sampleRate, double fromSec = 0, double toSec = -1);
} // namespace arranger
} // namespace dzod
