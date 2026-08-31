// MechaCatalog.h — vehicles and buddy hardware: the UTH-66 helicopter, the
// D-Walker, and whatever else the /Assets/tpp/mecha tree carries.
//
// These assemble like weapons, not like characters: the parts are separate
// models seated on connect points, NOT skinned to one shared skeleton. That is
// not an assumption — it is what the .fcnp files say, and the seating table in
// the .cpp quotes the dump it came from. Getting it wrong is the bug the weapon
// builder had until update 4p: a part seated on the wrong host silently sits at
// the origin.
//
// Where the assets live is worth writing down, because none of it is where you
// would first look:
//
//   /Assets/tpp/mecha/uth/Scenes/…      the helicopter's models
//   /Assets/tpp/pack/mission2/common/mis_com_helicopter.fpk
//                                       …the pack that actually carries them
//   /Assets/tpp/mecha/mgm/Scenes/…      the D-Walker's models ("mgm", not
//                                       "btg" — the development list's icon
//                                       path ui_dw_mgm1_m0 agrees)
//   /Assets/tpp/pack/buddy/walkergear/buddy_wg2_00.fpk    body, heads, arms
//   /Assets/tpp/pack/buddy/walkergear/bw_wp_00…04.fpk     its five weapons
//
// Subjects are grouped by the three-letter directory, because one vehicle
// spans several model prefixes: the D-Walker's body is mgm1 and its weapons
// are mgm0.
#pragma once
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include "index/PartCatalog.h"

namespace fox {

class MechaCatalog {
public:
    static const MechaCatalog& instance();

    const QStringList& slotNames() const { return m_slotNames; }
    QVector<CatalogPart> partsFor(const QString& slot) const;
    QVector<CatalogVariation> variationsFor(const QString& modelStem) const;
    const QVector<CatalogSubject>& subjects() const { return m_subjects; }
    BuilderSource builderSource() const;

    // "mgm1_head0_def" → "mgm" (the subject's directory), "" when the stem is
    // not shaped like a mecha part.
    static QString subjectOf(const QString& stem);
    // "mgm1_head0_def" → "head"
    static QString kindOf(const QString& stem);
    // Seating for a slot, measured from the .fcnp dumps — see the .cpp.
    static QVector<AttachOption> attachPlanFor(const QString& slot);

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
    // Vehicle paint is authored ONCE for every vehicle, not per model:
    // /Assets/tpp/pack/fova/mecha/all/mfv_camo_cNN.fpk carries mfv_camo_cNN.fv2
    // and mfv_scol_cNN.fv2, and the substitution is keyed on material name, so
    // the same table applies to the helicopter and the D-Walker alike.
    QVector<CatalogVariation> m_globalVariations;
    QVector<CatalogSubject> m_subjects;
    QHash<QString, QString> m_stemToDir;
};

}  // namespace fox
