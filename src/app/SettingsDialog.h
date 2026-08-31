// SettingsDialog.h — the settings dialog, in tabs (template §10).
//
// It was one flat column: game folders, a deep-scan box, three PBR switches
// and the viewport defaults, stacked, with nowhere to put anything else. The
// template's §10 shape is tabs, in a stated order — *setup → presentation →
// the per-area pages → what lands on disk → keys → upkeep → reference* — and
// that ordering is the reason the export options, the hotkey editor and the
// cache controls now have obvious homes instead of no home at all.
//
// The tabs Fox has:
//
//   General      the folders and what is read from them
//   Interface    how the application behaves before you have opened anything
//   Viewport     what a viewport comes up as
//   Export       sub-tabs: Models · Images & GIFs · File names
//   Hotkeys      the app/Hotkeys.h registry, editable
//   Maintenance  caches, with their sizes, and reset
//   Information  what the confusable options actually mean
//
// **Models** and **Wardrobe** are deliberately absent. In D4 those hold
// per-tab browsing and performance options; Fox's equivalents are the three
// PBR switches and the viewport defaults, which are one subject and live on
// Viewport. Two tabs holding one checkbox each would be the template's shape
// without its substance. **Experimental** is absent because nothing is.
#pragma once
#include <QDialog>
#include <QVector>

#include <memory>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QListWidget;
class QTabWidget;

namespace fox {
class ExportPages;
}

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);
    ~SettingsDialog() override;
    void accept() override;

    // Open on a named tab ("Export", "Hotkeys", …). Used by the viewport's
    // "Export settings…" entry, which asks for a specific page rather than
    // for the dialog in general. An unknown name opens on the first tab.
    void showTab(const QString& name);

protected:
    // The tab bar must never elide a label (§10). Its full width is folded
    // into the dialog's requested size here, because a QTabWidget does not do
    // that itself — which is how "General" once rendered as "eneral" in D4.
    void showEvent(QShowEvent* e) override;

private:
    // Every tab is a scroll area over its page, so no page can ever clip.
    void addPage(const QString& title, QWidget* page);
    // Two actions on one sequence make Qt fire neither. Say so while it is
    // being typed rather than letting it be found by pressing the key.
    void checkHotkeyClashes();

    QTabWidget* m_tabs = nullptr;

    QListWidget* m_dirs = nullptr;
    QLineEdit* m_dictDir = nullptr;
    QLineEdit* m_modDir = nullptr;
    QCheckBox* m_deepScan = nullptr;
    // One box per viewport — Config::PbrView has three members and so does
    // this, so a viewport can never end up without a switch of its own.
    QCheckBox* m_pbrFiles = nullptr;
    QCheckBox* m_pbrModels = nullptr;
    QCheckBox* m_pbrCustomize = nullptr;
    // What a NEW viewport comes up as. Not live — see Config::viewEnvironment.
    QComboBox* m_viewEnv = nullptr;
    QDoubleSpinBox* m_viewExposure = nullptr;
    QCheckBox* m_viewPanel = nullptr;
    // Interface
    QCheckBox* m_rememberPanels = nullptr;
    QCheckBox* m_rememberViewport = nullptr;
    // Hotkeys: one editor per row of app/Hotkeys.h, in that order.
    QVector<QPair<QString, QKeySequenceEdit*>> m_hotkeys;
    QLabel* m_hotkeyWarning = nullptr;
    // Export
    std::shared_ptr<fox::ExportPages> m_export;
};
