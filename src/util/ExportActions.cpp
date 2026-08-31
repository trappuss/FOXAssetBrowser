// ExportActions.cpp — see ExportActions.h.
#include "util/ExportActions.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>
#include <QMessageBox>
#include <QSaveFile>

#include "app/Config.h"
#include "audio/WemFile.h"
#include "fox/BcDecode.h"
#include <QDialog>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QLabel>
#include <QRegularExpression>
#include <QBrush>
#include <algorithm>
#include "fox/FmdlFile.h"
#include "index/TextureUsers.h"
#include "app/Hotkeys.h"
#include "util/TableCopy.h"
#include "index/ArchiveIndex.h"
#include "index/NameCatalog.h"
#include "util/Extract.h"
#include "util/ModFolder.h"
#include "fox/FtexWriter.h"
#include "util/ExportLayout.h"
#include "util/MenuText.h"
#include "export/ExportOptions.h"
#include "model/GlbExporter.h"
#include "preview/ModelLoader.h"
#include "util/Extract.h"

using fox::ArchiveIndex;
using fox::IndexedFile;

namespace exportactions {
namespace {

QString baseNameOf(int fileIdx)
{
    const auto& files = ArchiveIndex::instance().files();
    if (fileIdx < 0 || fileIdx >= files.size()) return {};
    const QString rel = extract::relativePathFor(files[fileIdx]);
    return rel.section(QLatin1Char('/'), -1);
}

// Ask where to save; remembers the folder. Empty on cancel.
//
// `toLast` skips the dialog and writes into the remembered folder under the
// suggested name. Every export in this file goes through here, so putting the
// choice in ONE place is what makes "there is a `to last folder` twin for
// every export" true by construction rather than by twelve remembered edits.
QString askSavePath(QWidget* parent, const QString& title,
                    const QString& suggestedName, const QString& filter,
                    bool toLast = false)
{
    if (toLast) {
        const QString dir = Config::exportDir();
        if (dir.isEmpty()) return {};
        return QDir(dir).filePath(suggestedName);
    }
    const QString out = QFileDialog::getSaveFileName(
        parent, title, QDir(Config::exportDir()).filePath(suggestedName),
        filter);
    if (!out.isEmpty()) Config::setExportDir(QFileInfo(out).absolutePath());
    return out;
}

bool writeBlob(QWidget* parent, const QString& path, const QByteArray& data)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()
        || !f.commit()) {
        QMessageBox::warning(parent, QStringLiteral("Export"),
                             QStringLiteral("Could not write %1").arg(path));
        return false;
    }
    return true;
}

bool warn(QWidget* parent, const QString& why)
{
    QMessageBox::warning(parent, QStringLiteral("Export"), why);
    return false;
}

}  // namespace

bool exportRaw(int fileIdx, QWidget* parent, bool toLast)
{
    const auto& files = ArchiveIndex::instance().files();
    if (fileIdx < 0 || fileIdx >= files.size()) return false;
    // Copy by VALUE: the modal dialog spins the event loop, and a finishing
    // rescan swaps the file vector under a reference.
    const IndexedFile f = files[fileIdx];
    const QString name = baseNameOf(fileIdx);
    const QString out = askSavePath(parent, QStringLiteral("Extract %1").arg(name),
                                    name, QStringLiteral("All files (*)"), toLast);
    if (out.isEmpty()) return false;
    const QByteArray data = ArchiveIndex::instance().readFile(f);
    if (data.isEmpty() && f.size != 0)
        return warn(parent, QStringLiteral("Could not read the entry."));
    return writeBlob(parent, out, data);
}

bool exportFtexDds(int fileIdx, QWidget* parent, bool toLast)
{
    const auto& files = ArchiveIndex::instance().files();
    if (fileIdx < 0 || fileIdx >= files.size()) return false;
    QString err;
    const QByteArray dds = extract::assembleFtexToDds(files[fileIdx], &err);
    if (dds.isEmpty())
        return warn(parent, QStringLiteral("Texture assembly failed: %1").arg(err));
    QString base = baseNameOf(fileIdx);
    if (base.endsWith(QLatin1String(".ftex"))) base.chop(5);
    base = fox::templatedStem(base, fileIdx);
    const QString out =
        askSavePath(parent, QStringLiteral("Export DDS"),
                    base + QStringLiteral(".dds"),
                    QStringLiteral("DDS texture (*.dds)"), toLast);
    if (out.isEmpty()) return false;
    return writeBlob(parent, out, dds);
}

bool exportFtexPng(int fileIdx, QWidget* parent, bool toLast)
{
    const auto& files = ArchiveIndex::instance().files();
    if (fileIdx < 0 || fileIdx >= files.size()) return false;
    QString err;
    const QByteArray dds = extract::assembleFtexToDds(files[fileIdx], &err);
    const QImage img = fox::bc::decodeDds(dds);
    if (img.isNull())
        return warn(parent, QStringLiteral("Texture decode failed: %1").arg(err));
    QString base = baseNameOf(fileIdx);
    if (base.endsWith(QLatin1String(".ftex"))) base.chop(5);
    base = fox::templatedStem(base, fileIdx);
    const QString out =
        askSavePath(parent, QStringLiteral("Export PNG"),
                    base + QStringLiteral(".png"),
                    QStringLiteral("PNG image (*.png)"), toLast);
    if (out.isEmpty()) return false;
    if (!img.save(out))
        return warn(parent, QStringLiteral("Could not write %1").arg(out));
    return true;
}

bool writeFtexPng(int fileIdx, const QString& outPath, QString* error)
{
    const auto& files = ArchiveIndex::instance().files();
    if (fileIdx < 0 || fileIdx >= files.size()) {
        if (error) *error = QStringLiteral("no such file in the index");
        return false;
    }
    QString err;
    const QByteArray dds = extract::assembleFtexToDds(files[fileIdx], &err);
    const QImage img = fox::bc::decodeDds(dds);
    if (img.isNull()) {
        if (error) *error = err.isEmpty() ? QStringLiteral("decode failed") : err;
        return false;
    }
    QDir().mkpath(QFileInfo(outPath).absolutePath());
    if (!img.save(outPath)) {
        if (error) *error = QStringLiteral("could not write %1").arg(outPath);
        return false;
    }
    return true;
}

bool exportFmdlGlb(int fileIdx, QWidget* parent, bool toLast)
{
    modelload::LoadedModel lm = modelload::load(fileIdx);
    if (!lm.ok)
        return warn(parent, QStringLiteral("Model load failed: %1").arg(lm.error));
    QString base = baseNameOf(fileIdx);
    if (base.endsWith(QLatin1String(".fmdl"))) base.chop(5);
    base = fox::templatedStem(base, fileIdx);
    const QString out =
        askSavePath(parent, QStringLiteral("Export glTF binary"),
                    base + QStringLiteral(".glb"),
                    QStringLiteral("glTF binary (*.glb)"), toLast);
    if (out.isEmpty()) return false;
    QString err;
    // The user's export settings, like every other .glb this build writes.
    const fox::ExportOptions eo = fox::loadExportOptions();
    qInfo("export: %s", qUtf8Printable(eo.describe()));
    if (!glb::exportGlb(lm.model, lm.textures, out, &err, nullptr,
                        &lm.normalMaps, fox::sceneOptionsFrom(eo)))
        return warn(parent, QStringLiteral("glb export failed: %1").arg(err));
    return true;
}

bool exportWemWav(int fileIdx, QWidget* parent, bool toLast)
{
    const auto& files = ArchiveIndex::instance().files();
    if (fileIdx < 0 || fileIdx >= files.size()) return false;
    const QByteArray wem = ArchiveIndex::instance().readFile(files[fileIdx]);
    const audio::WemInfo info = audio::parseWem(wem);
    if (!info.riff)
        return warn(parent, QStringLiteral("Not a RIFF/Wwise audio file."));
    QByteArray wav;
    QString err;
    if (info.isPcm()) wav = audio::wemToWav(wem, info);
    else wav = audio::convertWithVgmstream(wem, &err);
    if (wav.isEmpty())
        return warn(parent,
                    err.isEmpty()
                        ? QStringLiteral("Audio decode failed.")
                        : QStringLiteral("Audio decode failed: %1").arg(err));
    QString base = baseNameOf(fileIdx);
    if (base.endsWith(QLatin1String(".wem"))) base.chop(4);
    const QString out =
        askSavePath(parent, QStringLiteral("Save wav"),
                    base + QStringLiteral(".wav"),
                    QStringLiteral("WAVE audio (*.wav)"), toLast);
    if (out.isEmpty()) return false;
    return writeBlob(parent, out, wav);
}

// One copy action, built to §1 and §6: the label carries the value it will put
// on the clipboard, an empty value renders as "—" and the action is DISABLED
// rather than hidden, and the value is snapshotted into the lambda before the
// menu ever runs (§4 — menu.exec() spins a nested event loop and the index can
// move under it).
static void addCopyAction(QMenu* menu, const QString& label,
                          const QString& value)
{
    QAction* a = menu->addAction(MenuText::withCopyValue(label, value), menu,
                                 [value] {
                                     QApplication::clipboard()->setText(value);
                                 });
    a->setEnabled(!value.isEmpty());
}

// The multi-row form. One value per LINE, so the clipboard pastes as a list.
static void addCopyActionRows(QMenu* menu, const QString& label,
                              const QStringList& values)
{
    const QString joined = values.join(QLatin1Char('\n'));
    QAction* a = menu->addAction(MenuText::withRows(label, values.size()), menu,
                                 [joined] {
                                     QApplication::clipboard()->setText(joined);
                                 });
    a->setEnabled(!values.isEmpty());
}

void addCopyActions(QMenu* menu, const QString& path)
{
    if (path.isEmpty()) return;
    addCopyAction(menu, MenuText::kCopyPath, path);
    addCopyAction(menu, MenuText::kCopyFileName,
                  path.section(QLatin1Char('/'), -1));
}

// ── Replace / Revert ────────────────────────────────────────────────────────
// Both end in a RESCAN, and that is not a detail: the replacement only exists
// as far as this tool is concerned once the index has walked the mod folder
// and decided the new copy wins. Writing the file and leaving the index
// holding the old one is the failure this feature has to be unable to have —
// it would show the user the game's copy of a file they had just replaced and
// give them no way to tell which they were looking at.
static void replaceAsset(const QString& assetPath, QWidget* parent)
{
    const QString name = assetPath.section(QLatin1Char('/'), -1);
    const QString src = QFileDialog::getOpenFileName(
        parent, QStringLiteral("Replace %1 with…").arg(name),
        Config::exportDir(), QStringLiteral("All files (*)"));
    if (src.isEmpty()) return;
    const QString err = modfolder::putFile(assetPath, src);
    if (!err.isEmpty()) {
        QMessageBox::warning(parent, QStringLiteral("Replace"), err);
        return;
    }
    qInfo("mod: replaced %s <- %s", qUtf8Printable(assetPath),
          qUtf8Printable(src));
    modfolder::notifyChanged();
}

// A TEXTURE IS NOT ONE FILE, so replacing one is not one copy.
//
// The DDS is re-encoded into the ORIGINAL texture's own layout — same header,
// same mip-to-stream assignment — and the .ftex plus every .N.ftexs it needs
// are installed as ONE unit. Three of four would leave a texture mounted with
// mismatched mips: garbage on screen, from a mod folder that looks correct.
static void replaceTexture(int fileIdx, const QString& assetPath,
                           QWidget* parent)
{
    const auto& files = ArchiveIndex::instance().files();
    if (fileIdx < 0 || fileIdx >= files.size()) return;
    const QByteArray original = ArchiveIndex::instance().readFile(files[fileIdx]);
    if (original.isEmpty()) {
        QMessageBox::warning(parent, QStringLiteral("Replace texture"),
                             QStringLiteral("The original texture could not be "
                                            "read out of the archive."));
        return;
    }
    const QString name = assetPath.section(QLatin1Char('/'), -1);
    const QString src = QFileDialog::getOpenFileName(
        parent, QStringLiteral("Replace %1 with…").arg(name), Config::exportDir(),
        QStringLiteral("DirectDraw Surface (*.dds);;All files (*)"));
    if (src.isEmpty()) return;
    QFile in(src);
    if (!in.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(parent, QStringLiteral("Replace texture"),
                             QStringLiteral("Could not read %1").arg(src));
        return;
    }
    const QByteArray dds = in.readAll();
    in.close();

    const fox::FtexWriteResult w = fox::writeFtexLike(original, dds);
    if (!w.ok()) {
        QMessageBox::warning(parent, QStringLiteral("Replace texture"), w.error);
        return;
    }
    // The stream files sit beside the .ftex under its own stem, which is the
    // name the loose mount will hash them back from.
    const QString stem = assetPath.left(assetPath.size() - 5);
    QVector<QPair<QString, QByteArray>> set;
    set.append({assetPath, w.ftex});
    for (auto it = w.ftexs.constBegin(); it != w.ftexs.constEnd(); ++it)
        set.append({stem + QStringLiteral(".%1.ftexs").arg(it.key()), it.value()});
    const QString err = modfolder::putSet(set);
    if (!err.isEmpty()) {
        QMessageBox::warning(parent, QStringLiteral("Replace texture"), err);
        return;
    }
    qInfo("mod: replaced texture %s <- %s (%lld file(s))",
          qUtf8Printable(assetPath), qUtf8Printable(src), qint64(set.size()));
    modfolder::notifyChanged();
}

static void revertAsset(const QString& assetPath, QWidget* parent)
{
    const QString err = modfolder::revert(assetPath);
    if (!err.isEmpty()) {
        QMessageBox::warning(parent, QStringLiteral("Revert"), err);
        return;
    }
    qInfo("mod: reverted %s", qUtf8Printable(assetPath));
    modfolder::notifyChanged();
}

QAction* addExportPair(QMenu* menu, QWidget* parent, const QString& label,
                       const std::function<void(bool)>& run, bool enabled)
{
    QAction* ask = menu->addAction(MenuText::prompts(label), parent,
                                   [run] { run(false); });
    ask->setEnabled(enabled);
    // Hidden, not greyed, when there is no remembered folder: "to last
    // folder" with no folder is not a thing the user can enable by
    // clicking it (§7).
    const QString lastDir = Config::exportDir();
    if (!lastDir.isEmpty())
        menu->addAction(
            MenuText::withValue(
                QStringLiteral("%1 to last folder").arg(label),
                MenuText::condensePath(lastDir)),
            parent, [run] { run(true); })
            ->setEnabled(enabled);
    return ask;
}

void addFileActions(QMenu* menu, int fileIdx, QWidget* parent,
                    const std::function<void(int)>& jumpTo)
{
    const auto& files = ArchiveIndex::instance().files();
    if (fileIdx < 0 || fileIdx >= files.size()) return;
    const IndexedFile& f = files[fileIdx];
    const QString ext = ArchiveIndex::extensionOf(f);
    const QString name = baseNameOf(fileIdx);

    // The first action is what "export the selection" means for a FILE, so it
    // carries that role — see app/Hotkeys.h for why this is a stamped role and
    // not a search through the labels.
    // ── EVERY EXPORT COMES IN A PAIR ────────────────────────────────────
    // One that asks where, and one that writes into the folder you last chose
    // and says which folder that is. The user asked for both on every export
    // type — models, textures, images, audio — and only the batch extract had
    // the pair. `pair` is the one place that decides what the twin is called,
    // so a new export cannot be added with only half of it.
    const auto pair = [&](const QString& label,
                          const std::function<void(bool)>& run) {
        return addExportPair(menu, parent, label, run);
    };

    Hotkeys::Role::set(
        pair(QStringLiteral("Extract %1").arg(name),
             [fileIdx, parent](bool last) { exportRaw(fileIdx, parent, last); }),
        Hotkeys::Role::exportSelection());

    if (ext == QLatin1String("ftex")) {
        pair(QStringLiteral("Export as DDS"), [fileIdx, parent](bool last) {
            exportFtexDds(fileIdx, parent, last);
        });
        pair(QStringLiteral("Export as PNG"), [fileIdx, parent](bool last) {
            exportFtexPng(fileIdx, parent, last);
        });
    } else if (ext == QLatin1String("fmdl")) {
        pair(QStringLiteral("Export as .glb"), [fileIdx, parent](bool last) {
            exportFmdlGlb(fileIdx, parent, last);
        });
    } else if (ext == QLatin1String("wem")) {
        // Always offered; the operation itself explains a missing decoder.
        // (Probing the codec here would decompress the whole entry just to
        // grey a menu item — too slow for a right-click.)
        pair(QStringLiteral("Export as .wav"), [fileIdx, parent](bool last) {
            exportWemWav(fileIdx, parent, last);
        });
    }

    // ── THE MOD FOLDER ───────────────────────────────────────────────────
    // Replace goes in the SHARED builder, beside the exports, for the same
    // reason §12's entries do: a file is a file whether it was right-clicked
    // in a list, a tile, a tree node or the viewport, and a write action that
    // only some of those offered would be a worse answer than none.
    //
    // §7 — OMIT, DO NOT DISABLE. With no mod folder set there is nothing to
    // click that would enable it, so the entries are simply not there and a
    // one-line note says where the setting is. An unnamed asset is refused for
    // a REAL reason (the mount resolves by path, so a hash-only file could be
    // written and would override nothing) and that reason is said out loud
    // rather than shown as a grey row.
    // The separator belongs to the BLOCK, so it is added only when the block
    // has something in it. Adding it unconditionally left two rules stacked on
    // top of each other whenever no mod folder was set — a doubled divider
    // fencing off nothing, which is what --assetmenu printed as "--- | ---".
    if (!modfolder::dir().isEmpty()) {
        menu->addSeparator();
        const bool named = !f.path.isEmpty() && f.named;
        const QString assetPath = f.path;
        if (!named) {
            QAction* why = menu->addAction(
                QStringLiteral("Cannot be replaced — this install has no name "
                               "for it"));
            why->setEnabled(false);
        } else if (modfolder::overrides(assetPath)) {
            menu->addAction(
                MenuText::withValue(QStringLiteral("Replaced by"),
                                    MenuText::condensePath(
                                        modfolder::pathFor(assetPath))))
                ->setEnabled(false);
            menu->addAction(
                MenuText::prompts(QStringLiteral("Replace again")), parent,
                [assetPath, parent] { replaceAsset(assetPath, parent); });
            menu->addAction(
                QStringLiteral("Revert to the game's own copy"), parent,
                [assetPath, parent] { revertAsset(assetPath, parent); });
        } else {
            menu->addAction(
                MenuText::prompts(QStringLiteral("Replace with a file")),
                parent, [assetPath, parent] { replaceAsset(assetPath, parent); });
        }
        // …and for a TEXTURE, the one that does the conversion. Offered
        // alongside the raw copy rather than instead of it: dropping in
        // somebody else's already-converted .ftex is a real workflow, and so
        // is authoring a DDS in a paint program. They are different inputs,
        // not two spellings of one action.
        if (named && ext == QLatin1String("ftex"))
            menu->addAction(
                MenuText::prompts(QStringLiteral("Replace texture from a DDS")),
                parent, [fileIdx, assetPath, parent] {
                    replaceTexture(fileIdx, assetPath, parent);
                });
    }

    // §12's two entries, in the shared builder so every list, tile, tree node
    // and viewport click offers them — which is the whole point of §12.
    menu->addSeparator();
    addVariantActions(menu, fileIdx, parent, jumpTo);
    addDependencyActions(menu, fileIdx, parent, jumpTo);

    // §3's copy block. Every value is snapshotted HERE, before the menu runs.
    menu->addSeparator();
    addCopyAction(menu, MenuText::kCopyPath, f.path);
    addCopyAction(menu, MenuText::kCopyFileName,
                  f.path.isEmpty() ? name : f.path.section(QLatin1Char('/'), -1));
    // Fox's identity. "Copy SNO" in the spec; the engine's term here.
    addCopyAction(menu, MenuText::kCopyHash,
                  QStringLiteral("0x%1").arg(f.hash, 16, 16, QLatin1Char('0')));
    // A localised in-game name, where this asset HAS one. Offered rather than
    // faked: for most assets the entry name IS the file name, and two labels
    // for one payload means whichever you pick you get the same string (§5).
    // NameCatalog keys on the model STEM, which is the file name without its
    // extension — the extension is split at the FIRST dot everywhere in this
    // tool, so `x.1.ftexs` has stem `x`.
    QString stem = f.path.section(QLatin1Char('/'), -1);
    if (const int d = stem.indexOf(QLatin1Char('.')); d > 0) stem.truncate(d);
    if (const QString label = fox::NameCatalog::instance().nameFor(stem);
        !label.isEmpty())
        addCopyAction(menu, MenuText::kCopyName, label);
}



// What this asset needs and what needs it, in one dialog, both directions of
// the same map read two ways.
//
// A MODEL's dependencies come from its own material table — the walk
// index/TextureUsers does to build its cache, done here for one file. A
// TEXTURE's come from that cache read backwards, which is what it is for. The
// two are not two features: they are one relation, and the dialog says which
// end of it the user is standing on.
static void showDependencies(int fileIdx, QWidget* parent,
                             const std::function<void(int)>& jumpTo)
{
    const ArchiveIndex& index = ArchiveIndex::instance();
    const auto& files = index.files();
    if (fileIdx < 0 || fileIdx >= files.size()) return;
    const IndexedFile& f = files[fileIdx];
    const QString ext = ArchiveIndex::extensionOf(f);
    const QString title = baseNameOf(fileIdx);

    auto* dlg = new QDialog(parent);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(QStringLiteral("Dependencies — %1").arg(title));
    dlg->resize(720, 480);
    auto* lay = new QVBoxLayout(dlg);
    auto* note = new QLabel(dlg);
    note->setWordWrap(true);
    lay->addWidget(note);
    auto* tree = new QTreeWidget(dlg);
    tree->setColumnCount(3);
    tree->setHeaderLabels({QStringLiteral("Asset"), QStringLiteral("Role"),
                           QStringLiteral("Material")});
    tree->setRootIsDecorated(true);
    lay->addWidget(tree, 1);
    // §12, and this one especially: a dependency list is exactly the thing
    // somebody wants to paste into a bug report.
    tablecopy::installWithMenu(tree);

    const auto row = [&](QTreeWidgetItem* parentItem, const QString& asset,
                         const QString& role, const QString& material,
                         int target) {
        auto* it = parentItem ? new QTreeWidgetItem(parentItem)
                              : new QTreeWidgetItem(tree);
        it->setText(0, asset);
        it->setText(1, role);
        it->setText(2, material);
        // -1 and 0 mean different things here and an unset property reads as
        // 0 (convention 12), so the index is stored as a QVariant that is
        // TESTED for validity rather than compared against a sentinel.
        if (target >= 0) it->setData(0, Qt::UserRole, target);
        else it->setForeground(0, QBrush(Qt::gray));
        return it;
    };

    int needs = 0, needed = 0;

    // ── What it needs (a model's textures) ───────────────────────────────
    if (ext == QLatin1String("fmdl")) {
        fox::FmdlFile m;
        if (m.parse(index.readFile(f))) {
            auto* head = row(nullptr, QStringLiteral("Needs"), QString(),
                             QString(), -1);
            head->setExpanded(true);
            for (const fox::FmdlMaterialInstance& mat : m.materials())
                for (const fox::FmdlTextureRef& t : mat.textures) {
                    // An unresolved reference is REPORTED, not skipped. "This
                    // model wants a texture whose name this install cannot
                    // resolve" is the answer someone opening this dialog is
                    // most likely looking for.
                    const QString shown =
                        t.path.isEmpty()
                            ? QStringLiteral("0x%1 (name not in the dictionary)")
                                  .arg(t.pathHash, 16, 16, QLatin1Char('0'))
                            : t.path;
                    // The HASH, not the path: a texture reference's resolved
                    // name carries no extension and the index keys on one, so
                    // fileIndexForPath returned -1 for rows whose name had
                    // resolved perfectly well. findByHash is what ModelLoader
                    // uses to fetch the pixels for the same reference.
                    int target = -1;
                    if (const fox::IndexedFile* tf =
                            index.findByHash(t.pathHash)) {
                        const qsizetype ti = tf - index.files().constData();
                        if (ti >= 0 && ti < index.files().size())
                            target = int(ti);
                    }
                    row(head, shown, t.role, mat.name, target);
                    ++needs;
                }
        }
    }

    // ── What needs it (a texture's models) ───────────────────────────────
    if (ext == QLatin1String("ftex")) {
        const fox::TextureUsers& users = fox::TextureUsers::instance();
        auto* head = row(nullptr, QStringLiteral("Needed by"), QString(),
                         QString(), -1);
        head->setExpanded(true);
        if (!users.ready()) {
            // Distinct from "nothing uses it" — an unfinished sweep answering
            // "no users" is the confidently wrong answer, and this dialog is
            // exactly where someone would believe it.
            row(head, QStringLiteral("The texture→model sweep has not finished "
                                     "yet — ask again in a moment."),
                QString(), QString(), -1);
        } else {
            for (const fox::TextureUse& u : users.usesOf(f.hash)) {
                row(head,
                    u.modelPath.isEmpty()
                        ? QStringLiteral("0x%1").arg(u.modelHash, 16, 16,
                                                     QLatin1Char('0'))
                        : u.modelPath,
                    u.role, u.material,
                    u.modelPath.isEmpty()
                        ? -1
                        : index.fileIndexForPath(u.modelPath));
                ++needed;
            }
            if (needed == 0)
                row(head, QStringLiteral("Nothing in this install references it."),
                    QString(), QString(), -1);
        }
    }

    note->setText(
        QStringLiteral("%1 — %2").arg(
            f.path.isEmpty() ? title : f.path,
            ext == QLatin1String("fmdl")
                ? QStringLiteral("%1 texture reference(s)").arg(needs)
                : QStringLiteral("%1 model(s) reference it").arg(needed)));

    if (jumpTo) {
        QObject::connect(tree, &QTreeWidget::itemDoubleClicked, dlg,
                         [jumpTo, dlg](QTreeWidgetItem* it, int) {
                             const QVariant v = it->data(0, Qt::UserRole);
                             if (!v.isValid()) return;
                             jumpTo(v.toInt());
                             dlg->close();
                         });
    }
    tree->resizeColumnToContents(0);
    dlg->show();
}

// ── §12: Variants ▸ ────────────────────────────────────────────────────────
// The vocabulary is stated in the paths and nowhere else needs to be guessed.
// TPP names a character part <family><n>_<part><n>_<fit>, and a variant of an
// asset differs from it ONLY in the trailing variant token. So the stem is the
// name with that token removed, and two assets in the same folder sharing a
// stem are variants of each other.
//
// The tokens, all of them observed in the shipped archives:
//   _v00 _v01 …        numbered variation
//   _vrtn003 …         the longer spelling of the same thing
//   _def _cov _dmg …   the fit vocabulary (default, covered, damaged, …)
//
// What this deliberately does NOT do is compare renders to decide whether two
// assets look like versions of each other. That is a standing instruction, and
// it is also worse: a name is a statement by the people who built the game,
// and a render is an inference about one.
QString variantStemOf(const QString& path)
{
    QString name = path.section(QLatin1Char('/'), -1);
    // The extension is split at the FIRST dot, as everywhere else in this tool.
    const int dot = name.indexOf(QLatin1Char('.'));
    if (dot > 0) name.truncate(dot);

    // _v00 / _vrtn003, and MGO's special-colour packs _m01 _m02 _m03 _m68
    // (gold, silver, copper, light blue). The last of those is measured, not
    // assumed: /Assets/mgo/fova/weapon/dlc_specialColor/{m01_gold,m02_silver,
    // m03_copper,m68_lightBlue}/ each hold one pack per weapon, named
    // <weapon>_m<NN>. `_m\\d+$` cannot swallow a part token — "_main0" fails
    // it at the 'a'.
    static const QRegularExpression kNumbered(
        QStringLiteral("_(?:v|vrtn|m)\\d+$"));
    static const QStringList kFits = {
        QStringLiteral("def"), QStringLiteral("cov"), QStringLiteral("dmg"),
        QStringLiteral("conv"), QStringLiteral("arm"), QStringLiteral("alp"),
    };
    // At most one of each, and the numbered token is the outer one:
    // "…_v03_def" and "…_def" are variants of the same stem.
    for (int pass = 0; pass < 2; ++pass) {
        const QRegularExpressionMatch m = kNumbered.match(name);
        if (m.hasMatch()) { name.truncate(m.capturedStart()); continue; }
        const QString last = name.section(QLatin1Char('_'), -1);
        if (kFits.contains(last) && name.contains(QLatin1Char('_')))
            name.truncate(name.size() - last.size() - 1);
        else
            break;
    }
    return name;
}

void addVariantActions(QMenu* menu, int fileIdx, QWidget* parent,
                       const std::function<void(int)>& jumpTo)
{
    if (!jumpTo) return;
    const auto& files = ArchiveIndex::instance().files();
    if (fileIdx < 0 || fileIdx >= files.size()) return;
    const QString path = files[fileIdx].path;
    if (path.isEmpty()) return;
    const QString dir = path.section(QLatin1Char('/'), 0, -2);
    const QString stem = variantStemOf(path);
    if (stem.isEmpty() || dir.isEmpty()) return;
    const QString ext = ArchiveIndex::extensionOf(files[fileIdx]);

    // Same folder, same stem, same extension. The extension matters: a model
    // and its .fv2 share a stem by construction and are not variants of each
    // other — one describes the other.
    QVector<QPair<QString, int>> found;
    for (int i = 0; i < files.size(); ++i) {
        if (i == fileIdx) continue;
        const QString& p = files[i].path;
        if (p.isEmpty()) continue;
        if (p.section(QLatin1Char('/'), 0, -2) != dir) continue;
        if (ArchiveIndex::extensionOf(files[i]) != ext) continue;
        if (variantStemOf(p) != stem) continue;
        QString label = p.section(QLatin1Char('/'), -1);
        const int d = label.indexOf(QLatin1Char('.'));
        if (d > 0) label.truncate(d);
        found.append({label, i});
    }
    if (found.isEmpty()) return;
    // Sorted by NAME, not by file index: a QHash-ordered menu is convention 11
    // in another costume, and a variant list that reorders between runs is
    // unusable as well as wrong.
    std::sort(found.begin(), found.end(),
              [](const QPair<QString, int>& a, const QPair<QString, int>& b) {
                  return a.first < b.first;
              });

    QMenu* sub = menu->addMenu(QStringLiteral("Variants (%1)").arg(found.size()));
    // Bounded, like every other generated menu in this tool: a folder with two
    // hundred variants would build a menu taller than the screen, and a menu
    // that runs off the screen is the §5 bug in a different place.
    constexpr int kMax = 40;
    for (int i = 0; i < found.size() && i < kMax; ++i) {
        const int target = found[i].second;
        sub->addAction(found[i].first, parent, [jumpTo, target] { jumpTo(target); });
    }
    if (found.size() > kMax) {
        sub->addSeparator();
        QAction* more = sub->addAction(
            QStringLiteral("…and %1 more").arg(found.size() - kMax));
        more->setEnabled(false);
    }
}

// ── §12: Show dependencies… ────────────────────────────────────────────────
void addDependencyActions(QMenu* menu, int fileIdx, QWidget* parent,
                          const std::function<void(int)>& jumpTo)
{
    const auto& files = ArchiveIndex::instance().files();
    if (fileIdx < 0 || fileIdx >= files.size()) return;
    const QString ext = ArchiveIndex::extensionOf(files[fileIdx]);
    if (ext != QLatin1String("fmdl") && ext != QLatin1String("ftex")) return;
    menu->addAction(QStringLiteral("Show dependencies…"), parent,
                    [fileIdx, parent, jumpTo] {
                        showDependencies(fileIdx, parent, jumpTo);
                    });
}

QString nounForFiles(const QVector<int>& fileIdxs)
{
    const auto& files = ArchiveIndex::instance().files();
    QString ext;
    for (int fi : fileIdxs) {
        if (fi < 0 || fi >= files.size()) continue;
        const QString e = ArchiveIndex::extensionOf(files[fi]);
        if (ext.isEmpty()) ext = e;
        else if (ext != e) return QStringLiteral("file");
    }
    // Only the extensions this tool has a WORD for. Calling a mixed bag of
    // .fpk and .gz "files" is honest; calling every .sbp a "model" is not.
    if (ext == QLatin1String("fmdl")) return QStringLiteral("model");
    if (ext == QLatin1String("ftex")) return QStringLiteral("texture");
    if (ext == QLatin1String("wem")) return QStringLiteral("sound");
    if (ext == QLatin1String("gani")) return QStringLiteral("clip");
    return QStringLiteral("file");
}

// The two halves of "extract a set": ASK where, or write to the folder already
// remembered. Split so the silent path and the prompting path cannot drift —
// they share every line below the directory, which is the part with the output
// layout, the rescan guard and the failure counting in it.
int extractSetTo(const QVector<int>& fileIdxs, const QString& dir);

int extractSet(const QVector<int>& fileIdxs, QWidget* parent)
{
    if (fileIdxs.isEmpty()) return 0;
    const QString noun = nounForFiles(fileIdxs);
    const QString dir = QFileDialog::getExistingDirectory(
        parent,
        QStringLiteral("Extract %1 %2 to…")
            .arg(fileIdxs.size())
            .arg(MenuText::plural(noun, fileIdxs.size())),
        Config::exportDir());
    if (dir.isEmpty()) return 0;
    Config::setExportDir(dir);
    const int written = extractSetTo(fileIdxs, dir);
    // The PROMPTING path reports in a dialog: the user asked for this and is
    // waiting on it. The silent path does not — a modal box is not "silent",
    // and an action whose whole promise is "writes where I said without asking"
    // must not then interrupt.
    QMessageBox::information(
        parent, QStringLiteral("Extract"),
        written == fileIdxs.size()
            ? QStringLiteral("Extracted %1 %2.")
                  .arg(written).arg(MenuText::plural(noun, written))
            : QStringLiteral("Extracted %1 %2; %3 failed (see log).")
                  .arg(written)
                  .arg(MenuText::plural(noun, written))
                  .arg(fileIdxs.size() - written));
    return written;
}

int extractSetTo(const QVector<int>& fileIdxs, const QString& dir)
{
    if (fileIdxs.isEmpty() || dir.isEmpty()) return 0;
    const QString noun = nounForFiles(fileIdxs);
    const ArchiveIndex& index = ArchiveIndex::instance();
    int written = 0, failed = 0;
    // THE SHARED OUTPUT LAYOUT (template §8). This is a BATCH path — several
    // rows selected, or a whole folder from the tree — so it obeys the one
    // setting in Settings ▸ Export, exactly as Bulk Extract does. It did not,
    // which meant a user who set "By family" got grouped folders from one of
    // the two batch paths and a flat dump from the other, with nothing to say
    // why. The single-file paths (the preview pane's button, the context
    // menu's "Export as…") deliberately still ignore it.
    const QString layout = ExportLayout::mode();
    for (const ExportLayout::Group& g : ExportLayout::group(layout, fileIdxs)) {
        const QString groupDir = ExportLayout::folderFor(dir, g);
        for (const int fi : g.items) {
            // A rescan can finish while the directory dialog was open — the
            // captured indices are then meaningless.
            if (fi < 0 || fi >= index.files().size()) { ++failed; continue; }
            const IndexedFile& f = index.files()[fi];
            const QByteArray data = index.readFile(f);
            if (data.isEmpty() && f.size != 0) { ++failed; continue; }
            const QString rel = extract::relativePathFor(f);
            const QString full = QDir(groupDir).filePath(rel);
            QDir().mkpath(QFileInfo(full).absolutePath());
            QSaveFile out(full);
            if (!out.open(QIODevice::WriteOnly)) { ++failed; continue; }
            out.write(data);
            if (out.commit()) ++written;
            else ++failed;
        }
    }
    // Always on and bounded: the silent path has no dialog, so the log is the
    // only place its outcome exists.
    qInfo("extract: %d %s written to %s%s", written,
          qUtf8Printable(MenuText::plural(noun, written)),
          qUtf8Printable(dir),
          failed ? qUtf8Printable(QStringLiteral(" — %1 FAILED").arg(failed))
                 : "");
    return written;
}

void addFileSetActions(QMenu* menu, const QVector<int>& fileIdxs,
                       QWidget* parent)
{
    if (fileIdxs.isEmpty()) return;
    if (fileIdxs.size() == 1) { addFileActions(menu, fileIdxs.first(), parent); return; }
    const QString noun = nounForFiles(fileIdxs);

    // §3: the "to last folder" variant comes FIRST and is OMITTED ENTIRELY
    // when no folder is remembered. It used to be offered regardless, reading
    // "Extract 3 files to last folder" with no folder named, and then opening a
    // directory dialog — a label promising the opposite of what it did.
    const QString lastDir = Config::exportDir();
    if (!lastDir.isEmpty() && QDir(lastDir).exists()) {
        menu->addAction(
            MenuText::exportLastLabel(QStringLiteral("Extract"), fileIdxs.size(),
                                      QString(), noun, lastDir),
            parent, [fileIdxs, lastDir] { extractSetTo(fileIdxs, lastDir); });
    }
    // No oneName: the count branch is the only one this can take.
    Hotkeys::Role::set(
        menu->addAction(
            MenuText::prompts(MenuText::exportLabel(
                QStringLiteral("Extract"), fileIdxs.size(), QString(), noun)),
            parent, [fileIdxs, parent] { extractSet(fileIdxs, parent); }),
        Hotkeys::Role::exportSelection());

    // §3's multi-selection copy block: one value per line, paste-ready.
    menu->addSeparator();
    const auto& files = ArchiveIndex::instance().files();
    QStringList paths, names, hashes;
    for (const int fi : fileIdxs) {
        if (fi < 0 || fi >= files.size()) continue;
        const IndexedFile& f = files[fi];
        paths << f.path;
        names << (f.path.isEmpty() ? baseNameOf(fi)
                                   : f.path.section(QLatin1Char('/'), -1));
        hashes << QStringLiteral("0x%1").arg(f.hash, 16, 16, QLatin1Char('0'));
    }
    addCopyActionRows(menu, MenuText::kCopyPath, paths);
    addCopyActionRows(menu, MenuText::kCopyFileName, names);
    addCopyActionRows(menu, MenuText::kCopyHash, hashes);
}

}  // namespace exportactions
