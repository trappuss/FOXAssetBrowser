// Extract.h — the extraction vocabulary shared by the Files tab and the Bulk
// Extractor (one implementation so single-file and bulk extraction can never
// disagree about names or texture assembly).
#pragma once
#include <QByteArray>
#include <QImage>
#include <QString>

namespace fox {
struct IndexedFile;
struct FmdlTextureRef;
}

namespace extract {

// Disk-relative path for an indexed file: resolved names keep their full
// /Assets/... structure; unresolved ones land under unresolved/<hex>.<ext>.
// Always forward-slashed, never absolute, never escapes the output root.
QString relativePathFor(const fox::IndexedFile& f);

// True when this file is a .ftex (candidate for DDS assembly).
bool isFtex(const fox::IndexedFile& f);
// True for .N.ftexs streams (skippable when assembling textures instead).
bool isFtexs(const fox::IndexedFile& f);

// Assemble a .ftex + its streamed .N.ftexs mips into one .dds. Sibling streams
// are found by hash across ALL indexed archives (TPP keeps them in separate
// texture .dat files) or inside the same PFTXS pack for packed textures.
// Returns empty + `error` on failure; `missingMips` > 0 means a degraded (but
// valid) DDS with the top mips absent.
// `lowRes` assembles from the mips stored INSIDE the .ftex and skips the
// .N.ftexs stream files entirely. The result is the small end of the mip chain
// — a few hundred pixels rather than 2K or 4K — which is all a thumbnail can
// show, and it avoids reading and inflating the streams, which is nearly all of
// the cost of a full assembly.
QByteArray assembleFtexToDds(const fox::IndexedFile& f, QString* error = nullptr,
                             int* missingMips = nullptr, bool lowRes = false);

// Resolve an FMDL texture reference to a decoded RGBA image: find the .ftex in
// the indexed archives (TPP: by the stored PathCode64; GZ: by legacy-hashing
// the stored path string with the ftex extension id), assemble, decode.
// Null image when the texture is not present in any indexed archive.
QImage textureImageFor(const fox::FmdlTextureRef& ref, bool gzModel,
                       bool lowRes = false);

// Drop every decoded texture textureImageFor() is holding. It already discards
// itself when a rebuilt index is installed, so this is only for handing the
// memory back sooner (a rescan) or for a cold measurement.
void clearTextureCache();

// ── The run-scoped decode cache (template §8) ──────────────────────────────
// A bulk run over textures decompresses the SAME container over and over. The
// measured case is a PFTXS pack: assembleFtexToDds reads and inflates the whole
// pack to find one texture's .ftexs siblings, so a pack holding 200 textures is
// inflated 200 times. The container grouping in the extractor already solves
// this for the entries themselves; it does nothing for the sibling lookups
// inside the assembler, which is where the cost actually is.
//
// So: a bounded, mutex-guarded cache of DECOMPRESSED archive-entry blobs, keyed
// on (archiveId, entryIdx). It exists only while a Scope is alive — RAII, so a
// cancelled or crashed run cannot leave hundreds of megabytes behind — and it
// is consulted only through readEntryCached(), so a caller outside a run gets
// exactly the behaviour it had before this existed.
//
// Not a disk cache and deliberately not a global one: the blobs are worth
// keeping for the minutes a run lasts and worthless afterwards.
namespace blobcache {

// Read one archive entry, through the active scope's cache when there is one.
// Identical bytes either way.
QByteArray readEntry(int archiveId, int entryIdx);

// Bytes currently held, and how many reads the cache answered. Both are for
// the run's own log line — a cache nobody can measure is a cache nobody can
// justify keeping.
struct Stats { qint64 bytes = 0; qint64 hits = 0; qint64 misses = 0; int entries = 0; };
Stats stats();

class Scope {
public:
    // `budgetBytes` is a ceiling, not a target: the cache evicts its
    // least-recently-used entries down to it. One entry larger than the whole
    // budget is cached anyway for the duration of that read and then dropped,
    // because refusing it would turn the pathological case into a permanent
    // miss.
    explicit Scope(qint64 budgetBytes = 512LL * 1024 * 1024);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

}  // namespace blobcache

}  // namespace extract
