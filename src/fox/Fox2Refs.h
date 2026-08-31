// Fox2Refs.h — light reader over Fox2 entity binaries (.fox2 / .parts,
// magic 0xf2"box" + version): extracts the asset paths the file references,
// WITHOUT porting the full Fox2 property type system. Enough for:
//   • the authoritative model → rig binding (.parts names the model's .frig,
//     .frdv, .sim — correct for dogs/horses/vehicles, not just humans);
//   • a references summary page in the contextual preview.
#pragma once
#include <QByteArray>
#include <QStringList>

namespace fox {

bool looksLikeFox2(const QByteArray& data);

// Every "/Assets/…​.<known ext>" string in the file, deduplicated, in file
// order. Strings in Fox2 blobs are not reliably NUL-terminated, so matches
// are anchored on a known extension set.
QStringList fox2AssetPaths(const QByteArray& data);

}  // namespace fox
