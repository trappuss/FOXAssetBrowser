// AnimBind.h — WHICH ANIMATIONS BELONG TO A MODEL, answered from the data.
//
// The Animations panel used to be able to show one thing: every clip in the
// install. That is the right answer to "what walk cycles exist" and the wrong
// answer to "what can I play on THIS", which is the question anyone with a
// model already loaded is actually asking.
//
// The association is NOT a guess from the filename, the folder or the icon. A
// gani animates RIG UNITS — track i drives unit i — and a model's .frig is the
// only thing that says which bone each unit moves. So an archive belongs to a
// model exactly when the model's own rig, given that archive's track count,
// resolves a drive for most of the bones it can drive at all. Everything here
// is one call on top of AnimCatalog's animBindScore; it exists so the panel,
// the tabs and the --animbind probe cannot disagree about what "this model's
// animations" means.
//
// Measured on the shipped data (see AnimCatalog::animBindThreshold): scores
// are bimodal — 72 of 109 archives at exactly 1.000 against any humanoid, the
// other 37 at 0.358 or below, nothing in between.
#pragma once
#include <QSet>
#include <QString>
#include <QVector>

namespace animbind {

struct Binding {
    QString modelPath;     // empty when nothing is loaded
    QString label;         // the model's stem, for the UI to name the scope
    int bones = 0;         // distinct bone names in the model
    int ceiling = 0;       // most of them any clip could drive, via the rig
    QString rigVia;        // the .parts that named the rig, or "heuristic"
    bool haveRig = false;
    QSet<int> archives;    // ArchiveIndex fileIdx of the archives that bind
    int clips = 0;         // clips inside those archives
    bool valid() const { return !modelPath.isEmpty(); }
};

// Resolve the binding for one model, by its path in the archive index. The
// last few answers are cached against the index generation, because the panel
// asks again on every model change and the sweep parses a .parts map.
Binding forModel(const QString& modelPath);

// Drop the cache (a rescan). Safe to call when nothing is cached.
void clearCache();

}  // namespace animbind
