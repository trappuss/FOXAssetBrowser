// FOXAssetBrowser — entry point.
//
// Native C++/Qt6 asset browser and bulk extractor for Fox Engine games
// (MGSV: The Phantom Pain, Ground Zeroes, Metal Gear Survive). Fully portable:
// settings, caches and logs live in data\ beside the exe.
#include <QApplication>
#include <QSurfaceFormat>
#include <QCommandLineParser>
#include <QIcon>
#include <QSettings>

#include "app/AppLog.h"
#include "app/Config.h"
#include "util/ModFolder.h"
#include "export/ExportOptions.h"
#include "app/AppPaths.h"
#include "app/MainWindow.h"
#include "app/SehGuard.h"
#include "util/CheckStyle.h"
#include "fox/BcDecode.h"
#include "index/ArchiveIndex.h"
#include "index/TextureUsers.h"
#include "util/QueryTerm.h"
#include "util/SearchQuery.h"

int main(int argc, char** argv)
{
    // A STENCIL BUFFER, for the selection silhouette (see
    // GLModelWidget::drawSelectionOutline). Requested on the DEFAULT format
    // before any QOpenGLWidget is built, because QOpenGLWidget takes its FBO's
    // format from this and a widget already constructed keeps the old one.
    //
    // Requesting it is not the same as getting it: a driver may hand back a
    // context whose format differs, so the outline pass asks the bound
    // framebuffer at draw time and falls back to a wireframe if the answer is
    // no. Depth is asked for explicitly alongside it — setting a stencil size
    // on a default-constructed format leaves depth at -1 on some drivers,
    // which is "don't care" and has been read as "none".
    {
        QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
        fmt.setDepthBufferSize(24);
        fmt.setStencilBufferSize(8);
        QSurfaceFormat::setDefaultFormat(fmt);
    }
    // Hardware faults (bad archive bytes → wild pointer) become catchable
    // exceptions on guarded paths instead of killing the process.
    seh::installSehTranslator();

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("FOXAssetBrowser"));
    QApplication::setApplicationName(QStringLiteral("FOXAssetBrowser"));
    QApplication::setApplicationVersion(QStringLiteral(FOXAB_VERSION));

    // Draw every check indicator ourselves, as an X. Wrapping the platform
    // style rather than replacing it, so the application keeps the native look
    // everywhere else. See CheckStyle.h for why the tick was worth replacing.
    // setStyle takes ownership of the proxy and parents it to the application;
    // the proxy in turn creates and owns its own base instance from the
    // platform's style key, so nothing here retains or frees the style that
    // was in force before.
    QApplication::setStyle(new fox::XCheckStyle);

    // Portable: every QSettings() default-ctor writes to an INI in data\ beside
    // the exe — no registry, no %AppData%. Must run before any QSettings use.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, AppPaths::dataDir());

    AppLog::install();
    qInfo("FOXAssetBrowser v%s starting",
          QApplication::applicationVersion().toLatin1().constData());

    // Superseded caches, gone before anything can read one (template §1). A
    // portable folder that keeps working when you move it should not also
    // accumulate a dead index per release.
    //
    // AFTER AppLog::install(), not before: this was the first thing main() did
    // and its qInfo went to Qt's default handler — which on a Windows GUI
    // build is a debugger nobody is attached to — so the one line that says it
    // happened never reached the log or the Ctrl+L console, while the settings
    // page told the user it had.
    if (const int pruned = fox::pruneOldCaches(); pruned > 0)
        qInfo("index: %d superseded cache file(s) pruned", pruned);

    // Decoder self-check: a broken BC table would otherwise ship silently wrong
    // pixels. Loud, not fatal.
    if (const QString err = fox::bc::selfTest(); !err.isEmpty())
        qWarning("SELF-TEST FAILED — %s", qUtf8Printable(err));
    // The search matcher's own invariants (template §4). Microseconds, and the
    // cheapest guard there is against a filtering site drifting away from the
    // shared helper — a regression that neither fails to compile nor crashes,
    // and shows up only as Bulk Extract exporting a different set from the one
    // on screen.
    if (const QString err = QueryTerm::selfTest(); !err.isEmpty())
        qWarning("SELF-TEST FAILED — %s", qUtf8Printable(err));

    // The query-string EDITORS, beside the term matcher. Same argument: a bug
    // in either silently changes the set the user filtered without changing
    // anything they can see.
    if (const QString err = searchq::selfTest(); !err.isEmpty())
        qWarning("SELF-TEST FAILED — %s", qUtf8Printable(err));

    {
        const QIcon appIcon(QStringLiteral(":/app_256.png"));
        if (!appIcon.isNull()) app.setWindowIcon(appIcon);
    }

    // ── Dev/agent CLI (screenshot harness — lets an automated session SEE the
    // running UI, e.g. under xvfb in a cloud sandbox) ────────────────────────
    //   --game <dir>     override the game folder for this run (not persisted)
    //   --dict <dir>     override the dictionary folder for this run
    //   --shot <png>     save a window screenshot once the index is ready, then quit
    //   --model <text>   before the shot: open Models tab, load first match
    //   --settle <ms>    delay between view ready and the grab (default 1500)
    //   --stay           keep running after the shot
    QCommandLineParser cli;
    cli.addOption({QStringLiteral("game"), QString(), QStringLiteral("dir")});
    cli.addOption({QStringLiteral("dict"), QString(), QStringLiteral("dir")});
    cli.addOption({QStringLiteral("loose"), QString(), QStringLiteral("dir")});
    cli.addOption({QStringLiteral("shot"), QString(), QStringLiteral("png")});
    cli.addOption({QStringLiteral("model"), QString(), QStringLiteral("text")});
    cli.addOption(QCommandLineOption(QStringLiteral("matpanel")));
    cli.addOption(QCommandLineOption(QStringLiteral("submeshes")));
    cli.addOption({QStringLiteral("hidemesh"), QString(), QStringLiteral("text")});
    cli.addOption({QStringLiteral("pbr"), QString(), QStringLiteral("0|1")});
    cli.addOption({QStringLiteral("viewenv"), QString(), QStringLiteral("id")});
    cli.addOption({QStringLiteral("viewdebug"), QString(), QStringLiteral("name")});
    cli.addOption({QStringLiteral("light"), QString(), QStringLiteral("az,el")});
    cli.addOption({QStringLiteral("keygain"), QString(), QStringLiteral("x")});
    cli.addOption({QStringLiteral("ambgain"), QString(), QStringLiteral("x")});
    cli.addOption({QStringLiteral("exposure"), QString(), QStringLiteral("x")});
    cli.addOption({QStringLiteral("viewpanel"), QString(), QStringLiteral("page")});
    // Template §5, so the harness can photograph what it built.
    cli.addOption({QStringLiteral("shading"), QString(),
                   QStringLiteral("wireframe|flat|shaded|rendered")});
    cli.addOption({QStringLiteral("channel"), QString(), QStringLiteral("name")});
    cli.addOption({QStringLiteral("overlays"), QString(),
                   QStringLiteral("comma list, or \"all\"/\"none\"")});
    cli.addOption({QStringLiteral("popover"), QString(),
                   QStringLiteral("graphics|lighting|camera")});
    // §10, so the settings dialog can be LOOKED at rather than reasoned about.
    cli.addOption({QStringLiteral("settings"), QString(),
                   QStringLiteral("tab name")});
    cli.addOption({QStringLiteral("turntable"), QString(), QStringLiteral("deg/s")});
    cli.addOption(QCommandLineOption(QStringLiteral("logconsole")));
    cli.addOption({QStringLiteral("capture"), QString(), QStringLiteral("png")});
    cli.addOption({QStringLiteral("turngif"), QString(), QStringLiteral("gif")});
    cli.addOption({QStringLiteral("animgif"), QString(), QStringLiteral("gif")});
    cli.addOption({QStringLiteral("frames"), QString(), QStringLiteral("n")});
    cli.addOption({QStringLiteral("exportparts"), QString(), QStringLiteral("dir")});
    // Customize: one slot on its own, one file per variation, and the slot
    // menu as text (a menu is a top-level window and never appears in a grab).
    cli.addOption({QStringLiteral("exportslot"), QString(),
                   QStringLiteral("slot=file")});
    cli.addOption({QStringLiteral("exportvariations"), QString(),
                   QStringLiteral("dir")});
    cli.addOption({QStringLiteral("slotmenu"), QString(),
                   QStringLiteral("slot|row")});
    cli.addOption({QStringLiteral("geardump"), QString(), QStringLiteral("tsv")});
    cli.addOption({QStringLiteral("icondump"), QString(), QStringLiteral("tsv")});
    cli.addOption({QStringLiteral("lightdump"), QString(), QStringLiteral("tsv")});
    cli.addOption({QStringLiteral("imageformat"), QString(), QStringLiteral("fmt")});
    cli.addOption({QStringLiteral("imagescale"), QString(), QStringLiteral("pct")});
    cli.addOption({QStringLiteral("gifbudget"), QString(), QStringLiteral("mb")});
    cli.addOption(QCommandLineOption(QStringLiteral("giftransparent")));
    cli.addOption(QCommandLineOption(QStringLiteral("gifcrop")));
    cli.addOption({QStringLiteral("gifcolors"), QString(), QStringLiteral("n")});
    cli.addOption({QStringLiteral("gifscale"), QString(), QStringLiteral("pct")});
    cli.addOption({QStringLiteral("exportname"), QString(), QStringLiteral("tpl")});
    cli.addOption({QStringLiteral("exportdir"), QString(), QStringLiteral("dir")});
    cli.addOption({QStringLiteral("unlocked"), QString(), QStringLiteral("0|1")});
    cli.addOption({QStringLiteral("exportscale"), QString(), QStringLiteral("x")});
    cli.addOption(QCommandLineOption(QStringLiteral("exportzup")));
    cli.addOption(QCommandLineOption(QStringLiteral("exportnorig")));
    cli.addOption({QStringLiteral("matfilter"), QString(), QStringLiteral("text")});
    cli.addOption({QStringLiteral("search"), QString(), QStringLiteral("text")});
    cli.addOption({QStringLiteral("export"), QString(), QStringLiteral("glb")});
    cli.addOption({QStringLiteral("exportanim"), QString(), QStringLiteral("glb")});
    cli.addOption({QStringLiteral("animclips"), QString(), QStringLiteral("spec")});
    cli.addOption({QStringLiteral("animpanel"), QString(), QStringLiteral("filter")});
    cli.addOption({QStringLiteral("animscope"), QString(),
                   QStringLiteral("model|all|other:<name>")});
    cli.addOption({QStringLiteral("exportanimdir"), QString(), QStringLiteral("dir")});
    cli.addOption({QStringLiteral("exportsceneanim"), QString(),
                   QStringLiteral("glb")});
    cli.addOption({QStringLiteral("strings"), QString(), QStringLiteral("filter")});
    cli.addOption(QCommandLineOption(QStringLiteral("stringsall")));
    cli.addOption({QStringLiteral("stringdump"), QString(), QStringLiteral("tsv")});
    cli.addOption({QStringLiteral("parts"), QString(), QStringLiteral("a,b,c")});
    cli.addOption({QStringLiteral("attach"), QString(), QStringLiteral("cnp")});
    cli.addOption({QStringLiteral("mtar"), QString(), QStringLiteral("text")});
    cli.addOption({QStringLiteral("clip"), QString(), QStringLiteral("text")});
    cli.addOption({QStringLiteral("frame"), QString(), QStringLiteral("n")});
    cli.addOption({QStringLiteral("settle"), QString(), QStringLiteral("ms")});
    cli.addOption({QStringLiteral("weapon"), QString(), QStringLiteral("spec")});
    cli.addOption({QStringLiteral("camo"), QString(), QStringLiteral("name")});
    cli.addOption({QStringLiteral("fova"), QString(), QStringLiteral("name")});
    cli.addOption({QStringLiteral("gearcolor"), QString(), QStringLiteral("stem")});
    cli.addOption({QStringLiteral("vehicle"), QString(), QStringLiteral("spec")});
    cli.addOption({QStringLiteral("preset"), QString(), QStringLiteral("name")});
    cli.addOption({QStringLiteral("userpreset"), QString(), QStringLiteral("name")});
    cli.addOption({QStringLiteral("savepreset"), QString(), QStringLiteral("name")});
    cli.addOption(QCommandLineOption(QStringLiteral("compat")));
    cli.addOption({QStringLiteral("character"), QString(), QStringLiteral("spec")});
    cli.addOption({QStringLiteral("popup"), QString(), QStringLiteral("which")});
    cli.addOption({QStringLiteral("popuptype"), QString(), QStringLiteral("text")});
    cli.addOption({QStringLiteral("hoverrow"), QString(), QStringLiteral("n")});
    cli.addOption({QStringLiteral("equipdump"), QString(), QStringLiteral("tsv")});
    cli.addOption({QStringLiteral("camodump"), QString(), QStringLiteral("tsv")});
    cli.addOption(QCommandLineOption(QStringLiteral("camodefault")));
    cli.addOption({QStringLiteral("restalignsweep"), QString(), QStringLiteral("tsv")});
    cli.addOption(QCommandLineOption(QStringLiteral("texusersweep")));
    cli.addOption({QStringLiteral("moddir"), QString(), QStringLiteral("dir")});
    cli.addOption({QStringLiteral("modreplace"), QString(),
                   QStringLiteral("asset=file")});
    cli.addOption({QStringLiteral("modrevert"), QString(), QStringLiteral("asset")});
    cli.addOption({QStringLiteral("modreplacetex"), QString(),
                   QStringLiteral("asset=dds")});
    cli.addOption({QStringLiteral("moddump"), QString(), QStringLiteral("tsv")});
    cli.addOption({QStringLiteral("modpackage"), QString(), QStringLiteral("zip")});
    cli.addOption({QStringLiteral("mgsvpackage"), QString(), QStringLiteral("mgsv")});
    // key=value pairs separated by ';' — name, version, author, website,
    // description, mgsversion, sbversion. Anything not given keeps its
    // default, and the name defaults to the output file's own base name.
    cli.addOption({QStringLiteral("mgsvmeta"), QString(), QStringLiteral("k=v;…")});
    // The metadata form, opened modelessly so --shot can photograph it. A
    // dialog is the one piece of this feature a log cannot describe.
    cli.addOption(QCommandLineOption(QStringLiteral("mgsvdialog")));
    cli.addOption({QStringLiteral("ftexroundtrip"), QString(), QStringLiteral("tsv")});
    cli.addOption({QStringLiteral("texdds"), QString(), QStringLiteral("asset=file")});
    cli.addOption({QStringLiteral("assetmenu"), QString(), QStringLiteral("asset")});
    cli.addOption({QStringLiteral("compatdump"), QString(), QStringLiteral("tsv")});
    cli.addOption({QStringLiteral("swatchdump"), QString(), QStringLiteral("dir")});
    cli.addOption({QStringLiteral("filedump"), QString(), QStringLiteral("tsv")});
    cli.addOption({QStringLiteral("hashdump"), QString(), QStringLiteral("sub=tsv")});
    cli.addOption({QStringLiteral("avatardump"), QString(), QStringLiteral("tsv")});
    cli.addOption({QStringLiteral("avatartexdump"), QString(), QStringLiteral("tsv")});
    cli.addOption({QStringLiteral("matdump"), QString(), QStringLiteral("model=dir")});
    cli.addOption({QStringLiteral("pngdump"), QString(), QStringLiteral("path=png")});
    cli.addOption({QStringLiteral("playerdump"), QString(), QStringLiteral("tsv")});
    // Bulk Extract, headless (§8). --bulkout is the one that RUNS; the rest
    // only configure it, so a typo in a filter cannot start a run by itself.
    cli.addOption({QStringLiteral("bulkout"), QString(), QStringLiteral("dir")});
    cli.addOption({QStringLiteral("bulk"), QString(), QStringLiteral("query")});
    cli.addOption({QStringLiteral("bulkext"), QString(), QStringLiteral("ext")});
    cli.addOption({QStringLiteral("bulkworkers"), QString(), QStringLiteral("n")});
    cli.addOption({QStringLiteral("bulkexisting"), QString(),
                   QStringLiteral("new|overwrite")});
    cli.addOption(QCommandLineOption(QStringLiteral("bulkqueue")));
    cli.addOption(QCommandLineOption(QStringLiteral("bulkqueueadd")));
    cli.addOption(QCommandLineOption(QStringLiteral("bulkall")));
    cli.addOption({QStringLiteral("bulkcancel"), QString(), QStringLiteral("ms")});
    cli.addOption({QStringLiteral("bulkpause"), QString(), QStringLiteral("ms")});
    cli.addOption(QCommandLineOption(QStringLiteral("cachecheck")));
    cli.addOption({QStringLiteral("texused"), QString(),
                   QStringLiteral("all|used|orphans")});
    cli.addOption({QStringLiteral("texformat"), QString(), QStringLiteral("fmt")});
    cli.addOption({QStringLiteral("texusertag"), QString(), QStringLiteral("tag")});
    cli.addOption({QStringLiteral("texchannel"), QString(), QStringLiteral("ch")});
    cli.addOption({QStringLiteral("npanel"), QString(), QStringLiteral("keys")});
    // "Are the sidebar panels actually draggable" is a measurement, not an
    // opinion: this reads the stack's heights, moves a boundary the way a drag
    // would, and reads them back.
    cli.addOption(QCommandLineOption(QStringLiteral("npanelsizes")));
    // NOT "--display": QApplication itself consumes -display as the X server
    // to connect to, and the two collide before the parser ever sees it —
    // "--display outliner" died with "could not connect to display outliner".
    cli.addOption({QStringLiteral("viewmode"), QString(),
                   QStringLiteral("list|outliner|grid")});
    cli.addOption({QStringLiteral("pickat"), QString(), QStringLiteral("x,y")});
    cli.addOption({QStringLiteral("rightdrag"), QString(),
                   QStringLiteral("x,y,dx,dy")});
    cli.addOption({QStringLiteral("gizmo"), QString(),
                   QStringLiteral("x|y|z|-x|-y|-z|ortho|hover:<n>")});
    // The asset-health audit (§13). Writes a TSV and diffs it against the
    // previous one, which is the whole point: after a game patch or a mod
    // install it reports "newly working / newly broken" rather than letting a
    // regression age into a bug report.
    cli.addOption({QStringLiteral("healthaudit"), QString(),
                   QStringLiteral("out.tsv")});
    cli.addOption({QStringLiteral("animsort"), QString(),
                   QStringLiteral("archive|name|asset|category")});
    cli.addOption({QStringLiteral("rowzoom"), QString(), QStringLiteral("n")});
    // The outliner's contents are inside collapsed headings, so no shot of it
    // could show a material's textures or an archive's clips. This opens them.
    cli.addOption({QStringLiteral("outliner"), QString(),
                   QStringLiteral("mesh,materials,armature,animations[,play]")});
    // The whole folder tree as a TSV. "I can't open sub-folders" cannot be
    // read off a screenshot; this says, per row, whether it has children,
    // whether it has an arrow, and how many models the grouping pass put
    // under it.
    cli.addOption({QStringLiteral("outlinerdump"), QString(),
                   QStringLiteral("out.tsv")});
    // A scripted selection sequence, because the RULES are invisible in a
    // screenshot: an outline says which parts are selected and nothing about
    // whether Ctrl toggled or replaced. Steps are separated by ";" and each is
    // "<gesture>@<x>,<y>" with gesture pick | ctrl | shift.
    cli.addOption({QStringLiteral("selseq"), QString(),
                   QStringLiteral("pick@x,y;ctrl@x,y;…")});
    // --undoseq: Customize's undo stack, scripted, printing the authored scene
    // description after every step. Selection rules were invisible in a
    // screenshot and so is an undo stack — see undoSeqReport(). Steps are
    // separated by ";" and each is either a build field ("slot=/Assets/…",
    // "camo=camo_c03") or the literal word undo or redo.
    cli.addOption({QStringLiteral("undoseq"), QString(),
                   QStringLiteral("field=value;undo;redo;…")});
    // --variantcensus: every variant group the naming vocabulary produces
    // across the whole index, largest first. "Variants ▸" is built on a claim
    // about how the game names things; this is that claim checked against the
    // real archives instead of taken on trust.
    cli.addOption({QStringLiteral("variantcensus"), QString(),
                   QStringLiteral("tsv")});
    // --tab <name>: which tab a shot photographs. Matched case-insensitively
    // against the tab bar's own labels, so "Files", "Textures", "Models",
    // "Customize", "Bulk Extract".
    //
    // It did not exist, and its absence was invisible: every run that passed
    // one photographed whatever tab happened to be in front (Files, the first
    // one) and reported nothing wrong, so a --npanel run aimed at Textures
    // silently set the FILES column and produced a screenshot of the wrong
    // tab that looked entirely plausible.
    cli.addOption({QStringLiteral("tab"), QString(), QStringLiteral("name")});
    // --filemenu: open the canonical file context menu on the Models tab
    // and log every entry in it. §12 claims one menu everywhere; this is
    // how that claim gets checked instead of asserted.
    cli.addOption(QCommandLineOption(QStringLiteral("filemenu")));
    // --menudump: the whole menu bar as text. §6 is a shape, and a shape
    // can be checked against a dump and not against a screenshot.
    cli.addOption(QCommandLineOption(QStringLiteral("menudump")));
    // --partmenu x,y: right-click the viewport there and LOG the part
    // menu's shape (§4) without raising it.
    cli.addOption({QStringLiteral("partmenu"), QString(),
                   QStringLiteral("x,y")});
    cli.addOption({QStringLiteral("selectrows"), QString(),
                   QStringLiteral("n")});
    cli.addOption({QStringLiteral("viewkeys"), QString(), QStringLiteral("list")});
    cli.addOption(QCommandLineOption(QStringLiteral("viewhelp")));
    cli.addOption(QCommandLineOption(QStringLiteral("overlaymenu")));
    cli.addOption({QStringLiteral("channelscroll"), QString(),
                   QStringLiteral("n")});
    cli.addOption({QStringLiteral("dumpfile"), QString(), QStringLiteral("match=out")});
    cli.addOption({QStringLiteral("dumptree"), QString(), QStringLiteral("match=dir")});
    cli.addOption({QStringLiteral("fovadump"), QString(), QStringLiteral("match")});
    cli.addOption({QStringLiteral("matcensus"), QString(), QStringLiteral("match=tsv")});
    cli.addOption({QStringLiteral("colordump"), QString(), QStringLiteral("match")});
    cli.addOption({QStringLiteral("pbrdump"), QString(), QStringLiteral("match")});
    cli.addOption({QStringLiteral("fovacensus"), QString(), QStringLiteral("match=tsv")});
    cli.addOption({QStringLiteral("fovabind"), QString(), QStringLiteral("tsv")});
    cli.addOption({QStringLiteral("animdump"), QString(), QStringLiteral("tsv")});
    cli.addOption({QStringLiteral("animbind"), QString(),
                   QStringLiteral("model=tsv")});
    cli.addOption({QStringLiteral("paramdump"), QString(), QStringLiteral("match=tsv")});
    cli.addOption({QStringLiteral("bonesdump"), QString(), QStringLiteral("match")});
    cli.addOption({QStringLiteral("dfrmdump"), QString(), QStringLiteral("match")});
    cli.addOption({QStringLiteral("texdump"), QString(), QStringLiteral("match=out")});
    cli.addOption(QCommandLineOption(QStringLiteral("grid")));
    cli.addOption({QStringLiteral("texsearch"), QString(), QStringLiteral("text")});
    cli.addOption({QStringLiteral("gridicon"), QString(), QStringLiteral("px")});
    cli.addOption(QCommandLineOption(QStringLiteral("filterpopup")));
    cli.addOption({QStringLiteral("tagdump"), QString(), QStringLiteral("tsv")});
    cli.addOption({QStringLiteral("thumbdump"), QString(), QStringLiteral("sub=dir")});
    cli.addOption({QStringLiteral("exportmenu"), QString()});
    cli.addOption({QStringLiteral("stay"), QString()});
    cli.parse(QApplication::arguments());
    // values(), not value(): --game is repeatable, and the app has always
    // supported several installs side by side. Taking only the first meant a
    // harness run could not mount a model folder and a data1 folder together,
    // which is exactly what checking the material preset table needs.
    if (cli.isSet(QStringLiteral("game")))
        Config::setSessionGameDirs(cli.values(QStringLiteral("game")));
    if (cli.isSet(QStringLiteral("loose")))
        Config::setSessionLooseDir(cli.value(QStringLiteral("loose")));
    if (cli.isSet(QStringLiteral("dict")))
        Config::setSessionDictDir(cli.value(QStringLiteral("dict")));

    // ── The mod folder, before anything indexes ──────────────────────────
    // --moddir is a session override exactly as --game and --dict are: a
    // harness run must not rewrite the user's configured folder. The two
    // mutations run HERE rather than in the devshot handler because the whole
    // point of a replacement is that the index picks it up — applying it after
    // the scan would report on a mount that had not been walked yet, which is
    // the one failure this feature has to be able to rule out.
    if (cli.isSet(QStringLiteral("moddir")))
        Config::setSessionModDir(cli.value(QStringLiteral("moddir")));
    if (cli.isSet(QStringLiteral("exportdir")))
        Config::setSessionExportDir(cli.value(QStringLiteral("exportdir")));
    if (cli.isSet(QStringLiteral("modrevert"))) {
        const QString asset = cli.value(QStringLiteral("modrevert"));
        const QString err = modfolder::revert(asset);
        qInfo("mod: revert %s — %s", qUtf8Printable(asset),
              err.isEmpty() ? "ok" : qUtf8Printable(err));
    }
    // The texture path needs the INDEX (it reads the original out of the
    // archive to re-encode into its layout), so unlike the plain replace it
    // cannot run before the window. It is handled in the devshot instead.
    if (cli.isSet(QStringLiteral("modreplace"))) {
        const QString spec = cli.value(QStringLiteral("modreplace"));
        const QString asset = spec.section(QLatin1Char('='), 0, 0);
        const QString file = spec.section(QLatin1Char('='), 1);
        const QString err = modfolder::putFile(asset, file);
        qInfo("mod: replace %s <- %s — %s", qUtf8Printable(asset),
              qUtf8Printable(file), err.isEmpty() ? "ok" : qUtf8Printable(err));
    }

    MainWindow window;
    // The data probes are dumps, not screenshots — they should not need
    // --shot, and they deliberately replace the grab rather than accompany it.
    const bool anyDump = cli.isSet(QStringLiteral("equipdump"))
        || cli.isSet(QStringLiteral("camodump"))
        || cli.isSet(QStringLiteral("camodefault"))
        || cli.isSet(QStringLiteral("texusersweep"))
        || cli.isSet(QStringLiteral("moddump"))
        || cli.isSet(QStringLiteral("modpackage"))
        || cli.isSet(QStringLiteral("mgsvpackage"))
        || cli.isSet(QStringLiteral("slotmenu"))
        || cli.isSet(QStringLiteral("ftexroundtrip"))
        || cli.isSet(QStringLiteral("modreplacetex"))
        || cli.isSet(QStringLiteral("texdds"))
        || cli.isSet(QStringLiteral("assetmenu"))
        // …and the wardrobe sweep, which 9u shipped WITHOUT this and which
        // therefore did nothing at all unless a --tab or a --shot happened to
        // be beside it. Every test run in that batch had "--tab Customize" in
        // it, so the flag looked fine; the .bat written from it had neither
        // and would have sat there with no window and no output. It writes a
        // file and the run is over, which is the whole rule this list exists
        // for.
        || cli.isSet(QStringLiteral("restalignsweep"))
        || cli.isSet(QStringLiteral("compatdump"))
        || cli.isSet(QStringLiteral("swatchdump"))
        || cli.isSet(QStringLiteral("filedump"))
        || cli.isSet(QStringLiteral("hashdump"))
        || cli.isSet(QStringLiteral("avatardump"))
        || cli.isSet(QStringLiteral("avatartexdump"))
        || cli.isSet(QStringLiteral("matdump"))
        || cli.isSet(QStringLiteral("pngdump"))
        || cli.isSet(QStringLiteral("playerdump"))
        || cli.isSet(QStringLiteral("bulkout"))
        || cli.isSet(QStringLiteral("bulkqueueadd"))
        || cli.isSet(QStringLiteral("cachecheck"))
        || cli.isSet(QStringLiteral("dumpfile"))
        || cli.isSet(QStringLiteral("dumptree"))
        || cli.isSet(QStringLiteral("fovadump"))
        || cli.isSet(QStringLiteral("matcensus"))
        || cli.isSet(QStringLiteral("colordump"))
        || cli.isSet(QStringLiteral("pbrdump"))
        || cli.isSet(QStringLiteral("fovacensus"))
        || cli.isSet(QStringLiteral("fovabind"))
        || cli.isSet(QStringLiteral("animdump"))
        || cli.isSet(QStringLiteral("undoseq"))
        || cli.isSet(QStringLiteral("variantcensus"))
        || cli.isSet(QStringLiteral("tab"))
        || cli.isSet(QStringLiteral("filemenu"))
        || cli.isSet(QStringLiteral("menudump"))
        || cli.isSet(QStringLiteral("partmenu"))
        || cli.isSet(QStringLiteral("selectrows"))
        || cli.isSet(QStringLiteral("animbind"))
        || cli.isSet(QStringLiteral("paramdump"))
        || cli.isSet(QStringLiteral("bonesdump"))
        || cli.isSet(QStringLiteral("dfrmdump"))
        || cli.isSet(QStringLiteral("texdump"))
        || cli.isSet(QStringLiteral("tagdump"))
        || cli.isSet(QStringLiteral("thumbdump"))
        // The capture and batch-export flags are outputs in their own right —
        // they write a file and the run is over — so they arm the harness the
        // same way a dump does. Without this they were parsed only when --shot
        // was ALSO given, and "--model venom --capture out.png" opened the app
        // interactively and wrote nothing.
        || cli.isSet(QStringLiteral("capture"))
        || cli.isSet(QStringLiteral("turngif"))
        || cli.isSet(QStringLiteral("animgif"))
        || cli.isSet(QStringLiteral("exportparts"))
        // …and the plain .glb export, which was the ONE output flag the rule
        // above was written for and did not cover: "--model x --export a.glb"
        // opened the app interactively and wrote nothing, while
        // "--exportanim", "--exportparts" and "--exportsceneanim" beside it
        // all worked.
        || cli.isSet(QStringLiteral("export"))
        || cli.isSet(QStringLiteral("geardump"))
        || cli.isSet(QStringLiteral("icondump"))
        || cli.isSet(QStringLiteral("lightdump"))
        // …and the animated export, for the same reason: it writes a file and
        // the run is over.
        || cli.isSet(QStringLiteral("exportanim"))
        || cli.isSet(QStringLiteral("animpanel"))
        || cli.isSet(QStringLiteral("animscope"))
        || cli.isSet(QStringLiteral("exportanimdir"))
        || cli.isSet(QStringLiteral("stringdump"))
        || cli.isSet(QStringLiteral("healthaudit"))
        || cli.isSet(QStringLiteral("exportsceneanim"))
        // …and this batch's two, for the third time the same way: a flag that
        // writes a file and ends the run has to ARM the run. 9u shipped
        // --restalignsweep without this and the .bat built from it hung; the
        // list above already carries two comments about the same mistake.
        // Checked here rather than noticed later.
        || cli.isSet(QStringLiteral("exportslot"))
        || cli.isSet(QStringLiteral("exportvariations"));
    if (cli.isSet(QStringLiteral("shot")) || anyDump) {
        MainWindow::DevShot shot;
        shot.outPng = cli.value(QStringLiteral("shot"));
        shot.modelFilter = cli.value(QStringLiteral("model"));
        shot.showDebugPanel = cli.isSet(QStringLiteral("matpanel"));
        shot.viewEnv = cli.value(QStringLiteral("viewenv"));
        shot.viewDebug = cli.value(QStringLiteral("viewdebug"));
        shot.viewLight = cli.value(QStringLiteral("light"));
        shot.viewPanel = cli.value(QStringLiteral("viewpanel"));
        shot.shading = cli.value(QStringLiteral("shading"));
        shot.channel = cli.value(QStringLiteral("channel"));
        shot.overlays = cli.value(QStringLiteral("overlays"));
        shot.popover = cli.value(QStringLiteral("popover"));
        shot.settingsTab = cli.value(QStringLiteral("settings"));
        if (cli.isSet(QStringLiteral("settings")) && shot.settingsTab.isEmpty())
            shot.settingsTab = QStringLiteral("General");
        if (cli.isSet(QStringLiteral("turntable")))
            shot.viewTurn = cli.value(QStringLiteral("turntable")).toFloat();
        shot.logConsole = cli.isSet(QStringLiteral("logconsole"));
        shot.capturePng = cli.value(QStringLiteral("capture"));
        shot.captureGif = cli.value(QStringLiteral("turngif"));
        shot.captureAnimGif = cli.value(QStringLiteral("animgif"));
        shot.captureFrames = cli.value(QStringLiteral("frames")).toInt();
        shot.exportParts = cli.value(QStringLiteral("exportparts"));
        shot.gearDump = cli.value(QStringLiteral("geardump"));
        shot.iconDump = cli.value(QStringLiteral("icondump"));
        shot.lightDump = cli.value(QStringLiteral("lightdump"));
        if (cli.isSet(QStringLiteral("unlocked")))
            shot.gearUnlocked =
                cli.value(QStringLiteral("unlocked")) == QLatin1String("1") ? 1 : 0;
        // Export settings for THIS RUN only — never written to the user's
        // configuration, exactly as --game and --dict are not.
        if (cli.isSet(QStringLiteral("exportscale"))
            || cli.isSet(QStringLiteral("exportzup"))
            || cli.isSet(QStringLiteral("exportnorig"))
            || cli.isSet(QStringLiteral("exportname"))) {
            fox::ExportOptions eo = fox::loadExportOptions();
            if (cli.isSet(QStringLiteral("exportscale")))
                eo.scale = cli.value(QStringLiteral("exportscale")).toDouble();
            eo.zUp = cli.isSet(QStringLiteral("exportzup")) ? true : eo.zUp;
            if (cli.isSet(QStringLiteral("exportnorig"))) eo.skeleton = false;
            if (cli.isSet(QStringLiteral("exportname")))
                eo.nameTemplate = cli.value(QStringLiteral("exportname"));
            fox::setSessionExportOptions(eo);
        }
        // The CAPTURE settings, same rule: this run only. Written straight
        // into QSettings would be a harness run reaching into the user's
        // configuration, which is the thing --exportscale was careful not to
        // do; the session override below is read by loadCaptureOptions.
        if (cli.isSet(QStringLiteral("gifscale"))
            || cli.isSet(QStringLiteral("gifcolors"))
            || cli.isSet(QStringLiteral("gifcrop"))
            || cli.isSet(QStringLiteral("giftransparent"))
            || cli.isSet(QStringLiteral("gifbudget"))
            || cli.isSet(QStringLiteral("imagescale"))
            || cli.isSet(QStringLiteral("imageformat"))) {
            fox::CaptureOptions co = fox::loadCaptureOptions();
            if (cli.isSet(QStringLiteral("gifscale")))
                co.scalePct = cli.value(QStringLiteral("gifscale")).toInt();
            if (cli.isSet(QStringLiteral("gifcolors")))
                co.colors = cli.value(QStringLiteral("gifcolors")).toInt();
            if (cli.isSet(QStringLiteral("gifcrop"))) co.cropToModel = true;
            if (cli.isSet(QStringLiteral("giftransparent")))
                co.transparent = true;
            if (cli.isSet(QStringLiteral("gifbudget"))) {
                co.optimize = true;
                co.targetMB = cli.value(QStringLiteral("gifbudget")).toInt();
            }
            if (cli.isSet(QStringLiteral("imagescale")))
                co.imageScale = cli.value(QStringLiteral("imagescale")).toInt();
            if (cli.isSet(QStringLiteral("imageformat")))
                co.imageFormat = cli.value(QStringLiteral("imageformat"));
            fox::setSessionCaptureOptions(co);
        }
        if (cli.isSet(QStringLiteral("keygain")))
            shot.viewKey = cli.value(QStringLiteral("keygain")).toFloat();
        if (cli.isSet(QStringLiteral("ambgain")))
            shot.viewAmbient = cli.value(QStringLiteral("ambgain")).toFloat();
        if (cli.isSet(QStringLiteral("exposure")))
            shot.viewExposure = cli.value(QStringLiteral("exposure")).toFloat();
        shot.showSubmeshTree = cli.isSet(QStringLiteral("submeshes"));
        shot.hideSubmesh = cli.value(QStringLiteral("hidemesh"));
        shot.matFilter = cli.value(QStringLiteral("matfilter"));
        if (cli.isSet(QStringLiteral("pbr")))
            shot.pbrOverride = cli.value(QStringLiteral("pbr")).toInt() ? 1 : 0;
        shot.searchFilter = cli.value(QStringLiteral("search"));
        shot.texSearch = cli.value(QStringLiteral("texsearch"));
        shot.exportGlb = cli.value(QStringLiteral("export"));
        shot.exportAnim = cli.value(QStringLiteral("exportanim"));
        shot.exportAnimDir = cli.value(QStringLiteral("exportanimdir"));
        shot.exportSceneAnim = cli.value(QStringLiteral("exportsceneanim"));
        if (cli.isSet(QStringLiteral("strings"))) {
            shot.strings = true;
            shot.stringFilter = cli.value(QStringLiteral("strings"));
            shot.stringsAll = cli.isSet(QStringLiteral("stringsall"));
        }
        shot.stringDump = cli.value(QStringLiteral("stringdump"));
        shot.animClips = cli.value(QStringLiteral("animclips"));
        if (cli.isSet(QStringLiteral("animpanel"))) {
            shot.animPanel = true;
            shot.animPanelFilter = cli.value(QStringLiteral("animpanel"));
        }
        shot.partsFilter = cli.value(QStringLiteral("parts"));
        shot.attachCnp = cli.value(QStringLiteral("attach"));
        shot.mtarFilter = cli.value(QStringLiteral("mtar"));
        shot.clipFilter = cli.value(QStringLiteral("clip"));
        shot.frame = cli.value(QStringLiteral("frame")).toFloat();
        if (cli.isSet(QStringLiteral("settle")))
            shot.settleMs = cli.value(QStringLiteral("settle")).toInt();
        else
            shot.settleMs = 1500;
        shot.stay = cli.isSet(QStringLiteral("stay"));
        shot.weaponSpec = cli.value(QStringLiteral("weapon"));
        shot.camoFilter = cli.value(QStringLiteral("camo"));
        shot.fovaForce = cli.value(QStringLiteral("fova"));
        shot.gearColor = cli.value(QStringLiteral("gearcolor"));
        shot.vehicleSpec = cli.value(QStringLiteral("vehicle"));
        shot.presetFilter = cli.value(QStringLiteral("preset"));
        shot.userPreset = cli.value(QStringLiteral("userpreset"));
        shot.savePreset = cli.value(QStringLiteral("savepreset"));
        shot.compatOnly = cli.isSet(QStringLiteral("compat"));
        shot.charSpec = cli.value(QStringLiteral("character"));
        shot.popupCombo = cli.value(QStringLiteral("popup"));
        shot.hoverRow = cli.value(QStringLiteral("hoverrow")).toInt(&shot.hoverSet);
        shot.popupType = cli.value(QStringLiteral("popuptype"));
        shot.dumpEquip = cli.value(QStringLiteral("equipdump"));
        shot.dumpCamo = cli.value(QStringLiteral("camodump"));
        shot.camoDefaultTest = cli.isSet(QStringLiteral("camodefault"));
        shot.restAlignSweep = cli.value(QStringLiteral("restalignsweep"));
        shot.texUserSweep = cli.isSet(QStringLiteral("texusersweep"));
        shot.dumpMod = cli.value(QStringLiteral("moddump"));
        shot.modPackage = cli.value(QStringLiteral("modpackage"));
        shot.mgsvPackage = cli.value(QStringLiteral("mgsvpackage"));
        shot.mgsvMeta = cli.value(QStringLiteral("mgsvmeta"));
        shot.mgsvDialog = cli.isSet(QStringLiteral("mgsvdialog"));
        shot.exportSlot = cli.value(QStringLiteral("exportslot"));
        shot.exportVariations = cli.value(QStringLiteral("exportvariations"));
        shot.slotMenu = cli.value(QStringLiteral("slotmenu"));
        shot.ftexRoundTrip = cli.value(QStringLiteral("ftexroundtrip"));
        shot.modReplaceTex = cli.value(QStringLiteral("modreplacetex"));
        shot.texDds = cli.value(QStringLiteral("texdds"));
        shot.assetMenu = cli.value(QStringLiteral("assetmenu"));
        shot.dumpCompat = cli.value(QStringLiteral("compatdump"));
        shot.dumpSwatch = cli.value(QStringLiteral("swatchdump"));
        shot.dumpFiles = cli.value(QStringLiteral("filedump"));
        shot.dumpHash = cli.value(QStringLiteral("hashdump"));
        shot.dumpAvatar = cli.value(QStringLiteral("avatardump"));
        shot.dumpAvatarTex = cli.value(QStringLiteral("avatartexdump"));
        shot.dumpMaterials = cli.value(QStringLiteral("matdump"));
        shot.dumpPng = cli.value(QStringLiteral("pngdump"));
        shot.dumpPlayers = cli.value(QStringLiteral("playerdump"));
        shot.bulkOut = cli.value(QStringLiteral("bulkout"));
        shot.bulkQuery = cli.value(QStringLiteral("bulk"));
        shot.bulkExt = cli.value(QStringLiteral("bulkext"));
        shot.bulkWorkers = cli.value(QStringLiteral("bulkworkers")).toInt();
        shot.bulkOverwrite = cli.value(QStringLiteral("bulkexisting"))
                                 .compare(QLatin1String("overwrite"),
                                          Qt::CaseInsensitive) == 0;
        shot.bulkUseQueue = cli.isSet(QStringLiteral("bulkqueue"));
        shot.bulkAll = cli.isSet(QStringLiteral("bulkall"));
        shot.bulkCancelMs = cli.value(QStringLiteral("bulkcancel")).toInt();
        shot.bulkPauseMs = cli.value(QStringLiteral("bulkpause")).toInt();
        shot.cacheCheck = cli.isSet(QStringLiteral("cachecheck"));
        shot.texUsed = cli.value(QStringLiteral("texused"));
        shot.texFormat = cli.value(QStringLiteral("texformat"));
        shot.texUserTag = cli.value(QStringLiteral("texusertag"));
        shot.texChannel = cli.value(QStringLiteral("texchannel"));
        shot.pickAt = cli.value(QStringLiteral("pickat"));
        shot.rightDrag = cli.value(QStringLiteral("rightdrag"));
        shot.gizmo = cli.value(QStringLiteral("gizmo"));
        shot.healthAudit = cli.value(QStringLiteral("healthaudit"));
        shot.animSort = cli.value(QStringLiteral("animsort"));
        shot.outlinerProbe = cli.value(QStringLiteral("outliner"));
        shot.outlinerDump = cli.value(QStringLiteral("outlinerdump"));
        shot.npanelSizes = cli.isSet(QStringLiteral("npanelsizes"));
        shot.rowZoomSet = cli.isSet(QStringLiteral("rowzoom"));
        shot.rowZoom = cli.value(QStringLiteral("rowzoom")).toInt();
        shot.selSeq = cli.value(QStringLiteral("selseq"));
        shot.undoSeq = cli.value(QStringLiteral("undoseq"));
        shot.variantCensus = cli.value(QStringLiteral("variantcensus"));
        shot.tab = cli.value(QStringLiteral("tab"));
        shot.fileMenu = cli.isSet(QStringLiteral("filemenu"));
        shot.menuDump = cli.isSet(QStringLiteral("menudump"));
        shot.partMenu = cli.value(QStringLiteral("partmenu"));
        shot.selectRows = cli.value(QStringLiteral("selectrows")).toInt();
        shot.viewKeys = cli.value(QStringLiteral("viewkeys"));
        shot.npanel = cli.value(QStringLiteral("npanel"));
        shot.display = cli.value(QStringLiteral("viewmode"));
        shot.viewHelp = cli.isSet(QStringLiteral("viewhelp"));
        shot.overlayMenu = cli.isSet(QStringLiteral("overlaymenu"));
        shot.channelScrollSet = cli.isSet(QStringLiteral("channelscroll"));
        shot.channelScroll = cli.value(QStringLiteral("channelscroll")).toInt();
        shot.bulkQueueAdd = cli.isSet(QStringLiteral("bulkqueueadd"))
                                ? QStringLiteral("1") : QString();
        shot.dumpFile = cli.value(QStringLiteral("dumpfile"));
        shot.dumpTree = cli.value(QStringLiteral("dumptree"));
        shot.dumpFova = cli.value(QStringLiteral("fovadump"));
        shot.dumpMatCensus = cli.value(QStringLiteral("matcensus"));
        shot.dumpColors = cli.value(QStringLiteral("colordump"));
        shot.dumpPbr = cli.value(QStringLiteral("pbrdump"));
        shot.dumpFovaCensus = cli.value(QStringLiteral("fovacensus"));
        shot.dumpFovaBind = cli.value(QStringLiteral("fovabind"));
        shot.dumpAnims = cli.value(QStringLiteral("animdump"));
        shot.dumpAnimBind = cli.value(QStringLiteral("animbind"));
        shot.animScope = cli.value(QStringLiteral("animscope"));
        shot.dumpParams = cli.value(QStringLiteral("paramdump"));
        shot.dumpBones = cli.value(QStringLiteral("bonesdump"));
        shot.dumpDfrm = cli.value(QStringLiteral("dfrmdump"));
        shot.dumpTex = cli.value(QStringLiteral("texdump"));
        shot.grid = cli.isSet(QStringLiteral("grid"));
        shot.gridIcon = cli.value(QStringLiteral("gridicon")).toInt();
        shot.filterPopup = cli.isSet(QStringLiteral("filterpopup"));
        shot.dumpTags = cli.value(QStringLiteral("tagdump"));
        shot.dumpThumbs = cli.value(QStringLiteral("thumbdump"));
        shot.exportMenu = cli.isSet(QStringLiteral("exportmenu"));
        window.scheduleDevShot(shot);
    }
    window.show();
    const int rc = QApplication::exec();
    // BEFORE the static singletons this leans on are destroyed. The
    // texture→model sweep is a detached thread that locks a static mutex and
    // reads ArchiveIndex; a harness run quits mid-sweep every time, and a
    // detached thread that outlives its singletons locks a destroyed mutex.
    fox::TextureUsers::instance().shutdown();
    return rc;
}
