// ExportActions.h — THE shared export vocabulary. Every place a file can be
// exported from (the Export menu, right-click context menus, preview-pane
// buttons) goes through these builders, so single-file, menu and context
// exports can never disagree about formats or naming (the D4 shared-menu
// convention: extend the builders, don't fork per-view menus).
//
// addFileActions() appends the CONTEXTUAL set for one indexed file:
//   always: "Extract <name>…" (raw bytes, original format)
//   .ftex : "Export as DDS…" / "Export as PNG…" (streamed mips assembled)
//   .fmdl : "Export as .glb…" (textured, rigged)
//   .wem  : "Export as .wav…" (PCM re-wrap, or vgmstream for packed codecs)
//   text  : "Save as text…"
// plus "Copy path" / "Copy file name".
#pragma once
#include <QString>
#include <QVector>

class QMenu;
class QAction;
class QWidget;

#include <functional>

namespace exportactions {

// Append the contextual export actions for one ArchiveIndex file.
// `jumpTo`, when given, is how a view navigates to another asset — it is what
// makes "Variants ▸" and the dependency dialog's rows clickable. A view that
// cannot navigate omits it and those entries are simply not offered, rather
// than offered and dead.
void addFileActions(QMenu* menu, int fileIdx, QWidget* parent,
                    const std::function<void(int)>& jumpTo = {});

// ── EVERY EXPORT COMES IN A PAIR ────────────────────────────────────────────
// One entry that asks where, and one that writes into the folder you last
// chose and says which folder that is. `run(true)` means "the last folder".
//
// This was a lambda inside addFileActions, which meant it governed the FILE
// exports and nothing else — so the Customize tab's part and scene exports,
// which are the ones people repeat ten times in a row, had no silent twin at
// all, and the first attempt at giving them one invented a second phrasing for
// it. One function, so a new export cannot be added with only half of it and
// cannot be added under a different name.
//
// `label` carries no ellipsis: this adds it to the asking entry and not to the
// silent one, which is the rule §7 states and the reason it is applied here
// rather than by each caller.
// `enabled` applies to BOTH entries. It is a parameter rather than something
// the caller sets on the returned action because the caller only gets the
// asking one back — the first version of this left the silent twin live on an
// empty scene, an action that runs and does nothing, which is precisely what
// §0 forbids.
QAction* addExportPair(QMenu* menu, QWidget* parent, const QString& label,
                       const std::function<void(bool lastFolder)>& run,
                       bool enabled = true);

// ── Template §12: the two entries every view owes a single asset ──────────
// Both go into the canonical builder rather than into any one tab, which is
// the whole point of §12 — a list, a tile, a tree node and a viewport click
// must offer the same menu.

// "Variants ▸" — the other assets that are variants of this one, built ONLY
// from what the files state: the naming vocabulary the paths themselves carry
// (`_v00`, `_vrtn003`, and the fit tokens `_def` / `_cov` / `_dmg` …), and the
// co-located `.fv2` tables. Never from comparing renders — that is a standing
// instruction, and the alternative is exactly this: a binding the data states.
//
// `jumpTo` is called with the chosen file index. Views that cannot navigate
// pass nothing and the submenu is not added.
void addVariantActions(QMenu* menu, int fileIdx, QWidget* parent,
                       const std::function<void(int)>& jumpTo);

// "Show dependencies…" — what this asset needs and what needs it, both
// directions of the one map. A model's textures come from its own material
// table; a texture's models come from index/TextureUsers, which is that same
// walk already done and cached.
void addDependencyActions(QMenu* menu, int fileIdx, QWidget* parent,
                          const std::function<void(int)>& jumpTo);

// The variant STEM of an asset path: the part of the file name that its
// variants share. Public because it is a claim about the naming vocabulary
// that a harness can check against the real archives rather than take on
// trust — see --variantcensus.
QString variantStemOf(const QString& path);

// The same, for a SET. This is the context-sensitive half of the rule in
// util/MenuText.h: one file gets addFileActions and its per-format entries
// under its own name; several get one "Extract 12 models…" that says how many
// and what they are, and writes them through the shared output layout.
//
// A set of one is not a special case here — it is forwarded to addFileActions,
// so "select one row" and "click one row" cannot offer different menus. That
// divergence is exactly what this function exists to remove: the Files tab
// used to show the single-file menu AND a countless "Extract selection…"
// beside it, so a twelve-row selection offered one entry that acted on twelve
// rows and four that acted on the row under the cursor.
void addFileSetActions(QMenu* menu, const QVector<int>& fileIdxs,
                       QWidget* parent);

// What a set of files should be CALLED in a label: "models" when they all
// share an extension this tool has a word for, "files" otherwise.
QString nounForFiles(const QVector<int>& fileIdxs);

// Prompt for a folder and write every file in `fileIdxs` into it through the
// shared output layout (template §8). Returns the number written; reports the
// outcome to the user itself. This is the one batch-extract implementation —
// it was a private method of the Files tab, which is why no other view could
// offer it.
int extractSet(const QVector<int>& fileIdxs, QWidget* parent);

// The same, WITHOUT asking — for the "to last folder" menu entries, which say
// where they will write and so must not then open a dialog. Shares every line
// below the directory with extractSet: the output layout, the rescan guard and
// the failure counting are written once.
int extractSetTo(const QVector<int>& fileIdxs, const QString& dir);

// Append "Copy path" / "Copy file name" for a display path.
void addCopyActions(QMenu* menu, const QString& path);

// The individual operations (each opens a save dialog rooted at the last
// export folder; returns false on cancel/failure):
bool exportRaw(int fileIdx, QWidget* parent, bool toLast = false);
bool exportFtexDds(int fileIdx, QWidget* parent, bool toLast = false);
bool exportFtexPng(int fileIdx, QWidget* parent, bool toLast = false);
// The same decode, written to a path the caller chose and with no dialog. The
// MATERIALS panel's "export this material's images" needs one PNG per texture
// under the texture's own real name inside a folder named for the material,
// which is a set of writes with one dialog at the front — not one dialog per
// file. Returns false and leaves `error` set on failure.
bool writeFtexPng(int fileIdx, const QString& outPath, QString* error = nullptr);
bool exportFmdlGlb(int fileIdx, QWidget* parent, bool toLast = false);
bool exportWemWav(int fileIdx, QWidget* parent, bool toLast = false);

}  // namespace exportactions
