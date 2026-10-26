#pragma once
// DZOD Stemmer core - выравнивание стемов песни под ровный темп проекта.
// Общая библиотека для DZOD Drummer (вкладка STEMS) и DZOD Studio (группа Stems). Без привязок к Drummer.
// Зависимости: модули JUCE juce_audio_formats и juce_dsp (JUCE 8), кодировщик shine (ThirdParty/shine) для MP3.
//
// Что делает:
//   analyse      - по барабанному стему и полной песне: сетка долей (темп может плавать), начало музыки,
//                  удары барабанов с признаками (бочка / корпус / щелчок, высота, хвост), удары тарелок,
//                  первая доля такта (autoShift);
//   placement    - куда первый такт песни встаёт в проекте (такт проекта, секунды);
//   renderAligned- все стемы под ровный темп: 48 кГц, песня целиком стерео, остальные моно; MP3 320 или WAV 24;
//   writeManifest- dzod_stems.json рядом со стемами (что лежит, роль, формат);
//   writeAnalysis/readAnalysis - dzod_stemmer.json: сетка и удары. Это связь с Composer DZOD Drummer:
//                  по нему Composer снимает партию без повторного анализа (см. README_RU.md).
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include <functional>
#include <memory>
#include <vector>

namespace dzod
{
namespace stemmer
{
inline juce::String ru (const char* utf8) { return juce::String (juce::CharPointer_UTF8 (utf8)); }

struct Job
{
    juce::File drums, full;              // обязательные: барабанный стем и полная песня
    juce::Array<juce::File> others;      // остальные стемы - только выравниваются
    double projectBpm = 120.0;           // 0 - темп без подсказки (анализ версии для Arrange)
};

// Удар барабанов (атака в барабанном стеме) и его признаки.
struct Hit
{
    double t = 0;                        // время в исходном файле, с
    float kick = 0, body = 0, stick = 0; // уровни по типам: бочка, корпус (малый / томы), щелчок (сайд-стик)
    float pitch = 0, tail = 0;           // у ударов с корпусом: высота, Гц, и «звон» (хвост / атака)
};
// Удар тарелок (отдельный детектор верхних частот).
struct HiHit { double t = 0; float sustain = 0, level = 0; };

// Стем версии с ролью (по имени файла Suno: Drums, Bass, Guitar, Keyboard, Synth, Backing Vocals, Vocals, ...).
struct RoleFile { juce::File file; juce::String role; };
// Часть песни - целые такты от первой сильной доли (bar 0 = такт с первой долей сетки после начала музыки).
struct Section
{
    int startBar = 0, endBar = 0;        // [start, end) в тактах версии
    double startSec = 0, endSec = 0;     // в исходных файлах
    juce::String label;                  // Intro / Verse N / Chorus N / Bridge / Instrumental / Solo / Outro - догадка, автор переименовывает
    int group = 0;                       // одинаковые по музыке части - один номер
    bool vocals = false;
    float levelDb = -90;                 // средняя громкость всех стемов
};

struct Analysis
{
    double sampleRate = 44100, projectBpm = 120;
    std::vector<double> grid;            // время каждой доли в исходных файлах, с
    double musicStart = 0, bpmMin = 0, bpmMedian = 0, bpmMax = 0, snarePitch = 190;
    std::vector<Hit> hits;
    std::vector<HiHit> hi;
    int autoShift = 0;                   // первая доля такта (0..3) - выбрана автоматически
    std::vector<double> bars;            // начала тактов (сильные доли) в исходных файлах, с - после analyseSections
    std::vector<Section> sections;       // части песни - после analyseSections
    double tempoEstimate = 0;            // темп, вокруг которого искалась сетка (подсказка или найденный без неё)
};

// Привязка к проекту. shift = autoShift + сдвиг долей, barShift - сдвиг тактов.
struct Placement
{
    int d0 = 0, bar = 0, off = 0;        // доля сетки первого такта, такт проекта (с 0), смещение: доля проекта = доля сетки + off
    double songStartSec = 0;             // где первый такт был в файле
    double placedAtSec = 0;              // куда встал в проекте
    int projectBar = 1;                  // такт проекта, с 1
};

// положение времени t на сетке долей (дробный индекс)
double gridPos (double t, const std::vector<double>& grid);
// процентиль (как numpy.percentile, линейно)
float percentile (std::vector<float> v, double p);

class Stemmer
{
public:
    using Progress = std::function<bool (float, const juce::String&)>;      // возвращает false - прервать

    bool analyse (const Job&, Progress);                 // тяжёлая часть
    bool isReady() const { return ready; }
    const juce::String& lastError() const { return error; }
    const Job& job() const { return jobData; }
    const Analysis& analysis() const { return result; }

    Placement placement (int shift, int barShift) const; // shift - абсолютный (autoShift + сдвиг долей)
    double srcTime (double projectBeat, int off) const;  // доля проекта -> время в исходном файле

    // Arrange: такты и части песни по всем стемам версии (после analyse; шаг - autoShift)
    bool analyseSections (const juce::Array<RoleFile>& stems, Progress);
    static juce::String roleFromFileName (const juce::String& fileName);   // «RIGOR 04A (Backing Vocals).flac» -> backs
    // Arrange: кусок версии [srcFromSec, srcToSec) (притягивается к долям) -> темп песни targetBpm, доли на долях.
    // Возвращает функцию «кадр выхода -> секунда исходника» для planStretch и длину выхода, с.
    bool pieceMap (double srcFromSec, double srcToSec, double targetBpm, std::function<double (int)>& srcSecOfFrame, double& durOutSec,
                   double& snappedFromSec, double& snappedToSec) const;

    // Выравнивание всех стемов в папку outFolder с сохранением высоты: 48 кГц, песня целиком в стерео, остальные стемы в моно.
    // mp3: MP3 320 кбит/с, иначе WAV 24 бита. В written - пути записанных файлов, в errors - пропущенные.
    bool renderAligned (int shift, int barShift, const juce::File& outFolder, bool mp3,
                        juce::StringArray& written, juce::String& errors, Progress);

    // dzod_stems.json (format 1) в папке стемов; пишется ПОСЛЕДНИМ, после всех аудиофайлов. Возвращает файл.
    static juce::File writeManifest (const Job&, const juce::StringArray& alignedFiles, int projectBar, double placedAtSec,
                                     const juce::File& folder, const juce::String& writer);
    // dzod_stemmer.json: анализ (сетка, удары) и привязка - для Composer
    bool writeAnalysis (const juce::File& target, int shift, int barShift, const juce::String& writer) const;
    // Прочитать анализ из dzod_stemmer.json (без звука: выравнивать нельзя, снимать партию - можно)
    bool readAnalysis (const juce::File& source);

    // Растяжка WSOLA по карте времени (общая для выравнивания и для кусков Arrange):
    // plan - один раз по опорному моно-звуку, apply - к каждому стему (одинаково, стемы не расходятся). Высота не меняется.
    struct StretchPlan { static constexpr int N = 2048, Hs = 1024, PAD = 8192; std::vector<juce::int64> X; int frames = 0; };
    static bool planStretch (const std::vector<float>& referenceMono, double sampleRate, double durOutSec,
                             const std::function<double (int)>& srcSecOfFrame,          // кадр выхода (шаг Hs) -> время в исходнике, с
                             StretchPlan&, const std::function<bool (float)>& progress = nullptr);
    static juce::AudioBuffer<float> applyStretch (const juce::AudioBuffer<float>& in, const StretchPlan&);

    static bool loadAudio (const juce::File&, juce::AudioBuffer<float>&, double& sampleRate, juce::String& error);
    static juce::AudioBuffer<float> resample (const juce::AudioBuffer<float>&, double fromRate, double toRate);     // без сдвига во времени

private:
    struct Audio;                                        // моно барабанов и песни для выравнивания
    std::shared_ptr<Audio> audio;
    Job jobData; Analysis result;
    bool ready = false; juce::String error;
    int chooseShift() const;
};
} // namespace stemmer
} // namespace dzod
