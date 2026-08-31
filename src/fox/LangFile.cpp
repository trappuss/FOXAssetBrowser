// LangFile.cpp — see LangFile.h.
#include "fox/LangFile.h"

#include <QtEndian>
#include <cstring>

#include "fox/FoxHash.h"

namespace fox {
namespace {
constexpr int kHeaderSize = 24;
}  // namespace

bool LangFile::isLang(const QByteArray& data)
{
    return data.size() >= kHeaderSize
        && std::memcmp(data.constData(), "LANG", 4) == 0;
}

bool LangFile::parse(const QByteArray& data)
{
    m_strings.clear();
    m_error.clear();
    m_version = 0;

    if (!isLang(data)) {
        m_error = QStringLiteral("not a LANG file");
        return false;
    }
    const char* p = data.constData();
    const auto u32be = [p](int o) { return qFromBigEndian<quint32>(p + o); };

    m_version = u32be(0x04);
    const quint32 count = u32be(0x0C);
    const qint64 textOff = u32be(0x10);
    const qint64 keyOff = u32be(0x14);

    // Bounds first: this is archive data and a truncated file must fail, not
    // walk off the end.
    if (count > 1000000u) {
        m_error = QStringLiteral("implausible entry count (%1)").arg(count);
        return false;
    }
    if (keyOff < 0 || keyOff + 8LL * count > data.size()) {
        m_error = QStringLiteral("key table out of range");
        return false;
    }
    if (textOff < 0 || textOff > data.size()) {
        m_error = QStringLiteral("text block out of range");
        return false;
    }

    m_strings.reserve(int(count));
    for (quint32 i = 0; i < count; ++i) {
        const quint32 hash = u32be(int(keyOff + 8 * i));
        const quint32 off = u32be(int(keyOff + 8 * i + 4));
        qint64 at = textOff + off;
        if (at < 0 || at >= data.size()) continue;
        // Every string carries a 2-byte header before its bytes. The first
        // byte is a small type code that VARIES by table (0x01 in
        // tpp_weapon, 0x05 in tpp_announce_log), so it must be skipped
        // unconditionally — testing for one particular value silently
        // truncated every string in the tables that use another.
        if (at + 2 <= data.size()) at += 2;
        const int end = data.indexOf('\0', int(at));
        const int stop = end < 0 ? data.size() : end;
        m_strings.insert(hash,
                         QString::fromUtf8(p + at, int(stop - at)));
    }
    return true;
}

QString LangFile::textFor(const QString& label) const
{
    return m_strings.value(
        quint32(hashFileNameLegacy(label, /*removeExtension=*/false) & 0xFFFFFFFFu));
}

QString LangFile::describe() const
{
    return QStringLiteral("LANG v%1 — %2 string%3")
        .arg(m_version)
        .arg(m_strings.size())
        .arg(m_strings.size() == 1 ? QString() : QStringLiteral("s"));
}

}  // namespace fox
