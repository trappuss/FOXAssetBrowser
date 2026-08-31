// PftxsFile.h — reader for Fox Engine PFTXS texture packs ("PFTX" + "TEXL").
// A pack is a list of FTEX groups; each group holds the .ftex plus its
// numbered .ftexs stream files, all hash-named. Port of GzsTool.Core/Pftxs.
#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>
#include <cstdint>

namespace fox {

struct PftxsSubEntry {
    quint64 hash = 0;      // full PathFileNameCode of the .ftex / .N.ftexs
    quint32 offset = 0;    // relative to the group's FTEX header
    quint32 size = 0;
    qint64 absOffset = 0;  // resolved absolute offset within the pack blob
};

struct PftxsGroup {
    quint64 hash = 0;
    QVector<PftxsSubEntry> entries;
};

class PftxsFile {
public:
    static bool isPftxs(const QByteArray& data);

    bool parse(const QByteArray& data);
    const QVector<PftxsGroup>& groups() const { return m_groups; }
    QString errorString() const { return m_error; }

    // How many textures the pack's TEXL header says it holds, against how many
    // the walk could actually read. They match on a complete pack. They do not
    // on a partially-downloaded install, where the archive region behind the
    // pack is a hole: the walk stops at the first gap and returns SUCCESS with
    // a short list, which is honest about what it read and silent about what
    // is missing. Comparing the two is what lets a caller say so — see the
    // deep scan in ArchiveIndex.
    int declaredTextures() const { return int(m_declared); }
    int readTextures() const;

    static QByteArray readEntry(const QByteArray& data, const PftxsSubEntry& entry);

private:
    QVector<PftxsGroup> m_groups;
    QString m_error;
    quint32 m_declared = 0;
};

}  // namespace fox
