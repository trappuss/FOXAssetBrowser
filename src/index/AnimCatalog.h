// AnimCatalog.h — every animation in the install, grouped and made readable.
//
// The two animation combos used to be flat lists: one of all 164 .mtar files by
// bare filename, one of whatever clip names the selected archive happened to
// hold. With 2,855 clips across the shipped data that is a list you scroll, not
// a list you use.
//
// Three things are added here, and it is worth being precise about which of
// them the data states and which are ours:
//
//   THE ARCHIVE'S SHELF — stated by the data. Every motion archive lives at
//     /Assets/<game>/motion/mtar/<group>/<name>.mtar
//   so the game and the group ("player2/Receiver", "buddydog2", "walkergear2")
//   come straight off the path. Measured: 159 archives parse, 0 fail, in 16
//   groups across tpp / mgo / ssd.
//
//   THE CLIP'S CATEGORY — OURS, derived from the clip name's tokens. Fox names
//   clips as underscore-separated tokens ("snapnon_q_rn_st_r90_r",
//   "wolf_s_atk_std_ed") and the action token is drawn from a long tail — 235
//   distinct ones over 2,855 clips, where the top 60 cover 81%. So the mapping
//   is a table of well-attested tokens matched ANYWHERE in the name, tried in
//   priority order, and everything else lands in "Other" rather than being
//   forced somewhere. Measured coverage over the shipped data:
//
//     Weapon 27.3%   Locomotion 24.4%   Idle 8.7%   Damage 6.9%
//     Interaction 4.6%   Ready move 4.6%   CQC 3.2%   Facial 2.1%
//     Other 18.3%
//
//   An 18% "Other" is reported rather than tuned away: it is genuinely
//   miscellaneous (walker-gear patrol turns, charge cycles, demo poses), and a
//   category table stretched until nothing was left over would be describing
//   the table rather than the data.
//
//   THE READABLE LABEL — also OURS. There is NO in-game translation for clip
//   names: the .lng2 catalogues carry UI and story text and not one motion
//   name, which was checked before any of this was written. So the label is an
//   expansion of the abbreviations, and it expands ONLY tokens that are well
//   attested in the corpus. Anything else is passed through verbatim, and the
//   raw name is always shown on the row beneath, so the expansion can never
//   hide what the asset is actually called.
//
// The single-letter token at position 1 — s (1,432 clips), q (627), c (216),
// p (62) — is left as a bare tag on purpose. It behaves like a stance: 'c' and
// 'p' have essentially no locomotion among their top actions while 's' and 'q'
// both walk and run, which is what crouch and prone would look like. That is
// suggestive and not proof, and one wrong character in a label is worse than
// one unexpanded one.
#pragma once
#include <QHash>
#include <QString>
#include <QVector>

namespace fox {

class FmdlFile;
class FrigFile;

enum class AnimCategory {
    Weapon,
    ReadyMove,
    Locomotion,
    Damage,
    Cqc,
    Idle,
    Facial,
    Interaction,
    Other,
    Count
};

// Display name for a category ("Ready move", "CQC"). Stable, and used as the
// group caption in the clip combo.
QString animCategoryName(AnimCategory c);

// Which category a clip name falls in. `archiveHint` is the archive's stem and
// group, which is what identifies the facial archives — those are stated by the
// file they live in rather than by any token in the clip name.
AnimCategory animCategoryFor(const QString& ganiName, const QString& archiveHint);

// Our readable expansion of a clip name. "snapnon_q_rn_st_r90_r" becomes
// "Run start · right 90° · right". Tokens with no entry pass through unchanged.
QString animLabelFor(const QString& ganiName);

struct AnimClip {
    int index = 0;             // clip index within its archive
    QString name;              // raw .gani stem, exactly as the archive names it
    QString label;             // our expansion — see animLabelFor
    AnimCategory category = AnimCategory::Other;
};

struct AnimArchive {
    int fileIdx = -1;          // into ArchiveIndex::files()
    QString path;
    QString stem;              // "mgo_pl_rcvr_ar02"
    QString game;              // "tpp" / "mgo" / "ssd" — from the path
    QString group;             // "player2/Receiver" — the shelf, from the path
    bool v2 = false;
    QVector<AnimClip> clips;
    // Per-category clip counts, so a combo can caption a group without
    // walking the clip list again.
    int perCategory[int(AnimCategory::Count)] = {};

    // ── WHICH SKELETON THIS ARCHIVE IS FOR — stated by the file ─────────
    // The bone names of the shared track layout, as StrCode32, sorted and
    // deduped. These are the SAME hashes animpose matches against a model's
    // bones, so a clip set drives a model exactly to the extent that these
    // two sets agree; this is not a guess from the filename or the shelf.
    // A v2 archive states them once for all its clips (tracks are built one
    // per layout unit); a v1/GZ archive carries a layout per clip, so clip 0
    // stands for the archive and `unitsFromClip0` says so.
    QVector<quint32> unitHashes;
    int unitCount = 0;
    bool unitsFromClip0 = false;
};

// How much of a model an archive can actually pose, 0..1 — which is what
// "animations for this model" has to mean if it is to be more than a guess
// from the filename.
//
// A gani does NOT animate bones by name: it animates RIG UNITS, track i
// driving unit i, and the model's .frig is the only thing that says which
// bone each unit moves. (Measured: over the shipped data the intersection of
// mtar layout-unit name hashes with FMDL bone name hashes is exactly ZERO for
// every archive against every model — the two are different namespaces, and a
// name-overlap score would have reported "no animations for anything".) So
// the score is the frig's own resolution, run against the archive's track
// count: the fraction of this model's bones the archive has tracks to drive.
// Without a frig it falls back to direct name matching, which is exactly what
// animpose::buildPalette falls back to, so the score and the poser never
// disagree about what will move.
//
// Normalised against what this model's rig could drive AT BEST, not against
// its bone count: a humanoid's rig drives 53 of its 114 bones (the rest are
// cloth and help bones no clip touches), so dividing by 114 capped every
// character archive at 0.465 and made the threshold a per-model number. Over
// the best achievable, an archive authored for the model lands at 1.000 and
// the threshold means the same thing for every model.
//
// `driven` (optional) receives the raw bone count.
// `ceiling` is animBindCeiling() for this model; pass it when scoring many
// archives against one model, which is every real caller — recomputing it per
// archive doubles the work for an answer that cannot change. -1 computes it.
float animBindScore(const AnimArchive& a, const QVector<quint32>& modelBoneHashes,
                    const FrigFile* frig, int* driven = nullptr,
                    int ceiling = -1);

// The most bones this model's rig can drive, given a clip with every track.
// The denominator of the score, exposed because it is also the honest answer
// to "why does nothing bind to this model" — zero here means no motion
// archive can pose it at all, which is true of a one-bone prop.
int animBindCeiling(const QVector<quint32>& modelBoneHashes,
                    const FrigFile* frig);

// The score at or above which an archive is offered as "this model's". Chosen
// from measurement, not taste — see AnimCatalog.cpp.
float animBindThreshold();

// DIAGNOSTIC: how many of this archive's rig-unit names the model's .frig also
// names, or -1 when there is no rig or the archive's layout was not parsed.
//
// Nothing filters on it. It exists because animBindScore's rigged path never
// looks at WHICH bones an archive animates — only how many tracks it has — so
// any archive with enough tracks binds, which is why a Walker Gear reports
// 23,713 of 24,353 clips. This measures the thing that should decide it, so
// one --animbind run on a real install can settle it. See the long note above
// the definition.
int animBindNameOverlap(const AnimArchive& a, const FrigFile* frig);

// A model's bone StrCode32 set, sorted and deduped — the other half of the
// score. Here rather than at each caller so the two sides are built the same
// way and the merge in animBindScore can rely on the order.
QVector<quint32> modelBoneHashes(const FmdlFile& model);

// Lazily built and cached against the archive index's identity, exactly like
// the other catalogues here: the first access after a rescan rebuilds, and
// every access after that is a reference to the same data.
class AnimCatalog {
public:
    static const AnimCatalog& instance();

    const QVector<AnimArchive>& archives() const { return m_archives; }
    // Archive indices in display order: by game, then group, then stem.
    const QVector<int>& order() const { return m_order; }
    int clipCount() const { return m_clipCount; }
    int failedCount() const { return m_failed; }
    QString note() const { return m_note; }

private:
    void build();
    QVector<AnimArchive> m_archives;
    QVector<int> m_order;
    int m_clipCount = 0;
    int m_failed = 0;
    QString m_note;
    const void* m_indexKey = nullptr;
    int m_indexCount = -1;
    int m_filterGeneration = -1;
};

}  // namespace fox
