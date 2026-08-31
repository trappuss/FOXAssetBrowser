// AnimPose.h — gani frame → per-bone skin matrices for an FMDL skeleton.
// Port of Fox_Parser's AnimSkinner.BuildPalette (itself ported from FoxBrowser):
//   • bone drives resolved through the .frig when available (rig units → bones),
//     with direct StrCode32 name matching as the humanoid fallback;
//   • LOCAL vs WORLD-space rig units, Root/Transform translation rules, the
//     root-frame view shift that keeps the character centred while re-applying
//     its net turn;
//   • analytic two-bone IK for TwoBone (legs) and Arm chains, bend plane from
//     the gani's pole channel (the reference's proven "legacy" solver — the
//     bit-exact engine solver is a follow-up).
// Help bones (_HLP twist/roll) and AnimalLeg quadruped chains are not driven
// yet; those bones hold bind pose.
#pragma once
#include <QHash>
#include <QVector>

#include "anim/AnimMath.h"
#include "anim/FrdvFile.h"
#include "fox/FmdlFile.h"
#include "fox/FrigFile.h"
#include "fox/GaniAnim.h"

namespace animpose {

// One skin matrix per FMDL bone (invBind · animWorld · viewShift), row-major
// row-vector — apply as skinned = v · M. Identity for undriven bones.
// `frdv` (optional) runs the help-bone operators over the posed skeleton
// (twist/roll *_HLP bones) after FK + IK, before skinning.
// Pose a model whose skeleton is a FRAGMENT of another's, by reusing the
// host's palette outright.
//
// Some parts do not carry the character's whole skeleton. An avatar hairstyle
// carries six bones — SKL_002_CHEST, _003_NECK, _004_HEAD and three helpers —
// and makes the CHEST a root at the origin. In the body that bone is a child
// of the spine sitting at y = 0.154, so every bone below it in the fragment
// has a rest position short by that much and a chain that accumulates from the
// wrong place. Driving it directly from the clip poses it as though the
// character's chest were at the origin: at bind pose it looks correct, and the
// moment anything plays the hair detaches and floats.
//
// The meshes are not the problem — a hairstyle's vertices are authored in the
// same world space as the body's. So the fix is not to correct the fragment's
// chain but to stop building one: a bone shared by name takes the host's skin
// matrix exactly, and a bone the host does not have stays at bind.
//
// `matched` (optional) reports how many bones were found in the host. Zero
// means these two models share no skeleton and the caller should pose the part
// on its own after all.
// A bone the host does NOT have does not stay at bind: it takes the matrix of
// its nearest matched ANCESTOR in the fragment. The host's palette carries the
// clip's root motion and view shift, so leaving a bone at identity does not
// leave it where it was — it leaves it where the character STARTED, and a
// ponytail's own helper bones would hang at the spot the walk began while the
// rest of the hair walked away. Riding the parent is what those bones do in
// the game anyway; they are physics-driven, and this project does not run the
// sim.
QVector<animmath::Mat4> borrowPalette(const fox::FmdlFile& fragment,
                                      const fox::FmdlFile& host,
                                      const QVector<animmath::Mat4>& hostPalette,
                                      int* matched = nullptr,
                                      // Prebuilt host name->index map. setFrame
                                      // calls this once per part and once per
                                      // attachment, and rebuilding a 300-entry
                                      // hash for each of them every frame is
                                      // pure waste.
                                      const QHash<quint32, int>* hostIndex = nullptr,
                                      // Bones the borrow could resolve NEITHER
                                      // by name NOR through an ancestor — i.e.
                                      // a second root the host does not carry.
                                      // ZERO is what makes a borrow usable, and
                                      // it is not the same question as how MANY
                                      // bones matched: a cap shares exactly one
                                      // bone with the body and every other bone
                                      // it has hangs off that one, so the borrow
                                      // resolves all four and is exactly right.
                                      // See CustomizeTab::setFrame.
                                      int* unresolved = nullptr);

// name StrCode32 -> bone index for a model, first occurrence wins. Exposed so
// a caller posing many parts against one host builds it once.
QHash<quint32, int> boneIndexByHash(const fox::FmdlFile& m);

// True when `fragment`'s skeleton is a truncated copy of `host`'s: its root
// bone exists in the host WITH A PARENT, so everything the host has above that
// root is missing here. The precise test for the case borrowPalette() exists
// to handle — a part that merely has fewer bones, but roots them where the
// host does, is not a fragment and poses correctly on its own.
bool isSkeletonFragment(const fox::FmdlFile& fragment, const fox::FmdlFile& host,
                        const QHash<quint32, int>* hostIndex = nullptr);

// The smallest bone count a model must have before it is trusted as the HOST
// everything else borrows from. Measured: an avatar hairstyle carries 6 bones
// and a pair of glasses fewer; the bodies carry 96, 136 and 297. Without this
// a scene of nothing but fragments would elect one fragment as the host and
// propagate its own broken chain to the rest — each part is wrong on its own
// in that case, which is bad, but correlating the error is worse and harder
// to diagnose.
constexpr int kMinHostBones = 16;

QVector<animmath::Mat4> buildPalette(const fox::FmdlFile& model,
                                     const fox::GaniAnim& anim, float frame,
                                     const fox::FrigFile* frig,
                                     const frdv::FrdvFile* frdvDrv = nullptr,
                                     int* drivenBones = nullptr);

// The same pose, one step earlier: the WORLD matrix of every bone with the
// clip's view shift already applied, and no inverse bind. This is what an
// EXPORT needs — glTF animates node transforms and multiplies the inverse bind
// itself, so handing it a skin matrix would apply that inverse twice.
//
// Same solver as buildPalette (they share poseWorld), so a clip exported this
// way is the pose the viewport drew, not a second opinion about it.
QVector<animmath::Mat4> buildWorld(const fox::FmdlFile& model,
                                   const fox::GaniAnim& anim, float frame,
                                   const fox::FrigFile* frig,
                                   const frdv::FrdvFile* frdvDrv = nullptr,
                                   int* drivenBones = nullptr);

}  // namespace animpose
