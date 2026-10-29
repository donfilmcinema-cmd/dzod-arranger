#include "Arranger.h"
#include "../third_party/stemmer/Mp3Writer.h"
#include "Stretch.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <thread>

namespace dzod
{
namespace arranger
{
using stemmer::Stemmer;

namespace
{
constexpr double kOutRate = 48000.0;

bool loadAt (const juce::File& f, double rate, juce::AudioBuffer<float>& out, juce::String& err)
{
    double sr = 0;
    if (! Stemmer::loadAudio (f, out, sr, err)) return false;
    if (rate > 0 && std::abs (sr - rate) > 0.5) out = Stemmer::resample (out, sr, rate);
    if (out.getNumChannels() == 1) { out.setSize (2, out.getNumSamples(), true); out.copyFrom (1, 0, out, 0, 0, out.getNumSamples()); }
    return true;
}

bool harmonicRole (const juce::String& r)
{
    return r == "bass" || r == "guitar" || r == "keys" || r == "synth" || r == "strings" || r == "brass" || r == "woodwinds";
}

juce::var arr (const std::vector<double>& v) { juce::Array<juce::var> a; for (double x : v) a.add (x); return a; }
double num (const juce::var& v, double d = 0) { return v.isVoid() ? d : (double) v; }
juce::String fmt (double v, int d = 1) { return juce::String (v, d); }

bool isCancelled (const std::atomic<bool>* c) { return c != nullptr && c->load(); }

// n задач на все ядра (растяжка стемов): каждая задача — свой индекс
void parallelFor (size_t n, const std::function<void (size_t)>& f)
{
    const size_t threads = juce::jlimit ((size_t) 1, (size_t) 8, (size_t) juce::jmax (1u, std::thread::hardware_concurrency()));
    if (n <= 1 || threads == 1) { for (size_t i = 0; i < n; ++i) f (i); return; }
    std::atomic<size_t> next { 0 };
    std::vector<std::thread> pool;
    for (size_t k = 0; k < juce::jmin (threads, n); ++k)
        pool.emplace_back ([&] { for (size_t i; (i = next++) < n;) f (i); });
    for (auto& th : pool) th.join();
}

// равномощный переход: 0..1 -> вход/выход
float fadeIn (double t) { t = juce::jlimit (0.0, 1.0, t); return (float) std::sin (t * juce::MathConstants<double>::halfPi); }
float fadeOut (double t) { t = juce::jlimit (0.0, 1.0, t); return (float) std::cos (t * juce::MathConstants<double>::halfPi); }
} // namespace

const juce::StringArray& outRoles()
{
    static const juce::StringArray r { "drums", "bass", "guitar", "keys", "synth", "backs", "vocals", "other" };
    return r;
}

juce::String outRoleOf (const juce::String& r)
{
    if (r == "drums" || r == "bass" || r == "guitar" || r == "keys" || r == "synth" || r == "backs" || r == "vocals") return r;
    return "other";    // percussion, strings, brass, woodwinds, fx, other
}

juce::String neuroName (const juce::String& o)
{
    static const std::map<juce::String, juce::String> n { { "drums", "Neuro Drums" }, { "bass", "Neuro Bass" }, { "guitar", "Neuro Guitar" },
        { "keys", "Neuro Keys" }, { "synth", "Neuro Synth" }, { "backs", "Neuro Backs" }, { "vocals", "Neuro Vocal" }, { "other", "Neuro Other" } };
    auto it = n.find (o); return it != n.end() ? it->second : "Neuro " + o;
}

//==============================================================================
double Version::snapToBar (double sec) const
{
    const auto& b = bars.empty() ? grid : bars;
    if (b.empty()) return sec;
    double best = b.front();
    for (double x : b) if (std::abs (x - sec) < std::abs (best - sec)) best = x;
    return best;
}

static double snapTo (const std::vector<double>& b, double sec)
{
    if (b.empty()) return sec;
    double best = b.front();
    for (double x : b) if (std::abs (x - sec) < std::abs (best - sec)) best = x;
    return best;
}

double Version::localBpm (double from, double to) const
{
    std::vector<double> g; for (double t : grid) if (t >= from - 1e-3 && t <= to + 1e-3) g.push_back (t);
    if (g.size() < 3) return bpm;
    return 60.0 * (double) (g.size() - 1) / (g.back() - g.front());
}

bool Version::hasRole (const juce::String& o) const
{
    for (auto& s : stems) if (s.outRole == o && ! s.silent) return true;
    return false;
}

Version* Project::findVersion (const juce::String& id)
{
    for (auto& v : versions) if (v.id == id) return &v;
    return nullptr;
}

const Version* Project::findVersion (const juce::String& id) const
{
    for (auto& v : versions) if (v.id == id) return &v;
    return nullptr;
}

double Project::strengthFor (const juce::String& o) const
{
    auto it = strengths.find (o);
    return it != strengths.end() ? juce::jlimit (0.0, 1.0, it->second) : strength;
}

bool Project::isOn (const Piece& pc, const juce::String& o) const
{
    const auto* v = findVersion (pc.version);
    if (v == nullptr || ! v->hasRole (o)) return false;
    return v->off.count (o) == 0 && pc.off.count (o) == 0;
}

double Project::limitFor (const juce::String& o) const
{
    auto it = limits.find (o);
    return it != limits.end() ? juce::jlimit (0.0, 12.0, it->second) : limitDb;
}

double Project::tableLength() const { double s = 0; for (auto& p : pieces) s += p.length(); return s; }

double Project::tableStartOf (int i) const
{
    double s = 0; for (int k = 0; k < i && k < (int) pieces.size(); ++k) s += pieces[(size_t) k].length();
    return s;
}

int Project::pieceAt (double t) const
{
    double s = 0;
    for (size_t i = 0; i < pieces.size(); ++i) { if (t >= s && t < s + pieces[i].length()) return (int) i; s += pieces[i].length(); }
    return -1;
}

bool Project::split (int i, double t)
{
    if (i < 0 || i >= (int) pieces.size()) return false;
    const double cut = pieces[(size_t) i].from + (t - tableStartOf (i));
    auto a = pieces[(size_t) i];
    if (cut <= a.from + 0.05 || cut >= a.to - 0.05) return false;      // не режем у самого края
    auto b = a; a.to = cut; b.from = cut; b.id = nextId++;      // метка части (part) — у обеих половин
    pieces[(size_t) i] = a;
    pieces.insert (pieces.begin() + i + 1, b);
    for (auto& [r, s] : samples) if (s > i) ++s;
    if (sample > i) ++sample;
    return true;
}

int Project::splitAll (double t, const std::vector<int>& only)
{
    // на столе куски идут друг за другом, под курсором всегда один кусок; «выделенные» — только если он среди них
    const int i = pieceAt (t);
    if (i < 0) return 0;
    if (! only.empty() && std::find (only.begin(), only.end(), i) == only.end()) return 0;
    return split (i, t) ? 1 : 0;
}

double Project::snapCut (double tt) const
{
    const int pi = pieceAt (tt);
    if (pi < 0) return tt;
    const auto& pc = pieces[(size_t) pi];
    const auto* v = findVersion (pc.version);
    if (v == nullptr) return tt;
    const auto& g = snapToBars ? (v->bars.empty() ? v->grid : v->bars) : v->grid;
    if (g.empty()) return tt;
    const double start = tableStartOf (pi);
    return start + (snapTo (g, pc.from + (tt - start)) - pc.from);
}

void Project::remove (int i)
{
    if (i < 0 || i >= (int) pieces.size()) return;
    const int part = pieces[(size_t) i].part;
    pieces.erase (pieces.begin() + i);
    if (part != 0) { bool used = false; for (auto& p : pieces) if (p.part == part) used = true; if (! used) parts.erase (part); }
    for (auto it = samples.begin(); it != samples.end();)
    {
        if (it->second == i) it = samples.erase (it);
        else { if (it->second > i) --it->second; ++it; }
    }
    if (sample == i) sample = -1; else if (sample > i) --sample;
}

void Project::move (int i, int to)
{
    if (i < 0 || i >= (int) pieces.size()) return;
    to = juce::jlimit (0, (int) pieces.size() - 1, to);
    if (i == to) return;
    auto pc = pieces[(size_t) i];
    pieces.erase (pieces.begin() + i);
    pieces.insert (pieces.begin() + to, pc);
    auto remap = [&] (int s) { if (s == i) return to; if (i < to && s > i && s <= to) return s - 1; if (to < i && s >= to && s < i) return s + 1; return s; };
    for (auto& [r, s] : samples) s = remap (s);
    sample = sample >= 0 ? remap (sample) : -1;
}

Piece Project::makePiece (const juce::String& version, double from, double to)
{
    Piece p; p.version = version; p.from = from; p.to = to; p.id = nextId++;
    return p;
}

int Project::indexOf (int id) const
{
    for (size_t i = 0; i < pieces.size(); ++i) if (pieces[i].id == id) return (int) i;
    return -1;
}

int Project::setPart (const std::vector<int>& idx, const juce::String& label)
{
    int part = 0;
    if (label.isNotEmpty()) { part = 1; while (parts.count (part)) ++part; parts[part] = label; }
    std::set<int> was;
    for (int i : idx) if (i >= 0 && i < (int) pieces.size()) { was.insert (pieces[(size_t) i].part); pieces[(size_t) i].part = part; }
    for (int w : was) { bool used = false; for (auto& p : pieces) if (p.part == w) used = true; if (w != 0 && ! used) parts.erase (w); }
    return part;
}

juce::String Project::partType (const juce::String& label)
{
    auto s = label.trim();
    while (s.isNotEmpty() && (juce::CharacterFunctions::isDigit (s.getLastCharacter()) || s.getLastCharacter() == ' ')) s = s.dropLastCharacters (1);
    return s;
}

juce::String Project::numberedLabel (const std::vector<int>& idx, const juce::String& type) const
{
    if (idx.empty()) return type;
    double from = 1e12; juce::String ver;
    for (int i : idx) if (i >= 0 && i < (int) pieces.size()) { if (pieces[(size_t) i].from < from) { from = pieces[(size_t) i].from; ver = pieces[(size_t) i].version; } }
    // части того же типа в той же версии раньше по исходнику (одна часть = одна метка)
    std::set<int> earlier;
    for (auto& p : pieces)
        if (p.version == ver && p.part != 0 && parts.count (p.part) && p.from < from - 0.05 && partType (parts.at (p.part)).equalsIgnoreCase (type)
            && std::find_if (idx.begin(), idx.end(), [&] (int i) { return i >= 0 && i < (int) pieces.size() && pieces[(size_t) i].id == p.id; }) == idx.end())
            earlier.insert (p.part);
    return type + " " + juce::String ((int) earlier.size() + 1);
}

bool Project::setEdges (int i, double from, double to)
{
    if (i < 0 || i >= (int) pieces.size()) return false;
    auto& p = pieces[(size_t) i];
    double len = 1e9; if (auto* v = findVersion (p.version)) if (v->lengthSec > 0) len = v->lengthSec;
    from = juce::jlimit (0.0, len, from); to = juce::jlimit (0.0, len, to);
    if (to <= from + 0.01) return false;
    p.from = from; p.to = to;
    return true;
}

bool Project::extendGroup (const std::vector<int>& idx, double dtStart, double dtEnd)
{
    if (idx.empty()) return false;
    const int first = *std::min_element (idx.begin(), idx.end()), last = *std::max_element (idx.begin(), idx.end());
    if (first < 0 || last >= (int) pieces.size()) return false;
    bool ok = true;
    if (dtStart != 0) ok &= setEdges (first, pieces[(size_t) first].from - dtStart, pieces[(size_t) first].to);
    if (dtEnd != 0) ok &= setEdges (last, pieces[(size_t) last].from, pieces[(size_t) last].to + dtEnd);
    return ok;
}

double Project::moveEdge (int i, bool right, double dt, double minLen)
{
    if (i < 0 || i >= (int) pieces.size() || dt == 0) return 0;
    auto lenOf = [&] (const Piece& p) { if (auto* v = findVersion (p.version)) if (v->lengthSec > 0) return v->lengthSec; return 1e9; };
    // граница справа от куска k = между k и k+1 (если k+1 есть)
    const int a = right ? i : i - 1, b = right ? i + 1 : i;     // слева и справа от границы
    double lo = -1e9, hi = 1e9;
    if (a >= 0)
    {
        auto& A = pieces[(size_t) a];
        lo = juce::jmax (lo, A.from + minLen - A.to);           // A не короче minLen
        hi = juce::jmin (hi, lenOf (A) - A.to);                 // A не дальше конца файла
    }
    if (b < (int) pieces.size())
    {
        auto& B = pieces[(size_t) b];
        lo = juce::jmax (lo, -B.from);                          // B не раньше начала файла
        hi = juce::jmin (hi, B.to - minLen - B.from);           // B не короче minLen
    }
    const double d = juce::jlimit (juce::jmin (0.0, lo), juce::jmax (0.0, hi), dt);
    if (d == 0) return 0;
    if (a >= 0) pieces[(size_t) a].to += d;
    if (b < (int) pieces.size()) pieces[(size_t) b].from += d;
    return d;
}

void Project::renamePart (int part, const juce::String& label)
{
    if (parts.count (part)) parts[part] = label;
}

std::vector<Project::Section> Project::sections() const
{
    std::vector<Section> r;
    for (int i = 0; i < (int) pieces.size(); ++i)
    {
        const int pt = pieces[(size_t) i].part;
        if (pt == 0 || ! parts.count (pt)) continue;
        if (! r.empty() && r.back().lastPiece == i - 1 && pieces[(size_t) i - 1].part == pt) { r.back().lastPiece = i; continue; }
        r.push_back ({ parts.at (pt), i, i });
    }
    return r;
}

static juce::var offVar (const std::set<juce::String>& s) { juce::Array<juce::var> a; for (auto& r : s) a.add (r); return a; }
static std::set<juce::String> offSet (const juce::var& a) { std::set<juce::String> s; if (auto* x = a.getArray()) for (auto& r : *x) s.insert (r.toString()); return s; }
static juce::var mapVar (const std::map<juce::String, double>& m) { auto* o = new juce::DynamicObject(); for (auto& [k, v] : m) o->setProperty (k, v); return juce::var (o); }

juce::var Project::editState() const
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("songBpm", songBpm); root->setProperty ("crossfadeMs", crossfadeMs);
    root->setProperty ("strength", strength); root->setProperty ("limitDb", limitDb);
    root->setProperty ("matchEq", matchEq); root->setProperty ("matchLoudness", matchLoudness); root->setProperty ("snapToBars", snapToBars); root->setProperty ("stretcher", stretcher); root->setProperty ("matchBy", matchBy);
    root->setProperty ("sample", sample); root->setProperty ("nextId", nextId);
    auto* sm = new juce::DynamicObject(); for (auto& [k, v] : samples) sm->setProperty (k, v);
    root->setProperty ("samples", juce::var (sm));
    root->setProperty ("strengths", mapVar (strengths));
    root->setProperty ("limits", mapVar (limits));
    auto* pt = new juce::DynamicObject(); for (auto& [k, v] : parts) pt->setProperty (juce::String (k), v);
    root->setProperty ("parts", juce::var (pt));
    auto* vo = new juce::DynamicObject(); for (auto& v : versions) vo->setProperty (v.id, offVar (v.off));
    root->setProperty ("versionOff", juce::var (vo));
    juce::Array<juce::var> ps;
    for (auto& p : pieces)
    {
        auto* o = new juce::DynamicObject(); o->setProperty ("version", p.version); o->setProperty ("from", p.from); o->setProperty ("to", p.to);
        o->setProperty ("id", p.id); o->setProperty ("part", p.part); o->setProperty ("off", offVar (p.off));
        ps.add (juce::var (o));
    }
    root->setProperty ("pieces", ps);
    return juce::var (root);
}

void Project::applyEditState (const juce::var& v)
{
    songBpm = num (v["songBpm"]); crossfadeMs = num (v["crossfadeMs"], 10); strength = num (v["strength"], 0.85); limitDb = num (v["limitDb"], 12);
    matchEq = v["matchEq"].isVoid() || (bool) v["matchEq"]; matchLoudness = v["matchLoudness"].isVoid() || (bool) v["matchLoudness"];
    snapToBars = v["snapToBars"].isVoid() || (bool) v["snapToBars"];
    stretcher = v["stretcher"].isVoid() ? juce::String ("phase") : v["stretcher"].toString();
    matchBy = v["matchBy"].isVoid() ? juce::String ("piece") : v["matchBy"].toString();
    sample = (int) num (v["sample"], -1); nextId = (int) num (v["nextId"], 1);
    samples.clear(); strengths.clear(); limits.clear(); parts.clear(); pieces.clear();
    if (auto* sm = v["samples"].getDynamicObject()) for (auto& p : sm->getProperties()) samples[p.name.toString()] = (int) p.value;
    if (auto* sg = v["strengths"].getDynamicObject()) for (auto& p : sg->getProperties()) strengths[p.name.toString()] = (double) p.value;
    if (auto* sg = v["limits"].getDynamicObject()) for (auto& p : sg->getProperties()) limits[p.name.toString()] = (double) p.value;
    if (auto* pt = v["parts"].getDynamicObject()) for (auto& p : pt->getProperties()) parts[p.name.toString().getIntValue()] = p.value.toString();
    if (auto* vo = v["versionOff"].getDynamicObject()) for (auto& ver : versions) ver.off = offSet (vo->getProperty (ver.id));
    if (auto* ps = v["pieces"].getArray())
        for (auto& x : *ps)
        {
            Piece pc; pc.version = x["version"].toString(); pc.from = num (x["from"]); pc.to = num (x["to"]);
            pc.id = (int) num (x["id"]); pc.part = (int) num (x["part"]); pc.off = offSet (x["off"]);
            if (pc.id <= 0) pc.id = nextId++;
            nextId = juce::jmax (nextId, pc.id + 1);
            pieces.push_back (pc);
        }
}

void History::push (const Project& p)
{
    back.push_back (juce::JSON::toString (p.editState(), true));
    if (back.size() > limit) back.erase (back.begin());
    fwd.clear();
}

bool History::undo (Project& p)
{
    if (back.empty()) return false;
    fwd.push_back (juce::JSON::toString (p.editState(), true));
    p.applyEditState (juce::JSON::parse (back.back())); back.pop_back();
    return true;
}

bool History::redo (Project& p)
{
    if (fwd.empty()) return false;
    back.push_back (juce::JSON::toString (p.editState(), true));
    p.applyEditState (juce::JSON::parse (fwd.back())); fwd.pop_back();
    return true;
}

juce::StringArray Project::rolesInSong() const
{
    juce::StringArray r;
    for (auto& o : outRoles()) for (auto& p : pieces) if (isOn (p, o)) { r.add (o); break; }
    return r;
}

int Project::sampleFor (const juce::String& o) const
{
    auto it = samples.find (o);
    if (it != samples.end() && it->second >= 0 && it->second < (int) pieces.size()) return it->second;
    return sample >= 0 && sample < (int) pieces.size() ? sample : -1;
}

juce::var Project::toVar() const
{
    auto root = editState();
    auto* o = root.getDynamicObject();
    o->setProperty ("format", 2);
    o->setProperty ("app", "DZOD Arranger");
    juce::Array<juce::var> vs;
    for (auto& v : versions)
    {
        auto* vo = new juce::DynamicObject();
        vo->setProperty ("id", v.id); vo->setProperty ("folder", v.folder.getFullPathName());
        juce::Array<juce::var> st;
        for (auto& s : v.stems) { auto* so = new juce::DynamicObject(); so->setProperty ("file", s.file.getFullPathName()); so->setProperty ("role", s.role); st.add (juce::var (so)); }
        vo->setProperty ("stems", st);
        vo->setProperty ("off", offVar (v.off));
        vs.add (juce::var (vo));
    }
    o->setProperty ("versions", vs);
    return root;
}

bool Project::fromVar (const juce::var& v, juce::String& error)
{
    if (! v.isObject() || v["versions"].getArray() == nullptr) { error = juce::String ("Not a DZOD Arranger project"); return false; }
    *this = Project();
    for (auto& x : *v["versions"].getArray())
    {
        Version ver; ver.id = x["id"].toString(); ver.folder = juce::File (x["folder"].toString());
        if (auto* st = x["stems"].getArray())
            for (auto& s : *st) { VersionStem vs; vs.file = juce::File (s["file"].toString()); vs.role = s["role"].toString(); vs.outRole = outRoleOf (vs.role); ver.stems.push_back (vs); }
        else ver = Engine::scanVersion (ver.folder);
        ver.off = offSet (x["off"]);
        versions.push_back (ver);
    }
    const auto vo = v["versionOff"];
    applyEditState (v);
    if (vo.isVoid()) for (size_t i = 0; i < versions.size(); ++i) versions[i].off = offSet ((*v["versions"].getArray())[(int) i]["off"]);
    return true;
}

bool Project::save (const juce::File& f) const { return f.replaceWithText (juce::JSON::toString (toVar())); }

bool Project::load (const juce::File& f, juce::String& error)
{
    const auto v = juce::JSON::parse (f.loadFileAsString());
    return fromVar (v, error);
}

//==============================================================================
Version Engine::scanVersion (const juce::File& folder)
{
    Version v; v.id = folder.getFileName(); v.folder = folder;
    auto files = folder.findChildFiles (juce::File::findFiles, false, "*.flac;*.wav;*.aif;*.aiff;*.mp3");
    files.sort();
    for (auto& f : files)
    {
        VersionStem s; s.file = f; s.role = Stemmer::roleFromFileName (f.getFileName()); s.outRole = outRoleOf (s.role);
        v.stems.push_back (s);
    }
    v.title = folder.getFileName();
    return v;
}

bool Engine::isAudioFile (const juce::File& f)
{
    return f.hasFileExtension ("flac;wav;aif;aiff;mp3");
}

Version Engine::versionFromFiles (const juce::String& id, const juce::Array<juce::File>& files)
{
    Version v; v.id = id;
    juce::Array<juce::File> fs;
    for (auto& f : files)
    {
        if (f.isDirectory()) { for (auto& c : f.findChildFiles (juce::File::findFiles, false)) if (isAudioFile (c)) fs.add (c); }
        else if (isAudioFile (f)) fs.add (f);
    }
    fs.sort();
    for (auto& f : fs)
    {
        VersionStem s; s.file = f; s.role = Stemmer::roleFromFileName (f.getFileName()); s.outRole = outRoleOf (s.role);
        v.stems.push_back (s);
    }
    if (! fs.isEmpty()) { v.folder = fs[0].getParentDirectory(); v.title = v.folder.getFileName(); }
    return v;
}

std::vector<float> Engine::envelopeOf (const juce::File& f)
{
    std::vector<float> e;
    juce::AudioBuffer<float> b; double sr = 0; juce::String err;
    if (! Stemmer::loadAudio (f, b, sr, err) || sr <= 0) return e;
    const int hop = juce::jmax (1, (int) (sr / 100));
    for (int i = 0; i < b.getNumSamples(); i += hop)
    {
        float m = 0; const int n = juce::jmin (hop, b.getNumSamples() - i);
        for (int c = 0; c < b.getNumChannels(); ++c) m = juce::jmax (m, b.getMagnitude (c, i, n));
        e.push_back (m);
    }
    return e;
}

// Кэш разбора версии без звука: длина, громкость стемов, огибающие (байт на точку), тональность.
static bool readMeta (Version& v, const juce::File& f)
{
    const auto j = juce::JSON::parse (f.loadFileAsString());
    auto* st = j["stems"].getArray();
    if (st == nullptr || st->size() != (int) v.stems.size() || (int) j["format"] != 1) return false;
    v.sampleRate = (double) j["sampleRate"]; v.lengthSec = (double) j["lengthSec"]; v.mixLufs = (double) j["mixLufs"];
    for (int i = 0; i < st->size(); ++i)
    {
        auto& s = v.stems[(size_t) i];
        s.lufs = (double) (*st)[i]["lufs"]; s.peakDb = (double) (*st)[i]["peakDb"]; s.silent = s.lufs < -55.0;
    }
    v.key = {}; v.key.tonic = (int) j["key"]["tonic"]; v.key.minor = (bool) j["key"]["minor"]; v.key.confidence = (float) (double) j["key"]["confidence"];
    if (auto* c = j["key"]["chroma"].getArray()) for (int k = 0; k < juce::jmin (12, c->size()); ++k) v.key.chroma[(size_t) k] = (float) (double) (*c)[k];
    v.env.clear();
    if (auto* e = j["env"].getDynamicObject())
        for (auto& p : e->getProperties())
        {
            juce::MemoryBlock mb; mb.fromBase64Encoding (p.value.toString());
            auto& out = v.env[p.name.toString()]; out.resize (mb.getSize());
            const auto* b = static_cast<const juce::uint8*> (mb.getData());
            for (size_t k = 0; k < mb.getSize(); ++k) { const float x = b[k] / 255.0f; out[k] = x * x; }   // корень при записи — точность тихих мест
        }
    return v.sampleRate > 0;
}

static void writeMeta (const Version& v, const juce::File& f)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("format", 1); o->setProperty ("sampleRate", v.sampleRate); o->setProperty ("lengthSec", v.lengthSec); o->setProperty ("mixLufs", v.mixLufs);
    juce::Array<juce::var> st;
    for (auto& s : v.stems) { auto* so = new juce::DynamicObject(); so->setProperty ("file", s.file.getFileName()); so->setProperty ("lufs", s.lufs); so->setProperty ("peakDb", s.peakDb); st.add (juce::var (so)); }
    o->setProperty ("stems", st);
    auto* k = new juce::DynamicObject(); k->setProperty ("tonic", v.key.tonic); k->setProperty ("minor", v.key.minor); k->setProperty ("confidence", v.key.confidence);
    juce::Array<juce::var> ch; for (float c : v.key.chroma) ch.add (c); k->setProperty ("chroma", ch);
    o->setProperty ("key", juce::var (k));
    auto* e = new juce::DynamicObject();
    for (auto& [role, env] : v.env)
    {
        juce::MemoryBlock mb (env.size());
        auto* b = static_cast<juce::uint8*> (mb.getData());
        for (size_t i = 0; i < env.size(); ++i) b[i] = (juce::uint8) juce::jlimit (0, 255, (int) std::lround (std::sqrt (juce::jlimit (0.0f, 1.0f, env[i])) * 255.0f));
        e->setProperty (role, mb.toBase64Encoding());
    }
    o->setProperty ("env", juce::var (e));
    f.replaceWithText (juce::JSON::toString (juce::var (o), true));
}

bool Engine::analyseVersion (Version& v, const juce::File& cacheDir, Progress progress, juce::String& error, const std::atomic<bool>* cancel)
{
    auto say = [&] (float f, const juce::String& m) { if (progress) progress (f, m); };
    cacheDir.createDirectory();
    juce::File drums;
    for (auto& s : v.stems) if (s.role == "drums") drums = s.file;
    const bool noDrums = drums == juce::File();      // нет стема барабанов (полный микс) — темп ищем по сумме всех стемов
    if (v.stems.empty()) { error = juce::String ("Variant ") + v.id + juce::String (" has no audio files"); return false; }

    // ключ кэша — файлы и роли (одинаковые id «V1» у разных песен не путаются)
    juce::String key; for (auto& s : v.stems) key << s.file.getFullPathName() << "|" << s.file.getSize() << "|" << s.file.getLastModificationTime().toMilliseconds() << "|" << s.role << ";";
    const auto base = juce::File::createLegalFileName (v.id) + "_" + juce::String::toHexString (key.hashCode64());
    const auto cache = cacheDir.getChildFile (base + "_dzod_stemmer_v2.json");
    const auto meta = cacheDir.getChildFile (base + "_meta_v1.json");
    if (cache.existsAsFile() && meta.existsAsFile() && readMeta (v, meta))
    {   // быстрый путь: версия уже разбиралась — звук не читаем
        Stemmer st;
        if (st.readAnalysis (cache))
        {
            const auto& A = st.analysis();
            v.analysisFile = cache;
            v.grid = A.grid; v.bars = A.bars; v.sections = A.sections;
            v.bpm = A.tempoEstimate > 0 ? A.tempoEstimate : A.bpmMedian; v.bpmMin = A.bpmMin; v.bpmMax = A.bpmMax;
            v.analysed = true;
            say (1.0f, juce::String ("Ready (cached): ") + v.id);
            return true;
        }
    }

    // все стемы: громкость, сумма для анализа, гармонические — для тональности
    say (0.02f, juce::String ("Reading stems: ") + v.id);
    std::vector<juce::AudioBuffer<float>> bufs (v.stems.size());
    double sr = 0;
    for (size_t i = 0; i < v.stems.size(); ++i)
    {
        juce::String e; double fsr = 0;
        if (! Stemmer::loadAudio (v.stems[i].file, bufs[i], fsr, e)) { error = e; return false; }
        if (sr == 0) sr = fsr;
        if (std::abs (fsr - sr) > 0.5) bufs[i] = Stemmer::resample (bufs[i], fsr, sr);
        if (bufs[i].getNumChannels() == 1) { bufs[i].setSize (2, bufs[i].getNumSamples(), true); bufs[i].copyFrom (1, 0, bufs[i], 0, 0, bufs[i].getNumSamples()); }
        v.stems[i].lufs = lufs (bufs[i], sr);
        v.stems[i].peakDb = peakDb (bufs[i]);
        v.stems[i].silent = v.stems[i].lufs < -55.0;
    }
    v.sampleRate = sr;
    int n = 0; for (auto& b : bufs) n = juce::jmax (n, b.getNumSamples());
    v.lengthSec = n / sr;
    {   // огибающие строк стола и громкость всей версии
        v.env.clear();
        const int hop = juce::jmax (1, (int) (sr / 100)), frames = n / hop + 1;
        juce::AudioBuffer<float> mix (2, n); mix.clear();
        for (size_t i = 0; i < v.stems.size(); ++i)
        {
            auto& e = v.env[v.stems[i].outRole]; if (e.empty()) e.assign ((size_t) frames, 0.0f);
            const auto& b = bufs[i];
            for (int f = 0; f < frames; ++f)
            {
                const int s0 = f * hop, len = juce::jmin (hop, b.getNumSamples() - s0); if (len <= 0) break;
                float m = 0; for (int c = 0; c < b.getNumChannels(); ++c) m = juce::jmax (m, b.getMagnitude (c, s0, len));
                e[(size_t) f] = juce::jmax (e[(size_t) f], m);
            }
            for (int c = 0; c < 2; ++c) mix.addFrom (c, 0, b, juce::jmin (c, b.getNumChannels() - 1), 0, b.getNumSamples());
        }
        v.mixLufs = lufs (mix, sr);
    }

    Stemmer st;
    if (! (cache.existsAsFile() && st.readAnalysis (cache)))
    {
        say (0.15f, juce::String ("Finding tempo, beats and parts: ") + v.id);
        juce::AudioBuffer<float> sum (2, n); sum.clear();
        for (auto& b : bufs) for (int c = 0; c < 2; ++c) sum.addFrom (c, 0, b, c, 0, b.getNumSamples());
        const auto tmp = cacheDir.getChildFile (v.id + "_sum.wav"); tmp.deleteFile();
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (tmp), sr, 2, 24, {}, 0));
            if (w == nullptr) { error = juce::String ("Could not write a temporary file"); return false; }
            w->writeFromAudioSampleBuffer (sum, 0, sum.getNumSamples());
        }
        stemmer::Job job; job.drums = noDrums ? tmp : drums; job.full = tmp; job.projectBpm = 0; job.straightenGrid = true;   // сетка без местных «вилок» в сбивках (drummer, 26.10)
        juce::Array<stemmer::RoleFile> rf; for (auto& s : v.stems) rf.add ({ s.file, s.role });
        const bool ok = st.analyse (job, nullptr) && st.analyseSections (rf, nullptr);
        tmp.deleteFile();
        if (! ok) { error = st.lastError(); return false; }
        st.writeAnalysis (cache, st.analysis().autoShift, 0, "DZOD Arranger");
    }
    if (isCancelled (cancel)) { error = juce::String ("Cancelled"); return false; }
    const auto& A = st.analysis();
    v.analysisFile = cache;
    v.grid = A.grid; v.bars = A.bars; v.sections = A.sections;
    v.bpm = A.tempoEstimate > 0 ? A.tempoEstimate : A.bpmMedian; v.bpmMin = A.bpmMin; v.bpmMax = A.bpmMax;

    say (0.85f, juce::String ("Key: ") + v.id);
    std::vector<const juce::AudioBuffer<float>*> harm;
    for (size_t i = 0; i < v.stems.size(); ++i) if (harmonicRole (v.stems[i].role) && ! v.stems[i].silent) harm.push_back (&bufs[i]);
    v.key = detectKey (harm, sr);
    v.analysed = true;
    writeMeta (v, meta);
    say (1.0f, juce::String ("Ready: ") + v.id);
    return true;
}

double Engine::songBpm (const Project& p)
{
    if (p.songBpm > 0) return p.songBpm;
    return analyseTempo (p).proposed;
}

TempoProposal Engine::analyseTempo (const Project& p, double song)
{
    TempoProposal r;
    std::vector<std::pair<double, double>> w;      // темп, длина
    for (auto& pc : p.pieces)
    {
        const auto* v = p.findVersion (pc.version);
        const double b = v != nullptr ? v->localBpm (pc.from, pc.to) : 0;
        r.pieceBpm.push_back (b);
        if (b > 0) w.push_back ({ b, pc.length() });
    }
    if (! w.empty())
    {
        std::sort (w.begin(), w.end());
        double total = 0; for (auto& x : w) total += x.second;
        double acc = 0; for (auto& x : w) { acc += x.second; if (acc >= total / 2) { r.median = std::round (x.first * 10.0) / 10.0; break; } }
    }
    const int s = p.sampleFor ("drums");
    if (s >= 0 && s < (int) r.pieceBpm.size()) r.drumsSample = std::round (r.pieceBpm[(size_t) s] * 10.0) / 10.0;
    r.proposed = r.drumsSample > 0 ? r.drumsSample : r.median;
    const double ref = song > 0 ? song : r.proposed;
    for (double b : r.pieceBpm) r.deviationPct.push_back (b > 0 && ref > 0 ? 100.0 * (b - ref) / ref : 0.0);
    return r;
}

//==============================================================================
bool Engine::mixIntoStems (Project& P, const juce::File& cacheDir, const juce::File& outFolder, Progress progress, juce::var& report, juce::String& error, const std::atomic<bool>* cancel)
{
    auto say = [&] (float f, const juce::String& m) { if (progress) progress (f, m); };
    if (P.pieces.empty()) { error = juce::String ("No slices on the table"); return false; }
    for (auto& v : P.versions)
        if (! v.analysed && ! analyseVersion (v, cacheDir, progress, error, cancel)) return false;

    const double bpm = songBpm (P);
    if (bpm <= 0) { error = juce::String ("No song tempo and no drums reference"); return false; }
    juce::StringArray warnings;

    // 1) края кусков — на сильные доли; длина каждого куска на выходе и место в песне
    std::vector<int> plOf (P.pieces.size(), -1);                   // номер куска -> номер в раскладке (-1 — пропущен)
    struct Placed { int index = 0; Piece pc; Version* v = nullptr; double a = 0, b = 0, dur = 0, at = 0, srcBpm = 0, stretchPct = 0; std::function<double (int)> map; Key key; };
    std::vector<Placed> pl;
    std::map<juce::String, std::unique_ptr<Stemmer>> st;
    double at = 0;
    for (size_t i = 0; i < P.pieces.size(); ++i)
    {
        auto& pc = P.pieces[i];
        auto* v = P.findVersion (pc.version);
        if (v == nullptr) { error = juce::String ("Slice ") + juce::String ((int) i + 1) + juce::String (": no variant ") + pc.version; return false; }
        if (st.find (v->id) == st.end())
        {
            auto s = std::make_unique<Stemmer>();
            if (! s->readAnalysis (v->analysisFile)) { error = s->lastError(); return false; }
            st[v->id] = std::move (s);
        }
        Placed p; p.pc = pc; p.v = v; p.index = (int) i;
        // края — на сильные доли; кусок короче такта — на доли; короче доли — пропускаем (не валим всю песню)
        const auto& g = P.snapToBars ? (v->bars.empty() ? v->grid : v->bars) : v->grid;
        double f = snapTo (g, pc.from), t = snapTo (g, pc.to);
        if (t <= f + 0.1 && &g != &v->grid) { f = snapTo (v->grid, pc.from); t = snapTo (v->grid, pc.to);
            if (t > f + 0.1) warnings.add (juce::String ("Slice ") + juce::String ((int) i + 1) + " (" + v->id + juce::String (") is shorter than a bar - edges put on beats, the song moves off the bars")); }
        if (t <= f + 0.1) { warnings.add (juce::String ("Slice ") + juce::String ((int) i + 1) + " (" + v->id + ", " + fmt (pc.length(), 2) + juce::String (" s) is shorter than a beat - skipped")); continue; }
        if (! st[v->id]->pieceMap (f, t, bpm, p.map, p.dur, p.a, p.b) || p.dur <= 0.05)
        { warnings.add (juce::String ("Slice ") + juce::String ((int) i + 1) + " (" + v->id + juce::String (") - could not build the stretch, skipped")); continue; }
        p.srcBpm = v->localBpm (p.a, p.b);
        p.stretchPct = 100.0 * (bpm / juce::jmax (1.0, p.srcBpm) - 1.0);
        p.at = at; at += p.dur;
        pc.from = p.a; pc.to = p.b;
        plOf[i] = (int) pl.size();
        pl.push_back (std::move (p));
    }
    if (pl.empty()) { error = juce::String ("No slice is longer than a beat"); return false; }
    const double total = at;
    const int handle = Stemmer::StretchPlan::Hs;                    // запас по краям куска для кроссфейда
    const int xf = juce::jmax (8, (int) std::lround (P.crossfadeMs * 0.001 * kOutRate));
    const juce::int64 nOut = (juce::int64) std::ceil (total * kOutRate) + xf;

    // 2) подгонка к образцу: по каждой роли — кривая и усиление на версию
    //    спектр образца — кусок-образец роли; спектр версии — все её места в песне;
    //    громкость — по стему версии целиком (так сохраняется разница куплет / припев внутри версии)
    say (0.05f, juce::String ("Comparing with the references"));
    const bool byPiece = P.matchBy != "version";
    auto fixKey = [&] (size_t plIndex) { return byPiece ? "#" + juce::String ((int) plIndex) : pl[plIndex].v->id; };
    struct Fix { Spectrum curve {}; double gainDb = 0; double lufs = -70; bool any = false; };
    std::map<juce::String, std::map<juce::String, Fix>> fix;      // outRole -> версия -> поправка
    std::map<juce::String, Spectrum> refSpec;                       // outRole -> спектр образца
    std::map<juce::String, double> refLufs;
    std::map<juce::String, juce::String> refVersion;
    for (auto& role : outRoles())
    {
        const int s = P.sampleFor (role);
        if (s < 0 || plOf[(size_t) s] < 0) continue;
        refVersion[role] = P.pieces[(size_t) s].version;
    }
    std::map<juce::String, std::map<juce::String, Spectrum>> useSpec;   // outRole -> версия -> спектр мест в песне
    std::map<juce::String, std::map<juce::String, double>> wholeLufs;
    for (auto& v : P.versions)
    {
        bool used = false; for (auto& p : pl) if (p.v == &v) used = true;
        bool isRef = false; for (auto& [r, id] : refVersion) if (id == v.id) isRef = true;
        if (! used && ! isRef) continue;
        say (0.08f, juce::String ("Measuring: ") + v.id);
        std::map<juce::String, juce::AudioBuffer<float>> roleBuf;
        for (auto& s : v.stems)
        {
            if (s.silent) continue;
            juce::AudioBuffer<float> b; juce::String e;
            if (! loadAt (s.file, v.sampleRate, b, e)) { error = e; return false; }
            auto& rb = roleBuf[s.outRole];
            if (rb.getNumSamples() < b.getNumSamples()) rb.setSize (2, b.getNumSamples(), true, true);
            for (int c = 0; c < 2; ++c) rb.addFrom (c, 0, b, c, 0, b.getNumSamples());
        }
        for (auto& [role, b] : roleBuf)
        {
            wholeLufs[role][v.id] = lufs (b, v.sampleRate);
            // места этой версии в песне — одним буфером
            juce::AudioBuffer<float> used2 (2, 0);
            for (auto& p : pl)
                if (p.v == &v && P.isOn (p.pc, role))
                {
                    const int s0 = (int) (p.a * v.sampleRate), s1 = juce::jmin (b.getNumSamples(), (int) (p.b * v.sampleRate));
                    if (s1 <= s0) continue;
                    const int off = used2.getNumSamples(); used2.setSize (2, off + s1 - s0, true, true);
                    for (int c = 0; c < 2; ++c) used2.copyFrom (c, off, b, c, s0, s1 - s0);
                }
            if (used2.getNumSamples() > 0 && ! byPiece) useSpec[role][v.id] = ltas (used2, v.sampleRate);
            const int s = P.sampleFor (role);
            if (s >= 0 && plOf[(size_t) s] >= 0 && P.pieces[(size_t) s].version == v.id)
            {
                const auto& sp = pl[(size_t) plOf[(size_t) s]];
                const auto a0 = (juce::int64) (sp.a * v.sampleRate), a1 = (juce::int64) (sp.b * v.sampleRate);
                refSpec[role] = ltas (b, v.sampleRate, a0, a1);
                refLufs[role] = byPiece ? lufs (b, v.sampleRate, a0, a1) : lufs (b, v.sampleRate);
            }
            if (byPiece)   // каждый кусок отдельно: свой спектр и своя громкость
                for (size_t k = 0; k < pl.size(); ++k)
                {
                    const auto& p = pl[k];
                    if (p.v != &v || ! P.isOn (p.pc, role)) continue;
                    const auto a0 = (juce::int64) (p.a * v.sampleRate), a1 = juce::jmin ((juce::int64) b.getNumSamples(), (juce::int64) (p.b * v.sampleRate));
                    if (a1 <= a0) continue;
                    const double l = lufs (b, v.sampleRate, a0, a1);
                    if (l < -60) continue;                       // инструмент в куске почти молчит — не трогаем
                    useSpec[role]["#" + juce::String ((int) k)] = ltas (b, v.sampleRate, a0, a1);
                    wholeLufs[role]["#" + juce::String ((int) k)] = l;
                }
        }
    }
    for (auto& role : outRoles())
    {
        if (refSpec.find (role) == refSpec.end())
        {
            bool needed = false; for (auto& p : pl) if (P.isOn (p.pc, role)) needed = true;
            if (needed) warnings.add (neuroName (role) + juce::String (": no reference - sound not unified (mark a reference slice)"));
            continue;
        }
        for (auto& [vid, spec] : useSpec[role])
        {
            Fix fx;
            if (P.matchEq) fx.curve = eqCurve (spec, refSpec[role], P.strengthFor (role), P.limitFor (role));
            fx.lufs = wholeLufs[role][vid];
            // громкость — с учётом того, насколько кривая сама меняет уровень (иначе кусок остаётся тише образца)
            double eqDelta = 0;
            {
                double p0 = 0, p1 = 0;
                for (int k = 0; k < kGrid; ++k) { const double pw = std::pow (10.0, spec[(size_t) k] / 10.0); p0 += pw; p1 += pw * std::pow (10.0, fx.curve[(size_t) k] / 10.0); }
                if (p0 > 0 && p1 > 0) eqDelta = 10.0 * std::log10 (p1 / p0);
            }
            if (P.matchLoudness) fx.gainDb = juce::jlimit (-18.0, 18.0, (refLufs[role] - fx.lufs) * juce::jmin (1.0, P.strengthFor (role) / 0.7) - eqDelta);
            else fx.gainDb = -eqDelta;
            fx.any = true;
            fix[role][vid] = fx;
        }
    }

    // 3) растяжка и сведение: по версиям, чтобы держать в памяти одну версию
    std::map<juce::String, juce::AudioBuffer<float>> out;            // outRole -> итог
    std::set<juce::String> rolesPresent;
    for (auto& r : P.rolesInSong()) rolesPresent.insert (r);
    for (auto& r : rolesPresent) { out[r].setSize (2, (int) nOut); out[r].clear(); }

    int doneVersions = 0, usedVersions = 0;
    for (auto& v : P.versions) for (auto& p : pl) if (p.v == &v) { ++usedVersions; break; }
    for (auto& v : P.versions)
    {
        std::vector<size_t> mine; for (size_t i = 0; i < pl.size(); ++i) if (pl[i].v == &v) mine.push_back (i);
        if (mine.empty()) continue;
        if (isCancelled (cancel)) { error = juce::String ("Cancelled"); return false; }
        say (0.15f + 0.75f * doneVersions / juce::jmax (1, usedVersions), juce::String ("Stretching slices of ") + v.id + " -> " + fmt (bpm) + " BPM");
        std::vector<juce::AudioBuffer<float>> bufs (v.stems.size());
        int n = 0;
        for (size_t i = 0; i < v.stems.size(); ++i)
        {
            juce::String e; if (! loadAt (v.stems[i].file, v.sampleRate, bufs[i], e)) { error = e; return false; }
            n = juce::jmax (n, bufs[i].getNumSamples());
        }
        std::vector<float> mono ((size_t) n, 0.0f);
        for (auto& b : bufs) for (int c = 0; c < 2; ++c) { const float* x = b.getReadPointer (c); for (int i = 0; i < b.getNumSamples(); ++i) mono[(size_t) i] += 0.5f * x[i]; }
        const double sr = v.sampleRate;
        for (size_t idx : mine)
        {
            auto& p = pl[idx];
            // ключ куска (по гармоническим стемам) — для предупреждения о несовпадении соседей
            std::vector<const juce::AudioBuffer<float>*> harm;
            for (size_t i = 0; i < v.stems.size(); ++i) if (harmonicRole (v.stems[i].role) && ! v.stems[i].silent) harm.push_back (&bufs[i]);
            p.key = detectKey (harm, sr, p.a, p.b);

            // карта с запасом по кадру с каждой стороны (для кроссфейда на стыке)
            const auto base = p.map;
            const double m0 = base (0), m1 = base (1);
            std::function<double (int)> map2 = [base, m0, m1] (int m) { const int q = m - 1; return q < 0 ? m0 + q * (m1 - m0) : base (q); };
            const double dur2 = p.dur + 2.0 * handle / sr;
            Stemmer::StretchPlan plan;
            const bool wsola = P.stretcher == "wsola";
            if (wsola && ! Stemmer::planStretch (mono, sr, dur2, map2, plan)) { error = juce::String ("Stretch failed: ") + v.id; return false; }
            const int want = (int) std::lround (dur2 * sr);
            // растяжка стемов куска — параллельно (стемы независимы), раскладка в песню — по очереди
            std::vector<size_t> todo;
            for (size_t i = 0; i < v.stems.size(); ++i) if (! v.stems[i].silent && P.isOn (p.pc, v.stems[i].outRole)) todo.push_back (i);
            std::vector<juce::AudioBuffer<float>> segs (v.stems.size());
            parallelFor (todo.size(), [&] (size_t k)
            {
                const size_t i = todo[k];
                const auto& s = v.stems[i];
                if (isCancelled (cancel)) return;
                auto seg = wsola ? Stemmer::applyStretch (bufs[i], plan)
                                 : stretchByMap (bufs[i], sr, map2, handle, want, s.outRole == "drums");
                if (seg.getNumChannels() == 1) { seg.setSize (2, seg.getNumSamples(), true); seg.copyFrom (1, 0, seg, 0, 0, seg.getNumSamples()); }
                seg.setSize (2, want, true, true);
                if (std::abs (sr - kOutRate) > 0.5) seg = Stemmer::resample (seg, sr, kOutRate);
                auto fit = fix.find (s.outRole);
                if (fit != fix.end())
                {
                    auto f2 = fit->second.find (fixKey (idx));
                    if (f2 != fit->second.end())
                    {
                        applyEq (seg, kOutRate, f2->second.curve);
                        seg.applyGain ((float) std::pow (10.0, f2->second.gainDb / 20.0));
                    }
                }
                segs[i] = std::move (seg);
            });
            if (isCancelled (cancel)) { error = juce::String ("Cancelled"); return false; }
            for (size_t i : todo)
            {
                const auto& s = v.stems[i];
                const auto& seg = segs[i];
                // место: начало куска на выходе = p.at; в seg запас handle (в отсчётах источника) до начала
                const juce::int64 lead = (juce::int64) std::lround ((double) handle / sr * kOutRate);
                const juce::int64 start = (juce::int64) std::lround (p.at * kOutRate);
                const juce::int64 end = (juce::int64) std::lround ((p.at + p.dur) * kOutRate);
                const bool first = idx == 0, last = idx + 1 == pl.size();
                auto& dst = out[s.outRole];
                const juce::int64 from = first ? start : start - xf / 2, to = last ? end : end + xf / 2;
                for (juce::int64 t = juce::jmax ((juce::int64) 0, from); t < juce::jmin (to, nOut); ++t)
                {
                    const juce::int64 k = t - start + lead;
                    if (k < 0 || k >= seg.getNumSamples()) continue;
                    float g = 1.0f;
                    if (! first && t < start + xf / 2) g *= fadeIn ((double) (t - (start - xf / 2)) / xf);
                    if (! last && t > end - xf / 2) g *= fadeOut ((double) (t - (end - xf / 2)) / xf);
                    if (last && t > end - xf) g *= fadeOut ((double) (t - (end - xf)) / xf);
                    for (int c = 0; c < 2; ++c) dst.addSample (c, (int) t, seg.getSample (c, (int) k) * g);
                }
            }
        }
        ++doneVersions;
    }

    // 4) общий запас: если сумма где-то громче -1 dBFS — все роли тише на одно и то же
    double peak = -100; for (auto& [r, b] : out) peak = juce::jmax (peak, peakDb (b));
    const double trim = juce::jmin (0.0, -1.0 - peak);
    if (trim < 0) for (auto& [r, b] : out) b.applyGain ((float) std::pow (10.0, trim / 20.0));

    // 5) запись: WAV по ролям, dzod_stems.json, отчёт
    say (0.92f, juce::String ("Writing stems"));
    outFolder.createDirectory();
    juce::Array<juce::var> stemsList;
    for (auto& role : outRoles())
    {
        auto it = out.find (role); if (it == out.end()) continue;
        const auto f = outFolder.getChildFile (neuroName (role) + ".wav"); f.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), kOutRate, 2, 24, {}, 0));
        if (w == nullptr) { error = juce::String ("Could not write ") + f.getFullPathName(); return false; }
        w->writeFromAudioSampleBuffer (it->second, 0, it->second.getNumSamples());
        auto* o = new juce::DynamicObject();
        o->setProperty ("file", f.getFileName()); o->setProperty ("role", role); o->setProperty ("name", neuroName (role));
        o->setProperty ("sampleRate", kOutRate); o->setProperty ("channels", 2); o->setProperty ("lengthSamples", (juce::int64) it->second.getNumSamples());
        o->setProperty ("startSec", 0.0);
        stemsList.add (juce::var (o));
        // роль есть в песне, но не во всех кусках
        for (size_t i = 0; i < pl.size(); ++i)
            if (! P.isOn (pl[i].pc, role))
                warnings.add (neuroName (role) + juce::String (": slice ") + juce::String (pl[i].index + 1) + " (" + pl[i].v->id
                              + (pl[i].v->hasRole (role) ? juce::String (") instrument removed - silence there") : juce::String (") has no such instrument - silence there")));
    }
    for (size_t i = 0; i < pl.size(); ++i)
    {
        if (std::abs (pl[i].stretchPct) > 10) warnings.add (juce::String ("Slice ") + juce::String (pl[i].index + 1) + juce::String (": stretch ") + fmt (pl[i].stretchPct) + juce::String (" % - more than 10 %, listen carefully"));
        if (i > 0 && ! pl[i].key.compatibleWith (pl[i - 1].key))
            warnings.add (juce::String ("Seam ") + juce::String (pl[i - 1].index + 1) + "-" + juce::String (pl[i].index + 1) + juce::String (": the keys may differ (") + pl[i - 1].key.name() + " / " + pl[i].key.name()
                          + juce::String (", notes similarity ") + juce::String (pl[i].key.similarity (pl[i - 1].key), 2) + ")");
    }
    if (trim < 0) warnings.add (juce::String ("The sum of the stems is above -1 dBFS - all stems lowered by ") + fmt (-trim) + juce::String (" dB"));

    // метки частей -> секунды и такты песни
    juce::Array<juce::var> sectionsVar;
    {
        const double barSec = 240.0 / bpm;
        for (auto& sc : P.sections())
        {
            int f0 = -1, l0 = -1;
            for (int k = sc.firstPiece; k <= sc.lastPiece; ++k) if (plOf[(size_t) k] >= 0) { if (f0 < 0) f0 = plOf[(size_t) k]; l0 = plOf[(size_t) k]; }
            if (f0 < 0) continue;
            auto* o = new juce::DynamicObject();
            const double a = pl[(size_t) f0].at, b = pl[(size_t) l0].at + pl[(size_t) l0].dur;
            o->setProperty ("label", sc.label); o->setProperty ("startSec", a); o->setProperty ("endSec", b);
            o->setProperty ("startBar", (int) std::lround (a / barSec) + 1); o->setProperty ("endBar", (int) std::lround (b / barSec));
            sectionsVar.add (juce::var (o));
        }
    }
    {
        auto* m = new juce::DynamicObject();
        m->setProperty ("format", 1); m->setProperty ("writer", "DZOD Arranger");
        m->setProperty ("writtenAt", juce::Time::getCurrentTime().toISO8601 (true));
        m->setProperty ("projectBpm", bpm); m->setProperty ("songStartsAtBar", 1); m->setProperty ("songStartSec", 0.0);
        m->setProperty ("source", "arranger"); m->setProperty ("report", "arranger_report.json");
        m->setProperty ("stems", stemsList);
        m->setProperty ("sections", sectionsVar);
        outFolder.getChildFile ("dzod_stems.json").replaceWithText (juce::JSON::toString (juce::var (m)));
    }

    auto* R = new juce::DynamicObject();
    R->setProperty ("songBpm", bpm);
    R->setProperty ("lengthSec", total);
    R->setProperty ("crossfadeMs", P.crossfadeMs);
    R->setProperty ("trimDb", trim);
    juce::Array<juce::var> pr;
    for (size_t i = 0; i < pl.size(); ++i)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("n", pl[i].index + 1); o->setProperty ("version", pl[i].v->id);
        o->setProperty ("srcFrom", pl[i].a); o->setProperty ("srcTo", pl[i].b);
        o->setProperty ("at", pl[i].at); o->setProperty ("dur", pl[i].dur);
        o->setProperty ("bars", juce::roundToInt (pl[i].dur * bpm / 60.0 / 4.0 * 100) / 100.0);
        o->setProperty ("id", pl[i].pc.id);
        o->setProperty ("srcBpm", pl[i].srcBpm); o->setProperty ("stretchPct", pl[i].stretchPct);
        o->setProperty ("key", pl[i].key.name());
        o->setProperty ("keyConfidence", pl[i].key.confidence);
        if (i > 0) o->setProperty ("notesSimilarityToPrev", pl[i].key.similarity (pl[i - 1].key));
        pr.add (juce::var (o));
    }
    R->setProperty ("pieces", pr);
    {   // сетка песни для окна: доли и такты (ровные, темп песни), с
        juce::Array<juce::var> beats, bars; const double beat = 60.0 / bpm;
        for (int k = 0; k * beat < total + 1e-6; ++k) { beats.add (k * beat); if (k % 4 == 0) bars.add (k * beat); }
        R->setProperty ("beats", beats); R->setProperty ("bars", bars); R->setProperty ("matchBy", byPiece ? "piece" : "version");
    }
    R->setProperty ("sections", sectionsVar);
    {
        auto tp = analyseTempo (P, bpm);
        auto* o = new juce::DynamicObject(); o->setProperty ("median", tp.median); o->setProperty ("drumsSample", tp.drumsSample); o->setProperty ("proposed", tp.proposed);
        R->setProperty ("tempo", juce::var (o));
    }
    juce::Array<juce::var> fx;
    for (auto& [role, per] : fix)
        for (auto& [vid, f] : per)
        {
            auto* o = new juce::DynamicObject();
            const bool pk = vid.startsWith ("#");
            const int k = pk ? vid.substring (1).getIntValue() : -1;
            o->setProperty ("role", role); o->setProperty ("version", pk ? pl[(size_t) k].v->id : vid);
            if (pk) o->setProperty ("piece", pl[(size_t) k].index + 1); o->setProperty ("gainDb", f.gainDb); o->setProperty ("strength", P.strengthFor (role)); o->setProperty ("limitDb", P.limitFor (role));
            o->setProperty ("sample", refVersion[role]);
            juce::Array<juce::var> oct; for (double x : octaveTable (f.curve)) oct.add (std::round (x * 10) / 10);
            o->setProperty ("eqOctaves", oct);
            fx.add (juce::var (o));
        }
    R->setProperty ("matching", fx);
    juce::Array<juce::var> w; for (auto& s : warnings) w.add (s);
    R->setProperty ("warnings", w);
    juce::Array<juce::var> fs; for (auto& s : stemsList) fs.add (s["file"]);
    R->setProperty ("files", fs);
    report = juce::var (R);
    outFolder.getChildFile ("arranger_report.json").replaceWithText (juce::JSON::toString (report));
    P.songBpm = bpm;
    say (1.0f, juce::String ("Done"));
    return true;
}
//==============================================================================
void Project::resetUnify()
{
    samples.clear(); sample = -1; strengths.clear(); limits.clear();
    strength = 0.85; limitDb = 12; matchEq = true; matchLoudness = true; matchBy = "piece";
}

bool Engine::pieceSpectrum (const Project& P, int piece, const juce::String& role, Spectrum& out, juce::String& error, double maxSec)
{
    if (piece < 0 || piece >= (int) P.pieces.size()) { error = juce::String ("No such slice"); return false; }
    const auto& pc = P.pieces[(size_t) piece];
    const auto* v = P.findVersion (pc.version);
    if (v == nullptr) { error = juce::String ("No variant ") + pc.version; return false; }
    juce::AudioBuffer<float> sum; double sr = 0;
    const double len = maxSec > 0 ? juce::jmin (pc.length(), maxSec) : pc.length();
    for (auto& s : v->stems)
    {
        if (s.outRole != role || s.silent) continue;
        std::unique_ptr<juce::AudioFormatReader> r;
        { juce::AudioFormatManager fm; fm.registerBasicFormats(); r.reset (fm.createReaderFor (s.file)); }
        if (r == nullptr) { error = juce::String ("Cannot read ") + s.file.getFileName(); return false; }
        sr = r->sampleRate;
        const int n = (int) (len * sr);
        if (sum.getNumSamples() == 0) { sum.setSize (2, n); sum.clear(); }
        juce::AudioBuffer<float> b (2, n); b.clear();
        r->read (&b, 0, n, (juce::int64) (pc.from * sr), true, true);
        if (r->numChannels == 1) b.copyFrom (1, 0, b, 0, 0, n);
        for (int c = 0; c < 2; ++c) sum.addFrom (c, 0, b, c, 0, juce::jmin (n, sum.getNumSamples()));
    }
    if (sum.getNumSamples() == 0) { error = juce::String ("The slice has no instrument ") + role; return false; }
    out = normalised (ltas (sum, sr));
    return true;
}

std::vector<Engine::PlayItem> Engine::playPlan (const Project& P, bool equal)
{
    std::vector<PlayItem> r;
    double ref = 0; bool any = false;
    for (auto& pc : P.pieces) if (auto* v = P.findVersion (pc.version)) if (v->analysed) { ref = any ? juce::jmin (ref, v->mixLufs) : v->mixLufs; any = true; }
    double at = 0;
    for (int i = 0; i < (int) P.pieces.size(); ++i)
    {
        const auto& pc = P.pieces[(size_t) i];
        PlayItem it; it.piece = i; it.at = at; it.length = pc.length(); it.src = pc.from;
        if (auto* v = P.findVersion (pc.version))
        {
            for (auto& s : v->stems) if (! s.silent && P.isOn (pc, s.outRole)) it.files.push_back (s.file);
            if (equal && any && v->analysed) it.gain = (float) juce::Decibels::decibelsToGain (ref - v->mixLufs);
        }
        r.push_back (it); at += pc.length();
    }
    return r;
}

//==============================================================================
static bool readStemsList (const juce::File& folder, juce::Array<juce::File>& files, juce::StringArray& roles, juce::String& error)
{
    const auto j = juce::JSON::parse (folder.getChildFile ("dzod_stems.json").loadFileAsString());
    auto* a = j["stems"].getArray();
    if (a == nullptr) { error = juce::String ("No dzod_stems.json in the folder - MIX INTO STEMS first"); return false; }
    for (auto& s : *a) { files.add (folder.getChildFile (s["file"].toString())); roles.add (s["role"].toString()); }
    return true;
}

bool Engine::exportSong (const juce::File& stemsFolder, const juce::File& target, const juce::String& format, juce::String& error)
{
    juce::Array<juce::File> files; juce::StringArray roles;
    if (! readStemsList (stemsFolder, files, roles, error)) return false;
    juce::AudioBuffer<float> sum (2, 0); double sr = kOutRate;
    for (auto& f : files)
    {
        juce::AudioBuffer<float> b; juce::String e;
        if (! Stemmer::loadAudio (f, b, sr, e)) { error = e; return false; }
        if (b.getNumChannels() == 1) { b.setSize (2, b.getNumSamples(), true); b.copyFrom (1, 0, b, 0, 0, b.getNumSamples()); }
        if (sum.getNumSamples() < b.getNumSamples()) sum.setSize (2, b.getNumSamples(), true, true);
        for (int c = 0; c < 2; ++c) sum.addFrom (c, 0, b, c, 0, b.getNumSamples());
    }
    if (sum.getNumSamples() == 0) { error = juce::String ("No stems"); return false; }
    // без мастеринга: только защита от перегруза — общий запас до -1 dBFS (MP3: -1,5)
    const double ceil = format == "mp3" ? -1.5 : -1.0, pk = peakDb (sum);
    if (pk > ceil) sum.applyGain ((float) std::pow (10.0, (ceil - pk) / 20.0));
    target.getParentDirectory().createDirectory(); target.deleteFile();
    if (format == "mp3")
    {
        if (! dzod::writeMp3 (target, sum, sr, 320, false)) { error = juce::String ("Could not write the MP3"); return false; }
        return true;
    }
    const int bits = format == "wav16" ? 16 : 24;
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (target), sr, 2, bits, {}, 0));
    if (w == nullptr) { error = juce::String ("Could not write ") + target.getFullPathName(); return false; }
    if (bits == 16)
    {   // TPDF-дизер перед 16 битами
        juce::Random rnd (1);
        const float q = 1.0f / 32768.0f;
        for (int c = 0; c < 2; ++c) { auto* x = sum.getWritePointer (c); for (int i = 0; i < sum.getNumSamples(); ++i) x[i] += (rnd.nextFloat() - rnd.nextFloat()) * q; }
    }
    return w->writeFromAudioSampleBuffer (sum, 0, sum.getNumSamples());
}

bool Engine::saveStems (const juce::File& stemsFolder, const juce::File& destRoot, const juce::String& pattern, const juce::String& song,
                        juce::StringArray& written, juce::String& error)
{
    juce::Array<juce::File> files; juce::StringArray roles;
    if (! readStemsList (stemsFolder, files, roles, error)) return false;
    auto clean = [] (juce::String s) { return juce::File::createLegalFileName (s.trim()); };
    const juce::String name = clean (song.isEmpty() ? juce::String ("Song") : song);
    juce::File folderOut;
    for (int i = 0; i < files.size(); ++i)
    {
        const auto label = neuroName (roles[i]).fromFirstOccurrenceOf ("Neuro ", false, false);
        auto rel = pattern.replace ("{song}", name).replace ("{instrument}", clean (label)).replace ("\\", "/");
        if (! rel.endsWithIgnoreCase (".wav")) rel << ".wav";
        const auto dst = destRoot.getChildFile (rel);
        dst.getParentDirectory().createDirectory();
        if (! files[i].copyFileTo (dst)) { error = juce::String ("Could not write ") + dst.getFullPathName(); return false; }
        written.add (dst.getFullPathName());
        folderOut = dst.getParentDirectory();
    }
    // паспорт рядом со стемами — с новыми именами файлов
    auto j = juce::JSON::parse (stemsFolder.getChildFile ("dzod_stems.json").loadFileAsString());
    if (auto* a = j["stems"].getArray())
        for (int i = 0; i < a->size() && i < written.size(); ++i)
            if (auto* o = (*a)[i].getDynamicObject()) o->setProperty ("file", juce::File (written[i]).getFileName());
    if (auto* o = j.getDynamicObject()) o->setProperty ("song", song);
    folderOut.getChildFile ("dzod_stems.json").replaceWithText (juce::JSON::toString (j));
    return true;
}
} // namespace arranger
} // namespace dzod
