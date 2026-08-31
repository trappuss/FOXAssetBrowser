// AnimPose.cpp — see AnimPose.h.
#include "anim/AnimPose.h"

#include <QHash>
#include <QSet>

namespace animpose {

using namespace animmath;
using fox::FrigFile;
using fox::GaniAnim;
using fox::GaniChannel;
using fox::GaniTrack;
using fox::Quat;

namespace {

bool carriesTranslation(FrigFile::RigUnitType t)
{
    return t == FrigFile::RigUnitType::Transform
        || t == FrigFile::RigUnitType::LocalTransform
        || t == FrigFile::RigUnitType::LocalTransformSrt;
}

// Analytic two-bone IK (AnimSkinner.SolveTwoBone legacy path). Overwrites
// animWorld[root] and animWorld[mid]; end + children follow in the FK loop.
void solveTwoBone(const fox::FmdlFile& model, QVector<Mat4>& animWorld, int root,
                  int mid, int end, const Vec3& goal, const Quat* poleQuat)
{
    const auto bindW = [&](int i) {
        const auto& b = model.bones()[i];
        return Vec3(b.worldPos[0], b.worldPos[1], b.worldPos[2]);
    };
    const Vec3 Pa = bindW(root), Pb = bindW(mid), Pe = bindW(end);
    const float L1 = (Pb - Pa).length(), L2 = (Pe - Pb).length();
    if (L1 < 1e-5f || L2 < 1e-5f) return;

    const Vec3 rootPos = animWorld[root].translationRow();
    Vec3 toGoal = goal - rootPos;
    float dist = toGoal.length();
    if (dist < 1e-5f) return;

    dist = qBound(std::fabs(L1 - L2) + 1e-4f, dist, L1 + L2 - 1e-4f);
    const Vec3 dir = toGoal / toGoal.length();

    // Bend plane: gani pole (perpendicularised) → bind-derived fallback.
    Vec3 pole;
    Vec3 polePerp{0, 0, 0};
    if (poleQuat) {
        const Vec3 pd = rotate(Vec3(1, 0, 0), *poleQuat);
        polePerp = norm(pd - dir * dot(pd, dir));
    }
    if (polePerp.lengthSq() > 1e-6f) {
        pole = polePerp;
    } else {
        const Vec3 bindUpper = norm(Pb - Pa), bindStraight = norm(Pe - Pa);
        Vec3 poleBind = bindUpper - bindStraight * dot(bindUpper, bindStraight);
        if (poleBind.lengthSq() < 1e-8f) poleBind = perpendicularTo(bindStraight);
        const Quat swing = fromTo(bindStraight, dir);
        pole = norm(rotate(norm(poleBind), swing));
        pole = norm(pole - dir * dot(pole, dir));
        if (pole.lengthSq() < 1e-8f) pole = perpendicularTo(dir);
    }

    const float cosA =
        qBound(-1.0f, (L1 * L1 + dist * dist - L2 * L2) / (2 * L1 * dist), 1.0f);
    const float along = L1 * cosA;
    const float h = L1 * std::sqrt(qMax(0.0f, 1.0f - cosA * cosA));
    const Vec3 midPos = rootPos + dir * along + pole * h;

    const Quat rot1 = fromTo(norm(Pb - Pa), norm(midPos - rootPos));
    const Quat rot2 = fromTo(norm(Pe - Pb), norm(goal - midPos));
    animWorld[root] = mul(Mat4::fromQuat(rot1), Mat4::translation(rootPos));
    animWorld[mid] = mul(Mat4::fromQuat(rot2), Mat4::translation(midPos));
}

}  // namespace

QHash<quint32, int> boneIndexByHash(const fox::FmdlFile& m)
{
    QHash<quint32, int> out;
    for (int b = 0; b < m.bones().size(); ++b)
        if (!out.contains(m.bones()[b].nameHash32()))
            out.insert(m.bones()[b].nameHash32(), b);
    return out;
}

bool isSkeletonFragment(const fox::FmdlFile& fragment, const fox::FmdlFile& host,
                        const QHash<quint32, int>* hostIndex)
{
    if (fragment.bones().isEmpty() || host.bones().isEmpty()) return false;
    if (fragment.bones().size() >= host.bones().size()) return false;
    if (host.bones().size() < kMinHostBones) return false;
    const QHash<quint32, int> owned =
        hostIndex ? QHash<quint32, int>() : boneIndexByHash(host);
    const QHash<quint32, int>& hostIdx = hostIndex ? *hostIndex : owned;
    for (const fox::FmdlBone& b : fragment.bones()) {
        if (b.parentIndex >= 0) continue;         // not a root
        const int hb = hostIdx.value(b.nameHash32(), -1);
        if (hb < 0) continue;                     // host does not have it
        if (host.bones()[hb].parentIndex >= 0) return true;
    }
    return false;
}

QVector<Mat4> borrowPalette(const fox::FmdlFile& fragment,
                            const fox::FmdlFile& host,
                            const QVector<Mat4>& hostPalette, int* matched,
                            const QHash<quint32, int>* hostIndex,
                            int* unresolved)
{
    const int count = fragment.bones().size();
    QVector<Mat4> out(count);
    QVector<bool> have(count, false);
    int n = 0;
    const QHash<quint32, int> owned =
        hostIndex ? QHash<quint32, int>() : boneIndexByHash(host);
    const QHash<quint32, int>& hostIdx = hostIndex ? *hostIndex : owned;
    for (int b = 0; b < count; ++b) {
        const int hb = hostIdx.value(fragment.bones()[b].nameHash32(), -1);
        if (hb < 0 || hb >= hostPalette.size()) continue;
        out[b] = hostPalette[hb];
        have[b] = true;
        ++n;
    }
    // Unmatched bones ride their nearest matched ancestor. Parents precede
    // children in an FMDL skeleton, so one forward pass resolves the whole
    // chain — a hair helper bone two levels below the head ends up with the
    // head's matrix rather than with identity, which is where the rest of the
    // character is NOT.
    for (int b = 0; b < count; ++b) {
        if (have[b]) continue;
        const int p = fragment.bones()[b].parentIndex;
        if (p >= 0 && p < b && have[p]) {
            out[b] = out[p];
            have[b] = true;
        }
    }
    if (matched) *matched = n;
    // What the caller actually has to know: whether anything was left on
    // IDENTITY. A bone the host does not carry and that has no matched
    // ancestor is a second root, and a palette with one in it places part of
    // the model at the character's starting point instead of on the character.
    if (unresolved) {
        int u = 0;
        for (int b = 0; b < count; ++b)
            if (!have[b]) ++u;
        *unresolved = u;
    }
    return out;
}

namespace {

// The posed WORLD matrix of every bone, before the inverse bind that turns it
// into a skin matrix — plus the clip's view shift, which the two callers apply
// in different places (the palette folds it into the skin matrix; an EXPORT
// needs it on the node transforms themselves). Split out of buildPalette
// rather than copied: two solvers that drift are two different characters, one
// on screen and one in the file, and that shows up as an export nobody can
// reproduce from what they were looking at.
QVector<Mat4> poseWorld(const fox::FmdlFile& model, const GaniAnim& anim,
                        float frame, const FrigFile* frig,
                        const frdv::FrdvFile* frdvDrv, int* drivenBones,
                        Mat4* viewShiftOut)
{
    const int count = model.bones().size();
    QVector<Mat4> animWorld(count);

    // FMDL bone StrCode32 hashes.
    QVector<quint32> boneHash32(count);
    for (int b = 0; b < count; ++b) boneHash32[b] = model.bones()[b].nameHash32();

    // ── drives ──────────────────────────────────────────────────────────────
    struct Drive {
        const GaniTrack* track = nullptr;
        bool ws = false;
        int channel = -1;
        FrigFile::RigUnitType type = FrigFile::RigUnitType::LocalOrientation;
        const GaniChannel* chRot = nullptr;
        const GaniChannel* chPos = nullptr;
    };
    QHash<int, Drive> boneDrive;

    if (frig && frig->valid()) {
        int matched = 0;
        const auto drives =
            frig->resolveBoneDrives(boneHash32, anim.tracks.size(), &matched);
        for (auto it = drives.constBegin(); it != drives.constEnd(); ++it) {
            const FrigFile::BoneDrive& d = it.value();
            if (d.track < 0 || d.track >= anim.tracks.size()) continue;
            Drive dr;
            dr.track = &anim.tracks[d.track];
            dr.ws = FrigFile::isWorldSpace(d.type);
            dr.channel = d.channel;
            dr.type = d.type;
            const GaniChannel* cr = anim.channelBySeg(d.segRot);
            if (cr && cr->isRot) dr.chRot = cr;
            const GaniChannel* cp = anim.channelBySeg(d.segPos);
            if (cp && !cp->isRot) dr.chPos = cp;
            boneDrive.insert(it.key(), dr);
        }
    } else {
        // Direct StrCode32 name match — every track a local transform.
        QHash<quint32, int> hashToBone;
        for (int b = 0; b < count; ++b)
            if (!hashToBone.contains(boneHash32[b]))
                hashToBone.insert(boneHash32[b], b);
        for (int ti = 0; ti < anim.tracks.size(); ++ti) {
            const int bi = hashToBone.value(anim.tracks[ti].nameHash, -1);
            if (bi < 0) continue;
            Drive dr;
            dr.track = &anim.tracks[ti];
            boneDrive.insert(bi, dr);
        }
    }
    if (drivenBones) *drivenBones = boneDrive.size();

    // ── root frame / view shift ─────────────────────────────────────────────
    const Quat rootRotF =
        !anim.tracks.isEmpty() ? anim.tracks[0].sampleRot(frame) : Quat{};
    const Quat rootRotInv = conjugate(rootRotF);
    Vec3 rootPos{0, 0, 0};
    if (!anim.tracks.isEmpty()) {
        float p[3];
        if (anim.tracks[0].samplePos(frame, p)) rootPos = Vec3(p[0], p[1], p[2]);
    }
    const Quat rootRot0 =
        !anim.tracks.isEmpty() ? anim.tracks[0].sampleRot(0) : Quat{};
    const Mat4 viewShift = Mat4::fromQuat(quatMul(rootRotF, conjugate(rootRot0)));

    // ── IK setup ────────────────────────────────────────────────────────────
    struct IkAt {
        int mid = -1, end = -1, track = -1;
        bool arm = false;
    };
    QHash<int, IkAt> ikSolveAt;
    QSet<int> ikSet;
    QHash<int, QPair<const GaniTrack*, int>> ikShoulder;
    if (frig && frig->valid()) {
        const auto jobs = frig->resolveIkJobs(boneHash32, anim.tracks.size());
        for (const FrigFile::IkJob& j : jobs) {
            if (j.track >= anim.tracks.size()) continue;
            const GaniTrack& tr = anim.tracks[j.track];
            if (!tr.hasPos()) continue;   // no effector channel → leave FK
            const bool arm = j.type == FrigFile::RigUnitType::Arm
                || j.type == FrigFile::RigUnitType::ThreeBoneLikeTwoBone;
            if (arm && j.chainB >= 0 && j.chainC >= 0 && j.effector >= 0) {
                ikSolveAt.insert(j.chainB, IkAt{j.chainC, j.effector, j.track, true});
                ikSet.insert(j.chainC);
                // Shoulder is driven by the track's FIRST ROTATION channel;
                // when the clip leaves it static, channel 0 can be the
                // effector position — don't sample that as a rotation.
                if (j.chainA >= 0)
                    for (int c = 0; c < tr.channels.size(); ++c)
                        if (tr.channels[c].isRot) {
                            ikShoulder.insert(j.chainA, {&tr, c});
                            break;
                        }
            } else if (!arm && j.chainA >= 0 && j.chainB >= 0 && j.effector >= 0) {
                ikSolveAt.insert(j.chainA, IkAt{j.chainB, j.effector, j.track, false});
                ikSet.insert(j.chainB);
            }
        }
    }

    // ── FK + integrated IK ──────────────────────────────────────────────────
    for (int b = 0; b < count; ++b) {   // parents precede children
        const auto& bone = model.bones()[b];
        const Vec3 localOffset(bone.localPos[0], bone.localPos[1], bone.localPos[2]);
        const int parent = bone.parentIndex;
        const Mat4 parentW =
            parent >= 0 && parent < count ? animWorld[parent] : Mat4();

        if (ikSet.contains(b)) continue;   // placed by the chain-root solve

        Quat rot{};
        Vec3 trans = localOffset;
        bool ws = false, absPos = false;

        const auto sh = ikShoulder.constFind(b);
        if (sh != ikShoulder.constEnd()) {
            rot = sh.value().first->channels[sh.value().second].sampleRot(frame);
            ws = true;
        } else {
            const auto di = boneDrive.constFind(b);
            if (di != boneDrive.constEnd()) {
                const Drive& d = di.value();
                if (d.chRot) {
                    rot = d.chRot->sampleRot(frame);
                } else if (d.channel >= 0 && d.channel < d.track->channels.size()
                           && d.track->channels[d.channel].isRot) {
                    rot = d.track->channels[d.channel].sampleRot(frame);
                } else {
                    rot = d.track->sampleRot(frame);
                }
                float ap[3];
                if (carriesTranslation(d.type)) {
                    if (d.chPos) {
                        d.chPos->sampleVec(frame, ap);
                        trans = Vec3(ap[0], ap[1], ap[2]);
                        absPos = d.type == FrigFile::RigUnitType::Root;
                    } else if (d.track->samplePos(frame, ap)) {
                        trans = Vec3(ap[0], ap[1], ap[2]);
                        absPos = d.type == FrigFile::RigUnitType::Root;
                    }
                }
                ws = d.ws;
            }
        }

        if (absPos) {
            animWorld[b] = mul(Mat4::fromQuat(rot), Mat4::translation(trans));
        } else if (ws) {
            const Vec3 worldPos = transform(localOffset, parentW);
            animWorld[b] = mul(Mat4::fromQuat(rot), Mat4::translation(worldPos));
        } else {
            const Mat4 local = mul(Mat4::fromQuat(rot), Mat4::translation(trans));
            animWorld[b] = mul(local, parentW);
        }

        const auto job = ikSolveAt.constFind(b);
        if (job != ikSolveAt.constEnd()) {
            float off[3];
            if (anim.tracks[job.value().track].samplePos(frame, off)) {
                const Vec3 goal =
                    rotate(Vec3(off[0], off[1], off[2]) - rootPos, rootRotInv);
                // pole = LAST rotation channel of the IK track.
                const GaniTrack& tr = anim.tracks[job.value().track];
                const GaniChannel* poleCh = nullptr;
                for (int c = tr.channels.size() - 1; c >= 0; --c)
                    if (tr.channels[c].isRot) {
                        poleCh = &tr.channels[c];
                        break;
                    }
                Quat pole;
                if (poleCh) pole = poleCh->sampleRot(frame);
                solveTwoBone(model, animWorld, b, job.value().mid, job.value().end,
                             goal, poleCh ? &pole : nullptr);
            }
        }
    }

    // ── help bones (twist/roll *_HLP) — after FK+IK, before skinning ────────
    if (frdvDrv && frdvDrv->valid()) frdvDrv->apply(model, animWorld);
    if (viewShiftOut) *viewShiftOut = viewShift;
    return animWorld;
}

}  // namespace

QVector<Mat4> buildPalette(const fox::FmdlFile& model, const GaniAnim& anim,
                           float frame, const FrigFile* frig,
                           const frdv::FrdvFile* frdvDrv, int* drivenBones)
{
    const int count = model.bones().size();
    QVector<Mat4> skin(count);
    if (count == 0 || !anim.valid()) return skin;
    Mat4 viewShift;
    const QVector<Mat4> animWorld =
        poseWorld(model, anim, frame, frig, frdvDrv, drivenBones, &viewShift);

    // ── skin matrices ───────────────────────────────────────────────────────
    for (int b = 0; b < count; ++b) {
        const auto& bone = model.bones()[b];
        const Mat4 invBind = Mat4::translation(
            Vec3(-bone.worldPos[0], -bone.worldPos[1], -bone.worldPos[2]));
        skin[b] = mul(mul(invBind, animWorld[b]), viewShift);
    }
    return skin;
}

QVector<Mat4> buildWorld(const fox::FmdlFile& model, const GaniAnim& anim,
                         float frame, const FrigFile* frig,
                         const frdv::FrdvFile* frdvDrv, int* drivenBones)
{
    const int count = model.bones().size();
    QVector<Mat4> out(count);
    if (count == 0 || !anim.valid()) return out;
    Mat4 viewShift;
    const QVector<Mat4> animWorld =
        poseWorld(model, anim, frame, frig, frdvDrv, drivenBones, &viewShift);
    // The view shift is part of the POSE, not of the skinning: it is what
    // keeps a walk cycle centred while still applying its net turn. Folding it
    // in here is what makes an exported clip move exactly as the viewport did.
    for (int b = 0; b < count; ++b) out[b] = mul(animWorld[b], viewShift);
    return out;
}

}  // namespace animpose
