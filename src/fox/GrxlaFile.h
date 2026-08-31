// GrxlaFile.h — Fox's light arrays, read structurally.
//
// A .grxla is the game's own LIGHTING: an array of point lights, spot lights
// and light probes placed in a scene, and .grxoc is the same container holding
// occluders. The wiki documents both down to the field
// (https://mgsvmoddingwiki.github.io/GRXLA/) — magic "FGxL"/"FGxO", a 16-byte
// header, then a chain of entries each beginning with a four-character type and
// its size, terminated by four zero bytes.
//
// THIS READER STOPS AT THE CHAIN. It reads the magic, the header and the
// type/size walk, and it does NOT decode a single light's colour, position or
// brightness. That is deliberate and it is the project's own rule: a layout is
// settled when a throwaway reader has run over EVERY instance in the archives
// and asserted something meaningful, and this build has never had one of these
// files to run over — they live inside FPKs and no extract on hand carries any.
// A type/size walk that reaches the terminator on every file in an install is
// exactly that assertion, and it is the cheapest one that can be wrong out
// loud. Decode the fields in the session that can point this at a real install
// and see "N of N" come back; until then, --lightdump counts and nothing here
// feeds the renderer.
#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>

namespace fox {

struct GrxEntry {
    QString type;      // "CM00", "PL01", "SL02", "EP00", "OC00", …
    quint32 size = 0;  // as declared, before any bounds clamp
    qint64 offset = 0; // where it starts in the file
};

class GrxlaFile {
public:
    // "FGxL" (grxla) or "FGxO" (grxoc), and long enough for the header.
    static bool isGrx(const QByteArray& data);

    bool parse(const QByteArray& data);

    const QVector<GrxEntry>& entries() const { return m_entries; }
    // The walk reached the four-zero terminator with every entry inside the
    // file. False means the documented layout did not hold for this file, and
    // that is the finding — not something to work around.
    bool complete() const { return m_complete; }
    // The 0x8/0xC header constants held. A file where they do NOT hold is one
    // whose chain may not start at 0x10 at all, so `complete()` on its own
    // would be a false positive — the one way this reader could tell you the
    // layout is settled when it is not.
    bool headerOk() const { return m_headerOk; }
    bool occluder() const { return m_occluder; }
    QString error() const { return m_error; }
    // "PL01×12 SL01×3 EP00×40" — one line for a census row.
    QString describe() const;

private:
    QVector<GrxEntry> m_entries;
    bool m_complete = false;
    bool m_headerOk = false;
    bool m_occluder = false;
    QString m_error;
};

}  // namespace fox
