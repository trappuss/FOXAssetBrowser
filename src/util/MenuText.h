// MenuText.h — ONE vocabulary for every menu label in this application
// (template §12).
//
// Ported from D4AssetBrowser's util/ViewportPartMenu.h, whose header records
// what happens without it: the same clipboard action appeared as "Copy SNO
// id", "Copy SNO" and "Copy source SNO ID"; exporting to the remembered folder
// was "Export to last dir" in three tabs and "Export Model Last dir" in three
// others; and worst of the set, "Save image" saved silently to the last folder
// while "Save image…" opened a dialog — one ellipsis apart, opposite
// behaviours. This file exists so a label is written once.
//
// Rules, so additions stay consistent:
//   * Sentence case. "Export part", not "Export Part".
//   * A trailing "…" means AND ONLY MEANS "this opens a dialog". Never
//     decorative. prompts() is the only thing that adds one.
//   * An action that writes somewhere remembered says where:
//     "Export to last folder (…/FOX/exports)".
//   * Copy actions carry their value in parentheses, so you can read what you
//     are about to copy without invoking it.
//
// ── The context-sensitive export label ──────────────────────────────────────
// The rule the user stated, which now has one implementation: with ONE thing
// selected the label names it, and with several it counts them and says what
// they are. "Export bdf6_main0_def…" or "Export 12 models…", never "Export
// selection…" — a label that does not say what it will do is a label you have
// to test by pressing it. exportLabel() is that rule and every export site in
// the application goes through it, context menus included.
#pragma once
#include <QDir>
#include <QLocale>
#include <QString>
#include <QStringList>

namespace MenuText {

inline const QString kCopyPath     = QStringLiteral("Copy path");
inline const QString kCopyFileName = QStringLiteral("Copy file name");
inline const QString kCopyName     = QStringLiteral("Copy name");
inline const QString kCopyMaterial = QStringLiteral("Copy material name");

// ── The rest of the canonical vocabulary (docs/CONTEXT_MENUS.md §1) ────────
// EXTEND this list; never duplicate an entry with new wording. Everything
// below was inline at a call site until it was moved here, which is how the
// four spellings the header block describes came about in the first place.
//
// The Fox translation of D4's list, per §0's "rename per engine, and where the
// engine genuinely lacks a concept OMIT the action — never invent a
// placeholder for it":
//
//   Copy SNO             -> kCopyHash. Fox's identity is a PathFileNameCode.
//   Copy collection name -> ABSENT. Fox has no collections, and an action that
//                           could never do anything is not parity, it is
//                           clutter (§5's reasoning for the Textures tab).
//   Copy part material   -> kCopyMaterial, which already existed and is named
//                           after what it actually copies.
inline const QString kCopyHash      = QStringLiteral("Copy hash");
// Payload-specific copies. Each names what it actually puts on the clipboard,
// which is why they are separate entries and not one "Copy": a strings table's
// row, its key and its translated text are three different things somebody
// wants, and "Copy" would mean whichever the author happened to pick.
inline const QString kCopyValue     = QStringLiteral("Copy value");
inline const QString kCopyText      = QStringLiteral("Copy text");
inline const QString kCopyKey       = QStringLiteral("Copy key");
inline const QString kCopyRow       = QStringLiteral("Copy row");
// The export pair, base labels only: NO ellipsis, because suffixes get
// appended ("(12,345 tris)") and prompts() adds the ellipsis last.
inline const QString kExportModel     = QStringLiteral("Export model");
inline const QString kExportModelLast = QStringLiteral("Export model to last folder");
inline const QString kExportPart      = QStringLiteral("Export part");
inline const QString kExportPartLast  = QStringLiteral("Export part to last folder");
inline const QString kCopyImage     = QStringLiteral("Copy image");
inline const QString kSaveImage     = QStringLiteral("Save image…");        // PROMPTS
inline const QString kSaveImageLast = QStringLiteral("Save image to last folder");
inline const QString kShowDeps      = QStringLiteral("Show dependencies…");
inline const QString kShowAll       = QStringLiteral("Show all");
inline const QString kHideAll       = QStringLiteral("Hide all");
inline const QString kInvert        = QStringLiteral("Invert");
inline const QString kFramePart     = QStringLiteral("Frame part");
inline const QString kSelectPart    = QStringLiteral("Select part");
inline const QString kHidePart      = QStringLiteral("Hide part");
inline const QString kShowPart      = QStringLiteral("Show part");
inline const QString kIsolatePart   = QStringLiteral("Isolate part");

// "C:/Users/me/Documents/FOX/exports" → "…/FOX/exports". A full path makes a
// menu unreadable; the last two components are what actually distinguishes one
// export folder from another.
inline QString condensePath(const QString& path)
{
    if (path.isEmpty()) return {};
    const QString clean = QDir::fromNativeSeparators(QDir::cleanPath(path));
    const QStringList parts = clean.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() <= 2) return QDir::toNativeSeparators(clean);
    return QStringLiteral("…/%1/%2").arg(parts[parts.size() - 2], parts.last());
}

inline QString withValue(const QString& label, const QString& value)
{
    return value.isEmpty() ? label : QStringLiteral("%1 (%2)").arg(label, value);
}

// A value preview, cut at 30 characters with a real ellipsis character. An
// asset path is longer than a menu should be, and a menu as wide as the screen
// is the §5 off-screen-panel bug wearing different clothes.
inline QString preview(const QString& s)
{
    return s.size() > 30 ? s.left(29) + QStringLiteral("…") : s;
}

// The COPY form of withValue: the value slot is always drawn, and an empty one
// renders as "—" with the caller disabling the action. §6 — disable, don't
// hide, when the action applies to this kind of object but has no value right
// now. withValue() above drops the slot entirely, which is right for a folder
// name and wrong for a clipboard value: "Copy name" with nothing after it reads
// as an action that works.
inline QString withCopyValue(const QString& label, const QString& value)
{
    return QStringLiteral("%1  (%2)")
        .arg(label, value.isEmpty() ? QStringLiteral("—") : preview(value));
}

// The multi-selection form: "Copy hash  —  12 rows". The clipboard gets one
// value per line, so the label says how many lines are coming.
inline QString withRows(const QString& label, int rows)
{
    return QStringLiteral("%1  —  %2 rows").arg(label).arg(rows);
}

inline QString withCount(const QString& label, int tris)
{
    return tris > 0 ? QStringLiteral("%1 (%2 tris)")
                          .arg(label, QLocale().toString(tris))
                    : label;
}

// The ONLY way an ellipsis gets onto a label. Applied last, to the finished
// string, so a suffix can never strand one mid-label.
inline QString prompts(const QString& label) { return label + QStringLiteral("…"); }

// The plural of a noun this application actually uses. Not a general
// pluraliser — a general one would be wrong for exactly the words that matter.
inline QString plural(const QString& noun, int n)
{
    if (n == 1) return noun;
    if (noun.endsWith(QLatin1Char('h')) || noun.endsWith(QLatin1Char('s')))
        return noun + QStringLiteral("es");
    return noun + QStringLiteral("s");
}

// THE export label. `verb` is "Export" or "Extract"; `noun` is what the things
// are ("model", "texture", "file", "submesh", "material"); `oneName` is the
// name to use when there is exactly one. No ellipsis — wrap in prompts() when
// the action opens a dialog, which is what the caller knows and this does not.
inline QString exportLabel(const QString& verb, int count,
                           const QString& oneName, const QString& noun)
{
    if (count <= 0) return verb;
    if (count == 1 && !oneName.isEmpty())
        return QStringLiteral("%1 %2").arg(verb, oneName);
    return QStringLiteral("%1 %2 %3")
        .arg(verb, QLocale().toString(count), plural(noun, count));
}

// The same, for the silent path that writes to the remembered folder. Says
// where, because an action that writes without asking has to.
inline QString exportLastLabel(const QString& verb, int count,
                               const QString& oneName, const QString& noun,
                               const QString& dir)
{
    return withValue(
        QStringLiteral("%1 to last folder")
            .arg(exportLabel(verb, count, oneName, noun)),
        condensePath(dir));
}

// ── THE SUBJECT OF AN EXPORT ─────────────────────────────────────────────
// "Export .glb (rigged, bind pose)…" tells you the FILE FORMAT and the pose
// and never says what is being exported. The user's complaint was exactly
// that: the Export menu should say "Export character", "Export weapon",
// "Export model", "Export part", "Export scene" — the noun for the thing in
// front of them, which is the only part of that label they did not already
// know.
//
// So the tab supplies the noun (see BrowserTab::exportSubjectNoun) and this
// builds the label. The format stays, in parentheses, because .glb-vs-.fbx is
// still worth knowing — it is just not the headline.
inline QString exportSubject(const QString& subject, const QString& detail)
{
    return detail.isEmpty()
               ? QStringLiteral("Export %1").arg(subject)
               : QStringLiteral("Export %1 (%2)").arg(subject, detail);
}

}  // namespace MenuText
