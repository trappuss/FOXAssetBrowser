#include "util/ModPackage.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QTextStream>

#include "fox/FoxHash.h"
#include "index/ArchiveIndex.h"
#include "util/ModFolder.h"
#include "util/ZipWriter.h"

namespace modpackage {
namespace {

// The mod mount itself, which is the copy we are packaging and therefore the
// one copy that must NOT answer "where does the game keep this file". Same
// test the index uses to decide the mount wins: a loose directory above every
// archive.
bool isModMount(const fox::IndexedArchive& a)
{
    return a.kind == fox::ArchiveKind::Loose && a.priority >= 1100;
}

QString md5Of(const QByteArray& data)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex());
}

// ── One gather, two writers ────────────────────────────────────────────────
// Both package shapes need the same three things about every replacement: its
// bytes, its date, and where the GAME keeps its own copy. Deriving that twice
// would be two spellings of the question that decides whether a package is
// installable at all, and they would drift exactly when it mattered.
struct Item {
    QString asset;
    QByteArray data;
    QDateTime modified;
    QString gameCopy;        // archive short name, or empty when not indexed
    bool inIndex = false;
    bool inContainer = false;
    qint64 gameBytes = -1;
};

struct Gathered {
    QVector<Item> items;
    QDateTime newest;
    qint64 bytes = 0;
    int inContainer = 0;
    int notInIndex = 0;
    QString error;
};

Gathered gather()
{
    Gathered g;
    if (modfolder::dir().isEmpty()) {
        g.error = QStringLiteral("No mod folder is set — Settings ▸ Folders.");
        return g;
    }
    const QStringList assets = modfolder::list();
    if (assets.isEmpty()) {
        g.error = QStringLiteral(
            "The mod folder holds no replacements, so there is nothing to "
            "package. An empty archive is a file that looks like a delivery "
            "and is not one.");
        return g;
    }

    // Where does the GAME keep each of these? One linear pass, not one lookup
    // per asset: fileIndexForPath deliberately returns the copy that WINS,
    // which for every asset here is our own mod mount — the exact copy whose
    // origin we are trying to look past. So the pass skips the mod mount and
    // keeps the highest-priority remaining copy, which is the one the game
    // would load if this package were never installed.
    struct Origin {
        int fileIdx = -1;
        int priority = -1;
    };
    QHash<QString, Origin> origin;
    origin.reserve(assets.size());
    for (const QString& a : assets) origin.insert(a, Origin{});

    const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
    const QVector<fox::IndexedFile>& files = ix.files();
    for (int i = 0; i < files.size(); ++i) {
        const fox::IndexedFile& f = files[i];
        if (f.path.isEmpty()) continue;
        auto it = origin.find(f.path);
        if (it == origin.end()) continue;
        const fox::IndexedArchive& a = ix.archives().value(f.archiveId);
        if (isModMount(a)) continue;
        if (a.priority > it->priority) {
            it->fileIdx = i;
            it->priority = a.priority;
        }
    }

    g.items.reserve(assets.size());
    for (const QString& asset : assets) {
        const QFileInfo fi(modfolder::pathFor(asset));
        QFile in(fi.absoluteFilePath());
        if (!in.open(QIODevice::ReadOnly)) {
            g.error = QStringLiteral("Could not read %1: %2")
                          .arg(in.fileName(), in.errorString());
            return g;
        }
        Item item;
        item.asset = asset;
        item.data = in.readAll();
        in.close();
        // The replacement's own date, not the clock — so packaging an
        // unchanged mod folder twice produces the same archive twice.
        item.modified = fi.lastModified();
        if (item.modified.isValid()
            && (!g.newest.isValid() || item.modified > g.newest))
            g.newest = item.modified;

        const Origin o = origin.value(asset);
        if (o.fileIdx >= 0) {
            const fox::IndexedFile& gf = files[o.fileIdx];
            item.inIndex = true;
            item.gameCopy = ix.archives().value(gf.archiveId).shortName;
            // THE FACT BOTH WRITERS TURN ON. childIdx >= 0 means the game's
            // own copy is not an archive entry at all but a file inside an
            // .fpk/.fpkd/.pftxs container — the difference between a QarEntry
            // and an FpkEntry to every loader in this ecosystem, and something
            // that cannot be recovered from the file name later.
            item.inContainer = gf.childIdx >= 0;
            item.gameBytes = gf.size;
            if (item.inContainer) ++g.inContainer;
        } else {
            ++g.notInIndex;
        }
        g.bytes += item.data.size();
        g.items.append(item);
    }
    return g;
}

QString assetToZipName(const QString& asset)
{
    QString name = asset;
    while (name.startsWith(QLatin1Char('/'))) name = name.mid(1);
    return name;
}

// ── The XML, spelt the way .NET spells it ──────────────────────────────────
// Escaping is not a matter of taste here: the reference output was produced by
// compiling SnakeBite's own classes and running System.Xml.Serialization
// against them, and these two functions reproduce what came out. An attribute
// escapes " & < > and turns newlines into character references; element text
// escapes & < > and leaves quotes alone. An apostrophe is escaped by neither.
QString xmlAttr(const QString& text)
{
    QString out;
    out.reserve(text.size() + 8);
    for (const QChar c : text) {
        switch (c.unicode()) {
        case u'&':  out += QLatin1String("&amp;"); break;
        case u'<':  out += QLatin1String("&lt;"); break;
        case u'>':  out += QLatin1String("&gt;"); break;
        case u'"':  out += QLatin1String("&quot;"); break;
        case u'\n': out += QLatin1String("&#xA;"); break;
        case u'\r': out += QLatin1String("&#xD;"); break;
        default:    out += c; break;
        }
    }
    return out;
}

QString xmlText(const QString& text)
{
    QString out;
    out.reserve(text.size() + 8);
    for (const QChar c : text) {
        switch (c.unicode()) {
        case u'&': out += QLatin1String("&amp;"); break;
        case u'<': out += QLatin1String("&lt;"); break;
        case u'>': out += QLatin1String("&gt;"); break;
        default:   out += c; break;
        }
    }
    return out;
}

}  // namespace

bool isVersionString(const QString& text, int* partsOut)
{
    const QStringList parts = text.split(QLatin1Char('.'));
    if (partsOut) *partsOut = int(parts.size());
    if (parts.isEmpty() || parts.size() > 4) return false;
    for (const QString& p : parts) {
        if (p.isEmpty()) return false;
        for (const QChar c : p)
            if (c < QLatin1Char('0') || c > QLatin1Char('9')) return false;
        bool ok = false;
        p.toInt(&ok);
        if (!ok) return false;
    }
    return true;
}

Result write(const QString& outPath)
{
    Result r;
    const Gathered g = gather();
    if (!g.error.isEmpty()) {
        r.error = g.error;
        return r;
    }
    r.files = int(g.items.size());
    r.bytes = g.bytes;
    r.inContainer = g.inContainer;
    r.notInIndex = g.notInIndex;

    QVector<zipwriter::Entry> entries;
    entries.reserve(g.items.size() + 2);

    QString manifest;
    QTextStream man(&manifest);
    man << "asset\tbytes\tmd5\tpathCode64\tgameCopy\tcontainer\tgameBytes\n";
    for (const Item& it : g.items) {
        entries.append({assetToZipName(it.asset), it.data, it.modified});
        man << it.asset << '\t' << it.data.size() << '\t' << md5Of(it.data)
            << '\t'
            << QStringLiteral("0x%1").arg(
                   fox::hashFileNameWithExtension(it.asset), 16, 16,
                   QLatin1Char('0'))
            << '\t'
            << (it.inIndex ? it.gameCopy
                           : QStringLiteral("(not in this install's index)"))
            << '\t'
            << (!it.inIndex ? QStringLiteral("?")
                            : it.inContainer ? QStringLiteral("yes")
                                             : QStringLiteral("no"))
            << '\t' << it.gameBytes << '\n';
    }
    man.flush();
    r.manifest = manifest;

    QString readme;
    QTextStream rd(&readme);
    rd << "FOXAssetBrowser mod package\n"
       // The newest replacement's date, NOT the time this ran: an archive that
       // says when it was zipped cannot be compared byte for byte against the
       // same folder zipped again, and this project's exports are.
       << "newest replacement " << g.newest.toString(Qt::ISODate) << "\n\n"
       << r.files << " replacement asset(s), " << r.bytes
       << " byte(s) before compression.\n\n"
       << "The Assets/ tree in this archive IS a mod folder. Unzip it over "
          "one\nand every file in it is mounted over the game's own copy; "
          "delete the\nfiles again and the game's copies are back. Nothing "
          "here writes into a\n.dat, .qar or .fpk, and installing this cannot "
          "damage a game install.\n\n"
       << "manifest.tsv records, per asset: its size and MD5, its Fox Engine\n"
          "PathCode64, and whether the GAME's own copy of that asset lives "
          "inside\nan .fpk/.pftxs container.\n\n"
       << "This is the PLAIN package. For one a mod manager installs, use\n"
          "Export > Package as a SnakeBite mod, which writes a .mgsv.\n";
    rd.flush();

    entries.append({QStringLiteral("manifest.tsv"), manifest.toUtf8(), g.newest});
    entries.append({QStringLiteral("README.txt"), readme.toUtf8(), g.newest});

    r.error = zipwriter::write(outPath, entries);
    return r;
}

Result writeMgsv(const QString& outPath, const MgsvMeta& meta)
{
    Result r;
    if (meta.name.trimmed().isEmpty()) {
        r.error = QStringLiteral(
            "A mod needs a name — it is what the mod manager lists it under.");
        return r;
    }
    // The mod's own Version is a string SnakeBite stores and never compares,
    // so it only has to parse. The two gated fields are compared with
    // System.Version, where a missing component is -1 rather than 0 — so
    // "0.8" sorts below "0.8.0.0" and is refused at install. Measured, not
    // assumed; see ModPackage.h.
    int parts = 0;
    if (!isVersionString(meta.version, &parts)) {
        r.error = QStringLiteral(
            "Version '%1' is not a version SnakeBite can read. It wants one to "
            "four numbers separated by dots, such as 1.0.0.0.")
            .arg(meta.version);
        return r;
    }
    for (const auto& pair : {qMakePair(QStringLiteral("MGSVersion"), meta.mgsVersion),
                             qMakePair(QStringLiteral("SBVersion"), meta.sbVersion)}) {
        if (!isVersionString(pair.second, &parts) || parts != 4) {
            r.error = QStringLiteral(
                "%1 '%2' must be exactly four numbers separated by dots, such "
                "as %3. SnakeBite compares this field with System.Version, "
                "which treats a missing component as lower than zero — so a "
                "three-part version sorts BELOW the four-part one it is "
                "checked against and the mod is refused at install time.")
                .arg(pair.first, pair.second,
                     pair.first == QLatin1String("SBVersion")
                         ? QStringLiteral("0.8.0.0")
                         : QStringLiteral("0.0.0.0"));
            return r;
        }
    }

    const Gathered g = gather();
    if (!g.error.isEmpty()) {
        r.error = g.error;
        return r;
    }
    r.files = int(g.items.size());
    r.bytes = g.bytes;
    r.inContainer = g.inContainer;
    r.notInIndex = g.notInIndex;

    // The refusal this whole format turns on — see ModPackage.h. Every
    // offender is named, because "some of your files are the wrong kind" is
    // not something anyone can act on.
    for (const Item& it : g.items)
        if (it.inContainer) r.blocked << it.asset;
    if (!r.blocked.isEmpty()) {
        r.error = QStringLiteral(
            "%1 of the %2 replacement(s) replace a file the game keeps INSIDE "
            "a container, and a SnakeBite mod has to re-pack those into that "
            "container rather than install them beside it. Writing an .fpk is "
            "not implemented yet, and a package that listed them as ordinary "
            "entries would install cleanly and change nothing. Use the plain "
            "package for these, or remove them:\n    %3")
            .arg(r.blocked.size()).arg(r.files)
            .arg(r.blocked.join(QStringLiteral("\n    ")));
        return r;
    }

    QVector<zipwriter::Entry> entries;
    entries.reserve(g.items.size() + 1);

    QString xml;
    QTextStream x(&xml);
    // Byte-for-byte the shape System.Xml.Serialization produces for this
    // class, down to the two unused namespace declarations, the two-space
    // indent and the space before every "/>". CRLF because MakeBite runs on
    // Windows and that is what StreamWriter writes there.
    const QString nl = QStringLiteral("\r\n");
    x << "<?xml version=\"1.0\" encoding=\"utf-8\"?>" << nl
      << "<ModEntry xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\" "
         "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\""
      << " Name=\"" << xmlAttr(meta.name.trimmed()) << '"'
      << " Version=\"" << xmlAttr(meta.version) << '"'
      << " Author=\"" << xmlAttr(meta.author) << '"'
      << " Website=\"" << xmlAttr(meta.website) << "\">" << nl
      << "  <MGSVersion Version=\"" << xmlAttr(meta.mgsVersion) << "\" />" << nl
      << "  <SBVersion Version=\"" << xmlAttr(meta.sbVersion) << "\" />" << nl;
    // A carriage return inside element text is not round-trip safe — a parser
    // normalises it away — so the description is normalised here rather than
    // written and quietly changed by whoever reads it back.
    QString desc = meta.description;
    desc.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    desc.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    // An EMPTY description is self-closed, because that is what .NET writes
    // for an empty string and this file is measured byte for byte against
    // .NET's own output. The two forms mean the same thing to a parser; the
    // point of matching is that a difference here would mean a difference
    // somewhere it does not.
    if (desc.isEmpty())
        x << "  <Description />" << nl;
    else
        x << "  <Description>" << xmlText(desc) << "</Description>" << nl;
    x << "  <QarEntries>" << nl;
    for (const Item& it : g.items) {
        // Hash is a ulong on the C# side, so it serialises as DECIMAL. Writing
        // the hex this tool shows everywhere else would deserialise as zero.
        const quint64 hash = fox::hashFileNameWithExtension(it.asset);
        // SnakeBite's own rule, and it is about the FILE being installed, not
        // about where the game keeps it: an .fpk goes in compressed.
        const QString ext = QFileInfo(it.asset).suffix().toLower();
        const bool compressed = ext.contains(QLatin1String("fpk"));
        x << "    <QarEntry Hash=\"" << QString::number(hash)
          << "\" FilePath=\"" << xmlAttr(it.asset)
          << "\" Compressed=\"" << (compressed ? "true" : "false")
          << "\" ContentHash=\"" << md5Of(it.data).toUpper() << "\" />" << nl;
        entries.append({assetToZipName(it.asset), it.data, it.modified});
    }
    x << "  </QarEntries>" << nl
      // ALWAYS written, even though it is always empty. A null list on the C#
      // side omits the element, and an omitted element deserialises to null
      // rather than to an empty list — which is a null dereference waiting in
      // whatever iterates it. An empty element is an empty list.
      << "  <FpkEntries />" << nl
      << "</ModEntry>";
    x.flush();

    entries.append({QStringLiteral("metadata.xml"), xml.toUtf8(), g.newest});
    r.manifest = xml;
    r.error = zipwriter::write(outPath, entries);
    return r;
}

}  // namespace modpackage
