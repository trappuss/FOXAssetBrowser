// GlbExporter.h — write a parsed FMDL as a single self-contained .glb
// (glTF 2.0 binary): meshes with positions/normals/UVs, full skeleton as a
// node hierarchy with skinning (joints/weights, inverse bind matrices from the
// translation-only bind pose), base-color textures embedded as PNG, and
// tangent-space normal maps (unswizzled from Fox DXT5nm) with TANGENT data.
//
// No external glTF library: the format is JSON + one binary buffer, written
// directly (Qt JSON + QByteArray), so the exporter adds no dependencies.
#pragma once
#include <QImage>
#include <QSet>
#include <QString>
#include <QVector>
#include <functional>

#include "anim/AnimMath.h"

namespace fox {
class FmdlFile;
struct ConnectPoint;
}
// The viewport's shading set. Forward-declared: this header is included from
// several tabs and only needs the pointer type.
struct GLPbrMaterial;

namespace glb {

// `textures` maps material-instance index → decoded base-color image (null
// entries export as untextured). Returns false + `error` on failure.
// `pose` (optional): one skin matrix per bone — the current animation frame is
// then BAKED into the vertices and the file exports as a static posed mesh
// (no skeleton/skin), a snapshot of exactly what the viewport shows.
// `normalMaps` (optional, parallel to `textures`) are Fox DXT5nm tangent-space
// maps; they are unswizzled to plain RGB and written as glTF normalTextures,
// with a TANGENT attribute so importers use the engine's own tangent frame.
// What "export" means this time. Every default is what this exporter did
// before the struct existed, so a caller that passes nothing gets the file it
// always got. The user-facing dialog is fox::ExportOptions (export/
// ExportOptions.h); this is the writer's own view of the same five decisions,
// kept separate so the model layer does not include a widget header.
struct SceneOptions {
    // Applied through the scene's ROOT NODE, not to the vertices: a node
    // transform is one line of JSON and cannot cost a coordinate its
    // precision. glTF applies an ancestor transform to the joints as well as
    // to the meshes, so a skinned export scales and rotates with everything
    // else — which a per-vertex scale would not have done for the skeleton.
    double scale = 1.0;
    bool zUp = false;
    // False writes no bone hierarchy and no skin: the mesh alone, in bind
    // pose. A POSED export is already static whichever way this is set.
    bool skeleton = true;
    // False exports each base map as it ships instead of multiplying the
    // runtime colour layer through its mask first. The SRM still becomes glTF
    // occlusion/roughness either way — that is a different use of the same
    // material set, and switching one off must not switch off the other.
    bool bakeColourLayer = true;
    bool normalMaps = true;
    // Write each part's connect points (.fcnp sockets — CNP_RIGHT_HAND,
    // CNP_HEAD, CNP_ASRROOT) as EMPTY NODES. They carry no geometry, so they
    // cost a node each and nothing else, and they are the only thing in the
    // file that says where a hat, a scope or a hand goes. Off by default here
    // because every default in this struct is what the exporter did before it
    // existed; the user-facing setting defaults ON.
    bool connectPoints = false;
};

// One animation clip, BAKED — one sample per frame of the posed skeleton.
//
// Baked rather than transcoded on purpose. A Fox clip's keys belong to the
// SEGMENTS of a rig unit, and the pose that reaches the screen is those keys
// plus the .frig's rig-unit rules, analytic two-bone IK and the .frdv help
// bones — none of which glTF can express. Sampling the solver once per frame is
// the only form in which the exported motion IS the motion the viewport played;
// a "cleverer" curve export would be a different animation that happens to
// agree at the keys.
struct GlbAnimation {
    QString name;
    // Samples per second of `pose`'s index. Fox motion is authored at 30, which
    // is also what the viewport plays.
    float fps = 30.0f;
    int sampleCount = 0;
    // Fills `out` with the FINAL WORLD matrix of every bone of scene part
    // `part`, in that model's bone order — i.e. animpose::buildWorld.
    //
    // The exporter calls this SAMPLE BY SAMPLE, every part of one sample
    // before the next sample. That order is the whole reason the callback
    // takes a part index instead of living on the part: a composed character
    // is posed as a scene (one host elects the palette, fragments borrow it,
    // attachments are seated from it), so a caller that had to answer for one
    // part at a time would pose the whole scene once per part per frame.
    std::function<void(int part, int sample, QVector<animmath::Mat4>* out)> pose;
};

bool exportGlb(const fox::FmdlFile& model, const QVector<QImage>& textures,
               const QString& outPath, QString* error = nullptr,
               const QVector<animmath::Mat4>* pose = nullptr,
               const QVector<QImage>* normalMaps = nullptr,
               const SceneOptions& opts = SceneOptions());

// Multi-model scene export (the Customize composer): every part lands in ONE
// .glb — each with its own materials/textures, and its own skeleton + skin
// when unposed. Parts with a `pose` are baked static (like exportGlb).
struct ScenePart {
    const fox::FmdlFile* model = nullptr;
    const QVector<QImage>* textures = nullptr;          // may be null
    const QVector<QImage>* normalMaps = nullptr;        // may be null
    const QVector<animmath::Mat4>* pose = nullptr;      // null = bind pose
    // Rigid transform applied to EVERY vertex (attachments: an item seated
    // at a connect point). Implies a static export (no skeleton) for this
    // part; composes after `pose` when both are set.
    const animmath::Mat4* rigid = nullptr;
    // ── Rest-pose alignment, WITHOUT stripping the rig ───────────────────
    // A part whose skeleton is a fragment of another's — a hairstyle, a cap —
    // is authored against a bone the wearer places elsewhere, so it needs a
    // rigid offset to sit where the viewport draws it. With a clip loaded the
    // composer folds that offset into the part's palette and this is zero.
    // With NO clip there is no palette to fold into, and `rigid` cannot carry
    // it because setting `rigid` makes the part export static — so a rigged
    // no-clip export dropped the offset and put the cap at the model origin.
    //
    // Written as a wrapper NODE over everything the part contributed, which
    // is the same mechanism the scale/up-axis wrapper below uses and works for
    // the same reason: glTF ignores a skinned mesh node's own transform, but
    // not its joints' ancestors, so wrapping both moves the skin and the
    // static meshes alike and moves neither twice.
    float restOffset[3] = {0, 0, 0};
    // When non-zero, export ONLY the triangles skinned to the subtree under
    // this bone (a StrCode32). The viewport does the same thing for a base body
    // worn under clothing, and an export that quietly disagreed with what is on
    // screen would be worse than either choice on its own.
    quint32 subtreeOnly = 0;
    // Mesh GROUPS to leave out, by FmdlMesh::meshGroupIndex. The viewport
    // erases these before it uploads anything — a variation's hidden bandanna,
    // the baked head under an attached one, a submesh the user unticked — and
    // an export that kept them put two heads on the character.
    QSet<int> hiddenGroups;
    // Individual meshes to leave out, by index into this model's own
    // meshes(). Mesh GROUPS are what a variation hides; individual meshes are
    // what the submesh tree switches off, and both had to reach the file for
    // it to match the screen.
    QSet<int> hiddenMeshes;
    // The viewport's own material set for this model, parallel to `textures`.
    // Given it, the exporter BAKES the runtime colour layer into the base map
    // (a colourable garment otherwise exports the white it ships as) and
    // writes the SRM out as glTF occlusion + roughness. Null exports the base
    // map raw, which is what this exporter did before.
    const QVector<GLPbrMaterial>* pbr = nullptr;
    // This part's connect points, from its sibling .fcnp. Written as empty
    // nodes named for the point, parented to the BONE each hangs off when the
    // part exports rigged — so they follow the animation — and placed in the
    // part's own space when it exports static.
    const QVector<fox::ConnectPoint>* connectPoints = nullptr;
};
// `animations` (optional) are clips written over the scene's SKINNED parts —
// one glTF animation each, with channels on every part that the clip drives.
// A part that exports static (a baked pose, an attachment's rigid seat, or
// skeleton off) has no joint nodes and takes no channels: it stays where the
// export found it, and the log says how many parts that was.
// `animationsWritten` (optional) reports how many clips actually reached the
// file. It exists because "the writer succeeded" and "the clips are in there"
// are different facts: a clip whose parts all export static, or one whose pose
// callback answers for the wrong skeleton, is dropped with a warning while the
// write itself succeeds — and a caller that reported its own request back to
// the user then claimed clips the file does not contain.
bool exportGlbScene(const QVector<ScenePart>& parts, const QString& outPath,
                    QString* error = nullptr,
                    const SceneOptions& opts = SceneOptions(),
                    const QVector<GlbAnimation>* animations = nullptr,
                    int* animationsWritten = nullptr);

}  // namespace glb
