// NameCatalog.h — real in-game display names for assets.
//
// The archives hold two halves of this and nothing joins them, so the browser
// showed asset stems. The join is:
//
//   WeaponPartsUiSetting.lua   partsID → { messageID, ftexPath }
//                              …and ftexPath embeds the model stem:
//                              ".../ui_wpp_hg07_main4_def_alp" → hg07_main4_def
//   *.lng2 (LangFile)          StrCode32(messageID) → the localised text
//
// so `hg07_main4_def` → messageID "prts_rc_1005" → **"S.P CB-FRAME"**.
// Verified end to end: all 648 records in WeaponPartsUiSetting resolve to a
// string, and the same pipeline resolves 524 of 526 emblem labels and 141 of
// 155 announce labels from their own tables.
//
// Both inputs live in archives a normal install has (00.dat and a_chunk7.dat
// under master/), so this needs no extra configuration — but it degrades
// quietly: with either half missing, nameFor() returns empty and callers fall
// back to the asset stem.
#pragma once
#include <QHash>
#include <QMultiHash>
#include <QString>
#include <QStringList>

namespace fox {

class NameCatalog {
public:
    static const NameCatalog& instance();

    // The in-game name for a model stem ("hg07_main4_def"), or empty.
    QString nameFor(const QString& modelStem) const;
    // Any localised string by its label ("prts_rc_1005").
    QString textForLabel(const QString& label) const;

    // The equip-parameter tables identify parts by id ("RC_10001"), the
    // archives by model stem; WeaponPartsUiSetting carries both, so this is
    // the bridge EquipCatalog needs. One stem can back several ids (the same
    // physical part at different weapon grades), hence the list.
    QString stemForPartId(const QString& partId) const;
    QStringList partIdsForStem(const QString& modelStem) const;

    // The UI icon the GAME draws for this part, as an extensionless asset path
    // ("/Assets/tpp/ui/texture/WeaponPartsIcon/receiver/ui_wpp_ar02_main0_def_alp").
    // Same source as the name: WeaponPartsUiSetting gives both. Empty when the
    // part has no icon of its own — 69 of the 160 magazine rows do not.
    QString iconPathFor(const QString& modelStem) const;
    int iconCount() const { return m_stemToIcon.size(); }

    int stringCount() const { return m_strings.size(); }
    int mappedAssets() const { return m_stemToLabel.size(); }
    // How many .lng2 language tables were read, and how many strings came out
    // of them. Reported separately from the weapon-parts table because the two
    // fail INDEPENDENTLY: an install can carry every language table and no
    // WeaponPartsUiSetting.lua, and the old single "no names available" line
    // claimed both were missing whenever the second one was.
    int langTables() const { return m_langTables; }
    // Every .lng2 in the index, whatever its language and whether or not it
    // parsed — so "none found" and "found, none usable" stay distinguishable.
    int langFilesSeen() const { return m_langFilesSeen; }
    // Every message label this catalogue knows, deduplicated. StrCode32 is not
    // invertible, so a .lng2 key can only be shown with its label when the
    // label came from somewhere else — and this is the somewhere else.
    QStringList knownLabels() const;
    // "12,438 strings, 648 named assets" — for the status line and the log.
    QString describe() const;

private:
    void build();

    const void* m_indexKey = nullptr;
    int m_indexCount = -1;
    int m_langTables = 0;
    int m_langFilesSeen = 0;
    QHash<quint32, QString> m_strings;      // StrCode32(label) → text
    QHash<QString, QString> m_stemToLabel;  // model stem → messageID
    QHash<QString, QString> m_stemToIcon;        // model stem → icon asset path
    QHash<QString, QString> m_partIdToStem;      // "RC_10001" → "hg00_main0_def"
    QMultiHash<QString, QString> m_stemToPartIds;
};

}  // namespace fox
