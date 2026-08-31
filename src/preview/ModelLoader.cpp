// ModelLoader.cpp — see ModelLoader.h.
#include "preview/ModelLoader.h"

#include <algorithm>

#include <QPainter>

#include <QtGlobal>
#include <cmath>

#include "fox/FoxHash.h"
#include "fox/FoxMaterial.h"
#include "index/MaterialPresets.h"
#include "index/ArchiveIndex.h"
#include "util/Extract.h"

namespace modelload {

using fox::ArchiveIndex;
using fox::IndexedFile;

FovaOverrides fovaOverrides(const fox::FmdlFile& model, const fox::FovaFile& fova,
                            int* matched)
{
    // FOVA names its target materials by StrCode32, so first map that onto this
    // model's material indices. A name can legitimately repeat across material
    // instances (weapons reuse one material for several meshes), so every
    // matching index takes the substitution.
    FovaOverrides out;
    int hits = 0;
    for (const fox::FovaSubstitution& sub : fova.substitutions()) {
        if (sub.textureIndex < 0 || sub.textureIndex >= fova.textures().size())
            continue;   // 0xFFFF: this role keeps the model's own texture
        // A zero material hash is padding, not a target: a model whose material
        // name failed to resolve also hashes to 0, and the two would match each
        // other and substitute textures at random.
        if (sub.materialHash32 == 0) continue;
        const quint64 tex = fova.textures()[sub.textureIndex];
        for (int mi = 0; mi < model.materials().size(); ++mi) {
            if (model.materials()[mi].nameHash32 != sub.materialHash32) continue;
            out.insert({mi, sub.roleHash32}, tex);
            ++hits;
        }
    }
    if (matched) *matched = hits;
    return out;
}

void fovaGroupVisibility(const fox::FmdlFile& model, const fox::FovaFile& fova,
                         QSet<int>* hide, QSet<int>* show)
{
    if (hide) hide->clear();
    if (show) show->clear();
    // One pass over the model's groups per named hash rather than a lookup per
    // group: a group name can legitimately repeat (FMDL does not enforce unique
    // names) and every instance has to move together, or half a bandanna stays
    // on screen.
    const auto collect = [&](const QVector<quint32>& names, QSet<int>* out) {
        if (!out) return;
        for (quint32 h : names) {
            if (h == 0) continue;   // padding — and a name that failed to
                                    // resolve hashes to 0 too, so it would
                                    // match every such group in the model
            for (int gi = 0; gi < model.meshGroups().size(); ++gi)
                if (model.meshGroups()[gi].nameHash32 == h) out->insert(gi);
        }
    };
    collect(fova.hiddenMeshGroups(), hide);
    collect(fova.shownMeshGroups(), show);
    // Show is applied after hide by the engine, so a group in both is visible.
    if (hide && show)
        for (int gi : *show) hide->remove(gi);
}

// Load the texture a material slot should use: the FOVA substitution when one
// applies AND actually resolves, otherwise the model's own texture.
//
// The fallback is the point. A substitution can fail to resolve — the texture
// archive is not configured, the install is partial, or (for GZ models) the
// lookup goes through the legacy path hash rather than the PathCode64 the
// variation stores. Without a fallback the material would come back with no
// texture at all, so choosing a camouflage could BLANK a surface instead of
// recolouring it.
QImage loadWithOverride(const fox::FmdlTextureRef& ref, int materialIndex,
                        bool gz, const FovaOverrides* fova,
                        fox::FmdlTextureRef* usedOut, bool lowRes = false)
{
    if (fova) {
        const auto it = fova->constFind({materialIndex, ref.roleHash32});
        if (it != fova->constEnd()) {
            fox::FmdlTextureRef sub = ref;
            sub.pathHash = it.value();
            // Give the GZ branch of textureImageFor() something to work with
            // too: it looks the texture up by PATH, not by the stored code.
            QString resolved;
            sub.path = fox::HashResolver::instance().tryResolve(it.value(), &resolved)
                ? resolved
                : QString();
            const QImage img = extract::textureImageFor(sub, gz, lowRes);
            if (!img.isNull()) {
                if (usedOut) *usedOut = sub;
                return img;
            }
        }
    }
    if (usedOut) *usedOut = ref;
    return extract::textureImageFor(ref, gz, lowRes);
}

QImage unswizzleDxt5nm(const QImage& src)
{
    // Fox tangent-space normal maps are DXT5nm: x in ALPHA, y in GREEN; red and
    // blue carry no signal at all (measured across the corpus: standard
    // deviation 0.0 on every _nrm texture). Rebuild a plain RGB normal map with
    // z = sqrt(1 - x^2 - y^2) so everything downstream — mip generation, the
    // shader, the glTF exporter — sees an ordinary normal map with no alpha.
    const QImage in = src.convertToFormat(QImage::Format_RGBA8888);
    if (in.isNull()) return {};
    QImage out(in.width(), in.height(), QImage::Format_RGB888);
    if (out.isNull()) return {};
    for (int y = 0; y < in.height(); ++y) {
        const uchar* s = in.constScanLine(y);
        uchar* d = out.scanLine(y);
        for (int x = 0; x < in.width(); ++x) {
            const float nx = s[x * 4 + 3] / 127.5f - 1.0f;   // alpha
            const float ny = s[x * 4 + 1] / 127.5f - 1.0f;   // green
            const float nz =
                std::sqrt(std::max(0.0f, 1.0f - nx * nx - ny * ny));
            d[x * 3 + 0] = s[x * 4 + 3];
            d[x * 3 + 1] = s[x * 4 + 1];
            d[x * 3 + 2] =
                static_cast<uchar>(qBound(0.0f, (nz + 1.0f) * 127.5f, 255.0f));
        }
    }
    return out;
}

// Alpha-composite `decal` over `base`, scaled to the base's size. The decal is
// authored at the face's UV layout, so this is a straight overlay — no
// placement maths, which is exactly why the game can ship one decal per skin
// tone and drop it onto any face.
static void compositeDecal(QImage& base, const QImage& decal)
{
    if (base.isNull() || decal.isNull()) return;
    QImage over = decal;
    if (over.size() != base.size())
        over = over.scaled(base.size(), Qt::IgnoreAspectRatio,
                           Qt::SmoothTransformation);
    if (!over.hasAlphaChannel()) return;   // nothing to blend through
    base = base.convertToFormat(QImage::Format_ARGB32);
    QPainter p(&base);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.drawImage(0, 0, over.convertToFormat(QImage::Format_ARGB32));
    p.end();
    base = base.convertToFormat(QImage::Format_RGBA8888);
}

// Composite an iris map into the eyeball map's own iris disc.
//
// The two share a UV layout — both are the eye seen face on, centred, with the
// pupil at the middle — but the iris map is fully OPAQUE and covers the whole
// square, so laying it over the base the way a scar decal goes over a face
// would paint out the sclera and the eyelid shading with it. The game's shader
// masks it to the iris, and the mask is not shipped as a texture (checked: the
// eye material's other maps are a normal map, a lens translucency ramp, a lens
// highlight blob and a specular map — none of them is a disc).
//
// So the disc is MEASURED off the eyeball map itself rather than hard-coded:
// the sclera is pink and the iris disc is not, so walking out from the centre
// along the four axes and stopping where the pixels turn pink finds its edge.
// A soft edge over the last few percent hides the seam. When the measurement
// finds nothing plausible the composite is skipped entirely — a face with its
// default eyes is a far better failure than one with two flat discs on it.
static void compositeIris(QImage& base, const QImage& irisMap)
{
    if (base.isNull() || irisMap.isNull()) return;
    QImage dst = base.convertToFormat(QImage::Format_RGBA8888);
    const int w = dst.width(), h = dst.height();
    if (w < 16 || h < 16) return;
    const int cx = w / 2, cy = h / 2;

    // The map runs dark to light outward: black pupil, then the iris disc, then
    // the bright sclera. Measured across the centre row of cm_eyes2_def_bsm the
    // pupil is luminance 0, the iris around 45 and the sclera 115-155, so the
    // iris edge is where the walk first crosses into the bright band. Hue does
    // NOT separate them — an early version tested "more red than blue" and the
    // iris and sclera are only 8 apart on that, so the walk ran off the edge of
    // the map every time and the composite silently never happened.
    const auto lumAt = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= w || y >= h) return 0;
        const uchar* px = dst.constScanLine(y) + x * 4;
        return (3 * int(px[0]) + 6 * int(px[1]) + int(px[2])) / 10;
    };
    const int limit = qMin(w, h) / 2;
    int peak = 0;
    for (int y = 0; y < h; y += 2)
        for (int x = 0; x < w; x += 2) peak = qMax(peak, lumAt(x, y));
    if (peak < 60) return;   // no bright band at all: not an eyeball map
    const int bright = peak * 55 / 100;
    int rad[4];
    for (int axis = 0; axis < 4; ++axis) {
        int r = 1;
        for (; r < limit; ++r) {
            const int x = cx + (axis == 0 ? r : axis == 1 ? -r : 0);
            const int y = cy + (axis == 2 ? r : axis == 3 ? -r : 0);
            if (lumAt(x, y) > bright) break;
        }
        rad[axis] = r;
    }
    // The MEDIAN of the four, not the largest: on this art the upward walk sits
    // under the eyelid shadow and never reaches the sclera, so it returns the
    // whole half-width. Three axes agree to within three pixels and the fourth
    // is thrown away by taking the middle.
    std::sort(rad, rad + 4);
    const int radius = (rad[1] + rad[2]) / 2;
    // A disc that fills the map, or one barely bigger than the pupil, means the
    // measurement failed on art this code has not seen.
    if (radius < limit / 5 || radius > limit * 9 / 10) return;

    QImage src = irisMap.convertToFormat(QImage::Format_RGBA8888);
    if (src.size() != dst.size())
        src = src.scaled(dst.size(), Qt::IgnoreAspectRatio,
                         Qt::SmoothTransformation)
                  .convertToFormat(QImage::Format_RGBA8888);
    const double rIn = radius * 0.88, rOut = double(radius);
    for (int y = 0; y < h; ++y) {
        uchar* drow = dst.scanLine(y);
        const uchar* srow = src.constScanLine(y);
        const double dy = y - cy;
        for (int x = 0; x < w; ++x) {
            const double dx = x - cx;
            const double d = std::sqrt(dx * dx + dy * dy);
            if (d >= rOut) continue;
            const double a = d <= rIn ? 1.0 : (rOut - d) / (rOut - rIn);
            uchar* dp = drow + x * 4;
            const uchar* sp = srow + x * 4;
            for (int c = 0; c < 3; ++c)
                dp[c] = uchar(qBound(0.0, dp[c] * (1.0 - a) + sp[c] * a, 255.0));
        }
    }
    base = dst;
}

void avatarOverrides(const fox::FmdlFile& model, const QString& modelStem,
                     const fox::AvatarLook& look, const FovaOverrides* base,
                     FovaOverrides* out, TextureOverlays* overlays,
                     TextureForce* force, QSet<int>* hideGroups, int* classified,
                     FovaOverrides* outNrm, TextureIris* iris,
                     FovaOverrides* outSrm)
{
    const fox::AvatarTextures& at = fox::AvatarTextures::instance();
    if (!at.ok() || !out) { if (classified) *classified = 0; return; }
    if (base) *out = *base;
    int n = 0;
    const bool isHair = modelStem.contains(QLatin1String("_hair"));

    // ── What each material draws ────────────────────────────────────────────
    // An avatar head names almost nothing. Every face-layer material binds the
    // same placeholder (cm_flat_gry128) with the same shader and the same
    // translucency map, so no property of a material tells them apart. The
    // MESHES do: the face is one big mesh, the eyebrows a narrow strip, the
    // feature slots four-vertex decal quads. So the geometry is what classifies
    // here — measured per material, not assumed.
    const int matCount = model.materials().size();
    QVector<int> verts(matCount, 0);
    QVector<float> ylo(matCount, 1e30f), yhi(matCount, -1e30f);
    // Mean X per material — the only thing that tells the two eyes apart, since
    // they are the same shader binding the same maps.
    QVector<double> xsum(matCount, 0.0);
    QVector<int> groupOf(matCount, -1);
    for (const fox::FmdlMesh& mesh : model.meshes()) {
        const int mi = mesh.materialInstanceIndex;
        if (mi < 0 || mi >= matCount) continue;
        const int nv = mesh.positions.size() / 3;
        verts[mi] += nv;
        if (groupOf[mi] < 0) groupOf[mi] = mesh.meshGroupIndex;
        for (int v = 0; v + 2 < mesh.positions.size(); v += 3) {
            ylo[mi] = qMin(ylo[mi], mesh.positions[v + 1]);
            yhi[mi] = qMax(yhi[mi], mesh.positions[v + 1]);
            xsum[mi] += mesh.positions[v];
        }
    }

    // Which materials sit on the face at all — the ones whose translucency or
    // dirt map lives in the avatar's own face folder.
    QVector<bool> onFace(matCount, false);
    int faceMat = -1, browMat = -1;
    QVector<int> decalMats;
    for (int mi = 0; mi < matCount; ++mi) {
        const fox::FmdlMaterialInstance& mat = model.materials()[mi];
        bool hasBase = false, faceish = false;
        for (const fox::FmdlTextureRef& t : mat.textures) {
            if (t.role.contains(QLatin1String("Base_Tex"))) hasBase = true;
            const QString p = t.path.toLower();
            if (p.contains(QLatin1String("/face/"))) faceish = true;
        }
        onFace[mi] = faceish;
        if (!faceish) continue;
        // The face itself is the material with by far the most geometry.
        if (hasBase && (faceMat < 0 || verts[mi] > verts[faceMat])) faceMat = mi;
        // Four-vertex decal quads. Measured and named so the next person does
        // not mistake them for the feature slots — the features composite onto
        // the face map instead.
        if (hasBase && verts[mi] > 0 && verts[mi] <= 16) decalMats.append(mi);
        // The eyebrows carry a normal and a specular map and NO base colour —
        // there is nothing for the model to bind until the game picks a brow.
        if (!hasBase && verts[mi] > 0
            && (browMat < 0 || verts[mi] > verts[browMat]))
            browMat = mi;
    }
    // ── The eyebrow and facial-hair slots ───────────────────────────────────
    //
    // Both heads carry two "hair on skin" materials, and they are told apart
    // from the rest of the head by their TEXTURE LIST, which is exactly base +
    // normal + specular and nothing else. Every other material on a head binds
    // more than that: the face layers add Translucent and Dirty, the eyes add
    // three unnamed maps, and there is one Mask-only material that is neither.
    // Measured on both shipped heads:
    //
    //   avf0_type0_def  12 materials   9 = Mask only   10 & 11 = base/nrm/spec
    //   avm0_type0_def  13 materials  10 = Mask only    9 & 11 = base/nrm/spec
    //
    // Which of the two is which is geometry, not properties — they are the same
    // shader with the same flat placeholder maps:
    //
    //   avf  mat11  186 verts  Y 0.589-0.630   brows (eye height)
    //   avf  mat10  376 verts  Y 0.519-0.557   facial hair (jaw)
    //   avm  mat11  106 verts  Y 0.611-0.647   brows
    //   avm  mat9   333 verts  Y 0.532-0.570   facial hair
    //
    // Taking "the material with no base map" instead — which is what this did —
    // picked the Mask-only one on BOTH heads, so every brow choice was landing
    // on a 31-to-45 vertex mesh that is not the eyebrows.
    int beardMat = -1;
    {
        // The head TELLS you which material is which, if you read the paths it
        // already binds rather than guessing from geometry. The folder is the
        // answer — /ebrw/ is the brows, /berd/ is the facial hair — and no
        // vertex count or Y bound is needed.
        //
        // BUT NOT ONLY ON Base_Tex, and that restriction is the whole of the
        // "male facial hair changes nothing" bug. Measured on the head this
        // browser actually loads for the MGO male, MGO's own copy of
        // avm0_type0_def (13 materials):
        //
        //   mat11  106 verts  Base=cm_flat_white  nrm=cm_flat_nrm  spec=…lambert
        //   mat12  559 verts  Base=cm_flat_white  …  Translucent_Tex_LIN ->
        //                     /Assets/tpp/chara/avm/Pictures/berd/avm_berd0_a0_trm
        //
        // Its beard and brow slots bind FLAT PLACEHOLDERS on Base_Tex; the only
        // authored /berd/ path on the head is on the beard's TRANSLUCENT slot.
        // A Base_Tex-only scan therefore left beardMat at -1 on every MGO male,
        // and with no beard material there is nothing to substitute a map into
        // and nothing to hide for clean-shaven — so every Facial Hair choice,
        // clean-shaven included, rendered the same untextured beard mesh.
        // (Another head, TPP's own copy, does bind a real default beard on
        // Base_Tex; both readings are true of the data, so the test has to
        // accept either.)
        //
        // Base_Tex is still tried FIRST and on its own, because it is the more
        // specific evidence: a head that names a real default beard there
        // should not be out-voted by some other material that happens to
        // mention the folder in a secondary slot. Only if that finds nothing
        // does the scan widen to every texture role.
        //
        // Getting here by geometry cost two wrong guesses — the base-less test
        // picked the Mask-only material, and "exactly base+normal+specular"
        // picked the TEETH, which share that signature with the brows and sit
        // at mouth height.
        // browMat may already hold the face scan's GEOMETRY guess (a faceish
        // material with no base colour). An authored /ebrw/ or /berd/ path
        // outranks that guess and is allowed to replace it; what it must never
        // do is out-vote the OTHER path pass, so the widened pass may only
        // improve on a geometry guess or an empty slot, never on a Base_Tex
        // find. That is what these two flags track.
        bool browFromPath = false, beardFromPath = false;
        // Found in the BASE slot (pass 0), which outranks anything the
        // widened pass turns up: see the ranking note in the loop below.
        bool browFromBase = false, beardFromBase = false;
        for (int pass = 0; pass < 2; ++pass) {
            const bool baseOnly = pass == 0;
            if (!baseOnly && browFromPath && beardFromPath) break;
            for (int mi = 0; mi < matCount; ++mi) {
                if (verts[mi] <= 0) continue;
                // NEVER the face. The widened pass ranks by vertex count, and
                // the face is by construction the largest material on the
                // head — so a face that mentions the beard folder in any
                // secondary slot would be claimed as the beard, and then
                // substitute() would paint a beard map over the face and
                // clean-shaven would hideSoleGroupOf() the whole face mesh.
                // The base-slot pass is left alone: a face binding a real
                // beard map as its own base colour is not a thing the data
                // does, and pass 0 has to stay behaviour-identical.
                if (!baseOnly && mi == faceMat) continue;
                bool brow = false, berd = false;
                for (const fox::FmdlTextureRef& t :
                     model.materials()[mi].textures) {
                    if (baseOnly && !t.role.contains(QLatin1String("Base_Tex")))
                        continue;
                    const QString tp = t.path.toLower();
                    if (tp.contains(QLatin1String("/ebrw/"))) brow = true;
                    else if (tp.contains(QLatin1String("/berd/"))) berd = true;
                    if (baseOnly) break;   // the base slot alone, as before
                }
                // A material that mentions both folders is the beard's: the
                // beard is the larger mesh and the one whose maps come in
                // pairs, and no shipped head binds a brow map anywhere but on
                // a brow material.
                // Largest wins WITHIN a pass; a pass-1 find never displaces a
                // pass-0 one. The earlier form gated on !beardFromPath, which
                // made the first match in pass 1 unbeatable — so the loop that
                // the comment above describes as ranking by vertex count in
                // fact took whichever material came first. On every shipped
                // head exactly one material names each folder, so the two
                // agree on the data; they stop agreeing the moment a head
                // mentions the folder twice, and then the small one wins.
                if (berd && (baseOnly || !beardFromBase)
                    && (beardMat < 0 || !beardFromPath
                        || verts[mi] > verts[beardMat])) {
                    beardMat = mi;
                    beardFromPath = true;
                    if (baseOnly) beardFromBase = true;
                } else if (brow && (baseOnly || !browFromBase)
                           && (browMat < 0 || !browFromPath
                               || verts[mi] > verts[browMat])) {
                    browMat = mi;
                    browFromPath = true;
                    if (baseOnly) browFromBase = true;
                }
            }
            if (beardFromPath && browFromPath) break;
        }

        // Fallback for a head that names none of its maps (an install whose
        // dictionary does not cover them): the two "hair on skin" materials
        // bind exactly base + normal + specular and nothing else, and of those
        // the higher one is the brows.
        QVector<int> hairish;
        for (int mi = 0; mi < matCount; ++mi) {
            if (verts[mi] <= 0) continue;
            bool base = false, nrm = false, spec = false, other = false;
            for (const fox::FmdlTextureRef& t : model.materials()[mi].textures) {
                if (t.role.contains(QLatin1String("Base_Tex"))) base = true;
                else if (t.role.contains(QLatin1String("NormalMap"))) nrm = true;
                else if (t.role.contains(QLatin1String("SpecularMap"))) spec = true;
                else other = true;   // Translucent, Dirty, Mask, Layer, unnamed
            }
            Q_UNUSED(base);
            if (nrm && spec && !other
                && model.materials()[mi].textures.size() <= 3)
                hairish.append(mi);
        }
        if (browMat < 0 && !hairish.isEmpty()) {
            int hi = hairish[0];
            for (int mi : hairish)
                if (yhi[mi] > yhi[hi]) hi = mi;
            browMat = hi;
        }
        // Nothing matched — fall back to the old "no base map at all" test
        // rather than leaving the brows unstyled on a head shaped differently
        // from the two measured above.
        if (browMat < 0)
            for (int mi = 0; mi < matCount; ++mi) {
                bool hasBase = false;
                for (const fox::FmdlTextureRef& t :
                     model.materials()[mi].textures)
                    if (t.role.contains(QLatin1String("Base_Tex")))
                        hasBase = true;
                if (!hasBase && verts[mi] > 0
                    && (browMat < 0 || verts[mi] > verts[browMat]))
                    browMat = mi;
            }
    }

    // Decal quads only count when there is a real face to sit on.
    if (faceMat < 0) decalMats.clear();

    const auto substitute = [&](int mi, const QString& want) {
        if (want.isEmpty() || mi < 0 || mi >= matCount) return;
        // The value must be the FULL archive key, extension code and all:
        // textureImageFor() hands pathHash straight to findByHash().
        const quint64 key =
            fox::hashFileNameWithExtension(want + QLatin1String(".ftex"));
        bool placed = false;
        for (const fox::FmdlTextureRef& t : model.materials()[mi].textures) {
            if (!t.role.contains(QLatin1String("Base_Tex"))) continue;
            out->insert({mi, t.roleHash32}, key);
            placed = true;
            break;
        }
        // No base slot to substitute into — force one.
        if (!placed && force) force->insert(mi, key);
        ++n;
    };

    // The SRM slot. Same shape as substituteNrm and for the same reason: the
    // map exists, the material's own reference is a flat placeholder (or
    // empty), and the shader reads occlusion, roughness and reflection out of
    // whatever is bound. Unlike the base and normal slots this one is NOT
    // per-colour or per-tone — an SRM does not change because the hair went
    // from blonde to black — so every caller passes a shape and nothing else.
    const auto substituteSrm = [&](int mi, const QString& want) {
        if (!outSrm || want.isEmpty() || mi < 0 || mi >= matCount) return;
        // ONLY when the material's own reference leads nowhere. Three cases
        // count as nowhere: no specular slot, a slot naming a file no mounted
        // archive holds (a dangling reference, which is precisely what the
        // engine substitutes for at runtime), and a slot naming one of the
        // common_source/flat placeholders. Anything else is a real map the
        // artist chose, and replacing it with a name derived from the model
        // stem would be this tool overruling the data — the failure mode is
        // silent and looks like a bug in the game.
        const ArchiveIndex& index = ArchiveIndex::instance();
        for (const fox::FmdlTextureRef& t : model.materials()[mi].textures) {
            if (t.roleHash32 != fox::texrole::kSpecular) continue;
            if (t.pathHash == 0) break;                  // an empty slot
            const fox::IndexedFile* have = index.findByHash(t.pathHash);
            if (!have) break;                            // dangling
            if (have->path.contains(QLatin1String("/common_source/flat/"),
                                    Qt::CaseInsensitive))
                break;                                   // a placeholder
            return;                                      // a real map — leave it
        }
        const quint64 key =
            fox::hashFileNameWithExtension(want + QLatin1String(".ftex"));
        // Keyed on the ROLE, not on whatever ref happens to be first: a
        // material can bind several maps and loadPbrMaps looks this up by
        // {material, role} exactly as the gear-colour overrides do.
        outSrm->insert({mi, fox::texrole::kSpecular}, key);
    };

    if (isHair) {
        // A hair model is hair the whole way through, and its material points
        // at cm_flat_white — a literally white one-colour placeholder, which is
        // why unstyled hair renders white.
        // Family 0 ONLY: hair0 is the card atlas this mesh wants. Falling
        // through to hair1 would paint the face-UV hairline onto the cards.
        const QString want = at.hairPath(modelStem, look.hairColour, 0);
        // Family 0 for BOTH, and for the same reason: hair0 is the card atlas
        // this mesh wants, hair1 is the face-UV hairline. The SRM follows the
        // base map's family or the strands would be lit by a map drawn for a
        // different layout.
        const QString wantSrm = at.hairSpecularPathFor(modelStem, 0);
        for (int mi = 0; mi < matCount; ++mi) {
            substitute(mi, want);
            substituteSrm(mi, wantSrm);
        }
        if (classified) *classified = n;
        return;
    }

    // The NORMAL map is per wrinkle set and shared by every tone, and it is
    // where the wrinkles actually are — swapping the colour map alone gives a
    // smooth face whichever set is picked.
    const auto substituteNrm = [&](int mi, const QString& want) {
        if (!outNrm || want.isEmpty() || mi < 0 || mi >= matCount) return;
        const quint64 key =
            fox::hashFileNameWithExtension(want + QLatin1String(".ftex"));
        // The SAME two-pass pick loadNormalMaps() uses, or the key would name a
        // ref the loader never looks at and the substitution would do nothing.
        const fox::FmdlTextureRef* fallback = nullptr;
        for (const fox::FmdlTextureRef& t : model.materials()[mi].textures) {
            if (t.role.startsWith(QLatin1String("NormalMap_Tex"))
                || t.role == QLatin1String("0xcc4305511ae0")) {
                outNrm->insert({mi, t.roleHash32}, key);
                return;
            }
            if (!fallback && t.role.contains(QLatin1String("NormalMap_Tex"))
                && !t.role.startsWith(QLatin1String("Sub")))
                fallback = &t;
        }
        if (fallback) outNrm->insert({mi, fallback->roleHash32}, key);
    };

    substitute(faceMat, at.facePathFor(modelStem, look.wrinkle, look.skin));
    substituteNrm(faceMat, at.faceNormalPathFor(modelStem, look.wrinkle));
    substituteSrm(faceMat, at.faceSpecularPathFor(modelStem, look.wrinkle));
    // Whether a map actually LANDED, not whether an index looked plausible.
    // Both of these end in a hide when they come back empty, and "this install
    // does not ship that brow" must not be answered by shaving the character.
    const QString browMap = at.browPath(look.browShape, look.browColour);
    const QString beardMap = (beardMat >= 0 && look.beard >= 0)
        ? at.beardPath(look.beard, look.browColour) : QString();
    substitute(browMat, browMap);
    if (!browMap.isEmpty()) substituteSrm(browMat, at.browSpecularPath(look.browShape));
    if (!beardMap.isEmpty()) {
        substitute(beardMat, beardMap);
        substituteSrm(beardMat, at.beardSpecularPath(look.beard));
    }
    // FOXAB_DUMP_FACE=1 prints the head's whole material table and the three
    // slots this function resolved out of it. That table is what identified
    // the "male facial hair changes nothing" bug — the beard material binds
    // its only /berd/ path on Translucent_Tex_LIN, not on Base_Tex — and it is
    // the first thing to look at whenever a face row stops doing anything.
    static const bool kDumpFace = qEnvironmentVariableIsSet("FOXAB_DUMP_FACE");
    if (kDumpFace) {
        for (int mi = 0; mi < matCount; ++mi) {
            QStringList ts;
            for (const fox::FmdlTextureRef& t : model.materials()[mi].textures)
                ts << (t.role + QLatin1Char('=') + t.path);
            qInfo("face-dump: %s mat %d verts=%d %s", qUtf8Printable(modelStem), mi,
                  verts[mi], qUtf8Printable(ts.join(QLatin1String(" | "))));
        }
        qInfo("face-dump: %s face=%d brow=%d beard=%d | look beard=%d "
              "wanted=%d colour=%d | beardMap='%s' browMap='%s'",
              qUtf8Printable(modelStem), faceMat, browMat, beardMat, look.beard,
              int(look.beardWanted), look.browColour, qUtf8Printable(beardMap),
              qUtf8Printable(browMap));
    }

    // A bare limb is skin, and skin has the same five tones the face does —
    // /chara/avm/Pictures/body/avf0_body0_def_c00..c04_bsm. Without this the
    // head is whichever tone the preset chose and the body is whatever tone its
    // model happened to ship, which reads as a head that does not belong to the
    // body. Only parts the caller marked as bare are treated this way: putting
    // a skin map on a jacket would be considerably worse than a tone mismatch.
    // …and ONLY for a part from the game whose avatar these maps are. See
    // AvatarLook::avatarGame: applying them to a Survive limb paints one
    // character's skin onto another's mesh, and the result reads as a flipped
    // or corrupt texture rather than as the wrong file, which is what made it
    // hard to spot.
    // FOXAB_OLD_BARESKIN=1 restores the old unconditional behaviour, so the
    // difference this makes can be seen in one binary.
    static const bool kOldBare = qEnvironmentVariableIsSet("FOXAB_OLD_BARESKIN");
    // …and only when the tone was CHOSEN. See AvatarLook::skinChosen: a look
    // carrying nothing but the install's first tone has none of its own to
    // impose, and imposing it anyway is how the avatar's body map came to land
    // on characters that ship their own.
    if (look.bareSkin && look.skinChosen && (look.avatarGame || kOldBare)) {
        const QString b = at.bodyPath(look.skin, look.men);
        // WHICH materials, though. A survivor's torso is one model with two
        // materials that bind the SAME placeholder — the skin and the belt —
        // so no texture tells them apart. The shader does:
        //
        //   skin      Translucent_Tex_LIN, no Layer   (subsurface)
        //   clothing  Layer_Tex + LayerMask_Tex       (the colourable one)
        //
        // Checked on arf0 (1 material, skin), bdf0 (skin + belt) and lgf0
        // (trousers + skin). Re-toning the layered material is what painted
        // skin over the belt. The test now lives in skinMaterials() because
        // the Customize tab has to ask the same question to know whether a
        // part has any skin on it at all.
        if (!b.isEmpty())
            for (const int mi : skinMaterials(model)) substitute(mi, b);
    }

    // The facial feature is a DECAL ON THE FACE MAP, not a material of its own.
    // Decoded, avm_gash0_v01_c02_bsm_alp is a 1024x1024 canvas in the face's
    // own UV layout with the scar painted in one corner and a real 0-255 alpha
    // everywhere else — so it composites over the face texture, and one map per
    // skin tone is all the game needs. (The head does carry a handful of
    // four-vertex decal quads; they are NOT this, and binding the feature to
    // them put it nowhere visible.)
    if (overlays && faceMat >= 0 && look.decoType >= 0 && look.decoId >= 0) {
        const QString d = at.decoPath(look.decoType, look.decoId, look.skin);
        if (!d.isEmpty())
            (*overlays)[faceMat].append(
                fox::hashFileNameWithExtension(d + QLatin1String(".ftex")));
    }

    // THE HAIRLINE, and then the other half of the beard. Both are the same
    // trick: a style that also ships painted in the FACE's own UV layout, laid
    // over the face map, because cards alone leave a bald scalp and a
    // clean-shaven jaw under the geometry.
    //
    //   hair1  the scalp, the stubble at the temples, the shaved sides — and
    //          the whole of a style whose mesh is nothing but a few cards
    //   berd1  the beard on the skin, under the beard mesh's own cards
    //
    // Told apart from the mesh atlases by their map sets, measured across the
    // shipped styles: hair0/berd0 carry dtm and trm, the ones that belong on a
    // hair-card shader; hair1/berd1 carry nrm and srm and neither of those.
    //
    // ORDER IS THE POINT. loadBaseTextures composites this list in sequence,
    // each map over the running result, so "under" means EARLIER. The feature
    // goes down first (a scar is on the skin), then the hairline over it (hair
    // covers a scar on the temple), then the beard over that — the sideburns
    // are the one place the hairline and the beard actually meet, and there the
    // beard wins.
    if (overlays && faceMat >= 0 && !isHair && !look.hairStem.isEmpty()
        && look.hairColour >= 0) {
        const QString h = at.hairSkinPath(look.hairStem, look.hairColour);
        if (!h.isEmpty())
            (*overlays)[faceMat].append(
                fox::hashFileNameWithExtension(h + QLatin1String(".ftex")));
    }
    if (overlays && faceMat >= 0 && beardMat >= 0 && !beardMap.isEmpty()) {
        const QString b = at.beardSkinPath(look.beard, look.browColour);
        if (!b.isEmpty())
            (*overlays)[faceMat].append(
                fox::hashFileNameWithExtension(b + QLatin1String(".ftex")));
    }

    // ── The two eyes ────────────────────────────────────────────────────────
    //
    // An avatar head carries TWO eye materials, identical in every property:
    // same shader, same eyeball base map (cm_eyes2_def_bsm), same lens and
    // specular maps, and the same default iris (cm_iris0_c00_bsm) bound in an
    // unnamed slot. Nothing but position separates them, so they are told apart
    // by the mean X of their own geometry — and because the preset table really
    // does hold the two eyes independently (the men's preset 17 is one copper
    // eye and one white one), getting the sides the right way round matters.
    //
    // Which side is which is measured off avf0_type0_def rather than assumed.
    // The two eye materials sit at mean X +0.0318 and -0.0318 — mirrored to
    // four decimal places — and the head's own forward axis is +Z: the eyes are
    // at mean Z +0.0978 and the brows +0.1079 against a head centroid of
    // +0.0483, the nose tip is the model's Z maximum, and the material covering
    // the back of the head is at -0.0621. With forward +Z and up +Y in the
    // right-handed space the viewport already draws these models in, the
    // character's own right is forward x up = -X.
    //
    // So the eye with the LARGER mean X is the character's LEFT and the smaller
    // is the RIGHT. (Corroboration: the default camera sits at +X/+Z, which has
    // a positive dot with +Z, and every avatar shot from it shows the face and
    // not the back of the head.)
    if (iris && !isHair) {
        QVector<int> eyeMats;
        for (int mi = 0; mi < matCount; ++mi) {
            if (verts[mi] <= 0) continue;
            for (const fox::FmdlTextureRef& t : model.materials()[mi].textures)
                if (t.path.toLower().contains(QLatin1String("cm_iris"))) {
                    eyeMats.append(mi);
                    break;
                }
        }
        if (eyeMats.size() == 2) {
            const auto meanX = [&](int mi) {
                return verts[mi] > 0 ? xsum[mi] / verts[mi] : 0.0;
            };
            const int left = meanX(eyeMats[0]) >= meanX(eyeMats[1]) ? eyeMats[0]
                                                                    : eyeMats[1];
            const int right = left == eyeMats[0] ? eyeMats[1] : eyeMats[0];
            const auto put = [&](int mi, int colour, int shade) {
                if (colour < 0) return;
                const QString p = at.irisPath(colour, shade);
                if (p.isEmpty()) return;
                iris->insert(mi, fox::hashFileNameWithExtension(
                                     p + QLatin1String(".ftex")));
                ++n;
            };
            put(right, look.eyeColourR, look.eyeShadeR);
            put(left, look.eyeColourL, look.eyeShadeL);
        } else if (eyeMats.size() == 1) {
            // One eye material for both eyes — then there is only one colour to
            // give it, and the right eye is the one the row order leads with.
            const int c = look.eyeColourR >= 0 ? look.eyeColourR : look.eyeColourL;
            const int sh = look.eyeColourR >= 0 ? look.eyeShadeR : look.eyeShadeL;
            const QString p = at.irisPath(c, sh);
            if (!p.isEmpty()) {
                iris->insert(eyeMats[0], fox::hashFileNameWithExtension(
                                             p + QLatin1String(".ftex")));
                ++n;
            }
        }
    }

    // Clean-shaven, and browless, are STATES — not the absence of a choice.
    //
    // The head ships a REAL beard and a REAL brow already bound (measured on
    // avm0_type0_def: mat12 -> berd/avm_berd0_a2_gd0_bsm_alp, mat11 ->
    // ebrw/avm_ebrw0_a1_gd0_bsm_alp), which is what makes the folder test above
    // work at all. It also means "no beard" cannot be expressed by declining
    // to substitute: that leaves the model's own beard on the face, so a preset
    // with no beard wore the previous one's — or, on a fresh page, the default
    // blonde one it was authored with.
    //
    // So the MESH goes. Both sit in a mesh group of their own on the shipped
    // heads (beard 559 verts, brows 106), which is what makes this safe.
    const auto hideSoleGroupOf = [&](int mi) {
        if (!hideGroups || mi < 0) return;
        const int g = groupOf[mi];
        if (g < 0) return;
        // ONLY when the group is that material's alone. groupOf[] records the
        // first group a material appears in, and a material that shares one
        // with the face would take the whole face down with it.
        for (const fox::FmdlMesh& mesh : model.meshes())
            if (mesh.meshGroupIndex == g && mesh.materialInstanceIndex != mi)
                return;
        hideGroups->insert(g);
    };
    // ONLY when the look asked for none. `look.beard < 0` alone would also
    // catch "the preset names a beard this install does not carry", and
    // answering a missing texture by deleting the geometry turns a bearded
    // preset clean-shaven on a partial install — strictly worse than leaving
    // the model's own beard on. beardWanted says which of the two it is.
    if (!look.beardWanted && beardMap.isEmpty()) hideSoleGroupOf(beardMat);
    if (look.browShape < 0 && browMap.isEmpty()) hideSoleGroupOf(browMat);

    // The bandanna, BY THE GROUP'S OWN NAME first. Every avatar head ships one
    // welded on in a group called MESH_bdn_IV — the game only draws it when
    // the matching item is equipped (hat21_main0_def_f is the bandanna that
    // turns it on), so on a head by itself it must be off.
    //
    // The name is a far better signal than the texture heuristic below it: a
    // group is named by the artist and travels with the model, while the
    // texture path only says "this material came out of /chara/hats", which
    // misses any head whose bandanna is textured from somewhere else. That is
    // why heads were still turning up wearing one.
    //
    // Matched on "bdn" rather than on the exact MESH_bdn_IV, because the
    // suffix is a variant marker and the same welded headwrap appears under
    // more than one of them.
    if (hideGroups && !look.bandana)
        for (int gi = 0; gi < model.meshGroups().size(); ++gi) {
            const QString gname = model.meshGroups()[gi].name.toLower();
            if (gname.contains(QLatin1String("bdn"))) hideGroups->insert(gi);
        }
    if (hideGroups && !look.bandana)
        for (int mi = 0; mi < matCount; ++mi) {
            for (const fox::FmdlTextureRef& t : model.materials()[mi].textures) {
                const QString p = t.path.toLower();
                if (!p.contains(QLatin1String("/hats/"))
                    && !p.contains(QLatin1String("_bdn")))
                    continue;
                if (groupOf[mi] >= 0) hideGroups->insert(groupOf[mi]);
                break;
            }
        }

    if (classified) *classified = n;
}

QVector<LoadedModel> fovaAttachedModels(const fox::FovaFile& fova, int* skipped,
                                       QVector<int>* fileIdxOut, PbrMode mode,
                                       quint64 gearColor)
{
    QVector<LoadedModel> out;
    if (fileIdxOut) fileIdxOut->clear();
    int lost = 0;
    const ArchiveIndex& index = ArchiveIndex::instance();
    for (const fox::FovaAttachment& a : fova.attachments()) {
        // Bone form only, for now. A connect-point attachment needs the
        // WEARER's .fcnp to know where to sit, and guessing the origin would
        // put a pair of glasses inside someone's chest.
        if (a.byConnectPoint || a.modelIndex < 0
            || a.modelIndex >= fova.files().size()) { ++lost; continue; }
        // findByHash returns a pointer into the index's own file vector, and
        // load() wants the INDEX — the distance is the index, and it is stable
        // because nothing rebuilds the vector between these two lines.
        const fox::IndexedFile* f = index.findByHash(fova.files()[a.modelIndex]);
        if (!f) { ++lost; continue; }   // not in this install
        const int fileIdx = int(f - index.files().constData());
        if (fileIdx < 0 || fileIdx >= index.files().size()) { ++lost; continue; }
        LoadedModel lm = modelload::load(fileIdx, mode);
        if (!lm.ok) { ++lost; continue; }
        // An attachment is part of the same character, so it takes the same
        // colour. Resolved against ITS OWN materials — a hat and the head it
        // sits on share no material hashes, so the wearer's override table
        // says nothing about it. Without this the uniform turned tan and the
        // headgear the variation attached stayed white.
        //
        // Only the PBR maps are reloaded, and only under PbrMode::Full. The
        // colour lives entirely in the Layer_Tex slot, and loadBaseTextures()
        // walks only the Base_Tex roles — so re-running it with this override
        // table returns pixel-identical images at the cost of a second full
        // decode of every base map on the attachment.
        if (gearColor != 0 && mode == PbrMode::Full) {
            int painted = 0;
            const FovaOverrides tint =
                layerColorOverrides(lm.model, gearColor, &painted);
            if (painted > 0)
                lm.pbr = loadPbrMaps(lm.model, index.files()[fileIdx].gz,
                                     &lm.pbrMapsFound, &tint);
        }
        out.append(std::move(lm));
        if (fileIdxOut) fileIdxOut->append(fileIdx);
    }
    if (skipped) *skipped = lost;
    return out;
}

QVector<QImage> loadBaseTextures(const fox::FmdlFile& model, bool gz, int* found,
                                 const FovaOverrides* fova, bool lowRes,
                                 const TextureOverlays* overlays,
                                 const TextureForce* force,
                                 const TextureIris* iris)
{
    // Diagnostic (FOXAB_DUMP_TEX=1): what each material actually resolved to,
    // and what survived the decode — the viewport binds exactly these.
    const bool dump = qEnvironmentVariableIsSet("FOXAB_DUMP_TEX");
    QVector<QImage> textures(model.materials().size());
    int n = 0;
    for (int mi = 0; mi < model.materials().size(); ++mi) {
        // A forced map wins outright, and applies even to a material that has
        // no base-colour reference for an override to attach to.
        if (force) {
            const auto fit = force->constFind(mi);
            if (fit != force->constEnd()) {
                fox::FmdlTextureRef sub;
                sub.pathHash = fit.value();
                QString rp;
                sub.path = fox::HashResolver::instance().tryResolve(fit.value(), &rp)
                    ? rp : QString();
                const QImage img = extract::textureImageFor(sub, gz, lowRes);
                if (!img.isNull()) { textures[mi] = img; ++n; continue; }
            }
        }
        for (const fox::FmdlTextureRef& ref : model.materials()[mi].textures) {
            if (!ref.role.contains(QLatin1String("Base_Tex"))) continue;
            fox::FmdlTextureRef use = ref;
            const QImage img = loadWithOverride(ref, mi, gz, fova, &use, lowRes);
            if (dump) {
                int amin = 255, amax = 0;
                qint64 asum = 0, np = 0;
                if (!img.isNull() && img.hasAlphaChannel()) {
                    const QImage a = img.convertToFormat(QImage::Format_RGBA8888);
                    for (int y = 0; y < a.height(); y += 4) {
                        const uchar* row = a.constScanLine(y);
                        for (int x = 0; x < a.width(); x += 4) {
                            const int v = row[x * 4 + 3];
                            amin = qMin(amin, v); amax = qMax(amax, v);
                            asum += v; ++np;
                        }
                    }
                }
                qInfo("dumptex: mat[%d] role=%s path=%s img=%dx%d fmt=%d alpha=%d "
                      "amin=%d amax=%d amean=%d",
                      mi, qUtf8Printable(use.role), qUtf8Printable(use.path),
                      img.width(), img.height(), int(img.format()),
                      img.hasAlphaChannel() ? 1 : 0, np ? amin : -1,
                      np ? amax : -1, np ? int(asum / np) : -1);
            }
            if (!img.isNull()) {
                textures[mi] =
                    img.width() > 1024
                        ? img.scaled(1024, 1024, Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation)
                              .convertToFormat(QImage::Format_RGBA8888)
                        : img;
                if (overlays) {
                    const auto ov = overlays->constFind(mi);
                    if (ov != overlays->constEnd())
                        for (quint64 key : ov.value()) {
                            fox::FmdlTextureRef dec;
                            dec.pathHash = key;
                            QString rp;
                            dec.path =
                                fox::HashResolver::instance().tryResolve(key, &rp)
                                    ? rp : QString();
                            compositeDecal(
                                textures[mi],
                                extract::textureImageFor(dec, gz, lowRes));
                        }
                }
                if (iris) {
                    const auto ir = iris->constFind(mi);
                    if (ir != iris->constEnd()) {
                        fox::FmdlTextureRef eye;
                        eye.pathHash = ir.value();
                        QString rp;
                        eye.path =
                            fox::HashResolver::instance().tryResolve(ir.value(), &rp)
                                ? rp : QString();
                        compositeIris(textures[mi],
                                      extract::textureImageFor(eye, gz, lowRes));
                    }
                }
                ++n;
            }
            break;
        }
    }
    if (found) *found = n;
    return textures;
}

QVector<QImage> loadNormalMaps(const fox::FmdlFile& model, bool gz, int* found,
                               const FovaOverrides* fova)
{
    // Role name resolves to "NormalMap_Tex_NRM" (StrCode64 0xcc4305511ae0 —
    // present on every material in the TPP/MGO corpus surveyed). The raw hex is
    // matched too, so a stripped dictionary still finds it. A material may also
    // carry "SubNormalMap_Tex_NRM" (a second detail layer) and the "CM_" common
    // variant; the plain role wins, and the search is two-pass so the winner
    // does not depend on parameter-table order.
    const auto pick = [](const fox::FmdlMaterialInstance& mat)
        -> const fox::FmdlTextureRef* {
        const fox::FmdlTextureRef* fallback = nullptr;
        for (const fox::FmdlTextureRef& ref : mat.textures) {
            if (ref.role.startsWith(QLatin1String("NormalMap_Tex"))
                || ref.role == QLatin1String("0xcc4305511ae0"))
                return &ref;
            if (!fallback && ref.role.contains(QLatin1String("NormalMap_Tex"))
                && !ref.role.startsWith(QLatin1String("Sub")))
                fallback = &ref;
        }
        return fallback;
    };

    QVector<QImage> maps(model.materials().size());
    int n = 0;
    for (int mi = 0; mi < model.materials().size(); ++mi) {
        const fox::FmdlTextureRef* ref = pick(model.materials()[mi]);
        if (!ref) continue;
        fox::FmdlTextureRef use = *ref;
        const QImage img = loadWithOverride(*ref, mi, gz, fova, &use);
        if (img.isNull()) continue;
        // Unswizzle FIRST, downscale second — and never with an alpha channel
        // present. Qt's smooth scaler routes RGBA through a PREMULTIPLIED
        // intermediate, and in a DXT5nm map alpha IS the x channel, so smooth-
        // scaling the raw map multiplies y by x and divides it back out again.
        // That destroys y exactly where x is near -1 (creases, seams, hard
        // edges — where a normal map carries its signal). Unswizzling to plain
        // RGB888 first removes the alpha channel, so the scale is safe.
        QImage rgb = unswizzleDxt5nm(img);
        if (rgb.isNull()) continue;
        if (rgb.width() > 1024)
            rgb = rgb.scaled(1024, 1024, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation)
                      .convertToFormat(QImage::Format_RGB888);
        maps[mi] = rgb;
        ++n;
    }
    if (found) *found = n;
    return maps;
}

QVector<GLPbrMaterial> loadPbrMaps(const fox::FmdlFile& model, bool gz,
                                   int* found, const FovaOverrides* fova)
{
    QVector<GLPbrMaterial> out(model.materials().size());
    int n = 0;

    // Roles are matched by HASH, not by name text. A dictionary-less install
    // resolves the role to "0x<hash>" and a name comparison would then find
    // nothing at all, quietly turning full PBR back into base+normal on every
    // material. The hash is in the file either way.
    const auto findRole = [](const fox::FmdlMaterialInstance& mat, quint32 role)
        -> const fox::FmdlTextureRef* {
        for (const fox::FmdlTextureRef& r : mat.textures)
            if (r.roleHash32 == role) return &r;
        return nullptr;
    };

    // Decode one role into an RGB888 image capped at 512 px, or a null image.
    // RGB888 and not RGBA: every one of these maps was measured to carry a
    // constant 0xFF alpha, the shader reads only .rgb, and dropping the
    // channel is a quarter of the memory for nothing lost. It also keeps Qt's
    // smooth scaler off its premultiplied path, which is what corrupts a Fox
    // normal map (see loadNormalMaps) and would corrupt an SRM the same way.
    QString grabbedFrom;   // set by grab() to what it actually resolved to
    // `smooth` is false for INDEX maps. An MTM's value is a region number,
    // not a colour: averaging a 33 against a 222 gives 127, which decodes to
    // region 1 — a third material appearing as a stripe along every boundary
    // the artist drew. Every other map here is continuous and wants the
    // smooth path.
    const auto grabMode = [&](int mi, quint32 role, bool smooth) -> QImage {
        grabbedFrom.clear();
        const fox::FmdlTextureRef* ref = findRole(model.materials()[mi], role);
        if (!ref) return QImage();
        fox::FmdlTextureRef used = *ref;
        QImage img = loadWithOverride(*ref, mi, gz, fova, &used);
        if (img.isNull()) return QImage();
        grabbedFrom = used.path.isEmpty()
            ? QStringLiteral("0x%1").arg(used.pathHash, 0, 16)
            : used.path;
        img = img.convertToFormat(QImage::Format_RGB888);
        if (img.width() > 512 || img.height() > 512)
            img = img.scaled(512, 512, Qt::KeepAspectRatio,
                             smooth ? Qt::SmoothTransformation
                                    : Qt::FastTransformation)
                      .convertToFormat(QImage::Format_RGB888);
        ++n;
        return img;
    };
    const auto grab = [&](int mi, quint32 role) {
        return grabMode(mi, role, /*smooth=*/true);
    };

    for (int mi = 0; mi < model.materials().size(); ++mi) {
        const fox::MaterialModel mm =
            fox::classifyShader(model.materials()[mi].shader);
        GLPbrMaterial& p = out[mi];
        p.layerMul = mm.layerMul;
        p.layerBlend = mm.layerBlend;
        p.skin = mm.kind == fox::ShaderKind::Skin;
        p.noMetal = mm.kind == fox::ShaderKind::Skin
                 || mm.kind == fox::ShaderKind::Hair
                 || mm.kind == fox::ShaderKind::Cloth
                 || mm.kind == fox::ShaderKind::Eye;
        p.unlit = mm.kind == fox::ShaderKind::Constant;
        p.hair = mm.kind == fox::ShaderKind::Hair;

        p.materialTypes = mm.materialTypes;

        // The FMTT presets this material selects. MatParamIndex_0..3 carry the
        // row number in x; 256 is the game's own "no material here" and lands
        // on the dielectric default like any other out-of-range index.
        const fox::MaterialPresetTable& presets =
            fox::MaterialPresetTable::instance();
        for (int k = 0; k < 4; ++k) {
            const quint32 want = fox::texrole::kMatParamIndex[k];
            for (const fox::FmdlMaterialParam& par : model.materials()[mi].params) {
                if (par.nameHash32 != want) continue;
                // int() on a NaN or an out-of-range float is undefined and
                // architecture-dependent: x86 yields INT_MIN (which the table
                // bounds-rejects, harmlessly), ARM64 SATURATES — NaN becomes 0,
                // silently selecting a real preset instead of falling back.
                const float raw = par.value[0];
                if (!qIsFinite(raw) || raw < 0.0f || raw > 4096.0f) continue;
                const int idx = int(raw);
                p.presetIndex[k] = idx;
                const fox::MaterialPreset mp = presets.at(idx);
                p.presetF0[k] = mp.f0;
                p.presetSpec[k][0] = mp.specular[0];
                p.presetSpec[k][1] = mp.specular[1];
                p.presetSpec[k][2] = mp.specular[2];
                p.presetTrans[k] = mp.translucency;
                // Railed like every other value this feature reads. The
                // shipped table tops out at 1.0 and FmttFile validates only
                // the file SIZE, so a modded or truncated material_params.fmtt
                // would otherwise put an unbounded float — or a NaN — straight
                // into an additive specular term with no ceiling of its own.
                p.presetAniso[k] = qIsFinite(mp.anisotropicRoughness)
                    ? qBound(0.0f, mp.anisotropicRoughness, 4.0f)
                    : 0.0f;
                break;
            }
        }
        // No fallback from region 0 into the others. It was there for a 4MT
        // material that names only MatParamIndex_0 — and measured across the
        // shipped data that material does not exist: all 304 multi-material
        // materials (151 2MT, 109 3MT, 44 4MT) name all four slots. So the
        // rule could only ever fire on data the game does not ship, and what
        // it did there was spread one region's METAL across the whole mesh.
        // An unnamed slot keeps the dielectric default instead.

        // Layer tiling. Read for every material that carries it; the shader
        // only samples it where a layer is actually bound, so a non-layer
        // material carrying the parameter costs nothing.
        // Both halves set both flags: these are PAIRS, and a material that
        // ships only the V half of one is still a material that carries it.
        // Setting the plain flag on the U arm alone would have left such a
        // material's V rate sitting in layerRepeat on a layerless surface.
        bool sawPlainPair = false, sawSubNormPair = false;
        for (const fox::FmdlMaterialParam& par : model.materials()[mi].params) {
            const float v = par.value[0];
            if (!qIsFinite(v)) continue;
            if (par.nameHash32 == fox::texrole::kURepeatUv) {
                p.layerRepeat[0] = v;
                sawPlainPair = true;
            }
            else if (par.nameHash32 == fox::texrole::kVRepeatUv) {
                p.layerRepeat[1] = v;
                sawPlainPair = true;
            }
            else if (par.nameHash32 == fox::texrole::kUShiftUv) p.layerShift[0] = v;
            else if (par.nameHash32 == fox::texrole::kVShiftUv) p.layerShift[1] = v;
            else if (par.nameHash32 == fox::texrole::kURepeatSubNorm) {
                p.subRepeat[0] = v;
                sawSubNormPair = true;
            }
            else if (par.nameHash32 == fox::texrole::kVRepeatSubNorm) {
                p.subRepeat[1] = v;
                sawSubNormPair = true;
            }
            else if (par.nameHash32 == fox::texrole::kSubNormalBlend)
                p.subBlend = v;
            else if (mm.incidence
                     && par.nameHash32 == fox::texrole::kIncidenceColor) {
                // The STRENGTH is w and the TINT is rgb. Both are read: a
                // third of the materials whose shader names Incidence carry a
                // non-white rim colour, and an earlier pass here took only the
                // w on the strength of a corpus-wide count that had been
                // dominated by the 1,270-odd materials carrying the parameter
                // on a shader that never reads it.
                if (qIsFinite(par.value[3])) p.incidence = par.value[3];
                for (int c = 0; c < 3; ++c)
                    if (qIsFinite(par.value[c]))
                        p.incidenceTint[c] = qBound(0.0f, par.value[c], 1.0f);
            } else if (mm.incidence
                       && par.nameHash32 == fox::texrole::kIncidenceRough) {
                p.incidencePower = v;
            } else if (p.hair
                       && par.nameHash32 == fox::texrole::kAnisoDiffusion) {
                p.hairExponent = v;
            } else if (p.hair
                       && par.nameHash32 == fox::texrole::kHairShiftScale) {
                p.hairShift = v;
            }
        }
        // GATED ON THE SHADER, not on the parameter — and the difference is
        // large. 1362 materials carry Incidence_Color; only 89 name Incidence
        // in their shader. Applying a rim to all 1362 would have put one on
        // every second surface in the game off the back of a parameter the
        // authoring tool writes whether the shader reads it or not, which is
        // the same over-declaration the tiling and sub-normal parameters show.
        // The header states the shader is the authority; this is that rule.
        //
        // Measured over the 89 that DO name it (all 89 carry both parameters):
        //   strength w  0.25 x45, 0.35 x12, 0.2/0.15 x6, 0.5 x5, 0.1/0.3/1.0
        //               x4, and ZERO on 3 — bound and switched off, exactly
        //               like the 17 sub-normals at blend 0.
        //   power x     4.0 x40, 5.0 x26, 0.5 x12, 3.0/6.0 x4, 2.0 x2, 3.5 x1
        // The 0.5 exponent is real and is a BROAD lift rather than a thin rim,
        // so the lower bound has to sit below 1 or twelve materials would be
        // silently sharpened. Bounds are just sanity rails against a modded or
        // truncated float, not a reshaping of the shipped range.
        p.incidence = qBound(0.0f, p.incidence, 1.0f);
        p.incidencePower = qBound(0.05f, p.incidencePower, 16.0f);
        // The shipped range is 16..64 and 0.5..20; the bounds are sanity rails
        // against a modded float, not a reshaping of it. A pow() exponent
        // below 1 broadens the lobe until it is not a highlight at all, and
        // above ~256 it disappears into a single pixel.
        p.hairExponent = qBound(1.0f, p.hairExponent, 256.0f);
        p.hairShift = qBound(0.0f, p.hairShift, 32.0f);
        // The tiling rates get an upper bound too. The shipped data tops out
        // at 150 and the shader does not care what it is handed, but the .glb
        // exporter BAKES the layer by sampling it per texel — and there
        // int(floor(u * width)) with a rate of 1e30 is signed overflow, which
        // is undefined rather than merely wrong. 4096 is far above anything
        // the data contains and far below where a float times a texture
        // dimension can leave int range.
        for (int k = 0; k < 2; ++k) {
            p.layerRepeat[k] = qBound(-4096.0f, p.layerRepeat[k], 4096.0f);
            p.subRepeat[k] = qBound(-4096.0f, p.subRepeat[k], 4096.0f);
            p.layerShift[k] = qBound(-4096.0f, p.layerShift[k], 4096.0f);
        }
        // Kill switch, like FOXAB_NO_SUBNORMAL: the rim touches every lit
        // fragment of the materials that carry it, so being able to take it
        // back out of the same binary is what makes an A/B mean anything.
        static const bool kNoInc = qEnvironmentVariableIsSet("FOXAB_NO_INCIDENCE");
        if (kNoInc) p.incidence = 0.0f;
        // THE NON-LAYER HALF OF URepeat_UV. It was left unapplied for a long
        // time as "unexplained"; measured across the whole shipped corpus it
        // is not unexplained at all, and the rule is exclusive with no
        // exceptions either way:
        //
        //   a material with a LAYER      the plain pair tiles the layer, and a
        //                                sub-normal (if it has one) gets the
        //                                separately-named URepeat_SubNorm_UV.
        //                                All 27 layer-family materials that
        //                                bind a sub-normal carry that pair.
        //   a material with NO layer     the plain pair tiles the SUB-NORMAL.
        //                                All 31 such materials bind a
        //                                SubNormalMap_Tex_NRM, and NOT ONE of
        //                                them carries the SubNorm-named pair —
        //                                there is nothing to disambiguate from,
        //                                so the plain name is used.
        //
        // The decisive single model is dds0_main1_def, the Diamond Dogs
        // fatigues, which carries both readings at once: mat 0 (Cloth_Dirty,
        // no layer) puts 100 in the plain pair, while mat 2 (LayerMul_SubNorm)
        // drops the plain pair to 1 and moves its 50 into URepeat_SubNorm_UV.
        // What this fixes visually is real: the fatigues, the dogs, the horses
        // and Quiet's outfit were all drawing their cloth weave at 1:1 instead
        // of the 80x-150x the material asks for.
        //
        // HAIR is a third population and is still left alone. 51 fox3DDF_Hair
        // materials carry the plain pair; none of them binds a sub-normal or a
        // layer, their roles are only Base / Specular / Shift / Translucent,
        // and their values are 1.0 (30 of them) or 5.0 (21) and never higher.
        // So 21 materials in the whole corpus have a non-unit plain repeat
        // whose target is undetermined — down from the 102 this comment used
        // to have to admit to — and guessing at Shift_Tex_LIN on a hunch is
        // not worth 21 materials.
        // Kill switch, like the other two: this changes the surface detail of
        // every cloth material in the game at once, so being able to take it
        // out of the same binary is what makes the before/after mean anything.
        static const bool kNoPlainSub =
            qEnvironmentVariableIsSet("FOXAB_NO_PLAINSUBREPEAT");
        // Gated on whether the material DECLARES a sub-normal, not on the
        // blend weight. Two reasons, both measured:
        //   • one of the 31 sets SubNormal_Blend to zero — bound and switched
        //     off — and gating on the weight left its rate of 100 showing
        //     against "Layer" in the inspector on a material with no layer,
        //     which is the display this whole block exists to remove;
        //   • 51 of the 82 materials the other three conditions select are
        //     HAIR, which binds no sub-normal at all and carries no blend
        //     parameter. Reinterpreting their rate as a sub-normal's would be
        //     asserting something about a map that is not there.
        // The reference is looked up rather than the decoded image tested,
        // because the sub-normal is not decoded until further down and a
        // material that declares one is making the statement either way.
        const bool declaresSubNormal =
            findRole(model.materials()[mi], fox::texrole::kSubNormal) != nullptr;
        if (!kNoPlainSub && sawPlainPair && !sawSubNormPair
            && !mm.colourable() && declaresSubNormal) {
            p.subRepeat[0] = p.layerRepeat[0];
            p.subRepeat[1] = p.layerRepeat[1];
            // …and the layer's own rate goes back to neutral, because on this
            // material the plain pair was never about a layer. Leaving it set
            // would have shown a rate of 100 against "Layer" in the material
            // inspector on a material that has no layer at all.
            p.layerRepeat[0] = p.layerRepeat[1] = 1.0f;
        }

        // A repeat of ZERO collapses the layer to one texel. Four materials
        // ship it (dlb0_main0, dlb0_plym0, mnt0 mats 1 and 3) and it is not a
        // rate — treat it as "not set" rather than painting the whole garment
        // the colour of one corner of the swatch. The test is a POSITIVE
        // threshold rather than == 0, so a denormal or a negative — neither of
        // which the shipped data contains, but both of which a modded or
        // truncated FMDL can produce, since the parser reinterprets raw file
        // bits as float — lands on the same neutral instead of sneaking past.
        for (int k = 0; k < 2; ++k)
            if (!(p.layerRepeat[k] > 1e-6f)) p.layerRepeat[k] = 1.0f;
        for (int k = 0; k < 2; ++k)
            if (!(p.subRepeat[k] > 1e-6f)) p.subRepeat[k] = 1.0f;
        if (!qIsFinite(p.subBlend) || p.subBlend < 0.0f) p.subBlend = 0.0f;

        // The sub-normal itself, and ONLY when the material asks for it to be
        // visible. 17 of the 78 materials that bind one set SubNormal_Blend to
        // zero — bound and switched off — and decoding a detail map to
        // multiply it by nothing is the one cost this can avoid outright.
        // Kill switch, in the same spirit as the other env-gated diagnostics:
        // the sub-normal changes shading everywhere at once, so being able to
        // take it back out in the same binary is what makes a before/after
        // comparison mean anything.
        // The hair strand map. Loaded only for a hair shader — 5 materials
        // outside the Hair family bind a Shift_Tex_LIN and none of their
        // shaders reads one, which is the same over-declaration everything
        // else in this data shows. Kill switch for the same A/B reason as the
        // other two.
        static const bool kNoHair = qEnvironmentVariableIsSet("FOXAB_NO_HAIR");
        if (kNoHair) p.hair = false;
        if (p.hair) {
            p.shift = grab(mi, fox::texrole::kShift);
            p.shiftSource = grabbedFrom;
        }

        static const bool kNoSub = qEnvironmentVariableIsSet("FOXAB_NO_SUBNORMAL");
        if (kNoSub) p.subBlend = 0.0f;
        if (p.subBlend > 0.0f) {
            const QImage sub = grab(mi, fox::texrole::kSubNormal);
            if (!sub.isNull()) {
                // Through the same DXT5nm unswizzle the base normal takes.
                // These are _NRM textures with x in alpha and y in green, and
                // handing the raw map to the GPU would light the surface with
                // the alpha channel.
                p.subNormal = unswizzleDxt5nm(sub);
                p.subNormalSource = grabbedFrom;
            }
            if (p.subNormal.isNull()) p.subBlend = 0.0f;
        }

        // The region map itself, loaded only for a multi-material shader —
        // with one region there is nothing to select between.
        if (mm.materialTypes > 1) {
            p.matParamMap =
                grabMode(mi, fox::texrole::kMatParamMap, /*smooth=*/false);
            p.matParamMapSource = grabbedFrom;
        }

        p.material = grab(mi, fox::texrole::kSpecular);
        p.materialSource = grabbedFrom;
        if (!p.material.isNull()) p.materialRole = fox::texrole::kSpecular;
        // Some materials carry a dedicated RoughnessMap instead of packing
        // roughness into the SRM's green channel. It is the SECOND choice, not
        // a replacement: where both exist the SRM is the one the shader reads.
        if (p.material.isNull()) {
            p.material = grab(mi, fox::texrole::kRoughness);
            p.materialSource = grabbedFrom;
            if (!p.material.isNull()) p.materialRole = fox::texrole::kRoughness;
        }
        p.translucent = grab(mi, fox::texrole::kTranslucent);
        p.translucentSource = grabbedFrom;

        // The layer pair is loaded only when the SHADER asks for it. 315 of
        // the 412 Layer_Tex references in the shipped models point at
        // cm_flat_white — a placeholder the game replaces at runtime — so
        // loading a layer map because one is bound would spend the decode on
        // a white square for most materials that have one.
        if (mm.colourable()) {
            p.layer = grab(mi, fox::texrole::kLayer);
            p.layerSource = grabbedFrom;
            p.layerMask = grab(mi, fox::texrole::kLayerMask);
            p.layerMaskSource = grabbedFrom;
        }
    }

    if (found) *found = n;
    return out;
}


void skinToneOverrides(const fox::FmdlFile& model, const QString& bodyPathNoExt,
                       FovaOverrides* out, TextureForce* force)
{
    if (!out || bodyPathNoExt.isEmpty()) return;
    const quint64 key =
        fox::hashFileNameWithExtension(bodyPathNoExt + QLatin1String(".ftex"));
    for (const int mi : skinMaterials(model)) {
        bool placed = false;
        for (const fox::FmdlTextureRef& t : model.materials()[mi].textures) {
            if (!t.role.contains(QLatin1String("Base_Tex"))) continue;
            out->insert({mi, t.roleHash32}, key);
            placed = true;
            break;
        }
        if (!placed && force) force->insert(mi, key);
    }
}

QVector<int> skinMaterials(const fox::FmdlFile& model)
{
    QVector<int> out;
    for (int mi = 0; mi < model.materials().size(); ++mi) {
        bool translucent = false, layered = false, hairOnSkin = false;
        for (const fox::FmdlTextureRef& t : model.materials()[mi].textures) {
            if (t.role.contains(QLatin1String("Translucent"))) translucent = true;
            if (t.role.startsWith(QLatin1String("Layer"))) layered = true;
            const QString p = t.path.toLower();
            if (p.contains(QLatin1String("/berd/"))
                || p.contains(QLatin1String("/ebrw/")))
                hairOnSkin = true;
        }
        if (translucent && !layered && !hairOnSkin) out.append(mi);
    }
    return out;
}

int colourableMaterialCount(const fox::FmdlFile& model)
{
    int n = 0;
    for (const fox::FmdlMaterialInstance& mat : model.materials())
        if (fox::classifyShader(mat.shader).colourable()) ++n;
    return n;
}

FovaOverrides layerColorOverrides(const fox::FmdlFile& model,
                                  quint64 swatchPathHash, int* painted)
{
    FovaOverrides out;
    int n = 0;
    if (swatchPathHash != 0) {
        for (int mi = 0; mi < model.materials().size(); ++mi) {
            const fox::FmdlMaterialInstance& mat = model.materials()[mi];
            if (!fox::classifyShader(mat.shader).colourable()) continue;
            // The material must actually HAVE BOTH slots.
            //
            // The layer alone is not enough: the composite needs a mask to say
            // WHERE the colour applies, and the viewport draws nothing when
            // one of the pair is missing. Counting a layer-only material as
            // painted made the log claim a colour had been applied to
            // surfaces that went on rendering exactly as before — 28 of
            // Survive's 362 layer materials are in that state.
            bool hasLayer = false, hasMask = false;
            for (const fox::FmdlTextureRef& r : mat.textures) {
                if (r.roleHash32 == fox::texrole::kLayer) hasLayer = true;
                else if (r.roleHash32 == fox::texrole::kLayerMask) hasMask = true;
            }
            if (!hasLayer || !hasMask) continue;
            out.insert({mi, fox::texrole::kLayer}, swatchPathHash);
            ++n;
        }
    }
    if (painted) *painted = n;
    return out;
}

void keepBoneSubtree(QVector<GLMeshUpload>& uploads, const fox::FmdlFile& model,
                     quint32 rootBone)
{
    const QVector<bool> inSubtree = model.boneSubtreeMask(rootBone);
    if (inSubtree.isEmpty()) return;   // no such bone here — leave it whole

    for (GLMeshUpload& up : uploads) {
        const int verts = up.interleaved.size() / kVertexFloats;
        if (up.joints.size() != verts * 4 || up.weights.size() != verts * 4) {
            // Unskinned mesh: it belongs to the body by definition — a head is
            // always skinned — so it goes entirely.
            up.indices.clear();
            continue;
        }
        // A vertex counts as head when its HEAVIEST bone is in the subtree.
        QVector<bool> headVert(verts, false);
        for (int v = 0; v < verts; ++v) {
            int best = -1;
            float bestW = -1.0f;
            for (int k = 0; k < 4; ++k)
                if (up.weights[v * 4 + k] > bestW) {
                    bestW = up.weights[v * 4 + k];
                    best = up.joints[v * 4 + k];
                }
            headVert[v] = best >= 0 && best < inSubtree.size() && inSubtree[best];
        }
        // ANY vertex, not all three: the ring of triangles that straddles the
        // neck seam belongs to both, and dropping it would leave a hole at the
        // jaw. Keeping it costs a few triangles that the collar covers anyway.
        QVector<quint32> kept;
        kept.reserve(up.indices.size());
        for (int t = 0; t + 2 < up.indices.size(); t += 3) {
            const quint32 a = up.indices[t], b = up.indices[t + 1],
                          c = up.indices[t + 2];
            if ((a < quint32(verts) && headVert[a])
                || (b < quint32(verts) && headVert[b])
                || (c < quint32(verts) && headVert[c])) {
                kept.append(a);
                kept.append(b);
                kept.append(c);
            }
        }
        up.indices = std::move(kept);
    }
    uploads.erase(std::remove_if(uploads.begin(), uploads.end(),
                                 [](const GLMeshUpload& u) {
                                     return u.indices.isEmpty();
                                 }),
                  uploads.end());
}

QVector<GLMeshUpload> buildUploads(const fox::FmdlFile& model)
{
    QVector<GLMeshUpload> uploads;
    for (int mi = 0; mi < model.meshes().size(); ++mi) {
        const fox::FmdlMesh& mesh = model.meshes()[mi];
        const int vertexCount = mesh.positions.size() / 3;
        if (vertexCount == 0 || mesh.triangles.isEmpty()) continue;
        GLMeshUpload up;
        up.materialSlot = mesh.materialInstanceIndex;
        up.groupId = mesh.meshGroupIndex;
        // The mesh's index in ITS OWN model. A scene assembled from several
        // models re-bases this into a scene-wide id; a single model can use it
        // as it stands.
        up.meshId = mi;
        up.interleaved.reserve(vertexCount * kVertexFloats);
        const bool hasNormals = mesh.normals.size() == vertexCount * 3;
        const bool hasUv = mesh.uv0.size() == vertexCount * 2;
        const bool hasTangents = mesh.tangents.size() == vertexCount * 4;
        for (int v = 0; v < vertexCount; ++v) {
            up.interleaved.append(mesh.positions[v * 3 + 0]);
            up.interleaved.append(mesh.positions[v * 3 + 1]);
            up.interleaved.append(mesh.positions[v * 3 + 2]);
            up.interleaved.append(hasNormals ? mesh.normals[v * 3 + 0] : 0.0f);
            up.interleaved.append(hasNormals ? mesh.normals[v * 3 + 1] : 1.0f);
            up.interleaved.append(hasNormals ? mesh.normals[v * 3 + 2] : 0.0f);
            up.interleaved.append(hasUv ? mesh.uv0[v * 2 + 0] : 0.0f);
            up.interleaved.append(hasUv ? mesh.uv0[v * 2 + 1] : 0.0f);
            // Tangent (xyz) + bitangent sign (w). Absent on a handful of meshes
            // (5 of 632 in the survey) — those fall back to the vertex normal
            // in the shader, which just disables the normal-map perturbation.
            up.interleaved.append(hasTangents ? mesh.tangents[v * 4 + 0] : 0.0f);
            up.interleaved.append(hasTangents ? mesh.tangents[v * 4 + 1] : 0.0f);
            up.interleaved.append(hasTangents ? mesh.tangents[v * 4 + 2] : 0.0f);
            up.interleaved.append(hasTangents ? mesh.tangents[v * 4 + 3] : 1.0f);
        }
        up.indices.reserve(mesh.triangles.size());
        for (const quint16 t : mesh.triangles) up.indices.append(t);

        // Skinning data for animated playback (palette-relative → skeleton).
        if (mesh.boneIndices.size() == vertexCount * 4
            && mesh.boneWeights.size() == vertexCount * 4
            && !mesh.palette.isEmpty()) {
            up.joints.resize(vertexCount * 4);
            up.weights.resize(vertexCount * 4);
            for (int v = 0; v < vertexCount * 4; ++v) {
                const int pal = mesh.boneIndices[v];
                up.joints[v] =
                    pal < mesh.palette.size() ? mesh.palette[pal] : quint16(0);
                up.weights[v] = mesh.boneWeights[v];
            }
        }
        uploads.append(up);
    }
    return uploads;
}

GLSkeletonUpload buildSkeleton(const fox::FmdlFile& model)
{
    GLSkeletonUpload skeleton;
    for (const fox::FmdlBone& b : model.bones()) {
        if (b.parentIndex < 0 || b.parentIndex >= model.bones().size()) continue;
        const fox::FmdlBone& p = model.bones()[b.parentIndex];
        skeleton.lines << p.worldPos[0] << p.worldPos[1] << p.worldPos[2]
                       << b.worldPos[0] << b.worldPos[1] << b.worldPos[2];
    }
    // One entry per BONE, in bone order — including the roots, which the line
    // loop above skips because a root has no parent to draw a bone TO. The
    // order is the contract that lets the viewport pose these labels: a scene's
    // combined palette is its parts' palettes concatenated in exactly this
    // order.
    skeleton.boneNames.reserve(int(model.bones().size()));
    skeleton.bonePositions.reserve(int(model.bones().size()) * 3);
    for (const fox::FmdlBone& b : model.bones()) {
        skeleton.boneNames << b.name;
        skeleton.bonePositions << b.worldPos[0] << b.worldPos[1] << b.worldPos[2];
    }
    return skeleton;
}

LoadedModel load(int fileIdx, PbrMode mode)
{
    LoadedModel out;
    const ArchiveIndex& index = ArchiveIndex::instance();
    if (fileIdx < 0 || fileIdx >= index.files().size()) {
        out.error = QStringLiteral("bad file index");
        return out;
    }
    const IndexedFile& f = index.files()[fileIdx];
    const QByteArray data = index.readFile(f);
    if (data.isEmpty() || !out.model.parse(data)) {
        out.error = data.isEmpty() ? QStringLiteral("could not read entry")
                                   : out.model.errorString();
        return out;
    }
    out.textures = loadBaseTextures(out.model, f.gz, &out.texturesFound);
    out.normalMaps = loadNormalMaps(out.model, f.gz, &out.normalMapsFound);
    if (mode == PbrMode::Full)
        out.pbr = loadPbrMaps(out.model, f.gz, &out.pbrMapsFound);
    out.uploads = buildUploads(out.model);
    out.skeleton = buildSkeleton(out.model);
    out.ok = true;
    return out;
}

LoadedModel loadForThumbnail(int fileIdx)
{
    LoadedModel out;
    const ArchiveIndex& index = ArchiveIndex::instance();
    if (fileIdx < 0 || fileIdx >= index.files().size()) {
        out.error = QStringLiteral("bad file index");
        return out;
    }
    const IndexedFile& f = index.files()[fileIdx];
    const QByteArray data = index.readFile(f);
    if (data.isEmpty() || !out.model.parse(data)) {
        out.error = data.isEmpty() ? QStringLiteral("could not read entry")
                                   : out.model.errorString();
        return out;
    }
    out.textures =
        loadBaseTextures(out.model, f.gz, &out.texturesFound, nullptr, true);
    out.uploads = buildUploads(out.model);
    out.ok = true;
    return out;
}

}  // namespace modelload
