#include "view/ViewportPanel.h"

#include <QTimer>

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QSlider>
#include <QLayout>
#include <QMargins>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QToolButton>
#include <QBoxLayout>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QtMath>

#include "app/Config.h"
#include "app/Hotkeys.h"
#include <QVariant>

#include "export/ExportOptions.h"
#include "export/ViewCapture.h"
#include "gl/GLModelWidget.h"
#include "view/ViewGlyphs.h"
#include "view/ViewportBar.h"
#include "view/ViewportGizmo.h"
#include "view/ViewportHud.h"

namespace fox {
namespace {

constexpr int kPanelW = 258;
constexpr int kMargin = 8;

// The card's own look. Translucent so the model stays readable behind it —
// this is an overlay ON the viewport, and a panel that hides a third of the
// character is worse than no panel.
const char* const kCardQss =
    "QWidget#fabViewCard{background:rgba(24,26,30,232);border:1px solid "
    "rgba(255,255,255,26);border-radius:6px;}"
    "QWidget#fabViewCard QLabel{color:#c9c9cf;}"
    "QWidget#fabViewCard QCheckBox{color:#c9c9cf;}";

// A label that wraps, stays small and does not fight the layout for width.
QLabel* noteLabel(QWidget* parent)
{
    auto* l = new QLabel(parent);
    l->setWordWrap(true);
    l->setStyleSheet(QStringLiteral("color:#8f8f98;font-size:10px;"));
    l->setMinimumHeight(1);
    return l;
}

QSlider* mkSlider(QWidget* parent, int lo, int hi, int v)
{
    auto* s = new QSlider(Qt::Horizontal, parent);
    s->setRange(lo, hi);
    s->setValue(v);
    s->setMinimumWidth(120);
    return s;
}

}  // namespace

ViewportPanel::ViewportPanel(GLModelWidget* view)
    : QWidget(view), m_view(view)
{
    setObjectName(QStringLiteral("fabViewCard"));
    setStyleSheet(QLatin1String(kCardQss));
    setFixedWidth(kPanelW);
    setAttribute(Qt::WA_StyledBackground, true);
    // The panel must never steal the drag: a click on the card is a click on
    // the card, and everywhere else belongs to the camera.
    setFocusPolicy(Qt::ClickFocus);

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(8, 6, 8, 8);
    v->setSpacing(6);

    // ── The card's head: which topic this is, and a way out ─────────────────
    // A TITLE, not a tab strip. The strip existed to move between four pages
    // in one card; the bar's three buttons each open their own topic now, so a
    // second way to switch page inside the card would be a control for a
    // choice the user has already made (template §5: popovers, not a panel).
    auto* tabs = new QWidget(this);
    auto* th = new QHBoxLayout(tabs);
    th->setContentsMargins(0, 0, 0, 0);
    th->setSpacing(2);
    m_title = new QLabel(tabs);
    m_title->setStyleSheet(QStringLiteral("color:#e8e8ee;font-weight:600;"));
    th->addWidget(m_title);
    th->addStretch(1);
    auto* close = new QToolButton(tabs);
    close->setText(QStringLiteral("✕"));
    close->setStyleSheet(QStringLiteral(
        "QToolButton{border:none;background:transparent;color:#9a9aa2;}"
        "QToolButton:hover{color:#e8e8ee;}"));
    close->setAutoRaise(true);
    close->setFixedSize(20, 24);
    close->setToolTip(QStringLiteral("Close (N)"));
    connect(close, &QToolButton::clicked, this, [this] { setPanelOpen(false); });
    th->addWidget(close);
    v->addWidget(tabs);

    m_pages = new QStackedWidget(this);
    // In a SCROLL AREA, for the reason the settings dialog puts every tab in
    // one (template §10): a page whose content is taller than the card gets
    // clipped, silently, and the first thing to go is the wrapped explanatory
    // note at the top — which is the part that was worth reading. Measured
    // here: the Graphics page's shading note lost its last two lines the first
    // time it was shown, because a word-wrapped label's sizeHint is computed
    // before the card has a width to wrap to.
    m_scroll = new QScrollArea(this);
    m_scroll->setWidget(m_pages);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // SCOPED to the scroll area and its viewport by type. A bare
    // "background:transparent" here cascades to every descendant, and the
    // first casualty was the Channel combo, which came up as a black
    // rectangle with no text in it.
    m_scroll->setStyleSheet(QStringLiteral(
        "QScrollArea{background:transparent;border:none;}"
        "QScrollArea > QWidget > QWidget{background:transparent;}"));
    v->addWidget(m_scroll);

    buildGraphicsPage();
    buildLightingPage();
    buildCameraPage();
    buildOverlaysPage();

    if (m_view) {
        m_view->installEventFilter(this);
        connect(m_view, &GLModelWidget::displayChanged, this,
                &ViewportPanel::syncFromView);
        // A new scene can change what the debug channels are able to say —
        // the SRM-derived ones read shader defaults when the maps were never
        // loaded — so the channel list is re-evaluated per scene, not once.
        connect(m_view, &GLModelWidget::sceneChanged, this,
                &ViewportPanel::syncFromView);
        // Only while the panel is up: the turntable emits this at 60 Hz, and
        // formatting three numbers into a hidden label sixty times a second is
        // work nobody asked for.
        connect(m_view, &GLModelWidget::cameraChanged, this, [this] {
            if (!isVisible() || !m_camRead || !m_view) return;
            m_camRead->setText(
                QStringLiteral("yaw %1°   pitch %2°   distance %3\n"
                               "Background %4")
                    .arg(qRound(m_view->cameraYaw()))
                    .arg(qRound(m_view->cameraPitch()))
                    .arg(m_view->cameraDistance(), 0, 'f', 2)
                    .arg(m_view->backgroundColor().name(QColor::HexRgb)));
        });
    }
    // setPage, not showPage: showPage OPENS the panel, and opening it here
    // would show the card for one event loop turn before the hide() below took
    // it away again — a flash on every tab that builds a viewport.
    setPage(0);
    hide();
    syncFromView();
    reposition();
}

// ── Pages ───────────────────────────────────────────────────────────────────

void ViewportPanel::buildGraphicsPage()
{
    auto* page = new QWidget(m_pages);
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    // What the shading balls are currently saying, in words. The balls are
    // pictures and pictures are fast to read and impossible to be precise in;
    // this is where "Rendered means the SRM and the FMTT F0" gets said.
    m_shadingNote = noteLabel(page);
    v->addWidget(m_shadingNote);

    auto* dl = new QLabel(QStringLiteral("Channel"), page);
    v->addWidget(dl);
    m_debug = new QComboBox(page);
    for (DebugView d : debugViews())
        m_debug->addItem(QString::fromLatin1(debugViewName(d)), int(d));
    v->addWidget(m_debug);
    m_debugNote = noteLabel(page);
    v->addWidget(m_debugNote);
    connect(m_debug, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (!m_debug) return;
        const DebugView d = DebugView(m_debug->itemData(i).toInt());
        if (!m_syncing && m_view) m_view->setDebugView(d);
        if (m_debugNote)
            m_debugNote->setText(QString::fromLatin1(debugViewNote(d)));
        // The notes are three lines for some channels and one for others, and
        // the card's height is set when it opens. Without this the long ones
        // are clipped until the panel is closed and opened again.
        reposition();
    });

    // ── Materials ───────────────────────────────────────────────────────
    // These two were a pair of glyph toggles on the Models tab's toolbar,
    // beside the wireframe and skeleton buttons the shading balls had already
    // replaced. They are viewport state — what the surface is lit BY — and the
    // header's old argument for keeping normal maps out of here was that
    // ticking PBR can trigger a reload. That is a fact about the OWNING TAB,
    // not about the switch: the tab watches displayChanged and fetches the maps
    // when its own viewport asks for them, exactly as it did when it owned the
    // button. The switch belongs where the rest of the shading state is.
    auto* ml = new QLabel(QStringLiteral("Materials"), page);
    v->addWidget(ml);
    m_normalMaps = new QCheckBox(QStringLiteral("Normal maps"), page);
    m_normalMaps->setToolTip(QStringLiteral(
        "Light the surface through each material's tangent-space normal map "
        "(Fox DXT5nm). Off = flat vertex-normal shading.\n\nGreyed out when "
        "nothing in this scene has one."));
    v->addWidget(m_normalMaps);
    connect(m_normalMaps, &QCheckBox::toggled, this, [this](bool on) {
        if (m_syncing || !m_view) return;
        m_view->setNormalMapping(on);
    });
    m_pbr = new QCheckBox(QStringLiteral("PBR shading"), page);
    m_pbr->setToolTip(QStringLiteral(
        "Physically based shading: occlusion, roughness and reflection from "
        "the material's SRM, translucency from its TRM, and the runtime colour "
        "layer. Off, the same scene draws with the flat lambert this viewport "
        "has always used — so ticking it back and forth is a direct A/B on "
        "identical geometry.\n\nIf the maps were not loaded for this tab "
        "(Settings → Full PBR shading), ticking it loads them once."));
    v->addWidget(m_pbr);
    connect(m_pbr, &QCheckBox::toggled, this, [this](bool on) {
        if (m_syncing || !m_view) return;
        m_view->setPbrShading(on);
    });

    // THE OVERLAYS ARE NOT ON THIS PAGE ANY MORE. They were here on the
    // argument that "what is drawn on top of the model" is a graphics
    // question — which it is, and it was still the wrong place: the bar's
    // overlay button is a master switch whose LIST lived two clicks away
    // under a different heading, and the same eight switches then appeared
    // in a dropdown beside that button as well. Three copies of one set.
    //
    // One copy now, on its own page (buildOverlaysPage), opened by its own
    // button on the bar — beside the master switch it is the list of.
    v->addStretch(1);
    m_pages->addWidget(page);
}

// Light AND World, on one card. Template §5 names three popovers — Graphics,
// Camera, Lighting — and "which environment is this lit in" is a lighting
// question by any reading; splitting them was an artefact of the tab strip
// having room for four glyphs.
void ViewportPanel::buildLightingPage()
{
    auto* page = new QWidget(m_pages);
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(6);
    auto* form = new QWidget(page);
    auto* f = new QFormLayout(form);
    f->setContentsMargins(0, 0, 0, 0);
    f->setSpacing(4);
    f->setLabelAlignment(Qt::AlignLeft);
    f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_az = mkSlider(form, -180, 180, 0);
    m_el = mkSlider(form, -89, 89, 0);
    // 0..400 == 0.00..4.00. Integer sliders with a documented scale rather
    // than a float widget: the scale is the only thing a reader needs and a
    // spin box per row would take the card's whole width.
    m_keyGain = mkSlider(form, 0, 400, 100);
    m_ambGain = mkSlider(form, 0, 400, 100);
    m_exposure = mkSlider(form, 5, 400, 100);
    f->addRow(QStringLiteral("Direction"), m_az);
    f->addRow(QStringLiteral("Height"), m_el);
    f->addRow(QStringLiteral("Key"), m_keyGain);
    f->addRow(QStringLiteral("Ambient"), m_ambGain);
    f->addRow(QStringLiteral("Exposure"), m_exposure);

    m_follow = new QCheckBox(QStringLiteral("Light follows the camera"), form);
    m_follow->setToolTip(QStringLiteral(
        "The key rides over your shoulder instead of staying put in the "
        "world. Good for reading a normal map, useless for judging form — a "
        "shape lit from where you are looking has no shading to read."));
    f->addRow(m_follow);

    m_lightRead = noteLabel(form);
    f->addRow(m_lightRead);

    auto* reset = new QPushButton(QStringLiteral("Reset the whole rig"), form);
    reset->setToolTip(QStringLiteral(
        "Back to the rig this viewport had before any of this existed: the "
        "Default environment, its background, the original key direction, and "
        "every gain at 1. It clears the World page too — a reset that left you "
        "on Night's colours would not be a reset."));
    f->addRow(reset);

    const auto push = [this] {
        if (m_syncing || !m_view) return;
        m_view->setKeyAngles(float(m_az->value()), float(m_el->value()));
        m_view->setKeyIntensity(m_keyGain->value() / 100.0f);
        m_view->setAmbientIntensity(m_ambGain->value() / 100.0f);
        m_view->setExposure(m_exposure->value() / 100.0f);
        syncFromView();
    };
    for (QSlider* s : {m_az, m_el, m_keyGain, m_ambGain, m_exposure})
        connect(s, &QSlider::valueChanged, this, push);
    connect(m_follow, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_syncing && m_view) m_view->setKeyFollowsCamera(on);
    });
    connect(reset, &QPushButton::clicked, this, [this] {
        if (!m_view) return;
        float az = 0.0f, el = 0.0f;
        // Same conversion GLModelWidget's constructor does, from the same
        // source, so "reset" means the original light and not a second
        // opinion about what it was.
        const QVector3D to = -legacyKeyDirection().normalized();
        el = float(qRadiansToDegrees(std::asin(qBound(-1.0f, to.y(), 1.0f))));
        az = float(qRadiansToDegrees(std::atan2(to.x(), to.z())));
        // The environment and the background override are part of the rig, so
        // they go back too — setEnvironment carries the preset's own exposure,
        // which is why it runs before the gains.
        m_view->setEnvironment(ViewEnvironment::presets().first());
        m_view->setBackgroundColor(QColor());
        m_view->setKeyAngles(az, el);
        m_view->setKeyFollowsCamera(false);
        m_view->setKeyIntensity(1.0f);
        m_view->setAmbientIntensity(1.0f);
        m_view->setExposure(m_view->environment().exposure);
        syncFromView();
        reposition();
    });
    outer->addWidget(form);

    auto* world = new QWidget(page);
    auto* v = new QVBoxLayout(world);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);
    v->addWidget(new QLabel(QStringLiteral("Environment"), world));
    m_env = new QComboBox(world);
    // AUTO first, and it is the one most people want: an install holds up to
    // four games and they do not look alike. The per-game rigs are listed
    // under a separator so it is obvious which entries are a game's look and
    // which are neutral tools.
    m_env->addItem(QStringLiteral("Auto — follow the game"),
                   ViewEnvironment::autoId());
    for (const ViewEnvironment& e : ViewEnvironment::presets())
        if (e.game == GameId::Unknown) m_env->addItem(e.name, e.id);
    bool sep = false;
    for (const ViewEnvironment& e : ViewEnvironment::presets())
        if (e.game != GameId::Unknown) {
            if (!sep) { m_env->insertSeparator(m_env->count()); sep = true; }
            m_env->addItem(e.name, e.id);
        }
    v->addWidget(m_env);
    m_envNote = noteLabel(world);
    v->addWidget(m_envNote);
    connect(m_env, &QComboBox::currentIndexChanged, this, [this](int i) {
        applyEnvironment(i);
        reposition();
    });

    m_bg = new QToolButton(world);
    m_bg->setText(QStringLiteral("Background colour…"));
    m_bg->setToolTip(QStringLiteral(
        "Override the environment's own background. Choosing a new "
        "environment does NOT clear the override — use the button below."));
    m_bg->setToolButtonStyle(Qt::ToolButtonTextOnly);
    v->addWidget(m_bg);
    connect(m_bg, &QToolButton::clicked, this, [this] {
        if (!m_view) return;
        const QColor c = QColorDialog::getColor(
            m_view->backgroundColor(), this,
            QStringLiteral("Viewport background"));
        // getColor spins a nested event loop. Nothing can destroy the view
        // without destroying this panel with it today, but the guard belongs
        // on the far side of a nested loop, not only on the near side.
        if (!m_view) return;
        if (c.isValid()) m_view->setBackgroundColor(c);
        syncFromView();
    });
    auto* clearBg = new QToolButton(world);
    clearBg->setText(QStringLiteral("Use the environment's background"));
    clearBg->setToolButtonStyle(Qt::ToolButtonTextOnly);
    v->addWidget(clearBg);
    connect(clearBg, &QToolButton::clicked, this, [this] {
        if (m_view) m_view->setBackgroundColor(QColor());
        syncFromView();
    });
    v->addStretch(1);
    outer->addWidget(world);
    m_pages->addWidget(page);
}

// ── Overlays ────────────────────────────────────────────────────────────────
// A page of its own, opened by the bar's overlay button. Everything drawn OVER
// the model, behind one master gate (§3.3), in ONE place — they were on the
// Graphics page AND in a dropdown caret beside the master switch, which is two
// lists of the same eight booleans that could disagree about which was ticked.
void ViewportPanel::buildOverlaysPage()
{
    auto* page = new QWidget(m_pages);
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    m_overlayMaster = new QCheckBox(QStringLiteral("Show overlays"), page);
    m_overlayMaster->setToolTip(QStringLiteral(
        "One switch over all of them. Off hides every overlay and remembers "
        "which were on; the same switch is the button on the viewport bar that "
        "opens this page."));
    v->addWidget(m_overlayMaster);
    connect(m_overlayMaster, &QCheckBox::toggled, this, [this](bool on) {
        if (m_syncing || !m_bar) return;
        m_bar->setOverlaysMaster(on);
    });
    QLabel* gateNote = noteLabel(page);
    gateNote->setText(QStringLiteral(
        "Everything below is drawn over the model and never into a capture, a "
        "turntable or an export — they describe the scene rather than being "
        "part of it."));
    v->addWidget(gateNote);

    // ONE table, ViewportBar's, so this page and anything else that lists the
    // overlays cannot drift apart about what exists or what it is called.
    struct Tip { const char* key; const char* tip; };
    static const Tip kTips[] = {
        {"stats",
         "Meshes, triangles, materials, bones and the shading mode, top-left. "
         "Counted for the SCENE at load, so unticking a submesh does not "
         "change them — it is a description of the asset, not of this frame."},
        {"grid",
         "A grid on the ground plane, sized to the model: the square is a "
         "round number of metres chosen from the scene's own radius, so it "
         "reads under a pistol and under a horse."},
        {"axes",
         "X red, Y green, Z blue drawn in the SCENE from the world origin — "
         "which on a Fox character is the floor between the feet. This is a "
         "measurement of where the model sits. The corner gizmo is the "
         "navigation control; this is the annotation."},
        {"skeleton",
         "Every bone as a line to its parent, drawn over the model and posed "
         "with it."},
        {"bonenames",
         "The bone name at each bone, following the pose. A character is ~490 "
         "bones, so this is capped at the first 200 on screen and says so."},
        {"connectpoints",
         "Fox's hardpoints: the .fcnp sockets this model carries, as crosses "
         "with their names. A socket hung off a bone travels with that bone, "
         "exactly as it does in an export."},
        {"selection",
         "The ring around a selected submesh — a screen-space silhouette of "
         "constant width, not a wireframe, whose apparent thickness scaled "
         "with the mesh's triangle density.\n\nWarm = selected. Blue = the "
         "parts a context menu is about."},
        {"gizmo",
         "The navigation gizmo in the top-right corner: hover a ball to see "
         "its axis, click it to look down that axis, click the ring around it "
         "to switch between perspective and orthographic."},
    };
    for (const ViewportBar::OverlayDef& d : ViewportBar::overlayDefs()) {
        auto* b = new QCheckBox(d.label, page);
        for (const Tip& t : kTips)
            if (d.key == QLatin1String(t.key))
                b->setToolTip(QString::fromLatin1(t.tip));
        v->addWidget(b);
        const QString key = d.key;
        connect(b, &QCheckBox::toggled, this, [this, key](bool on) {
            // THROUGH THE GATE, never straight at the viewport. This is the
            // rule the whole ViewportOverlays struct exists to enforce.
            if (m_syncing || !m_bar) return;
            m_bar->setOverlay(key, on);
        });
        m_overlayBoxes.append({key, b});
    }
    v->addStretch(1);
    m_pages->addWidget(page);
}

void ViewportPanel::buildCameraPage()
{
    auto* page = new QWidget(m_pages);
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    // ── Projection and framing ──────────────────────────────────────────
    // The projection is also the gizmo's ring, and the same state: this is a
    // second FACE on it, not a second copy (§3.1). The gizmo is where you
    // reach for it while navigating; this is where you reach for it while you
    // are already in the panel setting up a shot.
    m_ortho = new QCheckBox(QStringLiteral("Orthographic"), page);
    m_ortho->setToolTip(QStringLiteral(
        "Parallel projection: parallel edges stay parallel, so \"is this "
        "symmetrical\" and \"do these two line up\" become answerable by eye. "
        "The same switch as the ring around the corner gizmo.\n\nThe view's "
        "size on screen does not change — the orthographic box is derived "
        "from the field of view below and the camera's distance."));
    v->addWidget(m_ortho);
    connect(m_ortho, &QCheckBox::toggled, this, [this](bool on) {
        if (m_syncing || !m_view) return;
        m_view->setOrthographic(on);
    });

    m_fovLabel = new QLabel(page);
    v->addWidget(m_fovLabel);
    m_fov = mkSlider(page, 15, 100, 45);
    m_fov->setToolTip(QStringLiteral(
        "Vertical field of view, in degrees. 45 is this viewport's own "
        "default and a neutral look; below about 30 flattens perspective the "
        "way a long lens does, and above about 70 exaggerates it.\n\nIt also "
        "sets the SIZE of the orthographic box, so the two projections frame "
        "the model identically."));
    v->addWidget(m_fov);
    connect(m_fov, &QSlider::valueChanged, this, [this](int deg) {
        if (m_fovLabel)
            m_fovLabel->setText(QStringLiteral("Field of view — %1°").arg(deg));
        if (m_syncing || !m_view) return;
        m_view->setFieldOfView(float(deg));
    });

    {
        auto* row = new QHBoxLayout();
        row->setContentsMargins(0, 0, 0, 0);
        auto* fit = new QPushButton(QStringLiteral("Fit model"), page);
        fit->setToolTip(QStringLiteral(
            "Point the camera at the whole scene and back off until it fits — "
            "the same thing middle-clicking the viewport does."));
        connect(fit, &QPushButton::clicked, this, [this] {
            if (m_view) m_view->resetCamera();
        });
        row->addWidget(fit);
        m_frameSel = new QPushButton(QStringLiteral("Frame selection"), page);
        m_frameSel->setToolTip(QStringLiteral(
            "Fit the SELECTED submesh instead of the whole model — the \".\" "
            "key. Greyed out when nothing is selected."));
        connect(m_frameSel, &QPushButton::clicked, this, [this] {
            if (m_view) m_view->frameMesh(m_view->pickedMesh());
        });
        row->addWidget(m_frameSel);
        v->addLayout(row);
    }

    m_autoFit = new QCheckBox(QStringLiteral("Fit each model as it loads"),
                              page);
    m_autoFit->setToolTip(QStringLiteral(
        "Frame every model the moment it is opened, rather than keeping the "
        "camera where you left it.\n\nOn is the friendlier default for "
        "browsing — models differ in size by two orders of magnitude here, and "
        "a camera framed for a pistol shows nothing at all of a helicopter. "
        "Off is what you want while comparing variants of one thing, where the "
        "camera moving between them is the distraction."));
    m_autoFit->setChecked(Config::viewAutoFit());
    v->addWidget(m_autoFit);
    connect(m_autoFit, &QCheckBox::toggled, this, [this](bool on) {
        Config::setViewAutoFit(on);
        // Straight at the viewport as well: the setting is the record, and
        // this is the thing that reads it. A checkbox that only takes effect
        // next launch is a checkbox people press twice.
        if (m_view) m_view->setAutoFit(on);
    });

    m_turn = new QCheckBox(QStringLiteral("Turntable"), page);
    v->addWidget(m_turn);
    m_turnSpeed = mkSlider(page, -120, 120, 22);
    auto* sl = new QLabel(QStringLiteral("Speed (°/s, negative reverses)"), page);
    v->addWidget(sl);
    v->addWidget(m_turnSpeed);
    const auto pushTurn = [this] {
        if (m_syncing || !m_view) return;
        m_view->setTurntable(m_turn->isChecked(),
                             float(m_turnSpeed->value()));
    };
    connect(m_turn, &QCheckBox::toggled, this, pushTurn);
    connect(m_turnSpeed, &QSlider::valueChanged, this, pushTurn);

    auto* reset = new QPushButton(QStringLiteral("Reset view"), page);
    v->addWidget(reset);
    connect(reset, &QPushButton::clicked, this, [this] {
        if (m_view) m_view->resetCamera();
    });

    // ── Capture ─────────────────────────────────────────────────────────
    // On the CAMERA page, because both are pictures of where the camera is:
    // a turntable spins from the elevation and distance you framed, and a
    // still is exactly what is on screen. Both grab the GL surface, so
    // neither catches this panel or the toolbar.
    auto* still = new QPushButton(QStringLiteral("Save image…"), page);
    still->setToolTip(QStringLiteral(
        "The viewport as a PNG, at its current size. No toolbar, no panel — "
        "the GL surface itself."));
    v->addWidget(still);
    connect(still, &QPushButton::clicked, this, [this] {
        if (m_view) captureStillInteractive(this, m_view, suggestedName());
    });
    m_captureButtons << still;
    auto* turn = new QPushButton(QStringLiteral("Turntable…"), page);
    turn->setToolTip(QStringLiteral(
        "One full revolution as an animated GIF, optionally with the frames "
        "as PNGs beside it. Frame the model first — the turn keeps your "
        "elevation and distance."));
    v->addWidget(turn);
    connect(turn, &QPushButton::clicked, this, [this] {
        if (m_view) captureTurntableInteractive(this, m_view, suggestedName());
    });
    m_captureButtons << turn;
    m_animGif = new QPushButton(QStringLiteral("Save animation GIF…"), page);
    m_animGif->setToolTip(QStringLiteral(
        "The loaded CLIP as an animated GIF, sampled evenly from its first "
        "frame to its last. The camera stays where it is; what moves is the "
        "model.\n\nGreyed out until a clip is loaded — this viewport has to "
        "have something to step through."));
    v->addWidget(m_animGif);
    connect(m_animGif, &QPushButton::clicked, this, [this] {
        if (m_view) captureAnimationInteractive(this, m_view, suggestedName());
    });
    // DELIBERATELY NOT in m_captureButtons. That list is enabled wholesale
    // from hasGeometry(), and this button needs a second condition — a clip
    // to step — so being in the list meant the blanket rule ran last and
    // switched it back on with no clip loaded. One button, one rule, below.
    m_camRead = noteLabel(page);
    v->addWidget(m_camRead);
    v->addStretch(1);
    m_pages->addWidget(page);
}

// ── State ───────────────────────────────────────────────────────────────────

void ViewportPanel::applyEnvironment(int index)
{
    if (!m_env) return;
    const QString id = m_env->itemData(index).toString();
    if (id == ViewEnvironment::autoId()) {
        if (m_envNote) {
            const ViewEnvironment* g = m_view
                ? ViewEnvironment::forGame(m_view->sceneGame()) : nullptr;
            m_envNote->setText(
                g ? QStringLiteral("Following the scene: %1. %2")
                        .arg(g->name, g->note)
                  : QStringLiteral(
                        "Follows whichever game the scene's files came out "
                        "of. Nothing loaded, or a scene the index cannot "
                        "place — using Default."));
        }
        if (!m_syncing && m_view) {
            m_view->setEnvironmentAuto(true);
            syncFromView();
        }
        return;
    }
    const ViewEnvironment* e = ViewEnvironment::find(id);
    // An unknown id falls back to the first preset rather than to a
    // default-constructed environment: presets() is the whole vocabulary, and
    // a rig no preset describes is a rig nobody can get back to.
    if (!e) e = &ViewEnvironment::presets().first();
    if (m_envNote) m_envNote->setText(e->note);
    if (!m_syncing && m_view) {
        m_view->setEnvironment(*e);
        // The environment carries its own exposure, so the slider has to
        // follow it or the next drag would snap the scene back.
        syncFromView();
    }
}

void ViewportPanel::syncFromView()
{
    if (!m_view) return;
    m_syncing = true;
    if (m_shadingNote) {
        const ShadingMode sm = m_view->shadingMode();
        m_shadingNote->setText(
            QStringLiteral("Shading: %1 \u2014 %2")
                .arg(QString::fromLatin1(shadingModeName(sm)),
                     QString::fromLatin1(shadingModeNote(sm))));
    }
    syncOverlayBoxes();
    if (m_ortho) m_ortho->setChecked(m_view->orthographic());
    if (m_fov) {
        m_fov->setValue(qRound(m_view->fieldOfView()));
        if (m_fovLabel)
            m_fovLabel->setText(
                QStringLiteral("Field of view — %1°").arg(m_fov->value()));
        // The field of view still SETS the orthographic box, so it is never
        // meaningless — greying it out in ortho would be a lie.
    }
    if (m_frameSel) m_frameSel->setEnabled(m_view->pickedMesh() >= 0);
    if (m_normalMaps) {
        m_normalMaps->setChecked(m_view->normalMapping());
        // A switch with nothing to switch is worse than an absent one: the
        // Fox scenes that carry no normal map at all would otherwise offer a
        // tick that visibly does nothing.
        m_normalMaps->setEnabled(m_view->hasNormalMaps());
    }
    if (m_pbr) m_pbr->setChecked(m_view->pbrShading());
    if (m_debug) {
        const int want = m_debug->findData(int(m_view->debugView()));
        if (want >= 0) m_debug->setCurrentIndex(want);
        // FIVE OF THESE ARE ONLY TRUTHFUL WHEN THE MAPS WERE LOADED. Roughness,
        // Reflection mask, Ambient occlusion, Translucency and Metalness all
        // come from the material set the PBR path loads; with it absent the
        // shader falls back to its own constants (ao 1, roughness 0.55,
        // reflection 0, metalness 0) and the view renders a flat grey that
        // looks exactly like a real flat map. So: disabled, and said out loud.
        // The scene decides, so this is re-run on sceneChanged.
        const bool maps = m_view->hasPbrMaps();
        for (int i = 0; i < m_debug->count(); ++i) {
            const DebugView d = DebugView(m_debug->itemData(i).toInt());
            const bool needsMaps = d == DebugView::Roughness
                || d == DebugView::ReflectionMask || d == DebugView::Occlusion
                || d == DebugView::Translucency || d == DebugView::Metalness;
            if (auto* m = qobject_cast<QStandardItemModel*>(m_debug->model()))
                if (QStandardItem* it = m->item(i))
                    it->setEnabled(maps || !needsMaps);
        }
        if (m_debugNote) {
            QString note =
                QString::fromLatin1(debugViewNote(m_view->debugView()));
            if (!maps)
                note += QStringLiteral(
                    "\n\nThis scene was loaded without its PBR maps, so the "
                    "five map-derived channels would only show the shader's "
                    "own defaults and are switched off. Turn PBR on for this "
                    "tab to load them.");
            m_debugNote->setText(note);
        }
    }
    if (m_az) m_az->setValue(qRound(m_view->keyAzimuth()));
    if (m_el) m_el->setValue(qRound(m_view->keyElevation()));
    if (m_keyGain) m_keyGain->setValue(qRound(m_view->keyIntensity() * 100.0f));
    if (m_ambGain)
        m_ambGain->setValue(qRound(m_view->ambientIntensity() * 100.0f));
    if (m_exposure) m_exposure->setValue(qRound(m_view->exposure() * 100.0f));
    if (m_follow) m_follow->setChecked(m_view->keyFollowsCamera());
    if (m_lightRead)
        m_lightRead->setText(
            QStringLiteral("from %1° / %2° above · key ×%3 · ambient ×%4 · "
                           "exposure ×%5")
                .arg(qRound(m_view->keyAzimuth()))
                .arg(qRound(m_view->keyElevation()))
                .arg(m_view->keyIntensity(), 0, 'f', 2)
                .arg(m_view->ambientIntensity(), 0, 'f', 2)
                .arg(m_view->exposure(), 0, 'f', 2));
    if (m_env) {
        // While Auto is on the combo must READ "Auto", not the rig Auto
        // happens to have picked — otherwise the next scene silently moves
        // the selection and the mode is invisible.
        const bool autoOn = m_view->environmentAuto();
        const int want = autoOn ? m_env->findData(ViewEnvironment::autoId())
                                : m_env->findData(m_view->environment().id);
        if (want >= 0) m_env->setCurrentIndex(want);
        if (m_envNote) {
            if (autoOn) {
                const ViewEnvironment* g =
                    ViewEnvironment::forGame(m_view->sceneGame());
                m_envNote->setText(
                    g ? QStringLiteral("Following the scene: %1. %2")
                            .arg(g->name, g->note)
                      : QStringLiteral(
                            "Follows whichever game the scene's files came "
                            "out of. Nothing loaded, or a scene the index "
                            "cannot place — using Default."));
            } else {
                m_envNote->setText(m_view->environment().note);
            }
        }
    }
    // Nothing in the viewport, nothing to capture. The Export menu's copies of
    // these are already gated on the tab's own "is there a model" test; these
    // were not, and offered to photograph an empty background under the
    // previous model's name.
    for (QPushButton* b : m_captureButtons)
        if (b) b->setEnabled(m_view->hasGeometry());
    // The animation GIF needs BOTH: something to photograph and something to
    // step. Stated once, here, next to the rule it has to agree with.
    if (m_animGif)
        m_animGif->setEnabled(m_view->hasGeometry()
                              && bool(m_view->animationFrameProvider()));
    if (m_turn) m_turn->setChecked(m_view->turntable());
    if (m_turnSpeed) m_turnSpeed->setValue(qRound(m_view->turntableSpeed()));
    if (m_camRead)
        m_camRead->setText(
            QStringLiteral("yaw %1°   pitch %2°   distance %3\nBackground %4")
                .arg(qRound(m_view->cameraYaw()))
                .arg(qRound(m_view->cameraPitch()))
                .arg(m_view->cameraDistance(), 0, 'f', 2)
                .arg(m_view->backgroundColor().name(QColor::HexRgb)));
    m_syncing = false;
}

QString ViewportPanel::suggestedName() const
{
    // The panel does not know what is in the scene — the tabs do — so the
    // stem is whatever the owning tab published on the viewport as a dynamic
    // property, and a plain fallback when nothing did. A dynamic property
    // rather than a signal because there is nothing to react to: the name is
    // only ever read at the moment a file dialog opens.
    if (m_view) {
        const QString n =
            m_view->property("foxabCaptureName").toString().trimmed();
        // Templated like every other export name — the dialog's "File name"
        // field said it named the exported file and then named exactly one of
        // them. The companion property carries the index entry so {{Game}}
        // and {{Hash}} resolve; a scene with no indexed part leaves them
        // empty rather than refusing the name.
        if (!n.isEmpty()) {
            // An UNSET property converts to 0, and 0 is a perfectly valid file
            // index — a capture from a viewport whose owner published a name
            // but no index would have been stamped with file 0's game and
            // hash. Absent means -1 here, never 0.
            const QVariant idx = m_view->property("foxabCaptureFileIdx");
            return fox::templatedStem(n, idx.isValid() ? idx.toInt() : -1);
        }
    }
    return QStringLiteral("viewport");
}

void ViewportPanel::setBar(ViewportBar* bar)
{
    m_bar = bar;
    // FOLLOW THE GATE. The page's boxes are a view of ViewportOverlays and
    // nothing else writes them; without this, switching an overlay from the
    // viewport's right-click menu or from a settings replay left this page
    // showing the state from whenever it was last opened.
    if (bar)
        connect(bar, &ViewportBar::overlaysChanged, this,
                [this](const ViewportOverlays&) { syncOverlayBoxes(); });
    syncOverlayBoxes();
}

void ViewportPanel::syncOverlayBoxes()
{
    if (!m_bar) return;
    const ViewportOverlays& o = m_bar->overlays();
    if (m_overlayMaster) {
        QSignalBlocker block(m_overlayMaster);
        m_overlayMaster->setChecked(o.master);
    }
    for (const auto& pair : m_overlayBoxes) {
        if (!pair.second) continue;
        // THROUGH THE BAR'S OWN ACCESSOR, not a hand-written if-chain over the
        // struct's fields. The chain had to be extended for every new overlay
        // and was not: "selection" and "gizmo" both fell off the end of it and
        // read as false, so the page showed two switches unticked while the
        // overlays they gate were plainly on screen.
        const bool on = m_bar->overlayState(pair.first);
        QSignalBlocker block(pair.second);
        pair.second->setChecked(on);
        // Greyed out, not hidden, while the master switch is off: the state is
        // remembered and the user can see what will come back.
        pair.second->setEnabled(o.master);
    }
}

void ViewportPanel::setPage(int page)
{
    if (!m_pages) return;
    const int p = qBound(0, page, m_pages->count() - 1);
    m_pages->setCurrentIndex(p);
    static const char* const kTitles[] = {"Graphics", "Lighting", "Camera",
                                          "Overlays"};
    const int n = int(sizeof(kTitles) / sizeof(kTitles[0]));
    if (m_title && p < n) m_title->setText(QString::fromLatin1(kTitles[p]));
}

bool ViewportPanel::showPopover(const QString& key, QWidget* anchor)
{
    const int page = pageIndexFor(key);
    if (page < 0) return false;
    // Clicking the button that opened it closes it again. A popover with no
    // way back other than its own ✕ is a dialog with the title bar filed off.
    if (isVisible() && m_pages && m_pages->currentIndex() == page) {
        setPanelOpen(false);
        return true;
    }
    setPage(page);
    m_anchor = anchor;
    setPanelOpen(true);
    return true;
}

void ViewportPanel::showPage(int page)
{
    setPage(page);
    setPanelOpen(true);
}

int ViewportPanel::pageIndexFor(const QString& name)
{
    // The three topics, plus the four OLD names they replaced. The old ones
    // are in recorded harness commands and in this project's own documents,
    // and silently returning -1 for "--viewpanel world" would have looked like
    // the flag had stopped working.
    struct Alias { const char* name; int page; };
    static const Alias kNames[] = {
        {"graphics", 0}, {"lighting", 1}, {"camera", 2}, {"overlays", 3},
        {"display", 0},  {"light", 1},    {"world", 1},
    };
    for (const Alias& a : kNames)
        if (name.compare(QLatin1String(a.name), Qt::CaseInsensitive) == 0)
            return a.page;
    return -1;
}

void ViewportPanel::setPanelOpen(bool on)
{
    const bool was = isVisible();
    if (on) {
        syncFromView();
        reposition();
        raise();
        show();
    } else {
        hide();
    }
    if (was != on) emit openChanged(on);
}

void followDisplayState(GLModelWidget* view, QToolButton* wireframe,
                        QToolButton* skeleton)
{
    if (!view) return;
    // The skeleton is an OVERLAY now, so its button follows the gate's desired
    // state, not the viewport's. With the master switch off the two differ on
    // purpose — the button shows what you asked for and greys out to say it is
    // not being drawn — and the viewport emits nothing when a gated request
    // changes no state, so this is the only signal that can keep it honest.
    if (skeleton) {
        if (ViewportBar* bar = viewportBarFor(view)) {
            QObject::connect(bar, &ViewportBar::overlaysChanged, skeleton,
                             [skeleton](const ViewportOverlays& o) {
                                 QSignalBlocker b(skeleton);
                                 skeleton->setChecked(o.skeleton);
                                 skeleton->setEnabled(o.master);
                             });
            const ViewportOverlays& o = bar->overlays();
            QSignalBlocker b(skeleton);
            skeleton->setChecked(o.skeleton);
            skeleton->setEnabled(o.master);
        }
    }
    // Blocked while writing back, or setting the button would re-enter the
    // button's own toggled handler and call the view setter again.
    QObject::connect(view, &GLModelWidget::displayChanged, view,
                     [view, wireframe, skeleton] {
                         if (wireframe) {
                             QSignalBlocker b(wireframe);
                             wireframe->setChecked(view->wireframe());
                         }
                         // NOT the skeleton: it is gated, so the viewport's
                         // answer is "what is drawn", and the button has to
                         // say "what was asked for". See above.
                     });
}

// ── "Remember viewport settings" (Settings ▸ Interface) ─────────────────────
// One switch over the viewport's whole state: the light rig and whether it
// follows the camera, the environment, the exposure, the shading mode, the
// debug channel, the projection, the field of view, and which overlays are on.
//
// OFF by default, and that is a choice rather than caution: a tool that comes
// up in whatever state a one-off experiment left it in is a tool nobody can
// describe over a screenshot, and "why does mine look different" is the
// support question this avoids. On, it is exactly what someone who has tuned a
// rig for their own screenshots wants.
//
// ONE key per value, all under view/remember/, and nothing is written while
// the switch is off — a dead key that a future build starts reading is how a
// setting comes back from the dead with a value nobody chose (§3.1).
namespace {

constexpr const char* kRemPrefix = "view/remember/";

QString remKey(const char* leaf)
{
    return QString::fromLatin1(kRemPrefix) + QString::fromLatin1(leaf);
}

void saveViewportState(GLModelWidget* view, ViewportBar* bar)
{
    if (!view || !Config::rememberViewport()) return;
    QSettings s;
    s.setValue(remKey("keyAz"), view->keyAzimuth());
    s.setValue(remKey("keyEl"), view->keyElevation());
    s.setValue(remKey("keyGain"), view->keyIntensity());
    s.setValue(remKey("ambGain"), view->ambientIntensity());
    s.setValue(remKey("exposure"), view->exposure());
    s.setValue(remKey("follow"), view->keyFollowsCamera());
    s.setValue(remKey("ortho"), view->orthographic());
    s.setValue(remKey("fov"), view->fieldOfView());
    // STABLE IDS, never indices (§3.2): the shading mode and the channel are
    // stored by their own names, so inserting one in the middle of either
    // enum cannot silently re-point a saved value at its neighbour.
    s.setValue(remKey("shading"),
               QString::fromLatin1(shadingModeName(view->shadingMode())));
    s.setValue(remKey("channel"),
               QString::fromLatin1(debugViewName(view->debugView())));
    s.setValue(remKey("environment"), view->environment().id);
    if (bar) {
        QStringList on;
        for (const ViewportBar::OverlayDef& d : ViewportBar::overlayDefs())
            if (bar->overlayState(d.key)) on << d.key;
        s.setValue(remKey("overlays"), on.join(QLatin1Char(',')));
        s.setValue(remKey("overlayMaster"), bar->overlays().master);
    }
}

void restoreViewportState(GLModelWidget* view, ViewportBar* bar)
{
    if (!view || !Config::rememberViewport()) return;
    QSettings s;
    if (!s.contains(remKey("keyAz"))) return;   // nothing saved yet
    view->setKeyAngles(s.value(remKey("keyAz")).toFloat(),
                       s.value(remKey("keyEl")).toFloat());
    view->setKeyIntensity(s.value(remKey("keyGain"), 1.0).toFloat());
    view->setAmbientIntensity(s.value(remKey("ambGain"), 1.0).toFloat());
    view->setExposure(s.value(remKey("exposure"), 1.0).toFloat());
    view->setKeyFollowsCamera(s.value(remKey("follow"), false).toBool());
    view->setOrthographic(s.value(remKey("ortho"), false).toBool());
    view->setFieldOfView(s.value(remKey("fov"), 45.0).toFloat());
    const QString shading = s.value(remKey("shading")).toString();
    for (const ShadingMode m : {ShadingMode::Wireframe, ShadingMode::Flat,
                                ShadingMode::Shaded, ShadingMode::Rendered})
        if (shading.compare(QLatin1String(shadingModeName(m)),
                            Qt::CaseInsensitive) == 0)
            view->setShadingMode(m);
    const QString channel = s.value(remKey("channel")).toString();
    for (const DebugView d : debugViews())
        if (channel.compare(QLatin1String(debugViewName(d)),
                            Qt::CaseInsensitive) == 0)
            view->setDebugView(d);
    if (bar) {
        // THROUGH THE GATE, every one of them (§3.3) — a replay that called
        // setShow* directly is precisely what the gate exists to stop.
        const QStringList on =
            s.value(remKey("overlays")).toString().split(QLatin1Char(','),
                                                         Qt::SkipEmptyParts);
        for (const ViewportBar::OverlayDef& d : ViewportBar::overlayDefs())
            bar->setOverlay(d.key, on.contains(d.key));
        bar->setOverlaysMaster(s.value(remKey("overlayMaster"), true).toBool());
    }
}

}  // namespace

ViewportPanel* attachViewportPanel(GLModelWidget* view,
                                   std::function<void(partmenu::Context&, int)> pageContext)
{
    if (!view) return nullptr;
    // THE SAVED DEFAULTS ARE APPLIED HERE, not in GLModelWidget's constructor:
    // the widget is the renderer and knows nothing about QSettings, and every
    // viewport in the app comes through this function. Applied BEFORE the
    // panel is built so the panel's first sync reads the real state.
    if (Config::viewEnvironment() == ViewEnvironment::autoId()) {
        // AUTO is not a preset and find() deliberately does not resolve it —
        // it is a MODE, and it has to be restored as one or the setting comes
        // back as whatever rig the last scene happened to pick.
        view->setEnvironmentAuto(true);
    } else {
        const ViewEnvironment* saved =
            ViewEnvironment::find(Config::viewEnvironment());
        // An id no build knows — a stale setting, a hand-edited file — falls
        // back to the first preset rather than to whatever the constructor
        // happened to leave, so the viewport is always on a rig the panel can
        // name.
        view->setEnvironment(saved ? *saved
                                   : ViewEnvironment::presets().first());
    }
    // 0 means "whatever the environment says", which setEnvironment has just
    // done — so only a positive value overrides it.
    if (Config::viewExposure() > 0.0)
        view->setExposure(float(Config::viewExposure()));
    auto* panel = new ViewportPanel(view);
    // The BAR, and the panel it opens. Built in this order because the bar
    // needs the panel to open popovers and the panel needs the bar to reach
    // the overlay gate; the second half of that is a setter rather than a
    // constructor argument for exactly that reason.
    auto* bar = new ViewportBar(view, panel);
    panel->setBar(bar);
    bar->show();
    // The text overlays. A child of the viewport, above the bar in creation
    // order and below it in stacking, because the bar is clickable and the HUD
    // is not.
    // The gizmo, before the HUD so it stacks above it and below the bar: the
    // bar and the gizmo are both clickable and must not overlap, which is what
    // setTopMargin below is for; the HUD is text and is never clicked.
    auto* gizmo = new ViewportGizmo(view);
    // Under the bar. The bar is a floating strip at the top-right whose height
    // is font-dependent, so it is asked rather than guessed.
    // The bar is top-LEFT now, so nothing is above the gizmo — it takes the
    // top-right corner outright instead of hanging under a strip that has
    // moved away.
    gizmo->setTopMargin(6);
    auto* hud = new ViewportHud(view);
    hud->show();
    hud->lower();
    bar->raise();
    // Right-click the viewport and get the same things. Installed here rather
    // than in each tab for the reason the panel itself is: every viewport in
    // the app comes through this function, so there is exactly one menu.
    installViewportContextMenu(view, std::move(pageContext));
    // THE TOOLBAR BUTTON IS GONE. It opened the same three popovers the
    // viewport's own bar opens, from a strip of controls a few pixels above
    // them, and its own tooltip had to say so — "the same three the buttons on
    // the viewport's own bar open". Template §5 puts these on the viewport;
    // a second copy beside it is not discoverability, it is a duplicate
    // control the settings replay had to be taught about.
    // The panel's remembered open state, restored last so it lands over a
    // viewport whose bar and gizmo are already built.
    // The remembered state, and the writer that keeps it current. Both no-ops
    // while the switch is off. The save is DEBOUNCED: cameraChanged fires
    // sixty times a second under a turntable, and writing a settings file at
    // that rate is a way to make a spinning model stutter.
    view->setAutoFit(Config::viewAutoFit());
    restoreViewportState(view, bar);
    {
        auto* save = new QTimer(view);
        save->setSingleShot(true);
        save->setInterval(400);
        QObject::connect(save, &QTimer::timeout, view, [view, bar] {
            saveViewportState(view, bar);
        });
        QTimer* saveTimer = save;
        const auto queue = [saveTimer] {
            if (Config::rememberViewport()) saveTimer->start();
        };
        QObject::connect(view, &GLModelWidget::displayChanged, save, queue);
        QObject::connect(view, &GLModelWidget::cameraChanged, save, queue);
        QObject::connect(bar, &ViewportBar::overlaysChanged, save,
                         [queue](const ViewportOverlays&) { queue(); });
    }

    // THE POPOVER DOES NOT COME BACK OPEN. It used to restore whatever it was
    // last left as, which meant one stray click left a card sitting over every
    // viewport on every launch — and because a restore has no button to anchor
    // to, it opened hard against the RIGHT edge, nowhere near the bar that
    // owns it and half under the N-panel's arrow.
    //
    // §5 calls these popovers, and a popover that survives a restart is a
    // panel. Nothing here restores it; what IS remembered across launches is
    // the viewport's actual state — the environment, the exposure, the
    // overlays — under Settings ▸ Interface ▸ "Remember viewport settings".
    return panel;
}

ViewportBar* viewportBarFor(GLModelWidget* view)
{
    // findChild rather than a map keyed on the viewport: the bar is a CHILD of
    // the viewport, so the parent-child graph already holds this relationship
    // and a second copy of it could go stale when a viewport is destroyed.
    return view ? view->findChild<ViewportBar*>(QString(),
                                                Qt::FindDirectChildrenOnly)
                : nullptr;
}

void ViewportPanel::reposition()
{
    if (!m_view) return;
    // sizeHint() rather than height(): before the first show() the widget has
    // no height yet, and the card would otherwise arrive one pixel tall and
    // grow after the user could see it.
    // The CURRENT page's height at the card's width, not sizeHint(): a
    // QStackedWidget hints at the tallest page it holds, so the Camera card
    // would be as tall as the Lighting one and half of it empty.
    // heightForWidth is what a wrapped label actually needs once it knows how
    // wide it is, and the notes on these pages are wrapped labels.
    //
    // The height is pushed into the SCROLL AREA and the card is then asked to
    // size itself, rather than the card's height being computed here from a
    // sum of margins. Two attempts at that sum came out one checkbox short and
    // put a scroll bar on a card that did not need one; the layout knows its
    // own chrome and this asks it instead of guessing.
    if (m_pages && m_scroll) {
        if (QWidget* page = m_pages->currentWidget()) {
            const int inner = kPanelW - 20;
            const int hfw = page->hasHeightForWidth()
                ? page->heightForWidth(inner) : 0;
            const int want = qMax(qMax(hfw, page->sizeHint().height()),
                                  page->minimumSizeHint().height());
            const int room = qMax(80, m_view->height() - 2 * kMargin - 40);
            m_scroll->setFixedHeight(qBound(80, want + 2, room));
        }
    }
    adjustSize();
    const int h = qMin(m_view->height() - 2 * kMargin,
                       qMax(sizeHint().height(), height()));
    // NEXT TO THE BUTTON THAT OPENED IT — that is what makes it a popover
    // rather than a panel (template §5). Under the anchor, right-aligned with
    // it, then clamped into the viewport: a card that hangs off the edge is
    // the off-screen-pane bug in a smaller box.
    // The fallback when nothing anchored this — a keyboard N, or a harness
    // flag. UNDER THE BAR, which is top-left: the far right is where this used
    // to land, which is as far from the control that owns it as the viewport
    // allows and is where the N-panel's arrow lives.
    int x = kMargin;
    int y = kMargin;
    if (ViewportBar* bar = m_view->findChild<ViewportBar*>(
            QString(), Qt::FindDirectChildrenOnly))
        if (bar->isVisible()) y = bar->y() + bar->height() + 4;
    if (m_anchor && m_anchor->isVisible()) {
        const QPoint br =
            m_anchor->mapTo(m_view, QPoint(m_anchor->width(), m_anchor->height()));
        x = br.x() - kPanelW;
        y = br.y() + 4;
    }
    x = qBound(kMargin, x, qMax(kMargin, m_view->width() - kPanelW - kMargin));
    y = qBound(kMargin, y, qMax(kMargin, m_view->height() - qMax(60, h) - kMargin));
    setGeometry(x, y, kPanelW, qMax(60, h));
}

bool ViewportPanel::eventFilter(QObject* o, QEvent* e)
{
    if (o == m_view) {
        if (e->type() == QEvent::Resize) reposition();
        if (e->type() == QEvent::KeyPress) {
            auto* k = static_cast<QKeyEvent*>(e);
            // FROM THE REGISTRY, not a hardcoded N. The binding is listed in
            // Settings ▸ Hotkeys and was editable there while this compared
            // against Key_N regardless — so rebinding it did nothing and N
            // kept working, with no way to tell why.
            const QKeySequence want =
                Hotkeys::seq(QStringLiteral("hotkeys/viewPanel"));
            if (!want.isEmpty()
                && QKeySequence(k->keyCombination()) == want) {
                togglePanel();
                return true;
            }
        }
    }
    return QWidget::eventFilter(o, e);
}

}  // namespace fox
