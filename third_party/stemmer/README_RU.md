# DZOD Stemmer core

Библиотека выравнивания стемов песни под ровный темп проекта. Общая для DZOD Drummer (вкладка STEMS) и DZOD Studio (группа Stems).
Выделена из DZOD Drummer 1.0.18 (24.10.2026) без изменений в расчётах. Проверено на настоящих стемах: выровненные файлы
и снятая партия до и после выделения совпадают побайтно.

## Файлы

| файл | что внутри |
|---|---|
| `DzodStemmer.h/.cpp` | `dzod::stemmer::Stemmer`: анализ, привязка к проекту, выравнивание, `dzod_stems.json`, `dzod_stemmer.json` |
| `Mp3Writer.h/.cpp` | запись MP3 320 кбит/с кодировщиком shine (компенсация задержки MP3) |

**Зависимости:**
- модули JUCE 8 `juce_audio_formats` и `juce_dsp`;
- shine — исходники C лежат в `ThirdParty/shine` проекта Drummer; в CMake это библиотека `DZODShine`.

Привязок к Drummer нет.

**Подключение в DZOD Studio:**
1. Скопировать папку `Stemmer/` и `ThirdParty/shine/`.
2. Добавить в цель `DzodStemmer.cpp` и `Mp3Writer.cpp`.
3. Собрать shine как статическую библиотеку, как в `CMakeLists.txt` Drummer.

## Как пользоваться

```cpp
dzod::stemmer::Stemmer st;
dzod::stemmer::Job job; job.drums = drumsWav; job.full = songWav; job.others = { bass, guitars, vocals }; job.projectBpm = 120;
if (! st.analyse (job, progress)) show (st.lastError());              // тяжёлая часть, в фоне: 5-15 с на песню
const int shift = st.analysis().autoShift + userBeatShift;              // первая доля такта (сдвиг пользователем: -3..+3)
const auto place = st.placement (shift, userBarShift);                  // place.projectBar - такт проекта, где начинается песня
juce::StringArray files; juce::String errors;
st.renderAligned (shift, userBarShift, folder, /*mp3*/ false, files, errors, progress);
st.writeAnalysis (folder.getChildFile ("dzod_stemmer.json"), shift, userBarShift, "DZOD Studio x.y");
dzod::stemmer::Stemmer::writeManifest (job, files, place.projectBar, place.placedAtSec, folder, "DZOD Studio x.y");   // последним
```

Выравнивание делается по WSOLA с сохранением высоты. Сдвиги окон считаются один раз по полной песне и применяются ко всем стемам одинаково, поэтому стемы между собой не расходятся.

Формат на выходе — 48 кГц:
- песня целиком в стерео, остальные стемы в моно;
- WAV 24 бита или MP3 320;
- все файлы начинаются с начала проекта (0 с).

## Связь с Composer (DZOD Drummer)

Stemmer только выравнивает и находит удары. **Партию снимает Composer DZOD Drummer**: он знает установку и точки, в нём же сочинитель.
Связь не теряется ни в Samplitude, ни в Studio.

**В Samplitude.** Drummer содержит эту библиотеку и, как раньше, на вкладке STEMS сразу снимает партию в Composer.
- `StemAnalyzer::assemble` переводит удары в точки установки и запускает сочинитель.

**В DZOD Studio.** Stemmer группы Stems кладёт рядом с выровненными стемами два файла:
- `dzod_stems.json` — какие стемы лежат, их роль и формат;
- `dzod_stemmer.json` — сетка долей, привязка к проекту и все удары с признаками.

Дальше Drummer снимает партию по `dzod_stemmer.json`:
- без повторного анализа звука;
- побайтно ту же, что снял бы сам (тест «Stemmer core: dzod_stemmer.json gives Composer the same part»).

Код: `StemAnalyzer::patternFromAnalysisFile (json, summary)`. Команду «Снять партию» Studio будет передавать через `Kits/_transcribe_request.json` (шаг 2); поле `file` укажет на `dzod_stemmer.json`.

Шаг 2 (следующий): по той же команде Drummer подхватит выровненные «Песню» и «Стем» по путям из `dzod_stems.json` (роли `full` и `drums`), чтобы в Composer работали кнопки «Песня» и «Стем».

## dzod_stemmer.json (format 1)

Числа записаны строками, кратчайшей записью, которая читается обратно в то же число двойной точности (`std::to_chars`). Так снятая партия совпадает побайтно.

| поле | что это |
|---|---|
| `sampleRate`, `projectBpm` | частота исходных файлов; темп проекта |
| `grid` | время каждой доли в исходных файлах, с |
| `musicStart` | где начинается музыка в полной песне, с |
| `bpmMin` / `bpmMedian` / `bpmMax` | темп песни по 8 долям |
| `autoShift` | первая доля такта, найденная автоматически (0..3) |
| `shift`, `barShift` | выбранная привязка: `autoShift` + сдвиг долей; сдвиг тактов |
| `songStartsAtBar` | такт проекта, где начинается песня (с 1) |
| `songStartSec` | то же место в проекте, с |
| `sourceStartSec` | то же место в исходном файле, с |
| `hits` | удары барабанов, столбцы `t kick body stick pitch tail` (см. ниже) |
| `snarePitch` | высота малого, Гц |
| `cymbals` | удары тарелок, столбцы `t sustain level` (подъём хвоста и уровень выше 6 кГц, дБ) |
| `tempoEstimate` | темп, вокруг которого искалась сетка (подсказка или найденный без неё) |
| `bars` | начала тактов (сильные доли) в исходных файлах, с — после `analyseSections` |
| `sections` | части песни — после `analyseSections` (см. ниже) |

Столбцы `hits`:
- `t` — время в исходном файле;
- `kick`, `body`, `stick` — уровни по типам: бочка, корпус малого или тома, щелчок;
- `pitch` — высота удара с корпусом, Гц;
- `tail` — «звон», отношение хвоста к атаке.

## Arrange: анализ версий Suno и растяжка кусков (с 25.10.2026, проверено на RIGOR 04A–04D)

Для комнаты Arrange DZOD Studio (`shared/docs/song-assembly_RU.md`).

```cpp
dzod::stemmer::Job job; job.drums = drumsStem; job.full = sumOfAllStems; job.projectBpm = 0;   // 0 - темп без подсказки
st.analyse (job, progress);                                    // сетка долей, сильные доли (autoShift), начало музыки
juce::Array<dzod::stemmer::RoleFile> stems;                    // все стемы версии с ролью
for (auto& f : files) stems.add ({ f, dzod::stemmer::Stemmer::roleFromFileName (f.getFileName()) });
st.analyseSections (stems, progress);                          // такты (bars) и части (sections)
st.writeAnalysis (versionFolder.getChildFile ("dzod_stemmer.json"), st.analysis().autoShift, 0, "DZOD Studio x.y");

// кусок версии [from, to) -> темп песни; одна карта на все стемы куска
std::function<double (int)> map; double dur, a, b;
st.pieceMap (fromSec, toSec, songBpm, map, dur, a, b);         // a, b - притянуты к долям версии
dzod::stemmer::Stemmer::StretchPlan plan;
dzod::stemmer::Stemmer::planStretch (sumMono, sr, dur, map, plan);       // один раз по сумме стемов куска
for (auto& stem : pieceStems) out = dzod::stemmer::Stemmer::applyStretch (stem, plan);   // одинаково для всех стемов
```
Карту `map` (кадр выхода → секунда исходника, шаг кадра `StretchPlan::Hs` = 1024 отсчёта) можно отдать и Rubber Band вместо WSOLA.

`roleFromFileName`: `drums`, `percussion`, `bass`, `guitar`, `keys`, `synth`, `strings`, `brass`, `woodwinds`, `fx`, `backs`, `vocals`, `other`.

### sections (каждая часть)
| поле | что это |
|---|---|
| `startBar`, `endBar` | такты [от, до), такт 0 — первая сильная доля после начала музыки |
| `startSec`, `endSec` | в исходных файлах версии, с |
| `label` | догадка: `Intro`, `Outro` (края без вокала), `Chorus X1..` (повторяющаяся громкая часть с вокалом), `Part A/B/C…` с номером повтора, `(instr)` — без вокала. Автор переименовывает |
| `group` | одинаковые по музыке части — один номер |
| `vocals`, `levelDb` | есть ли вокал; средняя громкость всех стемов |

Как ищутся части:
- по тактам считаются хрома гармонических стемов, рисунок барабанов и громкость каждой роли;
- границы — по новизне матрицы самоподобия (ядро 2 × 4 такта), не чаще 4 тактов, с предпочтением кратных 4;
- повторы — сходство частей выше `max(0,74; среднее + σ)` этой песни.

### Проверка на RIGOR 04A–04D (4 версии Suno, стемы Drums / Bass / Guitar / Vocals)
- **Темп без подсказки** найден во всех 4 версиях и в T_WeWere: 164,3 / 163,5 / 165,3 / 164,8 при медиане MIDI 164,3 / 163,6 / 165,0 / 164,8.
  Первая попытка без проверки по восьмым ошиблась на 04B (109 = 2/3 темпа) — исправлено.
- **Доли против MIDI Suno:** все доли найдены один к одному (385 / 378 / 369 / 361), ни одной лишней или пропущенной.
  Разброс после постоянного сдвига: p90 −  медиана ≈ 4–5 мс. Сильная доля такта совпала с MIDI во всех 4 версиях.
- **Растяжка куска:** 04D, 30–90 с, 164,8 → 150 BPM. Повторный анализ результата: шаг доли 0,4000 с (цель 0,4000), разброс 1,3 мс.
- **Части:** границы сходятся между версиями (такты 5, 9, 21, 37–45, ~59, ~85). Названия — догадка, у плотного металла с вокалом почти везде слабая (у 04B семь «Chorus D»).
