#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

namespace dzod
{
// Запись MP3 (MPEG-1 Layer III, постоянный битрейт) кодировщиком shine из папки ThirdParty.
// Частота 32000, 44100 или 48000 Гц, один или два канала.
//
// MP3 всегда добавляет тишину в начало: 528 отсчётов вносит кодировщик и ещё 529 любой декодер.
// Чтобы файл в проекте встал ровно по сетке, запись начинается с отсчёта kMp3LeadSamples исходного звука.
constexpr int kMp3LeadSamples = 1057;
bool writeMp3 (const juce::File& target, const juce::AudioBuffer<float>& audio, double sampleRate, int kbps, bool compensateDelay);
} // namespace dzod
