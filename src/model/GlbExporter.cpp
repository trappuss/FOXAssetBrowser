// GlbExporter.cpp — see GlbExporter.h.
#include "model/GlbExporter.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QImage>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <cmath>

#include "fox/FcnpFile.h"
#include "fox/FoxHash.h"
#include "fox/FmdlFile.h"
#include "fox/FoxMaterial.h"
#include "gl/GLModelWidget.h"

namespace glb {
namespace {

constexpr quint32 kGlbMagic = 0x46546C67;   // "glTF"
constexpr quint32 kChunkJson = 0x4E4F534A;  // "JSON"
constexpr quint32 kChunkBin = 0x004E4942;   // "BIN\0"

// glTF constants.
constexpr int kFloat = 5126, kUShort = 5123, kUByte = 5121;
constexpr int kArrayBuffer = 34962, kElementArrayBuffer = 34963;

struct BinBuilder {
    QByteArray data;
    QJsonArray bufferViews;
    QJsonArray accessors;

    void align(int n)
    {
        while (data.size() % n) data.append('\0');
    }

    // Add a buffer view + accessor over raw bytes; returns the accessor index.
    int accessor(const QByteArray& bytes, int componentType, const char* type,
                 int count, int target, const QJsonValue& min = {},
                 const QJsonValue& max = {}, bool normalized = false)
    {
        align(4);
        QJsonObject view;
        view[QStringLiteral("buffer")] = 0;
        view[QStringLiteral("byteOffset")] = data.size();
        view[QStringLiteral("byteLength")] = bytes.size();
        if (target) view[QStringLiteral("target")] = target;
        data += bytes;
        const int viewIdx = bufferViews.size();
        bufferViews.append(view);

        QJsonObject acc;
        acc[QStringLiteral("bufferView")] = viewIdx;
        acc[QStringLiteral("componentType")] = componentType;
        acc[QStringLiteral("type")] = QLatin1String(type);
        acc[QStringLiteral("count")] = count;
        if (!min.isNull()) acc[QStringLiteral("min")] = min;
        if (!max.isNull()) acc[QStringLiteral("max")] = max;
        if (normalized) acc[QStringLiteral("normalized")] = true;
        const int accIdx = accessors.size();
        accessors.append(acc);
        return accIdx;
    }

    // Embed a PNG; returns the buffer-view index.
    int imageView(const QByteArray& png)
    {
        align(4);
        QJsonObject view;
        view[QStringLiteral("buffer")] = 0;
        view[QStringLiteral("byteOffset")] = data.size();
        view[QStringLiteral("byteLength")] = png.size();
        data += png;
        bufferViews.append(view);
        return bufferViews.size() - 1;
    }
};

QByteArray floatsToBytes(const QVector<float>& v)
{
    QByteArray b(reinterpret_cast<const char*>(v.constData()),
                 static_cast<qsizetype>(v.size()) * 4);
    return b;
}

}  // namespace

namespace {

// Bake a pose into one mesh's positions/normals (CPU skin, palette-remapped).
void bakePose(const fox::FmdlMesh& mesh, const QVector<animmath::Mat4>& pose,
              QVector<float>* positions, QVector<float>* normals,
              QVector<float>* tangents)
{
    using animmath::Mat4;
    const int vertexCount = mesh.positions.size() / 3;
    *positions = mesh.positions;
    *normals = mesh.normals;
    *tangents = mesh.tangents;
    if (mesh.boneIndices.size() != vertexCount * 4
        || mesh.boneWeights.size() != vertexCount * 4 || mesh.palette.isEmpty())
        return;
    const bool haveN = normals->size() == vertexCount * 3;
    const bool haveT = tangents->size() == vertexCount * 4;
    for (int v = 0; v < vertexCount; ++v) {
        float B[4][3] = {};
        bool any = false;
        for (int k = 0; k < 4; ++k) {
            const float w = mesh.boneWeights[v * 4 + k];
            if (w <= 0.0f) continue;
            const int pal = mesh.boneIndices[v * 4 + k];
            if (pal >= mesh.palette.size()) continue;
            const int j = mesh.palette[pal];
            if (j < 0 || j >= pose.size()) continue;
            const Mat4& S = pose[j];
            for (int rr = 0; rr < 4; ++rr)
                for (int cc = 0; cc < 3; ++cc) B[rr][cc] += S.m[rr][cc] * w;
            any = true;
        }
        if (!any) continue;
        float* pp = positions->data() + v * 3;
        const float px = pp[0], py = pp[1], pz = pp[2];
        pp[0] = px * B[0][0] + py * B[1][0] + pz * B[2][0] + B[3][0];
        pp[1] = px * B[0][1] + py * B[1][1] + pz * B[2][1] + B[3][1];
        pp[2] = px * B[0][2] + py * B[1][2] + pz * B[2][2] + B[3][2];
        if (haveN) {
            float* np = normals->data() + v * 3;
            const float nx = np[0], ny = np[1], nz = np[2];
            np[0] = nx * B[0][0] + ny * B[1][0] + nz * B[2][0];
            np[1] = nx * B[0][1] + ny * B[1][1] + nz * B[2][1];
            np[2] = nx * B[0][2] + ny * B[1][2] + nz * B[2][2];
        }
        if (haveT) {
            // Tangents ride the same skin matrix as the normals; w is the
            // handedness flag and stays untouched.
            float* tp = tangents->data() + v * 4;
            const float tx = tp[0], ty = tp[1], tz = tp[2];
            tp[0] = tx * B[0][0] + ty * B[1][0] + tz * B[2][0];
            tp[1] = tx * B[0][1] + ty * B[1][1] + tz * B[2][1];
            tp[2] = tx * B[0][2] + ty * B[1][2] + tz * B[2][2];
        }
    }
}

// Shared accumulators for a (possibly multi-part) scene build.
struct SceneAccum {
    BinBuilder bin;
    QJsonArray meshesJson, materialsJson, texturesJson, imagesJson,
        samplersJson, skinsJson;
    QVector<QJsonObject> nodeObjs;
    QJsonArray sceneRoots;
    // How many maps the parts handed over, against how many distinct images
    // ended up embedded. The gap is what de-duplication saved.
    int mapsOffered = 0;
    // How many base maps had a runtime colour composited into them. Worth a
    // number: "0 baked" on a character wearing a chosen colour is the whole
    // symptom of the bug this exists to fix.
    int baked = 0;
    // Image de-duplication state. It lives HERE, on the scene, not in
    // appendModel: imagesJson accumulates across every part, so per-part state
    // would index a shared array with numbers from another part's numbering —
    // which is a crash on the second part, not merely a missed match. Keeping
    // it on the scene is also where the dedup earns most: the parts of one
    // character share a body, a face and a skin set between them.
    QVector<QImage> imageSource;                  // parallel to imagesJson
    QHash<quint64, QVector<int>> imageByContent;
    // ── Animations ──────────────────────────────────────────────────────────
    // Keyed by clip NAME so the same clip driving five parts of one character
    // lands in one glTF animation. Channels and samplers accumulate per name;
    // the JSON objects are assembled at the end, once, in insertion order.
    QVector<QString> animNames;
    QHash<QString, int> animByName;
    QVector<QJsonArray> animChannels, animSamplers;
    QHash<int, int> textureBySource;
};

}  // namespace

// Append one model (its materials, textures, skeleton/skin, meshes) to the
// scene accumulators. All JSON indices are global across parts.

// Sample an image with WRAP addressing at a normalised uv, nearest-neighbour.
// Nearest rather than bilinear on purpose: the two inputs here are a flat
// colour swatch and a mask, and a bilinear tap across a swatch's cell border
// invents a colour that is in neither cell.
inline QRgb sampleWrap(const QImage& img, double u, double v)
{
    const int w = img.width(), h = img.height();
    int x = int(std::floor(u * w)) % w;
    int y = int(std::floor(v * h)) % h;
    if (x < 0) x += w;
    if (y < 0) y += h;
    return img.pixel(x, y);
}

// Bake the runtime colour into the base map, by exactly the arithmetic the
// viewport shader runs (see the uLayerMode branch in GLModelWidget):
//
//   multiply  albedo *= mix(1, layer, mask)
//   blend     albedo  = mix(albedo, layer, mask)
//
// This is what makes a colourable garment export in its colour. Its base map
// SHIPS WHITE — the colour is a runtime multiply — so an export that copied
// the base map out verbatim was not losing the colour, it was faithfully
// exporting a white garment, which is worse because it looks deliberate.
//
// Done in the base map's own resolution and UV space. All three maps are
// sampled by the same uv0 in the shader, the layer through its tiling rate
// and the mask at 1:1, so a per-texel bake here lands where the shader's tap
// lands. The mask is the only one that must NOT tile: measured, it is a
// fitted per-model map saying where on this garment the colour applies.
QImage bakeLayerColour(const QImage& baseIn, const GLPbrMaterial& pm,
                       bool* didBake)
{
    if (didBake) *didBake = false;
    if (baseIn.isNull() || pm.layer.isNull() || pm.layerMask.isNull())
        return baseIn;
    if (!pm.layerMul && !pm.layerBlend) return baseIn;

    QImage base = baseIn.convertToFormat(QImage::Format_RGBA8888);
    const QImage layer = pm.layer.convertToFormat(QImage::Format_RGBA8888);
    const QImage mask = pm.layerMask.convertToFormat(QImage::Format_RGBA8888);
    if (base.isNull() || layer.isNull() || mask.isNull()) return baseIn;

    const double rw = pm.layerRepeat[0] != 0.0f ? double(pm.layerRepeat[0]) : 1.0;
    const double rh = pm.layerRepeat[1] != 0.0f ? double(pm.layerRepeat[1]) : 1.0;
    const double su = double(pm.layerShift[0]), sv = double(pm.layerShift[1]);
    const int W = base.width(), H = base.height();
    for (int y = 0; y < H; ++y) {
        uchar* row = base.scanLine(y);
        const double v = (y + 0.5) / H;
        for (int x = 0; x < W; ++x) {
            const double u = (x + 0.5) / W;
            // The mask is greyscale in every shipped file, so red is the whole
            // signal — the same read the shader makes.
            const double m = qRed(sampleWrap(mask, u, v)) / 255.0;
            if (m <= 0.0) continue;
            const QRgb lay = sampleWrap(layer, u * rw + su, v * rh + sv);
            const double lr = qRed(lay) / 255.0, lg = qGreen(lay) / 255.0,
                         lb = qBlue(lay) / 255.0;
            uchar* px = row + x * 4;
            const double br = px[0] / 255.0, bg = px[1] / 255.0,
                         bb = px[2] / 255.0;
            double r, g, b;
            if (pm.layerMul) {
                r = br * (1.0 + (lr - 1.0) * m);
                g = bg * (1.0 + (lg - 1.0) * m);
                b = bb * (1.0 + (lb - 1.0) * m);
            } else {
                r = br + (lr - br) * m;
                g = bg + (lg - bg) * m;
                b = bb + (lb - bb) * m;
            }
            px[0] = uchar(qBound(0.0, r, 1.0) * 255.0 + 0.5);
            px[1] = uchar(qBound(0.0, g, 1.0) * 255.0 + 0.5);
            px[2] = uchar(qBound(0.0, b, 1.0) * 255.0 + 0.5);
        }
    }
    if (didBake) *didBake = true;
    return base;
}

// Repack an SRM as the glTF "ORM" convention: occlusion in R, roughness in G,
// metalness in B. Fox's own layout is R = occlusion, G = roughness,
// B = reflection mask — measured, and the first two land exactly where glTF
// wants them. The third does NOT: the blue channel is a sparse reflectivity
// boost (61% of texels are exactly 0 and it never exceeds 0.63 anywhere in
// the shipped data), not a metalness map, and writing it into glTF's metal
// slot would tint the diffuse away on eyes and vehicle panels. So the SRM's
// own blue is discarded and metalness is carried by the material's scalar
// factor instead, derived from its FMTT preset as the viewport derives it.
//
// B is written as 255, NOT 0. glTF defines metalness as
// metallicFactor * texture.B, so a zero here would multiply the scalar away
// and every material carrying an SRM — which is to say every metal in the
// game — would export as a dielectric. 255 is the neutral that lets the
// factor through, exactly as roughnessFactor is set to 1.0 below for the
// same reason.
// `roughOnly` is the fallback case: a material that binds no SRM at all can
// bind a plain RoughnessMap instead, whose SINGLE channel is roughness and is
// not an occlusion map. Writing its red into glTF's occlusion slot would have
// shaded the whole surface by its own roughness, so occlusion is left at the
// neutral 255 and only the roughness channel is filled.
QImage buildOrm(const QImage& srmIn, bool roughOnly)
{
    if (srmIn.isNull()) return {};
    QImage orm = srmIn.convertToFormat(QImage::Format_RGBA8888);
    if (orm.isNull()) return {};
    for (int y = 0; y < orm.height(); ++y) {
        uchar* row = orm.scanLine(y);
        for (int x = 0; x < orm.width(); ++x) {
            uchar* px = row + x * 4;
            if (roughOnly) {
                px[1] = px[0];   // roughness lives in the lone channel
                px[0] = 255;     // no occlusion to state
            }
            px[2] = 255;    // neutral multiplier — see above
            px[3] = 255;    // opaque, so no viewer treats it as alpha
        }
    }
    return orm;
}

static bool appendModel(SceneAccum& S, const fox::FmdlFile& model,
                        const QVector<QImage>& textures,
                        const QVector<QImage>& normalMaps,
                        const QVector<animmath::Mat4>* pose,
                        const animmath::Mat4* rigid, quint32 subtreeOnly,
                        const QSet<int>& hiddenGroups,
                        const QSet<int>& hiddenMeshes,
                        const QVector<GLPbrMaterial>* pbrSet,
                        const QVector<fox::ConnectPoint>* connectPoints,
                        const SceneOptions& opts, QString* error)
{
    // Empty unless a subtree filter is asked for AND this model has that bone.
    const QVector<bool> subtree =
        subtreeOnly ? model.boneSubtreeMask(subtreeOnly) : QVector<bool>();
    const bool posed =
        (pose && pose->size() == model.bones().size()) || rigid != nullptr;
    const auto fail = [&](const QString& why) {
        if (error) *error = why;
        return false;
    };
    if (model.meshes().isEmpty()) return fail(QStringLiteral("model has no meshes"));

    BinBuilder& bin = S.bin;
    QJsonArray& meshesJson = S.meshesJson;
    QJsonArray& materialsJson = S.materialsJson;
    QJsonArray& texturesJson = S.texturesJson;
    QJsonArray& imagesJson = S.imagesJson;
    QJsonArray& skinsJson = S.skinsJson;
    QVector<QJsonObject>& nodeObjs = S.nodeObjs;

    // A baked pose exports as a static snapshot: vertices pre-skinned, no
    // skeleton/skin (a bind-pose armature under a posed mesh would mislead).
    // …and not when the caller asked for no rig at all.
    const bool skinned = !model.bones().isEmpty() && !posed && opts.skeleton;
    const int nodeBase = nodeObjs.size();   // this part's first node index

    // ── Nodes: bone hierarchy first (node index == nodeBase + bone index) ───
    if (skinned) {
        for (int b = 0; b < model.bones().size(); ++b) {
            const fox::FmdlBone& bone = model.bones()[b];
            QJsonObject n;
            n[QStringLiteral("name")] = bone.name;
            n[QStringLiteral("translation")] = QJsonArray{
                bone.localPos[0], bone.localPos[1], bone.localPos[2]};
            nodeObjs.append(n);
        }
        for (int b = 0; b < model.bones().size(); ++b) {
            const int parent = model.bones()[b].parentIndex;
            if (parent < 0 || parent >= model.bones().size()) continue;
            QJsonArray kids =
                nodeObjs[nodeBase + parent][QStringLiteral("children")].toArray();
            kids.append(nodeBase + b);
            nodeObjs[nodeBase + parent][QStringLiteral("children")] = kids;
        }
    }

    // ── Skin: inverse bind matrices = translate(-world) (bind pose is
    // translation-only in FMDL) ─────────────────────────────────────────────
    int skinIdx = -1;
    if (skinned) {
        QVector<float> ibm;
        ibm.reserve(model.bones().size() * 16);
        for (const fox::FmdlBone& bone : model.bones()) {
            const float m[16] = {
                1, 0, 0, 0,
                0, 1, 0, 0,
                0, 0, 1, 0,
                -bone.worldPos[0], -bone.worldPos[1], -bone.worldPos[2], 1,
            };
            for (float v : m) ibm.append(v);
        }
        const int ibmAcc = bin.accessor(floatsToBytes(ibm), kFloat, "MAT4",
                                        model.bones().size(), 0);
        QJsonObject skin;
        skin[QStringLiteral("inverseBindMatrices")] = ibmAcc;
        QJsonArray joints;
        for (int b = 0; b < model.bones().size(); ++b)
            joints.append(nodeBase + b);
        skin[QStringLiteral("joints")] = joints;
        skinIdx = skinsJson.size();
        skinsJson.append(skin);
    }

    // ── Materials + embedded textures ────────────────────────────────────────
    // Materials SHARE maps: a character's eighteen materials commonly draw on
    // five base textures, and the same normal atlas covers many of them. Every
    // one of those used to be PNG-encoded again and embedded again, so a
    // Venom export carried thirteen redundant copies of images it already
    // held — slower to write and larger to open.
    //
    // Images are de-duplicated by CONTENT, not by QImage identity: two decodes
    // of one .ftex are different objects, and the normal-map unswizzle builds a
    // fresh image per material. The hash covers only the meaningful bytes of
    // each scanline (a QImage row can carry padding, which is not part of the
    // picture), and a hash hit is confirmed with a real comparison before
    // anything is shared, so a collision can cost a wasted compare and never a
    // wrong texture.
    const auto contentHash = [](const QImage& img) -> quint64 {
        quint64 h = 1469598103934665603ULL;       // FNV-1a, 64-bit
        const auto mix = [&h](const uchar* p, qsizetype n) {
            for (qsizetype i = 0; i < n; ++i) {
                h ^= p[i];
                h *= 1099511628211ULL;
            }
        };
        const quint32 head[3] = {quint32(img.width()), quint32(img.height()),
                                 quint32(img.format())};
        mix(reinterpret_cast<const uchar*>(head), sizeof(head));
        // Rounded UP: a 1-bit or otherwise sub-byte format carries picture
        // data in a final partial byte that a truncating divide would leave
        // out of the hash. It never over-reads — this is always <= the real
        // bytesPerLine(), the rest of which is row padding and not part of the
        // picture. Nothing here produces such a format today; getting it wrong
        // would only cost a redundant compare, but the rule should be right.
        const qsizetype row =
            (qsizetype(img.width()) * img.depth() + 7) / 8;
        for (int y = 0; y < img.height(); ++y) mix(img.constScanLine(y), row);
        return h;
    };

    const auto emitImage = [&](const QImage& img) -> int {
        const quint64 h = contentHash(img);
        const auto bucket = S.imageByContent.constFind(h);
        if (bucket != S.imageByContent.constEnd())
            for (const int idx : *bucket)
                if (S.imageSource[idx] == img) return idx;
        QByteArray png;
        {
            QBuffer buf(&png);
            buf.open(QIODevice::WriteOnly);
            img.save(&buf, "PNG");
        }
        QJsonObject o;
        o[QStringLiteral("bufferView")] = bin.imageView(png);
        o[QStringLiteral("mimeType")] = QStringLiteral("image/png");
        imagesJson.append(o);
        const int idx = int(imagesJson.size()) - 1;
        S.imageSource.append(img);
        S.imageByContent[h].append(idx);
        return idx;
    };

    // One texture per image: there is a single sampler, so two textures over
    // the same image would be identical objects.
    const auto emitTexture = [&](const QImage& img) -> int {
        const int src = emitImage(img);
        const auto it = S.textureBySource.constFind(src);
        if (it != S.textureBySource.constEnd()) return *it;
        QJsonObject tex;
        tex[QStringLiteral("sampler")] = 0;
        tex[QStringLiteral("source")] = src;
        texturesJson.append(tex);
        const int idx = int(texturesJson.size()) - 1;
        S.textureBySource.insert(src, idx);
        return idx;
    };

    // Which materials a mesh that SURVIVES the filters actually uses. Built
    // first, because the material loop below embeds a PNG per map and doing
    // that for geometry nobody asked for is the whole cost of the file: hiding
    // eight of a character's ten groups still shipped all ten groups' images.
    // The subtree filter is deliberately NOT consulted here — it removes
    // triangles rather than meshes, and a mesh it empties entirely is rare
    // enough that carrying its material is cheaper than walking every vertex
    // twice to find out.
    QSet<int> usedMaterials;
    for (int k = 0; k < model.meshes().size(); ++k) {
        const fox::FmdlMesh& me = model.meshes()[k];
        if (me.positions.isEmpty() || me.triangles.isEmpty()) continue;
        if (!hiddenGroups.isEmpty() && hiddenGroups.contains(me.meshGroupIndex))
            continue;
        if (hiddenMeshes.contains(k)) continue;
        if (me.materialInstanceIndex >= 0)
            usedMaterials.insert(me.materialInstanceIndex);
    }

    QVector<int> materialGlbIdx(model.materials().size(), -1);
    for (int mi = 0; mi < model.materials().size(); ++mi) {
        if (!usedMaterials.contains(mi)) continue;
        QJsonObject mat;
        mat[QStringLiteral("name")] = model.materials()[mi].name;
        mat[QStringLiteral("doubleSided")] = true;
        QJsonObject pbr;
        pbr[QStringLiteral("metallicFactor")] = 0.0;
        pbr[QStringLiteral("roughnessFactor")] = 0.8;
        const GLPbrMaterial* pm =
            (pbrSet && mi < pbrSet->size()) ? &(*pbrSet)[mi] : nullptr;
        if (mi < textures.size() && !textures[mi].isNull()) {
            ++S.mapsOffered;
            // The COLOUR goes in here, not in a factor: a gear colour applies
            // through a mask, so there is no single baseColorFactor that could
            // stand for it.
            bool didBake = false;
            const QImage baked = (pm && opts.bakeColourLayer)
                ? bakeLayerColour(textures[mi], *pm, &didBake)
                : textures[mi];
            if (didBake) ++S.baked;
            QJsonObject texRef;
            texRef[QStringLiteral("index")] = emitTexture(baked);
            pbr[QStringLiteral("baseColorTexture")] = texRef;
            // Alpha-carrying maps (hair, lashes, decals) cut out at 0.5.
            if (textures[mi].hasAlphaChannel()) {
                mat[QStringLiteral("alphaMode")] = QStringLiteral("MASK");
                mat[QStringLiteral("alphaCutoff")] = 0.35;
            }
        }
        // Occlusion and roughness, from the SRM. Both glTF slots read from one
        // image in the ORM convention, and the de-duplicator below then emits
        // that image ONCE however many slots point at it.
        //
        // The two factors are set to the neutral 1.0 alongside, because glTF
        // MULTIPLIES factor by texture: leaving roughnessFactor at the 0.8 this
        // exporter uses when there is no map would have darkened every
        // roughness value it had just written by a fifth.
        if (pm && !pm->material.isNull()) {
            const QImage orm = buildOrm(
                pm->material, pm->materialRole == fox::texrole::kRoughness);
            if (!orm.isNull()) {
                ++S.mapsOffered;
                const int ti = emitTexture(orm);
                QJsonObject occ;
                occ[QStringLiteral("index")] = ti;
                mat[QStringLiteral("occlusionTexture")] = occ;
                QJsonObject mr;
                mr[QStringLiteral("index")] = ti;
                pbr[QStringLiteral("metallicRoughnessTexture")] = mr;
                pbr[QStringLiteral("roughnessFactor")] = 1.0;
            }
        }
        // Metalness as a SCALAR, derived the way the shader derives it: from
        // the FMTT preset's F0, eased rather than stepped, and forced to zero
        // for the families that can never be metal whatever preset they name
        // (skin, hair, cloth, eyes — see GLPbrMaterial::noMetal). Region 0
        // only: glTF has one metalness per material and no equivalent of the
        // MTM's four regions, so a multi-region material exports its first.
        if (pm) {
            const float f0 = pm->presetF0[0];
            const float t = qBound(0.0f, (f0 - 0.25f) / 0.35f, 1.0f);
            pbr[QStringLiteral("metallicFactor")] =
                pm->noMetal ? 0.0 : double(t * t * (3.0f - 2.0f * t));
        }
        if (opts.normalMaps && mi < normalMaps.size()
            && !normalMaps[mi].isNull()) {
            // Already a plain RGB normal map: modelload unswizzles Fox's
            // DXT5nm form at load, so viewport and export share one image.
            ++S.mapsOffered;
            QJsonObject texRef;
            texRef[QStringLiteral("index")] = emitTexture(normalMaps[mi]);
            mat[QStringLiteral("normalTexture")] = texRef;
        }
        mat[QStringLiteral("pbrMetallicRoughness")] = pbr;
        materialGlbIdx[mi] = materialsJson.size();
        materialsJson.append(mat);
    }

    // ── Meshes ───────────────────────────────────────────────────────────────
    QJsonArray& sceneRoots = S.sceneRoots;
    if (skinned)
        for (int b = 0; b < model.bones().size(); ++b)
            if (model.bones()[b].parentIndex < 0)
                sceneRoots.append(nodeBase + b);

    // ── Connect points ───────────────────────────────────────────────────────
    // A .fcnp point is authored in its parent BONE'S BIND FRAME, and an FMDL
    // bind pose is translation-only — so the point's transform relative to that
    // bone's node IS the record, unchanged: translation, rotation, scale
    // straight through. That is why this is a dozen lines and not a solver.
    //
    // Rigged, the point becomes a child of the bone's node and therefore
    // follows every animation written above. Static (a baked pose, or an
    // attachment's rigid seat), there are no bone nodes, so the point is
    // written at the scene root with the bone's own posed frame folded in —
    // recovered from the skin matrix, which is translate(-bindWorld) times
    // that frame.
    if (opts.connectPoints && connectPoints && !connectPoints->isEmpty()) {
        QHash<quint32, int> boneByHash;
        for (int b = 0; b < model.bones().size(); ++b)
            if (!boneByHash.contains(model.bones()[b].nameHash32()))
                boneByHash.insert(model.bones()[b].nameHash32(), b);
        int written = 0;
        for (const fox::ConnectPoint& cp : *connectPoints) {
            int bone = -1;
            if (!cp.parentBone.isEmpty()) {
                const quint32 want = static_cast<quint32>(
                    fox::hashFileNameLegacy(cp.parentBone, false) & 0xFFFFFFFFu);
                bone = boneByHash.value(want, -1);
            }
            QJsonObject n;
            n[QStringLiteral("name")] = cp.name;
            if (skinned) {
                n[QStringLiteral("translation")] =
                    QJsonArray{cp.pos[0], cp.pos[1], cp.pos[2]};
                n[QStringLiteral("rotation")] =
                    QJsonArray{cp.quat[0], cp.quat[1], cp.quat[2], cp.quat[3]};
                if (cp.scale[0] != 1.0f || cp.scale[1] != 1.0f
                    || cp.scale[2] != 1.0f)
                    n[QStringLiteral("scale")] =
                        QJsonArray{cp.scale[0], cp.scale[1], cp.scale[2]};
                nodeObjs.append(n);
                const int idx = nodeObjs.size() - 1;
                if (bone >= 0) {
                    QJsonArray kids =
                        nodeObjs[nodeBase + bone][QStringLiteral("children")]
                            .toArray();
                    kids.append(idx);
                    nodeObjs[nodeBase + bone][QStringLiteral("children")] = kids;
                } else {
                    // No parent bone named, or the model does not carry it:
                    // the point still belongs in the file, at the model's own
                    // origin, rather than being silently dropped.
                    sceneRoots.append(idx);
                }
            } else {
                using animmath::Mat4;
                using animmath::Vec3;
                fox::Quat q;
                q.x = cp.quat[0];
                q.y = cp.quat[1];
                q.z = cp.quat[2];
                q.w = cp.quat[3];
                // Scale FIRST, then rotate, then translate — the same order
                // glTF composes T·R·S, and the same order the rigged branch
                // gets for free by writing the three fields separately. The
                // first version of this branch dropped the scale entirely, so
                // one asset's socket came out the right size unposed and unit
                // size posed.
                Mat4 sc;
                sc.m[0][0] = cp.scale[0];
                sc.m[1][1] = cp.scale[1];
                sc.m[2][2] = cp.scale[2];
                Mat4 m = animmath::mul(
                    animmath::mul(sc, Mat4::fromQuat(q)),
                    Mat4::translation(Vec3(cp.pos[0], cp.pos[1], cp.pos[2])));
                if (bone >= 0) {
                    const fox::FmdlBone& b = model.bones()[bone];
                    const Mat4 bindT = Mat4::translation(
                        Vec3(b.worldPos[0], b.worldPos[1], b.worldPos[2]));
                    m = animmath::mul(
                        m, pose && bone < pose->size()
                               ? animmath::mul(bindT, (*pose)[bone])
                               : bindT);
                }
                if (rigid) m = animmath::mul(m, *rigid);
                // A row-major row-vector matrix serialises to glTF's
                // column-major array element for element: the first four
                // numbers are its first ROW, which is the first COLUMN of the
                // transpose glTF wants.
                QJsonArray mat;
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c) mat.append(double(m.m[r][c]));
                n[QStringLiteral("matrix")] = mat;
                nodeObjs.append(n);
                sceneRoots.append(nodeObjs.size() - 1);
            }
            ++written;
        }
        qInfo("glb: %d connect point(s) written as empties", written);
    }

    for (int mi = 0; mi < model.meshes().size(); ++mi) {
        const fox::FmdlMesh& mesh = model.meshes()[mi];
        const int vertexCount = mesh.positions.size() / 3;
        if (vertexCount == 0 || mesh.triangles.isEmpty()) continue;
        // Whatever the viewport erased, before anything is written. This is
        // the group a .fv2 variation hides (the baked head under an attached
        // one, a bandanna a hat replaces) and the group the user unticked.
        if (!hiddenGroups.isEmpty() && hiddenGroups.contains(mesh.meshGroupIndex))
            continue;
        // …and the individual meshes the submesh tree switched off.
        if (hiddenMeshes.contains(mi)) continue;

        // Subtree filter FIRST, before a single byte is written. Keep a
        // triangle when ANY of its corners is skinned (heaviest bone) into the
        // subtree — the same rule the viewport applies in
        // modelload::keepBoneSubtree, so the neck seam is cut in the same
        // place. Deciding here rather than after the vertex accessors are
        // built is what stops a fully-culled mesh from leaving its entire
        // vertex, joint and weight payload orphaned in the file.
        QVector<quint16> tris = mesh.triangles;
        if (!subtree.isEmpty()) {
            const bool skinnable =
                mesh.boneIndices.size() == vertexCount * 4
                && mesh.boneWeights.size() == vertexCount * 4
                && !mesh.palette.isEmpty();
            QVector<bool> keepVert(vertexCount, false);
            if (skinnable)
                for (int v = 0; v < vertexCount; ++v) {
                    int best = 0;
                    float bestW = -1.0f;
                    for (int k = 0; k < 4; ++k)
                        if (mesh.boneWeights[v * 4 + k] > bestW) {
                            bestW = mesh.boneWeights[v * 4 + k];
                            const int pal = mesh.boneIndices[v * 4 + k];
                            // Out-of-range palette entry resolves to bone 0,
                            // which is exactly what buildUploads() writes into
                            // the viewport's joints — the two rules have to
                            // agree here or an export can differ from what is
                            // on screen.
                            best = pal < mesh.palette.size() ? mesh.palette[pal] : 0;
                        }
                    keepVert[v] = best < subtree.size() && subtree[best];
                }
            QVector<quint16> kept;
            kept.reserve(tris.size());
            for (int t = 0; t + 2 < tris.size(); t += 3)
                if (keepVert.value(tris[t]) || keepVert.value(tris[t + 1])
                    || keepVert.value(tris[t + 2])) {
                    kept.append(tris[t]);
                    kept.append(tris[t + 1]);
                    kept.append(tris[t + 2]);
                }
            tris = std::move(kept);
            if (tris.isEmpty()) continue;   // nothing of this mesh survives
        }

        // Posed export: bake the current frame into the vertex data; a rigid
        // attachment transform applies to every vertex on top.
        QVector<float> bakedPos, bakedNrm, bakedTan;
        if (posed) {
            if (pose && pose->size() == model.bones().size()) {
                bakePose(mesh, *pose, &bakedPos, &bakedNrm, &bakedTan);
            } else {
                bakedPos = mesh.positions;
                bakedNrm = mesh.normals;
                bakedTan = mesh.tangents;
            }
            if (rigid) {
                using animmath::transform;
                using animmath::transformDir;
                using animmath::Vec3;
                for (int v = 0; v + 2 < bakedPos.size(); v += 3) {
                    const Vec3 p = transform(
                        Vec3(bakedPos[v], bakedPos[v + 1], bakedPos[v + 2]),
                        *rigid);
                    bakedPos[v] = p.x;
                    bakedPos[v + 1] = p.y;
                    bakedPos[v + 2] = p.z;
                }
                for (int v = 0; v + 2 < bakedNrm.size(); v += 3) {
                    const Vec3 n = transformDir(
                        Vec3(bakedNrm[v], bakedNrm[v + 1], bakedNrm[v + 2]),
                        *rigid);
                    bakedNrm[v] = n.x;
                    bakedNrm[v + 1] = n.y;
                    bakedNrm[v + 2] = n.z;
                }
                for (int v = 0; v + 3 < bakedTan.size(); v += 4) {
                    const Vec3 t = transformDir(
                        Vec3(bakedTan[v], bakedTan[v + 1], bakedTan[v + 2]),
                        *rigid);
                    bakedTan[v] = t.x;
                    bakedTan[v + 1] = t.y;
                    bakedTan[v + 2] = t.z;
                }
            }
        }
        const QVector<float>& srcPos = posed ? bakedPos : mesh.positions;
        const QVector<float>& srcNrm = posed ? bakedNrm : mesh.normals;
        const QVector<float>& srcTan = posed ? bakedTan : mesh.tangents;

        // POSITION with min/max (required by the spec).
        float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f};
        for (int v = 0; v < vertexCount; ++v)
            for (int k = 0; k < 3; ++k) {
                const float p = srcPos[v * 3 + k];
                if (p < mn[k]) mn[k] = p;
                if (p > mx[k]) mx[k] = p;
            }
        const int posAcc = bin.accessor(
            floatsToBytes(srcPos), kFloat, "VEC3", vertexCount,
            kArrayBuffer, QJsonArray{mn[0], mn[1], mn[2]},
            QJsonArray{mx[0], mx[1], mx[2]});

        QJsonObject attrs;
        attrs[QStringLiteral("POSITION")] = posAcc;

        if (srcNrm.size() == vertexCount * 3) {
            // Re-normalize: FMDL halves are close to unit but the spec requires it.
            QVector<float> n = srcNrm;
            for (int v = 0; v < vertexCount; ++v) {
                float* p = n.data() + v * 3;
                const float len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
                if (len > 1e-6f) {
                    p[0] /= len;
                    p[1] /= len;
                    p[2] /= len;
                } else {
                    p[1] = 1.0f;
                }
            }
            attrs[QStringLiteral("NORMAL")] = bin.accessor(
                floatsToBytes(n), kFloat, "VEC3", vertexCount, kArrayBuffer);
        }
        if (mesh.uv0.size() == vertexCount * 2)
            attrs[QStringLiteral("TEXCOORD_0")] = bin.accessor(
                floatsToBytes(mesh.uv0), kFloat, "VEC2", vertexCount, kArrayBuffer);
        // TANGENT (vec4, w = bitangent sign) — required by glTF for a
        // normalTexture to be applied the way the engine intended; without it
        // importers guess tangents from the UVs and mirrored islands flip.
        // …and only when a normal map is going with them. TANGENT exists for
        // the normal map's benefit; writing sixteen bytes a vertex of it into
        // a file that has no normal texture is the one thing the "Normal
        // maps" switch is asked to save.
        if (opts.normalMaps && srcTan.size() == vertexCount * 4) {
            QVector<float> t = srcTan;
            for (int v = 0; v < vertexCount; ++v) {
                float* p = t.data() + v * 4;
                const float len =
                    std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
                if (len > 1e-6f) { p[0] /= len; p[1] /= len; p[2] /= len; }
                else { p[0] = 1.0f; p[1] = 0.0f; p[2] = 0.0f; }
                p[3] = p[3] < 0.0f ? -1.0f : 1.0f;   // spec: exactly ±1
            }
            attrs[QStringLiteral("TANGENT")] = bin.accessor(
                floatsToBytes(t), kFloat, "VEC4", vertexCount, kArrayBuffer);
        }

        const bool meshSkinned = skinned
            && mesh.boneIndices.size() == vertexCount * 4
            && mesh.boneWeights.size() == vertexCount * 4
            && !mesh.palette.isEmpty();
        if (meshSkinned) {
            // Palette-relative indices → global joint indices. u8 joints when
            // the skeleton fits, u16 otherwise (some scene fmdl exceed 255).
            const bool wideJoints = model.bones().size() > 255;
            QByteArray joints;
            joints.resize(vertexCount * 4 * (wideJoints ? 2 : 1));
            QVector<float> weights(vertexCount * 4);
            for (int v = 0; v < vertexCount; ++v) {
                float sum = 0;
                for (int k = 0; k < 4; ++k) sum += mesh.boneWeights[v * 4 + k];
                for (int k = 0; k < 4; ++k) {
                    const int pal = mesh.boneIndices[v * 4 + k];
                    const float w = mesh.boneWeights[v * 4 + k];
                    quint16 joint = 0;
                    if (w > 0 && pal < mesh.palette.size())
                        joint = mesh.palette[pal];
                    if (wideJoints)
                        qToLittleEndian(joint, joints.data() + (v * 4 + k) * 2);
                    else
                        joints[v * 4 + k] = static_cast<char>(
                            static_cast<quint8>(qMin<int>(joint, 255)));
                    weights[v * 4 + k] = sum > 1e-6f ? w / sum : (k == 0 ? 1.0f : 0.0f);
                }
            }
            attrs[QStringLiteral("JOINTS_0")] = bin.accessor(
                joints, wideJoints ? kUShort : kUByte, "VEC4", vertexCount,
                kArrayBuffer);
            attrs[QStringLiteral("WEIGHTS_0")] = bin.accessor(
                floatsToBytes(weights), kFloat, "VEC4", vertexCount, kArrayBuffer);
        }

        QByteArray idx(reinterpret_cast<const char*>(tris.constData()),
                       static_cast<qsizetype>(tris.size()) * 2);
        const int idxAcc = bin.accessor(idx, kUShort, "SCALAR",
                                        tris.size(), kElementArrayBuffer);

        QJsonObject prim;
        prim[QStringLiteral("attributes")] = attrs;
        prim[QStringLiteral("indices")] = idxAcc;
        // The >= 0 test on the RESOLVED index matters as much as the one on
        // the source index: a material the used-set above skipped stays -1,
        // and "material": -1 is not valid glTF. By construction a mesh that
        // reaches here is in the used set, so this is a belt-and-braces check
        // rather than a live path — but the failure it guards is a file every
        // viewer rejects, which is not a thing to leave to construction.
        if (mesh.materialInstanceIndex >= 0
            && mesh.materialInstanceIndex < materialGlbIdx.size()
            && materialGlbIdx[mesh.materialInstanceIndex] >= 0)
            prim[QStringLiteral("material")] = materialGlbIdx[mesh.materialInstanceIndex];

        QJsonObject meshJson;
        const QString groupName =
            mesh.meshGroupIndex >= 0 && mesh.meshGroupIndex < model.meshGroups().size()
                ? model.meshGroups()[mesh.meshGroupIndex].name
                : QString();
        meshJson[QStringLiteral("name")] =
            groupName.isEmpty() ? QStringLiteral("mesh_%1").arg(mi)
                                : QStringLiteral("%1_%2").arg(groupName).arg(mi);
        meshJson[QStringLiteral("primitives")] = QJsonArray{prim};
        meshesJson.append(meshJson);

        QJsonObject meshNode;
        meshNode[QStringLiteral("name")] = meshJson[QStringLiteral("name")];
        meshNode[QStringLiteral("mesh")] = meshesJson.size() - 1;
        if (meshSkinned) meshNode[QStringLiteral("skin")] = skinIdx;
        nodeObjs.append(meshNode);
        sceneRoots.append(nodeObjs.size() - 1);
    }
    return true;
}

// Write the scene's clips as glTF animations over its skinned parts.
//
// glTF animates NODE transforms, so the world matrices the solver produces have
// to come back down to node-local: local = world · inverse(parentWorld). The
// pose solver only ever produces rotation+translation, which is why the inverse
// here is the rigid one and why there is no scale channel — a scale track of
// all ones on 300 bones is bytes that say nothing.
//
// Tracks that never leave the bone's REST transform are dropped. On a character
// that is most of the skeleton — a walk drives the legs, spine and arms and
// leaves the fingers, the face rig and every helper exactly where the bind pose
// put them — and keeping them turned a 400 KB clip into 3 MB of constants.
//
// Sampling order is sample-major (every part of one frame, then the next
// frame), because posing a composed scene is a whole-scene operation: the
// alternative order made the caller re-pose the character once per part per
// frame for exactly the same answer.
struct AnimPartTarget {
    const fox::FmdlFile* model = nullptr;
    int nodeBase = 0;
};

static void appendAnimations(SceneAccum& S,
                             const QVector<AnimPartTarget>& targets,
                             const QVector<int>& partIndex,
                             const QVector<GlbAnimation>& clips)
{
    using animmath::Mat4;
    if (targets.isEmpty()) return;

    for (const GlbAnimation& clip : clips) {
        if (clip.sampleCount <= 0 || !clip.pose) continue;
        const int n = clip.sampleCount;
        const float fps = clip.fps > 0.0f ? clip.fps : 30.0f;

        // Per-part, per-bone curves, filled sample by sample.
        QVector<QVector<QVector<float>>> rot(targets.size()), pos(targets.size());
        for (int t = 0; t < targets.size(); ++t) {
            const int nb = targets[t].model->bones().size();
            rot[t].resize(nb);
            pos[t].resize(nb);
            for (int b = 0; b < nb; ++b) {
                rot[t][b].reserve(n * 4);
                pos[t][b].reserve(n * 3);
            }
        }
        QVector<Mat4> world;
        bool sized = true;
        for (int sIdx = 0; sIdx < n && sized; ++sIdx) {
            for (int t = 0; t < targets.size(); ++t) {
                const fox::FmdlFile& model = *targets[t].model;
                const int nb = model.bones().size();
                clip.pose(partIndex[t], sIdx, &world);
                if (world.size() != nb) {   // a pose for another skeleton
                    sized = false;
                    break;
                }
                for (int b = 0; b < nb; ++b) {
                    const int parent = model.bones()[b].parentIndex;
                    const Mat4 local = parent >= 0 && parent < nb
                        ? animmath::mul(world[b],
                                        animmath::invertRigid(world[parent]))
                        : world[b];
                    const fox::Quat q = animmath::quatFromMatrix(local);
                    const animmath::Vec3 tr = local.translationRow();
                    rot[t][b] << q.x << q.y << q.z << q.w;
                    pos[t][b] << tr.x << tr.y << tr.z;
                }
            }
        }
        if (!sized) {
            qWarning("glb: clip '%s' poses a different skeleton than the scene "
                     "part it was given — not written",
                     qUtf8Printable(clip.name));
            continue;
        }

        // One time accessor for the whole clip, shared by every sampler in it.
        QVector<float> times(n);
        for (int i = 0; i < n; ++i) times[i] = static_cast<float>(i) / fps;
        const int timeAcc = S.bin.accessor(
            floatsToBytes(times), kFloat, "SCALAR", n, 0, QJsonArray{0.0},
            QJsonArray{double(times.isEmpty() ? 0.0f : times.back())});

        int slot = S.animByName.value(clip.name, -1);
        if (slot < 0) {
            slot = S.animNames.size();
            S.animNames.append(clip.name);
            S.animByName.insert(clip.name, slot);
            S.animChannels.append(QJsonArray());
            S.animSamplers.append(QJsonArray());
        }
        QJsonArray& channels = S.animChannels[slot];
        QJsonArray& samplers = S.animSamplers[slot];

        const auto addTrack = [&](int node, const QVector<float>& data,
                                  const char* type, const char* path) {
            const int outAcc =
                S.bin.accessor(floatsToBytes(data), kFloat, type, n, 0);
            QJsonObject smp;
            smp[QStringLiteral("input")] = timeAcc;
            smp[QStringLiteral("output")] = outAcc;
            smp[QStringLiteral("interpolation")] = QStringLiteral("LINEAR");
            samplers.append(smp);
            QJsonObject target;
            target[QStringLiteral("node")] = node;
            target[QStringLiteral("path")] = QLatin1String(path);
            QJsonObject ch;
            ch[QStringLiteral("sampler")] = samplers.size() - 1;
            ch[QStringLiteral("target")] = target;
            channels.append(ch);
        };

        constexpr float kEps = 1e-6f;
        int tracks = 0, bones = 0;
        for (int t = 0; t < targets.size(); ++t) {
            const fox::FmdlFile& model = *targets[t].model;
            const int nb = model.bones().size();
            bones += nb;
            for (int b = 0; b < nb; ++b) {
                const auto& bone = model.bones()[b];
                bool wantRot = false, wantPos = false;
                for (int i = 0; i < n && !wantRot; ++i) {
                    const float* q = rot[t][b].constData() + i * 4;
                    // Sign-agnostic: -q is the same rotation, and a solver that
                    // flips the sign mid-clip would otherwise "vary".
                    if (std::fabs(std::fabs(q[3]) - 1.0f) > kEps
                        || std::fabs(q[0]) > kEps || std::fabs(q[1]) > kEps
                        || std::fabs(q[2]) > kEps)
                        wantRot = true;
                }
                for (int i = 0; i < n && !wantPos; ++i) {
                    const float* tr = pos[t][b].constData() + i * 3;
                    if (std::fabs(tr[0] - bone.localPos[0]) > kEps
                        || std::fabs(tr[1] - bone.localPos[1]) > kEps
                        || std::fabs(tr[2] - bone.localPos[2]) > kEps)
                        wantPos = true;
                }
                if (wantRot) {
                    addTrack(targets[t].nodeBase + b, rot[t][b], "VEC4",
                             "rotation");
                    ++tracks;
                }
                if (wantPos) {
                    addTrack(targets[t].nodeBase + b, pos[t][b], "VEC3",
                             "translation");
                    ++tracks;
                }
            }
        }
        qInfo("glb: clip '%s' — %d sample(s) at %g fps, %d track(s) over %d "
              "bone(s) in %lld part(s)",
              qUtf8Printable(clip.name), n, double(fps), tracks, bones,
              static_cast<long long>(targets.size()));
    }
}

bool exportGlb(const fox::FmdlFile& model, const QVector<QImage>& textures,
               const QString& outPath, QString* error,
               const QVector<animmath::Mat4>* pose,
               const QVector<QImage>* normalMaps, const SceneOptions& opts)
{
    ScenePart part;
    part.model = &model;
    part.textures = &textures;
    part.normalMaps = normalMaps;
    part.pose = pose;
    return exportGlbScene({part}, outPath, error, opts);
}

bool exportGlbScene(const QVector<ScenePart>& parts, const QString& outPath,
                    QString* error, const SceneOptions& opts,
                    const QVector<GlbAnimation>* animations,
                    int* animationsWritten)
{
    if (animationsWritten) *animationsWritten = 0;
    QElapsedTimer exportTimer;
    exportTimer.start();
    const auto fail = [&](const QString& why) {
        if (error) *error = why;
        return false;
    };
    if (parts.isEmpty()) return fail(QStringLiteral("nothing to export"));

    SceneAccum S;
    QJsonObject sampler;   // one default sampler: linear, repeat
    S.samplersJson.append(sampler);

    const QVector<QImage> noTextures;
    // Which parts a clip can actually drive, and where their joints landed.
    // Collected during the append because nothing afterwards can recover a
    // part's node base once the next part has appended its own.
    QVector<AnimPartTarget> animTargets;
    QVector<int> animPartIndex;   // back into `parts`, for the pose callback
    int partIdx = 0, staticParts = 0;
    for (const ScenePart& p : parts) {
        if (!p.model) return fail(QStringLiteral("null part"));
        // A pose sized to a different skeleton is ignored (bind export).
        const QVector<animmath::Mat4>* pose =
            p.pose && p.pose->size() == p.model->bones().size() ? p.pose
                                                                : nullptr;
        // Where this part's joint nodes will start, recorded BEFORE the
        // append: the animation channels address nodes by absolute index and
        // there is no way to recover the base afterwards once another part has
        // appended its own.
        const int nodeBase = S.nodeObjs.size();
        const int rootsBefore = S.sceneRoots.size();
        if (!appendModel(S, *p.model, p.textures ? *p.textures : noTextures,
                         p.normalMaps ? *p.normalMaps : noTextures,
                         pose, p.rigid, p.subtreeOnly, p.hiddenGroups,
                         p.hiddenMeshes, p.pbr, p.connectPoints, opts, error))
            return false;
        // ── The part's rest-pose offset, as a wrapper node ───────────────
        // Everything this part just contributed to the scene becomes the
        // child of one translated node. Joints included: that is the whole
        // point, because a skinned mesh's own node transform is ignored while
        // its joints' ancestors are not.
        if (p.restOffset[0] != 0.0f || p.restOffset[1] != 0.0f
            || p.restOffset[2] != 0.0f) {
            QJsonArray mine;
            for (int r = rootsBefore; r < S.sceneRoots.size(); ++r)
                mine.append(S.sceneRoots[r]);
            if (!mine.isEmpty()) {
                QJsonObject wrap;
                wrap[QStringLiteral("name")] =
                    QStringLiteral("rest offset %1").arg(partIdx);
                wrap[QStringLiteral("translation")] =
                    QJsonArray{p.restOffset[0], p.restOffset[1],
                               p.restOffset[2]};
                wrap[QStringLiteral("children")] = mine;
                S.nodeObjs.append(wrap);
                const int wrapNode = S.nodeObjs.size() - 1;
                while (S.sceneRoots.size() > rootsBefore)
                    S.sceneRoots.removeLast();
                S.sceneRoots.append(wrapNode);
            }
        }
        // The same three conditions appendModel uses to decide it is writing a
        // skeleton. A static part has no joint nodes, so a clip aimed at it
        // would address whatever came next in the node array.
        const bool skinned = !p.model->bones().isEmpty() && !pose
            && p.rigid == nullptr && opts.skeleton;
        if (skinned) {
            animTargets.append(AnimPartTarget{p.model, nodeBase});
            animPartIndex.append(partIdx);
        } else {
            ++staticParts;
        }
        ++partIdx;
    }

    if (animations && !animations->isEmpty()) {
        if (animTargets.isEmpty())
            qWarning("glb: %lld clip(s) dropped — every part in this scene "
                     "exports static (baked pose, attachment, or skeleton off)",
                     static_cast<long long>(animations->size()));
        else {
            if (staticParts)
                qInfo("glb: %d part(s) export static and take no animation — "
                      "they stay as the export found them",
                      staticParts);
            appendAnimations(S, animTargets, animPartIndex, *animations);
        }
    }

    // Nothing survived. glTF 2.0 requires at least one mesh, one accessor and
    // a non-empty buffer, so writing this out would produce a file that fails
    // validation in every viewer while this function returned true and the
    // caller printed "Exported". Reachable with every mesh group hidden — in
    // Models by unticking all the rows, in Customize by a variation that hides
    // everything the one equipped part has. The part-level guard upstream does
    // not catch it: a part with all its groups hidden is still a part.
    if (S.meshesJson.isEmpty())
        return fail(QStringLiteral(
            "everything in the scene is hidden — nothing to export"));

    BinBuilder& bin = S.bin;
    QJsonArray nodesJson;
    for (const QJsonObject& n : S.nodeObjs) nodesJson.append(n);
    const QJsonArray& meshesJson = S.meshesJson;
    const QJsonArray& materialsJson = S.materialsJson;
    const QJsonArray& texturesJson = S.texturesJson;
    const QJsonArray& imagesJson = S.imagesJson;
    const QJsonArray& samplersJson = S.samplersJson;
    const QJsonArray& skinsJson = S.skinsJson;
    const QJsonArray& sceneRoots = S.sceneRoots;

    // ── Document ─────────────────────────────────────────────────────────────
    QJsonObject root;
    QJsonObject asset;
    asset[QStringLiteral("version")] = QStringLiteral("2.0");
    asset[QStringLiteral("generator")] = QStringLiteral("FOXAssetBrowser");
    root[QStringLiteral("asset")] = asset;
    root[QStringLiteral("scene")] = 0;
    QJsonObject scene;
    // ── Scale and up-axis, as ONE wrapper node ───────────────────────────
    // Everything that was a scene root becomes its child, so the transform
    // reaches the meshes AND the joints. That matters: glTF says a skinned
    // mesh node's own transform is ignored, but the joints' ancestors are
    // not — wrapping both is what makes a skinned export scale and rotate
    // with the rest instead of silently staying at 1:1.
    //
    // A column-major 4x4, written in the order glTF wants (translation last).
    // Z-up is a +90° turn about X: (x, y, z) -> (x, -z, y), so the model's Y
    // becomes world Z and its Z becomes world -Y. Determinant k^3 > 0, so
    // nothing is mirrored and no winding is inverted.
    QJsonArray roots = sceneRoots;
    const bool identity = qFuzzyCompare(opts.scale, 1.0) && !opts.zUp;
    if (!identity) {
        const double k = opts.scale;
        QJsonObject wrap;
        wrap[QStringLiteral("name")] = QStringLiteral("FOXAssetBrowser");
        wrap[QStringLiteral("children")] = sceneRoots;
        wrap[QStringLiteral("matrix")] = opts.zUp
            ? QJsonArray{k, 0, 0, 0,  0, 0, k, 0,  0, -k, 0, 0,  0, 0, 0, 1}
            : QJsonArray{k, 0, 0, 0,  0, k, 0, 0,  0, 0, k, 0,  0, 0, 0, 1};
        nodesJson.append(wrap);
        roots = QJsonArray{nodesJson.size() - 1};
    }
    scene[QStringLiteral("nodes")] = roots;
    root[QStringLiteral("scenes")] = QJsonArray{scene};
    root[QStringLiteral("nodes")] = nodesJson;
    root[QStringLiteral("meshes")] = meshesJson;
    if (!skinsJson.isEmpty()) root[QStringLiteral("skins")] = skinsJson;
    // ── Animations ───────────────────────────────────────────────────────
    // Assembled last: a name that appeared on several parts has been
    // accumulating channels from each of them, and only now is any of them
    // finished. An animation with no channels is INVALID glTF, so a clip that
    // drove nothing is dropped rather than written empty.
    QJsonArray animationsJson;
    for (int i = 0; i < S.animNames.size(); ++i) {
        if (S.animChannels[i].isEmpty()) {
            qWarning("glb: clip '%s' drove no bone of anything in the scene — "
                     "not written", qUtf8Printable(S.animNames[i]));
            continue;
        }
        QJsonObject a;
        a[QStringLiteral("name")] = S.animNames[i];
        a[QStringLiteral("channels")] = S.animChannels[i];
        a[QStringLiteral("samplers")] = S.animSamplers[i];
        animationsJson.append(a);
    }
    if (animationsWritten) *animationsWritten = animationsJson.size();
    if (!animationsJson.isEmpty())
        root[QStringLiteral("animations")] = animationsJson;
    if (!materialsJson.isEmpty()) root[QStringLiteral("materials")] = materialsJson;
    if (!texturesJson.isEmpty()) {
        root[QStringLiteral("textures")] = texturesJson;
        root[QStringLiteral("images")] = imagesJson;
        root[QStringLiteral("samplers")] = samplersJson;
    }
    root[QStringLiteral("bufferViews")] = bin.bufferViews;
    root[QStringLiteral("accessors")] = bin.accessors;
    QJsonObject buffer;
    bin.align(4);
    buffer[QStringLiteral("byteLength")] = bin.data.size();
    root[QStringLiteral("buffers")] = QJsonArray{buffer};

    QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
    while (json.size() % 4) json.append(' ');

    // ── GLB container ────────────────────────────────────────────────────────
    QByteArray out;
    const auto putU32 = [&out](quint32 v) {
        char raw[4];
        qToLittleEndian(v, raw);
        out.append(raw, 4);
    };
    putU32(kGlbMagic);
    putU32(2);
    putU32(static_cast<quint32>(12 + 8 + json.size() + 8 + bin.data.size()));
    putU32(static_cast<quint32>(json.size()));
    putU32(kChunkJson);
    out += json;
    putU32(static_cast<quint32>(bin.data.size()));
    putU32(kChunkBin);
    out += bin.data;

    QFile f(outPath);
    if (!f.open(QIODevice::WriteOnly))
        return fail(QStringLiteral("cannot write %1: %2").arg(outPath, f.errorString()));
    f.write(out);
    f.close();
    // Worth a line: embedded PNGs dominate both the time and the file size, so
    // "18 maps -> 10 images" is what explains an export that is half the size
    // of the one before it.
    qInfo("glb: %lld bytes — %lld mesh(es), %lld material(s), %lld image(s) "
          "from %d map(s), %d colour bake(s), %lld ms",
          static_cast<long long>(out.size()),
          static_cast<long long>(meshesJson.size()),
          static_cast<long long>(materialsJson.size()),
          static_cast<long long>(imagesJson.size()), S.mapsOffered, S.baked,
          static_cast<long long>(exportTimer.elapsed()));
    return true;
}

}  // namespace glb
