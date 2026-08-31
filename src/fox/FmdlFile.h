// FmdlFile.h — Fox Engine model (.fmdl) parser: skeleton, mesh groups,
// vertex/index buffers, materials and texture references.
//
// Structure reference: FMDL-Studio-v2 (BobDoleOwndU) — the community
// implementation that round-trips TPP character models. Layout in brief:
//   • 64-byte header → section-info tables. Section 0 = typed metadata blocks
//     (bones, mesh info, formats, …), section 1 = bulk data (vertex buffers,
//     strings).
//   • Vertex data: positions are tightly packed float3 in buffer slot 0; all
//     other attributes live interleaved in buffer slot 1 with per-mesh stride
//     and per-attribute offsets (halves for normals/tangents/UVs, bytes for
//     bone weights/indices). Faces are u16 lists in slot 2.
//   • Names: TPP models carry StrCode64 hashes (= the legacy CityHash — our
//     dictionaries resolve them); GZ-era models carry literal string tables.
//   • Texture references: PathCode64 of the extension-less texture path.
//   • Skinning: per-mesh bone groups are palettes of skeleton indices; vertex
//     bone indices index the palette, not the skeleton (the parents-precede-
//     children rule holds in the skeleton array).
#pragma once
#include <QString>
#include <QVector>
#include <cstdint>

namespace fox {

struct FmdlBone {
    QString name;             // resolved or "0x<hash>"
    qint16 parentIndex = -1;  // parents precede children
    float localPos[4] = {};   // translation-only bind pose
    float worldPos[4] = {};
    quint32 hash32 = 0;       // StrCode32 (low 32 of the legacy name hash)
    quint32 nameHash32() const { return hash32; }
};

struct FmdlTextureRef {
    QString role;             // e.g. "Base_Tex_SRGB", "NormalMap_Tex_NRM"
    quint32 roleHash32 = 0;   // StrCode32 of the role name — how .fv2 FOVA
                              // variations address a texture slot
    quint64 pathHash = 0;     // extension-less PathCode64
    QString path;             // resolved path text ("" when not in dictionary)
};

// A NON-texture material parameter: a named float4 the shader reads.
//
// The one that matters is MatParamIndex_0..3, which is how a multi-material
// surface works. A "_2MT_"/"_3MT_"/"_4MT_" shader treats one mesh as two,
// three or four surfaces; the MTM (MatParamMap_Tex_LIN) says which region each
// texel belongs to, and these say which FMTT preset each region uses. Without
// them a 4MT material can only be drawn as one surface, which is what this
// viewport did.
struct FmdlMaterialParam {
    QString name;             // e.g. "MatParamIndex_0", or "0x<hash>"
    quint32 nameHash32 = 0;   // StrCode32
    float value[4] = {};
};

struct FmdlMaterialInstance {
    QString name;
    quint32 nameHash32 = 0;   // StrCode32 — how .fv2 FOVA variations address
                              // a material instance
    QString shader;
    QVector<FmdlTextureRef> textures;
    QVector<FmdlMaterialParam> params;
};

struct FmdlMesh {
    QVector<float> positions;     // xyz per vertex
    QVector<float> normals;       // xyz per vertex (decoded from half4)
    QVector<float> uv0;           // uv per vertex
    QVector<float> tangents;      // xyzw per vertex (w = bitangent sign)
    QVector<quint8> boneIndices;  // 4 per vertex — indices into `palette`
    QVector<float> boneWeights;   // 4 per vertex, normalized
    QVector<quint16> triangles;   // index list (triangle list)
    QVector<quint16> palette;     // bone-group palette → skeleton indices
    int materialInstanceIndex = -1;
    int meshGroupIndex = -1;      // for the parts tree
    // Which vertex-attribute USAGES this mesh's format declares, in file
    // order. The parser consumes 1 (bone weights), 2 (normal), 7 (bone
    // indices), 8 (uv0) and 14 (tangent); anything else is skipped. Recorded
    // because "does this mesh carry a second UV set" is a question about the
    // FORMAT, not about the data, and it decides what a UV-repeat material
    // parameter can possibly be scaling.
    QVector<quint8> vertexUsages;
};

struct FmdlMeshGroup {
    QString name;
    qint16 parentIndex = -1;
    bool visible = true;
    quint32 nameHash32 = 0;   // StrCode32 — how a .fv2 FOVA variation names the
                              // groups it hides and shows ("MESH_mask", …)
};

class FmdlFile {
public:
    static bool isFmdl(const QByteArray& data);

    bool parse(const QByteArray& data);
    QString errorString() const { return m_error; }

    const QVector<FmdlBone>& bones() const { return m_bones; }
    const QVector<FmdlMesh>& meshes() const { return m_meshes; }
    const QVector<FmdlMeshGroup>& meshGroups() const { return m_meshGroups; }
    // Per-bone flags: true for `rootBone` (a StrCode32) and everything under
    // it. Empty when this model has no such bone. Callers use it to keep or
    // drop the geometry skinned to one limb — the head of a base body worn
    // under clothing, say — without each of them re-walking the hierarchy.
    QVector<bool> boneSubtreeMask(quint32 rootBone) const;
    const QVector<FmdlMaterialInstance>& materials() const { return m_materials; }
    float version() const { return m_version; }

    QString describe() const;

private:
    QVector<FmdlBone> m_bones;
    QVector<FmdlMesh> m_meshes;
    QVector<FmdlMeshGroup> m_meshGroups;
    QVector<FmdlMaterialInstance> m_materials;
    float m_version = 0;
    QString m_error;
};

}  // namespace fox
