// FmdlFile.cpp — see FmdlFile.h. Field order mirrors FMDL-Studio-v2's Fmdl.cs
// (Read* methods) exactly; deviations are marked.
#include "fox/FmdlFile.h"

#include <QHash>
#include <QtEndian>

#include "fox/FoxHash.h"

namespace fox {
namespace {

// IEEE 754 half → float.
float halfToFloat(quint16 h)
{
    const quint32 sign = (h >> 15) & 1;
    quint32 exp = (h >> 10) & 0x1F;
    quint32 mant = h & 0x3FF;
    quint32 bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign << 31;
        } else {
            // Subnormal: normalize.
            exp = 127 - 15 + 1;
            while (!(mant & 0x400)) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3FF;
            bits = (sign << 31) | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = (sign << 31) | 0x7F800000 | (mant << 13);
    } else {
        bits = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

struct Cursor {
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
    void skip(qsizetype n) { seek(pos + n); }
    quint8 u8()
    {
        if (pos + 1 > size) { ok = false; return 0; }
        return static_cast<quint8>(p[pos++]);
    }
    quint16 u16()
    {
        if (pos + 2 > size) { ok = false; return 0; }
        const quint16 v = qFromLittleEndian<quint16>(p + pos);
        pos += 2;
        return v;
    }
    quint32 u32()
    {
        if (pos + 4 > size) { ok = false; return 0; }
        const quint32 v = qFromLittleEndian<quint32>(p + pos);
        pos += 4;
        return v;
    }
    quint64 u64()
    {
        if (pos + 8 > size) { ok = false; return 0; }
        const quint64 v = qFromLittleEndian<quint64>(p + pos);
        pos += 8;
        return v;
    }
    float f32()
    {
        const quint32 v = u32();
        float f;
        memcpy(&f, &v, 4);
        return f;
    }
};

// Section-0 block ids (FMDL-Studio Section0BlockType).
enum : quint16 {
    kBones = 0, kMeshGroups = 1, kMeshGroupEntries = 2, kMeshInfo = 3,
    kMaterialInstances = 4, kBoneGroups = 5, kTextures = 6,
    kMaterialParameters = 7, kMaterials = 8, kMeshFormatInfo = 9,
    kMeshFormats = 10, kVertexFormats = 11, kStringInfo = 12,
    kBoundingBoxes = 13, kBufferOffsets = 14, kLodInfo = 16, kFaceInfo = 17,
    kPathCode64s = 21, kStrCode64s = 22,
};

struct Block {
    quint16 entryCount = 0;
    quint32 offset = 0;
    bool present = false;
};

}  // namespace

bool FmdlFile::isFmdl(const QByteArray& data)
{
    return data.size() >= 64 && data.startsWith(QByteArray("FMDL", 4));
}

bool FmdlFile::parse(const QByteArray& data)
{
    m_bones.clear();
    m_meshes.clear();
    m_meshGroups.clear();
    m_materials.clear();
    m_error.clear();

    if (!isFmdl(data)) {
        m_error = QStringLiteral("not an FMDL");
        return false;
    }
    Cursor c{data.constData(), data.size()};

    // ── Header ───────────────────────────────────────────────────────────────
    c.u32();                                    // "FMDL"
    m_version = c.f32();                        // 2.03 = GZ era, 2.04 = TPP
    const quint64 sectionInfoOffset = c.u64();
    c.u64();                                    // section0 block flags
    c.u64();                                    // section1 block flags
    const quint32 section0BlockCount = c.u32();
    const quint32 section1BlockCount = c.u32();
    const quint32 section0Offset = c.u32();
    c.u32();                                    // section0 length
    const quint32 section1Offset = c.u32();
    c.u32();                                    // section1 length
    if (!c.ok || section0BlockCount > 64 || section1BlockCount > 16) {
        m_error = QStringLiteral("corrupt FMDL header");
        return false;
    }

    // ── Section info tables ──────────────────────────────────────────────────
    if (!c.seek(static_cast<qsizetype>(sectionInfoOffset))) {
        m_error = QStringLiteral("bad section-info offset");
        return false;
    }
    QHash<quint16, Block> blocks;
    for (quint32 i = 0; i < section0BlockCount; ++i) {
        const quint16 type = c.u16();
        Block b;
        b.entryCount = c.u16();
        b.offset = c.u32();
        b.present = true;
        blocks.insert(type, b);
    }
    quint32 bufferOffsetS1 = 0, stringsOffsetS1 = 0;
    // Section-1 block 0 is the material parameter VECTORS: a flat array of
    // float4 that the non-texture material parameters index into.
    quint32 paramVecOffsetS1 = 0, paramVecLen = 0;
    bool haveBuffer = false, haveStrings = false;
    for (quint32 i = 0; i < section1BlockCount; ++i) {
        const quint32 type = c.u32();
        const quint32 offset = c.u32();
        const quint32 len = c.u32();
        if (type == 0) { paramVecOffsetS1 = offset; paramVecLen = len; }
        if (type == 2) { bufferOffsetS1 = offset; haveBuffer = true; }
        if (type == 3) { stringsOffsetS1 = offset; haveStrings = true; }
    }
    if (!c.ok) {
        m_error = QStringLiteral("truncated section info");
        return false;
    }

    const auto blockAt = [&](quint16 type) { return blocks.value(type); };
    const auto seekBlock = [&](const Block& b) {
        return c.seek(static_cast<qsizetype>(section0Offset) + b.offset);
    };

    // ── Names: literal strings (GZ/2.03) or StrCode64 hashes (TPP/2.04) ─────
    QVector<QString> strings;
    if (haveStrings && blockAt(kStringInfo).present) {
        const Block si = blockAt(kStringInfo);
        struct SInfo { quint16 block; quint16 length; quint32 offset; };
        QVector<SInfo> infos;
        if (!seekBlock(si)) return false;
        for (int i = 0; i < si.entryCount; ++i) {
            SInfo s;
            s.block = c.u16();
            s.length = c.u16();
            s.offset = c.u32();
            infos.append(s);
        }
        for (const SInfo& s : infos) {
            const qsizetype at = static_cast<qsizetype>(section1Offset)
                + stringsOffsetS1 + s.offset;
            if (at + s.length <= data.size())
                strings.append(QString::fromLatin1(data.constData() + at, s.length));
            else
                strings.append(QString());
        }
    }
    QVector<quint64> strCodes;
    if (blockAt(kStrCode64s).present) {
        const Block b = blockAt(kStrCode64s);
        if (!seekBlock(b)) return false;
        for (int i = 0; i < b.entryCount; ++i) strCodes.append(c.u64());
    }
    QVector<quint64> pathCodes;
    if (blockAt(kPathCode64s).present) {
        const Block b = blockAt(kPathCode64s);
        if (!seekBlock(b)) return false;
        for (int i = 0; i < b.entryCount; ++i) pathCodes.append(c.u64());
    }

    const HashResolver& resolver = HashResolver::instance();
    // StrCode32 of a name-table entry: TPP models store the 64-bit hash, GZ
    // models store the literal string. FOVA (.fv2) addresses materials and
    // texture roles by this value, so both eras must produce it.
    const auto hash32At = [&](int idx) -> quint32 {
        if (!strings.isEmpty())
            return idx >= 0 && idx < strings.size()
                ? quint32(hashFileNameLegacy(strings[idx], false) & 0xFFFFFFFFu)
                : 0u;
        return idx >= 0 && idx < strCodes.size()
            ? quint32(strCodes[idx] & 0xFFFFFFFFu)
            : 0u;
    };
    const auto nameAt = [&](int idx) -> QString {
        if (!strings.isEmpty())
            return idx >= 0 && idx < strings.size() ? strings[idx] : QString();
        if (idx >= 0 && idx < strCodes.size()) {
            const QString n = resolver.legacyNameFor(strCodes[idx]);
            return n.isEmpty()
                ? QStringLiteral("0x%1").arg(strCodes[idx], 0, 16)
                : n;
        }
        return QString();
    };
    const auto pathAt = [&](int idx) -> QString {
        // GZ-era models store literal path strings in the same string table.
        if (!strings.isEmpty())
            return idx >= 0 && idx < strings.size() ? strings[idx] : QString();
        if (idx >= 0 && idx < pathCodes.size()) {
            // The stored PathCode64 is the FULL code including the extension
            // bits — FMDL-Studio's TryGetPathName subtracts 0x1568000000000000,
            // which is exactly the "ftex" code (685) << 51. tryResolve handles
            // the whole 64-bit form directly.
            QString n;
            if (resolver.tryResolve(pathCodes[idx], &n)) return n;
        }
        return QString();
    };

    // ── Bones ────────────────────────────────────────────────────────────────
    if (blockAt(kBones).present) {
        const Block b = blockAt(kBones);
        if (!seekBlock(b)) return false;
        for (int i = 0; i < b.entryCount && c.ok; ++i) {
            FmdlBone bone;
            const quint16 nameIndex = c.u16();
            bone.parentIndex = static_cast<qint16>(c.u16());
            c.u16();   // boundingBoxIndex
            c.u16();   // unknown (1)
            c.skip(8);
            for (float& v : bone.localPos) v = c.f32();
            for (float& v : bone.worldPos) v = c.f32();
            bone.name = nameAt(nameIndex);
            // StrCode32 for rig matching: low 32 bits of the legacy hash. TPP
            // models store the hash directly; GZ models store literal names.
            if (!strings.isEmpty())
                bone.hash32 = static_cast<quint32>(
                    hashFileNameLegacy(bone.name, false) & 0xFFFFFFFFu);
            else if (nameIndex < strCodes.size())
                bone.hash32 = static_cast<quint32>(strCodes[nameIndex] & 0xFFFFFFFFu);
            m_bones.append(bone);
        }
    }

    // ── Mesh groups ──────────────────────────────────────────────────────────
    if (blockAt(kMeshGroups).present) {
        const Block b = blockAt(kMeshGroups);
        if (!seekBlock(b)) return false;
        for (int i = 0; i < b.entryCount && c.ok; ++i) {
            FmdlMeshGroup g;
            const quint16 nameIndex = c.u16();
            const quint16 invisible = c.u16();
            g.parentIndex = static_cast<qint16>(c.u16());
            c.u16();   // always 0xFF
            g.name = nameAt(nameIndex);
            g.nameHash32 = hash32At(nameIndex);
            g.visible = invisible == 0;
            m_meshGroups.append(g);
        }
    }

    // Mesh-group entries: map mesh ranges → mesh groups.
    struct GroupEntry { quint16 group; quint16 meshCount; quint16 firstMesh; };
    QVector<GroupEntry> groupEntries;
    if (blockAt(kMeshGroupEntries).present) {
        const Block b = blockAt(kMeshGroupEntries);
        if (!seekBlock(b)) return false;
        for (int i = 0; i < b.entryCount && c.ok; ++i) {
            GroupEntry e;
            c.skip(4);
            e.group = c.u16();
            e.meshCount = c.u16();
            e.firstMesh = c.u16();
            c.u16();       // index
            c.skip(4);
            c.u16();       // firstFaceInfoIndex
            c.skip(0xE);
            groupEntries.append(e);
        }
    }

    // ── Mesh info ────────────────────────────────────────────────────────────
    struct MeshInfo {
        quint16 materialInstanceIndex, boneGroupIndex, vertexCount;
        quint32 firstFaceVertexIndex, faceVertexCount;
    };
    QVector<MeshInfo> meshInfos;
    if (blockAt(kMeshInfo).present) {
        const Block b = blockAt(kMeshInfo);
        if (!seekBlock(b)) return false;
        for (int i = 0; i < b.entryCount && c.ok; ++i) {
            MeshInfo m;
            c.skip(4);   // alpha/shadow/unknown bytes
            m.materialInstanceIndex = c.u16();
            m.boneGroupIndex = c.u16();
            c.u16();     // index
            m.vertexCount = c.u16();
            c.skip(4);
            m.firstFaceVertexIndex = c.u32();
            m.faceVertexCount = c.u32();
            c.u64();     // firstFaceInfoIndex
            c.skip(0x10);
            meshInfos.append(m);
        }
    }

    // ── Bone groups (skin palettes) ──────────────────────────────────────────
    QVector<QVector<quint16>> boneGroups;
    if (blockAt(kBoneGroups).present) {
        const Block b = blockAt(kBoneGroups);
        if (!seekBlock(b)) return false;
        for (int i = 0; i < b.entryCount && c.ok; ++i) {
            c.u16();   // unknown
            const quint16 count = c.u16();
            QVector<quint16> indices;
            for (int j = 0; j < count; ++j) indices.append(c.u16());
            c.skip(0x40 - count * 2);
            boneGroups.append(indices);
        }
    }

    // ── Materials & textures ─────────────────────────────────────────────────
    struct TexEntry { quint16 nameIndex, pathIndex; };
    QVector<TexEntry> texEntries;
    if (blockAt(kTextures).present) {
        const Block b = blockAt(kTextures);
        if (!seekBlock(b)) return false;
        for (int i = 0; i < b.entryCount && c.ok; ++i) {
            TexEntry t;
            t.nameIndex = c.u16();
            t.pathIndex = c.u16();
            texEntries.append(t);
        }
    }
    struct MatParam { quint16 nameIndex, referenceIndex; };
    QVector<MatParam> matParams;
    if (blockAt(kMaterialParameters).present) {
        const Block b = blockAt(kMaterialParameters);
        if (!seekBlock(b)) return false;
        for (int i = 0; i < b.entryCount && c.ok; ++i) {
            MatParam mp;
            mp.nameIndex = c.u16();
            mp.referenceIndex = c.u16();
            matParams.append(mp);
        }
    }
    // The float4 array the non-texture parameters index into. Read straight
    // out of section 1; a truncated or absent block simply leaves it empty,
    // and every lookup below is bounds-checked against it.
    struct Vec4 { float v[4]; };
    QVector<Vec4> paramVectors;
    if (paramVecLen >= 16) {
        const qsizetype base =
            static_cast<qsizetype>(section1Offset) + paramVecOffsetS1;
        const int n = int(paramVecLen / 16);
        for (int i = 0; i < n; ++i) {
            const qsizetype at = base + qsizetype(i) * 16;
            if (at + 16 > data.size()) break;
            Vec4 v;
            for (int k = 0; k < 4; ++k) {
                quint32 bits = 0;
                memcpy(&bits, data.constData() + at + k * 4, 4);
                bits = qFromLittleEndian(bits);
                float f = 0.0f;
                memcpy(&f, &bits, 4);
                v.v[k] = f;
            }
            paramVectors.append(v);
        }
    }

    struct MatDef { quint16 nameIndex, typeIndex; };
    QVector<MatDef> matDefs;
    if (blockAt(kMaterials).present) {
        const Block b = blockAt(kMaterials);
        if (!seekBlock(b)) return false;
        for (int i = 0; i < b.entryCount && c.ok; ++i) {
            MatDef d;
            d.nameIndex = c.u16();
            d.typeIndex = c.u16();
            matDefs.append(d);
        }
    }
    if (blockAt(kMaterialInstances).present) {
        const Block b = blockAt(kMaterialInstances);
        if (!seekBlock(b)) return false;
        for (int i = 0; i < b.entryCount && c.ok; ++i) {
            FmdlMaterialInstance inst;
            const quint16 nameIndex = c.u16();
            c.skip(2);
            const quint16 materialIndex = c.u16();
            const quint8 textureCount = c.u8();
            const quint8 parameterCount = c.u8();
            const quint16 firstTextureIndex = c.u16();
            const quint16 firstParameterIndex = c.u16();
            c.skip(4);
            inst.name = nameAt(nameIndex);
            inst.nameHash32 = hash32At(nameIndex);
            if (materialIndex < matDefs.size())
                inst.shader = nameAt(matDefs[materialIndex].typeIndex);
            // Texture linking (FmdlImporter.cs): iterate the MATERIAL-PARAMETER
            // table from firstTextureIndex — each row gives the ROLE name and,
            // via referenceIndex, the row in the TEXTURE table. Indexing the
            // texture table directly mispairs roles and maps.
            for (int t = 0; t < textureCount; ++t) {
                const int pi = firstTextureIndex + t;
                if (pi >= matParams.size()) break;
                const int ti = matParams[pi].referenceIndex;
                if (ti < 0 || ti >= texEntries.size()) continue;
                FmdlTextureRef ref;
                ref.role = nameAt(matParams[pi].nameIndex);
                ref.roleHash32 = hash32At(matParams[pi].nameIndex);
                ref.path = pathAt(texEntries[ti].pathIndex);
                if (strings.isEmpty()) {
                    if (texEntries[ti].pathIndex < pathCodes.size())
                        ref.pathHash = pathCodes[texEntries[ti].pathIndex];
                } else {
                    // GZ string-table models split directory (pathIndex) and
                    // file name (nameIndex) — join them.
                    ref.path += nameAt(texEntries[ti].nameIndex);
                }
                inst.textures.append(ref);
            }
            // The NON-texture parameters, from the same table. A row here has
            // its referenceIndex pointing into the float4 array rather than
            // into the texture table — same row layout, different target,
            // which is why the two ranges have to be walked separately rather
            // than as one run.
            for (int q = 0; q < parameterCount; ++q) {
                const int pi = firstParameterIndex + q;
                if (pi < 0 || pi >= matParams.size()) break;
                const int vi = matParams[pi].referenceIndex;
                if (vi < 0 || vi >= paramVectors.size()) continue;
                FmdlMaterialParam mp;
                mp.name = nameAt(matParams[pi].nameIndex);
                mp.nameHash32 = hash32At(matParams[pi].nameIndex);
                for (int k = 0; k < 4; ++k) mp.value[k] = paramVectors[vi].v[k];
                inst.params.append(mp);
            }
            m_materials.append(inst);
        }
    }

    // ── Vertex / face buffers ────────────────────────────────────────────────
    struct MeshFormatInfo {
        quint8 meshFormatCount, vertexFormatCount;
        quint16 firstMeshFormatIndex, firstVertexFormatIndex;
    };
    QVector<MeshFormatInfo> meshFormatInfos;
    if (blockAt(kMeshFormatInfo).present) {
        const Block b = blockAt(kMeshFormatInfo);
        if (!seekBlock(b)) return false;
        for (int i = 0; i < b.entryCount && c.ok; ++i) {
            MeshFormatInfo m;
            m.meshFormatCount = c.u8();
            m.vertexFormatCount = c.u8();
            c.u8();
            c.u8();   // uvCount
            m.firstMeshFormatIndex = c.u16();
            m.firstVertexFormatIndex = c.u16();
            meshFormatInfos.append(m);
        }
    }
    struct MeshFormat { quint8 bufferOffsetIndex, vertexFormatCount, length, type; quint32 offset; };
    QVector<MeshFormat> meshFormats;
    if (blockAt(kMeshFormats).present) {
        const Block b = blockAt(kMeshFormats);
        if (!seekBlock(b)) return false;
        for (int i = 0; i < b.entryCount && c.ok; ++i) {
            MeshFormat m;
            m.bufferOffsetIndex = c.u8();
            m.vertexFormatCount = c.u8();
            m.length = c.u8();
            m.type = c.u8();
            m.offset = c.u32();
            meshFormats.append(m);
        }
    }
    struct VertexFormat { quint8 type, dataType; quint16 offset; };
    QVector<VertexFormat> vertexFormats;
    if (blockAt(kVertexFormats).present) {
        const Block b = blockAt(kVertexFormats);
        if (!seekBlock(b)) return false;
        for (int i = 0; i < b.entryCount && c.ok; ++i) {
            VertexFormat v;
            v.type = c.u8();
            v.dataType = c.u8();
            v.offset = c.u16();
            vertexFormats.append(v);
        }
    }
    struct BufOffset { quint32 length, offset; };
    QVector<BufOffset> bufOffsets;
    if (blockAt(kBufferOffsets).present) {
        const Block b = blockAt(kBufferOffsets);
        if (!seekBlock(b)) return false;
        for (int i = 0; i < b.entryCount && c.ok; ++i) {
            BufOffset o;
            c.u32();   // flags
            o.length = c.u32();
            o.offset = c.u32();
            c.skip(4);
            bufOffsets.append(o);
        }
    }

    if (!c.ok) {
        m_error = QStringLiteral("truncated section 0");
        return false;
    }
    if (!haveBuffer || meshInfos.isEmpty() || meshFormatInfos.size() < meshInfos.size()
        || bufOffsets.size() < 3) {
        // Metadata-only model (some .fmdlb / helper files) — expose what we have.
        return true;
    }

    const qsizetype bufBase = static_cast<qsizetype>(section1Offset) + bufferOffsetS1;

    for (int i = 0; i < meshInfos.size(); ++i) {
        const MeshInfo& mi = meshInfos[i];
        const MeshFormatInfo& mfi = meshFormatInfos[i];
        FmdlMesh mesh;
        mesh.materialInstanceIndex = mi.materialInstanceIndex;
        if (mi.boneGroupIndex < boneGroups.size())
            mesh.palette = boneGroups[mi.boneGroupIndex];

        if (mfi.firstMeshFormatIndex + 1 >= meshFormats.size()) continue;

        // Positions: tightly packed float3 in buffer slot 0.
        {
            const qsizetype at = bufBase + bufOffsets[0].offset
                + meshFormats[mfi.firstMeshFormatIndex].offset;
            if (at + static_cast<qsizetype>(mi.vertexCount) * 12 > data.size()) continue;
            mesh.positions.reserve(mi.vertexCount * 3);
            const char* vp = data.constData() + at;
            for (int v = 0; v < mi.vertexCount; ++v) {
                for (int k = 0; k < 3; ++k) {
                    quint32 raw = qFromLittleEndian<quint32>(vp + v * 12 + k * 4);
                    float f;
                    memcpy(&f, &raw, 4);
                    mesh.positions.append(f);
                }
            }
        }

        // Interleaved extras: buffer slot 1, stride = second mesh format length.
        {
            const MeshFormat& extra = meshFormats[mfi.firstMeshFormatIndex + 1];
            const qsizetype base = bufBase + bufOffsets[1].offset + extra.offset;
            const int stride = extra.length;
            const bool inRange =
                base + static_cast<qsizetype>(mi.vertexCount) * stride <= data.size();
            if (inRange && stride > 0) {
                // Tangents are allocated ONLY when the mesh really declares a
                // usage-14 attribute. A zero-filled array of the right size is
                // indistinguishable from real data downstream, and the glTF
                // exporter would then write a fabricated constant tangent frame
                // for a mesh that has none — worse than omitting TANGENT and
                // letting the importer derive it from the UVs.
                bool declaresTangent = false;
                for (int a = mfi.firstVertexFormatIndex;
                     a < mfi.firstVertexFormatIndex + mfi.vertexFormatCount
                     && a < vertexFormats.size();
                     ++a)
                    if (vertexFormats[a].type == 14) { declaresTangent = true; break; }

                mesh.normals.resize(mi.vertexCount * 3);
                mesh.uv0.resize(mi.vertexCount * 2);
                if (declaresTangent) mesh.tangents.resize(mi.vertexCount * 4);
                mesh.boneIndices.resize(mi.vertexCount * 4);
                mesh.boneWeights.resize(mi.vertexCount * 4);
                mesh.normals.fill(0);
                mesh.uv0.fill(0);
                mesh.tangents.fill(0);   // no-op when the mesh declares none
                mesh.boneIndices.fill(0);
                mesh.boneWeights.fill(0);
                for (int v = 0; v < mi.vertexCount; ++v) {
                    const char* rec = data.constData() + base
                        + static_cast<qsizetype>(v) * stride;
                    for (int a = mfi.firstVertexFormatIndex;
                         a < mfi.firstVertexFormatIndex + mfi.vertexFormatCount
                         && a < vertexFormats.size();
                         ++a) {
                        const VertexFormat& vf = vertexFormats[a];
                        if (v == 0) mesh.vertexUsages.append(vf.type);
                        // Attribute must fit inside the record (corrupt-file
                        // hardening: vf.offset is raw u16 from the file).
                        // usage 14 = tangent, stored as half4 like the normal
                        // but the w (bitangent sign) is needed, so all 8 bytes.
                        const int attrBytes = vf.type == 2 ? 6
                            : vf.type == 14 ? 8
                            : (vf.type == 1 || vf.type == 7 || vf.type == 8) ? 4
                                                                             : 0;
                        if (attrBytes == 0 || vf.offset + attrBytes > stride)
                            continue;
                        const char* q = rec + vf.offset;
                        switch (vf.type) {
                        case 1:   // bone weights, 4 bytes
                            for (int k = 0; k < 4; ++k)
                                mesh.boneWeights[v * 4 + k] =
                                    static_cast<quint8>(q[k]) / 255.0f;
                            break;
                        case 2:   // normal, half4
                            for (int k = 0; k < 3; ++k)
                                mesh.normals[v * 3 + k] =
                                    halfToFloat(qFromLittleEndian<quint16>(q + k * 2));
                            break;
                        case 7:   // bone indices, 4 bytes (palette-relative)
                            for (int k = 0; k < 4; ++k)
                                mesh.boneIndices[v * 4 + k] = static_cast<quint8>(q[k]);
                            break;
                        case 14:  // tangent, half4 (w = bitangent sign)
                            if (mesh.tangents.size() != mi.vertexCount * 4) break;
                            for (int k = 0; k < 4; ++k)
                                mesh.tangents[v * 4 + k] =
                                    halfToFloat(qFromLittleEndian<quint16>(q + k * 2));
                            break;
                        case 8:   // uv0, half2
                            for (int k = 0; k < 2; ++k)
                                mesh.uv0[v * 2 + k] =
                                    halfToFloat(qFromLittleEndian<quint16>(q + k * 2));
                            break;
                        default:  // vertex colors / extra UV sets — unused
                            break;
                        }
                    }
                }
            }
        }

        // Faces: u16 list in buffer slot 2.
        {
            const qsizetype at = bufBase + bufOffsets[2].offset
                + static_cast<qsizetype>(mi.firstFaceVertexIndex) * 2;
            if (at + static_cast<qsizetype>(mi.faceVertexCount) * 2 <= data.size()) {
                mesh.triangles.reserve(static_cast<int>(mi.faceVertexCount));
                const char* fp = data.constData() + at;
                for (quint32 t = 0; t < mi.faceVertexCount; ++t)
                    mesh.triangles.append(qFromLittleEndian<quint16>(fp + t * 2));
            }
        }
        m_meshes.append(mesh);
    }

    // Mesh → mesh-group assignment.
    for (const GroupEntry& e : groupEntries)
        for (int m = e.firstMesh; m < e.firstMesh + e.meshCount && m < m_meshes.size(); ++m)
            m_meshes[m].meshGroupIndex = e.group;

    return true;
}

QString FmdlFile::describe() const
{
    qint64 tris = 0;
    for (const FmdlMesh& m : m_meshes) tris += m.triangles.size() / 3;
    return QStringLiteral("v%1 — %2 meshes, %3 bones, %4 materials, %5 triangles")
        .arg(m_version, 0, 'f', 2)
        .arg(m_meshes.size())
        .arg(m_bones.size())
        .arg(m_materials.size())
        .arg(tris);
}


QVector<bool> FmdlFile::boneSubtreeMask(quint32 rootBone) const
{
    int root = -1;
    for (int i = 0; i < m_bones.size(); ++i)
        if (m_bones[i].hash32 == rootBone) { root = i; break; }
    if (root < 0) return {};
    // Parents precede children in every Fox skeleton, so one forward pass
    // marks the whole subtree.
    QVector<bool> mask(m_bones.size(), false);
    mask[root] = true;
    for (int i = root + 1; i < m_bones.size(); ++i) {
        const int p = m_bones[i].parentIndex;
        if (p >= 0 && p < i && mask[p]) mask[i] = true;
    }
    return mask;
}

}  // namespace fox
