#include "export/ExportOptions.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QImageWriter>
#include <QLineEdit>
#include <QSpinBox>
#include <QLabel>
#include <QRegularExpression>
#include <QSettings>
#include <QVBoxLayout>
#include <QtGlobal>

#include "index/ArchiveIndex.h"
#include "index/GameId.h"
#include "model/GlbExporter.h"
#include "util/ExportLayout.h"

namespace fox {

QString ExportOptions::describe() const
{
    return QStringLiteral("scale %1 · %2 up · %3 · %4 colour layer · %5 · %6")
        .arg(scale, 0, 'g', 4)
        .arg(zUp ? QStringLiteral("Z") : QStringLiteral("Y"),
             skeleton ? QStringLiteral("with skeleton")
                      : QStringLiteral("no skeleton"),
             bakeColourLayer ? QStringLiteral("baked") : QStringLiteral("raw"),
             normalMaps ? QStringLiteral("normal maps")
                        : QStringLiteral("no normal maps"),
             connectPoints ? QStringLiteral("connect points")
                           : QStringLiteral("no connect points"))
        + QStringLiteral(" \u00b7 %1 \u00b7 %2")
              .arg(assembleFtex ? QStringLiteral("ftex\u2192dds")
                                : QStringLiteral("raw ftex"),
                   convertWem ? QStringLiteral("wem\u2192wav")
                              : QStringLiteral("raw wem"));
}

namespace {
bool g_haveSession = false;
ExportOptions g_session;
}  // namespace

void setSessionExportOptions(const ExportOptions& o)
{
    g_session = o;
    g_session.scale = qBound(0.0001, o.scale, 10000.0);
    g_haveSession = true;
}

namespace {
CaptureOptions g_capSession;
bool g_haveCapSession = false;
}  // namespace

void setSessionCaptureOptions(const CaptureOptions& o)
{
    // CLAMPED here as well as on the way out of QSettings: a command line is
    // the one input nothing else validates, and "--gifscale 0" would otherwise
    // reach the scaler as a zero-pixel target.
    g_capSession = o;
    g_capSession.frames = qBound(2, o.frames, 360);
    g_capSession.delayCs = qBound(1, o.delayCs, 100);
    g_capSession.colors = qBound(2, o.colors, 256);
    g_capSession.scalePct = qBound(25, o.scalePct, 100);
    g_capSession.targetMB = qBound(1, o.targetMB, 200);
    g_capSession.imageScale = qBound(25, o.imageScale, 400);
    g_capSession.imageQuality = qBound(1, o.imageQuality, 100);
    const QString f = o.imageFormat.toLower();
    g_capSession.imageFormat =
        (f == QLatin1String("jpg") || f == QLatin1String("webp"))
            ? f : QStringLiteral("png");
    g_haveCapSession = true;
}

glb::SceneOptions sceneOptionsFrom(const ExportOptions& o)
{
    glb::SceneOptions s;
    s.scale = qBound(0.0001, o.scale, 10000.0);
    s.zUp = o.zUp;
    s.skeleton = o.skeleton;
    s.bakeColourLayer = o.bakeColourLayer;
    s.normalMaps = o.normalMaps;
    s.connectPoints = o.connectPoints;
    return s;
}

QString applyNameTemplate(const QString& tpl, const QString& stem,
                          const QString& game, quint64 hash)
{
    QString out = tpl.isEmpty() ? QStringLiteral("{{Name}}") : tpl;
    out.replace(QLatin1String("{{Name}}"), stem);
    out.replace(QLatin1String("{{Game}}"), game);
    out.replace(QLatin1String("{{Hash}}"),
                QStringLiteral("%1").arg(hash, 16, 16, QLatin1Char('0')));
    static const QRegularExpression bad(QStringLiteral("[\\\\/:*?\"<>|]"));
    out.replace(bad, QStringLiteral("_"));
    // SIMPLIFIED FIRST. Both checks below look at the ends of the string, and
    // a trailing space hid a trailing dot from one and a reserved name from
    // the other — " CON " passed, and Windows then wrote CON.
    out = out.simplified();
    // A trailing dot or a reserved device name makes the write fail on
    // Windows with nothing in the log to say why.
    while (out.endsWith(QLatin1Char('.'))) out.chop(1);
    out = out.trimmed();
    static const QStringList kReserved = {
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
        QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
        QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
        QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
        QStringLiteral("LPT9")};
    if (kReserved.contains(out, Qt::CaseInsensitive)) out.prepend(QLatin1Char('_'));
    return out.isEmpty() ? stem : out;
}

QString templatedStem(const QString& stem, int fileIdx)
{
    const ArchiveIndex& ix = ArchiveIndex::instance();
    const bool known = fileIdx >= 0 && fileIdx < ix.files().size();
    return applyNameTemplate(
        loadExportOptions().nameTemplate, stem,
        known ? QString::fromLatin1(gameShortName(ix.gameOf(ix.files()[fileIdx])))
              : QString(),
        known ? ix.files()[fileIdx].hash : 0);
}

ExportOptions loadExportOptions()
{
    if (g_haveSession) return g_session;
    QSettings s;
    ExportOptions o;
    o.scale = qBound(0.0001, s.value(QStringLiteral("export/scale"), 1.0).toDouble(),
                     10000.0);
    o.zUp = s.value(QStringLiteral("export/zUp"), false).toBool();
    o.skeleton = s.value(QStringLiteral("export/skeleton"), true).toBool();
    o.bakeColourLayer =
        s.value(QStringLiteral("export/bakeColourLayer"), true).toBool();
    o.normalMaps = s.value(QStringLiteral("export/normalMaps"), true).toBool();
    o.connectPoints =
        s.value(QStringLiteral("export/connectPoints"), true).toBool();
    o.nameTemplate = s.value(QStringLiteral("export/nameTemplate"),
                             QStringLiteral("{{Name}}")).toString();
    o.assembleFtex =
        s.value(QStringLiteral("export/assembleFtex"), true).toBool();
    o.convertWem = s.value(QStringLiteral("export/convertWem"), true).toBool();
    return o;
}

void saveExportOptions(const ExportOptions& o)
{
    // Same rule as saveCaptureOptions, for the same reason.
    if (g_haveSession) {
        qInfo("export: settings not saved — this run has command-line "
              "overrides in force");
        return;
    }
    QSettings s;
    // Clamped on the way in too, so what is stored is what will be read back.
    s.setValue(QStringLiteral("export/scale"), qBound(0.0001, o.scale, 10000.0));
    s.setValue(QStringLiteral("export/zUp"), o.zUp);
    s.setValue(QStringLiteral("export/skeleton"), o.skeleton);
    s.setValue(QStringLiteral("export/bakeColourLayer"), o.bakeColourLayer);
    s.setValue(QStringLiteral("export/normalMaps"), o.normalMaps);
    s.setValue(QStringLiteral("export/connectPoints"), o.connectPoints);
    s.setValue(QStringLiteral("export/nameTemplate"), o.nameTemplate);
    s.setValue(QStringLiteral("export/assembleFtex"), o.assembleFtex);
    s.setValue(QStringLiteral("export/convertWem"), o.convertWem);
}

CaptureOptions loadCaptureOptions()
{
    if (g_haveCapSession) return g_capSession;
    QSettings s;
    CaptureOptions o;
    o.frames = qBound(2, s.value(QStringLiteral("capture/frames"), 36).toInt(), 360);
    o.delayCs = qBound(1, s.value(QStringLiteral("capture/delayCs"), 4).toInt(), 100);
    o.colors = qBound(2, s.value(QStringLiteral("capture/colors"), 256).toInt(), 256);
    o.dither = s.value(QStringLiteral("capture/dither"), true).toBool();
    o.alsoFrames = s.value(QStringLiteral("capture/alsoFrames"), false).toBool();
    o.scalePct = qBound(25, s.value(QStringLiteral("capture/scalePct"), 100).toInt(), 100);
    o.cropToModel = s.value(QStringLiteral("capture/cropToModel"), false).toBool();
    o.transparent = s.value(QStringLiteral("capture/transparent"), false).toBool();
    o.optimize = s.value(QStringLiteral("capture/optimize"), false).toBool();
    o.targetMB = qBound(1, s.value(QStringLiteral("capture/targetMB"), 10).toInt(), 200);
    {
        // Only a container this build can actually write. A hand-edited
        // "tiff" would otherwise reach QImage::save as the format string and
        // fail per file with nothing in the dialog to explain it.
        const QString f = s.value(QStringLiteral("capture/imageFormat"),
                                  QStringLiteral("png")).toString().toLower();
        o.imageFormat = (f == QLatin1String("jpg") || f == QLatin1String("webp"))
            ? f : QStringLiteral("png");
    }
    o.imageScale = qBound(25, s.value(QStringLiteral("capture/imageScale"), 100).toInt(), 400);
    o.imageQuality = qBound(1, s.value(QStringLiteral("capture/imageQuality"), 92).toInt(), 100);
    return o;
}

void saveCaptureOptions(const CaptureOptions& o)
{
    // A HARNESS RUN MUST NOT WRITE THE USER'S SETTINGS. loadCaptureOptions
    // returns the session copy while an override is in force, so a dialog
    // opened during a "--gifbudget 4 --stay" run would read those values back
    // and press them into QSettings on OK — which is exactly what
    // setSessionCaptureOptions promises cannot happen.
    if (g_haveCapSession) {
        qInfo("capture: settings not saved — this run has command-line "
              "overrides in force");
        return;
    }
    QSettings s;
    s.setValue(QStringLiteral("capture/frames"), qBound(2, o.frames, 360));
    s.setValue(QStringLiteral("capture/delayCs"), qBound(1, o.delayCs, 100));
    s.setValue(QStringLiteral("capture/colors"), qBound(2, o.colors, 256));
    s.setValue(QStringLiteral("capture/dither"), o.dither);
    s.setValue(QStringLiteral("capture/alsoFrames"), o.alsoFrames);
    s.setValue(QStringLiteral("capture/scalePct"), qBound(25, o.scalePct, 100));
    s.setValue(QStringLiteral("capture/cropToModel"), o.cropToModel);
    s.setValue(QStringLiteral("capture/transparent"), o.transparent);
    s.setValue(QStringLiteral("capture/optimize"), o.optimize);
    s.setValue(QStringLiteral("capture/targetMB"), qBound(1, o.targetMB, 200));
    s.setValue(QStringLiteral("capture/imageFormat"), o.imageFormat);
    s.setValue(QStringLiteral("capture/imageScale"), qBound(25, o.imageScale, 400));
    s.setValue(QStringLiteral("capture/imageQuality"), qBound(1, o.imageQuality, 100));
}

// ── Settings ▸ Export (template §10) ────────────────────────────────────────
struct ExportPages::Widgets {
    ExportOptions io;
    CaptureOptions co;
    QDoubleSpinBox* scale = nullptr;
    QComboBox* axis = nullptr;
    QCheckBox* skel = nullptr;
    QCheckBox* layer = nullptr;
    QCheckBox* nrm = nullptr;
    QCheckBox* cnp = nullptr;
    QLineEdit* nameTpl = nullptr;
    QComboBox* layout = nullptr;
    QCheckBox* ftex = nullptr;
    QCheckBox* wem = nullptr;
    QComboBox* imgFmt = nullptr;
    QSpinBox* imgScale = nullptr;
    QSpinBox* imgQ = nullptr;
    QSpinBox* gifScale = nullptr;
    QCheckBox* gifCrop = nullptr;
    QCheckBox* gifTrans = nullptr;
    QCheckBox* gifOpt = nullptr;
    QSpinBox* gifMB = nullptr;
};

ExportPages::ExportPages(QWidget* parent)
    : m_w(std::make_shared<Widgets>())
{
    m_w->io = loadExportOptions();
    m_w->co = loadCaptureOptions();
    ExportOptions* io = &m_w->io;
    CaptureOptions& co = m_w->co;

    // ── Models: what a .glb contains ────────────────────────────────────
    m_models = new QWidget(parent);
    auto* mv = new QVBoxLayout(m_models);
    auto* mf = new QFormLayout();
    auto* scale = new QDoubleSpinBox(m_models);
    scale->setDecimals(4);
    scale->setRange(0.0001, 10000.0);
    scale->setValue(io->scale);
    scale->setToolTip(QStringLiteral(
        "Multiplier on every position. Fox authors in metres, which is what "
        "glTF wants — 100 gives centimetres, 0.01 gives hundreds of metres. "
        "Applied through the scene's root node, so no coordinate is rewritten "
        "and nothing loses precision."));
    mf->addRow(QStringLiteral("Scale"), scale);

    auto* axis = new QComboBox(m_models);
    axis->addItem(QStringLiteral("Y up (glTF standard)"), false);
    axis->addItem(QStringLiteral("Z up"), true);
    axis->setCurrentIndex(io->zUp ? 1 : 0);
    axis->setToolTip(QStringLiteral(
        "Blender's glTF importer converts Y up to Z up for you and is on by "
        "default; this is for the case where it is off, and for engines that "
        "want the axes as authored."));
    mf->addRow(QStringLiteral("Up axis"), axis);
    mv->addLayout(mf);

    auto* skel = new QCheckBox(QStringLiteral("Skeleton and skinning"), m_models);
    skel->setChecked(io->skeleton);
    skel->setToolTip(QStringLiteral(
        "Off writes the mesh with no rig — much smaller, and what a static "
        "prop or a print wants. A posed export is already baked static "
        "whichever way this is set."));
    mv->addWidget(skel);

    auto* layer =
        new QCheckBox(QStringLiteral("Bake the runtime colour layer"), m_models);
    layer->setChecked(io->bakeColourLayer);
    layer->setToolTip(QStringLiteral(
        "A customizable garment ships a WHITE base map and the game multiplies "
        "the chosen colour through a mask at runtime. On, the export looks "
        "like the viewport; off, it exports the white it ships, which is what "
        "you want if the colouring is being redone downstream."));
    mv->addWidget(layer);

    auto* nrm = new QCheckBox(QStringLiteral("Normal maps"), m_models);
    nrm->setChecked(io->normalMaps);
    nrm->setToolTip(QStringLiteral(
        "Fox DXT5nm maps, unswizzled to plain RGB, with a TANGENT attribute so "
        "importers use the engine's own tangent frame."));
    mv->addWidget(nrm);

    auto* cnp = new QCheckBox(QStringLiteral("Connect points as empties"), m_models);
    cnp->setChecked(io->connectPoints);
    cnp->setToolTip(QStringLiteral(
        "The model's .fcnp sockets — CNP_HEAD, CNP_RIGHT_HAND, CNP_ASRROOT — "
        "written as empty nodes parented to the bone each hangs off. They are "
        "what says where a hat or a scope goes, they cost a node each, and on "
        "a rigged export they follow the animation."));
    mv->addWidget(cnp);
    mv->addStretch(1);

    // ── File names ──────────────────────────────────────────────────────
    m_names = new QWidget(parent);
    auto* nv = new QVBoxLayout(m_names);
    auto* nameRow = new QFormLayout();
    auto* nameTpl = new QLineEdit(io->nameTemplate, m_names);
    nameTpl->setToolTip(QStringLiteral(
        "The name suggested for every CONVERTED export — .glb, .png, .dds — "
        "from the tabs, the preview pane and the per-part batch. {{Name}} is "
        "the asset's stem, {{Game}} is TPP / MGO3 / GZ / Survive, {{Hash}} its "
        "64-bit path hash. Characters a filename cannot carry become "
        "underscores, and a template that resolves to nothing falls back to "
        "the stem. Raw extractions keep their shipped names, because those are "
        "meant to go back into the game."));
    nameRow->addRow(QStringLiteral("File name"), nameTpl);
    nv->addLayout(nameRow);
    {
        auto* n = new QLabel(m_names);
        n->setWordWrap(true);
        n->setText(QStringLiteral(
            "Applies to every CONVERTED file this build writes — .glb, .png, "
            ".dds — from every tab, the preview pane, the per-part batch and "
            "the viewport captures. Raw extractions deliberately keep the "
            "names they ship under, because those files are meant to go back "
            "into the game."));
        nv->addWidget(n);
    }

    // ── Where a BATCH puts them (template §8) ─────────────────────────────
    // Persisted as a STABLE ID, restored with findData, and an id this build
    // does not know reads as Flat rather than as a group name that matches
    // nothing. The combo lives here rather than on the Bulk tab because the
    // Models tab's multi-select export and the context-menu batch ask the same
    // question, and three combos would be three answers.
    {
        auto* head = new QLabel(QStringLiteral("<b>Folders a batch writes into</b>"),
                                m_names);
        nv->addWidget(head);
        auto* form = new QFormLayout();
        auto* layout = new QComboBox(m_names);
        for (const ExportLayout::Mode& m : ExportLayout::modes()) {
            layout->addItem(m.label, m.id);
            layout->setItemData(layout->count() - 1, m.hint, Qt::ToolTipRole);
        }
        const int at = layout->findData(ExportLayout::mode());
        layout->setCurrentIndex(at >= 0 ? at : 0);
        layout->setToolTip(QStringLiteral(
            "Applies to every path that writes MORE THAN ONE file — Bulk Extract, "
            "a multi-row export, the context menu's batch. Single-file exports "
            "ignore it deliberately: turning Ctrl+E into sna\\foo.glb is a "
            "surprise you cannot undo. Grouping by game, type or family reads "
            "the same tags the search box's #tags do, so a file with none goes "
            "to _misc rather than loose into the folder you chose."));
        form->addRow(QStringLiteral("Group by"), layout);
        nv->addLayout(form);
        m_w->layout = layout;
    }

    // ── What a bulk run converts ────────────────────────────────────────
    {
        auto* head = new QLabel(QStringLiteral("<b>Conversions on the way out</b>"),
                                m_names);
        nv->addWidget(head);
        auto* ftex = new QCheckBox(
            QStringLiteral("Assemble .ftex into .dds"), m_names);
        ftex->setChecked(io->assembleFtex);
        ftex->setToolTip(QStringLiteral(
            "A Fox texture ships as a small .ftex header plus its mip streams "
            "in .ftexs files beside it; on, the pieces are put back together "
            "into one .dds anything can open, and the raw streams are left "
            "out. Turn it off if you are repacking the game and want the bytes "
            "exactly as they shipped."));
        nv->addWidget(ftex);
        auto* wem = new QCheckBox(
            QStringLiteral("Convert .wem into .wav"), m_names);
        wem->setChecked(io->convertWem);
        wem->setToolTip(QStringLiteral(
            "PCM is re-wrapped natively; the packed Wwise codecs need "
            "vgmstream on your PATH. A conversion that fails writes the raw "
            ".wem instead, so nothing is dropped without a line in the log."));
        nv->addWidget(wem);
        m_w->ftex = ftex;
        m_w->wem = wem;
    }
    nv->addStretch(1);

    // ── Images && GIFs ──────────────────────────────────────────────────
    m_images = new QWidget(parent);
    auto* iv = new QVBoxLayout(m_images);
    // ── Captures ────────────────────────────────────────────────────────
    // On the EXPORT pages rather than in a dialog of their own, because
    // "export settings" is what someone looks for when they want the
    // turntable smaller, and a separate home for the same noun is a second
    // place to forget (template §10: one setting, one key, one home).
    auto* capBox = new QGroupBox(QStringLiteral("Captures"), m_images);
    auto* cf = new QFormLayout(capBox);

    auto* imgFmt = new QComboBox(capBox);
    // ONLY containers this build can actually write. WebP needs a Qt image
    // plugin that a portable deployment may not carry, and offering it there
    // means picking a .webp name and then getting a bare "could not write" —
    // after the dialog has already defaulted the extension for you.
    const QList<QByteArray> canWrite = QImageWriter::supportedImageFormats();
    const auto writable = [&canWrite](const char* f) {
        return canWrite.contains(QByteArray(f));
    };
    imgFmt->addItem(QStringLiteral("PNG — lossless"), QStringLiteral("png"));
    if (writable("jpg") || writable("jpeg"))
        imgFmt->addItem(QStringLiteral("JPEG"), QStringLiteral("jpg"));
    if (writable("webp"))
        imgFmt->addItem(QStringLiteral("WebP"), QStringLiteral("webp"));
    {
        const int i = imgFmt->findData(co.imageFormat);
        imgFmt->setCurrentIndex(i >= 0 ? i : 0);
    }
    cf->addRow(QStringLiteral("Image format"), imgFmt);

    auto* imgScale = new QSpinBox(capBox);
    imgScale->setRange(25, 400);
    imgScale->setSuffix(QStringLiteral(" %"));
    imgScale->setValue(co.imageScale);
    imgScale->setToolTip(QStringLiteral(
        "Above 100% the scene is RENDERED again at the larger size rather "
        "than upscaled, so the extra pixels carry extra detail."));
    cf->addRow(QStringLiteral("Image size"), imgScale);

    auto* imgQ = new QSpinBox(capBox);
    imgQ->setRange(1, 100);
    imgQ->setValue(co.imageQuality);
    imgQ->setToolTip(QStringLiteral("JPEG and WebP only; PNG is lossless."));
    cf->addRow(QStringLiteral("Image quality"), imgQ);

    auto* gifScale = new QSpinBox(capBox);
    gifScale->setRange(25, 100);
    gifScale->setSuffix(QStringLiteral(" %"));
    gifScale->setValue(co.scalePct);
    gifScale->setToolTip(QStringLiteral(
        "Scale the frames before encoding. The cheapest size lever there is, "
        "until it starts costing detail."));
    cf->addRow(QStringLiteral("GIF size"), gifScale);

    auto* gifCrop = new QCheckBox(
        QStringLiteral("Crop the GIF to the model"), capBox);
    gifCrop->setChecked(co.cropToModel);
    gifCrop->setToolTip(QStringLiteral(
        "Trim the empty air around the model — one box for the whole turn, so "
        "the model does not swim about inside a shrinking frame."));
    cf->addRow(QString(), gifCrop);

    auto* gifTrans = new QCheckBox(
        QStringLiteral("Transparent background"), capBox);
    gifTrans->setChecked(co.transparent);
    gifTrans->setToolTip(QStringLiteral(
        "GIF has no alpha channel: one palette entry is reserved and the edges "
        "are hard. Right over a coloured page, wrong for a soft-edged shot."));
    cf->addRow(QString(), gifTrans);

    auto* gifOpt = new QCheckBox(
        QStringLiteral("Optimise the GIF to a size budget"), capBox);
    gifOpt->setChecked(co.optimize);
    gifOpt->setToolTip(QStringLiteral(
        "Re-encode until it fits: palette first, then dither, then "
        "resolution. The SMALLEST encode is written, not the last one tried. "
        "Frames are captured once, so the retries cost no rendering."));
    cf->addRow(QString(), gifOpt);

    auto* gifMB = new QSpinBox(capBox);
    gifMB->setRange(1, 200);
    gifMB->setSuffix(QStringLiteral(" MB"));
    gifMB->setValue(co.targetMB);
    gifMB->setEnabled(co.optimize);
    QObject::connect(gifOpt, &QCheckBox::toggled, gifMB, &QWidget::setEnabled);
    cf->addRow(QStringLiteral("Target size"), gifMB);
    iv->addWidget(capBox);

    auto* note = new QLabel(m_images);
    note->setWordWrap(true);
    note->setText(QStringLiteral(
        "These apply to every .glb and every capture this build writes, and "
        "are remembered. The settings used are printed in the log each time. "
        "Frame count and frame delay are asked for by the turntable itself, "
        "where the wait they cause is about to happen."));
    iv->addWidget(note);
    iv->addStretch(1);

    m_w->scale = scale;
    m_w->axis = axis;
    m_w->skel = skel;
    m_w->layer = layer;
    m_w->nrm = nrm;
    m_w->cnp = cnp;
    m_w->nameTpl = nameTpl;
    m_w->imgFmt = imgFmt;
    m_w->imgScale = imgScale;
    m_w->imgQ = imgQ;
    m_w->gifScale = gifScale;
    m_w->gifCrop = gifCrop;
    m_w->gifTrans = gifTrans;
    m_w->gifOpt = gifOpt;
    m_w->gifMB = gifMB;
}

void ExportPages::apply()
{
    if (!m_w) return;
    ExportOptions o = m_w->io;
    o.scale = m_w->scale->value();
    o.zUp = m_w->axis->currentData().toBool();
    o.skeleton = m_w->skel->isChecked();
    o.bakeColourLayer = m_w->layer->isChecked();
    o.normalMaps = m_w->nrm->isChecked();
    o.connectPoints = m_w->cnp->isChecked();
    o.nameTemplate = m_w->nameTpl->text().trimmed();
    o.assembleFtex = m_w->ftex->isChecked();
    o.convertWem = m_w->wem->isChecked();
    saveExportOptions(o);
    // ExportLayout owns its key, so this writes through it rather than keeping
    // a second copy in ExportOptions: one setting, one key, one reader.
    {
        const QString id = m_w->layout->currentData().toString();
        QSettings().setValue(QStringLiteral("export/folderLayout"),
                             ExportLayout::isKnown(id) ? id : ExportLayout::kFlat());
    }

    CaptureOptions c = m_w->co;
    c.imageFormat = m_w->imgFmt->currentData().toString();
    c.imageScale = m_w->imgScale->value();
    c.imageQuality = m_w->imgQ->value();
    c.scalePct = m_w->gifScale->value();
    c.cropToModel = m_w->gifCrop->isChecked();
    c.transparent = m_w->gifTrans->isChecked();
    c.optimize = m_w->gifOpt->isChecked();
    c.targetMB = m_w->gifMB->value();
    saveCaptureOptions(c);
}

}  // namespace fox
