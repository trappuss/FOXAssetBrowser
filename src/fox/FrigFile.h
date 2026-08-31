// FrigFile.h — Fox Engine rig (.frig): the bridge between a gani's rig-unit
// tracks and an FMDL's bones. A gani animates RIG UNITS (track i drives unit
// i); the frig's BoneList says, per skeleton bone, WHICH unit drives it and
// the bone's StrCode32 name. Port of Fox_Parser's FrigFile (version 102).
#pragma once
#include <QByteArray>
#include <QHash>
#include <QVector>
#include <cstdint>

namespace fox {

class FrigFile {
public:
    enum class RigUnitType : quint32 {
        None = 0, Root = 1, Orientation = 2, TwoBone = 3, LocalOrientation = 4,
        LocalTransform = 5, ThreeBoneLikeTwoBone = 6, Transform = 7, Arm = 8,
        LocalTransformSrt = 9, AnimalLeg = 10, MultiLocalOrientation = 11,
        TwoBoneTrans = 12,
    };

    struct RigUnit {
        RigUnitType type = RigUnitType::None;
        int skelIndex = -1;    // BoneList index (Orientation/Transform families)
        int effector = -1;     // IK goal bone (BoneList index)
        int chainA = -1, chainB = -1, chainC = -1;
        float planeNormal[3] = {0, 0, 0};
        int segQ0 = -1, segV = -1, segQ1 = -1, segQ2 = -1;   // flat segment shorts
        int trackCount = 0;
    };

    struct FrigBone {
        quint32 rigIndex = 0;
        quint32 nameHash32 = 0;
    };

    // How an FMDL bone is driven.
    struct BoneDrive {
        int track = -1;
        RigUnitType type = RigUnitType::LocalOrientation;
        int channel = -1;      // MultiLocalOrientation sub-channel
        int segRot = -1;       // flat segment index of the rotation channel
        int segPos = -1;       // flat segment index of the translation channel
    };

    struct IkJob {
        RigUnitType type = RigUnitType::TwoBone;
        int chainA = -1, chainB = -1, chainC = -1;   // FMDL bone indices
        int effector = -1;
        int track = -1;
        float planeNormal[3] = {0, 0, 0};
    };

    bool parse(const QByteArray& data);
    bool valid() const { return !m_bones.isEmpty(); }

    int rigUnitCount() const { return m_rigUnitCount; }
    const QVector<RigUnit>& units() const { return m_units; }
    const QVector<FrigBone>& bones() const { return m_bones; }

    static bool isWorldSpace(RigUnitType t)
    {
        return t == RigUnitType::Orientation || t == RigUnitType::TwoBone
            || t == RigUnitType::Arm;
    }

    // FMDL bone index → drive, matching frig bones to FMDL bones by StrCode32
    // (low 32 bits of each FMDL bone's StrCode64 name hash).
    QHash<int, BoneDrive> resolveBoneDrives(const QVector<quint32>& fmdlBoneHash32,
                                            int trackCount, int* matchCount) const;

    // Every resolvable TwoBone / Arm / ThreeBone IK chain.
    QVector<IkJob> resolveIkJobs(const QVector<quint32>& fmdlBoneHash32,
                                 int trackCount) const;

private:
    int m_rigUnitCount = 0;
    QVector<RigUnit> m_units;
    QVector<FrigBone> m_bones;
};

}  // namespace fox
