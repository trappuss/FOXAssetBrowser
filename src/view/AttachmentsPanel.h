// AttachmentsPanel.h — the ATTACHMENTS panel (template §6, "engine extras").
//
// WHAT A FOX ATTACHMENT ACTUALLY IS. Fox keeps a model's variations in .fv2
// FOVA tables beside it, and a table says some combination of three things:
// substitute these textures, hide/show these mesh groups, and bring this extra
// .fmdl along on this connect point. That third one is the hat. The Customize
// tab already applies whole tables when the user builds a character; this
// panel is the same information for the model you are LOOKING at, which is
// what the tab was missing — a character that ships with three hats had no
// way, in the Models tab, to say so.
//
// HOW A TABLE IS ASSOCIATED WITH A MODEL, and why it is not by filename. The
// project's standing rule is that assets are never identified by rendering and
// comparing icons; the filename equivalent is nearly as bad, and it is
// measurably wrong here — of the 339 "cm_"-prefixed tables in a full install,
// only 18 key to a model by name, and the eye-cover tables abbreviate on top
// of that ("cm_f0_h0_v000_eye0" against the model "cm_f0_head0_v000_cov").
//
// So the association is by HASH, out of the files themselves. A model's mesh
// groups and material instances carry StrCode32 name hashes; a .fv2's hide,
// show and substitution rows address exactly those hashes. A table BELONGS to
// this model when at least one of its rows names something the model actually
// declares. That is a fact read out of two files, and it is checkable.
//
// The candidate set is bounded to the model's own archive neighbourhood — the
// same directory, or the fova/ folder beside it — because a full install
// carries thousands of tables and parsing all of them on every model load is
// not a panel, it is a background job. Tables that live further away are
// reachable through the Customize tab, which is where a whole appearance is
// assembled.
//
// WHAT THE CHECKBOXES DO, exactly. A mesh-group row toggles that group in the
// viewport: this is the model's own geometry and it works completely. A row
// for an EXTERNAL attached model names the file and offers to open it, and
// says so — loading a second skeleton into this tab's single-model pipeline is
// a retarget, which is the Customize tab's job and is not quietly faked here.
#pragma once
#include <QHash>
#include <QString>
#include <QVector>
#include <QWidget>

#include "fox/FovaFile.h"

class QTreeWidget;
class QTreeWidgetItem;

namespace fox {

class FmdlFile;

class AttachmentsPanel : public QWidget {
    Q_OBJECT
public:
    explicit AttachmentsPanel(QWidget* parent = nullptr);

    // Rebuild for the model at `fileIdx` (an ArchiveIndex file index). Reads
    // the candidate tables synchronously — the set is bounded to a directory,
    // and the count is reported in the title line so an unexpectedly large one
    // is visible rather than merely slow.
    void setModel(int fileIdx, const FmdlFile* model);
    void clearModel();

    // "3 tables · 2 hats" for the panel header, or an empty string.
    QString summary() const { return m_summary; }
    // Dev harness: how many rows of each kind the last rebuild produced.
    int tableCount() const { return m_tables; }
    int attachmentCount() const { return m_attachments; }
    int groupRowCount() const { return m_groupRows; }

Q_SIGNALS:
    // A mesh group was ticked or unticked. `nameHash32` is the group's
    // StrCode32; the tab maps it to the group index the viewport knows.
    void meshGroupToggled(quint32 nameHash32, bool on);
    // "Open this attached model" — the file index of an .fmdl in the index.
    void openModelRequested(int fileIdx);
    void summaryChanged();

private:
    struct Candidate {
        int fileIdx = -1;
        QString path;
        FovaFile table;
        int matchedRows = 0;   // how many rows named something this model has
    };

    void rebuild(const FmdlFile* model);
    QVector<Candidate> findTables(int fileIdx, const FmdlFile* model) const;
    // The .fmdl in the index whose PathCode64 is `hash`, or -1.
    int modelFileFor(quint64 hash) const;

    QTreeWidget* m_tree = nullptr;
    QString m_summary;
    int m_tables = 0;
    int m_attachments = 0;
    int m_groupRows = 0;
};

}  // namespace fox
