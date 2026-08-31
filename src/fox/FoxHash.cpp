// FoxHash.cpp — port of GzsTool.Core/Utility/Hashing.cs (the reference
// implementation the modding community round-trips the shipped archives with).
// Every rule here mirrors that file: the FIRST '.' starts the extension, the
// /Assets/ prefix is stripped before hashing, "tpptest" and non-/Assets/ paths
// carry the meta flag, and seed1 is the last eight characters of the text
// packed little-endian from the END backwards.
#include "fox/FoxHash.h"

#include <QFile>
#include <QTextStream>
#include <cstring>
#include <thread>
#include <vector>

#include "fox/FoxCity.h"
#include "fox/GzsFile.h"

namespace fox {
namespace {

constexpr quint64 kSeed0 = 0x9ae16a3b2f90404fULL;

// GzsTool's known-extension list. The 13-bit code of each is computed, not
// hard-coded, so the table can never drift from the hash function.
// The .lng2 spellings were MISSING and every in-game name depended on them.
// hashFileNameWithExtension splits at the FIRST dot, so a language table is
// "<stem>.eng.lng2" — extension part "eng.lng2". An extension not in this list
// hashes to type id 0, so the file resolves as "<path>._unknown", `named` comes
// back false, and NameCatalog — which skips unnamed files — never saw a single
// language table. That is why every gear item, colour and part showed its id
// instead of its name. GZ's ".lng#eng" suffix form is a different scheme and is
// handled by tryResolveGzs, not here.
const char* const kKnownExtensions[] = {
    "1.ftexs", "1.nav2", "2.ftexs", "3.ftexs", "4.ftexs", "5.ftexs", "6.ftexs",
    "ag.evf", "aia", "aib", "aibc", "aig", "aigc", "aim", "aip", "ait", "atsh",
    "bnd", "bnk", "cc.evf", "clo", "csnav", "dat", "des", "dnav", "dnav2",
    "eng.lng", "eng.lng2", "ese", "evb", "evf", "fag", "fage", "fago", "fagp",
    "fagx",
    "fclo", "fcnp", "fcnpx", "fdes", "fdmg", "ffnt", "fmdl", "fmdlb", "fmtt",
    "fnt", "fova", "fox", "fox2", "fpk", "fpkd", "fpkl", "frdv", "fre.lng",
    "fre.lng2",
    "frig", "frt", "fsd", "fsm", "fsml", "fsop", "fstb", "ftex", "fv2",
    "fx.evf", "fxp", "gani", "geom", "ger.lng", "ger.lng2", "gpfp", "grxla",
    "grxoc", "gskl", "htre", "info", "ita.lng", "ita.lng2", "jpn.lng",
    "jpn.lng2", "json", "lad", "ladb",
    "lani", "las", "lba", "lng", "lng2", "lpsh", "lua", "mas", "mbl", "mog",
    "mtar",
    "mtl", "nav2", "nta", "obr", "obrb", "param", "parts", "path", "pftxs",
    "pcsp", "ph", "phep", "phsd", "por.lng", "por.lng2", "qar", "rbs", "rdb",
    "rdf",
    "rnav", "rus.lng", "rus.lng2", "sad", "sand", "sani", "sbp", "sd.evf",
    "sdf", "sim", "simep",
    "snav", "spa.lng", "spa.lng2", "spch", "sub", "subp", "tgt", "tre2", "txt",
    "uia",
    "uif", "uig", "uigb", "uil", "uilb", "utxl", "veh", "vfx", "vfxbin",
    "vfxdb", "vnav", "vo.evf", "vpc", "wem", "wmv", "xml",
};

quint64 cityOf(const QByteArray& latin, quint64 seed1)
{
    return foxcity::CityHash64WithSeeds(latin.constData(),
                                        static_cast<size_t>(latin.size()),
                                        kSeed0, seed1);
}

bool startsWithBytes(const char* s, int n, const char* lit, int litLen)
{
    return n >= litLen && std::memcmp(s, lit, size_t(litLen)) == 0;
}

}  // namespace

// The QString entry points below convert to Latin-1 FIRST and then run these,
// so there is exactly ONE implementation of each rule and the byte and string
// forms cannot drift apart. Converting first is not a behaviour change:
// toLatin1() maps every UTF-16 code unit to exactly one byte (unrepresentable
// ones to '?'), so offsets are 1:1 and every rule here — the first '.', the
// "/Assets/" prefix, "tpptest", leading '/' — is ASCII and lands identically
// either way.
//
// They exist because the dictionaries are the startup cost: 410k lines across
// six files, and hashing each one through QString cost a copy, a truncate and
// a toLatin1() twice over — measured at 1,278 ms of a 1,306 ms index build.
quint64 hashFileNameLatin1(const char* s, int n, bool removeExtension)
{
    if (removeExtension) {
        if (const void* dot = std::memchr(s, '.', size_t(n)))
            n = int(static_cast<const char*>(dot) - s);
    }

    bool metaFlag = false;
    if (startsWithBytes(s, n, "/Assets/", 8)) {
        s += 8;
        n -= 8;
        if (startsWithBytes(s, n, "tpptest", 7)) metaFlag = true;
    } else {
        metaFlag = true;
    }
    while (n > 0 && *s == '/') { ++s; --n; }

    // seed1: the LAST up-to-8 characters, packed little-endian walking backwards
    // (byte 0 = last char, byte 1 = second-to-last, ...). Exactly Hashing.cs.
    quint64 seed1 = 0;
    for (int i = n - 1, j = 0; i >= 0 && j < 8; --i, ++j)
        seed1 |= static_cast<quint64>(static_cast<quint8>(s[i])) << (8 * j);

    const quint64 masked =
        foxcity::CityHash64WithSeeds(s, size_t(n), kSeed0, seed1) & kPathMask;
    return metaFlag ? (masked | kMetaFlag) : masked;
}

quint64 hashFileNameLegacyLatin1(const char* s, int n, bool removeExtension)
{
    if (removeExtension) {
        if (const void* dot = std::memchr(s, '.', size_t(n)))
            n = int(static_cast<const char*>(dot) - s);
    }
    const quint64 seed1 =
        n == 0 ? 0
               : (static_cast<quint64>(static_cast<quint8>(s[0])) << 16)
                     + static_cast<quint64>(n);
    // The trailing NUL is part of the hashed text in the legacy scheme. Copying
    // is unavoidable for that one byte, but the copy is a small stack buffer for
    // ordinary paths rather than a heap QByteArray.
    char stackBuf[256];
    QByteArray heapBuf;
    char* buf = stackBuf;
    if (n + 1 > int(sizeof(stackBuf))) {
        heapBuf.resize(n + 1);
        buf = heapBuf.data();
    }
    std::memcpy(buf, s, size_t(n));
    buf[n] = '\0';
    return foxcity::CityHash64WithSeeds(buf, size_t(n) + 1, kSeed0, seed1)
           & 0xFFFFFFFFFFFFULL;
}

quint64 hashFileName(const QString& textIn, bool removeExtension)
{
    const QByteArray latin = textIn.toLatin1();
    return hashFileNameLatin1(latin.constData(), int(latin.size()),
                              removeExtension);
}

quint64 hashExtension(const QString& ext)
{
    return hashFileName(ext, /*removeExtension=*/false) & 0x1FFF;
}

quint64 hashFileNameWithExtension(const QString& filePathIn)
{
    QString filePath = filePathIn;
    filePath.replace(QLatin1Char('\\'), QLatin1Char('/'));

    QString hashablePart = filePath, extensionPart;
    const int dot = filePath.indexOf(QLatin1Char('.'));
    if (dot != -1) {
        hashablePart = filePath.left(dot);
        extensionPart = filePath.mid(dot + 1);
    }

    quint64 typeId = 0;
    for (const char* known : kKnownExtensions) {
        if (extensionPart == QLatin1String(known)) {
            typeId = hashExtension(extensionPart);
            break;
        }
    }
    return (typeId << 51) | hashFileName(hashablePart);
}

quint64 hashFileNameLegacy(const QString& textIn, bool removeExtension)
{
    const QByteArray latin = textIn.toLatin1();
    return hashFileNameLegacyLatin1(latin.constData(), int(latin.size()),
                                    removeExtension);
}

// ── HashResolver ─────────────────────────────────────────────────────────────

HashResolver& HashResolver::instance()
{
    static HashResolver r;
    return r;
}

HashResolver::HashResolver()
{
    for (const char* ext : kKnownExtensions)
        m_extensions.insert(hashExtension(QLatin1String(ext)), QLatin1String(ext));

    // Built-in StrCode64 vocabulary: the fixed engine strings FMDL materials
    // reference (texture roles, common shader names). Dictionaries only cover
    // asset paths, so without these every TPP material role shows as hex.
    static const char* const kKnownStrings[] = {
        "Base_Tex_SRGB", "NormalMap_Tex_NORMAL", "SpecularMap_Tex_LIN",
        "Translucent_Tex_LIN", "Layer_Tex_SRGB", "LayerMask_Tex_LIN",
        "Detail_Tex_SRGB", "SubNormalMap_Tex_NORMAL", "Dirty_Tex_SRGB",
        "Wrinkle_Nrm_Tex_NORMAL", "Wrinkle_Msk_Tex_LIN", "Ripple_Tex_LIN",
        "MaskMap_Tex_LIN", "Incidence_Roughness_Tex_LIN", "Fresnel_Tex_LIN",
        "Layer1_Tex_SRGB", "Sky_Tex_SRGB", "RoughnessMap_Tex_LIN",
        "MetalnessMap_Tex_LIN", "OcclusionMap_Tex_LIN", "Emissive_Tex_SRGB",
        "MatParamIndex_0", "MatParamIndex_1", "MatParamIndex_2",
        "CM_Base_Tex_SRGB", "CM_NormalMap_Tex_NORMAL", "CM_SpecularMap_Tex_LIN",
        // Recovered by hashing candidate names against the role codes that
        // actually occur in TPP/MGO models (the _NORMAL spellings above never
        // matched anything — the shipped names use _NRM):
        //   NormalMap_Tex_NRM     0xcc4305511ae0   356 materials
        //   SubNormalMap_Tex_NRM  0x455c5122ceee     6
        //   Dirty_Tex_LIN         0x5f967bcbb6cb   112
        //   Mask_Tex_LIN          0x3ed9b75cd480     9
        "NormalMap_Tex_NRM", "SubNormalMap_Tex_NRM", "Dirty_Tex_LIN",
        "Mask_Tex_LIN",
    };
    // Queued FIRST, so the first-wins rule the tables apply keeps these ahead
    // of any dictionary line that happens to collide with one.
    m_blob.append('\0');   // offset 0 is the empty-slot marker; never an entry
    for (const char* s : kKnownStrings)
        queue(s, int(std::strlen(s)), /*pathIndexed=*/false);
}

// ── The flat tables ─────────────────────────────────────────────────────────
namespace {
// One multiply-xor round of the 64-bit finaliser. The keys are already hashes,
// but their LOW bits are what a mask selects and the legacy scheme's low bits
// carry the length and first character — so masking them raw clusters badly.
inline quint64 spread(quint64 x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return x;
}
}  // namespace

void HashResolver::Table::reset(qsizetype wantEntries)
{
    // Half-full at most: linear probing degrades sharply past that, and the
    // memory is 16 bytes a slot rather than a node and a QString each.
    qsizetype cap = 16;
    while (cap < wantEntries * 2) cap <<= 1;
    cells.fill(Slot{}, cap);
    mask = quint64(cap) - 1;
    count = 0;
}

void HashResolver::Table::insert(quint64 key, quint32 off, quint32 len)
{
    if (cells.isEmpty() || len == 0) return;
    quint64 i = spread(key) & mask;
    while (cells[qsizetype(i)].len) {
        // FIRST WINS, exactly as the QHash form did: a later dictionary that
        // repeats a path must not replace the text already stored for it.
        if (cells[qsizetype(i)].key == key) return;
        i = (i + 1) & mask;
    }
    cells[qsizetype(i)] = Slot{key, off, len};
    ++count;
}

const HashResolver::Slot* HashResolver::Table::find(quint64 key) const
{
    if (cells.isEmpty()) return nullptr;
    quint64 i = spread(key) & mask;
    while (cells[qsizetype(i)].len) {
        if (cells[qsizetype(i)].key == key) return &cells[qsizetype(i)];
        i = (i + 1) & mask;
    }
    return nullptr;
}

void HashResolver::queue(const char* text, int len, bool pathIndexed)
{
    if (len <= 0) return;
    Pending p;
    p.pathIndexed = pathIndexed;
    p.off = quint32(m_blob.size());
    p.len = quint32(len);
    m_blob.append(text, len);
    // Hashed from the BLOB's copy, not the caller's buffer: identical bytes,
    // and it keeps the one place that decides what a record's text is.
    const char* at = m_blob.constData() + p.off;
    p.path = hashFileNameLatin1(at, len) & kPathMask;
    p.legacy = hashFileNameLegacyLatin1(at, len);
    m_pending.append(p);
    m_built.store(false, std::memory_order_release);
}

void HashResolver::build() const
{
    std::lock_guard<std::mutex> lock(m_buildMutex);
    if (m_built.load(std::memory_order_relaxed)) return;   // won the race
    const qsizetype n = m_pending.size();
    // THREE THREADS, one per table. The tables share nothing — each reads the
    // same const pending vector and writes its own slots — and the work is
    // dominated by a cache miss per insert, which is exactly what parallelises.
    const auto fill = [this](Table& t, int which) {
        t.reset(m_pending.size());
        for (const Pending& p : m_pending) {
            switch (which) {
            case 0:
                if (p.pathIndexed) t.insert(p.path, p.off, p.len);
                break;
            case 1: t.insert(p.legacy, p.off, p.len); break;
            default:
                t.insert(quint64(quint32(p.legacy & 0xFFFFFFFFu)), p.off, p.len);
                break;
            }
        }
    };
    if (n > 4096) {
        std::thread a([&] { fill(m_names, 0); });
        std::thread b([&] { fill(m_legacyNames, 1); });
        fill(m_strCode32, 2);
        a.join();
        b.join();
    } else {
        // Below a few thousand entries the threads cost more than they save.
        fill(m_names, 0);
        fill(m_legacyNames, 1);
        fill(m_strCode32, 2);
    }
    m_built.store(true, std::memory_order_release);
}

int HashResolver::loadDictionary(const QString& filePath)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    // Whole file, split by hand. QTextStream::readLine() decodes and allocates
    // per line and trimmed() allocates again, which across seven dictionaries
    // and 410k lines was the single largest cost of starting the program up.
    const QByteArray blob = f.readAll();
    f.close();
    if (blob.isEmpty()) return 0;

    // Dictionaries are ASCII, one path per line — every shipped one is (checked
    // byte by byte across all of them). Reading them as bytes rather than
    // through a decoding QTextStream is what makes this fast, and the one thing
    // that costs is a BYTE ORDER MARK: a text editor that adds one on save
    // would otherwise turn the first path into a different string and silently
    // drop it. Skipped explicitly. A non-ASCII byte elsewhere is taken as
    // Latin-1; the old path decoded UTF-8 and substituted '?', so such a line
    // would hash differently — no shipped dictionary contains one.
    int begin = 0;
    if (blob.size() >= 3 && quint8(blob[0]) == 0xEF && quint8(blob[1]) == 0xBB
        && quint8(blob[2]) == 0xBF)
        begin = 3;

    m_blob.reserve(m_blob.size() + blob.size());
    ++m_dictFiles;

    // ── Parse in parallel, append in order ──────────────────────────────
    // The bytes go into the blob FIRST, in one memcpy, so every record's
    // offset is final before any worker runs and the workers never touch the
    // blob. Then the byte range is split at newline boundaries and each chunk
    // is trimmed and hashed on its own thread into its own vector, and the
    // vectors are concatenated in chunk order — so m_pending comes out in
    // exactly the order a single-threaded walk would have produced it, which
    // is what the first-wins rule depends on.
    const qsizetype base = m_blob.size();
    m_blob.append(blob.constData() + begin, blob.size() - begin);
    const char* const data = m_blob.constData() + base;
    const qsizetype dataLen = m_blob.size() - base;

    const int hw = int(std::thread::hardware_concurrency());
    // Below a megabyte the split costs more than the walk: the small
    // dictionaries are a few hundred kilobytes and finish in single-digit
    // milliseconds either way.
    const int workers = (dataLen < (1 << 20) || hw <= 1)
        ? 1
        : qBound(1, hw, 16);

    // Chunk boundaries, snapped FORWARD to just after a newline so no line is
    // split across two workers and none is counted twice.
    QVector<qsizetype> bounds;
    bounds.reserve(workers + 1);
    bounds.append(0);
    for (int w = 1; w < workers; ++w) {
        qsizetype at = dataLen * w / workers;
        while (at < dataLen && data[at] != '\n') ++at;
        if (at < dataLen) ++at;   // start after the newline
        if (at > bounds.last()) bounds.append(at);
    }
    bounds.append(dataLen);

    const int chunks = int(bounds.size()) - 1;
    QVector<QVector<Pending>> parts(chunks);
    const auto runChunk = [&](int c) {
        QVector<Pending>& out = parts[c];
        // ~70 bytes a line over the shipped dictionaries; over-reserving costs
        // one allocation's worth of address space and under-reserving costs
        // regrowth of a vector holding hundreds of thousands of records.
        out.reserve(int((bounds[c + 1] - bounds[c]) / 48) + 16);
        const char* p = data + bounds[c];
        const char* const stop = data + bounds[c + 1];
        while (p < stop) {
            const char* const nl =
                static_cast<const char*>(std::memchr(p, '\n', size_t(stop - p)));
            const char* lineEnd = nl ? nl : stop;
            const char* lineStart = p;
            p = nl ? nl + 1 : stop;
            // trimmed(): the shipped dictionaries carry CRLF line endings in
            // places and the occasional trailing space.
            while (lineStart < lineEnd
                   && static_cast<quint8>(*lineStart) <= ' ') ++lineStart;
            while (lineEnd > lineStart
                   && static_cast<quint8>(lineEnd[-1]) <= ' ') --lineEnd;
            const int len = int(lineEnd - lineStart);
            if (len == 0) continue;
            Pending rec;
            rec.off = quint32(base + (lineStart - data));
            rec.len = quint32(len);
            rec.path = hashFileNameLatin1(lineStart, len) & kPathMask;
            rec.legacy = hashFileNameLegacyLatin1(lineStart, len);
            out.append(rec);
        }
    };
    if (chunks == 1) {
        runChunk(0);
    } else {
        std::vector<std::thread> pool;
        pool.reserve(size_t(chunks - 1));
        for (int c = 1; c < chunks; ++c)
            pool.emplace_back([&runChunk, c] { runChunk(c); });
        runChunk(0);
        for (std::thread& t : pool) t.join();
    }

    int added = 0;
    for (const QVector<Pending>& part : parts) added += part.size();
    m_pending.reserve(m_pending.size() + added);
    for (const QVector<Pending>& part : parts)
        for (const Pending& rec : part) m_pending.append(rec);
    m_built.store(false, std::memory_order_release);
    return added;
}

int HashResolver::size() const
{
    ensureBuilt();
    return m_names.count;
}

QString HashResolver::legacyNameFor(quint64 hash) const
{
    ensureBuilt();
    return textAt(m_legacyNames.find(hash & 0xFFFFFFFFFFFFULL));
}

QString HashResolver::strCode32NameFor(quint32 hash) const
{
    ensureBuilt();
    return textAt(m_strCode32.find(quint64(hash)));
}

QList<QString> HashResolver::allNames() const
{
    ensureBuilt();
    QList<QString> out;
    out.reserve(m_names.count);
    for (const Slot& s : m_names.cells)
        if (s.len) out.append(textAt(&s));
    return out;
}

bool HashResolver::tryResolveGzs(quint64 hash, QString* nameOut) const
{
    // GzsTool: fileExtensionId = (hash >> 52) & 0xFFFF; path = hash & 48 bits.
    const int extId = static_cast<int>((hash >> 52) & 0xFFFF);
    const quint64 pathHash = hash & 0xFFFFFFFFFFFFULL;

    ensureBuilt();
    bool found = true;
    QString name;
    if (const Slot* s = m_legacyNames.find(pathHash)) {
        name = textAt(s);
    } else {
        name = QString::number(pathHash, 16);
        found = false;
    }
    const QString ext = GzsFile::extensionForId(extId);
    if (ext == QLatin1String("?")) found = false;
    name += ext == QLatin1String("?") ? QStringLiteral("._unknown") : ext;
    if (nameOut) *nameOut = name;
    return found;
}

QString HashResolver::extensionFor(quint64 extCode) const
{
    return m_extensions.value(extCode);
}

bool HashResolver::tryResolve(quint64 hash, QString* nameOut) const
{
    const quint64 extCode = hash >> 51;
    const quint64 pathHash = hash & kPathMask;

    ensureBuilt();
    bool found = true;
    QString name;
    if (const Slot* s = m_names.find(pathHash)) {
        name = textAt(s);
    } else {
        name = QString::number(pathHash, 16);
        found = false;
    }

    const QString ext = m_extensions.value(extCode);
    if (ext.isEmpty()) {
        name += QLatin1String("._unknown");
        found = false;
    } else {
        name += QLatin1Char('.');
        name += ext;
    }
    if (nameOut) *nameOut = name;
    return found;
}

}  // namespace fox
