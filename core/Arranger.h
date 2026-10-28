#pragma once
// DZOD Arranger — ядро без окна: сборка песни из версий Suno (shared/docs/song-assembly_RU.md).
//   версии (папка стемов одной версии) -> анализ (темп, сильные доли, части — Stemmer core; тональность, громкость — свои)
//   -> куски [from, to) в секундах версии, по порядку песни, края — на сильных долях
//   -> «Свести в стемы»: каждый кусок растягивается к одному темпу песни (доли на долях), стыки — кроссфейд,
//      звук каждой роли подгоняется к образцу роли (громкость + кривая не больше ±limit дБ),
//      на роль — один WAV 48 кГц 24 бит (Neuro Drums, Neuro Bass, …) + dzod_stems.json + отчёт arranger_report.json.
#include "Match.h"
#include "../third_party/stemmer/DzodStemmer.h"
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace dzod
{
namespace arranger
{
using Progress = std::function<void (float, const juce::String&)>;
inline juce::String ru (const char* utf8) { return juce::String (juce::CharPointer_UTF8 (utf8)); }

// роль стема Suno (по имени файла, как у Stemmer) -> строка стола и итоговый стем
juce::String outRoleOf (const juce::String& stemmerRole);       // drums bass guitar keys synth backs vocals other
juce::String neuroName (const juce::String& outRole);           // «Neuro Drums» …
const juce::StringArray& outRoles();                            // порядок строк стола

struct VersionStem
{
    juce::File file;
    juce::String role;                // роль Stemmer (drums, bass, guitar, keys, synth, strings, …)
    juce::String outRole;             // строка стола
    double lufs = -70, peakDb = -100;
    bool silent = false;              // почти пустой (громкость ниже -55 LUFS)
};

struct Version
{
    juce::String id;                  // имя папки
    juce::File folder;
    std::vector<VersionStem> stems;
    // результат анализа
    juce::File analysisFile;          // dzod_stemmer.json этой версии (в папке кэша)
    double sampleRate = 0, lengthSec = 0, bpm = 0, bpmMin = 0, bpmMax = 0;
    std::vector<double> grid, bars;   // доли и сильные доли, с
    std::vector<dzod::stemmer::Section> sections;
    Key key;
    bool analysed = false;
    std::set<juce::String> off;       // инструменты варианта, выключенные во всех его кусках (плитка в левой колонке)

    double snapToBar (double sec) const;          // ближайшая сильная доля (нож)
    double localBpm (double fromSec, double toSec) const;   // темп по долям внутри участка
    bool hasRole (const juce::String& outRole) const;
};

struct Piece
{
    juce::String version;             // id версии
    double from = 0, to = 0;          // секунды в исходных файлах версии (нож режет где угодно; ALIGN ставит края на доли)
    std::set<juce::String> off;       // инструменты, убранные из этого куска (меню правой кнопки на стеме)
    int id = 0;                       // постоянный номер куска (метки частей, окно)
    int part = 0;                     // метка части (флажок): номер в Project::parts, 0 — без метки
    double length() const { return to - from; }
};

// Шаг Tempo: темп каждого куска и предложение для песни.
struct TempoProposal
{
    std::vector<double> pieceBpm;     // по порядку кусков
    std::vector<double> deviationPct; // отклонение куска от темпа песни, %: зелёный < 1, жёлтый < 2,5, красный больше
    double median = 0;                // медиана, взвешенная длиной кусков, округлена до 0,1
    double drumsSample = 0;           // темп куска-образца барабанов (0 — образца нет)
    double proposed = 0;              // что ставить в поле BPM: образец барабанов, иначе медиана
};

struct Project
{
    std::vector<Version> versions;
    std::vector<Piece> pieces;                    // по порядку песни
    std::map<int, juce::String> parts;            // метки частей (Intro, Verse, Chorus…): номер -> имя; ставятся на группу кусков
    int nextId = 1;
    std::map<juce::String, int> samples;          // образец по роли: outRole -> номер куска
    std::map<juce::String, double> strengths;     // сила подгонки по роли (таблица Unify); нет — общая strength
    std::map<juce::String, double> limits;        // предел кривой по роли, дБ; нет — общий limitDb
    int sample = -1;                              // образец для ролей без своего (-1 — нет)
    double songBpm = 0;                           // 0 — по образцу барабанов
    double crossfadeMs = 10, strength = 0.7, limitDb = 6;
    bool matchEq = true, matchLoudness = true;
    bool snapToBars = true;
    juce::String stretcher = "phase";             // "phase" — фазовый вокодер Signalsmith (по умолчанию), "wsola" — старая растяжка Stemmer (сравнение)                       // ALIGN: края кусков на сильные доли (false — на ближайшие доли)

    Version* findVersion (const juce::String& id);
    const Version* findVersion (const juce::String& id) const;
    int sampleFor (const juce::String& outRole) const;
    double strengthFor (const juce::String& outRole) const;
    double limitFor (const juce::String& outRole) const;
    bool isOn (const Piece&, const juce::String& outRole) const;   // инструмент звучит в куске (есть, не выключен)

    // Монтаж (стол до ALIGN: куски друг за другом).
    Piece makePiece (const juce::String& version, double from, double to);   // новый кусок со своим id
    int indexOf (int pieceId) const;
    double tableLength() const;
    double tableStartOf (int piece) const;
    int pieceAt (double tableSec) const;                           // -1 — вне кусков
    bool split (int piece, double tableSec);                      // нож / S по курсору: кусок -> два
    int splitAll (double tableSec, const std::vector<int>& only = {});   // S: выделенные (или все под курсором)
    void remove (int piece);                                      // Delete
    void move (int piece, int newIndex);                          // перетаскивание
    juce::StringArray rolesInSong() const;                        // строки стола: инструменты, что звучат хоть в одном куске
    // Метка части на группу кусков (едет с кусками, при резке — на обе половины). Пустое имя — снять метку.
    int setPart (const std::vector<int>& pieceIndices, const juce::String& label);
    void renamePart (int part, const juce::String& label);
    struct Section { juce::String label; int firstPiece = 0, lastPiece = 0; };
    std::vector<Section> sections() const;                        // подряд идущие куски с одной меткой — одна часть

    // Состояние монтажа без анализа (для Undo / Redo): куски, метки, образцы, настройки, выключенные инструменты.
    juce::var editState() const;
    void applyEditState (const juce::var&);
    juce::var toVar() const;
    bool fromVar (const juce::var&, juce::String& error);
    bool save (const juce::File&) const;
    bool load (const juce::File&, juce::String& error);
};

// Undo / Redo монтажа: перед каждым действием окно зовёт push (project).
class History
{
public:
    void push (const Project&);
    bool undo (Project&);
    bool redo (Project&);
    bool canUndo() const { return ! back.empty(); }
    bool canRedo() const { return ! fwd.empty(); }
    void clear() { back.clear(); fwd.clear(); }
    size_t limit = 200;
private:
    std::vector<juce::String> back, fwd;
};

class Engine
{
public:
    // Стемы папки версии с ролями по имени файла (без анализа).
    static Version scanVersion (const juce::File& folder);
    // Анализ версии: темп без подсказки, доли, сильные доли, части (Stemmer core), тональность и громкость стемов.
    // Результат Stemmer кэшируется в cacheDir/<id>_dzod_stemmer.json; повторный вызов читает кэш.
    static bool analyseVersion (Version&, const juce::File& cacheDir, Progress, juce::String& error);
    // Темп песни: заданный в проекте или темп образца барабанов (по его долям).
    static double songBpm (const Project&);
    // TEMPO ANALYSE: темп каждого куска и предложение.
    // songBpm > 0 — отклонения считать от него (после выбора темпа), иначе от предложения.
    static TempoProposal analyseTempo (const Project&, double songBpm = 0);
    // «Свести в стемы». Пишет WAV по ролям, dzod_stems.json и arranger_report.json в outFolder.
    static bool mixIntoStems (Project&, const juce::File& cacheDir, const juce::File& outFolder, Progress, juce::var& report, juce::String& error);
    // EXPORT SONG: сумма стемов папки (по dzod_stems.json) в один файл. format: "mp3" (320), "wav24", "wav16".
    static bool exportSong (const juce::File& stemsFolder, const juce::File& target, const juce::String& format, juce::String& error);
    // SAVE NEW STEMS: копии стемов по шаблону имени, {song} и {instrument} (например «{song}/Stems/{song} — Neuro {instrument}.wav»).
    static bool saveStems (const juce::File& stemsFolder, const juce::File& destRoot, const juce::String& pattern, const juce::String& song,
                           juce::StringArray& written, juce::String& error);
};
} // namespace arranger
} // namespace dzod
