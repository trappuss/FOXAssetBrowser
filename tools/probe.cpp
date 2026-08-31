// probe.cpp — console harness that exercises the fox format layer against real
// game archives (no GUI). Used for validation in development; ships disabled
// (CMake option FOX_BUILD_PROBE).
//
//   foxab_probe <dictDir> <archive.dat> [maxContainers]
//
// Prints: header info, name-resolution rate, a hash round-trip check over every
// resolved entry, an extension histogram, and parses the first few
// FPK/FPKD/PFTXS containers (which exercises Decrypt1/Decrypt2 + zlib + the
// container readers end to end).
#include <QCoreApplication>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QHash>
#include <cstdio>

#include "fox/FoxHash.h"
#include "fox/FpkFile.h"
#include "fox/FtexFile.h"
#include "fox/GzsFile.h"
#include "fox/PftxsFile.h"
#include "fox/QarFile.h"

using namespace fox;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        fprintf(stderr, "usage: foxab_probe <dictDir> <archive> [maxContainers]\n");
        return 2;
    }
    const QString dictDir = QString::fromLocal8Bit(argv[1]);
    const QString archive = QString::fromLocal8Bit(argv[2]);
    const int maxContainers = argc > 3 ? atoi(argv[3]) : 3;

    QElapsedTimer timer;
    timer.start();
    HashResolver& resolver = HashResolver::instance();
    {
        QDirIterator it(dictDir, {QStringLiteral("*.txt")}, QDir::Files);
        while (it.hasNext()) {
            const QString f = it.next();
            printf("dict %s: %d new\n", qPrintable(QFileInfo(f).fileName()),
                   resolver.loadDictionary(f));
        }
    }
    printf("dictionaries: %d names in %lld ms\n", resolver.size(),
           static_cast<long long>(timer.elapsed()));

    timer.restart();
    if (GzsFile::isGzs(archive)) {
        GzsFile gzs;
        if (!gzs.open(archive)) {
            fprintf(stderr, "FAIL g0s open: %s\n", qPrintable(gzs.errorString()));
            return 1;
        }
        printf("g0s: entries=%d in %lld ms\n", gzs.entries().size(),
               static_cast<long long>(timer.elapsed()));
        int named = 0;
        QHash<QString, int> gzHisto;
        for (const GzsEntry& e : gzs.entries()) {
            QString name;
            if (resolver.tryResolveGzs(e.hash, &name)) ++named;
            ++gzHisto[GzsFile::extensionForId(static_cast<int>((e.hash >> 52) & 0xFFFF))];
        }
        printf("resolved: %d/%d (%.1f%%)\n", named, gzs.entries().size(),
               100.0 * named / qMax(1, gzs.entries().size()));
        int shown = 0;
        for (auto it = gzHisto.constBegin(); it != gzHisto.constEnd() && shown < 40; ++it, ++shown)
            printf("  %-10s %d\n", it.key().isEmpty() ? "<none>" : qPrintable(it.key()),
                   it.value());
        // Read a few entries and validate known magics by extension.
        int checked = 0, magicOk = 0;
        for (const GzsEntry& e : gzs.entries()) {
            const int extId = static_cast<int>((e.hash >> 52) & 0xFFFF);
            const QString ext = GzsFile::extensionForId(extId);
            if (checked >= 8) break;
            if (ext == QLatin1String(".fpk") || ext == QLatin1String(".fpkd")
                || ext == QLatin1String(".pftxs") || ext == QLatin1String(".fmdl")
                || ext == QLatin1String(".ftex")) {
                const QByteArray data = gzs.readEntry(e);
                QString name;
                resolver.tryResolveGzs(e.hash, &name);
                const QByteArray head = data.left(4);
                bool ok = false;
                if (ext == QLatin1String(".fpk") || ext == QLatin1String(".fpkd")) {
                    FpkFile p;
                    ok = p.parse(data);
                    printf("entry %s: %u bytes, FPK parse %s (%d entries)\n",
                           qPrintable(name), e.size, ok ? "OK" : "FAIL",
                           ok ? p.entries().size() : 0);
                } else if (ext == QLatin1String(".pftxs")) {
                    PftxsFile p;
                    ok = p.parse(data);
                    printf("entry %s: %u bytes, PFTXS parse %s (%d groups)\n",
                           qPrintable(name), e.size, ok ? "OK" : "FAIL",
                           ok ? p.groups().size() : 0);
                } else if (ext == QLatin1String(".ftex")) {
                    FtexFile t;
                    ok = t.parse(data);
                    printf("entry %s: %u bytes, FTEX parse %s (%s)\n", qPrintable(name),
                           e.size, ok ? "OK" : "FAIL",
                           ok ? qPrintable(t.describe()) : "-");
                } else {
                    ok = head.startsWith("FMDL") || !data.isEmpty();
                    printf("entry %s: %u bytes, head=%02x%02x%02x%02x\n",
                           qPrintable(name), e.size,
                           (unsigned char)head[0], (unsigned char)head[1],
                           (unsigned char)head[2], (unsigned char)head[3]);
                }
                ++checked;
                if (ok) ++magicOk;
            }
        }
        printf("entry reads: %d/%d parsed OK\nPROBE DONE\n", magicOk, checked);
        return 0;
    }
    QarFile qar;
    if (!qar.open(archive)) {
        fprintf(stderr, "FAIL open: %s\n", qPrintable(qar.errorString()));
        return 1;
    }
    printf("qar: version=%u flags=0x%x entries=%d in %lld ms\n", qar.version(),
           qar.flags(), qar.entries().size(), static_cast<long long>(timer.elapsed()));

    // Resolution rate + FULL-hash round trip on every resolved name.
    int named = 0, roundtripFail = 0;
    QHash<QString, int> extHisto;
    for (const QarEntry& e : qar.entries()) {
        QString name;
        const bool ok = resolver.tryResolve(e.hash, &name);
        if (ok) {
            ++named;
            const quint64 rehash = hashFileNameWithExtension(name);
            if (rehash != e.hash && ++roundtripFail <= 3)
                printf("  ROUNDTRIP FAIL %s: %llx != %llx\n", qPrintable(name),
                       static_cast<unsigned long long>(rehash),
                       static_cast<unsigned long long>(e.hash));
        }
        ++extHisto[resolver.extensionFor(e.hash >> 51)];
    }
    printf("resolved: %d/%d (%.1f%%), roundtrip failures: %d\n", named,
           qar.entries().size(), 100.0 * named / qMax(1, qar.entries().size()),
           roundtripFail);

    printf("extension histogram:\n");
    for (auto it = extHisto.constBegin(); it != extHisto.constEnd(); ++it)
        printf("  %-12s %d\n",
               it.key().isEmpty() ? "<unknown>" : qPrintable(it.key()), it.value());

    // Containers: read + parse the first few (validates payload decryption and
    // inflation, since a single wrong byte breaks zlib or the child tables).
    int tried = 0;
    for (int i = 0; i < qar.entries().size() && tried < maxContainers; ++i) {
        const QarEntry& e = qar.entries()[i];
        const QString ext = resolver.extensionFor(e.hash >> 51);
        const bool isFpk = ext == QLatin1String("fpk") || ext == QLatin1String("fpkd");
        const bool isPftxs = ext == QLatin1String("pftxs");
        if (!isFpk && !isPftxs) continue;
        ++tried;
        QString name;
        resolver.tryResolve(e.hash, &name);
        timer.restart();
        const QByteArray blob = qar.readEntry(e);
        printf("container %s (%u -> %lld bytes, enc=%s, %lld ms): ", qPrintable(name),
               e.compressedSize, static_cast<long long>(blob.size()),
               e.encryption ? "yes" : "no", static_cast<long long>(timer.elapsed()));
        if (blob.isEmpty()) {
            printf("READ FAILED\n");
            continue;
        }
        if (isPftxs) {
            PftxsFile p;
            if (!p.parse(blob)) {
                printf("PARSE FAILED: %s\n", qPrintable(p.errorString()));
                continue;
            }
            int subs = 0;
            for (const PftxsGroup& g : p.groups()) subs += g.entries.size();
            printf("PFTXS ok, %d groups / %d files\n", p.groups().size(), subs);
            // End-to-end texture assembly from inside the pack: find a group
            // with a .ftex + streamed .ftexs and build the DDS.
            for (const PftxsGroup& g : p.groups()) {
                const PftxsSubEntry* ftexSub = nullptr;
                for (const PftxsSubEntry& s : g.entries)
                    if (resolver.extensionFor(s.hash >> 51) == QLatin1String("ftex"))
                        ftexSub = &s;
                if (!ftexSub) continue;
                const QByteArray ftexData = PftxsFile::readEntry(blob, *ftexSub);
                FtexFile t;
                if (!t.parse(ftexData)) {
                    printf("  pack ftex: parse failed: %s\n",
                           qPrintable(t.errorString()));
                    break;
                }
                int missing = 0;
                const QByteArray dds = t.assembleDds(
                    ftexData,
                    [&](int fileNo) -> QByteArray {
                        const QString want = QStringLiteral("%1.ftexs").arg(fileNo);
                        for (const PftxsSubEntry& s : g.entries)
                            if (resolver.extensionFor(s.hash >> 51) == want)
                                return PftxsFile::readEntry(blob, s);
                        return {};
                    },
                    &missing);
                QString name;
                resolver.tryResolve(ftexSub->hash, &name);
                const bool magicOk = dds.size() > 128
                    && dds.startsWith(QByteArray("DDS ", 4));
                printf("  pack ftex %s: %s -> DDS %lld bytes, magic %s, missing mips %d\n",
                       qPrintable(name), qPrintable(t.describe()),
                       static_cast<long long>(dds.size()), magicOk ? "OK" : "BAD",
                       missing);
                break;
            }
        } else {
            FpkFile p;
            if (!p.parse(blob)) {
                printf("PARSE FAILED: %s\n", qPrintable(p.errorString()));
                continue;
            }
            printf("%s ok, %d entries, %d refs\n", p.isFpkd() ? "FPKD" : "FPK",
                   p.entries().size(), p.references().size());
            for (int j = 0; j < qMin(3, static_cast<int>(p.entries().size())); ++j) {
                const FpkEntry& fe = p.entries()[j];
                const QByteArray child = FpkFile::readEntry(blob, fe);
                printf("  child %s (%d bytes read %lld)\n",
                       qPrintable(FpkFile::normalizedPath(fe.filePath)), fe.dataSize,
                       static_cast<long long>(child.size()));
            }
        }
    }
    if (tried == 0) printf("no containers in this archive\n");

    // First .ftex whose inline mips exist: header parse + inline-only DDS.
    for (const QarEntry& e : qar.entries()) {
        if (resolver.extensionFor(e.hash >> 51) != QLatin1String("ftex")) continue;
        const QByteArray data = qar.readEntry(e);
        FtexFile ftex;
        if (!ftex.parse(data)) {
            printf("ftex: parse failed: %s\n", qPrintable(ftex.errorString()));
            break;
        }
        QString name;
        resolver.tryResolve(e.hash, &name);
        int missing = 0;
        const QByteArray dds =
            ftex.assembleDds(data, [](int) { return QByteArray(); }, &missing);
        printf("ftex %s: %s -> dds %lld bytes (missing streamed mips: %d)\n",
               qPrintable(name), qPrintable(ftex.describe()),
               static_cast<long long>(dds.size()), missing);
        break;
    }

    printf("PROBE DONE\n");
    return 0;
}
