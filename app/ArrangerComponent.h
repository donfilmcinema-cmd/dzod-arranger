#pragma once
// DZOD Arranger — окно сборки песни из версий Suno (макет design v148, методика автора 26–27.10).
// Шаги: 1 Song & variants › 2 Arrange › 3 Tempo › 4 Unify sound › 5 Mix into stems › 6 Save · Export.
// Компонент без окна-рамки: в программе DZOD Arranger.exe — внутри DocumentWindow, в DZOD Studio — по кнопке ARRANGER.
#include "../core/Arranger.h"
#include "Player.h"
#include "Theme.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace dzod::arranger
{
class ArrangerComponent;

// Готовое сведение (ALIGN / UNIFY / MIX): папка стемов, отчёт, огибающие для просмотра.
struct Render
{
    juce::File folder;
    juce::var report;
    double bpm = 0, length = 0;
    std::vector<std::pair<juce::String, juce::File>> stems;     // роль -> WAV
    std::map<juce::String, std::vector<float>> env;
    int revision = -1;                                           // ревизия стола, с которой сведено
    bool matched = false;                                        // с подгонкой к образцу
    bool valid() const { return folder != juce::File() && ! stems.empty(); }
};

// Стол: в режиме Arrange — куски версий друг за другом (правка), в режиме Song — готовое сведение на тактах (просмотр).
class TableView : public juce::Component
{
public:
    TableView (ArrangerComponent&, bool songMode);
    void paint (juce::Graphics&) override;
    void resized() override {}
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void zoomAll();
    void zoomBy (float factor, float aroundX);
    bool knife = false;

private:
    ArrangerComponent& A;
    const bool song;
    double pxPerSec = 6, scroll = 0;
    bool fitted = false;
    static constexpr int rulerH = 24, markH = 24, rowH = 44, headW = 92;
    int dragId = -1, dropIndex = -1; bool dragging = false; juce::Point<int> downPos;
    float x (double t) const { return (float) (headW + (t - scroll) * pxPerSec); }
    double t (float px) const { return scroll + (px - headW) / pxPerSec; }
    double totalLength() const;
    juce::StringArray rows() const;
    int rowAt (int y) const;
    void pieceMenu (int index, const juce::String& role);
    void markerMenu (int index);
    void drawEnvelope (juce::Graphics&, const std::vector<float>&, double fromSec, juce::Rectangle<float> area, double secs, juce::Colour);
};

class ArrangerComponent : public juce::Component, public juce::FileDragAndDropTarget, private juce::Timer
{
public:
    ArrangerComponent();
    ~ArrangerComponent() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray&, int, int) override;

    // модель
    Project P;
    History hist;
    Player player;
    juce::String songName { "Song" };
    std::set<int> selected;                  // id кусков
    double cursor = 0;                       // секунды стола
    bool compare = false, snapBars = true;
    int revision = 0;                        // растёт с каждой правкой стола
    Render render;                           // последнее сведение (что видно в шагах 3–6)
    TempoProposal tempo; bool tempoDone = false;
    juce::File cacheDir, lastProjectFile;

    void edit (const std::function<void()>&);          // правка с Undo
    void changed();                                    // перерисовать всё после правки
    void undo(); void redo();
    void setStep (int);
    int step() const { return current; }
    void togglePlay();
    void playFrom (double sec);
    void splitAtCursor();
    void deleteSelected();
    void setPartForSelection (const juce::String& label);
    double snapTable (double tableSec) const;          // нож: на сильную долю версии под курсором
    juce::Array<juce::File> stemsOf (const Piece&) const;
    void notify (const juce::String& title, const juce::String& text);
    bool busy() const { return job != nullptr; }
    void runJob (const juce::String& title, std::function<bool (Progress, juce::String&)> work, std::function<void (bool, const juce::String&)> done);
    void startRender (const juce::String& kind);        // "align" | "unify" | "mix"
    void analyseAll (std::function<void()> then);
    // Самопроверка (--demo): загрузить папки вариантов, пройти шаги, снять экраны в shotDir, выйти.
    void demo (const juce::Array<juce::File>& variantFolders, const juce::File& shotDir);

private:
    void timerCallback() override;
    void layoutPages();
    void layoutSong();
    void rebuildSongPage();
    void rebuildVariantColumn();
    void rebuildUnifyPage();
    void rebuildMixPage();
    void updateTempoPage();
    void updateSavePreview();
    void updateFooter();
    void addVariant (const juce::Array<juce::File>&);
    void loadFilesInto (int variantIndex);
    void goToArranging();
    void saveProject (bool as);
    void openProject();
    void exportSong();
    void saveStems();
    void choose (const juce::String& title, const juce::String& patterns, int flags, std::function<void (const juce::Array<juce::File>&)>);
    juce::String nextVariantId() const;

    int current = 0;
    juce::OwnedArray<juce::TextButton> tabs;
    juce::TextButton openBtn { "OPEN" }, saveBtn { "SAVE" }, undoBtn { "UNDO" }, redoBtn { "REDO" }, nextBtn;
    juce::Label hint;
    double progressValue = 0; juce::ProgressBar progress { progressValue };
    juce::Label progressText;

    // 1 Song & variants
    juce::Component songPage; juce::Label songLabel; juce::TextEditor songEdit; juce::Viewport songView; juce::Component songList;
    juce::OwnedArray<juce::Component> cards; juce::TextButton addVariantBtn;

    // 2 Arrange
    juce::Component arrangePage;
    juce::TextButton playBtn, timeLbl { "0:00.0" }, pointerBtn { "POINTER" }, knifeBtn { "KNIFE" }, splitBtn { "SPLIT  S" }, flagBtn { "PART" },
        deleteBtn { "DELETE" }, compareBtn { "COMPARE" }, snapBtn { "SNAP TO BAR" }, refBtn { "REFERENCE" };
    juce::Viewport variantView; juce::Component variantCol; juce::OwnedArray<juce::Component> variantCards;
    std::unique_ptr<TableView> table;
    juce::TextButton zoomOut { "-" }, zoomAll { "ALL" }, zoomIn { "+" };

    // 3–6: карточки сверху, снизу — песня (просмотр)
    juce::Component tempoPage, unifyPage, mixPage, savePage;
    std::unique_ptr<TableView> songTable;
    juce::TextButton songPlay; juce::Label songTime;
    // Tempo
    juce::TextButton analyseBtn { "ANALYSE" }, alignBtn { "ALIGN" }; juce::Label tempoList, suggestion, bpmCaption, alignResult; juce::TextEditor bpmEdit;
    // Unify
    juce::TextButton unifyBtn { "START UNIFY" }; juce::ToggleButton toneTgl { "Tone (EQ)" }, loudTgl { "Loudness" };
    juce::Component unifyRows; juce::OwnedArray<juce::Component> unifyRowComps; juce::Label unifyNote;
    // Mix
    juce::TextButton mixBtn { "MIX INTO STEMS" }; juce::Component mixCards; juce::Label mixNote;
    // Save · Export
    juce::TextEditor folderEdit, patternEdit, exportFileEdit; juce::TextButton folderBrowse { "BROWSE..." }, exportBrowse { "BROWSE..." },
        saveStemsBtn { "SAVE NEW STEMS" }, exportBtn { "EXPORT SONG" }, mp3Btn { "MP3 320" }, wav24Btn { "WAV 24-bit" }, wav16Btn { "WAV 16-bit" }, sendBtn { "SEND TO STEMS" };
    juce::Label treeLbl, saveNote, spaceNote; juce::String exportFormat = "mp3";

    struct JobThread;
    std::unique_ptr<JobThread> job;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::CriticalSection msgLock; juce::String jobMsg; std::atomic<float> jobFrac { 0 };
    friend class TableView;
};
} // namespace dzod::arranger
