// LangFile.h — Fox Engine localisation table (".lng2", magic "LANG").
//
// This is where every piece of on-screen game text lives: weapon and item
// names, descriptions, menu labels, mission text. One file per language per
// category (tpp_weapon.eng.lng2, tpp_item.eng.lng2, tpp_menu.eng.lng2 …).
//
// Format (reverse-engineered here; note it is BIG-endian, unlike every other
// Fox format in this project):
//
//   0x00  char[4]  "LANG"
//   0x04  u32 BE   version (3 in TPP)
//   0x08  char[2]  byte order tag ("BE")
//   0x0C  u32 BE   entry count
//   0x10  u32 BE   offset of the text block
//   0x14  u32 BE   offset of the key table
//   …text block: each string is preceded by a 2-byte header (its first byte is
//     a small type code that differs per table — 0x01, 0x05 — so skip the two
//     bytes rather than testing for a value) and is NUL-terminated
//   …key table: `count` × { u32 BE StrCode32 of the label, u32 BE offset into
//     the text block }, sorted ascending by hash for binary search
//
// The key is the StrCode32 of a lowercase snake_case label ("announce_boy_died").
// Verified by taking the 155 announce_* labels that appear literally in
// TppUI.lua and hashing them against tpp_announce_log.eng.lng2: 141 resolve,
// and the 14 that do not are strings that file simply does not carry.
#pragma once
#include <QByteArray>
#include <QHash>
#include <QString>
#include <cstdint>

namespace fox {

class LangFile {
public:
    static bool isLang(const QByteArray& data);

    bool parse(const QByteArray& data);
    QString errorString() const { return m_error; }

    quint32 version() const { return m_version; }
    int count() const { return m_strings.size(); }
    const QHash<quint32, QString>& strings() const { return m_strings; }
    // The text for a label, hashing it to StrCode32 first. Empty when absent.
    QString textFor(const QString& label) const;
    QString textForHash(quint32 hash) const { return m_strings.value(hash); }

    QString describe() const;

private:
    quint32 m_version = 0;
    QHash<quint32, QString> m_strings;
    QString m_error;
};

}  // namespace fox
