// DfrmFile.h — Fox Engine ".dfrm" (DFRM), the deform-support file that ships
// beside every avatar head.
//
// WHAT IT IS NOT: it carries no shape. There is not one float in the file
// outside the header — 22,227 four-byte words in avf0_type0_def.dfrm and zero
// of them are a plausible non-zero float — so whatever moves an avatar's face,
// it is not a blendshape stored here.
//
// WHAT IT IS: the topology a deformer needs once the mesh HAS moved. Three
// things, all integers:
//
//   * a WELD map, mesh vertex -> shared position. avf0_type0_def has 2759 mesh
//     vertices across 16 meshes and 2672 distinct positions, so 87 of them are
//     duplicates sitting on a seam.
//   * a FACE FAN per welded position — the triangles that touch it, as
//     (mesh, triangle) pairs. Averaged 4.76 per vertex, and for the little
//     4-vertex quads it gives exactly 2 triangles, 8 gives 4, 3 gives 1, which
//     is what pins the reading down.
//   * 330 GROUPS of 2-6 welded vertices, which is the right shape for seam
//     sets that must be averaged so a deformed face does not split open.
//
// Layout, measured (all offsets absolute, all little-endian):
//
//   0x00  "DFRM", float 1.0
//   0x08  6 counts, then 6 offsets:
//           meshes, welded, meshVerts, fanRecords, groups, groupMembers
//   meshes[]        (u32 vertexCount, u32 offsetIntoWeld)  — one per FMDL mesh,
//                   and the counts match that model's mesh table exactly
//   weld[]          u32 per mesh vertex -> welded index
//   welded[]        (u32 fanCount, u32 offsetIntoFans)
//   fans[]          u16 mesh, u16 triangle
//   groups[]        (u32 memberCount, u32 offsetIntoMembers)
//   members[]       u32 welded index
//
// The browser does not deform anything yet, so nothing consumes this. It is
// parsed and dumped so the next person to look at avatar morphs starts from
// what the file IS rather than from what its name suggests.
#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>

namespace fox {

struct DfrmMesh {
    quint32 vertexCount = 0;
    quint32 firstWeld = 0;      // index into weld[]
};

struct DfrmFan {
    quint16 mesh = 0;
    quint16 triangle = 0;
};

class DfrmFile {
public:
    static bool isDfrm(const QByteArray& data);
    bool parse(const QByteArray& data);
    QString errorString() const { return m_error; }

    const QVector<DfrmMesh>& meshes() const { return m_meshes; }
    const QVector<quint32>& weld() const { return m_weld; }
    // Fan records for welded vertex i.
    QVector<DfrmFan> fansFor(int weldedIndex) const;
    int weldedCount() const { return m_fanAt.size(); }
    // Members of seam group i.
    QVector<quint32> group(int i) const;
    int groupCount() const { return m_groupAt.size(); }

    // One line of what was read, for the log.
    QString summary() const;

private:
    QString m_error;
    QVector<DfrmMesh> m_meshes;
    QVector<quint32> m_weld;
    QVector<QPair<quint32, quint32>> m_fanAt;     // (count, first index into m_fans)
    QVector<DfrmFan> m_fans;
    QVector<QPair<quint32, quint32>> m_groupAt;   // (count, first index into m_members)
    QVector<quint32> m_members;
};

}  // namespace fox
