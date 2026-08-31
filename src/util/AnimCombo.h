// AnimCombo.h — the two animation organizers, in one place.
//
// Models and Customize both carry an archive combo and a clip combo, and they
// have to behave identically: the same grouping, the same captions, the same
// payload contract. Two copies of that had already drifted once (Models grew a
// clip count in its rows and Customize did not), so the population lives here
// and both tabs call it.
//
// THE PAYLOAD CONTRACT, which matters more than it looks: both combos carry
// group captions, and a caption is a row. So a combo's row index is NOT an
// archive index and NOT a clip index — every read goes through
// currentPayload() / selectPayload(). Anything that treats the two as the same
// number loads the wrong clip the moment a group appears.
#pragma once
#include <QSet>
#include <QString>

class SearchableCombo;

namespace fox {
class MtarFile;
}

namespace animcombo {

// Every motion archive in the install, grouped by game and by the shelf its
// path states ("TPP · buddydog2"), with the clip count and the v1/GZ marker on
// the second line. Row payload = the archive's index into
// ArchiveIndex::files(); the leading "No animation" row carries -1.
//
// The combo is cleared first and its signals are the caller's business.
// `onlyFiles`, when given, restricts the list to archives whose fileIdx is in
// it — the ANIMATIONS panel's resolved scope. THE COMBO AND THE PANEL MUST
// AGREE: the combo listed all 159 archives while the panel showed the 71 that
// can actually pose the loaded model, so the two controls for one thing
// answered "what can I play" differently, and the one under the viewport was
// the one that was wrong. Pass nullptr for "everything", which is what the
// panel's "All animations" scope means.
//
// `scopeNote` captions the narrowed list so a short combo is explained rather
// than mysterious.
void fillArchives(SearchableCombo* combo,
                  const QSet<int>* onlyFiles = nullptr,
                  const QString& scopeNote = QString());

// One archive's clips, grouped by category, with our readable label as the
// headline and the asset's own name beneath it. Row payload = the CLIP INDEX
// within the archive.
//
// `fileIdx` lets the catalogue's precomputed categories be reused when it knows
// this archive; pass -1 (or an archive the catalogue never saw, which is what a
// loose folder produces) and the categories are computed on the spot instead.
void fillClips(SearchableCombo* combo, const fox::MtarFile& mtar, int fileIdx);

// Tooltip text for each, so the two tabs describe them the same way.
QString archiveTooltip();
QString clipTooltip();

}  // namespace animcombo
