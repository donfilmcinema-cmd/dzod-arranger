#pragma once
// DZOD Arranger — растяжка по карте времени фазовым вокодером Signalsmith Stretch (MIT).
// Заменяет WSOLA Stemmer в сведении: WSOLA на любой растяжке постоянно перескакивает по форме волны,
// и в местах скачков два сдвинутых куска звука складываются — металлический «стеклянный» призвук (приёмка автора 28.10).
#include <juce_audio_basics/juce_audio_basics.h>
#include <functional>

namespace dzod
{
namespace arranger
{
// srcSecOfFrame(m): секунда исходника для отсчёта выхода m * hop (hop — в отсчётах той же частоты sampleRate).
// Выход: wantSamples отсчётов, каналов столько же, сколько во входе. drums = true — короче окно (атаки чётче).
juce::AudioBuffer<float> stretchByMap (const juce::AudioBuffer<float>& in, double sampleRate, const std::function<double (int)>& srcSecOfFrame,
                                       int hop, int wantSamples, bool drums);
} // namespace arranger
} // namespace dzod
