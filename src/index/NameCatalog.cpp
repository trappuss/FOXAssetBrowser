// NameCatalog.cpp — see NameCatalog.h.
#include "index/NameCatalog.h"

#include <QRegularExpression>
#include <QtGlobal>

#include "fox/FoxHash.h"
#include <QSet>

#include "fox/LangFile.h"
#include "index/ArchiveIndex.h"

namespace fox {
namespace {

// The UI setting table is generated, so its shape is rigid:
//   RegistWeaponPartsInfo{type=0,partsID=TppEquip.RC_10001,
//     messageID="prts_rc_1010",ftexPath="…/ui_wpp_hg00_main0_def_alp",…}
// Only two fields matter here, and both are quoted strings.
const char* const kRecordRe =
    "RegistWeaponPartsInfo\\s*\\{([^}]*)\\}";
const char* const kMessageRe = "messageID\\s*=\\s*\"([^\"]*)\"";
const char* const kPartIdRe = "partsID\\s*=\\s*TppEquip\\.(\\w+)";
const char* const kFtexRe = "ftexPath\\s*=\\s*\"([^\"]*)\"";
// "…/WeaponPartsIcon/receiver/ui_wpp_hg00_main0_def_alp" → hg00_main0_def
const char* const kStemRe = "ui_wpp_([A-Za-z0-9_]+?)_alp$";

}  // namespace

namespace {
void logOnce(const NameCatalog& c)
{
    // TWO INDEPENDENT SOURCES, reported separately. The language tables give
    // every item, colour and menu string its words; WeaponPartsUiSetting.lua
    // gives weapon parts their icons and their part ids. An install can have
    // either without the other, and the old single line announced that BOTH
    // were missing whenever the weapon table was — which is what it said on an
    // install whose language tables had loaded perfectly well.
    if (c.langTables() > 0)
        qInfo("names: %d language table(s), %d string(s) — items and colours "
              "show their in-game names",
              c.langTables(), c.stringCount());
    else if (c.langFilesSeen() > 0)
        // THERE and unusable is a different problem from NOT THERE, and the
        // fix is different too: an install with only the French tables needs
        // the English ones, not a rescan. Saying "not in the configured
        // folders" about a file sitting in the file list is the same
        // misdiagnosis this pair of lines was written to end.
        qInfo("names: %d .lng2 table(s) found but none English or none "
              "readable — every item and colour shows its id instead of its "
              "name", c.langFilesSeen());
    else
        qInfo("names: no .lng2 language table in the configured folders — every "
              "item and colour shows its id instead of its name");
    if (c.mappedAssets() > 0)
        qInfo("names: %s", qUtf8Printable(c.describe()));
    else
        qInfo("names: no WeaponPartsUiSetting.lua — weapon parts have no icons "
              "and no part ids");
}
}  // namespace

const NameCatalog& NameCatalog::instance()
{
    static NameCatalog cache;
    const ArchiveIndex& index = ArchiveIndex::instance();
    const auto& files = index.files();
    if (cache.m_indexKey == files.constData() && cache.m_indexCount == files.size())
        return cache;
    cache.m_indexKey = files.constData();
    cache.m_indexCount = files.size();
    cache.build();
    logOnce(cache);
    return cache;
}

void NameCatalog::build()
{
    m_strings.clear();
    m_stemToLabel.clear();
    m_stemToIcon.clear();
    m_partIdToStem.clear();
    m_stemToPartIds.clear();
    m_langTables = 0;
    m_langFilesSeen = 0;

    const ArchiveIndex& index = ArchiveIndex::instance();
    const auto& files = index.files();

    static const QRegularExpression recordRe{QLatin1String(kRecordRe)};
    static const QRegularExpression messageRe{QLatin1String(kMessageRe)};
    static const QRegularExpression ftexRe{QLatin1String(kFtexRe)};
    static const QRegularExpression stemRe{QLatin1String(kStemRe)};
    static const QRegularExpression partIdRe{QLatin1String(kPartIdRe)};

    for (int i = 0; i < files.size(); ++i) {
        const IndexedFile& f = files[i];
        if (!f.named || f.shadowed) continue;

        // Localised text. Every .lng2 in the index contributes; the tables are
        // keyed by hash so several files merge without collision, and the
        // parts names live in a DIFFERENT table (lang_tpp_parts) from the
        // weapon names (lang_default_data), which is why all of them are read
        // rather than one well-known file.
        if (f.path.endsWith(QLatin1String(".lng2"), Qt::CaseInsensitive)) {
            ++m_langFilesSeen;
            // Only the English tables: reading all eight languages would
            // quadruple the work and the last one written would win.
            if (!f.path.contains(QLatin1String(".eng."), Qt::CaseInsensitive)
                && !f.path.contains(QLatin1String("_eng"), Qt::CaseInsensitive))
                continue;
            LangFile lg;
            const QByteArray d = index.readFile(f);
            if (d.isEmpty() || !lg.parse(d)) continue;
            for (auto it = lg.strings().constBegin(); it != lg.strings().constEnd(); ++it)
                if (!m_strings.contains(it.key())) m_strings.insert(it.key(), it.value());
            ++m_langTables;
            continue;
        }

        if (!f.path.endsWith(QLatin1String("WeaponPartsUiSetting.lua"),
                             Qt::CaseInsensitive))
            continue;
        const QByteArray d = index.readFile(f);
        if (d.isEmpty()) continue;
        const QString text = QString::fromLatin1(d);
        auto it = recordRe.globalMatch(text);
        while (it.hasNext()) {
            const QString rec = it.next().captured(1);
            const QRegularExpressionMatch mm = messageRe.match(rec);
            const QRegularExpressionMatch fm = ftexRe.match(rec);
            if (!mm.hasMatch() || !fm.hasMatch()) continue;
            const QRegularExpressionMatch sm = stemRe.match(fm.captured(1));
            if (!sm.hasMatch()) continue;
            const QRegularExpressionMatch pm = partIdRe.match(rec);
            if (pm.hasMatch() && !m_partIdToStem.contains(pm.captured(1))) {
                m_partIdToStem.insert(pm.captured(1), sm.captured(1));
                m_stemToPartIds.insert(sm.captured(1), pm.captured(1));
            }
            // Several parts share one icon (and so one stem); the first
            // record wins, which is the lowest-numbered variant.
            if (!m_stemToLabel.contains(sm.captured(1)))
                m_stemToLabel.insert(sm.captured(1), mm.captured(1));
            if (!m_stemToIcon.contains(sm.captured(1)))
                m_stemToIcon.insert(sm.captured(1), fm.captured(1));
        }
    }
}

QString NameCatalog::textForLabel(const QString& label) const
{
    if (label.isEmpty()) return {};
    return m_strings.value(
        quint32(hashFileNameLegacy(label, /*removeExtension=*/false) & 0xFFFFFFFFu));
}

QString NameCatalog::iconPathFor(const QString& modelStem) const
{
    return m_stemToIcon.value(modelStem);
}

QString NameCatalog::stemForPartId(const QString& partId) const
{
    return m_partIdToStem.value(partId);
}

QStringList NameCatalog::partIdsForStem(const QString& modelStem) const
{
    return m_stemToPartIds.values(modelStem);
}

QString NameCatalog::nameFor(const QString& modelStem) const
{
    return textForLabel(m_stemToLabel.value(modelStem));
}

QStringList NameCatalog::knownLabels() const
{
    // The VALUES of the stem→label map: the labels themselves, which is what a
    // reverse hash lookup needs. Deduplicated, because one label can name
    // several stems (a part at two weapon grades shares its name).
    QSet<QString> seen;
    QStringList out;
    for (auto it = m_stemToLabel.constBegin(); it != m_stemToLabel.constEnd(); ++it) {
        if (it.value().isEmpty() || seen.contains(it.value())) continue;
        seen.insert(it.value());
        out.append(it.value());
    }
    return out;
}

QString NameCatalog::describe() const
{
    return QStringLiteral("%1 localised string(s), %2 named asset(s), %3 icon(s)")
        .arg(m_strings.size())
        .arg(m_stemToLabel.size())
        .arg(m_stemToIcon.size());
}

}  // namespace fox
