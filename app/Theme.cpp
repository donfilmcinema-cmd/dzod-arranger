#include "Theme.h"
#include "AppFonts.h"

namespace dzod::ui
{
namespace
{
struct Faces
{
    juce::Typeface::Ptr sans, sansBold, narrow, narrowBold;
    Faces()
    {
        sans       = juce::Typeface::createSystemTypefaceFor (fonts::PTSansRegular, fonts::PTSansRegularSize);
        sansBold   = juce::Typeface::createSystemTypefaceFor (fonts::PTSansBold, fonts::PTSansBoldSize);
        narrow     = juce::Typeface::createSystemTypefaceFor (fonts::PTSansNarrowRegular, fonts::PTSansNarrowRegularSize);
        narrowBold = juce::Typeface::createSystemTypefaceFor (fonts::PTSansNarrowBold, fonts::PTSansNarrowBoldSize);
    }
};
Faces& faces() { static Faces f; return f; }
} // namespace

juce::Font font (float h, bool bold)   { return juce::Font (juce::FontOptions (bold ? faces().sansBold : faces().sans).withHeight (h)); }
juce::Font narrow (float h, bool bold) { return juce::Font (juce::FontOptions (bold ? faces().narrowBold : faces().narrow).withHeight (h)); }
juce::Font mono (float h)              { return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), h, juce::Font::plain)); }

Look::Look()
{
    setColour (juce::ResizableWindow::backgroundColourId, col::bg);
    setColour (juce::TextButton::buttonColourId, col::panelHi);
    setColour (juce::TextButton::textColourOffId, col::text);
    setColour (juce::TextButton::textColourOnId, col::text);
    setColour (juce::Label::textColourId, col::text);
    setColour (juce::TextEditor::backgroundColourId, col::panelLo);
    setColour (juce::TextEditor::textColourId, col::text);
    setColour (juce::TextEditor::outlineColourId, col::line);
    setColour (juce::TextEditor::focusedOutlineColourId, col::blue);
    setColour (juce::TextEditor::highlightColourId, col::blue.withAlpha (0.35f));
    setColour (juce::CaretComponent::caretColourId, col::orange);
    setColour (juce::PopupMenu::backgroundColourId, col::panelHi);
    setColour (juce::PopupMenu::textColourId, col::text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, col::blue.withAlpha (0.3f));
    setColour (juce::PopupMenu::highlightedTextColourId, col::text);
    setColour (juce::ComboBox::backgroundColourId, col::panelLo);
    setColour (juce::ComboBox::outlineColourId, col::line);
    setColour (juce::ComboBox::textColourId, col::text);
    setColour (juce::ComboBox::arrowColourId, col::dim);
    setColour (juce::Slider::trackColourId, col::orange);
    setColour (juce::Slider::backgroundColourId, col::panelLo);
    setColour (juce::Slider::thumbColourId, col::text);
    setColour (juce::Slider::textBoxTextColourId, col::text);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::ToggleButton::textColourId, col::text);
    setColour (juce::ToggleButton::tickColourId, col::orange);
    setColour (juce::ScrollBar::thumbColourId, col::line.brighter (0.2f));
    setColour (juce::ProgressBar::backgroundColourId, col::panelLo);
    setColour (juce::ProgressBar::foregroundColourId, col::orange);
    setColour (juce::AlertWindow::backgroundColourId, col::panelHi);
    setColour (juce::AlertWindow::textColourId, col::text);
    setColour (juce::DocumentWindow::textColourId, col::text);
}

juce::Typeface::Ptr Look::getTypefaceForFont (const juce::Font& f)
{
    if (f.getTypefaceName() == juce::Font::getDefaultSansSerifFontName())
        return f.isBold() ? faces().sansBold : faces().sans;
    return LookAndFeel_V4::getTypefaceForFont (f);
}

void Look::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down)
{
    const auto kind = b.getProperties()["kind"].toString();
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const bool on = b.getToggleState();
    const float a = b.isEnabled() ? 1.0f : 0.4f;
    if (kind == "tab")
    {
        if (on) { g.setColour (col::blue); g.fillRect (r.removeFromBottom (2.0f)); }
        else if (over) { g.setColour (col::panelHi); g.fillRoundedRectangle (r, 4.0f); }
        return;
    }
    juce::Colour top, bottom, edge;
    if (kind == "primary")      { top = col::orangeHi; bottom = col::orange; edge = col::orangeDeep; }
    else if (kind == "play")    { top = col::playBlue.brighter (0.25f); bottom = col::playBlue; edge = col::blue.withAlpha (0.6f); }
    else if (kind == "chip")    { top = bottom = on ? col::panelHi : col::panelLo; edge = col::line; }
    else                        { top = col::panelHi.brighter (0.08f); bottom = col::panelHi; edge = col::line.brighter (0.1f); }
    if (on && kind != "chip" && kind != "primary" && kind != "play") { top = col::blue.withAlpha (0.45f); bottom = col::blue.withAlpha (0.3f); edge = col::blue; }
    if (over) { top = top.brighter (0.12f); bottom = bottom.brighter (0.12f); }
    if (down) { top = top.darker (0.15f); bottom = bottom.darker (0.15f); }
    g.setGradientFill (juce::ColourGradient (top.withMultipliedAlpha (a), r.getX(), r.getY(), bottom.withMultipliedAlpha (a), r.getX(), r.getBottom(), false));
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (edge.withMultipliedAlpha (a)); g.drawRoundedRectangle (r, 5.0f, 1.0f);
    if (kind == "play")
    {   // белый треугольник / квадрат «стоп»
        auto c = r.getCentre(); const float s = juce::jmin (r.getWidth(), r.getHeight()) * 0.3f;
        g.setColour (juce::Colours::white.withAlpha (a));
        if (on) g.fillRect (juce::Rectangle<float> (s * 1.4f, s * 1.4f).withCentre (c));
        else { juce::Path p; p.addTriangle (c.x - s * 0.7f, c.y - s, c.x - s * 0.7f, c.y + s, c.x + s, c.y); g.fillPath (p); }
    }
}

juce::Font Look::getTextButtonFont (juce::TextButton& b, int h)
{
    const auto kind = b.getProperties()["kind"].toString();
    if (kind == "tab") return font (14.0f, b.getToggleState());
    if (kind == "chip") return font (12.5f);
    return narrow (juce::jmin (15.0f, h * 0.5f), true);
}

void Look::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    const auto kind = b.getProperties()["kind"].toString();
    if (kind == "play") return;
    g.setFont (getTextButtonFont (b, b.getHeight()));
    juce::Colour c = kind == "primary" ? juce::Colour (0xff2a1a0c) : (kind == "tab" ? (b.getToggleState() ? col::text : col::dim) : col::text);
    if (kind == "chip" && b.getProperties()["struck"]) c = col::off;
    g.setColour (c.withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.45f));
    auto r = b.getLocalBounds().reduced (6, 0);
    g.drawFittedText (b.getButtonText(), r, juce::Justification::centred, 1);
    if (kind == "chip" && b.getProperties()["struck"])
    {
        g.drawLine ((float) r.getX(), r.getCentreY() + 0.5f, (float) r.getRight(), r.getCentreY() + 0.5f, 1.0f);
    }
}

void Look::drawTickBox (juce::Graphics& g, juce::Component&, float x, float y, float w, float h, bool ticked, bool enabled, bool, bool)
{
    juce::Rectangle<float> r (x, y, w, h);
    g.setColour (col::panelLo); g.fillRoundedRectangle (r, 3.0f);
    g.setColour (col::line.brighter (0.2f)); g.drawRoundedRectangle (r, 3.0f, 1.0f);
    if (ticked) { g.setColour (col::orange.withMultipliedAlpha (enabled ? 1.0f : 0.4f)); g.fillRoundedRectangle (r.reduced (3.5f), 2.0f); }
}

void Look::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float, juce::Slider::SliderStyle, juce::Slider& s)
{
    const float cy = y + h * 0.5f;
    juce::Rectangle<float> track ((float) x, cy - 2.0f, (float) w, 4.0f);
    g.setColour (col::panelLo); g.fillRoundedRectangle (track, 2.0f);
    g.setColour (s.findColour (juce::Slider::trackColourId)); g.fillRoundedRectangle (track.withRight (pos), 2.0f);
    g.setColour (col::text); g.fillEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre ({ pos, cy }));
}

void Look::fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor&)
{
    g.setColour (col::panelLo); g.fillRoundedRectangle (0, 0, (float) w, (float) h, 4.0f);
}

void Look::drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& e)
{
    g.setColour (e.hasKeyboardFocus (true) ? col::blue : col::line);
    g.drawRoundedRectangle (0.5f, 0.5f, w - 1.0f, h - 1.0f, 4.0f, 1.0f);
}

juce::Font Look::getPopupMenuFont() { return font (15.0f); }
juce::Font Look::getComboBoxFont (juce::ComboBox&) { return font (14.0f); }
juce::Font Look::getLabelFont (juce::Label& l) { return l.getFont(); }
} // namespace dzod::ui
