# DZOD Arranger

Сборка песни из версий Suno: загрузить варианты (папки стемов) → разрезать и сложить куски →
один темп (ALIGN) → подгонка звука к образцу (UNIFY) → «Свести в стемы» (Neuro Drums, Bass, Guitar, Keys,
Synth, Backs, Vocal, Other — WAV 48 кГц 24 бит) → сохранить стемы / экспорт песни (MP3 320, WAV 24, WAV 16).
Методика — `shared/docs/song-assembly_RU.md` в dzod-hub; макет окна — design (ARRANGER v3).

Сейчас готово **ядро 0.3.0** (C++17, JUCE 8, Stemmer core; растяжка — Signalsmith Stretch) и программа проверки без окна `DZODArrangerTool`:

```
DZODArrangerTool analyse <папка версии> <кэш>
DZODArrangerTool render  <project.json> <кэш> <папка результата>
DZODArrangerTool tempo   <project.json> <кэш>
DZODArrangerTool export  <папка стемов> <файл> mp3|wav24|wav16
DZODArrangerTool save    <папка стемов> <куда> "{song}/Stems/{song} — Neuro {instrument}.wav" <песня>
DZODArrangerTool rigor   <папка RIGOR 04A–04D> <кэш> <папка результата>
DZODArrangerTool selftest
```

Окно (по макету design) — следующий шаг; затем встраивание в DZOD Studio (кнопка ARRANGER).

## Сборка
- Windows — GitHub Actions (вкладка Actions → артефакт `DZOD-Arranger-Windows`).
- Локально: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release [-DJUCE_DIR=<JUCE 8.0.4>]`, `cmake --build build`.

`third_party/stemmer` — Stemmer core из dzod-hub (`shared/stemmer/core-2026-10-25/`), `third_party/shine` — MP3-кодировщик shine, `third_party/signalsmith` — Signalsmith Stretch + Linear (MIT), растяжка фазовым вокодером.
