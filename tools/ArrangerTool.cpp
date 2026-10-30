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
#include <thread>

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
    if (cmd == "audit" && argc >= 7)
    {   // самопроверка движка (указание sound 29.10): крайние случаи на настоящих файлах, PASS/FAIL
        const juce::File rigor (juce::String::fromUTF8 (argv[2])), ruDir (juce::String::fromUTF8 (argv[3])), songs (juce::String::fromUTF8 (argv[4])),
                         cache (juce::String::fromUTF8 (argv[5])), dst (juce::String::fromUTF8 (argv[6]));
        int n = 0, bad = 0;
        auto check = [&] (bool c, const juce::String& what, const juce::String& detail = {})
        { ++n; if (! c) ++bad; out (juce::String (c ? "PASS  " : "FAIL  ") + what + (detail.isNotEmpty() ? "   [" + detail + "]" : juce::String())); };
        auto checkStems = [&] (const juce::File& folder, const juce::String& tag)
        {
            const auto j = juce::JSON::parse (folder.getChildFile ("dzod_stems.json").loadFileAsString());
            auto* a = j["stems"].getArray(); bool ok = a != nullptr && a->size() > 0; juce::int64 len = -1; double peak = -200; bool finite = true, sameLen = true, fmt = true;
            if (ok) for (auto& s : *a)
            {
                juce::AudioFormatManager fm; fm.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (folder.getChildFile (s["file"].toString())));
                if (r == nullptr) { ok = false; continue; }
                fmt &= r->sampleRate == 48000 && r->bitsPerSample == 24 && r->numChannels == 2;
                if (len < 0) len = r->lengthInSamples; else sameLen &= len == r->lengthInSamples;
                juce::AudioBuffer<float> b ((int) r->numChannels, (int) r->lengthInSamples); r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
                for (int c = 0; c < b.getNumChannels(); ++c) { auto* x = b.getReadPointer (c); for (int i = 0; i < b.getNumSamples(); ++i) if (! std::isfinite (x[i])) finite = false; }
                peak = juce::jmax (peak, peakDb (b));
            }
            check (ok, tag + ": dzod_stems.json and all Neuro WAV readable");
            check (fmt, tag + ": WAV 48 kHz 24-bit stereo");
            check (sameLen, tag + ": all stems the same length");
            check (finite, tag + ": no NaN / Inf");
            check (peak <= -0.99, tag + ": no stem above -1 dBFS", juce::String (peak, 2) + " dBFS");
            return j;
        };
        juce::String e; juce::var rep;

        // 1. русские имена, пробелы, MP3 44,1 кГц, моно, укороченный стем, .mid рядом
        juce::Array<juce::File> fs; for (auto& f : ruDir.findChildFiles (juce::File::findFiles, false)) fs.add (f);
        auto ru = Engine::versionFromFiles ("V2", fs);
        juce::StringArray roles; for (auto& s : ru.stems) roles.add (s.outRole);
        check (ru.stems.size() == 4 && roles.contains ("drums") && roles.contains ("bass") && roles.contains ("guitar") && roles.contains ("vocals"), "Russian names: 4 stems, roles by file name, .mid skipped", roles.joinIntoString (","));
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        const bool aok = Engine::analyseVersion (ru, cache, nullptr, e);
        check (aok, "Russian path with spaces, MP3 44.1k + mono WAV + FLAC: analysed", e);
        auto r04b = Engine::scanVersion (rigor.getChildFile ("04B")); Engine::analyseVersion (r04b, cache, nullptr, e);
        check (aok && std::abs (ru.bpm - r04b.bpm) < 0.6, "tempo of the converted copy = tempo of 04B", juce::String (ru.bpm, 2) + " vs " + juce::String (r04b.bpm, 2));
        check (aok && std::abs ((double) ru.bars.size() - (double) r04b.bars.size()) <= 1, "bars of the copy = bars of 04B", juce::String ((int) ru.bars.size()) + " vs " + juce::String ((int) r04b.bars.size()));
        auto ru2 = Engine::versionFromFiles ("V2", fs);
        const auto t1 = juce::Time::getMillisecondCounterHiRes();
        Engine::analyseVersion (ru2, cache, nullptr, e);
        const auto t2 = juce::Time::getMillisecondCounterHiRes();
        check (ru2.analysed && ru2.bars == ru.bars && std::abs (ru2.mixLufs - ru.mixLufs) < 0.01 && ru2.key.name() == ru.key.name(), "second opening from the cache = first", juce::String ((t1 - t0) / 1000.0, 1) + " s -> " + juce::String ((t2 - t1) / 1000.0, 2) + " s");

        // 2. сведение: 04A + копия + кусок за концом укороченной гитары
        auto r04a = Engine::scanVersion (rigor.getChildFile ("04A")); Engine::analyseVersion (r04a, cache, nullptr, e);
        Project p; p.versions = { r04a, ru };
        p.pieces = { p.makePiece ("04A", r04a.bars[0], r04a.bars[8]), p.makePiece ("V2", ru.bars[8], ru.bars[20]), p.makePiece ("V2", 100.0, 130.0) };
        p.sample = 0;
        const bool mok = Engine::mixIntoStems (p, cache, dst.getChildFile ("Сведение с пробелами"), nullptr, rep, e);
        check (mok, "mix: 3 slices, two variants with different sample rates, Russian output folder", e);
        if (mok)
        {
            checkStems (dst.getChildFile ("Сведение с пробелами"), "mix");
            check (rep["pieces"].getArray()->size() == 3, "report: 3 slices");
            juce::String w; for (auto& x : *rep["warnings"].getArray()) w << x.toString() << "; ";
            check (! w.containsAnyOf (juce::String::fromUTF8 ("абвгдеёжзийклмнопрстуфхцчшщъыьэюя")), "warnings in English", w);
            // экспорт и сохранение в путь с пробелами и кириллицей
            const auto ex = dst.getChildFile ("Экспорт песни").getChildFile ("Ригор итог.mp3");
            check (Engine::exportSong (dst.getChildFile ("Сведение с пробелами"), ex, "mp3", e) && ex.getSize() > 100000, "EXPORT SONG mp3 to a Russian path", e);
            const auto ex16 = ex.withFileExtension (".wav");
            check (Engine::exportSong (dst.getChildFile ("Сведение с пробелами"), ex16, "wav16", e) && ex16.getSize() > 100000, "EXPORT SONG wav16", e);
            juce::StringArray w2;
            check (Engine::saveStems (dst.getChildFile ("Сведение с пробелами"), dst.getChildFile ("Документы").getChildFile ("DZOD Songs"),
                                      juce::String::fromUTF8 ("{song}\\Stems\\{song} \xe2\x80\x94 Neuro {instrument}.wav"), juce::String::fromUTF8 ("Ригор"), w2, e)
                   && w2.size() == 4 && juce::File (w2[0]).existsAsFile(), "SAVE NEW STEMS: pattern with \\, Cyrillic song name, folders created", e);
        }
        juce::var r2;
        Project empty; check (! Engine::mixIntoStems (empty, cache, dst.getChildFile ("empty"), nullptr, r2, e) && e.isNotEmpty(), "empty table: clean error, no crash", e);
        std::atomic<bool> stop { true };
        Project pc = p; for (auto& v : pc.versions) v.analysed = false;
        check (! Engine::mixIntoStems (pc, cache, dst.getChildFile ("cancel"), nullptr, r2, e, &stop) && e == "Cancelled", "cancel: stops with 'Cancelled'", e);
        Spectrum sp {};
        check (Engine::pieceSpectrum (p, 1, "vocals", sp, e) && std::isfinite (sp[60]), "pieceSpectrum on the converted copy", e);

        // 3. Undo / Redo и сохранение после правок
        {
            Project q = p; History h;
            const auto before = juce::JSON::toString (q.editState());
            h.push (q); q.moveEdge (0, true, 1.5); h.push (q); q.setPart ({ 1 }, q.numberedLabel ({ 1 }, "Verse")); h.push (q); q.pieces[2].off.insert ("guitar");
            const auto after = juce::JSON::toString (q.editState());
            Project q2; q2.versions = q.versions; q2.fromVar (q.toVar(), e);
            check (juce::JSON::toString (q2.editState()) == after, "save / open keeps every edit (edges, labels, removed instruments)");
            h.undo (q); h.undo (q); h.undo (q);
            check (juce::JSON::toString (q.editState()) == before, "Undo x3 returns exactly to the start");
        }

        // 4. полные миксы песен с разметкой автора (без стема барабанов): разрез по частям, номера, сведение
        struct Mark { double a; const char* label; };
        auto song = [&] (const juce::String& folder, const juce::String& file, std::vector<Mark> marks, double endSec)
        {
            auto v = Engine::versionFromFiles ("S", { songs.getChildFile (folder).getChildFile (file) });
            const auto s0 = juce::Time::getMillisecondCounterHiRes();
            const bool ok = Engine::analyseVersion (v, cache, nullptr, e);
            check (ok && v.bpm > 60 && v.bpm < 220 && v.bars.size() > 20, folder + ": full mix without a drums stem analysed", juce::String (v.bpm, 1) + " BPM, " + juce::String ((int) v.bars.size()) + " bars, "
                   + juce::String ((juce::Time::getMillisecondCounterHiRes() - s0) / 1000.0, 1) + " s");
            if (! ok) return;
            Project q; q.versions = { v }; q.pieces = { q.makePiece ("S", 0.0, endSec) };
            for (size_t k = 1; k < marks.size(); ++k) { const double c = q.snapCut (marks[k].a); q.split (q.pieceAt (c), c); }
            check ((int) q.pieces.size() == (int) marks.size(), folder + ": cut at the author's parts on strong beats", juce::String ((int) q.pieces.size()) + " slices");
            juce::StringArray got, want;
            for (size_t k = 0; k < marks.size() && k < q.pieces.size(); ++k)
            {
                const auto type = Project::partType (juce::String::fromUTF8 (marks[k].label));
                const auto lab = q.numberedLabel ({ (int) k }, type);
                q.setPart ({ (int) k }, lab); got.add (lab); want.add (juce::String::fromUTF8 (marks[k].label));
            }
            check (got == want, folder + ": automatic part numbers = the author's", got.joinIntoString (", "));
            q.sample = 0;
            juce::var rr;
            const auto o = dst.getChildFile (folder);
            const auto m0 = juce::Time::getMillisecondCounterHiRes();
            const bool ok2 = Engine::mixIntoStems (q, cache, o, nullptr, rr, e);
            check (ok2, folder + ": mixed", e + " " + juce::String ((juce::Time::getMillisecondCounterHiRes() - m0) / 1000.0, 1) + " s");
            if (! ok2) return;
            auto j = checkStems (o, folder);
            juce::StringArray labels; int lastBar = 0; bool rising = true;
            if (auto* sc = j["sections"].getArray()) for (auto& x : *sc) { labels.add (x["label"].toString()); rising &= (int) x["startBar"] > lastBar; lastBar = (int) x["startBar"]; }
            check (labels == want && rising, folder + ": parts in dzod_stems.json for the Space ruler, bars rising", labels.joinIntoString (", "));
        };
        song ("crash-test-dummy", "CRASH_TEST_DUMMY_01.flac",
              { { 0, "Verse 1" }, { 26, "Chorus 1" }, { 51, "Verse 2" }, { 77, "Chorus 2" }, { 102, "Break 1" }, { 114, "Solo 1" }, { 127, "Verse 3" }, { 152, "Chorus 3" }, { 178, "Bridge 1" }, { 190, "Outro 1" } }, 205);
        song ("generate-the-day", "GENERATE_THE_DAY_06_01.flac",
              { { 0, "Intro 1" }, { 20, "Verse 1" }, { 48, "Pre-Chorus 1" }, { 65, "Chorus 1" }, { 92, "Break 1" }, { 102, "Verse 2" }, { 128, "Pre-Chorus 2" }, { 146, "Chorus 2" },
                { 180, "Bridge 1" }, { 206, "Solo 1" }, { 224, "Post-Chorus 1" }, { 245, "Outro 1" } }, 264);
        {   // предложение темпа: без явного образца Drums — медиана, а не темп первого куска
            auto v = Engine::versionFromFiles ("S", { songs.getChildFile ("generate-the-day").getChildFile ("GENERATE_THE_DAY_06_01.flac") });
            Engine::analyseVersion (v, cache, nullptr, e);
            Project q; q.versions = { v }; q.pieces = { q.makePiece ("S", 0, 20), q.makePiece ("S", 20, 130), q.makePiece ("S", 130, 245) }; q.sample = 0;
            const auto tp = Engine::analyseTempo (q);
            check (tp.drumsSample == 0 && std::abs (tp.proposed - tp.median) < 1e-9 && tp.proposed > 115, "tempo proposal: median when no Drums reference (not the intro)", juce::String (tp.proposed, 1) + " BPM");
            q.samples["drums"] = 0;
            check (Engine::analyseTempo (q).proposed == Engine::analyseTempo (q).drumsSample, "tempo proposal: explicit Drums reference wins");
        }
        {   // образец без инструмента: вокал подгоняется к самому длинному куску с вокалом
            Project q = p; q.pieces[0].off.insert ("vocals"); q.sample = 0;
            juce::var rr;
            const bool ok = Engine::mixIntoStems (q, cache, dst.getChildFile ("ref fallback"), nullptr, rr, e);
            bool vocalsMatched = false; juce::String refv;
            if (ok) for (auto& x : *rr["matching"].getArray()) if (x["role"].toString() == "vocals") { vocalsMatched = true; refv = x["sample"].toString(); }
            check (ok && vocalsMatched, "reference slice without vocals: vocals still unified (longest slice with vocals)", refv);
        }
        {   // параллельный разбор двух вариантов в один кэш (как пул задач Studio)
            const auto c2 = dst.getChildFile ("parallel cache"); c2.deleteRecursively();
            auto va = Engine::scanVersion (rigor.getChildFile ("04C")), vb = Engine::scanVersion (rigor.getChildFile ("04D"));
            juce::String ea, eb; bool oa = false, ob = false;
            std::thread ta ([&] { oa = Engine::analyseVersion (va, c2, nullptr, ea); }), tb ([&] { ob = Engine::analyseVersion (vb, c2, nullptr, eb); });
            ta.join(); tb.join();
            auto va2 = Engine::scanVersion (rigor.getChildFile ("04C")); Engine::analyseVersion (va2, cache, nullptr, e);
            check (oa && ob && va.bars == va2.bars, "two variants analysed in parallel into one cache = one by one", ea + eb);
        }
        out ("audit: " + juce::String (n) + " checks, " + juce::String (bad) + " failed");
        return bad == 0 ? 0 : 6;
    }
    if (cmd == "regress" && argc >= 5)
    {   // регрессии: жалобы автора, которые были исправлены, — каждая своим тестом (правило sound 30.10)
        const juce::File rigor (juce::String::fromUTF8 (argv[2])), cache (juce::String::fromUTF8 (argv[3])), dst (juce::String::fromUTF8 (argv[4]));
        int n = 0, bad = 0;
        auto check = [&] (const char* id, bool c, const juce::String& what, const juce::String& detail = {})
        { ++n; if (! c) ++bad; out (juce::String (c ? "PASS  " : "FAIL  ") + id + "  " + what + (detail.isNotEmpty() ? "   [" + detail + "]" : juce::String())); };
        juce::String e;
        std::vector<Version> vs;
        for (auto id : { "04A", "04B", "04C", "04D" }) { auto v = Engine::scanVersion (rigor.getChildFile (id)); Engine::analyseVersion (v, cache, nullptr, e); vs.push_back (v); }
        Project p; p.versions = vs;
        auto bar = [&] (int vi, int b) { const auto& bs = vs[(size_t) vi].bars; return bs[(size_t) juce::jlimit (0, (int) bs.size() - 1, b)]; };
        const int plan[][3] = { { 0, 0, 8 }, { 1, 8, 20 }, { 2, 20, 36 }, { 3, 36, 44 }, { 0, 44, 56 } };
        for (auto& q : plan) p.pieces.push_back (p.makePiece (vs[(size_t) q[0]].id, bar (q[0], q[1]), bar (q[0], q[2])));
        p.sample = 1; p.samples["drums"] = 1;    // темп песни = темп куска 04B — он почти не растягивается
        if (juce::SystemStats::getEnvironmentVariable ("DZOD_REGRESS_WSOLA", {}).isNotEmpty()) p.stretcher = "wsola";   // проверка самого теста
        juce::var rep; const auto o = dst.getChildFile ("rigor");
        const bool ok = Engine::mixIntoStems (p, cache, o, nullptr, rep, e);

        // R1 28.10 «металлические стеклянные призвуки» — WSOLA: растяжка по умолчанию фазовая, кусок 04B без растяжки почти не отличается от исходника
        check ("R1", Project().stretcher == "phase", "default stretcher is the phase vocoder (not WSOLA)");
        if (ok)
        {
            juce::AudioBuffer<float> outB, srcB; double sr = 0, sr2 = 0;
            Stemmer::loadAudio (o.getChildFile ("Neuro Drums.wav"), outB, sr, e);
            Stemmer::loadAudio (vs[1].stems[0].file.getSiblingFile (vs[1].stems[0].file.getFileName().replace ("(Bass)", "(Drums)")), srcB, sr2, e);
            const auto& pc = *rep["pieces"].getArray();
            const double at = (double) pc[1]["at"], from = (double) pc[1]["srcFrom"];
            // спектральное расстояние по кадрам 2048 (сдвиг ±10 мс), медиана
            juce::dsp::FFT fft (11); std::vector<float> fa (4096), fb (4096); std::vector<double> d;
            auto mag = [&] (const juce::AudioBuffer<float>& b, juce::int64 s0, std::vector<float>& f)
            { std::fill (f.begin(), f.end(), 0.0f); for (int i = 0; i < 2048; ++i) { const auto k = s0 + i; if (k >= 0 && k < b.getNumSamples()) f[(size_t) i] = (b.getSample (0, (int) k) + b.getSample (1, (int) k)) * 0.5f * (0.5f - 0.5f * std::cos (2 * juce::MathConstants<float>::pi * i / 2048)); }
              fft.performFrequencyOnlyForwardTransform (f.data()); };
            for (double tt = at + 1; tt < at + 9; tt += 0.1)
            {
                mag (outB, (juce::int64) (tt * sr), fa);
                double best = 1e9;
                for (int lag = -480; lag <= 480; lag += 24)
                {
                    mag (srcB, (juce::int64) ((from + (tt - at)) * sr2) + lag, fb);
                    float mx = 0; for (int k = 0; k < 1024; ++k) mx = juce::jmax (mx, fa[(size_t) k] + fb[(size_t) k]);
                    double s = 0; int m = 0;
                    for (int k = 1; k < 1024; ++k) if (fa[(size_t) k] + fb[(size_t) k] > 1e-4f * mx) { const double x = 20 * std::log10 ((fa[(size_t) k] + 1e-9) / (fb[(size_t) k] + 1e-9)); s += x * x; ++m; }
                    if (m > 0) best = juce::jmin (best, std::sqrt (s / m));
                }
                if (best < 1e8) d.push_back (best);
            }
            std::sort (d.begin(), d.end());
            const double med = d.empty() ? 99 : d[d.size() / 2];
            check ("R1", med < 3.0, "drums of an unstretched slice ~ the source (no metallic comb)", "spectral distance median " + juce::String (med, 2) + " dB, WSOLA gave 4.0");
        }
        // R2 28.10 ложная тревога «разная тональность» на стыке 29,4 с (автор: тональность одна)
        juce::String keyWarn; if (ok) for (auto& w : *rep["warnings"].getArray()) if (w.toString().contains ("keys may differ")) keyWarn << w.toString();
        check ("R2", ok && keyWarn.isEmpty(), "RIGOR: no false 'keys may differ' warning", keyWarn);
        // R7 29.10 п. 8 «непонятно, выравнивает каждый удар или кусок» — ALIGN по долям: доли на сетке
        if (ok)
        {
            Stemmer st; dzod::stemmer::Job job; job.drums = o.getChildFile ("Neuro Drums.wav"); job.full = job.drums; job.projectBpm = (double) rep["songBpm"]; job.straightenGrid = true;
            st.analyse (job, nullptr);
            const auto& g = st.analysis().grid; const double beat = 60.0 / (double) rep["songBpm"];
            std::vector<double> ph; for (double x : g) { double r = std::fmod (x, beat); if (r > beat / 2) r -= beat; ph.push_back (r); }
            auto s2 = ph; std::sort (s2.begin(), s2.end()); const double off = s2.empty() ? 0 : s2[s2.size() / 2];
            std::vector<double> dev; for (double x : ph) dev.push_back (std::abs (x - off) * 1000); std::sort (dev.begin(), dev.end());
            const double p90 = dev.empty() ? 99 : dev[dev.size() * 9 / 10];
            check ("R7", p90 < 10.0, "ALIGN puts every beat on the grid (p90 < 10 ms)", juce::String (p90, 1) + " ms");
        }
        // R3/R4 29.10 п. 9–11 «Unify не работает, Mix и Save ничего не делают» — обрезки короче такта
        {
            Project q = p; const double t0 = q.tableStartOf (2) + 3.0; q.split (q.pieceAt (t0), t0); q.split (q.pieceAt (t0 + 0.5), t0 + 0.5);
            juce::var r2; const bool ok2 = Engine::mixIntoStems (q, cache, dst.getChildFile ("sliver"), nullptr, r2, e);
            check ("R4", ok2, "a slice shorter than a bar does not stop MIX / UNIFY / SAVE", e);
        }
        // R3 29.10 п. 9 «тихий глухой кусок остался тихим и глухим»
        {
            const auto dull = dst.getChildFile ("dull 04C"); dull.deleteRecursively(); dull.createDirectory();
            Spectrum curve {}; for (int k = 0; k < kGrid; ++k) curve[(size_t) k] = gridHz (k) > 3000 ? -6.0 : 0.0;
            for (auto& s : vs[2].stems)
            {
                juce::AudioBuffer<float> b; double sr = 0; Stemmer::loadAudio (s.file, b, sr, e);
                applyEq (b, sr, curve); b.applyGain (0.5f);
                juce::WavAudioFormat wav; const auto f = dull.getChildFile (s.file.getFileNameWithoutExtension() + ".wav");
                std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), sr, (unsigned) b.getNumChannels(), 24, {}, 0));
                if (w) w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
            }
            auto vd = Engine::scanVersion (dull); vd.id = "dull"; Engine::analyseVersion (vd, cache, nullptr, e);
            Project q; q.versions = { vs[1], vd };
            q.pieces = { q.makePiece (vs[1].id, vs[1].bars[8], vs[1].bars[20]), q.makePiece ("dull", vd.bars[20], vd.bars[36]) }; q.sample = 0;
            juce::var r3; const auto od = dst.getChildFile ("unify"), on = dst.getChildFile ("no unify");
            const bool ok3 = Engine::mixIntoStems (q, cache, od, nullptr, r3, e);
            Project qn = q; qn.matchEq = false; qn.matchLoudness = false; juce::var r4;
            Engine::mixIntoStems (qn, cache, on, nullptr, r4, e);
            double worstL = 0, worstH = 1e9;
            if (ok3)
            {
                const double cut = (double) (*r3["pieces"].getArray())[1]["at"];
                for (auto role : { "drums", "guitar", "vocals" })
                {
                    juce::AudioBuffer<float> b, bn; double sr = 0; Stemmer::loadAudio (od.getChildFile (neuroName (role) + ".wav"), b, sr, e); Stemmer::loadAudio (on.getChildFile (neuroName (role) + ".wav"), bn, sr, e);
                    const auto c = (juce::int64) (cut * sr);
                    worstL = juce::jmax (worstL, std::abs (lufs (b, sr, c, b.getNumSamples()) - lufs (b, sr, 0, c)));
                    // верхи: насколько Unify поднял 4–12 кГц глухого куска (было срезано на 6 дБ)
                    const auto su = ltas (b, sr, c, b.getNumSamples()), sn = ltas (bn, sr, c, bn.getNumSamples());
                    const auto lu = lufs (b, sr, c, b.getNumSamples()), ln = lufs (bn, sr, c, bn.getNumSamples());
                    double hf = 0; int m = 0; for (int k = 0; k < kGrid; ++k) if (gridHz (k) >= 4000 && gridHz (k) <= 12000) { hf += (su[(size_t) k] - lu) - (sn[(size_t) k] - ln); ++m; }
                    worstH = juce::jmin (worstH, hf / juce::jmax (1, m));
                }
            }
            check ("R3", ok3 && worstL < 2.0, "Unify brings a slice 6 dB quieter back to the reference loudness (+-2 dB)", juce::String (worstL, 1) + " dB");
            check ("R3", ok3 && worstH > 3.0, "Unify brings back the highs of a dull slice (4-12 kHz cut by 6 dB: at least +3 dB back)", "+" + juce::String (worstH, 1) + " dB");
        }
        out ("regressions: " + juce::String (n - bad) + "/" + juce::String (n) + " passed");
        return bad == 0 ? 0 : 7;
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
