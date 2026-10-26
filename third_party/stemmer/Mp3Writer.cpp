#include "Mp3Writer.h"
#include <vector>

extern "C"
{
#include "layer3.h"
}

namespace dzod
{
bool writeMp3 (const juce::File& target, const juce::AudioBuffer<float>& audio, double sampleRate, int kbps, bool compensateDelay)
{
    const int channels = juce::jlimit (1, 2, audio.getNumChannels());
    const int rate = juce::roundToInt (sampleRate);
    if (audio.getNumChannels() < 1 || shine_check_config (rate, kbps) < 0) return false;

    shine_config_t config;
    shine_set_config_mpeg_defaults (&config.mpeg);
    config.wave.channels = channels == 2 ? PCM_STEREO : PCM_MONO;
    config.wave.samplerate = rate;
    config.mpeg.bitr = kbps;
    config.mpeg.mode = channels == 2 ? STEREO : MONO;
    shine_t encoder = shine_initialise (&config);
    if (encoder == nullptr) return false;

    target.deleteFile();
    juce::FileOutputStream stream (target);
    bool ok = stream.openedOk();

    const int block = shine_samples_per_pass (encoder);
    const int first = compensateDelay ? kMp3LeadSamples : 0;
    const int total = audio.getNumSamples() + 2 * block;           // запас тишины в конце, чтобы хвост не обрезался
    std::vector<juce::int16> pcm ((size_t) (block * channels));
    for (int pos = first; ok && pos < total; pos += block)
    {
        for (int i = 0; i < block; ++i)
            for (int c = 0; c < channels; ++c)
            {
                const int s = pos + i;
                const float v = s < audio.getNumSamples() ? audio.getSample (c, s) : 0.0f;
                pcm[(size_t) (i * channels + c)] = (juce::int16) juce::jlimit (-32768, 32767, juce::roundToInt (v * 32767.0f));
            }
        int written = 0;
        const unsigned char* data = shine_encode_buffer_interleaved (encoder, pcm.data(), &written);
        if (data != nullptr && written > 0) ok = stream.write (data, (size_t) written);
    }
    int written = 0;
    const unsigned char* data = shine_flush (encoder, &written);
    if (ok && data != nullptr && written > 0) ok = stream.write (data, (size_t) written);
    shine_close (encoder);
    stream.flush();
    return ok && stream.getStatus().wasOk();
}
} // namespace dzod
