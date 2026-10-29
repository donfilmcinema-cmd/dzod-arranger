// DZODArrangerTool — ядро DZOD Arranger без окна.
//   analyse <папка версии> <кэш>                       темп, сильные доли, части, тональность, громкость стемов
//   render  <project.json> <кэш> <папка результата>    «Свести в стемы»
//   rigor   <папка RIGOR> <кэш> <папка результата>     пример: песня из кусков 04A–04D, сведение и проверка
//   verify  <Neuro Drums.wav> <bpm> [швы, с через запятую]   доли результата против ровной сетки
//   tempo   <project.json> <кэш>                       TEMPO ANALYSE: темп кусков и предложение
//   export  <папка стемов> <файл> <mp3|wav24|wav16>    EXPORT SONG
//   save    <папка стемов> <куда> <шаблон> <песня>      SAVE NEW STEMS
//   selftest                                           нож, S, Delete, перетаскивание, маркеры (без звука)
#include "../core/Arranger.h"
#include <iostream>

using namespace dzod::arranger;
using dzod::stemmer::Stemmer;

static void out (const juce::String& s) { std::cout << s.toStdString() << std::endl; }
static Progress printer() { return [] (float f, const juce::String& m) { std::cout << "  [" << juce::roundToInt (f * 100) << "%] " << m.toStdString() << std::endl; }; }

static void printVersion (const Version& v)
{
    out (v.id + ": " + juce::String (v.lengthSec, 1) + " s, tempo " + juce::String (v.bpm, 2) + " (" + juce::String (v.bpmMin, 1) + ".." + juce::String (v.bpmMax, 1)
         + "), bars " + juce::String ((int) v.bars.size()) + ", first bar " + juce::String (v.bars.empty() ? 0.0 : v.bars[0], 3) + " s, key " + v.key.name()
         + " (" + juce::String (v.key.confidence, 2) + ")");
    for (auto& s : v.stems)
        out ("    " + s.file.getFileName() + "  ->  " + s.outRole + "  " + juce::String (s.lufs, 1) + " LUFS" + (s.silent ? "  (почти пусто)" : ""));
    for (auto& sc : v.sections)
        out ("    " + sc.label + "  bars " + juce::String (sc.startBar + 1) + "-" + juce::String (sc.endBar) + "  " + juce::String (sc.startSec, 2) + "-" + juce::String (sc.endSec, 2) + " s");
}

static void printReport (const juce::var& r)
{
    out ("song " + juce::String ((double) r["songBpm"], 2) + " BPM, " + juce::String ((double) r["lengthSec"], 2) + " s, trim " + juce::String ((double) r["trimDb"], 1) + " dB");
    for (auto& p : *r["pieces"].getArray())
        out ("  piece " + p["n"].toString() + "  " + p["version"].toString() + "  " + juce::String ((double) p["srcFrom"], 2) + "-" + juce::String ((double) p["srcTo"], 2)
             + " s  ->  at " + juce::String ((double) p["at"], 3) + " s, " + p["bars"].toString() + " bars, src " + juce::String ((double) p["srcBpm"], 2)
             + " BPM, stretch " + juce::String ((double) p["stretchPct"], 2) + " %, key " + p["key"].toString()
             + (p["notesSimilarityToPrev"].isVoid() ? juce::String() : ", notes vs prev " + juce::String ((double) p["notesSimilarityToPrev"], 2)));
    for (auto& m : *r["matching"].getArray())
    {
        juce::String oct; for (auto& x : *m["eqOctaves"].getArray()) oct << juce::String ((double) x, 1) << " ";
        out ("  match " + m["role"].toString() + " / " + m["version"].toString() + " (sample " + m["sample"].toString() + "): gain " + juce::String ((double) m["gainDb"], 1) + " dB, EQ " + oct);
    }
    for (auto& w : *r["warnings"].getArray()) out ("  ! " + w.toString());
}

static int verify (const juce::File& drums, double bpm, const juce::StringArray& seams)
{
    Stemmer st; dzod::stemmer::Job job; job.drums = drums; job.full = drums; job.projectBpm = bpm; job.straightenGrid = true;
    if (! st.analyse (job, nullptr)) { out ("ERROR " + st.lastError()); return 1; }
    const auto& g = st.analysis().grid;
    const double beat = 60.0 / bpm;
    // постоянный сдвиг сетки детектора — медиана; дальше отклонения каждой доли от ровной сетки
    std::vector<double> ph; for (double t : g) { double r = std::fmod (t, beat); if (r > beat / 2) r -= beat; ph.push_back (r); }
    auto sorted = ph; std::sort (sorted.begin(), sorted.end()); const double off = sorted[sorted.size() / 2];
    std::vector<double> dev; double worst = 0, worstAt = 0;
    for (size_t i = 0; i < g.size(); ++i) { const double d = std::abs (ph[i] - off) * 1000; dev.push_back (d); if (d > worst) { worst = d; worstAt = g[i]; } }
    std::sort (dev.begin(), dev.end());
    out ("verify: beats " + juce::String ((int) g.size()) + ", grid offset " + juce::String (off * 1000, 1) + " ms, |dev| p50 " + juce::String (dev[dev.size() / 2], 1)
         + " p90 " + juce::String (dev[dev.size() * 9 / 10], 1) + " max " + juce::String (worst, 1) + " ms at " + juce::String (worstAt, 2) + " s, tempo found "
         + juce::String (st.analysis().bpmMedian, 2));
    {   // худшие доли
        std::vector<std::pair<double, double>> w; for (size_t i = 0; i < g.size(); ++i) w.push_back ({ std::abs (ph[i] - off) * 1000, g[i] });
        std::sort (w.rbegin(), w.rend()); juce::String s2 = "  worst:";
        for (size_t i = 0; i < juce::jmin ((size_t) 10, w.size()); ++i) s2 << " " << juce::String (w[i].second, 2) << "s=" << juce::String (w[i].first, 0);
        out (s2);
    }
    {   // независимая проверка: где в звуке атака около каждой ровной доли (порог 25 % от пика окна, как «attacks» у Stemmer)
        juce::AudioBuffer<float> b; double sr = 0; juce::String e;
        if (Stemmer::loadAudio (drums, b, sr, e))
        {
            std::vector<double> d;
            for (double t = off; t < b.getNumSamples() / sr - 0.1; t += beat)
            {
                const int s0 = (int) ((t - 0.04) * sr), s1 = (int) ((t + 0.06) * sr); if (s0 < 0) continue;
                float mx = 0; for (int i = s0; i < s1; ++i) for (int c = 0; c < b.getNumChannels(); ++c) mx = juce::jmax (mx, std::abs (b.getSample (c, i)));
                if (mx < 0.05f) continue;
                for (int i = s0; i < s1; ++i) { float v = 0; for (int c = 0; c < b.getNumChannels(); ++c) v = juce::jmax (v, std::abs (b.getSample (c, i))); if (v >= 0.25f * mx) { d.push_back ((i / sr - t) * 1000.0); break; } }
            }
            std::sort (d.begin(), d.end());
            if (! d.empty())
                out ("  attacks vs straight grid: beats " + juce::String ((int) d.size()) + ", p10 " + juce::String (d[d.size() / 10], 1) + " median " + juce::String (d[d.size() / 2], 1)
                     + " p90 " + juce::String (d[d.size() * 9 / 10], 1) + " ms (spread p90-p10 " + juce::String (d[d.size() * 9 / 10] - d[d.size() / 10], 1) + ")");
        }
    }
    for (auto& s : seams)
    {
        const double t = s.getDoubleValue(); double m = 0;
        for (size_t i = 0; i < g.size(); ++i) if (std::abs (g[i] - t) < 4 * beat) m = juce::jmax (m, std::abs (ph[i] - off) * 1000);
        out ("  seam " + juce::String (t, 2) + " s: max dev of beats within 4 beats " + juce::String (m, 1) + " ms");
    }
    return worst < 40 ? 0 : 3;
}

int main (int argc, char** argv)
{
    const juce::String cmd = argc > 1 ? argv[1] : "";
    juce::String err;
    if (cmd == "analyse" && argc >= 4)
    {
        auto v = Engine::scanVersion (juce::File (argv[2]));
        if (! Engine::analyseVersion (v, juce::File (argv[3]), printer(), err)) { out ("ERROR " + err); return 1; }
        printVersion (v); return 0;
    }
    if (cmd == "render" && argc >= 5)
    {
        Project p; if (! p.load (juce::File (argv[2]), err)) { out ("ERROR " + err); return 1; }
        juce::var r; if (! Engine::mixIntoStems (p, juce::File (argv[3]), juce::File (argv[4]), printer(), r, err)) { out ("ERROR " + err); return 1; }
        printReport (r); return 0;
    }
    if (cmd == "tempo" && argc >= 4)
    {
        Project p; if (! p.load (juce::File (argv[2]), err)) { out ("ERROR " + err); return 1; }
        for (auto& v : p.versions) if (! Engine::analyseVersion (v, juce::File (argv[3]), nullptr, err)) { out ("ERROR " + err); return 1; }
        auto tp = Engine::analyseTempo (p);
        for (size_t i = 0; i < tp.pieceBpm.size(); ++i) out ("  piece " + juce::String ((int) i + 1) + " " + p.pieces[i].version + ": " + juce::String (tp.pieceBpm[i], 2) + " BPM, "
                                                             + juce::String (tp.deviationPct[i], 2) + " %");
        out ("median " + juce::String (tp.median, 1) + ", drums sample " + juce::String (tp.drumsSample, 1) + ", proposed " + juce::String (tp.proposed, 1));
        return 0;
    }
    if (cmd == "export" && argc >= 5)
    {
        if (! Engine::exportSong (juce::File (argv[2]), juce::File (argv[3]), argv[4], err)) { out ("ERROR " + err); return 1; }
        out ("written " + juce::String (argv[3]) + " " + juce::String (juce::File (argv[3]).getSize() / 1024) + " KB"); return 0;
    }
    if (cmd == "save" && argc >= 6)
    {
        juce::StringArray w;
        if (! Engine::saveStems (juce::File (argv[2]), juce::File (argv[3]), juce::String::fromUTF8 (argv[4]), juce::String::fromUTF8 (argv[5]), w, err)) { out ("ERROR " + err); return 1; }
        for (auto& f : w) out ("  " + f);
        return 0;
    }
    if (cmd == "selftest")
    {
        int bad = 0; auto check = [&] (bool c, const char* what) { out (juce::String (c ? "  ok   " : "  FAIL ") + what); if (! c) ++bad; };
        Project p;
        Version a; a.id = "A"; VersionStem s1; s1.outRole = "drums"; s1.role = "drums"; VersionStem s2 = s1; s2.outRole = "guitar"; s2.role = "guitar"; a.stems = { s1, s2 };
        Version b = a; b.id = "B";
        p.versions = { a, b };
        p.pieces = { p.makePiece ("A", 10, 20), p.makePiece ("B", 30, 50) };
        p.sample = 1; p.samples["drums"] = 0;
        History h;
        check (std::abs (p.tableLength() - 30) < 1e-9, "длина стола 30 с");
        check (p.pieceAt (15) == 1 && p.pieceAt (5) == 0 && p.pieceAt (31) == -1, "кусок под курсором");
        h.push (p); const int verse = p.setPart ({ 0 }, "Verse");
        h.push (p); check (p.splitAll (4) == 1 && p.pieces.size() == 3 && std::abs (p.pieces[0].to - 14) < 1e-9 && std::abs (p.pieces[1].from - 14) < 1e-9, "S: разрез 1-го куска на 4 с (14 с версии A)");
        check (p.pieces[0].part == verse && p.pieces[1].part == verse && p.pieces[0].id != p.pieces[1].id, "метка части — на обеих половинах, id разные");
        check (p.sample == 2 && p.samples["drums"] == 0, "образцы сдвинулись");
        check (p.splitAll (20, { 0 }) == 0, "S с выделением: курсор не над выделенным — не режет");
        check (! p.split (0, 0.01), "у самого края не режет");
        const int idB = p.pieces[2].id;
        h.push (p); p.move (2, 0);
        check (p.pieces[0].id == idB && p.sample == 0 && p.samples["drums"] == 1, "перетаскивание: образцы за кусками");
        { auto sc = p.sections(); check (sc.size() == 1 && sc[0].label == "Verse" && sc[0].firstPiece == 1 && sc[0].lastPiece == 2, "метка едет с кусками"); }
        h.push (p); p.setPart ({ 0 }, "Chorus");
        { auto sc = p.sections(); check (sc.size() == 2 && sc[0].label == "Chorus", "две части"); }
        h.push (p); p.pieces[1].off.insert ("guitar"); p.versions[1].off.insert ("drums");
        check (! p.isOn (p.pieces[1], "guitar") && p.isOn (p.pieces[2], "guitar") && ! p.isOn (p.pieces[0], "drums"), "убрать инструмент из куска и плитка варианта");
        check (p.rolesInSong().size() == 2, "строки стола: drums, guitar");
        h.push (p); p.versions[0].off.insert ("drums");
        check (p.rolesInSong().size() == 1, "drums выключен везде — строки нет");
        h.push (p); p.remove (0);
        check (p.pieces.size() == 2 && p.sample == -1 && p.samples["drums"] == 0 && p.parts.size() == 1, "Delete: образцы и метки");
        juce::String e; Project q; q.fromVar (p.toVar(), e);
        check (q.pieces[0].off.count ("guitar") == 1 && q.versions[0].off.count ("drums") == 1 && q.sections().size() == 1 && q.pieces[0].id == p.pieces[0].id, "сохранение и загрузка");
        check (h.undo (p) && p.pieces.size() == 3 && p.sample == 0, "Undo: Delete вернул кусок");
        check (h.undo (p) && p.rolesInSong().size() == 2, "Undo: инструмент вернулся");
        check (h.redo (p) && p.rolesInSong().size() == 1, "Redo");
        while (h.undo (p)) {}
        check (p.pieces.size() == 2 && p.parts.empty() && ! h.canUndo() && h.canRedo(), "Undo до начала");
        {   // нумерация частей внутри вариации и края кусков (замечания автора 29.10)
            Project r; Version va; va.id = "A"; va.lengthSec = 100; va.stems = { s1 }; Version vb = va; vb.id = "B"; r.versions = { va, vb };
            r.pieces = { r.makePiece ("A", 0, 10), r.makePiece ("A", 10, 30), r.makePiece ("A", 40, 60), r.makePiece ("B", 5, 25), r.makePiece ("B", 50, 70) };
            r.setPart ({ 1 }, r.numberedLabel ({ 1 }, "Verse"));
            r.setPart ({ 2 }, r.numberedLabel ({ 2 }, "Verse"));
            r.setPart ({ 3 }, r.numberedLabel ({ 3 }, "Verse"));
            check (r.parts[r.pieces[1].part] == "Verse 1" && r.parts[r.pieces[2].part] == "Verse 2" && r.parts[r.pieces[3].part] == "Verse 1", "Verse 1 / Verse 2 внутри вариации, первый куплет второй вариации — Verse 1");
            check (r.numberedLabel ({ 4 }, "Verse") == "Verse 2" && Project::partType ("Pre-chorus 3") == "Pre-chorus", "номер по порядку в исходнике");
            check (r.setEdges (0, -5, 12) && r.pieces[0].from == 0 && r.pieces[0].to == 12 && ! r.setEdges (0, 50, 50), "края куска: раздвинуть, не за начало файла");
            check (r.extendGroup ({ 1, 2 }, 2, 3) && r.pieces[1].from == 8 && r.pieces[2].to == 63, "края группы");
            {   // край как в Samplitude: звук на месте, граница общая с соседом
                Project e; Version v1; v1.id = "A"; v1.lengthSec = 100; v1.stems = { s1 }; Version v2 = v1; v2.id = "B"; e.versions = { v1, v2 };
                e.pieces = { e.makePiece ("A", 10, 20), e.makePiece ("B", 30, 40), e.makePiece ("A", 50, 60) };
                const double d1 = e.moveEdge (0, true, 2.0);      // правый край куска 1 вправо: открывает A 20..22, у B забирает 30..32
                check (d1 == 2.0 && e.pieces[0].to == 22 && e.pieces[1].from == 32 && std::abs (e.tableStartOf (1) + (32 - e.pieces[1].from) - 12) < 1e-9,
                       "край вправо: открыто 2 с, сосед укорочен, звук соседа на месте");
                const double d2 = e.moveEdge (1, false, -4.0);    // левый край куска 2 влево: открывает B 28..32, A укорочен до 18
                check (d2 == -4.0 && e.pieces[0].to == 18 && e.pieces[1].from == 28, "край влево: открыто у второго, первый укорочен");
                check (std::abs (e.tableLength() - 30) < 1e-9, "длина стола не меняется (звук не растягивается)");
                const double d3 = e.moveEdge (0, false, -15.0);   // левый край стола: открыть до начала файла (A с 10 — до 0)
                check (d3 == -10.0 && e.pieces[0].from == 0, "край стола: открывает до начала файла");
                const double d4 = e.moveEdge (1, true, 100.0);    // сосед не меньше 0.05 с
                check (std::abs (e.pieces[2].from - (60 - 0.05)) < 1e-9 && d4 > 0, "сосед не исчезает");
            }
            Project u; u.strength = 0.3; u.samples["drums"] = 1; u.resetUnify(); check (u.samples.empty() && u.strength == 0.85 && u.limitDb == 12, "resetUnify");
        }
        out (bad == 0 ? "selftest passed" : "selftest FAILED"); return bad == 0 ? 0 : 4;
    }
    if (cmd == "eqtest")
    {   // чистота эквалайзера подгонки: синусы через кривую ±4 дБ — остаток после вычитания идеального синуса
        const double sr = 48000; const int n = (int) (sr * 3);
        Spectrum curve {}; for (int k = 0; k < kGrid; ++k) curve[(size_t) k] = 4.0 * std::sin (k * 0.37) * std::sin (k * 0.11);
        for (double f : { 60.0, 440.0, 1000.0, 3150.0, 9000.0 })
        {
            juce::AudioBuffer<float> b (1, n);
            for (int i = 0; i < n; ++i) b.setSample (0, i, (float) (0.5 * std::sin (2 * juce::MathConstants<double>::pi * f * i / sr)));
            applyEq (b, sr, curve);
            // МНК-синус на средней части
            double ss = 0, sc = 0, cc = 0, xs = 0, xc = 0; const int a0 = n / 4, a1 = 3 * n / 4;
            for (int i = a0; i < a1; ++i) { const double ph = 2 * juce::MathConstants<double>::pi * f * i / sr, s = std::sin (ph), c = std::cos (ph), x = b.getSample (0, i);
                ss += s * s; cc += c * c; sc += s * c; xs += x * s; xc += x * c; }
            const double det = ss * cc - sc * sc, A = (xs * cc - xc * sc) / det, B = (xc * ss - xs * sc) / det;
            double e = 0, p = 0;
            for (int i = a0; i < a1; ++i) { const double ph = 2 * juce::MathConstants<double>::pi * f * i / sr, y = A * std::sin (ph) + B * std::cos (ph), x = b.getSample (0, i); e += (x - y) * (x - y); p += y * y; }
            out ("  " + juce::String (f, 0) + " Hz: gain " + juce::String (20 * std::log10 (std::sqrt (A * A + B * B) / 0.5), 2) + " dB, residual " + juce::String (10 * std::log10 (e / p + 1e-30), 1) + " dB");
        }
        return 0;
    }
    if (cmd == "stress" && argc >= 5)
    {   // монтаж как у автора: разрезы где угодно (короткие куски тоже), перестановки, убранные инструменты, метки; затем сведение
        const juce::File root (argv[2]), cache (argv[3]), dst (argv[4]);
        const int seeds = argc >= 6 ? juce::String (argv[5]).getIntValue() : 5;
        std::vector<Version> vs;
        for (auto id : { "04A", "04B", "04C", "04D" })
        {
            auto v = Engine::scanVersion (root.getChildFile (id));
            if (! Engine::analyseVersion (v, cache, nullptr, err)) { out ("ERROR " + err); return 1; }
            vs.push_back (v);
        }
        int failed = 0;
        for (int seed = 1; seed <= seeds; ++seed)
        {
            juce::Random rnd (seed);
            Project p; p.versions = vs;
            for (auto& v : vs) p.pieces.push_back (p.makePiece (v.id, v.bars.front(), v.lengthSec));
            for (int k = 0; k < 14; ++k) p.split (rnd.nextInt ((int) p.pieces.size()), rnd.nextDouble() * p.tableLength());
            // короткие куски (меньше такта) и обрезки у края
            for (int k = 0; k < 3; ++k) { const double t0 = rnd.nextDouble() * p.tableLength(); p.split (p.pieceAt (t0), t0); p.split (p.pieceAt (t0 + 0.6), t0 + 0.6); }
            while (p.pieces.size() > 9) p.remove (rnd.nextInt ((int) p.pieces.size()));
            for (int k = 0; k < 5; ++k) p.move (rnd.nextInt ((int) p.pieces.size()), rnd.nextInt ((int) p.pieces.size()));
            p.pieces[(size_t) rnd.nextInt ((int) p.pieces.size())].off.insert ("guitar");
            p.setPart ({ 0, 1 }, "Verse 1");
            p.sample = rnd.nextInt ((int) p.pieces.size());
            juce::String e; juce::var r;
            const auto o = dst.getChildFile ("seed" + juce::String (seed));
            const bool ok = Engine::mixIntoStems (p, cache, o, nullptr, r, e);
            juce::String lens; for (auto& pc : p.pieces) lens << juce::String (pc.length(), 1) << " ";
            out ("seed " + juce::String (seed) + ": " + (ok ? "OK " + juce::String ((double) r["lengthSec"], 1) + " s, warnings " + juce::String (r["warnings"].getArray()->size()) : "FAIL " + e) + "   pieces " + lens);
            if (ok) for (auto& w : *r["warnings"].getArray()) out ("    ! " + w.toString());
            if (! ok) ++failed;
        }
        return failed == 0 ? 0 : 5;
    }
    if (cmd == "unifytest" && argc >= 6)
    {   // кусок-образец из версии A, следом кусок из B (тише и глуше) — после Unify громкость и спектр B должны подтянуться к A
        const juce::File va (argv[2]), vb (argv[3]), cache (argv[4]), dst (argv[5]);
        Project p;
        for (auto f : { va, vb }) { auto v = Engine::scanVersion (f); if (! Engine::analyseVersion (v, cache, nullptr, err)) { out ("ERROR " + err); return 1; } p.versions.push_back (v); }
        const auto& A = p.versions[0]; const auto& B = p.versions[1];
        p.pieces = { p.makePiece (A.id, A.bars[8], A.bars[20]), p.makePiece (B.id, B.bars[20], B.bars[36]) };
        p.sample = 0;
        for (int mode = 0; mode < 3; ++mode)
        {
            Project q = p; if (mode == 0) { q.matchEq = false; q.matchLoudness = false; } if (mode == 2) q.matchBy = "version";
            juce::var r; const auto o = dst.getChildFile ("mode" + juce::String (mode));
            if (! Engine::mixIntoStems (q, cache, o, nullptr, r, err)) { out ("ERROR " + err); return 1; }
            const double cut = (double) (*r["pieces"].getArray())[1]["at"];
            juce::String line = juce::String (mode == 0 ? "no unify      " : mode == 1 ? "unify piece   " : "unify version ");
            for (auto role : { "drums", "bass", "guitar", "vocals" })
            {
                juce::AudioBuffer<float> b; double sr = 0; juce::String e;
                if (! Stemmer::loadAudio (o.getChildFile (neuroName (role) + ".wav"), b, sr, e)) continue;
                const auto c = (juce::int64) (cut * sr);
                const double la = lufs (b, sr, 0, c), lb = lufs (b, sr, c, b.getNumSamples());
                const auto sa = normalised (ltas (b, sr, 0, c)), sb = normalised (ltas (b, sr, c, b.getNumSamples()));
                double hf = 0; int n = 0; for (int k = 0; k < kGrid; ++k) if (gridHz (k) >= 4000 && gridHz (k) <= 12000) { hf += sb[(size_t) k] - sa[(size_t) k]; ++n; }
                line << role << ": loud " << juce::String (lb - la, 1) << " dB, highs " << juce::String (hf / juce::jmax (1, n), 1) << " dB   ";
            }
            out (line);
        }
        return 0;
    }
    if (cmd == "unifyreport" && argc >= 5)
    {   // RIGOR: 5 кусков 04A–04D, образец — кусок 2 (04B). Для каждой роли и куска: громкость и спектр против образца до/после Unify.
        const juce::File root (argv[2]), cache (argv[3]), dst (argv[4]);
        Project p;
        for (auto id : { "04A", "04B", "04C", "04D" }) { auto v = Engine::scanVersion (root.getChildFile (id)); if (! Engine::analyseVersion (v, cache, nullptr, err)) { out ("ERROR " + err); return 1; } p.versions.push_back (v); }
        auto bar = [&] (int vi, int b) { const auto& bs = p.versions[(size_t) vi].bars; return bs[(size_t) juce::jlimit (0, (int) bs.size() - 1, b)]; };
        const int plan[][3] = { { 0, 0, 8 }, { 1, 8, 20 }, { 2, 20, 36 }, { 3, 36, 44 }, { 0, 44, 56 } };
        for (auto& q : plan) p.pieces.push_back (p.makePiece (p.versions[(size_t) q[0]].id, bar (q[0], q[1]), bar (q[0], q[2])));
        p.sample = 1;
        juce::var rep[2];
        for (int m = 0; m < 2; ++m)
        {
            Project q = p; if (m == 0) { q.matchEq = false; q.matchLoudness = false; }
            if (! Engine::mixIntoStems (q, cache, dst.getChildFile (m ? "after" : "before"), nullptr, rep[m], err)) { out ("ERROR " + err); return 1; }
        }
        out ("| Line | кусок | громкость к образцу, дБ: до → после | отклонение спектра от образца (среднее по 10 октавам), дБ: до → после |");
        out ("|---|---|---|---|");
        for (auto role : { "drums", "bass", "guitar", "vocals" })
        {
            juce::AudioBuffer<float> b[2]; double sr = 0; juce::String e;
            for (int m = 0; m < 2; ++m) Stemmer::loadAudio (dst.getChildFile (m ? "after" : "before").getChildFile (neuroName (role) + ".wav"), b[m], sr, e);
            auto& ps = *rep[0]["pieces"].getArray();
            auto region = [&] (int i) { return std::make_pair ((juce::int64) ((double) ps[i]["at"] * sr), (juce::int64) (((double) ps[i]["at"] + (double) ps[i]["dur"]) * sr)); };
            const auto rr = region (1);
            for (int i = 0; i < ps.size(); ++i)
            {
                if (i == 1) continue;
                const auto r = region (i);
                double ld[2], sd[2];
                for (int m = 0; m < 2; ++m)
                {
                    ld[m] = lufs (b[m], sr, r.first, r.second) - lufs (b[m], sr, rr.first, rr.second);
                    const auto oa = octaveTable (normalised (ltas (b[m], sr, r.first, r.second))), ob = octaveTable (normalised (ltas (b[m], sr, rr.first, rr.second)));
                    double s = 0; for (int k = 1; k < 10; ++k) s += std::abs (oa[(size_t) k] - ob[(size_t) k]); sd[m] = s / 9;
                }
                out ("| " + neuroName (role) + " | " + juce::String (i + 1) + " " + ps[i]["version"].toString() + " | " + juce::String (ld[0], 1) + " → " + juce::String (ld[1], 1)
                     + " | " + juce::String (sd[0], 1) + " → " + juce::String (sd[1], 1) + " |");
            }
        }
        return 0;
    }
    if (cmd == "verify" && argc >= 4)
    {
        juce::StringArray seams; if (argc >= 5) seams.addTokens (argv[4], ",", "");
        return verify (juce::File (argv[2]), juce::String (argv[3]).getDoubleValue(), seams);
    }
    if (cmd == "rigor" && argc >= 5)
    {
        const juce::File root (argv[2]), cache (argv[3]), dst (argv[4]);
        Project p;
        for (auto id : { "04A", "04B", "04C", "04D" })
        {
            auto v = Engine::scanVersion (root.getChildFile (id));
            if (! Engine::analyseVersion (v, cache, printer(), err)) { out ("ERROR " + err); return 1; }
            printVersion (v);
            p.versions.push_back (v);
        }
        // песня: вступление из 04A, куплет из 04B, припев из 04C, дальше 04D и конец из 04A (по тактам каждой версии)
        auto bar = [&] (int vi, int b) { const auto& bs = p.versions[(size_t) vi].bars; return bs[(size_t) juce::jlimit (0, (int) bs.size() - 1, b)]; };
        const int plan[][3] = { { 0, 0, 8 }, { 1, 8, 20 }, { 2, 20, 36 }, { 3, 36, 44 }, { 0, 44, 56 } };
        for (auto& q : plan) p.pieces.push_back (p.makePiece (p.versions[(size_t) q[0]].id, bar (q[0], q[1]), bar (q[0], q[2])));
        p.sample = 1;                       // образец звука всех ролей — куплет из 04B
        {   // метки частей на кусках
            const char* names[] = { "Intro", "Verse", "Chorus", "Bridge", "Outro" };
            for (int i = 0; i < (int) p.pieces.size(); ++i) p.setPart ({ i }, names[i]);
        }
        dst.createDirectory(); p.save (dst.getChildFile ("rigor_arranger.json"));
        juce::var r;
        if (! Engine::mixIntoStems (p, cache, dst, printer(), r, err)) { out ("ERROR " + err); return 1; }
        printReport (r);
        juce::StringArray seams; for (auto& pc : *r["pieces"].getArray()) if ((int) pc["n"] > 1) seams.add (pc["at"].toString());
        return verify (dst.getChildFile ("Neuro Drums.wav"), (double) r["songBpm"], seams);
    }
    out ("usage: tempo <project> <cache> | export <stems> <file> <mp3|wav24|wav16> | save <stems> <dest> <pattern> <song> | selftest");
    out ("usage: analyse <version> <cache> | render <project.json> <cache> <out> | rigor <rigor-root> <cache> <out> | verify <drums.wav> <bpm> [seams]");
    return 2;
}
