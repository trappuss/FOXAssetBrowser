// FileInfoPanel.h — the Files tab's right-hand column (template §6).
//
// §6 asked for every tab to be on the NPanel column. Files was the last one
// that was not, and it is a different SHAPE of job, which is why it was left
// until last: its right-hand side is preview/PreviewPane — the main view, not a
// side panel. So §6 here does not mean "move the preview into a panel". The
// preview stays the main view, and the INFORMATION AROUND IT becomes the
// column.
//
// The pattern is TextureInfoPanel's, from 8s: this class is the CONTROLLER —
// it owns the reads and the parses — and hands out ready-made widgets for the
// column to place. It is never shown itself, and it has no section headings of
// its own, because PanelBox already draws the title and a bold label inside a
// panel whose header says the same thing is the title twice.
//
//   FILE INFO   what this file IS — name, path, extension, size, hash, which
//               archive it came from and whether that copy is the one the game
//               would actually load.
//   ASSOCIATED  what this file is connected to, in whichever direction the
//               file has one: a model's textures come from its own material
//               table, a texture's models from index/TextureUsers. Rows open
//               the asset they name.
//
// There is deliberately no STRINGS panel. A .lng2 previews AS the strings
// browser — that is the main view for that file, not information around it —
// and a second copy of its table combo in the column would be two live
// spellings of one control, which is the defect this project keeps finding.
#pragma once
#include <QWidget>

class QLabel;
class QTreeWidget;

namespace fox {

class FileInfoPanel : public QWidget {
    Q_OBJECT
public:
    explicit FileInfoPanel(QWidget* parent = nullptr);

    // The widgets the column places. Parented here so lifetime is one object's
    // problem; NPanel reparents them as it adds them.
    QWidget* fileInfoSection() const { return m_infoSection; }
    QWidget* associatedSection() const { return m_assocSection; }

    // Show one indexed file. -1 clears.
    void showFile(int fileIdx);
    int currentFile() const { return m_fileIdx; }

Q_SIGNALS:
    // A row in ASSOCIATED was activated — the tab navigates to it.
    void assetActivated(int fileIdx);
    // The panel header's live count ("ASSOCIATED (7)"). It belongs in the
    // header rather than on a row inside the panel, which costs a row and
    // repeats the title.
    void associatedTitleChanged(const QString& title);

private:
    void rebuildInfo();
    void rebuildAssociated();

    int m_fileIdx = -1;
    QWidget* m_infoSection = nullptr;
    QWidget* m_assocSection = nullptr;
    QLabel* m_info = nullptr;
    QTreeWidget* m_assoc = nullptr;
    QLabel* m_assocNote = nullptr;
};

}  // namespace fox
