#pragma once
// DZOD Arranger — цвета и шрифты дизайн-системы DZOD (макет design, shared/design/arranger, v148).
#include <juce_gui_basics/juce_gui_basics.h>

namespace dzod::ui
{
namespace col
{
inline const juce::Colour bg        { 0xff141518 };
inline const juce::Colour panel     { 0xff1e2024 };
inline const juce::Colour panelHi   { 0xff26282c };
inline const juce::Colour panelLo   { 0xff191b1e };
inline const juce::Colour line      { 0xff32353b };
inline const juce::Colour text      { 0xffe6e8eb };
inline const juce::Colour dim       { 0xff8e949c };
inline const juce::Colour off       { 0xff5e636b };
inline const juce::Colour orange    { 0xffe3a35e };
inline const juce::Colour orangeHi  { 0xffffb27a };
inline const juce::Colour orangeDeep{ 0xff8b4e27 };
inline const juce::Colour blue      { 0xff4aa3ee };
inline const juce::Colour playBlue  { 0xff1f4f7a };    // кнопка Play — тёмно-голубая (автор 27.10)
inline const juce::Colour green     { 0xff8fd0a8 };
inline const juce::Colour yellow    { 0xffd6c26b };
inline const juce::Colour red       { 0xffe5484d };
inline const juce::Colour cursor    { 0xffff8a1f };
} // namespace col

inline juce::Colour roleColour (const juce::String& r)
{
    if (r == "drums")  return juce::Colour (0xff6fb3ee);
    if (r == "bass")   return juce::Colour (0xffab9fdc);
    if (r == "guitar") return juce::Colour (0xffe8ab70);
    if (r == "keys")   return juce::Colour (0xffe3d6b0);
    if (r == "synth")  return juce::Colour (0xff7fc4d8);
    if (r == "backs")  return juce::Colour (0xffc697ae);
    if (r == "vocals") return juce::Colour (0xffff8f3f);
    return juce::Colour (0xffa3a9b1);
}

inline juce::String roleLabel (const juce::String& r)
{
    if (r == "drums")  return "Drums";
    if (r == "bass")   return "Bass";
    if (r == "guitar") return "Guitar";
    if (r == "keys")   return "Keys";
    if (r == "synth")  return "Synth";
    if (r == "backs")  return "Backs";
    if (r == "vocals") return "Vocal";
    return "Other";
}

inline juce::Colour variantColour (int i)
{
    static const juce::uint32 c[] { 0xff4f8fc4, 0xffd08a4a, 0xff6fae86, 0xff9a84c8, 0xffc4708f, 0xff6fb7b0, 0xffb9a75a, 0xff8c94a8 };
    return juce::Colour (c[(size_t) ((i % 8) + 8) % 8]);
}

inline juce::Colour partColour (const juce::String& label)
{
    const auto l = label.toLowerCase();
    if (l.startsWith ("intro"))  return juce::Colour (0xff5d6170);
    if (l.startsWith ("verse"))  return juce::Colour (0xff3f6f8d);
    if (l.startsWith ("pre"))    return juce::Colour (0xff4d6f54);
    if (l.startsWith ("chorus")) return juce::Colour (0xff9a5a2a);
    if (l.startsWith ("bridge")) return juce::Colour (0xff6a4f8c);
    if (l.startsWith ("solo"))   return juce::Colour (0xff8c3f4a);
    if (l.startsWith ("break"))  return juce::Colour (0xff6fa6c8).darker (0.5f);
    if (l.startsWith ("outro"))  return juce::Colour (0xff5d6170).darker (0.2f);
    return juce::Colour (0xff4a4e58);
}

// Шрифты: PT Sans / PT Sans Narrow (shared/design/fonts), встроены в программу.
juce::Font font (float height, bool bold = false);
juce::Font narrow (float height, bool bold = true);
juce::Font mono (float height);

class Look : public juce::LookAndFeel_V4
{
public:
    Look();
    juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool over, bool down) override;
    void drawTickBox (juce::Graphics&, juce::Component&, float x, float y, float w, float h, bool ticked, bool enabled, bool over, bool down) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos, juce::Slider::SliderStyle, juce::Slider&) override;
    void fillTextEditorBackground (juce::Graphics&, int w, int h, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int w, int h, juce::TextEditor&) override;
    juce::Font getPopupMenuFont() override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getLabelFont (juce::Label&) override;
};

// Роли кнопок (свойство "kind" у TextButton): "primary" — оранжевая, "play" — тёмно-голубая, "tab", иначе — стеклянная.
inline void setKind (juce::Button& b, const juce::String& kind) { b.getProperties().set ("kind", kind); }
} // namespace dzod::ui
