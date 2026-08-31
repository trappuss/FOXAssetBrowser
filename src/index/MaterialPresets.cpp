// MaterialPresets.cpp — see MaterialPresets.h.
#include "index/MaterialPresets.h"

#include "index/ArchiveIndex.h"

namespace fox {

MaterialPresetTable& MaterialPresetTable::instance()
{
    static MaterialPresetTable s;
    return s;
}

void MaterialPresetTable::build()
{
    // The TABLE goes too, not just the flags. Clearing only m_ok left a
    // rescan onto an install with no .fmtt still answering every lookup from
    // the previous install's rows — and nothing consults ok(), so the models
    // rendered with another game's metals while the log correctly said the
    // table was missing.
    m_file = FmttFile();
    m_ok = false;
    m_note.clear();
    const ArchiveIndex& ix = ArchiveIndex::instance();
    // The base table wins over the platform variant. They differ in two
    // presets (164 and 165, ReflectionDependDiffuse only), so the choice
    // barely matters — but making it explicitly beats taking whichever the
    // index happened to list first.
    const IndexedFile* base = nullptr;
    const IndexedFile* variant = nullptr;
    for (const IndexedFile& f : ix.files()) {
        // contains(), not endsWith(): the base copy is indexed as
        // "material_params.fmtt.fmtt" — the dictionary entry already carries
        // the extension and the archive's extension code appends it again — so
        // an endsWith() test matched only the platform variant and quietly
        // took the wrong one of the two.
        if (!f.path.contains(QLatin1String("material_params.fmtt"),
                             Qt::CaseInsensitive))
            continue;
        if (f.path.contains(QLatin1String("/steam/"), Qt::CaseInsensitive)) {
            if (!variant || (variant->shadowed && !f.shadowed)) variant = &f;
        } else if (!base || (base->shadowed && !f.shadowed)) {
            base = &f;
        }
        // (the variant branch above upgrades on `shadowed` for the same
        // reason: an install carrying only the platform copy can still have a
        // mod archive shadowing the one the index happened to list first.)
    }
    const IndexedFile* pick = base ? base : variant;
    if (!pick) {
        m_note = QStringLiteral(
            "no material_params.fmtt in the configured folders — every "
            "material renders at the dielectric default (F0 0.04)");
        return;
    }
    const QByteArray data = ArchiveIndex::instance().readFile(*pick);
    if (!m_file.parse(data)) {
        m_note = QStringLiteral("material_params.fmtt: %1").arg(m_file.errorString());
        return;
    }
    m_ok = true;
    int metals = 0;
    for (const MaterialPreset& p : m_file.presets())
        if (p.f0 > 0.5f) ++metals;
    // Report what was actually READ, not what the padded array holds. parse()
    // pads a short table to 256 so callers need no bounds rule, and reporting
    // the padded size meant a truncated file — a partial extraction, a mod —
    // was logged as "256 material preset(s)" while every index past its real
    // end silently rendered as plastic. The metals live at 164 and 165.
    const int read = int(m_file.readCount());
    m_note = QStringLiteral("%1 material preset(s), %2 metallic — %3")
                 .arg(read).arg(metals).arg(pick->path);
    if (read < 256)
        m_note += QStringLiteral("  (TRUNCATED — indices %1..255 render as the "
                                 "dielectric default)").arg(read);
}

}  // namespace fox
