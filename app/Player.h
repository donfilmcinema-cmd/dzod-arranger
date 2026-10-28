#pragma once
// DZOD Arranger — прослушивание стола и готовой песни: куски (отрезки файлов) один за другим, громкость на кусок.
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <map>
#include <memory>

namespace dzod::arranger
{
struct PlaySegment
{
    double at = 0, length = 0;           // место на шкале прослушивания, с
    double src = 0;                      // начало в файлах, с
    std::vector<juce::File> files;       // звучащие стемы куска
    float gain = 1.0f;                   // «Сравнить»: громкость куска приведена к общей
};

class Player : private juce::AudioSource
{
public:
    Player();
    ~Player() override;

    bool hasDevice() const { return deviceOk; }
    juce::String deviceError() const { return devError; }
    void setSegments (std::vector<PlaySegment>);      // с потока окна; открывает файлы
    void play (double fromSec);
    void stop();
    bool isPlaying() const { return playing.load(); }
    double position() const { return pos.load(); }
    double length() const { return total.load(); }
    juce::AudioDeviceManager& devices() { return dm; }

private:
    void prepareToPlay (int, double) override;
    void releaseResources() override {}
    void getNextAudioBlock (const juce::AudioSourceChannelInfo&) override;

    static constexpr double kRate = 48000.0;    // шкала прослушивания; поток к частоте карты — пересчётом
    juce::AudioDeviceManager dm;
    juce::AudioFormatManager fm;
    juce::AudioSourcePlayer sp;
    std::unique_ptr<juce::ResamplingAudioSource> rs;
    struct Src : juce::AudioSource
    {
        Player& p; explicit Src (Player& o) : p (o) {}
        void prepareToPlay (int, double) override {}
        void releaseResources() override {}
        void getNextAudioBlock (const juce::AudioSourceChannelInfo& i) override { p.getNextAudioBlock (i); }
    } src { *this };
    juce::CriticalSection lock;
    std::vector<PlaySegment> segs;
    std::map<juce::String, std::unique_ptr<juce::AudioFormatReader>> readers;
    juce::AudioBuffer<float> tmp;
    std::atomic<bool> playing { false };
    std::atomic<double> pos { 0 }, total { 0 };
    bool deviceOk = false; juce::String devError;
};
} // namespace dzod::arranger
