// MaterialInspector.cpp — see MaterialInspector.h.
#include "preview/MaterialInspector.h"
#include <algorithm>
#include <QApplication>
#include <QClipboard>
#include <QMouseEvent>
#include <QMenu>

#include <QFont>
#include <QFontMetrics>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QSet>
#include <QTimer>
#include <QScrollArea>
#include <QVBoxLayout>

#include "fox/FoxMaterial.h"
#include "util/Extract.h"

namespace {

// Card geometry. One place, because the painter and the widget's size hint
// have to agree exactly or the cards clip or float.
constexpr int kThumb = 62;      // one channel preview, square
constexpr int kGap = 5;
constexpr int kPad = 10;
// A map row is three bands: its title, the swatch strip, and the source path.
// The title spans the whole card so a role's meaning can be spelled out; the
// swatches sit under it in a single line the eye can scan straight down.
constexpr int kRowH = 15 + kThumb + 17;
// The header draws, from y = kPad: the title (17), the shader (16) and the
// geometry line (18). 10 + 17 + 16 + 18 = 61. It was 54, which left the card
// seven pixels short of its own padding — harmless today and a clip the
// moment another header line is added, which is exactly how this kind of
// constant goes wrong.
constexpr int kHeaderH = kPad + 17 + 16 + 18;
// Five swatches: the composite image plus up to four channels.
constexpr int kCardW = kPad * 2 + 5 * kThumb + 4 * kGap;

// Which channels of a map carry meaning, and what to call them. Fox packs
// different things into the same three bytes depending on the role, so the
// labels come from the ROLE and not from the channel index — an SRM's green is
// roughness, a base map's green is green.
struct ChannelPlan {
    const char* title;       // the row's name
    const char* names[4];    // per-channel captions; nullptr = channel unused
    bool composite = true;   // draw the RGB image itself as the first cell
};

ChannelPlan planFor(quint32 role)
{
    // Cell tags are kept to three characters. A tag shares its swatch with the
    // channel's mean value, and a word long enough to be a sentence pushes the
    // number off the edge — so the MEANING goes in the row title, which has
    // the width of the whole card to say it in.
    switch (role) {
    case fox::texrole::kBase:
    case fox::texrole::kBaseLin:
        return {"Base — albedo, and alpha for cutouts", {"R", "G", "B", "A"}, true};
    case fox::texrole::kNormal:
        // Already unswizzled to plain RGB by the loader (Fox stores x in ALPHA
        // and y in GREEN), so what is shown here is a normal map, not DXT5nm.
        return {"Normal — unswizzled from DXT5nm", {"x", "y", "z", nullptr}, true};
    case fox::texrole::kSpecular:
        return {"SRM — occlusion / roughness / reflection", {"AO", "Rgh", "Rfl", nullptr}, true};
    case fox::texrole::kTranslucent:
        return {"TRM — subsurface amount", {"T", nullptr, nullptr, nullptr}, false};
    case fox::texrole::kLayer:
        return {"Layer — the runtime colour swatch", {"R", "G", "B", nullptr}, true};
    case fox::texrole::kLayerMask:
        return {"Layer mask — where the colour applies", {"M", nullptr, nullptr, nullptr}, false};
    case fox::texrole::kDirty:
        // NOT drawn as a composite, deliberately. An RGB view of a DTM looks
        // like a coloured stain and reads as one; measured, its three channels
        // are UNCORRELATED greyscale masks (|corr| <= 0.21 between any pair,
        // never equal on a texel) packed into one file. Showing the composite
        // would be showing a picture the data does not contain. What the three
        // masks MEAN is not stated anywhere in the material — no Dirty
        // material in the shipped data carries a strength, a colour or a
        // combine parameter — so they are shown as three masks and named by
        // channel rather than captioned with a guess.
        return {"Dirty — three packed masks, uncorrelated (meaning unstated)",
                {"m0", "m1", "m2", nullptr}, false};
    case fox::texrole::kMatParamMap:
        return {"MTM — material-ID map into the FMTT presets",
                {"ID", nullptr, nullptr, nullptr}, false};
    case fox::texrole::kSubNormal:
    case fox::texrole::kTensionSubNrm:
    case fox::texrole::kInternalNrm:
        return {"Sub-normal — a second detail layer", {"x", "y", "z", nullptr}, true};
    case fox::texrole::kSubNormalMask:
        return {"Sub-normal mask", {"M", nullptr, nullptr, nullptr}, false};
    case fox::texrole::kShift:
        // Named rather than left to the generic "Map" caption, because what it
        // shows is the whole argument for how hair is shaded: a fitted atlas
        // of strand clumps in the base map's own UV layout, mostly black with
        // bright streaks — which is why it is read as an unsigned strand mask
        // and not as a signed ±0.5 shift.
        return {"Shift — hair strands, unsigned",
                {"S", nullptr, nullptr, nullptr}, false};
    case fox::texrole::kRoughness:
        return {"Roughness — no SRM on this material",
                {"Rgh", nullptr, nullptr, nullptr}, false};
    case fox::texrole::kMetalicLayer:
        return {"Metalic layer", {"R", "G", "B", nullptr}, true};
    case fox::texrole::kMetalicBact:
        return {"Metalic bacteria — Survive's crystal growth",
                {"R", "G", "B", nullptr}, true};
    case fox::texrole::kViewReflect:
        return {"View reflection — the eye's own highlight",
                {"R", "G", "B", nullptr}, true};
    case fox::texrole::kLensHeight:
        return {"Lens height", {"H", nullptr, nullptr, nullptr}, false};
    case fox::texrole::kBase2:
        return {"Base 2 — second albedo (the iris)", {"R", "G", "B", "A"}, true};
    case fox::texrole::kMask:
        return {"Mask", {"M", nullptr, nullptr, nullptr}, false};
    default:
        return {"Map", {"R", "G", "B", "A"}, true};
    }
}

// Reduce a map to a square thumbnail once. Aspect is deliberately NOT kept:
// these are being read as data, and a 2048x512 strip squeezed into a square
// shows more of itself than the same strip letterboxed into a sliver.
//
// COLOUR AND ALPHA ARE SCALED SEPARATELY, in formats that carry no alpha at
// all. Qt's smooth scaler has no path for a straight-alpha 32-bit format: it
// routes through ARGB32_PREMULTIPLIED, so every colour channel comes back
// multiplied by its own alpha. On a cutout map — hair, an eyelash, a decal
// sheet — that drags the albedo toward black wherever the texture is
// transparent, and the panel then reports a mean of 0.03 for a perfectly good
// base map. Which Qt version keeps which format through scaled() has also
// changed before, so relying on the input format surviving is not a fix.
//
// Scaling an alpha-free RGB888 buffer cannot premultiply anything, and the
// alpha is carried through the same way by replicating it into a second RGB
// image. The two are recombined here, so channelOf() below always reads a
// buffer this function built and the byte order is known rather than assumed.
QImage thumbOf(const QImage& src, int size)
{
    if (src.isNull()) return {};
    const QImage rgba = src.convertToFormat(QImage::Format_RGBA8888);

    // BOTH buffers are split out by hand rather than by convertToFormat().
    // Measured on Qt 6.4, RGBA8888 -> RGB888 does preserve colour under a zero
    // alpha — but that is a property of one converter in one Qt version, and
    // the whole point of this function is that the numbers underneath it can
    // be trusted on whatever Qt the app is built against. A twelve-line copy
    // makes the guarantee local instead of inherited.
    QImage colourSrc(rgba.width(), rgba.height(), QImage::Format_RGB888);
    QImage alphaSrc(rgba.width(), rgba.height(), QImage::Format_RGB888);
    for (int y = 0; y < rgba.height(); ++y) {
        const uchar* in = rgba.constScanLine(y);
        uchar* c = colourSrc.scanLine(y);
        uchar* out = alphaSrc.scanLine(y);
        for (int x = 0; x < rgba.width(); ++x) {
            c[x * 3 + 0] = in[x * 4 + 0];
            c[x * 3 + 1] = in[x * 4 + 1];
            c[x * 3 + 2] = in[x * 4 + 2];
            const uchar a = in[x * 4 + 3];
            out[x * 3 + 0] = a; out[x * 3 + 1] = a; out[x * 3 + 2] = a;
        }
    }
    const QImage colour =
        colourSrc.scaled(size, size, Qt::IgnoreAspectRatio,
                         Qt::SmoothTransformation)
            .convertToFormat(QImage::Format_RGB888);
    const QImage alpha =
        alphaSrc.scaled(size, size, Qt::IgnoreAspectRatio,
                        Qt::SmoothTransformation)
            .convertToFormat(QImage::Format_RGB888);

    QImage out(colour.width(), colour.height(), QImage::Format_RGBA8888);
    for (int y = 0; y < out.height(); ++y) {
        const uchar* c = colour.constScanLine(y);
        const uchar* a = alpha.constScanLine(y);
        uchar* o = out.scanLine(y);
        for (int x = 0; x < out.width(); ++x) {
            o[x * 4 + 0] = c[x * 3 + 0];
            o[x * 4 + 1] = c[x * 3 + 1];
            o[x * 4 + 2] = c[x * 3 + 2];
            o[x * 4 + 3] = a[x * 3 + 0];
        }
    }
    return out;
}

// One channel of a thumbnail, as greyscale, plus its mean in 0..1.
// `thumb` is always a buffer thumbOf() built, so its format is known: plain
// RGBA8888, straight alpha, byte 0 = red. The guard is here anyway, because a
// silently wrong channel index reads as a wrong measurement rather than as a
// broken picture, and that is the harder bug to notice.
QImage channelOf(const QImage& thumb, int c, double* mean)
{
    if (thumb.isNull() || c < 0 || c > 3
        || thumb.format() != QImage::Format_RGBA8888) {
        if (mean) *mean = -1.0;
        return {};
    }
    QImage out(thumb.width(), thumb.height(), QImage::Format_RGBA8888);
    qint64 sum = 0;
    for (int y = 0; y < thumb.height(); ++y) {
        const uchar* in = thumb.constScanLine(y);
        uchar* dst = out.scanLine(y);
        for (int x = 0; x < thumb.width(); ++x) {
            const uchar v = in[x * 4 + c];
            sum += v;
            dst[x * 4 + 0] = v; dst[x * 4 + 1] = v; dst[x * 4 + 2] = v;
            dst[x * 4 + 3] = 255;
        }
    }
    if (mean) {
        const qint64 n = qint64(thumb.width()) * thumb.height();
        *mean = n ? double(sum) / double(n) / 255.0 : -1.0;
    }
    return out;
}

// Fixed hues rather than palette roles: these are identity, not emphasis, and
// they have to mean the same thing under a light theme and a dark one.
QColor kindColour(fox::ShaderKind k)
{
    switch (k) {
    case fox::ShaderKind::Skin:            return QColor(214, 138, 106);
    case fox::ShaderKind::Cloth:           return QColor(120, 158, 196);
    case fox::ShaderKind::Hair:            return QColor(158, 120, 190);
    case fox::ShaderKind::Eye:             return QColor(104, 186, 176);
    case fox::ShaderKind::Glass:           return QColor(150, 200, 226);
    case fox::ShaderKind::Constant:        return QColor(206, 190, 110);
    case fox::ShaderKind::MetalicBacteria: return QColor(190, 110, 132);
    case fox::ShaderKind::Blin:            return QColor(132, 168, 132);
    case fox::ShaderKind::Other:           break;
    }
    return QColor(150, 150, 150);
}

QString featureText(const fox::MaterialModel& mm)
{
    static const char* kKind[] = {"Blin", "Skin", "Cloth", "Hair", "Eye",
                                  "Glass", "Constant", "MetalicBacteria",
                                  "Other"};
    QStringList f;
    if (mm.layerMul) f << QStringLiteral("layerMul");
    if (mm.layerBlend) f << QStringLiteral("layerBlend");
    if (mm.dirty) f << QStringLiteral("dirty");
    if (mm.subNormal) f << QStringLiteral("subNormal");
    if (mm.tension) f << QStringLiteral("tension");
    if (mm.incidence) f << QStringLiteral("incidence");
    if (mm.forward) f << QStringLiteral("forward");
    if (mm.alphaCutout) f << QStringLiteral("cutout");
    if (mm.materialTypes > 1) f << QStringLiteral("%1MT").arg(mm.materialTypes);
    QString out = QString::fromLatin1(kKind[int(mm.kind)]);
    if (!f.isEmpty()) out += QStringLiteral(" · ") + f.join(QStringLiteral(", "));
    return out;
}

// A small font that survives an application font set in PIXELS, where
// pointSizeF() returns -1 and every derived size collapses to the floor.
QFont smallFontFrom(const QFont& base)
{
    QFont f = base;
    if (base.pointSizeF() > 0.0)
        f.setPointSizeF(qMax(6.5, base.pointSizeF() - 1.5));
    else if (base.pixelSize() > 0)
        f.setPixelSize(qMax(9, base.pixelSize() - 2));
    return f;
}

// A label that owns a pre-painted card.
//
// SELECTABLE, because "export the images of these three materials" is a
// question about a set and the panel had no way to express one — the cards
// were pictures you could read and not things you could point at. Click,
// Ctrl+click and Shift+click behave as they do in every list in this
// application; the selected card draws a border rather than a wash, because a
// wash over a card whose whole content is colour swatches changes what the
// swatches read as.
class CardLabel : public QLabel {
public:
    explicit CardLabel(const QPixmap& pm, QWidget* parent)
        : QLabel(parent)
    {
        setPixmap(pm);
        setSelected(false);
        // deviceIndependentSize(), not size(): a pixmap carrying a device
        // pixel ratio reports its size in DEVICE pixels, so on a 2x display
        // every card widget came out twice the size of the picture QLabel
        // then drew inside it — a blank gutter beside every card and a
        // permanent horizontal scrollbar.
        setFixedSize(pm.deviceIndependentSize().toSize());
    }

    void setSelected(bool on)
    {
        m_selected = on;
        setStyleSheet(on ? QStringLiteral(
                               "border:2px solid #4aa3ff;border-radius:3px;")
                         : QStringLiteral("border:2px solid transparent;"));
    }
    bool selected() const { return m_selected; }

    std::function<void(Qt::KeyboardModifiers)> onPress;

protected:
    void mousePressEvent(QMouseEvent* e) override
    {
        // Right-click selects too, when the card is not already in the
        // selection: the context menu is about what you pointed at.
        if (onPress) onPress(e->modifiers());
        QLabel::mousePressEvent(e);
    }

private:
    bool m_selected = false;
};

}  // namespace

MaterialInspector::MaterialInspector(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(4);

    m_header = new QLabel(this);
    m_header->setWordWrap(true);
    m_header->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(m_header);

    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(
        QStringLiteral("Filter by material, shader, role or texture path…"));
    m_filter->setClearButtonEnabled(true);
    v->addWidget(m_filter);
    connect(m_filter, &QLineEdit::textChanged, this,
            [this](const QString&) { applyFilter(); });

    // The context menu is installed on the PANEL, not on each card: cards are
    // created and destroyed on every rebuild, and a policy set per card is a
    // policy that has to be re-set every time. showContextMenu resolves what
    // was under the cursor itself.
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& at) { showContextMenu(at); });

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_cards = new QWidget(m_scroll);
    m_cardsLayout = new QVBoxLayout(m_cards);
    m_cardsLayout->setContentsMargins(0, 0, 0, 0);
    m_cardsLayout->setSpacing(6);
    m_cardsLayout->addStretch(1);
    m_scroll->setWidget(m_cards);
    v->addWidget(m_scroll, 1);

    // Coalescing timer. Changing one slot in the Customize builder tears the
    // scene down a part at a time and then rebuilds it a part at a time, and
    // every one of those steps ends in rebuildScene(). Rebuilding the panel
    // synchronously on each meant a dozen full passes — every material, every
    // map, every decode — for one click, of which only the last was ever seen.
    m_rebuildTimer = new QTimer(this);
    m_rebuildTimer->setSingleShot(true);
    m_rebuildTimer->setInterval(60);
    connect(m_rebuildTimer, &QTimer::timeout, this, [this] { rebuild(); });
}

QVector<MaterialInspector::MaterialEntry>
MaterialInspector::entriesFor(const fox::FmdlFile& model)
{
    QVector<MaterialEntry> out;
    out.reserve(model.materials().size());
    for (const fox::FmdlMaterialInstance& mi : model.materials()) {
        MaterialEntry e;
        e.name = mi.name;
        e.shader = mi.shader;
        e.textures = mi.textures;
        e.params = mi.params;
        out.append(e);
    }
    for (int mi = 0; mi < model.meshes().size(); ++mi) {
        const fox::FmdlMesh& m = model.meshes()[mi];
        if (m.materialInstanceIndex < 0 || m.materialInstanceIndex >= out.size())
            continue;
        Use u;
        u.meshIndex = mi;
        u.tris = int(m.triangles.size() / 3);
        u.group = (m.meshGroupIndex >= 0
                   && m.meshGroupIndex < model.meshGroups().size())
            ? model.meshGroups()[m.meshGroupIndex].name
            : QStringLiteral("—");
        out[m.materialInstanceIndex].uses.append(u);
    }
    return out;
}

void MaterialInspector::clear()
{
    m_sources.clear();
    m_extraCache.clear();
    m_rebuildTimer->stop();
    rebuild();
}

void MaterialInspector::setSources(const QVector<Source>& sources)
{
    // Copied NOW, painted later. Everything a Source carries is by value, so
    // the caller may free or reallocate whatever it came from the instant this
    // returns — which a Customize scene rebuild does, repeatedly.
    m_sources = sources;
    m_rebuildTimer->start();
}

void MaterialInspector::setFilterText(const QString& text)
{
    if (m_filter) m_filter->setText(text);
}

void MaterialInspector::rebuild()
{
    // Tear down the old cards. The trailing stretch is kept — re-adding it
    // every time would stack one per rebuild and squash the list upward.
    for (QWidget* w : m_cardWidgets) { m_cardsLayout->removeWidget(w); delete w; }
    m_cardWidgets.clear();
    m_cardText.clear();
    // The selection is card INDICES, so it cannot survive a rebuild that
    // renumbers them — clearing it here is what stops a filter change from
    // exporting a material the user never pointed at.
    m_cardInfo.clear();
    m_selected.clear();
    m_anchor = -1;

    const QPalette pal = palette();
    const QColor cardBg = pal.color(QPalette::Base);
    const QColor cardEdge = pal.color(QPalette::Mid);
    const QColor fg = pal.color(QPalette::Text);
    const QColor dim = pal.color(QPalette::Disabled, QPalette::Text);

    int totalMats = 0, withSrm = 0, withTrm = 0, paintable = 0, withNrm = 0;

    for (const Source& src : m_sources) {
        for (int mat = 0; mat < src.materials.size(); ++mat) {
            const MaterialEntry& me = src.materials[mat];
            const fox::MaterialModel mm = fox::classifyShader(me.shader);
            ++totalMats;

            const QImage baseImg = src.base.value(mat);
            const QImage nrmImg = src.normals.value(mat);
            const GLPbrMaterial pm = src.pbr.value(mat);
            if (!nrmImg.isNull()) ++withNrm;
            if (!pm.material.isNull()) ++withSrm;
            if (!pm.translucent.isNull()) ++withTrm;
            if (mm.colourable()) ++paintable;

            // `bound` distinguishes what the slot RESOLVED to from what the
            // model DECLARES. They differ exactly when a variation or a gear
            // colour rebound the slot, which is the case a debug panel is
            // opened for — so the two are labelled differently rather than
            // conflated.
            struct Row { quint32 role; QImage img; QString path; bool bound; };
            QVector<Row> rows;
            // Which of the material's OWN references each drawn row consumed.
            // Matched by reference identity rather than by an assumed role
            // hash: the base loader takes any role CONTAINING "Base_Tex" and
            // the normal loader has a fallback of its own, so a material
            // binding Base_Tex_LIN would otherwise have its base map drawn
            // with no path and then decoded a second time as an extra row.
            QSet<quint32> consumed;

            const auto refFor = [&](quint32 role) -> const fox::FmdlTextureRef* {
                for (const fox::FmdlTextureRef& r : me.textures)
                    if (r.roleHash32 == role) return &r;
                return nullptr;
            };
            // These two mirror loadBaseTextures() and loadNormalMaps()
            // EXACTLY, including where the two differ from each other. The
            // panel's job is to name the texture the renderer used, so a pick
            // rule that is merely similar names the wrong file and then decodes
            // the right one a second time as a stray extra row.
            //
            // loadBaseTextures takes the FIRST role containing "Base_Tex" and
            // breaks — there is no preference for an exact match.
            const auto refBase = [&]() -> const fox::FmdlTextureRef* {
                for (const fox::FmdlTextureRef& r : me.textures)
                    if (r.role.contains(QLatin1String("Base_Tex"))) return &r;
                return nullptr;
            };
            // loadNormalMaps DOES run two passes, and also accepts the raw
            // hex form of the role — which is what every role looks like on an
            // install with no dictionary, a configuration that loader
            // deliberately supports.
            const auto refNormal = [&]() -> const fox::FmdlTextureRef* {
                const fox::FmdlTextureRef* fallback = nullptr;
                for (const fox::FmdlTextureRef& r : me.textures) {
                    if (r.role.startsWith(QLatin1String("NormalMap_Tex"))
                        || r.role == QLatin1String("0xcc4305511ae0"))
                        return &r;
                    if (!fallback && r.role.contains(QLatin1String("NormalMap_Tex"))
                        && !r.role.startsWith(QLatin1String("Sub")))
                        fallback = &r;
                }
                return fallback;
            };

            const auto addRow = [&](quint32 role, const QImage& img,
                                    const fox::FmdlTextureRef* ref,
                                    const QString& boundPath) {
                if (img.isNull()) return;
                const bool bound = !boundPath.isEmpty();
                QString path = boundPath;
                if (path.isEmpty() && ref)
                    path = ref->path.isEmpty()
                        ? QStringLiteral("0x%1").arg(ref->pathHash, 0, 16)
                        : ref->path;
                rows.append({role, img, path, bound});
                if (ref) consumed.insert(ref->roleHash32);
            };

            addRow(fox::texrole::kBase, baseImg, refBase(), QString());
            addRow(fox::texrole::kNormal, nrmImg, refNormal(), QString());
            // The SRM slot can hold a plain RoughnessMap when the material
            // binds no SpecularMap — loadPbrMaps falls back to it. Labelling
            // that as an SRM would caption a one-channel roughness map with
            // "AO / rough / reflect", so the role it actually loaded travels
            // with the image.
            addRow(pm.materialRole ? pm.materialRole : fox::texrole::kSpecular,
                   pm.material, refFor(pm.materialRole ? pm.materialRole
                                                       : fox::texrole::kSpecular),
                   pm.materialSource);
            addRow(fox::texrole::kTranslucent, pm.translucent,
                   refFor(fox::texrole::kTranslucent), pm.translucentSource);
            addRow(fox::texrole::kLayer, pm.layer,
                   refFor(fox::texrole::kLayer), pm.layerSource);
            addRow(fox::texrole::kLayerMask, pm.layerMask,
                   refFor(fox::texrole::kLayerMask), pm.layerMaskSource);
            addRow(fox::texrole::kMatParamMap, pm.matParamMap,
                   refFor(fox::texrole::kMatParamMap), pm.matParamMapSource);

            // Every OTHER role the material binds — the dirt map, the
            // material-ID map, the sub-normals — decoded here and nowhere
            // else. The renderer has no use for them yet, but "there is a
            // Dirty_Tex here and it is solid black" is exactly the kind of
            // thing this panel exists to say, and a name with no picture
            // cannot say it.
            //
            // At MIP resolution and cached by path: these are only ever seen
            // as 62-pixel swatches, and a full decode of a 2048-square dirt
            // map per material per rebuild froze the window on every slot
            // change.
            QStringList undecodable;
            for (const fox::FmdlTextureRef& r : me.textures) {
                if (consumed.contains(r.roleHash32)) continue;
                // The key has to identify the TEXTURE, and a PathCode64 does
                // not on a GZ model — every ref there has pathHash == 0 and is
                // resolved by path instead, exactly as textureImageFor does.
                const QString key =
                    (src.gz ? QStringLiteral("g|") : QStringLiteral("t|"))
                    + (r.pathHash ? QString::number(r.pathHash, 16) : r.path);
                QImage extra;
                const auto cached = m_extraCache.constFind(key);
                if (cached != m_extraCache.constEnd()) {
                    extra = cached.value();
                } else {
                    extra = extract::textureImageFor(r, src.gz, /*lowRes=*/true);
                    if (!extra.isNull())
                        extra = thumbOf(extra, kThumb);
                    m_extraCache.insert(key, extra);
                }
                if (extra.isNull()) {
                    // Not "not in this install": a decode can fail for a
                    // pixel format this build does not handle too, and only
                    // one of those two is the reader's problem.
                    undecodable << (r.role.isEmpty()
                                        ? QStringLiteral("0x%1").arg(r.roleHash32, 0, 16)
                                        : r.role);
                    continue;
                }
                rows.append({r.roleHash32, extra,
                             r.path.isEmpty()
                                 ? QStringLiteral("0x%1").arg(r.pathHash, 0, 16)
                                 : r.path,
                             false});
                consumed.insert(r.roleHash32);
            }

            // ── Geometry ──────────────────────────────────────────────────
            int tris = 0;
            QStringList groupNames, subs;
            for (const Use& x : me.uses) {
                tris += x.tris;
                if (!groupNames.contains(x.group)) groupNames << x.group;
                subs << QStringLiteral("mesh %1 · %2 tri · %3")
                            .arg(x.meshIndex).arg(x.tris).arg(x.group);
            }
            // What the FMTT said. This is the line that explains why one
            // model is gold and the identical one beside it is silver: the
            // only difference in the data is the preset number.
            QStringList presetLines;
            for (int k = 0; k < 4 && k < qMax(1, pm.materialTypes); ++k) {
                if (pm.presetIndex[k] < 0) continue;
                QString line = QStringLiteral("region %1 · preset %2")
                                   .arg(k).arg(pm.presetIndex[k]);
                if (pm.presetIndex[k] >= 256) {
                    line += QStringLiteral(" (none)");
                } else {
                    line += QStringLiteral(" · F0 %1 · spec %2 %3 %4")
                                .arg(pm.presetF0[k], 0, 'f', 3)
                                .arg(pm.presetSpec[k][0], 0, 'f', 2)
                                .arg(pm.presetSpec[k][1], 0, 'f', 2)
                                .arg(pm.presetSpec[k][2], 0, 'f', 2);
                    if (pm.presetF0[k] > 0.5f) line += QStringLiteral(" · METAL");
                }
                presetLines << line;
            }

            // ── THE MATERIAL'S OWN PARAMETERS ────────────────────────────
            // The float4s the shader reads, as the file states them. The
            // preset lines above are the INTERPRETATION of MatParamIndex_*;
            // these are the numbers themselves, plus every parameter nothing
            // in this tool interprets — TensionRate, TensionShift,
            // TensionController, the Edge pair — which is exactly why they
            // are here. A value that is read by nobody is still authored, and
            // an unread parameter that turns out to be non-zero across a whole
            // shader family is a question worth asking; with no way to see one
            // it was not askable.
            //
            // Zeros are NOT suppressed. Seventeen materials in the shipped
            // data bind a sub-normal and set its blend to zero deliberately,
            // and that measurement was only possible because the zero is data.
            const auto num = [](float v) {
                QString t = QString::number(double(v), 'g', 6);
                return t;
            };
            QStringList paramLines;
            for (const fox::FmdlMaterialParam& mp : me.params)
                paramLines << QStringLiteral("%1\t%2, %3, %4, %5")
                                  .arg(mp.name.isEmpty()
                                           ? QStringLiteral("0x%1").arg(mp.nameHash32, 8, 16, QLatin1Char('0'))
                                           : mp.name,
                                       num(mp.value[0]), num(mp.value[1]),
                                       num(mp.value[2]), num(mp.value[3]));
            // Capped for the same reason the submesh list is: past a point a
            // list stops being information and starts being a wall. The count
            // is on the caption either way, so nothing is hidden silently.
            const int kMaxParamLines = 8;
            const int paramShown = qMin(paramLines.size(), kMaxParamLines);
            const int paramBlock =
                paramLines.isEmpty()
                    ? 0
                    : 14 + paramShown * 13
                          + (paramLines.size() > kMaxParamLines ? 13 : 0);

            const QString geom =
                QStringLiteral("%1 · %2 mesh(es), %3 tri · %4")
                    .arg(featureText(mm))
                    .arg(me.uses.size())
                    .arg(tris)
                    .arg(groupNames.isEmpty() ? QStringLiteral("no group")
                                              : groupNames.join(QStringLiteral(", ")));

            // ── Paint ─────────────────────────────────────────────────────
            // At most four submesh lines are drawn; past that the list stops
            // being information and starts being a wall, so the rest are
            // summarised. The count is on the header line either way.
            const int subLines = qMin(subs.size() > 1 ? subs.size() : 0, 4)
                                 + ((subs.size() > 4) ? 1 : 0);
            const int h = kHeaderH + (undecodable.isEmpty() ? 0 : 16)
                          + subLines * 14 + presetLines.size() * 14
                          + paramBlock
                          + rows.size() * kRowH + kPad;
            const qreal dpr = devicePixelRatioF();
            QPixmap pm2(int(kCardW * dpr), int(h * dpr));
            pm2.setDevicePixelRatio(dpr);
            pm2.fill(Qt::transparent);
            QPainter p(&pm2);
            p.setRenderHint(QPainter::Antialiasing, true);
            // The swatches are 62 logical pixels drawn into a pixmap that may
            // be 2x that in device pixels; without this every one of them is
            // nearest-neighbour upscaled and reads as a much blockier texture
            // than it is.
            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            p.setPen(cardEdge);
            p.setBrush(cardBg);
            p.drawRoundedRect(QRectF(0.5, 0.5, kCardW - 1, h - 1), 5, 5);
            // A colour down the left edge per shader family. Forty cards is
            // more than anyone reads top to bottom, and "where does the skin
            // stop and the cloth start" is answerable at a glance with this
            // and not without it.
            p.setPen(Qt::NoPen);
            p.setBrush(kindColour(mm.kind));
            p.drawRect(QRect(1, 4, 4, h - 8));
            p.setRenderHint(QPainter::Antialiasing, false);
            // CLEAR THE BRUSH. drawRect() fills with the current brush, and
            // every cell outline below is a drawRect — leaving the card's own
            // background set repainted each swatch solid over the image that
            // had just been drawn into it. The numbers came out right and the
            // pictures came out blank, which reads as "the decode failed".
            p.setBrush(Qt::NoBrush);

            const QFont f = font();
            const QFont small = smallFontFrom(f);
            const QFontMetrics smallFm(small);
            int y = kPad;

            QFont bold = f;
            bold.setBold(true);
            p.setFont(bold);
            p.setPen(fg);
            const QString title =
                QStringLiteral("slot %1 · %2")
                    .arg(src.slotBase + mat)
                    .arg(me.name.isEmpty() ? QStringLiteral("(unnamed)") : me.name);
            p.drawText(QRect(kPad, y, kCardW - kPad * 2, 16),
                       Qt::AlignLeft | Qt::AlignVCenter, title);
            y += 17;

            p.setFont(f);
            p.setPen(fg);
            p.drawText(QRect(kPad, y, kCardW - kPad * 2, 15),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       me.shader.isEmpty() ? QStringLiteral("(no shader)") : me.shader);
            y += 16;

            p.setPen(dim);
            p.drawText(QRect(kPad, y, kCardW - kPad * 2, 15),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       QFontMetrics(f).elidedText(geom, Qt::ElideRight,
                                                  kCardW - kPad * 2));
            y += 18;

            QString searchText = src.label + QLatin1Char(' ') + title
                                 + QLatin1Char(' ') + me.shader
                                 + QLatin1Char(' ') + geom;

            if (subLines > 0) {
                p.setFont(small);
                p.setPen(dim);
                for (int i = 0; i < qMin(subs.size(), 4); ++i) {
                    p.drawText(QRect(kPad + 8, y, kCardW - kPad * 2 - 8, 13),
                               Qt::AlignLeft | Qt::AlignVCenter, subs[i]);
                    y += 14;
                }
                if (subs.size() > 4) {
                    p.drawText(QRect(kPad + 8, y, kCardW - kPad * 2 - 8, 13),
                               Qt::AlignLeft | Qt::AlignVCenter,
                               QStringLiteral("+%1 more").arg(subs.size() - 4));
                    y += 14;
                }
            }

            if (!presetLines.isEmpty()) {
                p.setFont(small);
                for (const QString& line : presetLines) {
                    // A metal row is worth spotting from across the panel.
                    p.setPen(line.endsWith(QLatin1String("METAL"))
                                 ? kindColour(fox::ShaderKind::Constant)
                                 : dim);
                    p.drawText(QRect(kPad + 8, y, kCardW - kPad * 2 - 8, 13),
                               Qt::AlignLeft | Qt::AlignVCenter,
                               smallFm.elidedText(line, Qt::ElideRight,
                                                  kCardW - kPad * 2 - 8));
                    y += 14;
                }
                searchText += QLatin1Char(' ') + presetLines.join(QLatin1Char(' '));
            }

            if (!paramLines.isEmpty()) {
                p.setFont(small);
                p.setPen(dim);
                p.drawText(QRect(kPad, y, kCardW - kPad * 2, 13),
                           Qt::AlignLeft | Qt::AlignVCenter,
                           QStringLiteral("shader parameters (%1)")
                               .arg(paramLines.size()));
                y += 14;
                // Two columns, BOTH LEFT-ALIGNED, with the value column
                // starting early. Right-aligning the value to the card's own
                // right edge is the obvious layout and it is wrong here: the
                // card is a fixed 350 px pixmap and the panel it sits in is
                // routinely narrower — 317 px in the shipped default — so
                // anything aligned to the card's right edge is painted off the
                // side of the panel. The names appeared and every value was
                // invisible, which reads as "the parameters have no values".
                // Found by opening the screenshot; no log line would have said
                // it. The column stop keeps the pair inside the narrow width
                // and still lines the numbers up, which is what the alignment
                // was for — the question these answer is usually "which of
                // these is not zero", and that is a column scan.
                constexpr int kNameCol = 140;
                for (int i = 0; i < paramShown; ++i) {
                    const QString name = paramLines[i].section(QLatin1Char('\t'), 0, 0);
                    const QString val = paramLines[i].section(QLatin1Char('\t'), 1);
                    // A parameter that is all zeros is dimmed rather than
                    // hidden: it is authored, and "this one is zero" is the
                    // answer often enough to be worth drawing.
                    p.setPen(val == QLatin1String("0, 0, 0, 0") ? dim : fg);
                    p.drawText(QRect(kPad + 8, y, kNameCol, 13),
                               Qt::AlignLeft | Qt::AlignVCenter,
                               smallFm.elidedText(name, Qt::ElideRight, kNameCol));
                    p.drawText(QRect(kPad + 8 + kNameCol, y,
                                     kCardW - kPad - 8 - kNameCol, 13),
                               Qt::AlignLeft | Qt::AlignVCenter,
                               smallFm.elidedText(val, Qt::ElideRight,
                                                  kCardW - kPad * 2 - 8 - kNameCol));
                    y += 13;
                }
                if (paramLines.size() > kMaxParamLines) {
                    p.setPen(dim);
                    p.drawText(QRect(kPad + 8, y, kCardW - kPad * 2 - 8, 13),
                               Qt::AlignLeft | Qt::AlignVCenter,
                               QStringLiteral("+%1 more")
                                   .arg(paramLines.size() - kMaxParamLines));
                    y += 13;
                }
                // Every parameter is searchable, including the ones the cap
                // did not draw — typing "Tension" has to find the material
                // whether or not its row happened to fit.
                for (const QString& pl : paramLines)
                    searchText += QLatin1Char(' ')
                        + QString(pl).replace(QLatin1Char('\t'), QLatin1Char(' '));
            }

            if (!undecodable.isEmpty()) {
                p.setFont(small);
                p.setPen(dim);
                p.drawText(QRect(kPad, y, kCardW - kPad * 2, 14),
                           Qt::AlignLeft | Qt::AlignVCenter,
                           smallFm.elidedText(
                               QStringLiteral("bound, did not decode: %1")
                                   .arg(undecodable.join(QStringLiteral(", "))),
                               Qt::ElideRight, kCardW - kPad * 2));
                y += 16;
            }

            for (const Row& row : rows) {
                const ChannelPlan plan = planFor(row.role);
                // The extras arrive already reduced (they are cached that
                // way), so re-running thumbOf on them would be two more full
                // passes over the pixels for no change.
                const QImage th =
                    (row.img.size() == QSize(kThumb, kThumb)
                     && row.img.format() == QImage::Format_RGBA8888)
                        ? row.img
                        : thumbOf(row.img, kThumb);

                // The layer's title carries its TILING. This is the panel
                // someone opens when a camo looks wrong of scale, and a 24x
                // repeat is invisible in a 62-pixel swatch of the untiled
                // swatch itself — the number is the only place it shows.
                QString title = QString::fromUtf8(plan.title);
                if (row.role == fox::texrole::kLayer
                    && (pm.layerRepeat[0] != 1.0f || pm.layerRepeat[1] != 1.0f
                        || pm.layerShift[0] != 0.0f
                        || pm.layerShift[1] != 0.0f)) {
                    title += QStringLiteral("  ·  tiled %1 x %2")
                                 .arg(pm.layerRepeat[0], 0, 'g', 4)
                                 .arg(pm.layerRepeat[1], 0, 'g', 4);
                    if (pm.layerShift[0] != 0.0f || pm.layerShift[1] != 0.0f)
                        title += QStringLiteral(", shift %1 / %2")
                                     .arg(pm.layerShift[0], 0, 'g', 4)
                                     .arg(pm.layerShift[1], 0, 'g', 4);
                }
                // The SUB-NORMAL's rate for the same reason the layer's is
                // shown: a weave tiled 80x is a few pixels across on screen and
                // invisible in a 62-pixel swatch of the untiled map, so the
                // number is the only place the rate is legible. On a material
                // with no layer this rate came from the plain URepeat_UV pair
                // rather than the SubNorm-named one — see ModelLoader — and
                // showing it here is what makes that reading checkable.
                if ((row.role == fox::texrole::kSubNormal
                     || row.role == fox::texrole::kTensionSubNrm
                     || row.role == fox::texrole::kInternalNrm)
                    && (pm.subRepeat[0] != 1.0f || pm.subRepeat[1] != 1.0f))
                    title += QStringLiteral("  ·  tiled %1 x %2")
                                 .arg(pm.subRepeat[0], 0, 'g', 4)
                                 .arg(pm.subRepeat[1], 0, 'g', 4);
                p.setFont(small);
                p.setPen(fg);
                p.drawText(QRect(kPad, y, kCardW - kPad * 2, 14),
                           Qt::AlignLeft | Qt::AlignVCenter,
                           smallFm.elidedText(title, Qt::ElideRight,
                                              kCardW - kPad * 2));

                int x = kPad;
                const int sy = y + 15;
                if (plan.composite && !th.isNull()) {
                    p.drawImage(QRect(x, sy, kThumb, kThumb), th);
                    p.setPen(cardEdge);
                    p.drawRect(QRect(x, sy, kThumb, kThumb));
                    x += kThumb + kGap;
                }
                for (int c = 0; c < 4; ++c) {
                    if (!plan.names[c]) continue;
                    double mean = -1.0;
                    const QImage ch = channelOf(th, c, &mean);
                    if (ch.isNull()) continue;
                    p.drawImage(QRect(x, sy, kThumb, kThumb), ch);
                    p.setPen(cardEdge);
                    p.drawRect(QRect(x, sy, kThumb, kThumb));
                    // The tag and the number sit INSIDE the swatch, on a
                    // dimmed strip. Beside it they would double the row
                    // height; under it they would collide with the path.
                    // Painted in fixed light-on-dark rather than palette
                    // colours, because what is behind them is an arbitrary
                    // image and the theme has nothing to say about it.
                    p.fillRect(QRect(x + 1, sy + kThumb - 14, kThumb - 1, 13),
                               QColor(0, 0, 0, 165));
                    p.setFont(small);
                    p.setPen(QColor(240, 240, 240));
                    p.drawText(QRect(x + 3, sy + kThumb - 14, kThumb - 6, 13),
                               Qt::AlignLeft | Qt::AlignVCenter,
                               QString::fromUtf8(plan.names[c]));
                    p.drawText(QRect(x + 3, sy + kThumb - 14, kThumb - 6, 13),
                               Qt::AlignRight | Qt::AlignVCenter,
                               QStringLiteral("%1").arg(mean, 0, 'f', 2));
                    x += kThumb + kGap;
                }

                // The source path last. Elided from the MIDDLE: the leaf name
                // identifies the texture and the /Assets/<game>/ prefix says
                // which game it came from, and both are worth keeping.
                const QString pathLine =
                    (row.bound ? QStringLiteral("bound: ")
                               : QStringLiteral("declares: "))
                    + (row.path.isEmpty() ? QStringLiteral("(unnamed texture)")
                                          : row.path);
                p.setFont(small);
                p.setPen(dim);
                p.drawText(QRect(kPad, sy + kThumb + 1, kCardW - kPad * 2, 15),
                           Qt::AlignLeft | Qt::AlignVCenter,
                           smallFm.elidedText(pathLine, Qt::ElideMiddle,
                                              kCardW - kPad * 2));
                searchText += QLatin1Char(' ') + row.path;
                y += kRowH;
            }
            p.end();

            auto* card = new CardLabel(pm2, m_cards);
            card->setToolTip(
                QStringLiteral("%1\n%2\n%3\n\nClick to select; Shift-click for "
                               "a range. Right-click to export this material's "
                               "images into a folder of its own.")
                    .arg(src.label, title, me.shader));
            const int myIdx = m_cardWidgets.size();
            card->onPress = [this, myIdx](Qt::KeyboardModifiers mods) {
                selectCard(myIdx, mods);
            };
            m_cardsLayout->insertWidget(m_cardsLayout->count() - 1, card);
            m_cardWidgets.append(card);
            m_cardText.append(searchText.toLower());
            // What an export of THIS card needs: the material's own name, for
            // the folder, and the textures it declares, for the files. Kept
            // beside the card rather than re-derived from the sources, because
            // the card index is what a click gives us and walking back from it
            // to a (source, material) pair is the kind of parallel indexing
            // that goes wrong the first time a source is filtered out.
            CardInfo ci;
            ci.material = me.name;
            ci.source = src.label;
            for (const fox::FmdlTextureRef& t : me.textures)
                ci.textures.append({t.role, t.path, t.pathHash});
            m_cardInfo.append(ci);
        }
    }

    m_summary = QStringLiteral(
                    "%1 source(s) · %2 material(s) · %3 normal, %4 SRM, "
                    "%5 translucency · %6 paintable")
                    .arg(m_sources.size())
                    .arg(totalMats)
                    .arg(withNrm)
                    .arg(withSrm)
                    .arg(withTrm)
                    .arg(paintable);
    m_header->setText(m_summary);
    applyFilter();
}

// Click / Ctrl+click / Shift+click, the same three gestures as every list in
// this application. The anchor is the last plainly-clicked card, which is what
// a range is measured from.
void MaterialInspector::selectCard(int idx, Qt::KeyboardModifiers mods)
{
    if (idx < 0 || idx >= m_cardWidgets.size()) return;
    if (mods & Qt::ShiftModifier) {
        const int from = m_anchor < 0 ? idx : m_anchor;
        m_selected.clear();
        for (int i = qMin(from, idx); i <= qMax(from, idx); ++i)
            if (!m_cardWidgets[i]->isHidden()) m_selected.insert(i);
    } else if (mods & Qt::ControlModifier) {
        if (m_selected.contains(idx)) m_selected.remove(idx);
        else m_selected.insert(idx);
        m_anchor = idx;
    } else {
        m_selected.clear();
        m_selected.insert(idx);
        m_anchor = idx;
    }
    syncSelection();
}

void MaterialInspector::syncSelection()
{
    for (int i = 0; i < m_cardWidgets.size(); ++i)
        if (auto* c = static_cast<CardLabel*>(m_cardWidgets[i]))
            c->setSelected(m_selected.contains(i));
}

QVector<MaterialInspector::CardInfo> MaterialInspector::selectedMaterials() const
{
    QVector<CardInfo> out;
    QList<int> idx(m_selected.constBegin(), m_selected.constEnd());
    std::sort(idx.begin(), idx.end());
    for (int i : idx)
        if (i >= 0 && i < m_cardInfo.size()) out.append(m_cardInfo[i]);
    return out;
}

bool MaterialInspector::selectMaterialNamed(const QString& name)
{
    if (name.isEmpty()) return false;
    for (int i = 0; i < m_cardInfo.size(); ++i) {
        if (m_cardInfo[i].material != name) continue;
        if (i < m_cardWidgets.size() && m_cardWidgets[i]->isHidden())
            return false;   // filtered out: selecting it would be invisible
        // Already the sole selection. A viewport pick reaches this twice — the
        // pick selects the parts-tree row, and the tree's own selection signal
        // asks for the same material again — and re-selecting would scroll the
        // panel a second time and log a second line for one click.
        if (m_selected.size() == 1 && m_selected.contains(i)) return true;
        m_selected.clear();
        m_selected.insert(i);
        m_anchor = i;
        syncSelection();
        if (m_scroll && i < m_cardWidgets.size())
            m_scroll->ensureWidgetVisible(m_cardWidgets[i]);
        return true;
    }
    return false;
}

void MaterialInspector::showContextMenu(const QPoint& at)
{
    // Which card is under the cursor. The cards are plain widgets in a
    // layout, so childAt on the scroll's content answers it directly.
    QWidget* w = m_cards->childAt(m_cards->mapFrom(this, at));
    int idx = -1;
    for (int i = 0; i < m_cardWidgets.size(); ++i)
        if (m_cardWidgets[i] == w) { idx = i; break; }
    if (idx >= 0 && !m_selected.contains(idx))
        selectCard(idx, Qt::NoModifier);
    const QVector<CardInfo> sel = selectedMaterials();
    if (sel.isEmpty()) return;
    QMenu m(this);
    const QString what = sel.size() == 1
                             ? sel.first().material
                             : QStringLiteral("%1 materials").arg(sel.size());
    m.addAction(QStringLiteral("Export %1's images…").arg(what), this,
                [this, sel] { Q_EMIT exportImagesRequested(sel); });
    m.addSeparator();
    if (sel.size() == 1)
        m.addAction(QStringLiteral("Copy material name (%1)")
                        .arg(sel.first().material),
                    this, [sel] {
                        QApplication::clipboard()->setText(sel.first().material);
                    });
    m.exec(mapToGlobal(at));
}

void MaterialInspector::applyFilter()
{
    const QString needle = m_filter ? m_filter->text().trimmed().toLower() : QString();
    int shown = 0;
    for (int i = 0; i < m_cardWidgets.size(); ++i) {
        const bool vis = needle.isEmpty() || m_cardText.value(i).contains(needle);
        m_cardWidgets[i]->setVisible(vis);
        if (vis) ++shown;
    }
    m_header->setText(needle.isEmpty()
                          ? m_summary
                          : m_summary + QStringLiteral("  ·  %1 shown").arg(shown));
}
