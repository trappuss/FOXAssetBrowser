// MaterialPresets.h — the game's one material parameter table, loaded once.
//
// The .fmtt itself is parsed by fox::FmttFile; this is the part that knows
// where to find it and keeps it. It lives in src/index/ and not next to the
// parser because finding it means searching the archive index, and the format
// layer does not depend on the index layer.
#pragma once
#include <QString>

#include "fox/FmttFile.h"

namespace fox {

// The game's ONE preset table, found in the archives and loaded once.
//
// A singleton rather than a per-model load because that is what it is: every
// model in every one of the four games indexes the same 256 rows. Absent on a
// partial install (it lives in data1), and then every lookup returns the
// dielectric default — which is exactly the constant this renderer used before
// the table existed, so a missing table cannot render worse than before.
class MaterialPresetTable {
public:
    static MaterialPresetTable& instance();

    // Scan the archive index for the table and parse it. Safe to call again
    // after a rescan.
    void build();
    bool ok() const { return m_ok; }
    QString note() const { return m_note; }
    MaterialPreset at(int index) const { return m_file.at(index); }

private:
    FmttFile m_file;
    bool m_ok = false;
    QString m_note;
};

}  // namespace fox
