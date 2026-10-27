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
        out (bad == 0 ? "selftest passed" : "selftest FAILED"); return bad == 0 ? 0 : 4;
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
