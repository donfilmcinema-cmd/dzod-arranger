#include "Player.h"

namespace dzod::arranger
{
Player::Player()
{
    fm.registerBasicFormats();
    devError = dm.initialiseWithDefaultDevices (0, 2);
    deviceOk = devError.isEmpty() && dm.getCurrentAudioDevice() != nullptr;
    rs = std::make_unique<juce::ResamplingAudioSource> (&src, false, 2);
    sp.setSource (rs.get());
    dm.addAudioCallback (&sp);
}

Player::~Player()
{
    dm.removeAudioCallback (&sp);
    sp.setSource (nullptr);
}

void Player::prepareToPlay (int, double) {}

void Player::setSegments (std::vector<PlaySegment> s)
{
    std::map<juce::String, std::unique_ptr<juce::AudioFormatReader>> rd;
    double t = 0;
    for (auto& g : s)
    {
        t = juce::jmax (t, g.at + g.length);
        for (auto& f : g.files)
        {
            const auto key = f.getFullPathName();
            if (rd.count (key)) continue;
            {   // уже открытый файл не открываем заново
                const juce::ScopedLock sl (lock);
                auto it = readers.find (key);
                if (it != readers.end()) { rd[key] = std::move (it->second); readers.erase (it); continue; }
            }
            if (auto* r = fm.createReaderFor (f)) rd[key].reset (r);
        }
    }
    const juce::ScopedLock sl (lock);
    segs = std::move (s);
    readers = std::move (rd);
    total = t;
}

void Player::play (double from)
{
    if (auto* d = dm.getCurrentAudioDevice()) rs->setResamplingRatio (kRate / d->getCurrentSampleRate());
    pos = juce::jlimit (0.0, juce::jmax (0.0, total.load()), from);
    playing = true;
}

void Player::stop() { playing = false; }

void Player::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    info.clearActiveBufferRegion();
    if (! playing.load()) return;
    const juce::ScopedTryLock sl (lock);
    if (! sl.isLocked()) return;
    const int n = info.numSamples;
    const double t0 = pos.load(), t1 = t0 + n / kRate;
    if (tmp.getNumSamples() < n) tmp.setSize (2, n, false, false, true);
    for (auto& g : segs)
    {
        if (g.at >= t1 || g.at + g.length <= t0) continue;
        const double a = juce::jmax (t0, g.at), b = juce::jmin (t1, g.at + g.length);
        const int o0 = (int) std::lround ((a - t0) * kRate), len = juce::jmin (n - o0, (int) std::lround ((b - a) * kRate));
        if (len <= 0) continue;
        for (auto& f : g.files)
        {
            auto it = readers.find (f.getFullPathName());
            if (it == readers.end() || it->second == nullptr) continue;
            auto* r = it->second.get();
            const auto start = (juce::int64) std::llround ((g.src + (a - g.at)) * r->sampleRate);
            tmp.clear (0, len);
            r->read (&tmp, 0, len, start, true, true);
            if (r->numChannels == 1) tmp.copyFrom (1, 0, tmp, 0, 0, len);
            // 5 мс плавно на краях куска — без щелчков на стыках
            const double fadeSec = 0.005;
            for (int i = 0; i < len; ++i)
            {
                const double t = a + i / kRate;
                float w = g.gain;
                const double e1 = t - g.at, e2 = g.at + g.length - t;
                if (e1 < fadeSec) w *= (float) (e1 / fadeSec);
                if (e2 < fadeSec) w *= (float) (e2 / fadeSec);
                for (int c = 0; c < juce::jmin (2, info.buffer->getNumChannels()); ++c)
                    info.buffer->addSample (c, info.startSample + o0 + i, tmp.getSample (c, i) * w);
            }
        }
    }
    pos = t1;
    if (t1 >= total.load()) playing = false;
}
} // namespace dzod::arranger
