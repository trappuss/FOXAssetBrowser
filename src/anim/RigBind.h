// RigBind.h — THE model → rig resolver, shared by the Models tab and the
// Customize composer (one implementation so both bind identically):
//   1. engine-authoritative: the .parts ModelDescription whose modelFile is
//      this model names its gameRigFile;
//   2. fallback: the shared humanoid rig (human_finger.frig — players,
//      soldiers and GZ clips all use it).
#pragma once
#include <QString>

#include "fox/FrigFile.h"

namespace rigbind {

// Load the rig for `modelPath` into `out`. Returns true on success;
// `boundVia` (optional) receives the .parts path or "heuristic".
bool loadFrigFor(const QString& modelPath, fox::FrigFile* out,
                 QString* boundVia = nullptr);

}  // namespace rigbind
