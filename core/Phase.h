#pragma once
// DZOD Arranger — проверка фазы сетки по звуку барабанов (sound 31.10: THIN AS PAPER — бочка на «и», малый на долях,
// сетка тактов стояла на 1/8 мимо настоящей сильной доли).
//   1) доли: если бочка в основном между долями, а малый на долях — сетка сдвигается на полдоли;
//   2) такты: начало такта — доля, где бочка + тарелка (crash) сильнее всего (из 4 возможных).
// Поправка — только при явном перевесе; почти равные варианты помечаются ambiguous (окно спрашивает автора).
#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>

namespace dzod::arranger
{
struct PhaseCheck
{
    double kickOnDb = 0, kickOffDb = 0;      // энергия бочки на долях / между долями (по всей песне), дБ
    double snareOnDb = 0, snareOffDb = 0;    // то же для малого
    bool halfShifted = false;                // сетка сдвинута на полдоли
    int barPhase = 0;                        // начало такта — доля с номером barPhase (0..3) от начала сетки
    double barMarginDb = 0;                  // насколько выбранная фаза такта сильнее следующей
    bool barChanged = false;                 // такты Stemmer заменены
    bool ambiguous = false;                  // две фазы почти равны — спросить автора
    double kickOnBeatsPct = 0;               // доля тактов, где бочка на долях сильнее, чем между ними, %
    double kickOnBarStartPct = 0;            // доля тактов, где на первой доле бочка или тарелка, %
    bool checked = false;
    double phaseScore[4] {};                 // оценка начала такта на доле 0..3 (бочка + тарелка), дБ
};

// Проверить и при явном перевесе поправить сетку долей (grid) и начала тактов (bars). drums — стем барабанов (или сумма).
// musicStart — начало музыки, с (сетка достраивается назад до него, если Stemmer пропустил первые доли).
PhaseCheck checkPhase (const juce::AudioBuffer<float>& drums, double sampleRate, std::vector<double>& grid, std::vector<double>& bars, double musicStart = 0);

// Только измерить (без правки): для отчётов и проверок.
PhaseCheck measurePhase (const juce::AudioBuffer<float>& drums, double sampleRate, const std::vector<double>& grid, const std::vector<double>& bars);
} // namespace dzod::arranger
