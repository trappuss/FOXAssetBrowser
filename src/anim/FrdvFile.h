// FrdvFile.h — Fox Engine help-bone driver (.frdv / "FoxRigDriver").
// Port of Fox_Parser's FrdvFile (itself an exact float32 port of the
// fox::animx::*Operator suite from Tpp_main_win64).
//
// A driven-bone layer that runs AFTER the gani pose + IK, BEFORE skinning.
// Character skeletons carry *_HLP "twist/roll/muscle" bones the mesh is
// partially skinned to; undriven they freeze at bind while the limb rotates
// and the mesh pinches into the classic candy-wrapper knot. Each operator
// distributes a fraction of a SOURCE bone's local motion onto a TARGET helper
// bone. The .frdv lives beside its .fmdl with the same stem.
//
// Pose-affecting operator types are implemented (1-15, 17, 18, 22); the
// scale/material side-output types (16, 19, 20, 21, 23) are skipped — they
// feed bone scale and shader parameters, not the pose.
#pragma once
#include <QByteArray>
#include <QVector>

#include "anim/AnimMath.h"
#include "fox/FmdlFile.h"

namespace frdv {

class FrdvFile {
public:
    struct Op {
        QByteArray d;   // the raw 0x80-byte entry
        qint16 s16(int o) const;
        qint32 i32(int o) const;
        float f32(int o) const;
        animmath::Vec3 v3(int o) const;
    };

    // Header: "FRDV" | u32 magic | u32 entryCount | align16 | u32 offsets[].
    bool parse(const QByteArray& data);
    bool valid() const { return !m_ops.isEmpty(); }
    const QVector<Op>& ops() const { return m_ops; }

    // Run the operators over the model-space pose (mutates animWorld for the
    // helper target bones). Op skeleton indices address model.bones() DIRECTLY
    // (canonical SKL order). Returns the number of operators applied.
    int apply(const fox::FmdlFile& model,
              QVector<animmath::Mat4>& animWorld) const;

private:
    QVector<Op> m_ops;
};

}  // namespace frdv
