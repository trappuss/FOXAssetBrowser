// ViewGlyphs.h — painter-drawn icons for the viewport toolbar and the N-panel.
//
// Header-only and with no Q_OBJECT, so every viewport draws from one source and
// a tweak to a glyph retunes all three tabs at once. Drawn rather than shipped
// as files for the same reason the rest of this tool draws its own swatches:
// an icon that is code cannot go missing from a portable build, and it scales
// with the device pixel ratio without a second asset.
//
// 18px canvas at 1x. Anything smaller reads as a featureless dot at toolbar
// height, which is the mistake the first pass made.
#pragma once
#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QRadialGradient>
#include <QToolButton>
#include <QtMath>
#include <cmath>

namespace foxglyph {

constexpr int kSize = 18;

// The stroke colour. FROM THE PALETTE, not a fixed grey: this tool follows the
// system theme, and the first pass drew a mid grey that vanished into a light
// toolbar. Callers that draw onto their own dark surface — the N-panel's card
// — pass their own colour instead.
inline QColor ink()
{
    const QColor c = qApp ? qApp->palette().color(QPalette::WindowText)
                          : QColor(0xc8, 0xc8, 0xcc);
    // Pure black hides the shading inside the filled glyphs; ease it off the
    // end of the range without losing the contrast that made it readable.
    return c.value() < 40 ? QColor(0x30, 0x30, 0x36) : c;
}

inline QPixmap canvas(qreal dpr, int px = kSize)
{
    QPixmap pm(int(px * dpr), int(px * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    return pm;
}

// ── The viewport toolbar ────────────────────────────────────────────────────
// 0 wireframe · 1 skeleton · 2 normal maps · 3 PBR shading · 4 submesh tree ·
// 5 material inspector · 6 reset view · 7 export · 8 the N-panel itself ·
// 9 turntable · 10 debug view · 11 environment · 12 light · 13 a source model
// (the outliner's top-level rows) · 14 animations · 15 attachments ·
// 16 info · 17 display mode · 18 filter (a funnel) · 19 folder · 20 folder
// (open) · 21 texture · 22 animation clip · 23 one bone · 24 one submesh ·
// 25 a file · 26 a script · 27 a container · 28 audio · 29 a tick.
inline QPixmap toolGlyph(int kind, qreal dpr = 1.0, QColor stroke = QColor(),
                         int px = kSize)
{
    QPixmap pm = canvas(dpr, px);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    // EVERY GLYPH BELOW IS DRAWN IN AN 18-UNIT SPACE. Scaling the painter
    // rather than the finished pixmap is what lets the outliner ask for a 96px
    // icon and get one that is drawn, not upscaled — the pen scales with it, so
    // a big icon is the same design bigger rather than the same design blurred.
    if (px != kSize) p.scale(qreal(px) / kSize, qreal(px) / kSize);
    const QColor c = stroke.isValid() ? stroke : ink();
    QPen pen(c, 1.25);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const QPointF ctr(kSize / 2.0, kSize / 2.0);
    switch (kind) {
        case 0: {   // WIREFRAME — a wire sphere: outline, equator, meridian
            p.drawEllipse(ctr, 6.6, 6.6);
            p.drawEllipse(ctr, 6.6, 2.7);
            p.drawEllipse(ctr, 2.7, 6.6);
            break;
        }
        case 1: {   // SKELETON — two bones from a joint
            p.setBrush(c);
            p.drawEllipse(QPointF(4.5, 13.5), 1.7, 1.7);
            p.setBrush(Qt::NoBrush);
            p.drawLine(QPointF(4.5, 13.5), QPointF(9.0, 7.0));
            p.drawLine(QPointF(9.0, 7.0), QPointF(13.6, 4.2));
            p.drawLine(QPointF(9.0, 7.0), QPointF(13.0, 10.5));
            p.setBrush(c);
            p.drawEllipse(QPointF(9.0, 7.0), 1.5, 1.5);
            p.drawEllipse(QPointF(13.6, 4.2), 1.3, 1.3);
            break;
        }
        case 2: {   // NORMAL MAPS — the tangent-space RGB corner
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0x8a, 0x8a, 0xf0));
            p.drawRoundedRect(QRectF(2.5, 2.5, 13, 13), 2.5, 2.5);
            p.setPen(QPen(QColor(255, 255, 255, 190), 1.2));
            p.drawLine(QPointF(9, 12.5), QPointF(9, 5.5));
            p.drawLine(QPointF(9, 5.5), QPointF(6.6, 8.0));
            p.drawLine(QPointF(9, 5.5), QPointF(11.4, 8.0));
            break;
        }
        case 3: {   // PBR — a lit, glossy ball
            QRadialGradient g(QPointF(6.4, 6.0), 12.0);
            g.setColorAt(0.0, QColor(0xf2, 0xf2, 0xf4));
            g.setColorAt(0.55, QColor(0x9a, 0x9a, 0xa4));
            g.setColorAt(1.0, QColor(0x36, 0x36, 0x3c));
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawEllipse(ctr, 6.8, 6.8);
            p.setBrush(QColor(255, 255, 255, 225));
            p.drawEllipse(QPointF(6.3, 5.8), 1.5, 1.5);
            break;
        }
        case 4: {   // SUBMESH TREE — an indented list
            for (int i = 0; i < 3; ++i) {
                const qreal y = 4.5 + i * 4.2;
                const qreal x = 3.5 + (i == 0 ? 0.0 : 3.0);
                p.drawLine(QPointF(x, y), QPointF(14.5, y));
                if (i > 0) p.drawLine(QPointF(4.2, y - 4.2), QPointF(4.2, y));
            }
            break;
        }
        case 5: {   // MATERIALS — a swatch stack
            p.setBrush(QColor(0x6e, 0x7a, 0x92));
            p.drawRoundedRect(QRectF(2.5, 5.5, 9, 9), 1.6, 1.6);
            p.setBrush(QColor(0xb0, 0x9a, 0x6e));
            p.drawRoundedRect(QRectF(6.5, 3.5, 9, 9), 1.6, 1.6);
            break;
        }
        case 6: {   // RESET VIEW — a re-centre reticle
            p.drawEllipse(ctr, 5.4, 5.4);
            p.drawLine(QPointF(9, 1.6), QPointF(9, 4.4));
            p.drawLine(QPointF(9, 13.6), QPointF(9, 16.4));
            p.drawLine(QPointF(1.6, 9), QPointF(4.4, 9));
            p.drawLine(QPointF(13.6, 9), QPointF(16.4, 9));
            break;
        }
        case 7: {   // EXPORT — a box with an out-arrow
            p.drawPolyline(QPolygonF({QPointF(3.5, 11.0), QPointF(3.5, 14.5),
                                      QPointF(14.5, 14.5), QPointF(14.5, 11.0)}));
            p.drawLine(QPointF(9, 12.0), QPointF(9, 3.5));
            p.drawLine(QPointF(9, 3.5), QPointF(6.2, 6.4));
            p.drawLine(QPointF(9, 3.5), QPointF(11.8, 6.4));
            break;
        }
        case 8: {   // THE N-PANEL — a pane sliding in from the right edge
            p.drawRoundedRect(QRectF(2.2, 3.2, 13.6, 11.6), 1.8, 1.8);
            p.setBrush(QColor(c.red(), c.green(), c.blue(), 90));
            p.drawRect(QRectF(10.4, 3.2, 5.4, 11.6));
            break;
        }
        case 9: {   // TURNTABLE — an orbit arrow round a dot
            p.setBrush(c);
            p.drawEllipse(ctr, 1.7, 1.7);
            p.setBrush(Qt::NoBrush);
            QPainterPath arc;
            arc.arcMoveTo(QRectF(2.4, 2.4, 13.2, 13.2), 40.0);
            arc.arcTo(QRectF(2.4, 2.4, 13.2, 13.2), 40.0, 280.0);
            p.drawPath(arc);
            p.drawLine(QPointF(14.0, 5.6), QPointF(12.2, 4.0));
            p.drawLine(QPointF(14.0, 5.6), QPointF(12.0, 7.0));
            break;
        }
        case 10: {  // DEBUG VIEW — a half-split ball, lit and raw
            p.setPen(Qt::NoPen);
            QPainterPath left;
            left.moveTo(ctr);
            left.arcTo(QRectF(2.2, 2.2, 13.6, 13.6), 90.0, 180.0);
            left.closeSubpath();
            p.setBrush(QColor(0x9a, 0x9a, 0xa4));
            p.drawPath(left);
            QPainterPath right;
            right.moveTo(ctr);
            right.arcTo(QRectF(2.2, 2.2, 13.6, 13.6), 270.0, 180.0);
            right.closeSubpath();
            p.setBrush(QColor(0xd8, 0x64, 0x64));
            p.drawPath(right);
            p.setPen(QPen(c, 1.1));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(ctr, 6.8, 6.8);
            break;
        }
        case 11: {  // ENVIRONMENT — a horizon: sky over ground
            p.setPen(Qt::NoPen);
            QPainterPath clip;
            clip.addEllipse(ctr, 6.8, 6.8);
            p.setClipPath(clip);
            p.setBrush(QColor(0x5c, 0x76, 0xa6));
            p.drawRect(QRectF(1, 1, 16, 8.6));
            p.setBrush(QColor(0x6a, 0x5c, 0x44));
            p.drawRect(QRectF(1, 9.6, 16, 8));
            p.setClipping(false);
            p.setPen(QPen(c, 1.1));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(ctr, 6.8, 6.8);
            break;
        }
        case 13: {  // SOURCE MODEL — a boxed-up part
            p.drawPolyline(QPolygonF({QPointF(9, 2.6), QPointF(15.3, 6.0),
                                      QPointF(15.3, 12.4), QPointF(9, 15.8),
                                      QPointF(2.7, 12.4), QPointF(2.7, 6.0),
                                      QPointF(9, 2.6)}));
            p.drawLine(QPointF(2.7, 6.0), QPointF(9, 9.4));
            p.drawLine(QPointF(15.3, 6.0), QPointF(9, 9.4));
            p.drawLine(QPointF(9, 9.4), QPointF(9, 15.8));
            break;
        }
        case 12: {  // LIGHT — a sun
            p.setBrush(c);
            p.drawEllipse(ctr, 3.1, 3.1);
            p.setBrush(Qt::NoBrush);
            for (int i = 0; i < 8; ++i) {
                const double a = i * M_PI / 4.0;   // QtMath defines M_PI on MSVC too
                p.drawLine(QPointF(9 + std::cos(a) * 5.0, 9 + std::sin(a) * 5.0),
                           QPointF(9 + std::cos(a) * 7.4, 9 + std::sin(a) * 7.4));
            }
            break;
        }
        case 14: {  // ANIMATIONS — a timeline with two keys on it
            p.drawLine(QPointF(2.6, 9), QPointF(15.4, 9));
            p.setBrush(c);
            const auto key = [&](qreal x) {
                p.drawPolygon(QPolygonF({QPointF(x, 5.4), QPointF(x + 2.6, 9),
                                         QPointF(x, 12.6), QPointF(x - 2.6, 9)}));
            };
            key(6.0);
            key(12.6);
            p.setBrush(Qt::NoBrush);
            break;
        }
        case 15: {  // ATTACHMENTS — a body with a clipped-on piece
            p.drawRoundedRect(QRectF(2.6, 4.4, 8.0, 9.2), 1.4, 1.4);
            p.setBrush(c);
            p.drawRoundedRect(QRectF(9.4, 7.4, 6.0, 5.0), 1.2, 1.2);
            p.setBrush(Qt::NoBrush);
            p.drawLine(QPointF(6.6, 4.4), QPointF(6.6, 13.6));
            break;
        }
        case 16: {  // INFO — the letter i in a circle
            p.drawEllipse(ctr, 6.8, 6.8);
            p.setBrush(c);
            p.drawEllipse(QPointF(9, 5.6), 0.95, 0.95);
            p.setBrush(Qt::NoBrush);
            p.drawLine(QPointF(9, 8.4), QPointF(9, 13.0));
            break;
        }
        case 17: {  // DISPLAY MODE — three rows next to a 2x2 of tiles
            p.drawLine(QPointF(2.6, 5.4), QPointF(7.6, 5.4));
            p.drawLine(QPointF(2.6, 9.0), QPointF(7.6, 9.0));
            p.drawLine(QPointF(2.6, 12.6), QPointF(7.6, 12.6));
            p.drawRect(QRectF(10.2, 4.0, 2.6, 2.6));
            p.drawRect(QRectF(13.6, 4.0, 2.6, 2.6));
            p.drawRect(QRectF(10.2, 7.6, 2.6, 2.6));
            p.drawRect(QRectF(13.6, 7.6, 2.6, 2.6));
            break;
        }
        case 18: {  // FILTER — a funnel
            QPainterPath path;
            path.moveTo(2.8, 3.6);
            path.lineTo(15.2, 3.6);
            path.lineTo(10.4, 9.2);
            path.lineTo(10.4, 14.6);
            path.lineTo(7.6, 12.9);
            path.lineTo(7.6, 9.2);
            path.closeSubpath();
            p.drawPath(path);
            break;
        }
        case 19: {  // FOLDER — a tab-top folder, drawn not shipped
            // The platform's SP_DirIcon was tried first and the user was right
            // about it: a photographic OS folder beside eleven line glyphs
            // reads as a sticker someone dropped on the tool. Blender draws its
            // own, in the same weight as everything beside it, and so does this.
            QPainterPath f;
            f.moveTo(2.2, 13.8);
            f.lineTo(2.2, 5.4);
            f.lineTo(7.0, 5.4);
            f.lineTo(8.4, 7.0);
            f.lineTo(15.8, 7.0);
            f.lineTo(15.8, 13.8);
            f.closeSubpath();
            p.drawPath(f);
            // The lid seam, which is what makes it read as a folder rather
            // than as a box with a notch.
            p.drawLine(QPointF(2.2, 8.6), QPointF(15.8, 8.6));
            break;
        }
        case 20: {  // FOLDER, OPEN — the same body with the lid swung back
            QPainterPath f;
            f.moveTo(2.2, 13.6);
            f.lineTo(2.2, 5.4);
            f.lineTo(6.8, 5.4);
            f.lineTo(8.2, 7.0);
            f.lineTo(13.4, 7.0);
            f.lineTo(13.4, 8.8);
            p.drawPath(f);
            QPainterPath lid;
            lid.moveTo(2.2, 13.6);
            lid.lineTo(4.6, 8.8);
            lid.lineTo(16.2, 8.8);
            lid.lineTo(13.8, 13.6);
            lid.closeSubpath();
            p.drawPath(lid);
            break;
        }
        case 21: {  // TEXTURE — a picture: a frame with a horizon and a sun
            p.drawRoundedRect(QRectF(2.6, 3.8, 12.8, 10.4), 1.4, 1.4);
            p.drawEllipse(QPointF(6.2, 7.2), 1.35, 1.35);
            QPainterPath hill;
            hill.moveTo(3.4, 13.2);
            hill.lineTo(7.6, 9.0);
            hill.lineTo(10.4, 11.8);
            hill.lineTo(12.2, 10.0);
            hill.lineTo(14.6, 13.2);
            p.drawPath(hill);
            break;
        }
        case 22: {  // CLIP — one key on a track, with a play head
            p.drawLine(QPointF(2.4, 6.2), QPointF(15.6, 6.2));
            p.setBrush(c);
            QPainterPath key;   // a diamond, which is what a key IS everywhere
            key.moveTo(9.0, 4.0);
            key.lineTo(10.9, 6.2);
            key.lineTo(9.0, 8.4);
            key.lineTo(7.1, 6.2);
            key.closeSubpath();
            p.drawPath(key);
            QPainterPath play;
            play.moveTo(6.6, 9.8);
            play.lineTo(12.4, 12.6);
            play.lineTo(6.6, 15.4);
            play.closeSubpath();
            p.drawPath(play);
            p.setBrush(Qt::NoBrush);
            break;
        }
        case 23: {  // BONE — one bone: two joints and the shaft between them
            p.drawEllipse(QPointF(5.2, 12.8), 2.2, 2.2);
            p.drawEllipse(QPointF(12.8, 5.2), 1.5, 1.5);
            p.drawLine(QPointF(6.5, 11.0), QPointF(11.6, 6.4));
            p.drawLine(QPointF(3.9, 11.0), QPointF(11.6, 4.0));
            break;
        }
        case 24: {  // SUBMESH — a quad patch with its diagonal, the mesh idiom
            p.drawRect(QRectF(3.0, 4.4, 12.0, 9.2));
            p.drawLine(QPointF(9.0, 4.4), QPointF(9.0, 13.6));
            p.drawLine(QPointF(3.0, 9.0), QPointF(15.0, 9.0));
            p.drawLine(QPointF(3.0, 4.4), QPointF(15.0, 13.6));
            break;
        }
        case 29: {  // A TICK — the mark for a checked menu row
            QPen t(c, 2.0);
            t.setCapStyle(Qt::RoundCap);
            t.setJoinStyle(Qt::RoundJoin);
            p.setPen(t);
            QPainterPath v;
            v.moveTo(4.0, 9.4);
            v.lineTo(7.4, 13.0);
            v.lineTo(14.2, 5.2);
            p.drawPath(v);
            break;
        }
        case 25: {  // A FILE — a page with a folded corner
            QPainterPath pg;
            pg.moveTo(4.4, 2.6);
            pg.lineTo(10.6, 2.6);
            pg.lineTo(13.6, 5.8);
            pg.lineTo(13.6, 15.4);
            pg.lineTo(4.4, 15.4);
            pg.closeSubpath();
            p.drawPath(pg);
            QPainterPath fold;
            fold.moveTo(10.6, 2.6);
            fold.lineTo(10.6, 5.8);
            fold.lineTo(13.6, 5.8);
            p.drawPath(fold);
            break;
        }
        case 26: {  // A SCRIPT — a page with lines of code on it
            QPainterPath pg;
            pg.moveTo(4.4, 2.6);
            pg.lineTo(10.6, 2.6);
            pg.lineTo(13.6, 5.8);
            pg.lineTo(13.6, 15.4);
            pg.lineTo(4.4, 15.4);
            pg.closeSubpath();
            p.drawPath(pg);
            for (int i = 0; i < 3; ++i) {
                const qreal y = 8.4 + i * 2.4;
                p.drawLine(QPointF(6.3, y), QPointF(i == 2 ? 9.6 : 11.7, y));
            }
            break;
        }
        case 27: {  // A CONTAINER — a crate with a lid seam and a band
            p.drawRect(QRectF(2.8, 4.6, 12.4, 9.8));
            p.drawLine(QPointF(2.8, 7.6), QPointF(15.2, 7.6));
            p.drawLine(QPointF(9.0, 7.6), QPointF(9.0, 14.4));
            break;
        }
        case 28: {  // AUDIO — a speaker with one wave
            QPainterPath sp;
            sp.moveTo(4.0, 7.2);
            sp.lineTo(6.4, 7.2);
            sp.lineTo(9.2, 4.4);
            sp.lineTo(9.2, 13.6);
            sp.lineTo(6.4, 10.8);
            sp.lineTo(4.0, 10.8);
            sp.closeSubpath();
            p.drawPath(sp);
            p.drawArc(QRectF(9.0, 6.0, 5.0, 6.0), -60 * 16, 120 * 16);
            break;
        }
        default:
            break;   // an unknown kind draws nothing rather than guessing
    }
    p.end();
    return pm;
}

// The same glyph as a QIcon, which is what a QToolButton actually wants. Built
// at 1x and 2x so a HiDPI screen does not get a blurred upscale.
inline QIcon toolIcon(int kind, QColor stroke = QColor())
{
    QIcon ic;
    ic.addPixmap(toolGlyph(kind, 1.0, stroke));
    ic.addPixmap(toolGlyph(kind, 2.0, stroke));
    return ic;
}

// The same glyph DRAWN at `px`, for the outliner, whose icons follow the row
// zoom from 24 to 96 pixels. toolIcon() alone would hand Qt an 18px pixmap and
// let it upscale, which at 64px is a smear beside the crisp rendered
// thumbnails on the model rows next to it.
inline QIcon toolIconAt(int kind, int px, QColor stroke = QColor())
{
    QIcon ic;
    ic.addPixmap(toolGlyph(kind, 1.0, stroke, px));
    ic.addPixmap(toolGlyph(kind, 2.0, stroke, px));
    return ic;
}

// A glyph drawn INSET in a px-sized canvas — the pixmap is full size, so Qt
// never upscales it, but the drawing occupies `frac` of it.
//
// This is how the outliner gets a visual hierarchy out of one icon size. A
// folder and a rendered model are not the same kind of thing and should not
// have the same visual weight; drawing the structural glyphs at ~70% leaves
// the pictures dominant and the scaffolding quiet, which is exactly how
// Blender's outliner reads.
inline QIcon toolIconInset(int kind, int px, qreal frac,
                           QColor stroke = QColor())
{
    const auto one = [&](qreal dpr) {
        QPixmap pm = canvas(dpr, px);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const int inner = qMax(8, int(px * frac));
        p.drawPixmap(QPointF((px - inner) / 2.0, (px - inner) / 2.0),
                     toolGlyph(kind, dpr, stroke, inner));
        p.end();
        return pm;
    };
    QIcon ic;
    ic.addPixmap(one(1.0));
    ic.addPixmap(one(2.0));
    return ic;
}

// ── Toolbar builders ────────────────────────────────────────────────────────
//
// A viewport toolbar of five words in a row reads as a form; the same five as
// glyphs reads as a toolbar, which is what it is. The TOGGLES become icon-only
// checkable buttons (their names move into the tooltip's first line, so
// nothing is lost to a hover); the ACTIONS keep their text and gain an icon,
// because "Export scene .glb…" opening a file dialog is not something to
// discover by hovering.
//
// Both return a QToolButton, which is a QAbstractButton: `toggled`,
// `setChecked` and `isChecked` behave exactly as they did on the QCheckBox
// these replaced, so nothing downstream has to change.
inline QToolButton* glyphToggle(QWidget* parent, int glyph, const QString& name,
                                const QString& tip, QColor stroke = QColor())
{
    auto* b = new QToolButton(parent);
    b->setIcon(toolIcon(glyph, stroke));
    b->setIconSize(QSize(kSize, kSize));
    b->setCheckable(true);
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::NoFocus);   // the viewport keeps the keyboard
    b->setToolTip(tip.isEmpty() ? name : (name + QStringLiteral("\n\n") + tip));
    b->setAccessibleName(name);
    return b;
}

// Icon-only and NOT checkable — a one-shot action with no text, for a toolbar
// that has run out of room for words. glyphToggle with setCheckable(false)
// afterwards does the same thing and reads like a mistake.
inline QToolButton* glyphIcon(QWidget* parent, int glyph, const QString& name,
                              const QString& tip)
{
    auto* b = new QToolButton(parent);
    b->setIcon(toolIcon(glyph));
    b->setIconSize(QSize(kSize, kSize));
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::NoFocus);
    b->setToolTip(tip.isEmpty() ? name : (name + QStringLiteral("\n\n") + tip));
    b->setAccessibleName(name);
    return b;
}

inline QToolButton* glyphAction(QWidget* parent, int glyph, const QString& name,
                                const QString& tip, bool checkable = false)
{
    auto* b = new QToolButton(parent);
    b->setIcon(toolIcon(glyph));
    b->setIconSize(QSize(kSize, kSize));
    b->setText(name);
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    b->setCheckable(checkable);
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::NoFocus);
    if (!tip.isEmpty()) b->setToolTip(tip);
    return b;
}

}  // namespace foxglyph
