// Fullscreen selection + annotation surface.
//
// The screenshot is captured BEFORE this appears, so what you drag over is a
// still frame. Menus and cursors cannot change under you mid-selection, and the
// compositor is not doing capture work while you take your time.

#include "overlay.h"
#include "icons.h"

#include <QApplication>
#include <QDir>
#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QtMath>
#include <cmath>

namespace {

constexpr int kCell = 38;
constexpr int kPad = 7;
constexpr int kSepW = 11;
constexpr int kBarH = kCell + kPad * 2;
constexpr int kHandle = 9;      // visual size of a resize grip
constexpr int kGrab = 14;       // hit radius -- larger than the grip, so the
                                // handles are catchable without pixel-hunting

// Ink choices. Deliberately high-contrast against typical screen content: a
// palette of pastels would annotate invisibly over a light UI.
const QVector<QColor> &inkPalette()
{
    static const QVector<QColor> c = {
        QColor(240,  60,  60),   // red
        QColor(250, 150,  30),   // orange
        QColor(250, 215,  60),   // yellow
        QColor( 80, 210, 110),   // green
        QColor( 70, 150, 250),   // blue
        QColor(200, 110, 250),   // violet
        QColor(255, 255, 255),   // white
        QColor( 20,  20,  24),   // near-black
    };
    return c;
}

const QVector<ToolbarItem> &items()
{
    static QVector<ToolbarItem> v;
    if (!v.isEmpty())
        return v;
    v = {
        {ToolbarItem::ToolItem, Tool::Select, Action::None, {}, "Adjust selection"},
        {ToolbarItem::ToolItem, Tool::Rect, Action::None, {}, "Rectangle"},
        {ToolbarItem::ToolItem, Tool::Ellipse, Action::None, {}, "Ellipse"},
        {ToolbarItem::ToolItem, Tool::Arrow, Action::None, {}, "Arrow"},
        {ToolbarItem::ToolItem, Tool::Pen, Action::None, {}, "Pen"},
        {ToolbarItem::ToolItem, Tool::Highlight, Action::None, {}, "Marker"},
        {ToolbarItem::ToolItem, Tool::Text, Action::None, {}, "Text"},
        {ToolbarItem::Separator, Tool::Select, Action::None, {}, ""},
        {ToolbarItem::ToggleFill, Tool::Select, Action::None, {}, "Solid fill (F)"},
        {ToolbarItem::Separator, Tool::Select, Action::None, {}, ""},
    };
    for (const QColor &c : inkPalette())
        v.push_back({ToolbarItem::ColorItem, Tool::Select, Action::None, c, "Colour"});
    v.push_back({ToolbarItem::Separator, Tool::Select, Action::None, {}, ""});
    v.push_back({ToolbarItem::ActionItem, Tool::Select, Action::Undo, {}, "Undo (Ctrl+Z)"});
    v.push_back({ToolbarItem::ActionItem, Tool::Select, Action::Copy, {}, "Copy (Enter)"});
    v.push_back({ToolbarItem::ActionItem, Tool::Select, Action::Save, {}, "Save (Ctrl+S)"});
    v.push_back({ToolbarItem::ActionItem, Tool::Select, Action::Cancel, {}, "Cancel (Esc)"});
    return v;
}

// Swatches are narrower than buttons -- the bar is already wide and a colour
// chip does not need a 38px hit target to be findable.
int cellWidthFor(const ToolbarItem &it)
{
    switch (it.kind) {
    case ToolbarItem::Separator: return kSepW;
    case ToolbarItem::ColorItem: return 28;
    default: return kCell;
    }
}

// Relative luminance (WCAG). Used to pick ink that is actually readable on
// whatever the cell background turns out to be -- the accent colour is
// user-configurable, so "cyan on cyan" cannot be fixed by hardcoding a shade.
qreal luminance(const QColor &c)
{
    auto lin = [](qreal v) {
        v /= 255.0;
        return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(c.red()) + 0.7152 * lin(c.green()) + 0.0722 * lin(c.blue());
}

// Flatten a translucent layer over a backdrop so we reason about what the eye
// actually sees, not the nominal colour.
QColor flatten(const QColor &fg, const QColor &bg)
{
    const qreal a = fg.alphaF();
    return QColor(int(fg.red() * a + bg.red() * (1 - a)),
                  int(fg.green() * a + bg.green() * (1 - a)),
                  int(fg.blue() * a + bg.blue() * (1 - a)));
}

// Black or white, whichever contrasts more with `bg`. 0.45 rather than 0.5
// because mid-tone cyans sit just above the naive midpoint and would otherwise
// get white ink at barely 2:1 contrast.
QColor inkFor(const QColor &bg)
{
    return luminance(bg) > 0.45 ? QColor(18, 20, 24) : QColor(238, 240, 244);
}

// WCAG contrast ratio, 1.0 (identical) to 21.0 (black on white).
qreal contrastRatio(const QColor &a, const QColor &b)
{
    const qreal la = luminance(a), lb = luminance(b);
    return (qMax(la, lb) + 0.05) / (qMin(la, lb) + 0.05);
}

// The selected tool keeps a DARK cell and an accent-coloured icon, rather than
// inverting to a solid accent fill. That is the look that reads best here, and
// a light-on-dark glyph is the higher-contrast arrangement anyway: this
// desktop's cyan against the bar sits near 7:1.
//
// But the accent is user-configurable. If someone picks a dark accent it would
// vanish against the cell, so fall back to plain black/white below 3:1 -- the
// point of deriving this is that it cannot become unreadable.
QColor activeInk(const QColor &accent, const QColor &cellBg)
{
    return contrastRatio(accent, cellBg) >= 3.0 ? accent : inkFor(cellBg);
}

IconId iconFor(const ToolbarItem &it)
{
    if (it.kind == ToolbarItem::ActionItem) {
        switch (it.action) {
        case Action::Copy: return IconId::Copy;
        case Action::Save: return IconId::Save;
        case Action::Undo: return IconId::Undo;
        default: return IconId::Cancel;
        }
    }
    switch (it.tool) {
    case Tool::Rect: return IconId::Rect;
    case Tool::Ellipse: return IconId::Ellipse;
    case Tool::Arrow: return IconId::Arrow;
    case Tool::Pen: return IconId::Pen;
    case Tool::Highlight: return IconId::Marker;
    case Tool::Text: return IconId::Text;
    default: return IconId::Crop;
    }
}

// Pull the accent colour from the running KDE colour scheme so the tool matches
// the desktop instead of hardcoding a brand colour that clashes the moment the
// theme changes.
QColor kdeAccent()
{
    const QString path = QDir::homePath() + QStringLiteral("/.config/kdeglobals");
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            if (line.startsWith(QLatin1String("AccentColor="))) {
                const QStringList rgb = line.mid(12).split(QLatin1Char(','));
                if (rgb.size() == 3)
                    return QColor(rgb[0].toInt(), rgb[1].toInt(), rgb[2].toInt());
            }
        }
    }
    return QColor(99, 208, 223); // this desktop's cyan, as a sane fallback
}

void drawArrow(QPainter &p, const QPoint &from, const QPoint &to, int width)
{
    p.drawLine(from, to);
    const double angle = std::atan2(double(to.y() - from.y()), double(to.x() - from.x()));
    const double head = qMax(12.0, width * 4.0);
    const double spread = M_PI / 7.0;
    QPointF a(to.x() - head * std::cos(angle - spread), to.y() - head * std::sin(angle - spread));
    QPointF b(to.x() - head * std::cos(angle + spread), to.y() - head * std::sin(angle + spread));
    QPainterPath tip;
    tip.moveTo(to);
    tip.lineTo(a);
    tip.lineTo(b);
    tip.closeSubpath();
    p.fillPath(tip, p.pen().color());
}

void paintAnnotation(QPainter &p, const Annotation &a)
{
    QPen pen(a.color, a.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    // A filled shape still gets its outline drawn, so it keeps a crisp edge
    // against similarly-coloured content underneath.
    if (a.filled)
        p.setBrush(a.color);

    switch (a.tool) {
    case Tool::Rect:
        p.drawRect(a.rect.normalized());
        break;
    case Tool::Ellipse:
        p.drawEllipse(a.rect.normalized());
        break;
    case Tool::Arrow:
        drawArrow(p, a.rect.topLeft(), a.rect.bottomRight(), a.width);
        break;
    case Tool::Pen:
        p.drawPath(a.path);
        break;
    case Tool::Highlight: {
        QColor c = a.color;
        c.setAlpha(90);
        QPen hp(c, a.width * 5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(hp);
        p.drawPath(a.path);
        break;
    }
    case Tool::Text: {
        if (a.text.isEmpty())
            break;
        p.setFont(a.font);
        // Dark halo so light text stays legible over a bright screenshot --
        // annotations land on arbitrary backgrounds and a plain draw is often
        // invisible over white.
        p.setPen(QPen(QColor(0, 0, 0, 190)));
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                if (dx || dy)
                    p.drawText(a.pos + QPoint(dx, dy), a.text);
        p.setPen(QPen(a.color));
        p.drawText(a.pos, a.text);
        break;
    }
    case Tool::Select:
        break;
    }
}

} // namespace


Overlay::Overlay(const QImage &shot, QWidget *parent)
    : QWidget(parent), m_shot(shot)
{
    // NOT BypassWindowManagerHint: that is an X11 concept, and on Wayland it
    // leaves the surface unmanaged so the compositor never gives it keyboard
    // focus -- every shortcut silently does nothing while the mouse still works.
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setCursor(Qt::CrossCursor);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled, true);

    m_accent = kdeAccent();
    m_drawColor = inkPalette().at(0);   // red
}

QRect Overlay::normalizedSelection() const
{
    return m_haveSelection ? m_selection.normalized() : QRect();
}

// ---------------------------------------------------------------- toolbar ---

QRect Overlay::toolbarRect() const
{
    const QRect sel = normalizedSelection();
    if (sel.isNull())
        return QRect();

    int w = kPad * 2;
    for (const ToolbarItem &it : items())
        w += cellWidthFor(it);

    int x = qBound(6, sel.center().x() - w / 2, width() - w - 6);
    int y = sel.bottom() + 10;
    if (y + kBarH > height() - 4)
        y = sel.top() - kBarH - 10;      // flip above when there is no room below
    if (y < 4)
        y = qMin(height() - kBarH - 4, sel.bottom() + 10);   // last resort: inside
    return QRect(x, y, w, kBarH);
}

QRect Overlay::cellRect(int index) const
{
    const QRect bar = toolbarRect();
    if (bar.isNull())
        return QRect();
    int x = bar.x() + kPad;
    const auto &v = items();
    for (int i = 0; i < v.size(); ++i) {
        const int w = cellWidthFor(v[i]);
        if (i == index)
            return QRect(x, bar.y() + kPad, w, kCell);
        x += w;
    }
    return QRect();
}

int Overlay::itemAtPoint(const QPoint &pt) const
{
    const QRect bar = toolbarRect();
    if (bar.isNull() || !bar.contains(pt))
        return -1;
    const auto &v = items();
    for (int i = 0; i < v.size(); ++i) {
        if (v[i].kind == ToolbarItem::Separator)
            continue;
        if (cellRect(i).contains(pt))
            return i;
    }
    return -1;
}

void Overlay::drawToolbar(QPainter &p)
{
    const QRect bar = toolbarRect();
    if (bar.isNull())
        return;

    p.setRenderHint(QPainter::Antialiasing, true);

    // drawPath FILLS with the painter's current brush, and drawHandles() leaves
    // it set to the accent colour. Without clearing it, the 1px outline below
    // flooded the whole bar solid cyan -- and only when the Select tool was
    // active, since that is the only time handles are drawn. Explicit state,
    // not inherited state.
    p.setBrush(Qt::NoBrush);

    QPainterPath bg;
    bg.addRoundedRect(bar, 12, 12);
    p.fillPath(bg, QColor(38, 38, 42, 242));
    p.setPen(QPen(QColor(255, 255, 255, 28), 1));
    p.drawPath(bg);

    const auto &v = items();
    for (int i = 0; i < v.size(); ++i) {
        const QRect cell = cellRect(i);
        if (v[i].kind == ToolbarItem::Separator) {
            p.setPen(QPen(QColor(255, 255, 255, 40), 1));
            const int cx = cell.center().x();
            p.drawLine(cx, cell.top() + 7, cx, cell.bottom() - 7);
            continue;
        }

        // Colour chips paint themselves; the selected one gets a ring.
        if (v[i].kind == ToolbarItem::ColorItem) {
            const bool sel = v[i].color.rgb() == m_drawColor.rgb();
            QRect sw = cell.adjusted(6, 10, -6, -10);
            p.setPen(Qt::NoPen);
            p.setBrush(v[i].color);
            p.drawRoundedRect(sw, 4, 4);
            if (sel || i == m_hover) {
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(sel ? QColor(255, 255, 255) : QColor(255, 255, 255, 120),
                              sel ? 2.0 : 1.2));
                p.drawRoundedRect(sw.adjusted(-3, -3, 3, 3), 6, 6);
            }
            continue;
        }

        if (v[i].kind == ToolbarItem::ToggleFill) {
            const bool on = m_fill;
            QPainterPath hl;
            hl.addRoundedRect(cell.adjusted(3, 3, -3, -3), 8, 8);
            p.setPen(Qt::NoPen);
            p.fillPath(hl, QColor(255, 255, 255, on ? 34 : (i == m_hover ? 20 : 0)));
            const QColor ink = on ? activeInk(m_accent, QColor(67, 67, 70))
                                  : QColor(206, 209, 214);
            // A filled square when on, an outlined one when off -- the button
            // shows the state it will produce, not an abstract symbol.
            QRectF box(cell.center().x() - 8, cell.center().y() - 7, 16, 14);
            p.setPen(QPen(ink, 1.8));
            if (on) {
                p.setBrush(ink);
                p.drawRoundedRect(box, 2, 2);
            } else {
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(box, 2, 2);
            }
            p.setBrush(Qt::NoBrush);
            continue;
        }

        const bool active = v[i].kind == ToolbarItem::ToolItem && v[i].tool == m_tool;
        const bool hovered = i == m_hover;

        const QColor barBg(38, 38, 42);
        QColor cellBg = barBg;

        if (active || hovered) {
            QPainterPath hl;
            hl.addRoundedRect(cell.adjusted(3, 3, -3, -3), 8, 8);
            // Lift the cell slightly rather than flooding it with the accent --
            // the icon carries the colour, the base stays neutral.
            QColor fill = QColor(255, 255, 255, active ? 34 : 20);
            p.fillPath(hl, fill);
            cellBg = flatten(fill, barBg);

            if (active) {
                QColor edge = m_accent;
                edge.setAlpha(120);
                p.setPen(QPen(edge, 1.3));
                p.setBrush(Qt::NoBrush);   // stroke only; drawPath would fill
                p.drawPath(hl);
            }
        }

        QColor ink = active ? activeInk(m_accent, cellBg)
                            : (hovered ? QColor(245, 246, 248) : QColor(206, 209, 214));
        if (v[i].action == Action::Cancel && (hovered || active))
            ink = QColor(242, 110, 110);

        drawIcon(p, iconFor(v[i]), QRectF(cell.adjusted(9, 9, -9, -9)), ink);
    }

    // Tooltip for the hovered cell, above the bar.
    if (m_hover >= 0 && m_hover < v.size() && *v[m_hover].tip) {
        QFont f = p.font();
        f.setPointSizeF(8.6);
        p.setFont(f);
        const QString tip = QString::fromLatin1(v[m_hover].tip);
        QRect tr = p.fontMetrics().boundingRect(tip).adjusted(-8, -4, 8, 4);
        tr.moveCenter(QPoint(cellRect(m_hover).center().x(), bar.top() - 15));
        tr.moveLeft(qBound(6, tr.left(), width() - tr.width() - 6));
        QPainterPath tb;
        tb.addRoundedRect(tr, 6, 6);
        p.fillPath(tb, QColor(28, 28, 32, 235));
        p.setPen(QColor(232, 234, 238));
        p.drawText(tr, Qt::AlignCenter, tip);
    }
}

// ------------------------------------------------------- selection handles ---

void Overlay::drawHandles(QPainter &p, const QRect &sel)
{
    const QPoint pts[8] = {
        sel.topLeft(), {sel.center().x(), sel.top()}, sel.topRight(),
        {sel.right(), sel.center().y()}, sel.bottomRight(),
        {sel.center().x(), sel.bottom()}, sel.bottomLeft(),
        {sel.left(), sel.center().y()}
    };
    p.save();   // contain the accent brush -- see the note in drawToolbar()
    p.setRenderHint(QPainter::Antialiasing, true);
    for (const QPoint &pt : pts) {
        QRect h(0, 0, kHandle, kHandle);
        h.moveCenter(pt);
        p.setPen(QPen(QColor(20, 20, 24, 200), 1));
        p.setBrush(m_accent);
        p.drawRoundedRect(h, 2, 2);
    }
    p.restore();
}

Handle Overlay::handleAtPoint(const QPoint &pt) const
{
    const QRect sel = normalizedSelection();
    if (sel.isNull())
        return Handle::None;

    const struct { Handle h; QPoint p; } grips[8] = {
        {Handle::TopLeft, sel.topLeft()},
        {Handle::Top, {sel.center().x(), sel.top()}},
        {Handle::TopRight, sel.topRight()},
        {Handle::Right, {sel.right(), sel.center().y()}},
        {Handle::BottomRight, sel.bottomRight()},
        {Handle::Bottom, {sel.center().x(), sel.bottom()}},
        {Handle::BottomLeft, sel.bottomLeft()},
        {Handle::Left, {sel.left(), sel.center().y()}},
    };
    for (const auto &g : grips) {
        if ((pt - g.p).manhattanLength() <= kGrab)
            return g.h;
    }
    if (sel.contains(pt))
        return Handle::Move;
    return Handle::None;
}

void Overlay::applyHandleDrag(const QPoint &pos)
{
    const QPoint d = pos - m_dragStartPos;
    QRect r = m_dragStartSel;

    switch (m_activeHandle) {
    case Handle::Move:
        r.translate(d);
        // Keep it on screen; a selection dragged off the edge cannot be
        // grabbed back.
        if (r.left() < 0) r.moveLeft(0);
        if (r.top() < 0) r.moveTop(0);
        if (r.right() > width() - 1) r.moveRight(width() - 1);
        if (r.bottom() > height() - 1) r.moveBottom(height() - 1);
        break;
    case Handle::TopLeft:     r.setTopLeft(r.topLeft() + d); break;
    case Handle::Top:         r.setTop(r.top() + d.y()); break;
    case Handle::TopRight:    r.setTopRight(r.topRight() + d); break;
    case Handle::Right:       r.setRight(r.right() + d.x()); break;
    case Handle::BottomRight: r.setBottomRight(r.bottomRight() + d); break;
    case Handle::Bottom:      r.setBottom(r.bottom() + d.y()); break;
    case Handle::BottomLeft:  r.setBottomLeft(r.bottomLeft() + d); break;
    case Handle::Left:        r.setLeft(r.left() + d.x()); break;
    default: return;
    }
    m_selection = r.intersected(rect());
}

void Overlay::updateCursor(const QPoint &pos)
{
    if (itemAtPoint(pos) >= 0) {
        setCursor(Qt::ArrowCursor);
        return;
    }
    if (m_tool == Tool::Text) {
        setCursor(Qt::IBeamCursor);
        return;
    }
    if (m_tool != Tool::Select || !m_haveSelection) {
        setCursor(Qt::CrossCursor);
        return;
    }
    switch (handleAtPoint(pos)) {
    case Handle::TopLeft:
    case Handle::BottomRight: setCursor(Qt::SizeFDiagCursor); break;
    case Handle::TopRight:
    case Handle::BottomLeft:  setCursor(Qt::SizeBDiagCursor); break;
    case Handle::Top:
    case Handle::Bottom:      setCursor(Qt::SizeVerCursor); break;
    case Handle::Left:
    case Handle::Right:       setCursor(Qt::SizeHorCursor); break;
    case Handle::Move:        setCursor(Qt::SizeAllCursor); break;
    default:                  setCursor(Qt::CrossCursor); break;
    }
}

// ------------------------------------------------------------------ paint ---

void Overlay::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.drawImage(rect(), m_shot);

    const QRect sel = normalizedSelection();

    p.setBrush(QColor(0, 0, 0, 135));
    p.setPen(Qt::NoPen);
    if (sel.isNull()) {
        p.drawRect(rect());
    } else {
        QPainterPath dim;
        dim.addRect(rect());
        QPainterPath hole;
        hole.addRect(sel);
        p.drawPath(dim.subtracted(hole));

        p.setPen(QPen(m_accent, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(sel.adjusted(-1, -1, 0, 0));

        const QString label = QStringLiteral("%1 × %2").arg(sel.width()).arg(sel.height());
        QFont f = p.font();
        f.setPointSizeF(9);
        f.setBold(true);
        p.setFont(f);
        QRect box = p.fontMetrics().boundingRect(label).adjusted(-9, -5, 9, 5);
        box.moveTopLeft(QPoint(sel.left(), qMax(2, sel.top() - box.height() - 6)));
        QPainterPath bg;
        bg.addRoundedRect(box, 6, 6);
        p.fillPath(bg, QColor(28, 28, 32, 225));
        p.setPen(m_accent);
        p.drawText(box, Qt::AlignCenter, label);
    }

    // Annotations clip to the selection so strokes cannot bleed outside it.
    p.save();
    if (!sel.isNull())
        p.setClipRect(sel);
    p.setRenderHint(QPainter::Antialiasing, true);
    for (const Annotation &a : m_annotations)
        paintAnnotation(p, a);
    if (m_drawing)
        paintAnnotation(p, m_pending);

    if (m_typing) {
        Annotation live;
        live.tool = Tool::Text;
        live.text = m_textBuffer;
        live.pos = m_textPos;
        live.color = m_drawColor;
        live.font = font();
        live.font.setPointSizeF(15);
        live.font.setBold(true);
        paintAnnotation(p, live);

        // Caret so it is obvious the app is accepting keystrokes.
        QFontMetrics fm(live.font);
        const int cx = m_textPos.x() + fm.horizontalAdvance(m_textBuffer) + 1;
        p.setPen(QPen(m_accent, 2));
        p.drawLine(cx, m_textPos.y() - fm.ascent(), cx, m_textPos.y() + fm.descent());
    }
    p.restore();

    if (!sel.isNull()) {
        if (m_tool == Tool::Select && !m_selecting)
            drawHandles(p, sel);
        drawToolbar(p);
    }

    if (sel.isNull()) {
        p.setPen(QColor(238, 240, 244));
        QFont f = p.font();
        f.setPointSizeF(11.5);
        p.setFont(f);
        p.drawText(rect(), Qt::AlignCenter,
                   QStringLiteral("Drag to select a region     ·     Esc to cancel"));
    }
}

// ------------------------------------------------------------------ input ---

void Overlay::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton)
        return;
    // Each screen has its own overlay. Keystrokes follow the one being used,
    // otherwise Enter/Esc stay stuck on the screen that opened first.
    activateWindow();
    grabKeyboard();
    const QPoint pos = e->pos();

    const int idx = itemAtPoint(pos);
    if (idx >= 0) {
        const ToolbarItem &it = items()[idx];
        if (it.kind == ToolbarItem::ColorItem) {
            m_drawColor = it.color;
        } else if (it.kind == ToolbarItem::ToggleFill) {
            m_fill = !m_fill;
        } else if (it.kind == ToolbarItem::ToolItem) {
            commitText();
            m_tool = it.tool;
        } else {
            switch (it.action) {
            case Action::Copy: commitText(); accept(false); return;
            case Action::Save: commitText(); accept(true); return;
            case Action::Cancel:
                m_result = QImage();
                emit finished(QImage(), false);
                close();
                return;
            case Action::Undo:
                if (m_typing) {
                    m_textBuffer.clear();
                    m_typing = false;
                } else if (!m_annotations.isEmpty()) {
                    m_annotations.removeLast();
                }
                break;
            default: break;
            }
        }
        update();
        return;
    }

    if (m_typing)
        commitText();

    // No selection yet: the first drag defines it.
    if (!m_haveSelection) {
        m_selecting = true;
        m_origin = pos;
        m_selection = QRect(pos, pos);
        m_haveSelection = true;
        return;
    }

    // Select tool: grab a handle to resize, or the interior to move.
    if (m_tool == Tool::Select) {
        const Handle h = handleAtPoint(pos);
        if (h != Handle::None) {
            m_activeHandle = h;
            m_dragStartSel = normalizedSelection();
            m_dragStartPos = pos;
            return;
        }
        // Clicking outside the selection starts a fresh one.
        m_selecting = true;
        m_origin = pos;
        m_selection = QRect(pos, pos);
        return;
    }

    if (m_tool == Tool::Text) {
        m_typing = true;
        m_textBuffer.clear();
        m_textPos = pos;
        update();
        return;
    }

    m_drawing = true;
    m_pending = Annotation{};
    m_pending.tool = m_tool;
    m_pending.color = m_drawColor;
    m_pending.filled = m_fill;
    m_pending.width = m_penWidth;
    m_pending.rect = QRect(pos, pos);
    if (m_tool == Tool::Pen || m_tool == Tool::Highlight)
        m_pending.path.moveTo(pos);
}

void Overlay::mouseMoveEvent(QMouseEvent *e)
{
    const QPoint pos = e->pos();

    const int hov = itemAtPoint(pos);
    if (hov != m_hover) {
        m_hover = hov;
        update();
    }
    updateCursor(pos);

    if (m_activeHandle != Handle::None) {
        applyHandleDrag(pos);
        update();
        return;
    }
    if (m_selecting) {
        m_selection = QRect(m_origin, pos);
        update();
        return;
    }
    if (m_drawing) {
        if (m_pending.tool == Tool::Pen || m_pending.tool == Tool::Highlight)
            m_pending.path.lineTo(pos);
        else
            m_pending.rect.setBottomRight(pos);
        update();
    }
}

void Overlay::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton)
        return;

    if (m_activeHandle != Handle::None) {
        m_activeHandle = Handle::None;
        m_selection = m_selection.normalized();
        update();
        return;
    }
    if (m_selecting) {
        m_selecting = false;
        if (m_selection.normalized().width() < 4 || m_selection.normalized().height() < 4) {
            m_haveSelection = false;   // a click without a drag is a misfire
        } else {
            m_selection = m_selection.normalized();
            if (m_tool == Tool::Select)
                m_tool = Tool::Select;  // stay in adjust mode; handles now live
        }
        update();
        return;
    }
    if (m_drawing) {
        m_drawing = false;
        m_annotations.append(m_pending);
        update();
    }
}

void Overlay::commitText()
{
    if (!m_typing)
        return;
    if (!m_textBuffer.isEmpty()) {
        Annotation a;
        a.tool = Tool::Text;
        a.text = m_textBuffer;
        a.pos = m_textPos;
        a.color = m_drawColor;
        a.font = font();
        a.font.setPointSizeF(15);
        a.font.setBold(true);
        m_annotations.append(a);
    }
    m_typing = false;
    m_textBuffer.clear();
}

void Overlay::inputMethodEvent(QInputMethodEvent *e)
{
    if (m_typing && !e->commitString().isEmpty()) {
        m_textBuffer += e->commitString();
        update();
    }
    e->accept();
}

void Overlay::keyPressEvent(QKeyEvent *e)
{
    // While typing, most keys are literal text -- only a few stay as commands.
    if (m_typing) {
        if (e->key() == Qt::Key_Escape) {
            m_typing = false;
            m_textBuffer.clear();
            update();
            return;
        }
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            commitText();
            update();
            return;
        }
        if (e->key() == Qt::Key_Backspace) {
            m_textBuffer.chop(1);
            update();
            return;
        }
        if (!e->text().isEmpty() && e->text().at(0).isPrint()) {
            m_textBuffer += e->text();
            update();
            return;
        }
        return;
    }

    switch (e->key()) {
    case Qt::Key_Escape:
        m_result = QImage();
        emit finished(QImage(), false);
        close();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        accept(false);
        return;
    case Qt::Key_S:
        if (e->modifiers() & Qt::ControlModifier) { accept(true); return; }
        break;
    case Qt::Key_C:
        if (e->modifiers() & Qt::ControlModifier) { accept(false); return; }
        break;
    case Qt::Key_Z:
        if ((e->modifiers() & Qt::ControlModifier) && !m_annotations.isEmpty()) {
            m_annotations.removeLast();
            update();
            return;
        }
        break;
    case Qt::Key_A:
        if (e->modifiers() & Qt::ControlModifier) {
            m_selection = rect();
            m_haveSelection = true;
            update();
            return;
        }
        break;
    case Qt::Key_F:
        m_fill = !m_fill;
        update();
        return;
    case Qt::Key_BracketLeft:
        m_penWidth = qMax(1, m_penWidth - 1);
        update();
        return;
    case Qt::Key_BracketRight:
        m_penWidth = qMin(24, m_penWidth + 1);
        update();
        return;
    default:
        break;
    }

    if (e->key() >= Qt::Key_1 && e->key() <= Qt::Key_7) {
        static const Tool order[7] = {Tool::Select, Tool::Rect, Tool::Ellipse, Tool::Arrow,
                                      Tool::Pen, Tool::Highlight, Tool::Text};
        m_tool = order[e->key() - Qt::Key_1];
        update();
    }
}

// ----------------------------------------------------------------- output ---

QImage Overlay::composite() const
{
    QRect sel = normalizedSelection();
    if (sel.isNull())
        sel = rect();
    sel = sel.intersected(rect());
    if (sel.isEmpty())
        return QImage();

    // Widget coords -> image pixels. On a scaled display the captured image is
    // larger than the widget; ignoring that crops the wrong region and softens
    // every screenshot.
    const double sx = double(m_shot.width()) / double(width());
    const double sy = double(m_shot.height()) / double(height());
    const QRect src(qRound(sel.x() * sx), qRound(sel.y() * sy),
                    qRound(sel.width() * sx), qRound(sel.height() * sy));

    QImage out = m_shot.copy(src.intersected(m_shot.rect()));
    if (out.isNull())
        return out;

    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.scale(sx, sy);
    p.translate(-sel.topLeft());
    for (const Annotation &a : m_annotations)
        paintAnnotation(p, a);
    return out;
}

void Overlay::accept(bool save)
{
    m_result = composite();
    // Clipboard ownership is established by main(), which also holds the process
    // alive to serve the data. Setting it here as well would hand ownership over
    // twice and race the hold logic.
    releaseKeyboard();
    emit finished(m_result, save);
    close();
}
