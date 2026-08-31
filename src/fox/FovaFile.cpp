// FovaFile.cpp — see FovaFile.h.
#include "fox/FovaFile.h"

#include <QStringList>
#include <utility>
#include <QtEndian>
#include <cstring>

namespace fox {
namespace {

constexpr int kHeaderMin = 0x28;   // through the per-section counts
constexpr quint16 kNone = 0xFFFF;

}  // namespace

bool FovaFile::isFova(const QByteArray& data)
{
    return data.size() >= kHeaderMin && std::memcmp(data.constData(), "FOV2", 4) == 0;
}

bool FovaFile::parse(const QByteArray& data)
{
    m_subs.clear();
    m_hide.clear();
    m_show.clear();
    m_attach.clear();
    m_files.clear();
    m_variableEntries = 0;
    m_error.clear();
    m_version = 0;
    m_unreadable = false;

    if (!isFova(data)) {
        m_error = QStringLiteral("not a FOV2 file");
        return false;
    }
    const char* p = data.constData();
    const qint64 size = data.size();
    const auto u8 = [p](int o) { return static_cast<quint8>(p[o]); };
    const auto u16 = [p](int o) { return qFromLittleEndian<quint16>(p + o); };
    const auto u64 = [p](int o) { return qFromLittleEndian<quint64>(p + o); };
    const auto u32 = [p](int o) { return qFromLittleEndian<quint32>(p + o); };

    const int fileOff = u16(0x0A);
    const int variableEntries = u16(0x0C);
    const int fileCount = u16(0x0E);
    const int hideCount = u8(0x20);
    const int showCount = u8(0x21);
    const int matCount = u8(0x22);
    const int boneCount = u8(0x24);
    const int cnpCount = u8(0x25);

    // Every bound is checked against the real file size: these come straight
    // out of a game archive, and a truncated or crafted one must fail cleanly
    // rather than walk off the end.
    if (fileOff < 0 || static_cast<qint64>(fileOff) + 8LL * fileCount > size) {
        m_error = QStringLiteral("external-file table out of range");
        return false;
    }
    QVector<quint64> files;
    files.reserve(fileCount);
    for (int i = 0; i < fileCount; ++i) files.append(u64(fileOff + 8 * i));

    // The static block is a fixed sequence, so each section's offset is just
    // the running total of the ones before it — no heuristics, no guessing.
    qint64 o = 0x28;
    const qint64 need = 4LL * hideCount + 4LL * showCount + 12LL * matCount
        + 12LL * boneCount + 20LL * cnpCount;
    if (o + need > size) {
        m_error = QStringLiteral("static block out of range");
        return false;
    }
    // Cross-check against the one field that says where the static block ends:
    // 0x08 is the offset of the variable-data section, which follows it. On all
    // 1,895 shipped tables the sections computed above end 4 or 8 bytes short of
    // it (padding), never past it — so overrunning means the counts have been
    // read from the wrong place and everything after would be garbage.
    if (u16(0x08) > 0 && o + need > qint64(u16(0x08))) {
        m_error = QStringLiteral(
            "static block (ends %1) overruns the variable-data section (%2)")
                      .arg(o + need).arg(u16(0x08));
        return false;
    }

    QVector<quint32> hide, show;
    hide.reserve(hideCount);
    for (int i = 0; i < hideCount; ++i) hide.append(u32(int(o) + 4 * i));
    o += 4LL * hideCount;
    show.reserve(showCount);
    for (int i = 0; i < showCount; ++i) show.append(u32(int(o) + 4 * i));
    o += 4LL * showCount;

    // Parallel arrays, not records — see the note in FovaFile.h.
    const qint64 matOff = o;
    const qint64 roleOff = matOff + 4LL * matCount;
    const qint64 idxOff = roleOff + 4LL * matCount;
    QVector<FovaSubstitution> subs;
    int dropped = 0;
    subs.reserve(matCount);
    for (int i = 0; i < matCount; ++i) {
        FovaSubstitution sub;
        sub.materialHash32 = u32(int(matOff) + 4 * i);
        sub.roleHash32 = u32(int(roleOff) + 4 * i);
        const quint16 ti = u16(int(idxOff) + 2 * i);
        // 0xFFFF means "this role keeps the model's own texture". It is a real
        // value, not a corrupt one, and dropping the row would silently change
        // which substitutions line up with which material. An index that is
        // merely OUT OF RANGE is a different thing — it means the table is not
        // being read correctly — so it is counted rather than quietly folded
        // into "no substitution", which would look identical to the caller.
        if (ti == kNone) {
            sub.textureIndex = -1;
        } else if (ti >= fileCount) {
            sub.textureIndex = -1;
            ++dropped;
        } else {
            sub.textureIndex = int(ti);
        }
        subs.append(sub);
    }
    o = idxOff + 4LL * matCount;   // the index array is followed by a second,
                                   // all-0xFFFF u16 array of the same length

    QVector<FovaAttachment> attach;
    for (int i = 0; i < boneCount; ++i) {
        const qint64 r = o + 12LL * i;
        FovaAttachment a;
        a.byConnectPoint = false;
        const quint16 m = u16(int(r));
        const quint16 fr = u16(int(r) + 2);
        const quint16 sim = u16(int(r) + 8);
        if (m != kNone && m >= fileCount) ++dropped;
        a.modelIndex = (m == kNone || m >= fileCount) ? -1 : int(m);
        a.frdvIndex = (fr == kNone || fr >= fileCount) ? -1 : int(fr);
        a.simIndex = (sim == kNone || sim >= fileCount) ? -1 : int(sim);
        attach.append(a);
    }
    o += 12LL * boneCount;
    for (int i = 0; i < cnpCount; ++i) {
        const qint64 r = o + 20LL * i;
        FovaAttachment a;
        a.byConnectPoint = true;
        a.connectPointHash32 = u32(int(r));
        const quint16 m = u16(int(r) + 8);
        const quint16 fr = u16(int(r) + 10);
        const quint16 sim = u16(int(r) + 16);
        if (m != kNone && m >= fileCount) ++dropped;
        a.modelIndex = (m == kNone || m >= fileCount) ? -1 : int(m);
        a.frdvIndex = (fr == kNone || fr >= fileCount) ? -1 : int(fr);
        a.simIndex = (sim == kNone || sim >= fileCount) ? -1 : int(sim);
        attach.append(a);
    }

    // Nothing is published until the whole table has been read: a caller that
    // ignores the false return (or reuses one FovaFile across files) must not
    // see half a parse dressed up as a whole one.
    m_version = static_cast<quint8>(p[7]);
    m_variableEntries = variableEntries;
    m_files = std::move(files);
    m_hide = std::move(hide);
    m_show = std::move(show);
    m_subs = std::move(subs);
    m_attach = std::move(attach);
    m_droppedRows = dropped;

    // A table that declares files and rows but points at none of them, or one
    // whose indices fall outside its own file table, has not really been
    // understood — say so rather than substituting nothing and looking normal.
    m_unreadable = dropped > 0
        || (!m_files.isEmpty() && !m_subs.isEmpty() && m_hide.isEmpty()
            && m_show.isEmpty() && m_attach.isEmpty() && m_variableEntries == 0);
    if (dropped == 0)
        for (const FovaSubstitution& sub : m_subs)
            if (sub.textureIndex >= 0) { m_unreadable = false; break; }
    return true;
}

QString FovaFile::describe() const
{
    int active = 0;
    for (const FovaSubstitution& s : m_subs)
        if (s.textureIndex >= 0) ++active;
    QStringList bits;
    bits << QStringLiteral("%1 substitution%2 (%3 active) over %4 file%5")
                .arg(m_subs.size())
                .arg(m_subs.size() == 1 ? QString() : QStringLiteral("s"))
                .arg(active)
                .arg(m_files.size())
                .arg(m_files.size() == 1 ? QString() : QStringLiteral("s"));
    if (!m_hide.isEmpty())
        bits << QStringLiteral("%1 mesh group%2 hidden").arg(m_hide.size())
                    .arg(m_hide.size() == 1 ? QString() : QStringLiteral("s"));
    if (!m_show.isEmpty())
        bits << QStringLiteral("%1 shown").arg(m_show.size());
    if (!m_attach.isEmpty())
        bits << QStringLiteral("%1 model%2 attached").arg(m_attach.size())
                    .arg(m_attach.size() == 1 ? QString() : QStringLiteral("s"));
    if (m_droppedRows > 0)
        bits << QStringLiteral("%1 row%2 out of range").arg(m_droppedRows)
                    .arg(m_droppedRows == 1 ? QString() : QStringLiteral("s"));
    if (m_variableEntries > 0)
        bits << QStringLiteral("%1 random variation%2").arg(m_variableEntries)
                    .arg(m_variableEntries == 1 ? QString() : QStringLiteral("s"));
    return QStringLiteral("FOVA v%1 — %2").arg(m_version).arg(bits.join(QStringLiteral(", ")));
}

}  // namespace fox
