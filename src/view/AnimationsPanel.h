// AnimationsPanel.h — the clips you can actually use, as a list you can work
// from.
//
// The animation UI was two combos: one of 159 archives, one of the clips in
// whichever archive was chosen. That is fine for "play this one thing" and no
// use at all for the two questions people actually arrive with — "what walk
// cycles are there" (which spans archives) and "give me these nine clips as a
// file" (which a combo cannot express, because a combo has one selection).
//
// So: a tree of archive → clip, one filter box that searches EVERY clip in the
// install at once, a category filter, and multi-select with an export that
// takes the whole selection.
//
// …and a SCOPE, which is the third question and the one the panel used to
// answer wrongly by default: "what can I play on the model I already have
// open". It shipped showing every clip in the install and nothing else, which
// is right for browsing and useless for that. The scope is resolved from the
// RIG (see anim/AnimBind.h) — a gani animates rig units and the model's .frig
// says which bone each unit moves — never from the file name, the folder or
// anything that would amount to guessing which asset this is. The combos stay; they are still the fastest way to
// step through one archive, and this panel drives the same loader they do.
//
// The panel OWNS no data. Rows carry (archive fileIdx, clip index) and the tab
// does the loading and the exporting — the same split the submesh tree and the
// material inspector already use, and the reason none of them can disagree with
// the viewport about what is loaded.
#pragma once
#include <QSet>
#include <QVector>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;

class AnimationsPanel : public QWidget {
    Q_OBJECT
public:
    explicit AnimationsPanel(QWidget* parent = nullptr);

    // Build (or rebuild) the tree from fox::AnimCatalog. Called when the panel
    // is first shown and after a rescan — never in the constructor, so a build
    // that never opens the panel never pays for 2,855 rows.
    void rebuild();
    // True once rebuild() has run against the CURRENT catalogue — and FALSE
    // again once the catalogue has changed under it.
    //
    // This used to be a bare `m_built` flag that, once set, was true for ever.
    // The panel is filled the moment it is restored open, which happens in the
    // tab's constructor, LONG before fox::AnimCatalog has read a single
    // archive — so it built 0 rows, called itself built, and never rebuilt.
    // The transport then played a clip the panel had no row for, which is the
    // second half of "the animation player doesn't match the panel".
    //
    // Comparing against the catalogue rather than trusting a call order means
    // no caller has to know when the catalogue arrives.
    bool isBuilt() const;

    // Follow the tab: select and scroll to this clip without emitting
    // clipChosen (which would be the panel answering its own signal).
    //
    // RETURNS whether the clip could actually be shown, and this is the whole
    // of a bug the user reported as "the animation player at the bottom
    // doesn't always match the ANIMATIONS panel". It used to be void and to
    // fail SILENTLY: with the default "This model's animations" scope, or with
    // a filter typed in the box, the playing clip is routinely not a row in
    // this tree at all — and the panel then left the PREVIOUS row highlighted,
    // which is a highlight that means "this is playing" pointing at a clip
    // that stopped playing three clicks ago.
    //
    // Now: shown when it can be, current-row cleared when it cannot, and a
    // note beside the count saying what is playing instead. Pass (-1, -1) for
    // "nothing is playing", which is the state every failure path in the
    // loader leaves the transport in.
    bool showCurrent(int archiveFileIdx, int clipIdx);

    // (archive fileIdx, clip index) for every selected clip row, in tree order.
    // Archive rows in the selection contribute ALL of their clips, so
    // "select the archive, press export" does the obvious thing.
    QVector<QPair<int, int>> selectedClips() const;

    // ── Scope: whose animations these are (§4) ──────────────────────────
    // The panel showed every clip in the install and nothing else, which is
    // the wrong default for someone who already has a model open. Three
    // scopes, as stable string ids: "model" (the clips the loaded model's own
    // rig can actually play), "all", and "other" (another model's, picked by
    // name). The binding is resolved by animbind, from the rig — never from
    // the filename and never from the folder.
    //
    // The tab tells the panel which model is loaded; the panel does the rest,
    // so the two viewport tabs cannot disagree about what the scope means.
    void setCurrentModel(const QString& modelPath);
    // Stable id in, false for an unknown one. "other" without a chosen model
    // is refused rather than silently behaving like "all".
    bool setScope(const QString& id);
    QString scope() const;
    // Scope to a NAMED model without opening the picker — what the headless
    // harness needs, and what the picker itself calls once it has an answer.
    // False when no model in the index matches.
    bool setScopeModel(const QString& modelNeedle);
    // One line describing the current scope, for a status line or a shot.
    QString scopeSummary() const;

    // ── The scope, for the ANIMATION BAR under the viewport ──────────────
    // The bar's archive combo listed all 159 archives while this panel showed
    // the 71 that can pose the loaded model. Two controls for one question,
    // disagreeing. These let the bar ask this panel what the answer is, so
    // there is one resolution and not two.
    //
    // scopeArchiveFiles() returns the fileIdx set in force, resolving it if
    // needed; scopeIsAll() is true when there is no restriction at all, which
    // is not the same as an EMPTY set (a prop that nothing can pose).
    bool scopeIsAll();
    QSet<int> scopeArchiveFiles();

signals:
    // The scope changed — anything mirroring it should re-read it.
    void scopeChanged();

public:

    // Harness/UI helper: type into the filter box (empty clears it).
    void setFilter(const QString& text);
    // The sort order, as a stable string id — "archive", "name", "asset" or
    // "category" (§3.1: never a combo index). False for an unknown one, which
    // fails to the default rather than silently reordering. This is the third
    // control §4 asks a list for and the panel did not have: with 2,855 clips
    // across 159 archives, "what walk cycles are there" is a question about
    // NAMES and the tree could only ever answer it archive by archive.
    bool setSortOrder(const QString& id);
    QString sortOrder() const;
    // Select every clip row the filter is currently showing; returns how many.
    // The panel's own "select all" and what a headless run needs are the same
    // operation, so there is one of it.
    int selectVisible();

    // Dev harness: the first `n` clip labels in tree order, joined. What a
    // sort actually produced, as opposed to which item the combo has selected.
    QString firstRowsForShot(int n) const;

    // What the buttons say they will do, for the tab's status line.
    QString selectionSummary() const;

signals:
    // A single clip was activated (clicked or double-clicked): load and play it.
    void clipChosen(int archiveFileIdx, int clipIdx);
    // Export the current selection. `separateFiles` = one .glb per clip.
    void exportRequested(bool separateFiles);

private:
    void applyFilter();
    void applySort();
    void refreshButtons();
    // One place writes the count label. applyFilter() computes the count half
    // and showCurrent() the "what is playing" half, and both call this — two
    // writers to one setText() is how one of them silently wins.
    void paintCount();

    QLineEdit* m_filter = nullptr;
    QComboBox* m_category = nullptr;
    QComboBox* m_sort = nullptr;
    QComboBox* m_scope = nullptr;
    QTreeWidget* m_tree = nullptr;
    QLabel* m_count = nullptr;
    QPushButton* m_exportOne = nullptr;
    QPushButton* m_exportEach = nullptr;
    bool m_built = false;
    // How many archives the catalogue held when rebuild() last ran. -1 = never.
    qsizetype m_builtArchives = -1;
    bool m_syncing = false;
    int m_totalClips = 0;
    // The resolved binding for the scope in force. Empty archive set with a
    // non-empty path means "this model can be posed by nothing", which is a
    // real answer and is shown as one.
    QString m_modelPath;      // what the tab has loaded
    QString m_scopeLabel;     // the model the scope names
    QSet<int> m_scopeArchives;
    int m_scopeClips = 0;
    int m_scopeCeiling = 0;
    // The binding's own account of why it found nothing, carried through so
    // the empty page can print it instead of a sentence written in advance
    // that assumes which of the causes it was.
    QString m_scopeWhy;
    bool m_scopeResolved = false;
    // Shown IN PLACE OF the tree when a scope leaves nothing: the sentence
    // needs to wrap, and a tree row in a docked column cannot.
    QLabel* m_empty = nullptr;
    QStackedWidget* m_pages = nullptr;

    // ── What the TRANSPORT is playing, as last pushed by the tab ─────────
    // Kept whether or not this panel can show it, because "can it be shown"
    // is the question the note answers.
    int m_playArchive = -1;
    int m_playClip = -1;
    bool m_playShown = false;
    // What was last LOGGED, so the line fires on any change of the triple
    // rather than only on a false→true transition. Logging the transition
    // alone hid the state that matters most: the very first sync, where
    // "cannot show" and "not yet asked" are both false and indistinguishable.
    int m_loggedArchive = -2;
    int m_loggedClip = -2;
    bool m_loggedShown = false;
    QString m_countBase;   // the count half of the label
    QString m_playNote;    // the "playing …" half, empty when they agree

    void resolveScope();
    void openModelPicker();
};
