// TextureInfoPanel.h — the Textures tab's right-hand column (template §7).
//
// Three PANELS in the tab's N-panel column, in the order the questions get
// asked. They were three headings stacked inside one pane; §6 asks for panels
// — each with its own header, ▲▼✕, size and switch on the icon strip.
//
// This class is the controller: it owns the data (the header parse, the mip
// walk, the users sweep) and hands out three ready-made widgets for the column
// to place. It is never shown itself.
//
//   FILE INFO          what this file IS — name, size, format, dimensions,
//                      mips, how many .ftexs stream files it needs, its tags
//   MIP LEVELS         the Fox answer to §7's atlas/sprite frames. A .ftex has
//                      no atlas; what it HAS is a mip chain split across
//                      `<name>.N.ftexs` files, and which mip lives in which
//                      file is the thing that explains a texture that decodes
//                      blurry on a partially-downloaded install. Each row can
//                      be exported on its own.
//   ASSOCIATED MODELS  what this texture is ON — model → material → role, from
//                      index/TextureUsers.h. Double-click a model to open it.
//
// The panel owns no decoding: the tab hands it a file index and the already
// decoded image, so selecting a texture costs one parse of a 64-byte header.
#pragma once
#include <QWidget>

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

namespace fox {

class TextureInfoPanel : public QWidget {
    Q_OBJECT
public:
    explicit TextureInfoPanel(QWidget* parent = nullptr);

    // The three widgets the column places. Parented here so lifetime is one
    // object's problem; NPanel reparents them as it adds them.
    QWidget* fileInfoSection() const { return m_infoSection; }
    QWidget* mipsSection() const { return m_mipsSection; }
    QWidget* usersSection() const { return m_usersSection; }

    // Show one indexed .ftex. -1 clears.
    void showTexture(int fileIdx);
    int currentFile() const { return m_fileIdx; }

Q_SIGNALS:
    // A model in ASSOCIATED MODELS was activated. The tab forwards it to the
    // Models tab — §7's "jumps to the Browse tab".
    void modelActivated(quint64 modelHash, const QString& modelPath);
    // A mip row's export was asked for.
    void exportMipRequested(int fileIdx, int mipIndex);
    // The two section titles carry live counts — "MIP LEVELS (10 — 2 not
    // mounted)". They were bold labels inside the column; the count belongs in
    // the PANEL HEADER now, which costs no row inside the panel. The tab wires
    // these into NPanel::titleLabel().
    void mipsTitleChanged(const QString& title);
    void usersTitleChanged(const QString& title);

private:
    void refreshUsers();
    void setUsersMessage(const QString& text);

    int m_fileIdx = -1;
    QWidget* m_infoSection = nullptr;
    QWidget* m_mipsSection = nullptr;
    QWidget* m_usersSection = nullptr;
    QLabel* m_info = nullptr;
    QTreeWidget* m_mips = nullptr;
    QTreeWidget* m_users = nullptr;
    QLabel* m_usersNote = nullptr;
};

}  // namespace fox
