#include "ArrangerComponent.h"

namespace dzod::arranger
{
using namespace dzod::ui;

static const char* kStepNames[] { "Song & variants", "Arrange", "Tempo", "Unify sound", "Mix into stems", "Save \xc2\xb7 Export" };
static const char* kNext[] { "GO TO ARRANGING", "NEXT: TEMPO", "NEXT: UNIFY SOUND", "NEXT: MIX INTO STEMS", "NEXT: SAVE \xc2\xb7 EXPORT", "" };
static const char* kHints[] {
    "Load the stems of every Suno version (files or a folder). The role of a stem comes from the file name - click the role to change it. Files can be dropped onto the window.",
    "Ruler - cursor  \xc2\xb7  pieces - select (Shift - several), drag to reorder  \xc2\xb7  S / SPLIT - cut at the cursor on the strong beat  \xc2\xb7  right click - remove an instrument from the piece, reference  \xc2\xb7  PART - label  \xc2\xb7  Space - Play  \xc2\xb7  Ctrl+Z / Ctrl+Y",
    "ANALYSE measures the tempo of every piece and suggests one for the song. ALIGN stretches every piece to it (pitch kept) and puts it on the bars.",
    "Every instrument is brought to the sound of its reference piece: loudness and a smooth EQ curve, not more than the limit.",
    "One line per instrument: all pieces of the instrument become one WAV (48 kHz, 24-bit) - Neuro Drums, Neuro Bass ...",
    "Save the new stems into a folder by a pattern, or export the whole song as one file." };

static juce::String timeText (double s)
{
    s = juce::jmax (0.0, s);
    return juce::String ((int) (s / 60)) + ":" + juce::String (std::fmod (s, 60.0), 1).paddedLeft ('0', 4);
}

static void styleLabel (juce::Label& l, float h, juce::Colour c = col::text, bool bold = false)
{
    l.setFont (bold ? narrow (h, true) : font (h)); l.setColour (juce::Label::textColourId, c);
    l.setJustificationType (juce::Justification::topLeft);
    l.setMinimumHorizontalScale (1.0f);
}

static void styleEditor (juce::TextEditor& e, float h = 15.0f)
{
    e.setFont (font (h)); e.setIndents (8, 6);
}

//==============================================================================
struct ArrangerComponent::JobThread : juce::Thread
{
    std::function<bool (Progress, juce::String&)> work;
    std::function<void (bool, const juce::String&)> done;
    ArrangerComponent& A;
    JobThread (ArrangerComponent& a) : juce::Thread ("DZOD Arranger job"), A (a) {}
    void run() override
    {
        juce::String err;
        const bool ok = work ([this] (float f, const juce::String& m) { A.jobFrac = f; const juce::ScopedLock sl (A.msgLock); A.jobMsg = m; }, err);
        juce::Component::SafePointer<ArrangerComponent> sp (&A);
        auto d = done;
        juce::MessageManager::callAsync ([sp, d, ok, err]
        {
            if (sp == nullptr) return;
            sp->job.reset();
            sp->updateFooter();
            if (d) d (ok, err);
            sp->changed();
        });
    }
};

//==============================================================================
// Карточка варианта (шаг 1): имя, LOAD FILES…, ×, файлы с ролями, строка BPM · тональность · LUFS · длина.
struct VariantCard : juce::Component
{
    ArrangerComponent& A; int index;
    juce::TextButton load { "LOAD FILES..." }, remove { "x" };
    juce::OwnedArray<juce::TextButton> chips;
    std::function<void()> onLoad, onRemove;
    VariantCard (ArrangerComponent& a, int i) : A (a), index (i)
    {
        addAndMakeVisible (load); addAndMakeVisible (remove);
        load.onClick = [this] { if (onLoad) onLoad(); };
        remove.onClick = [this] { if (onRemove) onRemove(); };
        auto& v = A.P.versions[(size_t) index];
        for (size_t s = 0; s < v.stems.size(); ++s)
        {
            auto* b = chips.add (new juce::TextButton (v.stems[s].file.getFileName() + "   " + roleLabel (v.stems[s].outRole)));
            setKind (*b, "chip"); addAndMakeVisible (b);
            b->onClick = [this, s, b]
            {
                juce::PopupMenu m; int id = 1;
                for (auto& r : outRoles()) m.addItem (id++, roleLabel (r), true, A.P.versions[(size_t) index].stems[s].outRole == r);
                juce::Component::SafePointer<VariantCard> self (this);
                m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (b), [self, s] (int r)
                {
                    if (self == nullptr || r == 0) return;
                    auto& A2 = self->A; const int vi = self->index;
                    const auto role = outRoles()[r - 1];
                    auto& st = A2.P.versions[(size_t) vi].stems[s];
                    st.role = role == "vocals" ? juce::String ("vocals") : role; st.outRole = role;
                    A2.P.versions[(size_t) vi].analysed = false;   // роли поменялись — анализ заново (барабаны, огибающие)
                    A2.changed();
                });
            };
        }
    }
    int heightFor (int w) const
    {
        int x = 16, rowsN = 1;
        for (auto* c : chips) { const int cw = juce::jmin (w - 32, 20 + (int) font (12.5f).getStringWidthFloat (c->getButtonText())); if (x + cw > w - 16) { x = 16; ++rowsN; } x += cw + 6; }
        return 58 + rowsN * 30 + 24;
    }
    void resized() override
    {
        load.setBounds (getWidth() - 170, 10, 120, 28); remove.setBounds (getWidth() - 42, 10, 28, 28);
        int x = 16, y = 50;
        for (auto* c : chips)
        {
            const int cw = juce::jmin (getWidth() - 32, 20 + (int) font (12.5f).getStringWidthFloat (c->getButtonText()));
            if (x + cw > getWidth() - 16) { x = 16; y += 30; }
            c->setBounds (x, y, cw, 24); x += cw + 6;
        }
    }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        const auto c = variantColour (index);
        g.setColour (col::panel.interpolatedWith (c, 0.06f)); g.fillRoundedRectangle (r, 6.0f);
        g.setColour (col::line); g.drawRoundedRectangle (r, 6.0f, 1.0f);
        g.setColour (c); g.fillRoundedRectangle (14, 14, 3, 20, 1.5f);
        const auto& v = A.P.versions[(size_t) index];
        g.setFont (font (18.0f, true)); g.setColour (c.brighter (0.3f));
        g.drawText ("Variant " + juce::String (index + 1), 24, 10, 140, 28, juce::Justification::centredLeft);
        g.setFont (font (13.5f)); g.setColour (col::dim);
        g.drawText (v.title, 130, 10, getWidth() - 320, 28, juce::Justification::centredLeft);
        juce::String info = juce::String ((int) v.stems.size()) + " stems";
        if (v.analysed) info << juce::String::fromUTF8 ("  \xc2\xb7  ") << juce::String (v.bpm, 1) << juce::String::fromUTF8 (" BPM  \xc2\xb7  ") << v.key.name() << juce::String::fromUTF8 ("  \xc2\xb7  ") << juce::String (v.mixLufs, 1) << juce::String::fromUTF8 (" LUFS  \xc2\xb7  ") << timeText (v.lengthSec).upToLastOccurrenceOf (".", false, false);
        else info << juce::String::fromUTF8 ("  \xc2\xb7  not analysed yet");
        bool drums = false; for (auto& s : v.stems) if (s.outRole == "drums") drums = true;
        if (! drums) info << juce::String::fromUTF8 ("  \xc2\xb7  NO DRUMS STEM - tempo cannot be found");
        g.setFont (mono (12.0f)); g.setColour (drums ? col::dim : col::red);
        g.drawText (juce::CharPointer_UTF8 (info.toRawUTF8()), 16, getHeight() - 26, getWidth() - 32, 18, juce::Justification::centredLeft);
    }
};

// Плитка варианта в левой колонке (шаг 2): инструменты словами, клик — выключить во всех кусках варианта.
struct VariantTile : juce::Component
{
    ArrangerComponent& A; int index;
    juce::OwnedArray<juce::TextButton> chips;
    VariantTile (ArrangerComponent& a, int i) : A (a), index (i)
    {
        auto& v = A.P.versions[(size_t) index];
        for (auto& r : outRoles())
        {
            if (! v.hasRole (r)) continue;
            auto* b = chips.add (new juce::TextButton (roleLabel (r))); setKind (*b, "chip");
            b->getProperties().set ("struck", v.off.count (r) > 0);
            addAndMakeVisible (b);
            b->onClick = [this, r] { A.edit ([&] { auto& o = A.P.versions[(size_t) index].off; if (o.count (r)) o.erase (r); else o.insert (r); }); };
        }
    }
    int heightFor() const { return 52 + ((chips.size() + 3) / 4) * 26; }
    void resized() override
    {
        int x = 10, y = 46; const int cw = (getWidth() - 20 - 9) / 4;
        for (int i = 0; i < chips.size(); ++i) { chips[i]->setBounds (x, y, cw, 22); x += cw + 3; if ((i + 1) % 4 == 0) { x = 10; y += 26; } }
    }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        const auto c = variantColour (index);
        g.setColour (col::panel.interpolatedWith (c, 0.08f)); g.fillRoundedRectangle (r, 5.0f);
        g.setColour (c.withAlpha (0.8f)); g.fillRect (r.getX(), r.getY() + 4, 3.0f, r.getHeight() - 8);
        const auto& v = A.P.versions[(size_t) index];
        g.setFont (font (14.5f, true)); g.setColour (c.brighter (0.3f));
        g.drawText (v.id + "  " + v.title, 12, 6, getWidth() - 20, 18, juce::Justification::centredLeft);
        g.setFont (mono (11.0f)); g.setColour (col::dim);
        g.drawText (v.analysed ? juce::String (v.bpm, 1) + juce::String::fromUTF8 (" BPM \xc2\xb7 ") + v.key.name() + juce::String::fromUTF8 (" \xc2\xb7 ") + juce::String (v.mixLufs, 1) + " LUFS" : juce::String ("-"),
                    12, 24, getWidth() - 20, 16, juce::Justification::centredLeft);
    }
};

// Строка таблицы Unify: инструмент · образец · сила · предел · результат.
struct UnifyRow : juce::Component
{
    ArrangerComponent& A; juce::String role;
    juce::ComboBox ref; juce::Slider strength, limit; juce::Label result;
    UnifyRow (ArrangerComponent& a, const juce::String& r) : A (a), role (r)
    {
        addAndMakeVisible (ref); addAndMakeVisible (strength); addAndMakeVisible (limit); addAndMakeVisible (result);
        ref.addItem ("- (as all)", 1);
        for (int i = 0; i < (int) A.P.pieces.size(); ++i)
        {
            const auto& pc = A.P.pieces[(size_t) i];
            juce::String label = juce::String (i + 1) + juce::String::fromUTF8 ("  \xc2\xb7  ") + pc.version + juce::String::fromUTF8 ("  \xc2\xb7  ") + timeText (pc.length()).upToLastOccurrenceOf (".", false, false);
            if (pc.part && A.P.parts.count (pc.part)) label << juce::String::fromUTF8 ("  \xc2\xb7  ") << A.P.parts.at (pc.part);
            ref.addItem (juce::CharPointer_UTF8 (label.toRawUTF8()), i + 2);
        }
        auto it = A.P.samples.find (role);
        ref.setSelectedId (it != A.P.samples.end() ? it->second + 2 : 1, juce::dontSendNotification);
        ref.onChange = [this] { const int id = ref.getSelectedId(); A.edit ([&] { if (id <= 1) A.P.samples.erase (role); else A.P.samples[role] = id - 2; }); };
        for (auto* s : { &strength, &limit }) { s->setSliderStyle (juce::Slider::LinearHorizontal); s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 20); }
        strength.setRange (0, 100, 1); strength.setTextValueSuffix (" %"); strength.setValue (A.P.strengthFor (role) * 100, juce::dontSendNotification);
        limit.setRange (0, 12, 0.5); limit.setTextValueSuffix (" dB"); limit.setValue (A.P.limitFor (role), juce::dontSendNotification);
        strength.onDragEnd = [this] { A.edit ([&] { A.P.strengths[role] = strength.getValue() / 100.0; }); };
        limit.onDragEnd = [this] { A.edit ([&] { A.P.limits[role] = limit.getValue(); }); };
        result.setFont (mono (11.5f)); result.setColour (juce::Label::textColourId, col::dim);
        juce::String res;
        if (A.render.matched)
            if (auto* m = A.render.report["matching"].getArray())
                for (auto& x : *m) if (x["role"].toString() == role)
                {
                    double mx = 0; if (auto* o = x["eqOctaves"].getArray()) for (auto& d : *o) mx = juce::jmax (mx, std::abs ((double) d));
                    res << x["version"].toString() << " " << ((double) x["gainDb"] >= 0 ? "+" : "") << juce::String ((double) x["gainDb"], 1) << " dB, EQ " << juce::String (mx, 1) << "   ";
                }
        result.setText (res.isEmpty() ? juce::String ("-") : res, juce::dontSendNotification);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (0, 4); r.removeFromLeft (110);
        ref.setBounds (r.removeFromLeft (250).reduced (4, 2)); r.removeFromLeft (10);
        strength.setBounds (r.removeFromLeft (200)); r.removeFromLeft (10);
        limit.setBounds (r.removeFromLeft (200)); r.removeFromLeft (14);
        result.setBounds (r);
    }
    void paint (juce::Graphics& g) override
    {
        g.setColour (col::line); g.drawHorizontalLine (getHeight() - 1, 0, (float) getWidth());
        g.setColour (roleColour (role)); g.fillEllipse (4, getHeight() / 2.0f - 4, 8, 8);
        g.setColour (col::text); g.setFont (font (15.0f, true)); g.drawText (roleLabel (role), 18, 0, 90, getHeight(), juce::Justification::centredLeft);
    }
};

//==============================================================================
ArrangerComponent::ArrangerComponent()
{
    setWantsKeyboardFocus (true);
    cacheDir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("DZOD").getChildFile ("Arranger").getChildFile ("cache");
    cacheDir.createDirectory();

    for (int i = 0; i < 6; ++i)
    {
        auto* b = tabs.add (new juce::TextButton (juce::String (juce::CharPointer_UTF8 (kStepNames[i]))));
        setKind (*b, "tab"); b->setClickingTogglesState (false); addAndMakeVisible (b);
        b->onClick = [this, i] { setStep (i); };
    }
    for (auto* b : { &openBtn, &saveBtn, &undoBtn, &redoBtn }) addAndMakeVisible (b);
    openBtn.onClick = [this] { openProject(); };
    saveBtn.onClick = [this] { saveProject (false); };
    undoBtn.onClick = [this] { undo(); };
    redoBtn.onClick = [this] { redo(); };
    setKind (nextBtn, "primary"); addAndMakeVisible (nextBtn);
    nextBtn.onClick = [this]
    {
        if (current == 0) goToArranging();
        else setStep (current + 1);
    };
    addAndMakeVisible (hint); hint.setFont (font (12.5f)); hint.setColour (juce::Label::textColourId, col::dim);
    addChildComponent (progress); addChildComponent (progressText); styleLabel (progressText, 13.0f, col::text);
    progressText.setJustificationType (juce::Justification::centredLeft);

    // 1 — песня и варианты
    addChildComponent (songPage);
    songPage.addAndMakeVisible (songLabel); songLabel.setText ("SONG NAME", juce::dontSendNotification); styleLabel (songLabel, 12.0f, col::dim, true);
    songPage.addAndMakeVisible (songEdit); styleEditor (songEdit, 22.0f); songEdit.setText (songName);
    songEdit.onTextChange = [this] { songName = songEdit.getText().trim(); updateSavePreview(); };
    songPage.addAndMakeVisible (songView); songView.setViewedComponent (&songList, false); songView.setScrollBarsShown (true, false);
    songList.addAndMakeVisible (addVariantBtn);
    addVariantBtn.onClick = [this] { loadFilesInto (-1); };

    // 2 — монтаж
    addChildComponent (arrangePage);
    table = std::make_unique<TableView> (*this, false);
    arrangePage.addAndMakeVisible (*table);
    setKind (playBtn, "play"); arrangePage.addAndMakeVisible (playBtn); playBtn.onClick = [this] { togglePlay(); };
    for (auto* b : { &timeLbl, &pointerBtn, &knifeBtn, &splitBtn, &flagBtn, &deleteBtn, &compareBtn, &snapBtn, &refBtn, &zoomOut, &zoomAll, &zoomIn })
        arrangePage.addAndMakeVisible (b);
    timeLbl.setEnabled (false);
    pointerBtn.setToggleState (true, juce::dontSendNotification);
    pointerBtn.onClick = [this] { table->knife = false; pointerBtn.setToggleState (true, juce::dontSendNotification); knifeBtn.setToggleState (false, juce::dontSendNotification); };
    knifeBtn.onClick = [this] { table->knife = true; knifeBtn.setToggleState (true, juce::dontSendNotification); pointerBtn.setToggleState (false, juce::dontSendNotification); };
    splitBtn.onClick = [this] { splitAtCursor(); };
    deleteBtn.onClick = [this] { deleteSelected(); };
    flagBtn.onClick = [this]
    {
        juce::PopupMenu m; const char* parts[] { "Intro", "Verse", "Pre-chorus", "Chorus", "Bridge", "Solo", "Break", "Outro" };
        for (int i = 0; i < 8; ++i) m.addItem (i + 1, parts[i], ! selected.empty());
        m.addSeparator(); m.addItem (100, "Remove label", ! selected.empty());
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&flagBtn), [this, parts] (int r)
        { if (r > 0) setPartForSelection (r == 100 ? juce::String() : juce::String (parts[r - 1])); });
    };
    compareBtn.setClickingTogglesState (true);
    compareBtn.onClick = [this] { compare = compareBtn.getToggleState(); if (player.isPlaying()) playFrom (player.position()); };
    snapBtn.setClickingTogglesState (true); snapBtn.setToggleState (true, juce::dontSendNotification);
    snapBtn.onClick = [this] { snapBars = snapBtn.getToggleState(); };
    refBtn.onClick = [this]
    {
        if (selected.empty()) { notify ("Reference", "Select the piece whose sound all the others should match."); return; }
        const int i = P.indexOf (*selected.begin());
        edit ([&] { P.sample = i; P.samples.clear(); });
    };
    zoomOut.onClick = [this] { table->zoomBy (0.7f, (float) table->getWidth() / 2); };
    zoomIn.onClick = [this] { table->zoomBy (1.4f, (float) table->getWidth() / 2); };
    zoomAll.onClick = [this] { table->zoomAll(); };
    arrangePage.addAndMakeVisible (variantView); variantView.setViewedComponent (&variantCol, false); variantView.setScrollBarsShown (true, false);

    // 3–6: песня снизу
    songTable = std::make_unique<TableView> (*this, true);
    addChildComponent (*songTable);
    setKind (songPlay, "play"); addChildComponent (songPlay);
    songPlay.onClick = [this] { togglePlay(); };
    addChildComponent (songTime); styleLabel (songTime, 15.0f); songTime.setFont (mono (15.0f)); songTime.setJustificationType (juce::Justification::centredLeft);

    // Tempo
    addChildComponent (tempoPage);
    for (auto* c : std::initializer_list<juce::Component*> { &analyseBtn, &alignBtn, &tempoList, &suggestion, &bpmEdit, &bpmCaption, &alignResult }) tempoPage.addAndMakeVisible (c);
    styleLabel (tempoList, 13.0f); tempoList.setFont (mono (12.5f));
    styleLabel (suggestion, 15.0f, col::text); styleLabel (alignResult, 13.5f, col::green);
    styleLabel (bpmCaption, 11.0f, col::dim, true); bpmCaption.setText ("BPM", juce::dontSendNotification); bpmCaption.setJustificationType (juce::Justification::centred);
    bpmEdit.setFont (narrow (40.0f, true)); bpmEdit.setJustification (juce::Justification::centred); bpmEdit.setInputRestrictions (6, "0123456789.");
    bpmEdit.onTextChange = [this] { const double b = bpmEdit.getText().getDoubleValue(); if (b > 30 && b < 300) P.songBpm = b; };
    analyseBtn.onClick = [this]
    {
        analyseAll ([this]
        {
            tempo = Engine::analyseTempo (P, P.songBpm); tempoDone = true;
            if (P.songBpm <= 0 && tempo.proposed > 0) P.songBpm = tempo.proposed;
            bpmEdit.setText (juce::String (P.songBpm, 1), juce::dontSendNotification);
            updateTempoPage();
        });
    };
    alignBtn.onClick = [this] { startRender ("align"); };

    // Unify
    addChildComponent (unifyPage);
    setKind (unifyBtn, "primary");
    for (auto* c : std::initializer_list<juce::Component*> { &unifyBtn, &toneTgl, &loudTgl, &unifyRows, &unifyNote }) unifyPage.addAndMakeVisible (c);
    toneTgl.setToggleState (true, juce::dontSendNotification); loudTgl.setToggleState (true, juce::dontSendNotification);
    toneTgl.onClick = [this] { P.matchEq = toneTgl.getToggleState(); };
    loudTgl.onClick = [this] { P.matchLoudness = loudTgl.getToggleState(); };
    unifyBtn.onClick = [this] { startRender ("unify"); };
    styleLabel (unifyNote, 13.0f, col::dim);

    // Mix
    addChildComponent (mixPage);
    setKind (mixBtn, "primary");
    for (auto* c : std::initializer_list<juce::Component*> { &mixBtn, &mixCards, &mixNote }) mixPage.addAndMakeVisible (c);
    mixBtn.onClick = [this] { startRender ("mix"); };
    styleLabel (mixNote, 13.5f, col::dim);

    // Save · Export
    addChildComponent (savePage);
    for (auto* c : std::initializer_list<juce::Component*> { &folderEdit, &patternEdit, &exportFileEdit, &folderBrowse, &exportBrowse, &saveStemsBtn, &exportBtn, &mp3Btn, &wav24Btn, &wav16Btn, &sendBtn, &treeLbl, &saveNote, &spaceNote })
        savePage.addAndMakeVisible (c);
    for (auto* e : { &folderEdit, &patternEdit, &exportFileEdit }) styleEditor (*e, 14.0f);
    folderEdit.setText (juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("DZOD Songs").getFullPathName());
    patternEdit.setText (juce::String (juce::CharPointer_UTF8 ("{song}/Stems/{song} \xe2\x80\x94 Neuro {instrument}.wav")));
    folderEdit.onTextChange = patternEdit.onTextChange = [this] { updateSavePreview(); };
    setKind (saveStemsBtn, "primary"); setKind (exportBtn, "primary");
    styleLabel (treeLbl, 12.5f, col::text); treeLbl.setFont (mono (12.5f));
    styleLabel (saveNote, 13.0f, col::green); styleLabel (spaceNote, 13.0f, col::dim);
    spaceNote.setText ("Put the new stems into the Block Stems of the Space (the Space tempo = the song tempo, from bar 1). Works when DZOD Arranger is opened from DZOD Studio.", juce::dontSendNotification);
    sendBtn.setEnabled (false);
    for (auto* b : { &mp3Btn, &wav24Btn, &wav16Btn }) b->setClickingTogglesState (false);
    auto fmtSel = [this] (const juce::String& f)
    {
        exportFormat = f;
        mp3Btn.setToggleState (f == "mp3", juce::dontSendNotification); wav24Btn.setToggleState (f == "wav24", juce::dontSendNotification); wav16Btn.setToggleState (f == "wav16", juce::dontSendNotification);
        auto file = juce::File (exportFileEdit.getText());
        if (exportFileEdit.getText().isNotEmpty()) exportFileEdit.setText (file.withFileExtension (f == "mp3" ? ".mp3" : ".wav").getFullPathName());
    };
    mp3Btn.onClick = [fmtSel] { fmtSel ("mp3"); }; wav24Btn.onClick = [fmtSel] { fmtSel ("wav24"); }; wav16Btn.onClick = [fmtSel] { fmtSel ("wav16"); };
    fmtSel ("mp3");
    folderBrowse.onClick = [this]
    {
        choose ("Folder for the new stems", "", juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                [this] (const juce::Array<juce::File>& r) { if (! r.isEmpty()) folderEdit.setText (r[0].getFullPathName()); });
    };
    exportBrowse.onClick = [this]
    {
        choose ("Export the song", exportFormat == "mp3" ? "*.mp3" : "*.wav", juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                [this] (const juce::Array<juce::File>& r) { if (! r.isEmpty()) exportFileEdit.setText (r[0].withFileExtension (exportFormat == "mp3" ? ".mp3" : ".wav").getFullPathName()); });
    };
    saveStemsBtn.onClick = [this] { saveStems(); };
    exportBtn.onClick = [this] { exportSong(); };

    setSize (1440, 900);
    rebuildSongPage();
    setStep (0);
    startTimerHz (30);
    if (! player.hasDevice()) DBG ("no audio device: " + player.deviceError());
}

ArrangerComponent::~ArrangerComponent()
{
    stopTimer();
    player.stop();
    if (job != nullptr) job->stopThread (60000);
}

//==============================================================================
void ArrangerComponent::paint (juce::Graphics& g)
{
    g.fillAll (col::bg);
    // шапка
    auto top = getLocalBounds().removeFromTop (52);
    g.setColour (col::panelLo); g.fillRect (top);
    g.setColour (col::line); g.drawHorizontalLine (52, 0, (float) getWidth());
    g.setFont (narrow (24.0f, true));
    g.setColour (col::orange); g.drawText ("DZOD", 18, 0, 70, 52, juce::Justification::centredLeft);
    g.setColour (col::text); g.drawText ("Arranger", 76, 0, 120, 52, juce::Justification::centredLeft);
    g.setFont (font (16.0f)); g.setColour (col::dim);
    g.drawText (juce::String (juce::CharPointer_UTF8 ("\xc2\xab")) + songName + juce::String (juce::CharPointer_UTF8 ("\xc2\xbb")), 180, 0, 300, 52, juce::Justification::centredLeft);
    // вкладки: выполненные шаги — зелёная галка
    for (int i = 0; i < tabs.size(); ++i)
    {
        auto b = tabs[i]->getBounds();
        const bool done = (i == 0 && ! P.pieces.empty()) || (i == 1 && ! P.pieces.empty() && current > 1) || (i == 2 && render.valid())
                          || (i == 3 && render.valid() && render.matched) || (i == 4 && render.valid() && render.folder.getFileName() == "mix");
        g.setColour (done ? col::green : (i == current ? col::blue : col::panelHi));
        g.fillEllipse ((float) b.getX() - 22, b.getCentreY() - 9.0f, 18, 18);
        g.setColour (done || i == current ? col::bg : col::dim); g.setFont (font (11.5f, true));
        g.drawText (done ? juce::String (juce::CharPointer_UTF8 ("\xe2\x9c\x93")) : juce::String (i + 1), b.getX() - 22, b.getCentreY() - 9, 18, 18, juce::Justification::centred);
    }
    g.setColour (col::line); g.drawHorizontalLine (96, 0, (float) getWidth());
    // подвал
    g.setColour (col::panelLo); g.fillRect (0, getHeight() - 56, getWidth(), 56);
    g.setColour (col::line); g.drawHorizontalLine (getHeight() - 56, 0, (float) getWidth());

    // карточки шагов 3–6
    auto card = [&] (juce::Component& page, juce::Rectangle<int> r, const juce::String& title, const juce::String& sub)
    {
        if (! page.isVisible()) return;
        r = r + page.getPosition();
        g.setColour (col::panel); g.fillRoundedRectangle (r.toFloat(), 6.0f);
        g.setColour (col::line); g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 6.0f, 1.0f);
        g.setColour (col::orange); g.setFont (narrow (13.0f, true)); g.drawText (title, r.getX() + 14, r.getY() + 10, r.getWidth() - 28, 16, juce::Justification::left);
        g.setColour (col::dim); g.setFont (font (13.0f)); g.drawFittedText (sub, r.getX() + 14, r.getY() + 30, r.getWidth() - 28, 34, juce::Justification::topLeft, 2);
    };
    const int w3 = (tempoPage.getWidth() - 20) / 3;
    card (tempoPage, { 0, 0, w3, 220 }, juce::String::fromUTF8 ("1 \xc2\xb7 TEMPO ANALYZER"), "Measures the tempo of every piece and suggests the best one for the song.");
    card (tempoPage, { w3 + 10, 0, w3, 220 }, juce::String::fromUTF8 ("2 \xc2\xb7 SONG TEMPO"), "Take the suggestion or type your own.");
    card (tempoPage, { 2 * w3 + 20, 0, w3, 220 }, juce::String::fromUTF8 ("3 \xc2\xb7 ALIGN"), "Every piece is stretched to this tempo (pitch kept) and put on the bars.");
    card (unifyPage, unifyPage.getLocalBounds(), "UNIFY SOUND", "Reference piece, strength and limit per instrument. The result: gain and EQ of every variant.");
    card (mixPage, mixPage.getLocalBounds(), "MIX INTO STEMS", "All pieces of an instrument become one line: Neuro Drums, Neuro Bass ... (WAV 48 kHz, 24-bit, from bar 1).");
    const int ws = (savePage.getWidth() - 20) / 3;
    card (savePage, { 0, 0, ws, savePage.getHeight() }, "SAVE NEW STEMS", "");
    card (savePage, { ws + 10, 0, ws, savePage.getHeight() }, "EXPORT THE SONG", "");
    card (savePage, { 2 * ws + 20, 0, ws, savePage.getHeight() }, "TO THE SPACE", "");
    if (current >= 2)
    {
        g.setColour (col::orange); g.setFont (narrow (13.0f, true));
        g.drawText (render.valid() && render.revision != revision ? "THE SONG - OUT OF DATE (press ALIGN again)" : "THE SONG - ON THE GRID", songPlay.getX() - 300, songPlay.getY(), 290, songPlay.getHeight(), juce::Justification::centredRight);
    }
}

void ArrangerComponent::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (52);
    redoBtn.setBounds (top.removeFromRight (70).reduced (4, 12)); undoBtn.setBounds (top.removeFromRight (70).reduced (4, 12));
    top.removeFromRight (16);
    saveBtn.setBounds (top.removeFromRight (70).reduced (4, 12)); openBtn.setBounds (top.removeFromRight (70).reduced (4, 12));
    auto tabRow = r.removeFromTop (44).reduced (14, 0);
    for (auto* t : tabs) { const int w = 20 + (int) font (14.0f, true).getStringWidthFloat (t->getButtonText()); tabRow.removeFromLeft (30); t->setBounds (tabRow.removeFromLeft (w).reduced (0, 4)); }
    auto foot = r.removeFromBottom (56).reduced (14, 10);
    nextBtn.setBounds (foot.removeFromRight (240));
    foot.removeFromRight (16);
    progress.setBounds (foot.removeFromRight (260).reduced (0, 6));
    progressText.setBounds (foot.removeFromRight (360));
    hint.setBounds (foot);
    r.reduce (14, 12);
    for (auto* p : { &songPage, &arrangePage, &tempoPage, &unifyPage, &mixPage, &savePage }) p->setBounds (r);

    // 1
    songPage.setBounds (r.withSizeKeepingCentre (juce::jmin (r.getWidth(), 1100), r.getHeight()));
    songLabel.setBounds (0, 0, 200, 18);
    songEdit.setBounds (0, 20, songPage.getWidth(), 44);
    songView.setBounds (0, 76, songPage.getWidth(), songPage.getHeight() - 76);
    rebuildSongPage();

    // 2
    auto a = arrangePage.getLocalBounds();
    auto bar = a.removeFromTop (40);
    playBtn.setBounds (bar.removeFromLeft (44).reduced (2)); bar.removeFromLeft (6);
    timeLbl.setBounds (bar.removeFromLeft (100).reduced (2)); bar.removeFromLeft (14);
    for (auto* b : { &pointerBtn, &knifeBtn }) b->setBounds (bar.removeFromLeft (84).reduced (2));
    bar.removeFromLeft (14);
    for (auto* b : { &splitBtn, &flagBtn, &deleteBtn }) b->setBounds (bar.removeFromLeft (92).reduced (2));
    bar.removeFromLeft (14);
    for (auto* b : { &compareBtn, &snapBtn, &refBtn }) b->setBounds (bar.removeFromLeft (110).reduced (2));
    a.removeFromTop (8);
    variantView.setBounds (a.removeFromLeft (270)); a.removeFromLeft (10);
    table->setBounds (a);
    zoomIn.setBounds (a.getRight() - 40, a.getBottom() - 36, 32, 28);
    zoomAll.setBounds (a.getRight() - 96, a.getBottom() - 36, 52, 28);
    zoomOut.setBounds (a.getRight() - 132, a.getBottom() - 36, 32, 28);
    rebuildVariantColumn();

    // 3–6: карточки сверху, песня снизу
    const int cardsH = 230;
    for (auto* p : { &tempoPage, &unifyPage, &mixPage, &savePage }) p->setBounds (r.withHeight (cardsH));
    unifyPage.setBounds (r.withHeight (juce::jmin (r.getHeight() - 220, 124 + 38 * juce::jmax (1, P.rolesInSong().size()))));
    layoutSong();

    const int w3 = (tempoPage.getWidth() - 20) / 3;
    analyseBtn.setBounds (14, 66, 130, 34);
    tempoList.setBounds (14, 106, w3 - 28, 76);
    suggestion.setBounds (14, 180, w3 - 28, 34);
    bpmCaption.setBounds (w3 + 24, 66, 160, 14);
    bpmEdit.setBounds (w3 + 24, 82, 160, 64);
    alignBtn.setBounds (2 * w3 + 34, 66, 130, 34);
    alignResult.setBounds (2 * w3 + 34, 108, w3 - 28, 100);

    unifyBtn.setBounds (unifyPage.getWidth() - 214, 12, 200, 40);
    toneTgl.setBounds (unifyPage.getWidth() - 420, 18, 100, 26); loudTgl.setBounds (unifyPage.getWidth() - 314, 18, 96, 26);
    unifyRows.setBounds (14, 70, unifyPage.getWidth() - 28, unifyPage.getHeight() - 104);
    unifyNote.setBounds (14, unifyPage.getHeight() - 30, unifyPage.getWidth() - 28, 22);

    mixBtn.setBounds (mixPage.getWidth() - 214, 12, 200, 40);
    mixCards.setBounds (14, 70, mixPage.getWidth() - 28, 110);
    mixNote.setBounds (14, 186, mixPage.getWidth() - 28, 36);

    const int ws = (savePage.getWidth() - 20) / 3, sh = savePage.getHeight();
    folderEdit.setBounds (14, 34, ws - 140, 30); folderBrowse.setBounds (ws - 120, 34, 106, 30);
    patternEdit.setBounds (14, 70, ws - 28, 30);
    treeLbl.setBounds (14, 106, ws - 28, sh - 160);
    saveStemsBtn.setBounds (14, sh - 48, 190, 36); saveNote.setBounds (214, sh - 48, ws - 228, 36);
    const int x2 = ws + 24;
    mp3Btn.setBounds (x2, 34, (ws - 28) / 3, 30); wav24Btn.setBounds (x2 + (ws - 28) / 3, 34, (ws - 28) / 3, 30); wav16Btn.setBounds (x2 + 2 * (ws - 28) / 3, 34, (ws - 28) / 3, 30);
    exportFileEdit.setBounds (x2, 74, ws - 154, 30); exportBrowse.setBounds (x2 + ws - 134, 74, 106, 30);
    exportBtn.setBounds (x2, 114, 190, 36);
    const int x3 = 2 * ws + 34;
    spaceNote.setBounds (x3, 34, ws - 28, 60); sendBtn.setBounds (x3, 100, 170, 34);
    rebuildUnifyPage(); rebuildMixPage();
}

//==============================================================================
void ArrangerComponent::layoutSong()
{
    auto r = getLocalBounds().withTrimmedTop (96).withTrimmedBottom (56).reduced (14, 12);
    const int pageH = current == 3 ? unifyPage.getHeight() : 230;
    auto lower = r.withTrimmedTop (pageH + 12);
    songPlay.setBounds (lower.getX() + 300, lower.getY(), 44, 34);
    songTime.setBounds (lower.getX() + 352, lower.getY(), 120, 34);
    songTable->setBounds (lower.withTrimmedTop (42));
}

void ArrangerComponent::setStep (int s)
{
    s = juce::jlimit (0, 5, s);
    if (s >= 1 && P.pieces.empty())
    {
        if (P.versions.empty()) { notify ("Arranger", "Load at least one variant first."); s = 0; }
        else { goToArranging(); return; }
    }
    if (player.isPlaying() && (s >= 2) != (current >= 2)) player.stop();
    current = s;
    for (int i = 0; i < tabs.size(); ++i) tabs[i]->setToggleState (i == s, juce::dontSendNotification);
    songPage.setVisible (s == 0); arrangePage.setVisible (s == 1);
    tempoPage.setVisible (s == 2); unifyPage.setVisible (s == 3); mixPage.setVisible (s == 4); savePage.setVisible (s == 5);
    songTable->setVisible (s >= 2); songPlay.setVisible (s >= 2); songTime.setVisible (s >= 2);
    if (s >= 2) songTable->zoomAll();
    if (s == 2) { bpmEdit.setText (P.songBpm > 0 ? juce::String (P.songBpm, 1) : juce::String(), juce::dontSendNotification); updateTempoPage(); }
    if (s == 3) { unifyPage.setBounds (unifyPage.getBounds().withHeight (124 + 38 * juce::jmax (1, P.rolesInSong().size()))); rebuildUnifyPage(); }
    layoutSong();
    if (s == 4) rebuildMixPage();
    if (s == 5) { if (exportFileEdit.getText().isEmpty()) exportFileEdit.setText (juce::File (folderEdit.getText()).getChildFile (songName).getChildFile (songName + ".mp3").getFullPathName()); updateSavePreview(); }
    if (s == 1) { table->zoomAll(); rebuildVariantColumn(); }
    hint.setText (juce::CharPointer_UTF8 (kHints[s]), juce::dontSendNotification);
    nextBtn.setButtonText (juce::CharPointer_UTF8 (kNext[s])); nextBtn.setVisible (s < 5);
    updateFooter();
    repaint();
    grabKeyboardFocus();
}

void ArrangerComponent::changed()
{
    rebuildVariantColumn();
    if (current == 0) rebuildSongPage();
    if (current == 3) rebuildUnifyPage();
    if (current == 4) rebuildMixPage();
    updateFooter();
    table->repaint(); songTable->repaint(); repaint();
}

void ArrangerComponent::edit (const std::function<void()>& f)
{
    if (busy()) return;
    hist.push (P);
    f();
    ++revision;
    if (player.isPlaying() && current == 1) playFrom (player.position());
    changed();
}

void ArrangerComponent::undo() { if (! busy() && hist.undo (P)) { ++revision; selected.clear(); changed(); } }
void ArrangerComponent::redo() { if (! busy() && hist.redo (P)) { ++revision; selected.clear(); changed(); } }

void ArrangerComponent::updateFooter()
{
    undoBtn.setEnabled (hist.canUndo() && ! busy()); redoBtn.setEnabled (hist.canRedo() && ! busy());
    const bool b = busy();
    progress.setVisible (b); progressText.setVisible (b);
    for (auto* c : std::initializer_list<juce::Component*> { &songPage, &arrangePage, &tempoPage, &unifyPage, &mixPage, &savePage, &nextBtn, &openBtn, &saveBtn }) c->setEnabled (! b);
    for (auto* t : tabs) t->setEnabled (! b);
}

void ArrangerComponent::timerCallback()
{
    if (busy())
    {
        progressValue = jobFrac.load();
        const juce::ScopedLock sl (msgLock); progressText.setText (jobMsg, juce::dontSendNotification);
    }
    const bool playing = player.isPlaying();
    playBtn.setToggleState (playing && current == 1, juce::dontSendNotification);
    songPlay.setToggleState (playing && current >= 2, juce::dontSendNotification);
    const double pos = playing ? player.position() : cursor;
    timeLbl.setButtonText (timeText (current == 1 ? pos : cursor));
    songTime.setText (timeText (playing && current >= 2 ? player.position() : 0.0), juce::dontSendNotification);
    if (playing) { if (current == 1) table->repaint(); else songTable->repaint(); }
}

bool ArrangerComponent::keyPressed (const juce::KeyPress& k)
{
    if (busy()) return false;
    const auto m = k.getModifiers();
    if (k == juce::KeyPress::spaceKey) { togglePlay(); return true; }
    if ((m.isCommandDown() || m.isCtrlDown()) && k.getKeyCode() == 'Z') { if (m.isShiftDown()) redo(); else undo(); return true; }
    if ((m.isCommandDown() || m.isCtrlDown()) && k.getKeyCode() == 'Y') { redo(); return true; }
    if ((m.isCommandDown() || m.isCtrlDown()) && k.getKeyCode() == 'S') { saveProject (m.isShiftDown()); return true; }
    if (current == 1)
    {
        if (k.getKeyCode() == 'S' && ! m.isAnyModifierKeyDown()) { splitAtCursor(); return true; }
        if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) { deleteSelected(); return true; }
    }
    return false;
}

//==============================================================================
juce::Array<juce::File> ArrangerComponent::stemsOf (const Piece& pc) const
{
    juce::Array<juce::File> r;
    if (auto* v = P.findVersion (pc.version))
        for (auto& s : v->stems) if (! s.silent && P.isOn (pc, s.outRole)) r.add (s.file);
    return r;
}

void ArrangerComponent::playFrom (double sec)
{
    std::vector<PlaySegment> segs;
    if (current >= 2)
    {
        if (! render.valid()) { notify ("Arranger", "Nothing to play yet: press ALIGN in step 3."); return; }
        PlaySegment s; s.at = 0; s.length = render.length; s.src = 0;
        for (auto& [r, f] : render.stems) s.files.push_back (f);
        segs.push_back (s);
    }
    else
    {
        // «Сравнить»: каждый кусок на громкости самой тихой версии стола (без подъёма — без перегруза)
        double ref = 0; bool any = false;
        for (auto& pc : P.pieces) if (auto* v = P.findVersion (pc.version)) if (v->analysed) { ref = any ? juce::jmin (ref, v->mixLufs) : v->mixLufs; any = true; }
        double at = 0;
        for (auto& pc : P.pieces)
        {
            PlaySegment s; s.at = at; s.length = pc.length(); s.src = pc.from;
            for (auto& f : stemsOf (pc)) s.files.push_back (f);
            if (compare && any) if (auto* v = P.findVersion (pc.version)) if (v->analysed) s.gain = (float) juce::Decibels::decibelsToGain (ref - v->mixLufs);
            segs.push_back (s); at += pc.length();
        }
    }
    player.setSegments (std::move (segs));
    if (! player.hasDevice()) { notify ("Audio", "No audio output: " + player.deviceError()); return; }
    player.play (sec);
}

void ArrangerComponent::togglePlay()
{
    if (player.isPlaying()) { player.stop(); if (current == 1) cursor = juce::jmin (cursor, P.tableLength()); return; }
    playFrom (current == 1 ? cursor : 0.0);
}

double ArrangerComponent::snapTable (double tt) const
{
    const int pi = P.pieceAt (tt);
    if (pi < 0 || ! snapBars) return tt;
    const auto& pc = P.pieces[(size_t) pi];
    const auto* v = P.findVersion (pc.version);
    if (v == nullptr || v->bars.empty()) return tt;
    const double start = P.tableStartOf (pi), src = pc.from + (tt - start);
    return start + (v->snapToBar (src) - pc.from);
}

void ArrangerComponent::splitAtCursor()
{
    const double cut = snapTable (cursor);
    std::vector<int> only; for (int id : selected) { const int i = P.indexOf (id); if (i >= 0) only.push_back (i); }
    const int i = P.pieceAt (cut);
    if (i < 0) return;
    if (! only.empty() && std::find (only.begin(), only.end(), i) == only.end()) only.clear();   // курсор не над выделенным — режем кусок под курсором
    edit ([&] { P.splitAll (cut, only); });
    cursor = cut;
}

void ArrangerComponent::deleteSelected()
{
    if (selected.empty()) return;
    edit ([&] { for (int id : selected) { const int i = P.indexOf (id); if (i >= 0) P.remove (i); } });
    selected.clear(); cursor = juce::jmin (cursor, P.tableLength()); changed();
}

void ArrangerComponent::setPartForSelection (const juce::String& label)
{
    std::vector<int> idx; for (int id : selected) { const int i = P.indexOf (id); if (i >= 0) idx.push_back (i); }
    if (idx.empty()) return;
    edit ([&] { P.setPart (idx, label); });
}

void ArrangerComponent::notify (const juce::String& title, const juce::String& text)
{
    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, title, text);
}

//==============================================================================
void ArrangerComponent::runJob (const juce::String& title, std::function<bool (Progress, juce::String&)> work, std::function<void (bool, const juce::String&)> done)
{
    if (busy()) return;
    player.stop();
    jobFrac = 0; { const juce::ScopedLock sl (msgLock); jobMsg = title; }
    job = std::make_unique<JobThread> (*this);
    job->work = std::move (work); job->done = std::move (done);
    updateFooter();
    job->startThread();
}

void ArrangerComponent::analyseAll (std::function<void()> then)
{
    bool need = false; for (auto& v : P.versions) if (! v.analysed) need = true;
    if (! need) { if (then) then(); return; }
    auto work = std::make_shared<std::vector<Version>> (P.versions);
    const auto cache = cacheDir;
    runJob ("Analysing the variants", [work, cache] (Progress pr, juce::String& err)
    {
        for (size_t i = 0; i < work->size(); ++i)
        {
            auto& v = (*work)[i];
            if (v.analysed) continue;
            const float a = (float) i / work->size(), b = (float) (i + 1) / work->size();
            if (! Engine::analyseVersion (v, cache, [&] (float f, const juce::String& m) { pr (a + (b - a) * f, m); }, err)) { err = v.id + ": " + err; return false; }
        }
        return true;
    }, [this, work, then] (bool ok, const juce::String& err)
    {
        // анализ возвращаем в версии (правки за время анализа заблокированы)
        for (auto& w : *work) if (auto* v = P.findVersion (w.id)) { auto off = v->off; *v = w; v->off = off; }
        if (! ok) { notify ("Analysis", err); return; }
        if (then) then();
    });
}

void ArrangerComponent::startRender (const juce::String& kind)
{
    if (P.pieces.empty()) { notify ("Arranger", "The table is empty."); return; }
    const double bpm = bpmEdit.getText().getDoubleValue();
    if (bpm > 30 && bpm < 300) P.songBpm = bpm;
    analyseAll ([this, kind]
    {
        auto work = std::make_shared<Project> (P);
        if (kind == "align") { work->matchEq = false; work->matchLoudness = false; }
        else if (work->sample < 0 && work->samples.empty())
        {   // образец не выбран — самый длинный кусок
            int best = 0; for (int i = 0; i < (int) work->pieces.size(); ++i) if (work->pieces[(size_t) i].length() > work->pieces[(size_t) best].length()) best = i;
            work->sample = best;
        }
        auto out = cacheDir.getChildFile (kind);
        auto res = std::make_shared<Render>();
        const int rev = revision;
        runJob (kind == "align" ? "Aligning to the song tempo" : kind == "unify" ? "Unifying the sound" : "Mixing into stems",
            [work, out, res, this] (Progress pr, juce::String& err)
            {
                out.deleteRecursively();
                juce::var report;
                if (! Engine::mixIntoStems (*work, cacheDir, out, [&] (float f, const juce::String& m) { pr (f * 0.95f, m); }, report, err)) return false;
                res->folder = out; res->report = report; res->bpm = (double) report["songBpm"]; res->length = (double) report["lengthSec"];
                if (auto* files = report["files"].getArray())
                    for (auto& f : *files)
                    {
                        const auto file = out.getChildFile (f.toString());
                        juce::String role; for (auto& r : outRoles()) if (neuroName (r) + ".wav" == f.toString()) role = r;
                        res->stems.push_back ({ role, file });
                        pr (0.96f, "Drawing " + f.toString());
                        res->env[role] = Engine::envelopeOf (file);
                    }
                return true;
            },
            [this, work, res, kind, rev] (bool ok, const juce::String& err)
            {
                if (! ok) { notify ("Arranger", err); return; }
                res->revision = rev; res->matched = kind != "align";
                render = *res;
                if (P.songBpm <= 0) P.songBpm = work->songBpm;
                bpmEdit.setText (juce::String (render.bpm, 1), juce::dontSendNotification);
                if (kind != "align" && P.sample < 0 && P.samples.empty()) P.sample = work->sample;
                updateTempoPage(); rebuildUnifyPage(); rebuildMixPage(); updateSavePreview();
                songTable->zoomAll();
                if (kind == "mix") mixNote.setText ("Done: " + juce::String ((int) render.stems.size()) + " stems, " + timeText (render.length) + juce::String::fromUTF8 (" - go to Save \xc2\xb7 Export."), juce::dontSendNotification);
            });
    });
}

//==============================================================================
juce::String ArrangerComponent::nextVariantId() const
{
    for (int n = 1;; ++n) { const auto id = "V" + juce::String (n); if (P.findVersion (id) == nullptr) return id; }
}

void ArrangerComponent::addVariant (const juce::Array<juce::File>& files)
{
    auto v = Engine::versionFromFiles (nextVariantId(), files);
    if (v.stems.empty()) { notify ("Load files", "No audio files (WAV, FLAC, MP3, AIFF) among them."); return; }
    if (songName == "Song" || songName.isEmpty())
    {   // имя песни — из имени файла до скобки («RIGOR 04B (Drums).flac» -> «RIGOR»)
        auto n = v.stems[0].file.getFileNameWithoutExtension().upToFirstOccurrenceOf ("(", false, false).trim();
        if (n.containsChar (' ')) n = n.upToLastOccurrenceOf (" ", false, false).trim();
        if (n.isNotEmpty()) { songName = n; songEdit.setText (n, juce::dontSendNotification); }
    }
    P.versions.push_back (v);
    changed();
}

void ArrangerComponent::loadFilesInto (int vi)
{
    choose ("Stems of the variant (files or a folder)", "*.flac;*.wav;*.mp3;*.aif;*.aiff",
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectDirectories | juce::FileBrowserComponent::canSelectMultipleItems,
            [this, vi] (const juce::Array<juce::File>& r)
            {
                if (r.isEmpty()) return;
                if (vi < 0 || vi >= (int) P.versions.size()) { addVariant (r); return; }
                auto v = Engine::versionFromFiles (P.versions[(size_t) vi].id, r);
                if (v.stems.empty()) return;
                P.versions[(size_t) vi] = v;
                changed();
            });
}

void ArrangerComponent::filesDropped (const juce::StringArray& files, int, int)
{
    if (busy()) return;
    juce::Array<juce::File> fs; for (auto& f : files) fs.add (juce::File (f));
    // перетащили несколько папок — каждая папка отдельный вариант
    bool allDirs = fs.size() > 1; for (auto& f : fs) if (! f.isDirectory()) allDirs = false;
    if (allDirs) { for (auto& f : fs) addVariant ({ f }); }
    else addVariant (fs);
    if (current != 0) setStep (0);
}

void ArrangerComponent::goToArranging()
{
    if (P.versions.empty()) { notify ("Arranger", "Load the stems of at least one variant."); return; }
    analyseAll ([this]
    {
        if (P.pieces.empty())
        {   // все варианты друг за другом, целиком (от первой сильной доли)
            hist.push (P);
            for (auto& v : P.versions)
                if (v.analysed) P.pieces.push_back (P.makePiece (v.id, v.bars.empty() ? 0.0 : v.bars.front(), v.lengthSec));
            ++revision;
        }
        else
        {   // новые варианты — в конец стола
            for (auto& v : P.versions)
            {
                bool used = false; for (auto& p : P.pieces) if (p.version == v.id) used = true;
                if (! used && v.analysed) { hist.push (P); P.pieces.push_back (P.makePiece (v.id, v.bars.empty() ? 0.0 : v.bars.front(), v.lengthSec)); ++revision; }
            }
        }
        setStep (1);
    });
}

//==============================================================================
void ArrangerComponent::rebuildSongPage()
{
    cards.clear();
    const int w = songView.getWidth() - 12;
    int y = 0;
    for (int i = 0; i < (int) P.versions.size(); ++i)
    {
        auto* c = new VariantCard (*this, i);
        cards.add (c); songList.addAndMakeVisible (c);
        const int h = c->heightFor (w);
        c->setBounds (0, y, w, h); y += h + 10;
        c->onLoad = [this, i] { loadFilesInto (i); };
        c->onRemove = [this, i]
        {
            const auto id = P.versions[(size_t) i].id;
            hist.push (P);
            for (int k = (int) P.pieces.size() - 1; k >= 0; --k) if (P.pieces[(size_t) k].version == id) P.remove (k);
            P.versions.erase (P.versions.begin() + i);
            ++revision;
            juce::MessageManager::callAsync ([this] { changed(); rebuildSongPage(); });
        };
    }
    addVariantBtn.setButtonText ("+ LOAD VARIANT " + juce::String ((int) P.versions.size() + 1));
    addVariantBtn.setBounds (0, y, w, 48); y += 60;
    songList.setSize (juce::jmax (1, w), y);
}

void ArrangerComponent::rebuildVariantColumn()
{
    variantCards.clear();
    const int w = variantView.getWidth() - 12;
    int y = 0;
    auto* head = new juce::Label ({}, "VARIANTS"); head->setFont (narrow (13.0f, true)); head->setColour (juce::Label::textColourId, col::orange);
    variantCards.add (head); variantCol.addAndMakeVisible (head); head->setBounds (0, 0, w, 20); y = 24;
    for (int i = 0; i < (int) P.versions.size(); ++i)
    {
        auto* t = new VariantTile (*this, i);
        variantCards.add (t); variantCol.addAndMakeVisible (t);
        t->setBounds (0, y, w, t->heightFor()); y += t->heightFor() + 8;
    }
    auto* note = new juce::Label ({}, "Click an instrument to turn it off in all pieces of the variant. Right click on a stem - remove it from one piece."); note->setFont (font (12.0f));
    note->setColour (juce::Label::textColourId, col::off); note->setJustificationType (juce::Justification::topLeft);
    variantCards.add (note); variantCol.addAndMakeVisible (note); note->setBounds (0, y, w, 60); y += 64;
    variantCol.setSize (juce::jmax (1, w), y);
}

void ArrangerComponent::updateTempoPage()
{
    juce::String list;
    if (tempoDone)
    {
        const size_t n = juce::jmin (tempo.pieceBpm.size(), P.pieces.size()), half = (n + 1) / 2;
        auto cell = [&] (size_t i)
        {
            const double dev = i < tempo.deviationPct.size() ? tempo.deviationPct[i] : 0;
            return (juce::String ((int) i + 1) + " " + P.pieces[i].version).paddedRight (' ', 7) + juce::String (tempo.pieceBpm[i], 1).paddedLeft (' ', 6)
                   + (dev >= 0 ? " +" : " ") + juce::String (dev, 1) + "%" + (std::abs (dev) < 1 ? "  " : std::abs (dev) < 2.5 ? " !" : " !!");
        };
        for (size_t i = 0; i < half; ++i) list << cell (i).paddedRight (' ', 26) << (i + half < n ? cell (i + half) : juce::String()) << "\n";
    }
    tempoList.setText (juce::CharPointer_UTF8 (list.toRawUTF8()), juce::dontSendNotification);
    suggestion.setText (tempoDone ? "Suggested  " + juce::String (tempo.proposed, 1) + " BPM" + (tempo.drumsSample > 0 ? "  (drums reference)" : "  (median of the pieces)") : juce::String ("Press ANALYSE"), juce::dontSendNotification);
    juce::String res;
    if (render.valid())
    {
        double mx = 0; if (auto* ps = render.report["pieces"].getArray()) for (auto& p : *ps) mx = juce::jmax (mx, std::abs ((double) p["stretchPct"]));
        res << "On the grid of " << juce::String (render.bpm, 1) << juce::String::fromUTF8 (" BPM  \xc2\xb7  ") << timeText (render.length) << juce::String::fromUTF8 ("  \xc2\xb7  max stretch ") << juce::String (mx, 1) << " %\n";
        if (auto* ps = render.report["pieces"].getArray())
            for (auto& p : *ps) res << "Piece " << p["n"].toString() << ": " << juce::String (render.bpm, 1) << " / was " << juce::String ((double) p["srcBpm"], 1) << "\n";
        if (render.revision != revision) res << "The table was changed - press ALIGN again.";
    }
    alignResult.setText (juce::CharPointer_UTF8 (res.toRawUTF8()), juce::dontSendNotification);
    alignResult.setColour (juce::Label::textColourId, render.valid() && render.revision == revision ? col::green : col::yellow);
}

void ArrangerComponent::rebuildUnifyPage()
{
    unifyRows.setBounds (14, 70, unifyPage.getWidth() - 28, unifyPage.getHeight() - 104);
    unifyNote.setBounds (14, unifyPage.getHeight() - 30, unifyPage.getWidth() - 28, 22);
    unifyRowComps.clear();
    juce::StringArray roles = P.rolesInSong();
    int y = 0;
    auto* head = new juce::Label ({}, "INSTRUMENT            REFERENCE PIECE                                STRENGTH                                LIMIT                                      RESULT (gain, max EQ)");
    head->setFont (narrow (12.0f, true)); head->setColour (juce::Label::textColourId, col::dim);
    unifyRowComps.add (head); unifyRows.addAndMakeVisible (head); head->setBounds (0, 0, unifyRows.getWidth(), 18); y = 20;
    for (auto& r : roles)
    {
        auto* row = new UnifyRow (*this, r);
        unifyRowComps.add (row); unifyRows.addAndMakeVisible (row);
        row->setBounds (0, y, unifyRows.getWidth(), 38); y += 38;
    }
    juce::String note;
    if (P.sample >= 0 && P.sample < (int) P.pieces.size()) note << "Reference for all: piece " << P.sample + 1 << " (" << P.pieces[(size_t) P.sample].version << ").  ";
    else note << "No reference chosen - the longest piece will be used.  ";
    if (render.matched && render.revision == revision) note << "Unified - listen below.";
    unifyNote.setText (note, juce::dontSendNotification);
    toneTgl.setToggleState (P.matchEq, juce::dontSendNotification); loudTgl.setToggleState (P.matchLoudness, juce::dontSendNotification);
}

void ArrangerComponent::rebuildMixPage()
{
    mixCards.deleteAllChildren();
    const auto roles = P.rolesInSong();
    const int n = juce::jmax (1, roles.size()), w = juce::jmin (180, (mixCards.getWidth() - (n - 1) * 8) / n);
    for (int i = 0; i < roles.size(); ++i)
    {
        auto* l = new juce::Label ({}, neuroName (roles[i]));
        bool has = false; for (auto& [r, f] : render.stems) if (r == roles[i]) has = render.folder.getFileName() == "mix";
        l->setFont (font (15.0f, true)); l->setJustificationType (juce::Justification::centred);
        l->setColour (juce::Label::backgroundColourId, has ? roleColour (roles[i]).withAlpha (0.25f) : col::panelHi);
        l->setColour (juce::Label::textColourId, col::text); l->setColour (juce::Label::outlineColourId, roleColour (roles[i]).withAlpha (0.6f));
        mixCards.addAndMakeVisible (l); l->setBounds (i * (w + 8), 10, w, 70);
    }
    if (! (render.valid() && render.folder.getFileName() == "mix")) mixNote.setText ("Press MIX INTO STEMS: tempo, unify and all pieces in one go (about a minute).", juce::dontSendNotification);
}

void ArrangerComponent::updateSavePreview()
{
    juce::String tree;
    const auto name = songName.isEmpty() ? juce::String ("Song") : songName;
    const auto pattern = patternEdit.getText();
    juce::StringArray roles; for (auto& [r, f] : render.stems) roles.add (r);
    if (roles.isEmpty()) roles = P.rolesInSong();
    tree << folderEdit.getText() << "\n";
    for (auto& r : roles) tree << "   " << pattern.replace ("{song}", name).replace ("{instrument}", neuroName (r).fromFirstOccurrenceOf ("Neuro ", false, false)) << "\n";
    tree << "   + dzod_stems.json";
    treeLbl.setText (tree, juce::dontSendNotification);
}

//==============================================================================
void ArrangerComponent::choose (const juce::String& title, const juce::String& patterns, int flags, std::function<void (const juce::Array<juce::File>&)> then)
{
    chooser = std::make_unique<juce::FileChooser> (title, juce::File(), patterns);
    chooser->launchAsync (flags, [then] (const juce::FileChooser& fc) { then (fc.getResults()); });
}

void ArrangerComponent::saveStems()
{
    if (! (render.valid() && render.folder.getFileName() == "mix")) { notify ("Save new stems", "Mix into stems first (step 5)."); return; }
    juce::StringArray written; juce::String err;
    if (! Engine::saveStems (render.folder, juce::File (folderEdit.getText()), patternEdit.getText(), songName, written, err)) { notify ("Save new stems", err); return; }
    // проект и отчёт — рядом со стемами
    const auto dir = juce::File (written[0]).getParentDirectory();
    render.folder.getChildFile ("arranger_report.json").copyFileTo (dir.getChildFile (songName + " - arranger_report.json"));
    auto v = P.toVar(); if (auto* o = v.getDynamicObject()) o->setProperty ("song", songName);
    dir.getChildFile (songName + ".dzarr").replaceWithText (juce::JSON::toString (v));
    saveNote.setText ("Saved " + juce::String (written.size()) + " stems to " + dir.getFullPathName(), juce::dontSendNotification);
    dir.revealToUser();
}

void ArrangerComponent::exportSong()
{
    if (! (render.valid() && render.folder.getFileName() == "mix")) { notify ("Export the song", "Mix into stems first (step 5)."); return; }
    const juce::File f (exportFileEdit.getText());
    if (exportFileEdit.getText().isEmpty()) { notify ("Export the song", "Choose a file."); return; }
    const auto folder = render.folder; const auto fmt = exportFormat;
    runJob ("Exporting the song", [folder, f, fmt] (Progress, juce::String& err) { return Engine::exportSong (folder, f, fmt, err); },
            [this, f] (bool ok, const juce::String& err) { if (! ok) notify ("Export the song", err); else { saveNote.setText ("Exported " + f.getFileName(), juce::dontSendNotification); f.revealToUser(); } });
}

void ArrangerComponent::saveProject (bool as)
{
    auto write = [this] (const juce::File& f)
    {
        auto v = P.toVar(); if (auto* o = v.getDynamicObject()) o->setProperty ("song", songName);
        if (f.replaceWithText (juce::JSON::toString (v))) lastProjectFile = f; else notify ("Save", "Could not write " + f.getFullPathName());
    };
    if (! as && lastProjectFile != juce::File()) { write (lastProjectFile); return; }
    choose ("Save the arrangement", "*.dzarr", juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
            [write] (const juce::Array<juce::File>& r) { if (! r.isEmpty()) write (r[0].withFileExtension (".dzarr")); });
}

void ArrangerComponent::openProject()
{
    choose ("Open an arrangement", "*.dzarr;*.json", juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::Array<juce::File>& r)
            {
                if (r.isEmpty()) return;
                Project p; juce::String err;
                const auto v = juce::JSON::parse (r[0].loadFileAsString());
                if (! p.fromVar (v, err)) { notify ("Open", err); return; }
                player.stop(); hist.clear(); selected.clear(); render = {}; tempoDone = false;
                P = p; songName = v["song"].toString().isEmpty() ? r[0].getFileNameWithoutExtension() : v["song"].toString();
                songEdit.setText (songName, juce::dontSendNotification);
                lastProjectFile = r[0]; ++revision;
                analyseAll ([this] { setStep (P.pieces.empty() ? 0 : 1); });
                changed();
            });
}
//==============================================================================
void ArrangerComponent::demo (const juce::Array<juce::File>& folders, const juce::File& shotDir)
{
    shotDir.createDirectory();
    auto shot = [this, shotDir] (const juce::String& name)
    {
        auto img = createComponentSnapshot (getLocalBounds(), true, 1.0f);
        juce::PNGImageFormat png; juce::FileOutputStream os (shotDir.getChildFile (name + ".png"));
        if (os.openedOk()) { os.setPosition (0); os.truncate(); png.writeImageToStream (img, os); }
    };
    for (auto& f : folders) addVariant ({ f });
    setStep (0); shot ("1_song_loaded");
    auto self = juce::Component::SafePointer<ArrangerComponent> (this);
    auto waitThen = std::make_shared<std::function<void (std::function<void()>)>>();
    *waitThen = [self, waitThen] (std::function<void()> f)
    {
        juce::Timer::callAfterDelay (300, [self, waitThen, f] { if (self == nullptr) return; if (self->busy()) (*waitThen) (f); else f(); });
    };
    goToArranging();
    (*waitThen) ([this, shot, waitThen]
    {
        shot ("2_song_analysed_arrange");
        if (P.pieces.size() < 2) { juce::JUCEApplicationBase::quit(); return; }
        // как автор: режем на сильных долях и собираем 5 кусков, метки частей
        auto cutAt = [this] (double sec) { cursor = sec; selected.clear(); splitAtCursor(); };
        cutAt (12.0); cutAt (P.tableStartOf (2) + 30.0);
        { const int last = (int) P.pieces.size() - 1; selected = { P.pieces[(size_t) last].id }; deleteSelected(); }
        selected = { P.pieces[0].id }; setPartForSelection ("Intro");
        selected = { P.pieces[1].id }; setPartForSelection ("Verse");
        selected.clear(); table->zoomAll(); changed();
        shot ("3_arrange_cut");
        setStep (2); analyseBtn.triggerClick();
        (*waitThen) ([this, shot, waitThen]
        {
            shot ("4_tempo_analysed");
            alignBtn.triggerClick();
            (*waitThen) ([this, shot, waitThen]
            {
                shot ("5_tempo_aligned");
                setStep (3); unifyBtn.triggerClick();
                (*waitThen) ([this, shot, waitThen]
                {
                    shot ("6_unify");
                    setStep (4); mixBtn.triggerClick();
                    (*waitThen) ([this, shot]
                    {
                        shot ("7_mix");
                        setStep (5); shot ("8_save");
                        juce::JUCEApplicationBase::quit();
                    });
                });
            });
        });
    });
}
} // namespace dzod::arranger
