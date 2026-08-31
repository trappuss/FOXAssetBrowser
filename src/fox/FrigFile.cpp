// FrigFile.cpp — see FrigFile.h. Field order mirrors Fox_Parser's FrigFile.cs.
#include "fox/FrigFile.h"

#include <QtEndian>

namespace fox {
namespace {

struct Rd {
    const char* p;
    qsizetype size;
    qsizetype pos = 0;
    bool ok = true;

    bool seek(qsizetype to)
    {
        if (to < 0 || to > size) { ok = false; return false; }
        pos = to;
        return true;
    }
    quint32 u32()
    {
        if (pos + 4 > size) { ok = false; return 0; }
        const quint32 v = qFromLittleEndian<quint32>(p + pos);
        pos += 4;
        return v;
    }
    qint32 s32() { return static_cast<qint32>(u32()); }
    qint16 s16()
    {
        if (pos + 2 > size) { ok = false; return 0; }
        const qint16 v = qFromLittleEndian<qint16>(p + pos);
        pos += 2;
        return v;
    }
    float f32()
    {
        const quint32 v = u32();
        float f;
        memcpy(&f, &v, 4);
        return f;
    }
    void skip(qsizetype n) { seek(pos + n); }
};

}  // namespace

bool FrigFile::parse(const QByteArray& data)
{
    m_units.clear();
    m_bones.clear();
    m_rigUnitCount = 0;

    Rd r{data.constData(), data.size()};
    r.skip(8);                          // FoxDataName
    const quint32 version = r.u32();
    if (!r.ok || version != 102) return false;
    m_rigUnitCount = r.s32();
    r.u32();                            // SegmentCount
    r.u32();                            // FileSize
    const qint32 boneListOffset = r.s32();
    r.s32();                            // MaskDefOffset (unused here)
    if (!r.ok || m_rigUnitCount <= 0 || m_rigUnitCount > 4096) return false;

    QVector<qint32> unitOffsets(m_rigUnitCount);
    for (int i = 0; i < m_rigUnitCount; ++i) unitOffsets[i] = r.s32();
    if (!r.ok) return false;

    for (const qint32 off : unitOffsets) {
        RigUnit u;
        if (off <= 0 || off + 4 > data.size()) {
            m_units.append(u);
            continue;
        }
        r.seek(off);
        u.type = static_cast<RigUnitType>(r.u32());
        u.trackCount = r.s16();
        r.s16();
        r.s16();
        r.s16();
        r.u32();   // bone/parent/parent/pad

        const auto readPlaneNormal = [&] {
            r.skip(16);
            u.planeNormal[0] = r.f32();
            u.planeNormal[1] = r.f32();
            u.planeNormal[2] = r.f32();
            r.f32();   // pad
        };
        switch (u.type) {
        case RigUnitType::Root:
            u.segQ0 = r.s16();
            u.segV = r.s16();
            break;
        case RigUnitType::Orientation:
        case RigUnitType::LocalOrientation:
            u.skelIndex = r.s16();
            u.segQ0 = r.s16();
            break;
        case RigUnitType::Transform:
        case RigUnitType::LocalTransform:
            u.skelIndex = r.s16();
            u.segQ0 = r.s16();
            u.segV = r.s16();
            break;
        case RigUnitType::ThreeBoneLikeTwoBone:
            readPlaneNormal();
            u.chainA = r.s16();
            u.chainB = r.s16();
            u.chainC = r.s16();
            u.segQ0 = r.s16();
            u.segV = r.s16();
            break;
        case RigUnitType::Arm:
            readPlaneNormal();
            u.chainA = r.s16();
            u.chainB = r.s16();
            u.chainC = r.s16();
            u.segQ0 = r.s16();
            u.segV = r.s16();
            u.segQ1 = r.s16();
            u.effector = r.s16();
            break;
        case RigUnitType::TwoBone:
            readPlaneNormal();
            u.chainA = r.s16();
            u.chainB = r.s16();
            u.segQ0 = r.s16();
            u.segV = r.s16();
            u.effector = r.s16();
            break;
        case RigUnitType::LocalTransformSrt:
            u.skelIndex = r.s16();
            break;
        case RigUnitType::MultiLocalOrientation:
            u.skelIndex = r.s16();
            u.segQ0 = r.s16();
            break;
        default:
            break;   // AnimalLeg / TwoBoneTrans left as list-only for now
        }
        m_units.append(u);
    }

    if (boneListOffset <= 0 || !r.seek(boneListOffset)) return false;
    const qint32 boneCount = r.s32();
    if (!r.ok || boneCount < 0
        || boneListOffset + 4 + static_cast<qint64>(boneCount) * 8 > data.size())
        return false;
    for (int i = 0; i < boneCount; ++i) {
        FrigBone b;
        b.rigIndex = r.u32();
        b.nameHash32 = r.u32();
        m_bones.append(b);
    }
    return r.ok && !m_bones.isEmpty();
}

QHash<int, FrigFile::BoneDrive> FrigFile::resolveBoneDrives(
    const QVector<quint32>& fmdlBoneHash32, int trackCount, int* matchCount) const
{
    QHash<quint32, int> hashToBone;
    for (int b = 0; b < fmdlBoneHash32.size(); ++b)
        if (!hashToBone.contains(fmdlBoneHash32[b]))
            hashToBone.insert(fmdlBoneHash32[b], b);

    QHash<int, BoneDrive> map;
    for (int bi = 0; bi < m_bones.size(); ++bi) {
        const FrigBone& fb = m_bones[bi];
        if (fb.rigIndex >= static_cast<quint32>(trackCount)) continue;
        const auto it = hashToBone.constFind(fb.nameHash32);
        if (it == hashToBone.constEnd()) continue;
        BoneDrive d;
        d.track = static_cast<int>(fb.rigIndex);
        d.type = fb.rigIndex < static_cast<quint32>(m_units.size())
            ? m_units[fb.rigIndex].type
            : RigUnitType::LocalOrientation;
        if (fb.rigIndex < static_cast<quint32>(m_units.size())) {
            const RigUnit& un = m_units[fb.rigIndex];
            switch (d.type) {
            case RigUnitType::Orientation:
            case RigUnitType::LocalOrientation:
                d.segRot = un.segQ0;
                break;
            case RigUnitType::Transform:
            case RigUnitType::LocalTransform:
                d.segRot = un.segQ0;
                d.segPos = un.segV;
                break;
            case RigUnitType::MultiLocalOrientation:
                if (un.skelIndex >= 0 && bi >= un.skelIndex) {
                    d.channel = bi - un.skelIndex;
                    if (un.segQ0 >= 0) d.segRot = un.segQ0 + d.channel;
                }
                break;
            default:
                break;
            }
        }
        map.insert(it.value(), d);
    }
    if (matchCount) *matchCount = map.size();
    return map;
}

QVector<FrigFile::IkJob> FrigFile::resolveIkJobs(const QVector<quint32>& fmdlBoneHash32,
                                                 int trackCount) const
{
    QHash<quint32, int> hashToBone;
    for (int b = 0; b < fmdlBoneHash32.size(); ++b)
        if (!hashToBone.contains(fmdlBoneHash32[b]))
            hashToBone.insert(fmdlBoneHash32[b], b);
    const auto mapBone = [&](int skel) -> int {
        if (skel < 0 || skel >= m_bones.size()) return -1;
        return hashToBone.value(m_bones[skel].nameHash32, -1);
    };

    QVector<IkJob> jobs;
    for (int u = 0; u < m_units.size() && u < trackCount; ++u) {
        const RigUnit& unit = m_units[u];
        if (unit.type != RigUnitType::TwoBone && unit.type != RigUnitType::Arm
            && unit.type != RigUnitType::ThreeBoneLikeTwoBone)
            continue;
        IkJob j;
        j.type = unit.type;
        j.chainA = mapBone(unit.chainA);
        j.chainB = mapBone(unit.chainB);
        j.chainC = mapBone(unit.chainC);
        j.effector = mapBone(unit.effector);
        j.track = u;
        for (int k = 0; k < 3; ++k) j.planeNormal[k] = unit.planeNormal[k];
        if (j.chainA < 0 || j.chainB < 0 || j.effector < 0) continue;
        jobs.append(j);
    }
    return jobs;
}

}  // namespace fox
