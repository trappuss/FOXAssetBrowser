// SbpFile.h — Fox/Wwise sound bundle (.sbp, magic "SBPL"): a tiny container
// of sub-blobs (bnk = Wwise soundbank metadata, stp/sab = stream packages).
// The audio itself lives in the STPL stream package as embedded .wem files,
// indexed by a {u32 wemId, u32 offset} table (validated against Ground Zeroes
// data_02.g0s). listWems() flattens every embedded wem in the bundle so the
// archive index can expose them as browsable children.
#pragma once
#include <QByteArray>
#include <QVector>

namespace fox {

struct SbpSub {
    QByteArray magic;    // "bnk", "stp", "sab" (trailing NUL stripped)
    quint32 offset = 0;  // absolute within the .sbp
    quint32 size = 0;
};

struct SbpWem {
    quint32 id = 0;         // Wwise wem id (the in-game file name)
    quint32 absOffset = 0;  // absolute within the .sbp
    quint32 size = 0;
};

class SbpFile {
public:
    static bool isSbp(const QByteArray& data)
    {
        return data.size() >= 8 && data.startsWith("SBPL");
    }

    bool parse(const QByteArray& data);
    const QVector<SbpSub>& entries() const { return m_entries; }

    // Every embedded wem across all stream packages, in stable table order.
    static QVector<SbpWem> listWems(const QByteArray& sbp);
    static QByteArray readWem(const QByteArray& sbp, const SbpWem& w)
    {
        if (static_cast<qint64>(w.absOffset) + w.size > sbp.size()) return {};
        return sbp.mid(w.absOffset, w.size);
    }

private:
    QVector<SbpSub> m_entries;
};

}  // namespace fox
