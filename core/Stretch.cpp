#include "Stretch.h"
#include <signalsmith-stretch/signalsmith-stretch.h>
#include <cmath>
#include <vector>

namespace dzod
{
namespace arranger
{
juce::AudioBuffer<float> stretchByMap (const juce::AudioBuffer<float>& in, double sr, const std::function<double (int)>& map, int hop, int want, bool drums)
{
    const int ch = juce::jmax (1, in.getNumChannels());
    signalsmith::stretch::SignalsmithStretch<float> st;
    if (drums) st.configure (ch, (int) (sr * 0.06), (int) (sr * 0.015));   // 60 мс окно — атаки не размазываются
    else       st.presetDefault (ch, (float) sr);                          // 120 мс окно, 30 мс шаг
    const int iL = st.inputLatency(), oL = st.outputLatency();

    // входная позиция для «времени обработки» t выхода (в отсчётах); за пределами карты — продолжение по краю
    auto srcAt = [&] (double tOut)
    {
        const double m = tOut / hop; const int m0 = (int) std::floor (m);
        const double a = map (m0), b = map (m0 + 1);
        return (a + (b - a) * (m - m0)) * sr;
    };

    // вход с запасом тишины по краям
    const double first = srcAt (0) - iL - 4 * hop, last = srcAt (want + oL + 4.0 * hop) + iL + 4 * hop;
    const juce::int64 base = (juce::int64) std::floor (juce::jmin (first, 0.0));
    const int len = (int) (std::ceil (juce::jmax (last, (double) in.getNumSamples())) - base) + 1;
    std::vector<std::vector<float>> x ((size_t) ch, std::vector<float> ((size_t) len, 0.0f));
    for (int c = 0; c < ch; ++c)
    {
        const float* s = in.getReadPointer (juce::jmin (c, in.getNumChannels() - 1));
        for (int i = 0; i < in.getNumSamples(); ++i) x[(size_t) c][(size_t) (i - base)] = s[i];
    }
    auto inPtr = [&] (juce::int64 pos) { std::vector<float*> p ((size_t) ch); for (int c = 0; c < ch; ++c) p[(size_t) c] = x[(size_t) c].data() + (pos - base); return p; };

    // старт: время обработки = srcAt(0); подаём iL отсчётов вперёд
    double readPos = srcAt (0);
    juce::int64 fed = (juce::int64) std::llround (readPos);
    {
        const double rate = (srcAt (hop) - srcAt (0)) / hop;
        st.seek (inPtr (fed), iL, rate);
        fed += iL;
    }
    // выход с предзвуком oL (выбрасываем)
    const int total = want + oL;
    std::vector<std::vector<float>> y ((size_t) ch, std::vector<float> ((size_t) total + hop, 0.0f));
    int made = 0;
    while (made < total)
    {
        const int n = juce::jmin (hop, total - made);
        // время обработки после seek = srcAt(0) и идёт вместе с выходом; выход отстаёт от него на oL (предзвук)
        const double target = srcAt (made + n) + iL;
        const juce::int64 need = juce::jmax ((juce::int64) 0, (juce::int64) std::llround (target) - fed);
        std::vector<float*> out ((size_t) ch); for (int c = 0; c < ch; ++c) out[(size_t) c] = y[(size_t) c].data() + made;
        st.process (inPtr (fed), (int) need, out, n);
        fed += need; made += n;
    }
    juce::AudioBuffer<float> r (ch, want);
    for (int c = 0; c < ch; ++c) r.copyFrom (c, 0, y[(size_t) c].data() + oL, want);
    return r;
}
} // namespace arranger
} // namespace dzod
