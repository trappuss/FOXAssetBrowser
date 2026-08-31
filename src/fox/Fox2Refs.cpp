// Fox2Refs.cpp — see Fox2Refs.h.
#include "fox/Fox2Refs.h"

#include <QRegularExpression>
#include <QSet>
#include <QtEndian>

namespace fox {

bool looksLikeFox2(const QByteArray& data)
{
    // FoxFile: MagicNumber1 = 0x786f62f2 ("\xf2box"), MagicNumber2 = version.
    return data.size() >= 8
        && qFromLittleEndian<quint32>(data.constData()) == 0x786f62f2u;
}

QStringList fox2AssetPaths(const QByteArray& data)
{
    // Anchor on the known asset extensions — Fox2 string payloads run into
    // adjacent binary, so a bare "printable run" over-captures.
    static const QRegularExpression re(QStringLiteral(
        "/(?:Assets|as)/[\\x20-\\x7e]+?\\."
        "(?:fmdl|frig|frdv|frld|sim|fcnp|fclo|ftex|pftxs|fpk|fpkd|mtar|gani|"
        "mog|fox2|parts|phsd|ph|tgt|vfx|vfxlf|fsd|ladb|lng|json|lua|sdf|sbp|"
        "wem|bnk|evf|fsm|fstb|rdf|sand|sani|uig|uif|uia|uigb|vfsm|xml)"));

    QStringList out;
    QSet<QString> seen;
    // Cap the scan: real .parts/.fox2 are tens of KB; a pathological file
    // must not stall the GUI (this runs on preview + rig binding).
    const QByteArray bounded =
        data.size() > 4 * 1024 * 1024 ? data.left(4 * 1024 * 1024) : data;
    const QString hay = QString::fromLatin1(bounded);   // byte-preserving scan
    auto it = re.globalMatch(hay);
    while (it.hasNext()) {
        const QString p = it.next().captured(0);
        if (p.size() > 260) continue;
        if (!seen.contains(p)) {
            seen.insert(p);
            out.append(p);
        }
    }
    return out;
}

}  // namespace fox
