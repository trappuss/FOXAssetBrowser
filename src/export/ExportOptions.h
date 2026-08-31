// ExportOptions.h — what "export" means, as settings you can see and keep.
//
// The .glb writer had one behaviour and no dial: metres, Y up, textures
// embedded, skeleton included, the runtime colour layer baked. Every one of
// those is right for some target and wrong for another — Blender wants Z up, a
// game engine wants centimetres, a print wants no rig at all — and getting
// them was a job for whatever tool the file landed in.
//
// So: a struct, a dialog, and a QSettings round trip. Nothing here changes what
// the exporter can do; it decides which of the things it can already do it
// should be doing this time.
//
// The DEFAULTS are exactly what the exporter did before this existed, so an
// export that ignores the dialog produces the file it always did.
#pragma once
#include <memory>
#include <QString>

class QWidget;
namespace glb {
struct SceneOptions;
}

namespace fox {

struct ExportOptions {
    // Multiplier on every vertex position, applied through the scene's root
    // node rather than by touching the vertices — a node transform is one
    // number in the JSON and cannot lose precision on a coordinate.
    double scale = 1.0;
    // Z up instead of glTF's Y up. Blender's importer has its own "Y up ->
    // Z up" conversion and most people leave it on, so this is for the ones
    // who turn it off and for engines that want the axes as authored.
    bool zUp = false;
    // Write the skeleton and skinning. Off exports the mesh in bind (or in the
    // current pose, which the exporter already bakes) with no rig at all —
    // which is what a static prop, a 3D print or a photogrammetry pipeline
    // wants, and is a much smaller file.
    bool skeleton = true;
    // Bake the runtime colour layer into the base map. Off exports the base
    // map as it ships — which for a customizable garment is white, and is what
    // you want if the colouring is going to be redone downstream.
    bool bakeColourLayer = true;
    // Write normal maps (unswizzled from Fox DXT5nm) with TANGENT data.
    bool normalMaps = true;

    // The exported file's NAME, as a template. Placeholders:
    //   {{Name}}  the model's stem ("tcl0_main0_def")
    //   {{Game}}  "TPP" / "MGO3" / "GZ" / "Survive"
    //   {{Hash}}  the asset's 64-bit path hash, in hex
    // Empty, or a template that resolves to nothing, falls back to the stem —
    // a run must never produce a file with no name.
    // Write the .fcnp connect points as empty nodes. ON by default: they are
    // a node each, they are the only record in the file of where a hat or a
    // scope goes, and every tool that reads glTF understands an empty.
    bool connectPoints = true;
    QString nameTemplate = QStringLiteral("{{Name}}");

    // ── What a BULK run converts on the way out ─────────────────────────
    // These were two checkboxes on the Bulk Extract tab, which is where they
    // did not belong: template §8 is explicit that the tab holds only run
    // controls and every export option lives in Settings ▸ Export, "shared
    // with every other export path: one setting, one key, one home". They are
    // export options — they decide what is written — so they are here, and the
    // tab reads them like every other path does.
    //
    // Assemble .ftex + its .ftexs streams into one .dds, rather than writing
    // the shipped pieces. Off is for someone repacking the game, who wants the
    // bytes back exactly as they came.
    bool assembleFtex = true;
    // .wem to playable .wav — PCM re-wrapped natively, packed Wwise codecs
    // through vgmstream when it is on PATH. A failed conversion writes the raw
    // .wem rather than dropping the file.
    bool convertWem = true;

    // Every field, as one line, for the log — an export whose settings are not
    // in the log is an export nobody can reproduce.
    QString describe() const;
};

// The writer's own view of the same five decisions. ONE converter, called by
// every .glb call site, because four hand-written copies of the same five
// assignments is four chances for one of them to be forgotten — which is
// exactly what happened to the Files tab and the preview pane.
glb::SceneOptions sceneOptionsFrom(const ExportOptions& o);

// The turntable/still capture settings. Here rather than beside the capture
// code so one header carries everything the export dialog and QSettings both
// have to know about.
struct CaptureOptions {
    int frames = 36;
    int delayCs = 4;
    int colors = 256;
    bool dither = true;
    bool alsoFrames = false;

    // ── What the encoder is handed ──────────────────────────────────────
    // Scale the captured frames before encoding. A GIF of a 1400px viewport
    // is enormous and nobody wants it at that size; this is the one lever
    // that costs nothing in quality per byte until it starts to.
    int scalePct = 100;          // 25..100
    // Trim every frame to the model's own bounds (with a small margin) rather
    // than shipping the viewport's empty air. Measured on the same pixels, so
    // it is a crop, not a re-render.
    bool cropToModel = false;
    // 1-bit transparency instead of the background colour. GIF has no alpha
    // channel — one palette entry is reserved and pixels below the threshold
    // take it — so edges are hard. Worth it over a coloured page, wrong for a
    // soft-edged shot.
    bool transparent = false;

    // ── The size budget ─────────────────────────────────────────────────
    // Off, the encode happens once at the settings above. On, it is retried
    // until it fits: palette first, then dither, then resolution — and the
    // SMALLEST encode is what gets written, not the last one tried.
    bool optimize = false;
    int targetMB = 10;           // 1..200

    // ── Still images ────────────────────────────────────────────────────
    // "png" / "jpg" / "webp". The save dialog takes its default extension and
    // filter from here so the setting and the dialog cannot disagree.
    QString imageFormat = QStringLiteral("png");
    // Above 100 the scene is genuinely RE-RENDERED larger rather than
    // upscaled, which is the whole point of asking for it.
    int imageScale = 100;        // 25..400
    int imageQuality = 92;       // 1..100, jpg/webp only
};

// QSettings round trip. Out-of-range values in a hand-edited file are clamped
// on the way OUT as well as in, so a reader never sees something a writer
// could not have produced.
// Apply a name template. Path separators and the other characters Windows
// forbids become underscores, so a template can never escape the folder the
// caller chose.
QString applyNameTemplate(const QString& tpl, const QString& stem,
                          const QString& game, quint64 hash);

// The CURRENT template, resolved against the index — the one call every
// export path should make. It exists because applyNameTemplate had a single
// call site for a while and every other .glb and .png in the build quietly
// ignored the "File name" field the dialog offered.
//
// fileIdx may be stale (a rescan swaps the file vector under a selection) or
// -1 (a composed scene belongs to no single file); either degrades to an
// empty {{Game}} and a zero {{Hash}} rather than refusing to name the file.
QString templatedStem(const QString& stem, int fileIdx);

ExportOptions loadExportOptions();
// Session-only override for the dev/screenshot harness, consulted by
// loadExportOptions() and NEVER persisted — a harness run must not rewrite the
// user's settings. Same shape as Config's --game / --dict overrides. CLAMPED
// like the stored values: a command line is the one input nothing validates,
// and "--exportscale 0" produced a scene collapsed to a point with the log
// cheerfully reporting "scale 0".
void setSessionExportOptions(const ExportOptions& o);
// The same, for the capture settings. Clamped, never persisted.
void setSessionCaptureOptions(const CaptureOptions& o);
void saveExportOptions(const ExportOptions& o);
CaptureOptions loadCaptureOptions();
void saveCaptureOptions(const CaptureOptions& o);

// ── The Settings ▸ Export pages (template §10) ──────────────────────────────
// These used to be a standalone "Export settings" modal — a complete, careful
// dialog with, as it turned out, NO CALLER anywhere in the application. The
// template puts export options in Settings ▸ Export, "shared with every other
// export path: one setting, one key, one home", so that is where they are now,
// and the orphan dialog is gone rather than being left as a second home.
//
// Three pages, per §10's "Export gets sub-tabs": what a .glb contains, what a
// capture looks like, and what files are called. `apply()` writes all three.
// The caller owns the widgets through their parents; this object only has to
// outlive the dialog.
class ExportPages {
public:
    // Builds the three pages. Each is a plain QWidget the caller adds to a tab
    // widget; none of them is a dialog and none of them has buttons, because
    // the OK the user presses belongs to the settings dialog around them.
    explicit ExportPages(QWidget* parent);
    QWidget* modelsPage() const { return m_models; }
    QWidget* imagesPage() const { return m_images; }
    QWidget* namesPage() const { return m_names; }
    // Persist everything on the three pages.
    void apply();

private:
    QWidget* m_models = nullptr;
    QWidget* m_images = nullptr;
    QWidget* m_names = nullptr;
    struct Widgets;
    // A shared_ptr rather than a unique_ptr so this header does not need the
    // definition to declare a destructor.
    std::shared_ptr<Widgets> m_w;
};

}  // namespace fox
