// CharacterCatalog.h — the Customize builder's Character category, discovered
// from the archives in the same way the weapon side is.
//
// Characters have no "chimera" directory to read slots from, so the slots come
// from the naming convention the models themselves use. Counting the part-kind
// token across every character model in the dictionary gives a clear
// vocabulary: `main`/`body` (767+12), `eqhd`/`ephd` head equipment (110+20),
// `face` (62), `eqit`/`eqbd` item equipment (32+12), `arm` (28), `hair`,
// `hood`, `coat`, `hand`. Those are the slots, and only the ones a given
// character actually has are shown.
//
// A subject here is a character id ("sna0", "dds5", "avm0"); its variants are
// that character's body models. Appearance variations are the same .fv2 FOVA
// tables the weapons use.
#pragma once
#include <QHash>
#include <QString>
#include <QVector>

#include "index/PartCatalog.h"

namespace fox {

class CharacterCatalog {
public:
    static const CharacterCatalog& instance();

    bool isEmpty() const { return m_subjects.isEmpty(); }
    BuilderSource builderSource() const;

    // "sna2_main0_def" → "sna2". Empty when the name does not fit the scheme.
    static QString characterOf(const QString& stem);
    // The part-kind token of a model stem: "sna0_arm6_cov" → "arm".
    static QString kindOf(const QString& stem);
    // Which slot that kind belongs to, or empty when it is not a body part.
    static QString slotForKind(const QString& kind);

private:
    void build();

    const void* m_indexKey = nullptr;
    int m_indexCount = -1;
    // The game toggles narrow this catalogue too, so a change to them has to
    // rebuild it exactly the way a rescan does.
    int m_filterGeneration = -1;
    QStringList m_slotNames;
    QHash<QString, QVector<CatalogPart>> m_bySlot;
    QHash<QString, QVector<CatalogVariation>> m_variations;
    // FOVA tables are authored against a slightly different stem from the
    // model's: the player camouflage for dds5_main0_def lives in
    // dds5_main0_ply_v03.fv2, and the bionic arm's in sna0_arm0_v03.fv2. Both
    // sides are normalised by dropping the trailing _def/_ply/_cov/_sta before
    // they are matched.
    static QString fovaKey(const QString& stem);
    QVector<CatalogSubject> m_subjects;
};

}  // namespace fox
