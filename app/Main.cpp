// DZOD Arranger.exe — отдельная программа: окно сборки песни из версий Suno.
#include "ArrangerComponent.h"

class ArrangerApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "DZOD Arranger"; }
    const juce::String getApplicationVersion() override { return DZOD_ARRANGER_VERSION; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String& cmd) override
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&look);
        window = std::make_unique<Window>();
        // самопроверка: --demo <папка с вариантами> <папка снимков>
        auto args = juce::StringArray::fromTokens (cmd, true);
        if (args[0] == "--demo" && args.size() >= 3)
            if (auto* c = dynamic_cast<dzod::arranger::ArrangerComponent*> (window->getContentComponent()))
            {
                juce::Array<juce::File> vs;
                for (auto& f : juce::File (args[1].unquoted()).findChildFiles (juce::File::findDirectories, false)) vs.add (f);
                vs.sort();
                c->demo (vs, juce::File (args[2].unquoted()));
            }
    }
    void shutdown() override { window.reset(); juce::LookAndFeel::setDefaultLookAndFeel (nullptr); }
    void systemRequestedQuit() override { quit(); }

private:
    struct Window : juce::DocumentWindow
    {
        Window() : DocumentWindow ("DZOD Arranger " DZOD_ARRANGER_VERSION, dzod::ui::col::bg, allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new dzod::arranger::ArrangerComponent(), true);
            setResizable (true, true);
            setResizeLimits (1100, 720, 4000, 3000);
            if (auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            {
                auto area = d->userArea.reduced (20);
                setBounds (area.withSizeKeepingCentre (juce::jmin (area.getWidth(), 1600), juce::jmin (area.getHeight(), 980)));
            }
            setVisible (true);
            if (auto* c = getContentComponent()) c->grabKeyboardFocus();
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    dzod::ui::Look look;
    std::unique_ptr<Window> window;
};

START_JUCE_APPLICATION (ArrangerApp)
