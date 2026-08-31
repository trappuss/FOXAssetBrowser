#pragma once
// Thin wrapper over QSettings for the paths and switches the app needs.
// Portable: main() points QSettings at data/ beside the exe.
#include <QString>
#include <QStringList>

class Config {
public:
    // Folders scanned for SQAR archives (game installs; multiple supported so
    // TPP + GZ + Survive can be browsed side by side).
    static QStringList gameDirs();
    static void setGameDirs(const QStringList& dirs);

    // Dictionary folder. Empty = the dict/ folder beside the exe.
    static QString dictDir();
    static void setDictDir(const QString& dir);

    // Deep-scan containers (FPK/FPKD/PFTXS listings) during indexing.
    static bool deepScan();
    static void setDeepScan(bool on);

    // Textures ▸ checkerboard behind transparent pixels. A PREFERENCE about
    // how every texture is drawn, not a per-image action — it was a toggle
    // button on the image bar, which is why it was the only control there
    // that persisted nothing across a restart.
    static bool textureAlphaBg();
    static void setTextureAlphaBg(bool on);

    // Full PBR shading, per viewport. Three independent switches because the
    // three viewports are used for different things: the Files preview is a
    // flick-through where load time dominates, the Models tab is where a model
    // is actually looked at, and the Customize tab rebuilds a whole character
    // every time a row changes. OFF loads the base colour and normal map only
    // — which is what every viewport did before this existed.
    //
    // Which viewport a caller IS lives in the enum, not in the call site, so
    // three tabs cannot drift onto two keys.
    enum class PbrView { Files, Models, Customize };
    static bool pbrEnabled(PbrView view);
    static void setPbrEnabled(PbrView view, bool on);

    // ── The viewport's starting state ────────────────────────────────────
    // What a new viewport comes up as. Not a live setting — the N-panel edits
    // the viewport in front of you and does not write here — so changing these
    // affects the next viewport built. In practice all three viewports are
    // constructed once, with the main window, so that means the next launch —
    // which the dialog says out loud rather than leaving to be discovered.
    //
    // The environment is stored by ID. ViewEnvironment::find returns null for
    // an id no build knows; the one caller (fox::attachViewportPanel) falls
    // back to the first preset, so a stale or hand-edited setting can never
    // produce a rig no preset describes.
    static QString viewEnvironment();
    static void setViewEnvironment(const QString& id);
    static double viewExposure();
    static void setViewExposure(double e);
    // Whether the N-panel is already open on a viewport that has just been
    // built. Off by default: it covers part of the model, and a panel you
    // asked for is different from one that is simply there.
    static bool viewPanelOpen();
    // Frame every model as it loads, rather than keeping the camera. ON by
    // default: models here differ in size by two orders of magnitude, and a
    // camera framed for a pistol shows nothing at all of a helicopter.
    static bool viewAutoFit();
    static void setViewAutoFit(bool on);
    // Remember the viewport's own state across launches — the environment,
    // the exposure, the light rig, the overlays, the projection. OFF by
    // default, because a tool that comes up in whatever state a one-off
    // experiment left it in is a tool nobody can describe over a screenshot.
    static bool rememberViewport();
    static void setRememberViewport(bool on);
    static void setViewPanelOpen(bool on);

    // Last-used extraction output folder.
    static QString exportDir();
    static void setExportDir(const QString& dir);
    // Session-only "last folder", for the harness — exactly as --game, --dict
    // and --moddir are. It is what makes the "…to last folder" half of every
    // export pair reachable in a test run, and it also swallows setExportDir
    // so a run cannot rewrite the user's own remembered folder.
    static void setSessionExportDir(const QString& dir);

    // ── THE MOD FOLDER ───────────────────────────────────────────────────
    // A writable directory of replacement assets, mounted OVER the game's
    // archives at a higher priority than any of them. It is the same mechanic
    // the dev loose mount already used, promoted to a persisted, user-visible
    // setting — and it is the whole of this tool's write story.
    //
    // WHY A MOUNT RATHER THAN AN EDIT. Writing a replacement into chunk0.dat
    // means re-encrypting and re-packing a four-gigabyte archive, and a bug
    // there does not produce a wrong pixel, it produces a game that will not
    // boot. A mount is reversible by deleting one file, costs no archive
    // writer at all, and resolves through machinery this index already has:
    // IndexedFile::shadowed is already the flag for "a higher-priority mount
    // carries this hash", and fileIndexForPath already returns the copy that
    // wins. Nothing about the viewport, the exporter or the customizer had to
    // learn that a file might be a replacement.
    //
    // The layout is the extractor's own: <mod>/Assets/<game>/... — the loose
    // walk builds an asset path by stripping the root, and that is exactly
    // what extract::relativePathFor writes. The two are inverses already,
    // which is why "replace this file" is a copy and not a conversion.
    static QString modDir();
    static void setModDir(const QString& dir);

    // Session-only overrides for the dev/screenshot CLI (--game / --dict):
    // consulted before QSettings, NEVER persisted — a harness run must not
    // rewrite the user's configured folders.
    static void setSessionGameDirs(const QStringList& dirs);
    static void setSessionDictDir(const QString& dir);
    // Development: a directory of loose extracted assets mounted over the
    // archives, so a partial install can be topped up with the files a feature
    // actually needs. Empty = not mounted.
    static void setSessionLooseDir(const QString& dir);
    static QString sessionLooseDir();
    // …and the same for the mod folder, so a harness run can install and
    // revert replacements without ever writing the user's configured one.
    static void setSessionModDir(const QString& dir);
};
