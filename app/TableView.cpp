#include "ArrangerComponent.h"

namespace dzod::arranger
{
using namespace dzod::ui;

static const char* kParts[] { "Intro", "Verse", "Pre-chorus", "Chorus", "Bridge", "Solo", "Break", "Outro" };

static juce::String timeText (double s)
{
    s = juce::jmax (0.0, s);
    return juce::String ((int) (s / 60)) + ":" + juce::String (std::fmod (s, 60.0), 1).paddedLeft ('0', 4);
}

TableView::TableView (ArrangerComponent& a, bool songMode) : A (a), song (songMode)
{
    setWantsKeyboardFocus (false);
}

double TableView::totalLength() const
{
    if (song) return A.render.valid() ? A.render.length : A.P.tableLength();
    return A.P.tableLength();
}

juce::StringArray TableView::rows() const
{
    if (song && A.render.valid()) { juce::StringArray r; for (auto& [role, f] : A.render.stems) r.add (role); return r; }
    // все строки, где хоть у одного варианта есть инструмент; пустые во всех кусках — скрыты
    juce::StringArray r;
    for (auto& o : outRoles())
    {
        bool any = false;
        for (auto& p : A.P.pieces) if (A.P.isOn (p, o)) any = true;
        if (! any && ! song) for (auto& p : A.P.pieces) if (auto* v = A.P.findVersion (p.version)) if (v->hasRole (o) && v->off.count (o) == 0) any = true;
        if (any) r.add (o);
    }
    return r;
}

int TableView::rowAt (int y) const
{
    const int i = (y - rulerH - markH) / rowH;
    return y < rulerH + markH ? -1 : i;
}

void TableView::zoomAll()
{
    const double len = juce::jmax (10.0, totalLength());
    pxPerSec = juce::jmax (0.5, (getWidth() - headW - 30) / len);
    scroll = 0; fitted = true; repaint();
}

void TableView::zoomBy (float f, float aroundX)
{
    const double at = t (aroundX);
    pxPerSec = juce::jlimit (0.3, 400.0, pxPerSec * f);
    scroll = juce::jmax (0.0, at - (aroundX - headW) / pxPerSec);
    repaint();
}

void TableView::drawEnvelope (juce::Graphics& g, const std::vector<float>& e, double from, juce::Rectangle<float> r, double secs, juce::Colour c)
{
    if (e.empty() || r.getWidth() < 1) return;
    juce::Path p;
    const float mid = r.getCentreY(), half = r.getHeight() * 0.46f;
    const int w = (int) r.getWidth();
    std::vector<float> top ((size_t) w);
    for (int i = 0; i < w; ++i)
    {
        const double a = from + secs * i / w, b = from + secs * (i + 1) / w;
        int i0 = (int) (a * 100), i1 = juce::jmax (i0 + 1, (int) (b * 100));
        float m = 0; for (int k = juce::jmax (0, i0); k < juce::jmin ((int) e.size(), i1); ++k) m = juce::jmax (m, e[(size_t) k]);
        top[(size_t) i] = juce::jmin (1.0f, std::sqrt (m)) * half;     // корень — тихие места видны
    }
    p.startNewSubPath (r.getX(), mid);
    for (int i = 0; i < w; ++i) p.lineTo (r.getX() + i, mid - top[(size_t) i]);
    for (int i = w - 1; i >= 0; --i) p.lineTo (r.getX() + i, mid + top[(size_t) i]);
    p.closeSubPath();
    g.setColour (c); g.fillPath (p);
}

void TableView::paint (juce::Graphics& g)
{
    if (! fitted && getWidth() > headW + 40)
    {
        pxPerSec = juce::jmax (0.5, (getWidth() - headW - 30) / juce::jmax (10.0, totalLength()));
        scroll = 0; fitted = true;
    }
    g.fillAll (col::bg);
    const auto R = rows();
    const int W = getWidth();
    const double total = totalLength();

    // линейка
    g.setColour (col::panelLo); g.fillRect (0, 0, W, rulerH);
    g.setFont (mono (11.0f));
    if (song && A.render.valid() && A.render.bpm > 0)
    {   // такты песни
        const double bar = 240.0 / A.render.bpm;
        int stepBars = 1; while (bar * stepBars * pxPerSec < 46) stepBars *= 2;
        for (int b = 0; b * bar < total + bar; b += stepBars)
        {
            const float px = x (b * bar); if (px < headW || px > W) continue;
            g.setColour (col::line); g.drawVerticalLine ((int) px, 12.0f, (float) rulerH);
            g.setColour (col::dim); g.drawText (juce::String (b + 1), (int) px + 3, 2, 40, 14, juce::Justification::left);
        }
    }
    else
    {
        double step = 1; for (double s : { 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0 }) { step = s; if (s * pxPerSec >= 60) break; }
        for (double s = 0; s < total + step; s += step)
        {
            const float px = x (s); if (px < headW || px > W) continue;
            g.setColour (col::line); g.drawVerticalLine ((int) px, 12.0f, (float) rulerH);
            g.setColour (col::dim); g.drawText (timeText (s).upToLastOccurrenceOf (".", false, false), (int) px + 3, 2, 50, 14, juce::Justification::left);
        }
    }

    // полоса меток частей
    g.setColour (col::panel); g.fillRect (0, rulerH, W, markH);
    auto drawPart = [&] (const juce::String& label, double a, double b)
    {
        auto r = juce::Rectangle<float> (x (a), (float) rulerH + 3, (float) ((b - a) * pxPerSec), (float) markH - 6);
        if (r.getRight() < headW) return;
        g.setColour (partColour (label)); g.fillRoundedRectangle (r, 3.0f);
        g.setColour (col::text); g.setFont (narrow (13.0f));
        g.drawText (label, r.reduced (5, 0), juce::Justification::centredLeft, true);
        g.setColour (partColour (label).withAlpha (0.08f)); g.fillRect (r.getX(), (float) rulerH + markH, r.getWidth(), (float) R.size() * rowH);
    };
    if (song && A.render.valid())
    {
        if (auto* s = A.render.report["sections"].getArray()) for (auto& x2 : *s) drawPart (x2["label"].toString(), (double) x2["startSec"], (double) x2["endSec"]);
    }
    else
        for (auto& sc : A.P.sections()) drawPart (sc.label, A.P.tableStartOf (sc.firstPiece), A.P.tableStartOf (sc.lastPiece) + A.P.pieces[(size_t) sc.lastPiece].length());

    // заголовки строк
    for (int i = 0; i < R.size(); ++i)
    {
        const int y = rulerH + markH + i * rowH;
        g.setColour (i % 2 ? col::bg : col::panelLo.withAlpha (0.5f)); g.fillRect (0, y, W, rowH);
        g.setColour (roleColour (R[i])); g.fillEllipse (10.0f, y + rowH / 2.0f - 3.5f, 7.0f, 7.0f);
        g.setColour (col::text); g.setFont (font (13.5f, true));
        g.drawText (song ? neuroName (R[i]).fromFirstOccurrenceOf ("Neuro ", false, false) : roleLabel (R[i]), 24, y, headW - 26, rowH, juce::Justification::centredLeft);
    }
    g.saveState();
    g.reduceClipRegion (headW, 0, W - headW, getHeight());

    if (song && A.render.valid())
    {
        // места кусков (оттенок варианта) и волны готовых стемов
        if (auto* ps = A.render.report["pieces"].getArray())
            for (auto& p : *ps)
            {
                int vi = 0; for (size_t k = 0; k < A.P.versions.size(); ++k) if (A.P.versions[k].id == p["version"].toString()) vi = (int) k;
                const float a = x ((double) p["at"]), b = x ((double) p["at"] + (double) p["dur"]);
                g.setColour (variantColour (vi).withAlpha (0.10f)); g.fillRect (a, (float) rulerH + markH, b - a, (float) R.size() * rowH);
                g.setColour (variantColour (vi)); g.setFont (narrow (12.0f));
                g.drawText (p["version"].toString(), (int) a + 4, rulerH + markH + 2, 60, 14, juce::Justification::left);
                g.setColour (col::line); g.drawVerticalLine ((int) a, (float) rulerH, (float) (rulerH + markH + R.size() * rowH));
            }
        for (int i = 0; i < R.size(); ++i)
        {
            auto it = A.render.env.find (R[i]); if (it == A.render.env.end()) continue;
            const float y = (float) (rulerH + markH + i * rowH);
            const double from = juce::jmax (0.0, scroll), to = juce::jmin (total, t ((float) W));
            if (to > from) drawEnvelope (g, it->second, from, { x (from), y + 4, (float) ((to - from) * pxPerSec), rowH - 8.0f }, to - from, roleColour (R[i]).withAlpha (0.85f));
        }
    }
    else
    {
        double at = 0;
        for (int pi = 0; pi < (int) A.P.pieces.size(); ++pi)
        {
            const auto& pc = A.P.pieces[(size_t) pi];
            const auto* v = A.P.findVersion (pc.version);
            int vi = 0; for (size_t k = 0; k < A.P.versions.size(); ++k) if (A.P.versions[k].id == pc.version) vi = (int) k;
            const bool sel = A.selected.count (pc.id) > 0;
            const float a = x (at), b = x (at + pc.length());
            const juce::Colour vc = variantColour (vi);
            for (int i = 0; i < R.size(); ++i)
            {
                const float y = (float) (rulerH + markH + i * rowH);
                juce::Rectangle<float> cell (a + 1, y + 3, b - a - 2, rowH - 6.0f);
                if (A.P.isOn (pc, R[i]))
                {
                    g.setColour (vc.withAlpha (sel ? 0.42f : 0.26f)); g.fillRoundedRectangle (cell, 3.0f);
                    if (v != nullptr) { auto it = v->env.find (R[i]); if (it != v->env.end()) {
                        const double vis0 = juce::jmax (pc.from, pc.from + (t (cell.getX()) - at)), vis1 = juce::jmin (pc.to, pc.from + (t (cell.getRight()) - at));
                        if (vis1 > vis0) drawEnvelope (g, it->second, vis0, { x (at + vis0 - pc.from), cell.getY() + 1, (float) ((vis1 - vis0) * pxPerSec), cell.getHeight() - 2 }, vis1 - vis0, vc.brighter (0.25f)); } }
                }
                else
                {   // нет инструмента или убран — штриховка
                    g.setColour (col::panelLo); g.fillRoundedRectangle (cell, 3.0f);
                    g.setColour (col::line.withAlpha (0.6f));
                    for (float hx = cell.getX() - cell.getHeight(); hx < cell.getRight(); hx += 9) g.drawLine (juce::jmax (hx, cell.getX()), cell.getBottom() - juce::jmax (0.0f, cell.getX() - hx), juce::jmin (hx + cell.getHeight(), cell.getRight()), cell.getY() + juce::jmax (0.0f, hx + cell.getHeight() - cell.getRight()), 1.0f);
                    if (v != nullptr && v->hasRole (R[i])) { g.setColour (col::off); g.setFont (font (11.0f)); g.drawText ("removed", cell.reduced (6, 0), juce::Justification::centredLeft); }
                }
            }
            // рамка куска, подпись варианта, образец
            juce::Rectangle<float> whole (a + 1, (float) rulerH + markH + 1, b - a - 2, (float) R.size() * rowH - 2);
            g.setColour (sel ? col::orange : vc.withAlpha (0.7f)); g.drawRoundedRectangle (whole, 3.0f, sel ? 2.0f : 1.0f);
            g.setColour (vc); g.fillRoundedRectangle (a + 4, (float) rulerH + markH + 5, 26, 14, 2.0f);
            g.setColour (col::bg); g.setFont (narrow (12.0f));
            g.drawText (pc.version.substring (0, 3), (int) a + 4, rulerH + markH + 5, 26, 14, juce::Justification::centred);
            bool isRef = A.P.sample == pi; for (auto& [r, s] : A.P.samples) if (s == pi) isRef = true;
            if (isRef) { g.setColour (col::orange); g.drawText ("REF", (int) a + 34, rulerH + markH + 5, 30, 14, juce::Justification::left); }
            at += pc.length();
        }
        if (dragging && dropIndex >= 0)
        {   // куда ляжет кусок
            const float px = x (A.P.tableStartOf (dropIndex));
            g.setColour (col::orange); g.fillRect (px - 1.5f, (float) rulerH, 3.0f, (float) (markH + R.size() * rowH));
        }
    }
    // курсор и место прослушивания
    const double play = A.player.isPlaying() && (song == (A.step() >= 2)) ? A.player.position() : -1;
    if (! song) { g.setColour (col::cursor); g.fillRect (x (A.cursor) - 0.5f, 0.0f, 1.5f, (float) (rulerH + markH + R.size() * rowH)); }
    if (play >= 0) { g.setColour (juce::Colours::white.withAlpha (0.85f)); g.fillRect (x (play) - 0.5f, 0.0f, 1.0f, (float) (rulerH + markH + R.size() * rowH)); }
    g.restoreState();

    if (R.isEmpty())
    {
        g.setColour (col::dim); g.setFont (font (15.0f));
        g.drawText (song ? "No mix yet - press ALIGN (step 3)" : "Load variants in step 1, then GO TO ARRANGING", getLocalBounds().withTrimmedTop (rulerH + markH), juce::Justification::centred);
    }
}

void TableView::mouseDown (const juce::MouseEvent& e)
{
    downPos = e.getPosition(); dragging = false; dragId = -1;
    if (A.busy()) return;
    const double tt = t ((float) e.x);
    if (e.y < rulerH || song)
    {   // линейка (и просмотр песни) — курсор / прослушивание с места
        if (song) { if (A.player.isPlaying()) A.player.play (juce::jmax (0.0, tt)); repaint(); return; }
        A.cursor = juce::jlimit (0.0, totalLength(), tt);
        if (A.player.isPlaying()) A.playFrom (A.cursor);
        repaint(); return;
    }
    const int pi = A.P.pieceAt (tt);
    if (e.y < rulerH + markH) { markerMenu (pi); return; }
    const auto R = rows(); const int ri = rowAt (e.y);
    if (pi < 0) { A.selected.clear(); A.changed(); return; }
    const auto& pc = A.P.pieces[(size_t) pi];
    if (e.mods.isPopupMenu()) { if (! A.selected.count (pc.id)) { A.selected = { pc.id }; A.changed(); } pieceMenu (pi, ri >= 0 && ri < R.size() ? R[ri] : juce::String()); return; }
    if (knife)
    {
        const double cut = A.snapTable (tt);
        A.edit ([&] { A.P.split (pi, cut); });
        A.cursor = cut; return;
    }
    if (e.mods.isShiftDown()) { if (A.selected.count (pc.id)) A.selected.erase (pc.id); else A.selected.insert (pc.id); }
    else if (! A.selected.count (pc.id)) A.selected = { pc.id };
    A.cursor = juce::jlimit (0.0, totalLength(), tt);
    dragId = pc.id;
    A.changed();
}

void TableView::mouseDrag (const juce::MouseEvent& e)
{
    if (song || dragId < 0 || A.busy()) return;
    if (! dragging && e.getDistanceFromDragStart() < 6) return;
    dragging = true;
    // место вставки: между кусками, ближайший стык
    const double tt = t ((float) e.x);
    int best = 0; double bd = 1e9;
    for (int i = 0; i <= (int) A.P.pieces.size(); ++i) { const double d = std::abs (A.P.tableStartOf (i) - tt); if (d < bd) { bd = d; best = i; } }
    dropIndex = best;
    if (e.x > getWidth() - 20) scroll += 2 / pxPerSec * 10; else if (e.x < headW + 10) scroll = juce::jmax (0.0, scroll - 20 / pxPerSec);
    repaint();
}

void TableView::mouseUp (const juce::MouseEvent&)
{
    if (dragging && dragId >= 0 && dropIndex >= 0)
    {
        const int from = A.P.indexOf (dragId);
        int to = dropIndex > from ? dropIndex - 1 : dropIndex;
        if (from >= 0 && to != from) A.edit ([&] { A.P.move (from, to); });
    }
    dragging = false; dragId = -1; dropIndex = -1; repaint();
}

void TableView::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (! song && knife && e.y > rulerH + markH ? juce::MouseCursor::CrosshairCursor : juce::MouseCursor::NormalCursor);
}

void TableView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCtrlDown() || e.mods.isCommandDown()) { zoomBy (w.deltaY > 0 ? 1.25f : 0.8f, (float) e.x); return; }
    const float d = std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : w.deltaY;
    scroll = juce::jlimit (0.0, juce::jmax (0.0, totalLength() - 5), scroll - d * 300.0 / pxPerSec);
    repaint();
}

void TableView::pieceMenu (int pi, const juce::String& role)
{
    juce::PopupMenu m;
    const auto& pc = A.P.pieces[(size_t) pi];
    if (role.isNotEmpty())
    {
        const bool on = pc.off.count (role) == 0;
        m.addItem (1, (on ? "Remove " : "Restore ") + roleLabel (role) + " in this piece");
        m.addItem (2, "Use as reference for " + roleLabel (role));
    }
    m.addItem (3, "Use as reference for all instruments");
    m.addSeparator();
    m.addItem (4, "Split at cursor (S)");
    m.addItem (5, "Delete piece");
    juce::Component::SafePointer<TableView> self (this);
    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [self, pi, role] (int r)
    {
        if (self == nullptr || r == 0) return;
        auto& A = self->A;
        if (pi >= (int) A.P.pieces.size()) return;
        if (r == 1) A.edit ([&] { auto& o = A.P.pieces[(size_t) pi].off; if (o.count (role)) o.erase (role); else o.insert (role); });
        if (r == 2) A.edit ([&] { A.P.samples[role] = pi; });
        if (r == 3) A.edit ([&] { A.P.sample = pi; A.P.samples.clear(); });
        if (r == 4) A.splitAtCursor();
        if (r == 5) A.deleteSelected();
    });
}

void TableView::markerMenu (int pi)
{
    if (pi < 0) return;
    if (! A.selected.count (A.P.pieces[(size_t) pi].id)) { A.selected = { A.P.pieces[(size_t) pi].id }; A.changed(); }
    juce::PopupMenu m;
    for (int i = 0; i < 8; ++i) m.addItem (i + 1, kParts[i]);
    m.addSeparator(); m.addItem (100, "Remove label");
    juce::Component::SafePointer<TableView> self (this);
    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [self] (int r)
    {
        if (self == nullptr || r == 0) return;
        self->A.setPartForSelection (r == 100 ? juce::String() : juce::String (kParts[r - 1]));
    });
}
} // namespace dzod::arranger
