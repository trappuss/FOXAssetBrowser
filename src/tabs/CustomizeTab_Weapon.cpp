// CustomizeTab_Weapon.cpp — the Weapon category of the Customize tab.
//
// Split out of CustomizeTab.cpp because it is a different shape of UI from the
// character composer: instead of a free-form part search, the weapon builder
// shows one row per customization slot, with the parts that actually exist in
// this install. Both halves share the same viewport, the same Part list and the
// same rebuildScene(), so a weapon is just a composition like any other.
//
// The slot list, its parts and the camouflage variations all come from
// fox::WeaponCatalog, which discovers them from the archives — see that header
// for the "chimera" directory layout and the connect-point mapping.
#include "tabs/CustomizeTab.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QMouseEvent>
#include <QCheckBox>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QComboBox>
#include <QFile>
#include <QTextStream>
#include <QKeyEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>

#include <algorithm>
#include <QToolButton>
#include <QPushButton>
#include <QHash>
#include <QSet>
#include <QSettings>
#include <QLabel>
#include <QMenu>
#include <QVBoxLayout>
#include <QWidget>

#include "fox/FovaFile.h"
#include "fox/FoxHash.h"
#include "gl/GLModelWidget.h"
#include "app/Config.h"
#include "index/ArchiveIndex.h"
#include "index/CharacterCatalog.h"
#include "index/EquipCatalog.h"
#include "gl/ThumbnailRenderer.h"
#include "index/IconCatalog.h"
#include "index/LayerColors.h"
#include "index/GameId.h"
#include "index/MechaCatalog.h"
#include "index/AvatarPresets.h"
#include "index/PlayerCatalog.h"
#include "index/MgoGearConfig.h"
#include "index/NameCatalog.h"
#include "index/WeaponCatalog.h"
#include "util/SearchableCombo.h"

using fox::ArchiveIndex;
using fox::WeaponCatalog;
using fox::WeaponPart;
using fox::WeaponVariation;

namespace {

// The game's own label for a slot. These are not prettified asset tokens —
// they are what the customize screen prints, in its order: Barrel, Magazine,
// Stock, Muzzle, Muzzle Accessory, Optics 1, Optics 2, Flashlight, Laser
// Sight, Underbarrel. A slot with no game name falls back to its token,
// spaced out, so an unfamiliar category still reads.
QString prettySlot(const QString& slot)
{
    static const QHash<QString, QString> kNames = {
        {QStringLiteral("receiver"), QStringLiteral("Base")},
        {QStringLiteral("barrel"), QStringLiteral("Barrel")},
        {QStringLiteral("magazine"), QStringLiteral("Magazine")},
        {QStringLiteral("stock"), QStringLiteral("Stock")},
        {QStringLiteral("muzzle"), QStringLiteral("Muzzle")},
        {QStringLiteral("muzzleOption"), QStringLiteral("Muzzle Accessory")},
        {QStringLiteral("sight"), QStringLiteral("Optics 1")},
        {QStringLiteral("sight2"), QStringLiteral("Optics 2")},
        {QStringLiteral("option"), QStringLiteral("Flashlight")},
        {QStringLiteral("option2"), QStringLiteral("Laser Sight")},
        {QStringLiteral("underBarrel"), QStringLiteral("Underbarrel")},
    };
    const auto it = kNames.constFind(slot);
    if (it != kNames.constEnd()) return it.value();
    QString out;
    for (int i = 0; i < slot.size(); ++i) {
        const QChar c = slot[i];
        if (i > 0 && c.isUpper()) out += QLatin1Char(' ');
        out += (i == 0) ? c.toUpper() : c.toLower();
    }
    return out;
}

// The headline for a part: the game's OWN display name when the name catalogue
// can resolve it ("hg07_main4_def" → "S.P CB-FRAME"), otherwise the asset stem
// tidied for reading. The exact stem is always on the second line, so a
// resolved name never hides which asset it is.
QString displayNameFor(const QString& stem)
{
    const QString real = fox::NameCatalog::instance().nameFor(stem);
    if (!real.isEmpty()) return real;
    // Buddy gear is named by the development list rather than the weapon-parts
    // table: "ddg0_main3_def" is BATTLE DRESS, "hrs3_main0_def" is FURICORN.
    const fox::EquipCatalog& equip = fox::EquipCatalog::instance();
    const QString gear = equip.gearName(stem);
    if (!gear.isEmpty()) return gear;
    // Uniforms are named by their model prefix: every part of sna5 is BATTLE
    // DRESS. Only the body model takes the name — putting "BATTLE DRESS" on a
    // face or an arm would say the wrong thing.
    const QString kind = stem.section(QLatin1Char('_'), 1, 1);
    if (kind.startsWith(QLatin1String("main")) || kind.startsWith(QLatin1String("body"))
        || kind.startsWith(QLatin1String("ply"))) {
        const QString suit = equip.suitName(stem.section(QLatin1Char('_'), 0, 0));
        if (!suit.isEmpty()) return suit;
    }
    QString out = stem;
    if (out.endsWith(QLatin1String("_def"))) out.chop(4);
    out.replace(QLatin1Char('_'), QLatin1Char(' '));
    return out;
}

// Extra item data on a TIER row of the version combo: which of the game's
// builds that tier is. -1/absent means the row is a plain asset variant.
constexpr int kPresetIdxRole = Qt::UserRole + 197;

// Where the customize screen keeps its colour chips.
#define kSwatchDir "/Assets/tpp/ui/texture/Customize/color/"

// The gear-colour palette, addressed both ways. Both walk the same two lists
// so a swatch can never be findable by hash and not by path, or the reverse.
QString layerSwatchPath(quint64 hash)
{
    const fox::LayerColorCatalog& lc = fox::LayerColorCatalog::instance();
    for (const QVector<fox::LayerSwatch>* set : {&lc.solids(), &lc.patterns()})
        for (const fox::LayerSwatch& sw : *set)
            if (sw.pathHash == hash) return sw.path;
    return {};
}

quint64 layerSwatchHash(const QString& path)
{
    const fox::LayerColorCatalog& lc = fox::LayerColorCatalog::instance();
    for (const QVector<fox::LayerSwatch>* set : {&lc.solids(), &lc.patterns()})
        for (const fox::LayerSwatch& sw : *set)
            if (sw.path == path) return sw.pathHash;
    return 0;
}

// The compatibility switch's standing explanation; the live per-weapon state
// is appended to it by refreshSlotItems().
const char* const kCompatTip =
    "Narrows every slot to the parts the game says the fitted receiver (and, "
    "for the muzzle and underbarrel slots, the fitted barrel) accepts — the "
    "same rule the customize screen uses.\n"
    "Two sources, in order: WeaponPartsCombinationSettings, and the parts the "
    "game's own shipped builds fit to this weapon. Off, every part in the "
    "install is offered. A part you have already fitted is never hidden.";

// The star row the customize screen shows against a weapon: filled pips for
// this tier's position, hollow for the ones above it, out of THIS weapon's own
// tier count. Bounded by the data — no weapon ships more than eight distinct
// grades — where the raw grade number is not (it runs to 11 and skips).
QString tierStars(int position, int total)
{
    QString out;
    for (int i = 0; i < total; ++i)
        out += (i < position) ? QChar(0x2605) : QChar(0x2606);
    return out;
}

QString subjectSubtitle(const fox::CatalogSubject& f)
{
    QString s = QStringLiteral("%1 variant%2")
                    .arg(f.variants.size())
                    .arg(f.variants.size() == 1 ? QString() : QStringLiteral("s"));
    if (f.ownPartCount > 0)
        s += QStringLiteral(", %1 own part slot%2")
                 .arg(f.ownPartCount)
                 .arg(f.ownPartCount == 1 ? QString() : QStringLiteral("s"));
    return s;
}


// The hairstyle icon set is indexed by the STYLE LETTER, not by list position.
// avf_hair_a0_v0_cov -> 0, b0 -> 1, c0 -> 2, d0 -> 3; anything with no letter
// (the bald entry) -> -1, which the icon helper maps to the last slot.
// The head model's own type digit: avf0_type6_def -> 6. The AVATAR screen's
// eye-shape tiles are numbered by that digit, NOT by where the head lands in
// the list — and the two are not the same, because the preset tables use only
// seven of the eight shipped heads (no preset picks type 5). Numbering the
// tiles by list position therefore handed type6 and type7 the art of the two
// shapes before them, and mislabelled the rows to match. Same class of bug as
// the hair one below, and it is fixed the same way: read the asset's number
// off the asset.
int headTypeIndexOf(const QString& stem)
{
    const int at = stem.indexOf(QLatin1String("_type"));
    if (at < 0) return -1;
    int v = 0, digits = 0;
    for (int i = at + 5; i < stem.size(); ++i) {
        const QChar c = stem.at(i);
        if (!c.isDigit()) break;
        v = v * 10 + (c.toLatin1() - '0');
        ++digits;
    }
    return digits > 0 ? v : -1;
}

int hairStyleIndexOf(const QString& stem)
{
    const int at = stem.indexOf(QLatin1String("_hair"));
    if (at < 0) return -1;
    for (int i = at + 5; i < stem.size(); ++i) {
        const QChar c = stem.at(i);
        if (c == QLatin1Char('_')) continue;
        if (c >= QLatin1Char('a') && c <= QLatin1Char('d'))
            return c.toLatin1() - 'a';
        return -1;
    }
    return -1;
}

}  // namespace

void CustomizeTab::buildWeaponPanel(QWidget* parent)
{
    m_weaponPanel = new QWidget(parent);
    auto* v = new QVBoxLayout(m_weaponPanel);
    v->setContentsMargins(0, 0, 0, 0);

    // Three stacked forms so the slot rows can be rebuilt wholesale (the slots
    // ARE the indexed data) without disturbing what sits above and below them.
    // Weapon first, then which version of it: starting at "receiver" means
    // picking a part id before you can pick a gun, which is backwards for
    // anyone who thinks in weapons rather than in assets.
    auto* header = new QFormLayout();
    header->setLabelAlignment(Qt::AlignRight);
    m_weaponPick = new SearchableCombo(m_weaponPanel);
    m_weaponPick->setToolTip(QStringLiteral(
        "Discovered from the game data and grouped by class. Picking one fills "
        "every slot that has a part of the same family.\n"
        "Type with the list open to search — it matches the name, the file name "
        "and the path."));
    m_weaponPickLabel = new QLabel(QStringLiteral("Weapon"), m_weaponPanel);
    header->addRow(m_weaponPickLabel, m_weaponPick);
    m_weaponVersion = new SearchableCombo(m_weaponPanel);
    m_weaponVersion->setToolTip(QStringLiteral(
        "Versions of this weapon — the receivers the game ships for it (five "
        "for hg01, four for sg02). The asset names are shown because the "
        "in-game display names live in game-logic data, not in the archives."));
    m_weaponVersionLabel = new QLabel(QStringLiteral("Version"), m_weaponPanel);
    header->addRow(m_weaponVersionLabel, m_weaponVersion);
    v->addLayout(header);

    m_weaponRowsForm = new QFormLayout();
    m_weaponRowsForm->setLabelAlignment(Qt::AlignRight);
    v->addLayout(m_weaponRowsForm);

    auto* footer = new QFormLayout();
    m_weaponFooterForm = footer;
    footer->setLabelAlignment(Qt::AlignRight);
    m_weaponCamo = new SearchableCombo(m_weaponPanel);
    m_weaponCamo->setToolTip(QStringLiteral(
        "The customize screen's Color menu, with the game's own swatch art: "
        "Camo Pattern above, Base Color below. These are .fv2 FOVA texture "
        "substitutions, not separate models, and each fitted part loads its "
        "own version of the one you pick.\n\n"
        "Character camouflage replaces the base colour map and is visible "
        "here. Vehicle paint replaces a LAYER MASK (cm_camo3_cNN_lym) that the "
        "game composites in a shader this viewport does not implement, so it "
        "resolves and applies but the model will not look different."));
    footer->addRow(QStringLiteral("Camo / Variation"), m_weaponCamo);

    m_weaponColor = new SearchableCombo(m_weaponPanel);
    m_weaponColor->setToolTip(QStringLiteral(
        "The game's own colour palette, applied to every colour-customizable "
        "material on everything fitted.\n\n"
        "Customizable gear does not ship coloured — its base map is white and "
        "its shader multiplies a separate flat swatch through a layer mask "
        "painted in the model's own UVs. Picking a colour here rebinds that "
        "one texture slot, which is exactly what the game does; 6,419 of the "
        "14,516 texture substitutions in the shipped variation tables target "
        "it.\n\n"
        "Materials whose shader does not read a layer are left alone, so a "
        "piece of gear the game will not let you colour does not change."));
    footer->addRow(QStringLiteral("Gear Color"), m_weaponColor);
    connect(m_weaponColor, &QComboBox::currentIndexChanged, this,
            [this](int) { onWeaponColorChanged(); });

    // Which games the builder draws from. Four of them can share one install
    // and their parts must never mix, so this narrows every catalogue at once.
    {
        auto* gameBar = new QHBoxLayout();
        gameBar->setContentsMargins(0, 0, 0, 0);
        static const fox::GameId kGames[] = {
            fox::GameId::Tpp, fox::GameId::Mgo, fox::GameId::GroundZeroes,
            fox::GameId::Survive,
        };
        for (fox::GameId g : kGames) {
            auto* box = new QCheckBox(QString::fromLatin1(fox::gameShortName(g)),
                                      m_weaponPanel);
            box->setToolTip(QStringLiteral(
                "%1. Off, nothing from this game is offered anywhere in the "
                "builder — its characters disappear from the list and its "
                "parts stop being fitted to anyone else's.")
                    .arg(QString::fromLatin1(fox::gameLongName(g))));
            box->setChecked(fox::GameFilter::instance().enabled(g));
            gameBar->addWidget(box);
            connect(box, &QCheckBox::toggled, this, [this, g](bool on) {
                fox::GameFilter::instance().setEnabled(g, on);
                if (m_weaponRebuilding) return;
                // Every catalogue folds the filter generation into its cache
                // key, so this is the same path a rescan takes — but a rescan
                // is not what the user asked for, so put them back on the
                // subject they were looking at if it survived the change.
                const QString was = m_weaponPick ? m_weaponPick->currentData().toString()
                                                 : QString();
                setBuilderCategory(m_builderCategory);
                if (!was.isEmpty() && m_weaponPick
                    && m_weaponPick->selectPayload(was))
                    return;   // selectPayload fires the handler, which rebuilds
                if (m_weaponInfo)
                    m_weaponInfo->setText(
                        QStringLiteral("Games: %1. The previous selection is "
                                       "not in the enabled games.")
                            .arg(fox::GameFilter::instance().describe()));
            });
        }
        gameBar->addStretch(1);
        auto* gameHost = new QWidget(m_weaponPanel);
        gameHost->setLayout(gameBar);
        footer->addRow(QStringLiteral("Games"), gameHost);
        m_gameRow = gameHost;
    }

    m_weaponCompat = new QCheckBox(
        QStringLiteral("Only parts that fit this weapon"), m_weaponPanel);
    m_weaponCompat->setToolTip(QLatin1String(kCompatTip));
    m_weaponCompat->setChecked(
        QSettings().value(QStringLiteral("weapon/compatOnly"), false).toBool());
    footer->addRow(QString(), m_weaponCompat);

    // UNLOCKED. The game refuses some combinations — a suit body clears the
    // hats and chest garments it cannot be worn with, a helmet drags its own
    // suit along — and this browser reproduces that because seeing a character
    // the way the game would build it is the point. It is also a browser, and
    // sometimes the point is the combination the game will not let you have.
    //
    // This switch turns off THE AUTOMATIC SLOT CHANGES and nothing else. Which
    // items a slot offers, which gender's models they are, which slot they go
    // in — none of that moves. Nothing here can put a woman's jacket on the
    // man; it only stops the browser taking something off that you put on.
    m_gearUnlocked = new QCheckBox(
        QStringLiteral("Unlocked — keep every slot as I set it"),
        m_weaponPanel);
    m_gearUnlocked->setToolTip(QStringLiteral(
        "Off: the game's own rules apply — equipping an item clears the ones "
        "it cannot be worn with, and equipping a piece that requires another "
        "puts that one on too.\n"
        "On: nothing is equipped or unequipped for you. Items can then overlap "
        "or clip, which the game would not allow.\n"
        "Only the automatic changes are affected: the lists, the gender and "
        "the slots are the same either way."));
    m_gearUnlocked->setChecked(
        QSettings().value(QStringLiteral("customize/gearUnlocked"), false)
            .toBool());
    footer->addRow(QString(), m_gearUnlocked);


    // A customizer with three hundred parts needs a way to start over and a way
    // to see what is in there. Both act on the slot rows only — the character
    // and its colour stay put.
    {
        auto* bar = new QHBoxLayout();
        bar->setContentsMargins(0, 0, 0, 0);
        auto* clear = new QPushButton(QStringLiteral("Clear slots"), m_weaponPanel);
        clear->setToolTip(QStringLiteral(
            "Empty every slot, keeping the chosen subject and colour."));
        auto* rand = new QPushButton(QStringLiteral("Surprise me"), m_weaponPanel);
        rand->setToolTip(QStringLiteral(
            "Fill each slot with one of the parts it actually offers — a fast "
            "way to see what a few hundred parts look like on this character. "
            "Slots the data says cannot take a part are left alone."));
        bar->addWidget(clear);
        bar->addWidget(rand);
        bar->addStretch(1);
        auto* host = new QWidget(m_weaponPanel);
        host->setLayout(bar);
        footer->addRow(QString(), host);
        connect(clear, &QPushButton::clicked, this, [this] { setAllSlots(false); });
        connect(rand, &QPushButton::clicked, this, [this] { setAllSlots(true); });
    }

    auto* presetBar = new QHBoxLayout();
    presetBar->setContentsMargins(0, 0, 0, 0);
    m_weaponPreset = new SearchableCombo(m_weaponPanel);
    m_weaponPreset->setToolTip(QStringLiteral(
        "Every weapon the game ships, at every grade, with the exact parts it "
        "is built from — read out of the development tables. Your own saved "
        "builds are listed first.\n"
        "Type with the list open to search by name, category or part."));
    m_weaponSavePreset = new QPushButton(QStringLiteral("Save…"), m_weaponPanel);
    m_weaponDeletePreset = new QPushButton(QStringLiteral("Delete"), m_weaponPanel);
    presetBar->addWidget(m_weaponPreset, 1);
    presetBar->addWidget(m_weaponSavePreset);
    presetBar->addWidget(m_weaponDeletePreset);
    auto* presetHost = new QWidget(m_weaponPanel);
    presetHost->setLayout(presetBar);
    footer->addRow(QStringLiteral("Preset"), presetHost);
    v->addLayout(footer);

    m_weaponInfo = new QLabel(m_weaponPanel);
    m_weaponInfo->setWordWrap(true);
    v->addWidget(m_weaponInfo);
    v->addStretch(1);

    connect(m_weaponCamo, &QComboBox::currentIndexChanged, this,
            [this](int) { onWeaponCamoChanged(); });
    connect(m_weaponPick, &QComboBox::currentIndexChanged, this,
            [this](int) { onWeaponPicked(); });
    connect(m_weaponVersion, &QComboBox::currentIndexChanged, this,
            [this](int) { onWeaponVersionChanged(); });
    connect(m_weaponCompat, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("weapon/compatOnly"), on);
        if (m_weaponRebuilding) return;
        refreshSlotItems();
    });
    // Remembered, and NOT retroactive: turning it on does not put back what an
    // earlier choice cleared, and turning it off does not go looking for
    // conflicts in what is already on. It governs the next choice, which is
    // the only reading that cannot surprise someone mid-build.
    connect(m_gearUnlocked, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("customize/gearUnlocked"), on);
    });
    connect(m_weaponPreset, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (i <= 0 || m_weaponRebuilding) return;
        // Payload is "g:<index into EquipCatalog::presets()>" for one of the
        // game's builds, "u:<name>" for one the user saved.
        const QString tag = m_weaponPreset->currentData().toString();
        if (tag.startsWith(QLatin1String("g:")))
            applyGamePreset(tag.mid(2).toInt());
        else if (tag.startsWith(QLatin1String("u:")))
            loadWeaponPreset(tag.mid(2));
    });
    connect(m_weaponSavePreset, &QPushButton::clicked, this,
            [this] { saveWeaponPreset(); });
    connect(m_weaponDeletePreset, &QPushButton::clicked, this,
            [this] { deleteWeaponPreset(); });
}

void CustomizeTab::refreshWeaponCatalogue()
{
    if (!m_weaponPanel || !m_weaponRowsForm) return;
    // The variant combo still holds the PREVIOUS category's selection until
    // something picks a new subject, and refreshSlotItems() reads it for the
    // receiver stem. Clear it here — at CATEGORY scope, not inside the row
    // rebuild, which a contextual category runs after filling this combo.
    if (m_weaponVersion) {
        const bool vb = m_weaponVersion->blockSignals(true);
        m_weaponVersion->clear();
        m_weaponVersion->blockSignals(vb);
    }
    // The subject list first: a contextual category's rows are a property of
    // the CHOSEN subject, and repopulating the list afterwards would drop the
    // selection those rows were built for without firing onWeaponPicked().
    refreshWeaponList();
    rebuildSlotRows();
    if (!m_source.valid() || m_source.slotNames.isEmpty()) {
        refreshWeaponPresets();
        refreshWeaponCamoList();
        m_weaponInfo->setText(m_source.emptyHint.isEmpty()
                                  ? QStringLiteral("Nothing to build in this category.")
                                  : m_source.emptyHint);
        return;
    }
    // The variations belong to the indexed data too — a folder change must not
    // leave the previous install's camo names in the list.
    refreshWeaponPresets();
    refreshWeaponCamoList();
    m_weaponInfo->setText(
        QStringLiteral("%1 slot(s) discovered from the game data. Pick a %2 to "
                       "start — everything it owns fills in automatically.")
            .arg(m_weaponRows.size())
            .arg(m_source.subjectLabel.toLower()));
}

// Just the slot rows. Separate from the whole-catalogue refresh because a
// contextual category rebuilds these on every subject change, and rebuilding
// the subject list at the same time would clear the selection that caused it.
// Does the character on this page have gear the game constrains? MGO's items
// carry Exclude and Must lists; nobody else's parts do. Asked of the PARTS
// rather than of the category, because "this page is contextual" and "these
// parts have rules" stopped being the same thing when the other TPP characters
// arrived.
// The Unlocked switch governs MGO's Exclude/Must rules, so it belongs on the
// pages whose parts carry them and nowhere else. Called on every SUBJECT
// change as well as on every category change: the category changes before a
// subject is chosen, and asking then always answered "no rules" — which is how
// the switch disappeared from the MGO avatar, the one page it is for.
void CustomizeTab::refreshGearRuleControl()
{
    if (!m_gearUnlocked) return;
    const bool show = m_source.contextual() && subjectHasGearRules();
    m_gearUnlocked->setVisible(show);
    if (QWidget* lb = m_weaponFooterForm
                          ? m_weaponFooterForm->labelForField(m_gearUnlocked)
                          : nullptr)
        lb->setVisible(show);
}

bool CustomizeTab::subjectHasGearRules() const
{
    if (!m_source.contextual() || !m_source.partsForSubject) return false;
    const QString subject = currentSubjectId();
    if (subject.isEmpty()) return false;
    // NOT named `slots`: Qt defines that as a keyword, and the error it
    // produces ("expected unqualified-id before '=' token") names neither Qt
    // nor the macro.
    const QStringList slotIds = m_source.slotsForSubject
        ? m_source.slotsForSubject(subject)
        : QStringList();
    for (const QString& slot : slotIds)
        for (const fox::CatalogPart& p : m_source.partsForSubject(subject, slot))
            if (!p.gearId.isEmpty()) return true;
    return false;
}

void CustomizeTab::rebuildSlotRows()
{
    if (!m_weaponPanel || !m_weaponRowsForm) return;
    // Rebuild the slot rows from scratch: which slots exist is a property of
    // the indexed data, and a rescan can change it.
    for (const WeaponSlotRow& row : m_weaponRows)
        if (row.combo) { m_weaponRowsForm->removeRow(row.combo); }
    m_weaponRows.clear();
    for (const GearColourRow& row : m_gearColourRows)
        if (row.combo) { m_weaponRowsForm->removeRow(row.combo); }
    m_gearColourRows.clear();
    // Colour choices belong to the subject whose items they were made for.
    m_mgoColours.clear();

    if (!m_source.valid() || m_source.slotNames.isEmpty()) return;

    const QString hostSlot = m_source.slotNames.value(0);
    // The slots to show. A contextual category (the player characters) answers
    // per subject — Snake has three, a Survive survivor has eight, a Ground
    // Zeroes model has none — so the rows are rebuilt whenever the subject
    // changes rather than once for the whole category.
    QStringList slotList;
    if (m_source.contextual()) {
        slotList = m_source.slotsForSubject(currentSubjectId());
    } else {
        // The catalogue's own, plus a SECOND sight and option row where the
        // game's shipped builds actually use one (a scope with a backup iron
        // sight, a flashlight with a laser). Both draw from the same catalogue
        // directory — see EquipCatalog::baseSlot.
        for (const QString& slot : m_source.slotNames) {
            if (slot == hostSlot) continue;   // chosen by the Version combo above
            slotList << slot;
            const QString second = slot + QLatin1Char('2');
            if (fox::EquipCatalog::baseSlot(second) == slot
                && fox::EquipCatalog::instance().slotIsUsed(second))
                slotList << second;
        }
    }

    for (const QString& slot : slotList) {
        if (!m_source.contextual()
            && m_source.partsFor(fox::EquipCatalog::baseSlot(slot)).isEmpty())
            continue;
        auto* combo = new SearchableCombo(m_weaponPanel);
        // A contextual category names its own slots ("Upper Armor", "Chest
        // Rig"); the weapon vocabulary would call them nothing.
        QString label = prettySlot(slot);
        if (m_source.slotLabelFor) {
            const QString own = m_source.slotLabelFor(currentSubjectId(), slot);
            if (!own.isEmpty()) label = own;
        }
        m_weaponRowsForm->addRow(label, combo);
        WeaponSlotRow row;
        row.slot = slot;
        row.combo = combo;
        m_weaponRows.append(row);
        const int myRow = m_weaponRows.size() - 1;
        connect(combo, &QComboBox::currentIndexChanged, this,
                [this, myRow](int) { onWeaponSlotChanged(myRow); });
        // Right-click the slot itself. The equipped list has offered a menu
        // for a long time and this row — the one the user actually chose the
        // item in — offered none, so exporting one piece of a build meant
        // finding it again in a second list. Same builder, so the two cannot
        // drift apart.
        combo->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(combo, &QWidget::customContextMenuRequested, this,
                [this, myRow, label](const QPoint& pos) {
                    if (myRow < 0 || myRow >= m_weaponRows.size()) return;
                    QWidget* w = m_weaponRows[myRow].combo;
                    if (!w) return;
                    QMenu menu(this);
                    addSlotMenuActions(&menu, m_weaponRows[myRow].partIdx,
                                       label);
                    menu.exec(w->mapToGlobal(pos));
                });
        // An MGO gear slot dyes its item in two channels, from that item's
        // own palette — so each one gets two colour rows directly beneath
        // it, hidden until the slot holds an item with a palette. See
        // fillGearColourRows().
        if (slot.startsWith(QLatin1String("mgo_"))) {
            // FOUR rows, not two. A two-piece garment brings a second model
            // with a palette of its own, and the game dyes the two halves
            // separately — which is why one of them ends up with two dye rows
            // and another with three. Every row is hidden until something with
            // a palette is actually in the slot, so a plain item still shows
            // exactly the two it always did.
            for (int r = 0; r < 4; ++r) {
                const bool companion = r >= 2;
                const int ch = r % 2;
                auto* cc = new SearchableCombo(m_weaponPanel);
                m_weaponRowsForm->addRow(
                    companion
                        ? QStringLiteral("%1 Vest Color %2").arg(label).arg(ch + 1)
                        : QStringLiteral("%1 Color %2").arg(label).arg(ch + 1),
                    cc);
                cc->setVisible(false);
                if (QWidget* lb = m_weaponRowsForm->labelForField(cc))
                    lb->setVisible(false);
                GearColourRow crow;
                crow.slot = slot;
                crow.channel = ch;
                crow.companion = companion;
                crow.combo = cc;
                m_gearColourRows.append(crow);
                const int cIdx = m_gearColourRows.size() - 1;
                connect(cc, &QComboBox::currentIndexChanged, this,
                        [this, cIdx](int) { onGearColourChanged(cIdx); });
            }
        }
    }
    buildLookRows();
    refreshSlotItems();
    fillLookRows();
}

namespace {
// The appearance rows, in the order the AVATAR screen puts them. Every one of
// these is a TEXTURE set rather than a model — the game ships a separate map
// for each skin tone, each wrinkle set, each eyebrow, each hair colour and each
// scar — except the eye shape, which is the head model itself.
struct LookRowDef { const char* slot; const char* label; };
const LookRowDef kLookRows[] = {
    {"look:eye",     "Eye Shape"},
    {"look:eyecolR", "Right Eye Color"},
    {"look:eyecolL", "Left Eye Color"},
    {"look:skin",    "Skin Color"},
    {"look:wrinkle", "Wrinkles Type"},
    {"look:brow",    "Eyebrow Style"},
    {"look:haircol", "Hair Color"},
    {"look:beard",   "Facial Hair"},
    {"look:deco",    "Feature"},
};
}  // namespace

void CustomizeTab::buildLookRows()
{
    if (!m_weaponPanel || !m_weaponRowsForm) return;
    // Either table is enough: a page showing the men's grid must get the
    // appearance rows even on an install whose women's table is missing.
    const fox::AvatarPresets& ap = fox::AvatarPresets::instance();
    if ((!ap.ok(fox::AvatarPresets::Sex::Women)
         && !ap.ok(fox::AvatarPresets::Sex::Men))
        || !fox::AvatarTextures::instance().ok())
        return;
    // Only a subject that actually has an avatar face has an appearance to set,
    // and "has a head slot" is not that test. A TPP soldier's slot for helmets
    // is also called "head", so every Diamond Dogs page was growing a full set
    // of inert appearance rows — Eye Shape, Skin Color, Wrinkles Type and the
    // rest — with nothing behind them to change. What separates the two is that
    // an avatar head slot is filled with the game's PRESETS: its parts carry a
    // preset index, a helmet's do not.
    bool hasFacePresets = false;
    if (m_source.partsForSubject) {
        for (const fox::CatalogPart& p : m_source.partsForSubject(
                 currentSubjectId(), QStringLiteral("head")))
            if (p.presetIndex >= 0) { hasFacePresets = true; break; }
    }
    if (!hasFacePresets) return;

    // The Eye Shape row's icons are RENDERS, and asking for the first one from
    // inside a paint event would build the GL context, the offscreen surface
    // and the render thread right there. Build them now, while nothing is
    // painting; the call is idempotent.
    fox::ThumbnailRenderer::instance().prewarm();

    for (const LookRowDef& d : kLookRows) {
        auto* combo = new SearchableCombo(m_weaponPanel);
        m_weaponRowsForm->addRow(QString::fromLatin1(d.label), combo);
        WeaponSlotRow row;
        row.slot = QString::fromLatin1(d.slot);
        row.combo = combo;
        row.isLook = true;
        m_weaponRows.append(row);
        const int myRow = m_weaponRows.size() - 1;
        connect(combo, &QComboBox::currentIndexChanged, this,
                [this, myRow](int) { onWeaponSlotChanged(myRow); });
    }
}

// Fill the appearance rows from what this install actually ships. Row 0 is
// always "from the face preset", so the preset stays in charge until something
// is chosen deliberately.
void CustomizeTab::fillLookRows()
{
    const fox::AvatarTextures& at = fox::AvatarTextures::instance();
    if (!at.ok()) return;
    const auto& files = ArchiveIndex::instance().files();
    // Every icon set ships twice, once per gender. Which one this page draws is
    // the SUBJECT's, decided once here rather than per row.
    const fox::AvatarPresets::Sex sex = lookSex();

    // Eye shape: the head models themselves, avf0_type0…7.
    QMap<QString, int> heads;
    if (m_source.partsForSubject)
        for (const fox::CatalogPart& p : m_source.partsForSubject(
                 currentSubjectId(), QStringLiteral("head")))
            heads.insert(p.id, p.modelFileIdx);

    for (WeaponSlotRow& r : m_weaponRows) {
        if (!r.isLook || !r.combo) continue;
        const bool b = r.combo->blockSignals(true);
        const QVariant keep = r.combo->currentIndex() > 0
            ? r.combo->currentData() : QVariant();
        r.combo->clear();
        r.combo->addPlainItem(QStringLiteral("— from face preset —"), -1);

        if (r.slot == QLatin1String("look:eye")) {
            int n = 0;
            for (auto it = heads.constBegin(); it != heads.constEnd(); ++it, ++n) {
                const QString path = (it.value() >= 0 && it.value() < files.size())
                    ? files[it.value()].path : QString();
                // The game ships a tile per eye shape — a close crop of that
                // eye — under Avatar_mgo/eye. It is not in the name dictionary,
                // which is why this used to fall back to a render of the whole
                // head: correct, but a grey thumbnail of a head is not what
                // "eye shape" needs to show. The render stays as the fallback
                // for an install without the tiles.
                const int shape = headTypeIndexOf(it.key());
                const QString tile =
                    fox::AvatarPresets::eyeIconPath(shape, sex);
                const bool haveTile =
                    !tile.isEmpty()
                    && !fox::IconCatalog::instance().swatchForPath(tile, 16).isNull();
                r.combo->addSwatchItem(
                    QStringLiteral("Eye Shape %1")
                        .arg(shape >= 0 ? shape + 1 : n + 1),
                    it.key(), path, n,
                    haveTile ? tile
                             : (it.value() >= 0
                                    ? QStringLiteral("model:%1").arg(it.value())
                                    : QString()));
            }
        } else if (r.slot == QLatin1String("look:eyecolR")
                   || r.slot == QLatin1String("look:eyecolL")) {
            // Eight iris colours, each in a bright and a dark print. The game
            // gives the two eyes their own row and really does allow them to
            // differ, so this is two rows and not one — see AvatarPreset.
            const bool any = !at.irisColours().isEmpty();
            r.combo->setVisible(any);
            if (m_weaponRowsForm)
                if (QWidget* lb = m_weaponRowsForm->labelForField(r.combo))
                    lb->setVisible(any);
            if (any)
                for (int ci = 0; ci < at.irisColours().size(); ++ci) {
                    const int colour = at.irisColours()[ci];
                    const QString p = at.irisPath(colour,
                                                  at.irisShades().value(0, 0));
                    const QString name = QString::fromLatin1(
                        fox::AvatarTextures::irisName(colour));
                    // The game's own tile if it ships one; otherwise the iris
                    // map itself, which IS a picture of the iris and needs no
                    // crop — unlike a face atlas.
                    const QString tile =
                        fox::AvatarPresets::eyeColourIconPath(colour);
                    const bool haveTile =
                        !tile.isEmpty()
                        && !fox::IconCatalog::instance()
                                .swatchForPath(tile, 16)
                                .isNull();
                    r.combo->addSwatchItem(
                        name.isEmpty()
                            ? QStringLiteral("Eye Colour %1").arg(colour + 1)
                            : name,
                        p.section(QLatin1Char('/'), -1), p, colour,
                        haveTile ? tile : p);
                }
        } else if (r.slot == QLatin1String("look:skin")) {
            // Preview THIS subject's own face grid — a male subject showing the
            // women's skin chips would be quietly misleading.
            const QString stem = lookHeadStem();
            for (int i = 0; i < at.skins().size(); ++i) {
                const QString p = at.facePathFor(stem, at.wrinkles().value(0, 0),
                                                 at.skins()[i]);
                // The cheek, in the face map's own UV space (the layout is a
                // flat unwrapped face: brow band around y 0.22-0.32, eyes 0.36,
                // cheeks 0.50-0.60, mouth 0.62). Averaged into a flat chip —
                // the point of a tone row is the tone, and handing the row the
                // whole 512x512 UV map shows a smear of ear and neck instead.
                r.combo->addSwatchItem(
                    QStringLiteral("Skin %1").arg(i + 1),
                    QStringLiteral("tone %1").arg(at.skins()[i]), p, i,
                    fox::IconCatalog::avgSpec(p, 0.28, 0.50, 0.10, 0.10));
            }
        } else if (r.slot == QLatin1String("look:wrinkle")) {
            const QString stem = lookHeadStem();
            for (int i = 0; i < at.wrinkles().size(); ++i) {
                const QString p = at.facePathFor(stem, at.wrinkles()[i],
                                                 at.skins().value(0, 0));
                // Brow and eye band — measured across all nine sets as the
                // region that actually differs between them (set 3 gains crow's
                // feet, set 9 is a different age entirely).
                // The game's own tile first — eight of them ship, one per
                // wrinkle set. Beyond those, a crop of the brow-and-eye band of
                // this set's own face map, which is the region that actually
                // differs; and if even that cannot be built, the texture
                // itself, because a raw UV map is a poor icon but an empty cell
                // is worse.
                const QString tile = fox::AvatarPresets::wrinkleIconPath(i, sex);
                const QString spec =
                    fox::IconCatalog::cropSpec(p, 0.32, 0.26, 0.36, 0.36);
                fox::IconCatalog& ic = fox::IconCatalog::instance();
                QString icon;
                if (!tile.isEmpty() && !ic.swatchForPath(tile, 16).isNull())
                    icon = tile;
                else if (!spec.isEmpty() && !ic.swatchForPath(spec, 16).isNull())
                    icon = spec;
                else
                    icon = p;
                r.combo->addSwatchItem(
                    QStringLiteral("Wrinkles %1").arg(i + 1),
                    QStringLiteral("set %1").arg(at.wrinkles()[i]), p, i, icon);
            }
        } else if (r.slot == QLatin1String("look:brow")) {
            for (int i = 0; i < at.browShapes().size(); ++i)
                r.combo->addSwatchItem(
                    QStringLiteral("Eyebrow %1").arg(i + 1), at.browShapes()[i],
                    at.browPath(i, 0), i,
                    fox::AvatarPresets::browIconPath(i, sex));
        } else if (r.slot == QLatin1String("look:beard")) {
            // Men only — and "men" means THIS SUBJECT, not "this install ships
            // beard textures". The female survivor was being offered a beard purely
            // because the archives carry the male set.
            const bool any = !at.beardShapes().isEmpty()
                && lookSex() == fox::AvatarPresets::Sex::Men;
            r.combo->setVisible(any);
            if (m_weaponRowsForm)
                if (QWidget* lb = m_weaponRowsForm->labelForField(r.combo))
                    lb->setVisible(any);
            if (any) {
                fox::IconCatalog& ic = fox::IconCatalog::instance();
                const QString none = fox::AvatarPresets::beardIconPath(-1);
                r.combo->addSwatchItem(
                    QStringLiteral("— clean-shaven —"),
                    QStringLiteral("no facial hair"), QString(), 1000,
                    (!none.isEmpty() && !ic.swatchForPath(none, 16).isNull())
                        ? none : QString());
                for (int i = 0; i < at.beardShapes().size(); ++i) {
                    const QString p = at.beardPath(i, 0);
                    // The shipped stem is <family letter><density digit>, and
                    // the game ships one tile per FAMILY — so a2 and a0 share
                    // the full-beard tile and differ only in the row's name.
                    const QString shape = at.beardShapes()[i];
                    // -1 would mean the CLEAN-SHAVEN tile, which is the one
                    // thing a beard row must never show, so an unreadable
                    // shape falls through to the swatch instead.
                    const int fam = shape.isEmpty()
                        ? -2 : shape.at(0).toLatin1() - 'a';
                    const QString tile = fox::AvatarPresets::beardIconPath(fam);
                    const bool haveTile =
                        !tile.isEmpty() && !ic.swatchForPath(tile, 16).isNull();
                    // Without a tile, take the SKIN layer rather than the mesh
                    // one: berd1 is the beard painted in the face's own UV
                    // space, so it is a picture of the actual beard, where the
                    // mesh atlas (berd0) is rows of loose hair cards.
                    //
                    // alphaSpec, NOT a crop. Both maps are hair on
                    // transparency, and only alphaSpec trims to the content and
                    // backs it with skin; cropping a fixed rectangle out of an
                    // alpha-keyed map gives stubble floating on nothing, and
                    // would look different again from the shapes that fall
                    // through to the mesh atlas.
                    const QString skin = at.beardSkinPath(i, 0);
                    const QString fallback =
                        fox::IconCatalog::alphaSpec(skin.isEmpty() ? p : skin);
                    r.combo->addSwatchItem(
                        QStringLiteral("Facial Hair %1").arg(shape.toUpper()), shape,
                        p, i, haveTile ? tile : fallback);
                }
            }
        } else if (r.slot == QLatin1String("look:haircol")) {
            static const char* const kColourNames[] = {
                "Blonde", "Brown", "Black", "White", "Red"};
            for (int i = 0; i < at.hairColours().size(); ++i) {
                // Prefer a HAIR map over a brow map: both carry the colour, but
                // the hair atlas has large solid areas and the brow one is a
                // few thin strokes on transparency.
                const QString hp = at.anyHairPath(i);
                const QString src = hp.isEmpty() ? at.browPath(0, i) : hp;
                r.combo->addSwatchItem(
                    i < 5 ? QString::fromLatin1(kColourNames[i])
                          : QStringLiteral("Hair Colour %1").arg(i + 1),
                    at.hairColours()[i], src, i,
                    hp.isEmpty()
                        ? fox::IconCatalog::alphaSpec(src)
                        // The top-left card of the atlas is the solid one.
                        : fox::IconCatalog::cropSpec(src, 0.02, 0.02, 0.22, 0.22));
            }
        } else if (r.slot == QLatin1String("look:deco")) {
            r.combo->addSwatchItem(QStringLiteral("— no feature —"),
                                   QStringLiteral("bare face"), QString(), 1000,
                                   fox::AvatarPresets::decoIconPath(-1, -1, sex));
            for (int fam = 0; fam < 3; ++fam)
                for (int id : at.decoIds(fam)) {
                    const QString p = at.decoPath(fam, id, at.skins().value(0, 0));
                    r.combo->addSwatchItem(
                        QStringLiteral("%1 %2")
                            .arg(QString::fromLatin1(
                                     fox::AvatarTextures::decoName(fam)).toUpper())
                            .arg(id + 1),
                        p.section(QLatin1Char('/'), -1), p, fam * 100 + id,
                        fox::AvatarPresets::decoIconPath(fam, id, sex));
                }
        }
        if (keep.isValid()) r.combo->selectPayload(keep);
        r.combo->blockSignals(b);
    }
}

QString CustomizeTab::lookHeadStem() const
{
    for (const WeaponSlotRow& r : m_weaponRows) {
        if (r.slot != QLatin1String("head") || !r.combo
            || r.combo->currentIndex() < 0)
            continue;
        const QString p = r.combo->itemData(r.combo->currentIndex(),
                                            richcombo::PathRole).toString();
        if (!p.isEmpty())
            return p.section(QLatin1Char('/'), -1).section(QLatin1Char('.'), 0, 0);
    }
    return {};
}

fox::AvatarPresets::Sex CustomizeTab::lookSex() const
{
    // The SUBJECT decides, not the head model: a male survivor with no head
    // model in this install still has no business being offered the women's
    // preset table, and the head stem is empty in exactly that case.
    const QString id = currentSubjectId();
    if (id.endsWith(QLatin1String("_m"))) return fox::AvatarPresets::Sex::Men;
    if (id.endsWith(QLatin1String("_f"))) return fox::AvatarPresets::Sex::Women;
    return fox::AvatarPresets::sexOfStem(lookHeadStem());
}

QString CustomizeTab::selectedHairStem() const
{
    // The HAIR row's model stem, for the head's hairline layer. Read off the
    // row rather than off m_parts: the head is textured before the hair part
    // is added, so asking the scene which hair is fitted would come back empty
    // on the pass that matters.
    for (const WeaponSlotRow& r : m_weaponRows) {
        if (r.slot != QLatin1String("hair") || !r.combo) continue;
        if (r.combo->currentIndex() <= 0) return {};
        const int fi = r.combo->currentData().toInt();
        const auto& files = fox::ArchiveIndex::instance().files();
        if (fi < 0 || fi >= files.size()) return {};
        return files[fi].path.section(QLatin1Char('/'), -1)
            .section(QLatin1Char('.'), 0, 0);
    }
    return {};
}

int CustomizeTab::lookEyeShapeFile() const
{
    for (const WeaponSlotRow& r : m_weaponRows) {
        if (r.slot != QLatin1String("look:eye") || !r.combo) continue;
        if (r.combo->currentIndex() <= 0) return -1;
        const QString stem = r.combo->itemData(r.combo->currentIndex(),
                                               richcombo::FileRole).toString();
        if (!m_source.partsForSubject) return -1;
        for (const fox::CatalogPart& p : m_source.partsForSubject(
                 currentSubjectId(), QStringLiteral("head")))
            if (p.id == stem) return p.modelFileIdx;
        return -1;
    }
    return -1;
}

// Empty every slot, or fill each one from what it offers. One rebuild at the
// end rather than one per slot: the scene is reassembled from scratch each
// time, and doing that eleven times over would be visible.
void CustomizeTab::setAllSlots(bool random)
{
    if (m_weaponRows.isEmpty()) return;
    m_weaponRebuilding = true;
    for (WeaponSlotRow& r : m_weaponRows) {
        if (!r.combo || r.unusable) continue;   // a crossed-out slot stays empty
        if (r.isLook && !random) { r.combo->setCurrentIndex(0); continue; }
        const bool b = r.combo->blockSignals(true);
        int want = 0;
        if (random && r.combo->count() > 1) {
            // Index 0 is "— none —"; leaving it in the draw means a build that
            // is not simply every slot filled.
            want = int(QRandomGenerator::global()->bounded(r.combo->count()));
        }
        r.combo->setCurrentIndex(want);
        r.combo->blockSignals(b);
    }
    // The barrel decides what fits below it, so narrow once with the new
    // selection in place, then again is unnecessary — refreshSlotItems keeps
    // whatever is fitted.
    refreshSlotItems();
    m_weaponRebuilding = false;
    for (WeaponSlotRow& r : m_weaponRows)
        if (r.combo) r.combo->refreshCurrentIcon();
    refreshWeaponCamoList();
    rebuildWeapon();
}

void CustomizeTab::onWeaponSlotChanged(int row)
{
    if (row >= 0 && row < m_weaponRows.size() && m_weaponRows[row].combo
        && !m_weaponRows[row].unusable)
        m_weaponRows[row].combo->refreshCurrentIcon();
    if (m_weaponRebuilding) return;
    // Variations are authored per model, so the camo list belongs to whichever
    // receiver is selected — refresh it before rebuilding, or the previous
    // weapon's camo would be carried onto the new one and silently not match.
    if (row >= 0 && row < m_weaponRows.size()
        && m_weaponRows[row].slot == m_source.slotNames.value(0))
        refreshWeaponCamoList();
    // In a contextual category the list is the UNION over everything fitted,
    // so fitting or clearing any part can add or remove entries — keeping the
    // chosen one selected where it survives.
    else if (m_source.contextual()) {
        const QString keep = m_weaponCamo ? m_weaponCamo->currentData().toString()
                                          : QString();
        refreshWeaponCamoList();
        if (!keep.isEmpty() && m_weaponCamo) {
            const bool b = m_weaponCamo->blockSignals(true);
            m_weaponCamo->selectPayload(keep);
            m_weaponCamo->blockSignals(b);
        }
    }
    // The muzzle slots are keyed on the BARREL, so changing the barrel changes
    // what fits below it.
    if (row >= 0 && row < m_weaponRows.size()
        && m_weaponRows[row].slot == QLatin1String("barrel"))
        refreshSlotItems();
    // Picking a Basic Face Shape in the game sets the whole preset, hair
    // included — the grid cell is a character, not a head. So does this.
    if (row >= 0 && row < m_weaponRows.size()
        && m_weaponRows[row].slot == QLatin1String("head"))
        applyFacePresetSideEffects(row, true);
    // The game's Exclude rule, after the choice and before the one rebuild.
    if (row >= 0 && row < m_weaponRows.size() && m_source.contextual())
        applyGearExcludes(row);
    // …then the colour rows, which follow both the choice and any clearing
    // the Exclude rule just did. Changing a slot's ITEM discards that slot's
    // dye — the old colour was a choice about a different garment. Done here,
    // on the interactive path only: a preset restore sets its own colours and
    // must not have them wiped by the very selections it makes.
    if (row >= 0 && row < m_weaponRows.size()
        && m_weaponRows[row].slot.startsWith(QLatin1String("mgo_"))) {
        m_mgoColours.remove(m_weaponRows[row].slot);
        m_mgoColours.remove(companionColourKey(m_weaponRows[row].slot));
        fillGearColourRows();
    }
    rebuildWeapon();
}

bool CustomizeTab::headgearWorn() const
{
    for (const WeaponSlotRow& r : m_weaponRows) {
        if (r.isLook || !r.combo || r.combo->currentIndex() < 0) continue;
        // "mgo_headgear" for MGO, "head_equipment"/"hats" for the others —
        // whatever the subject calls the slot a hat goes in. Accessory is
        // deliberately NOT here: glasses do not flatten hair.
        if (r.slot != QLatin1String("mgo_headgear")
            && r.slot != QLatin1String("head_equipment")
            && r.slot != QLatin1String("hats"))
            continue;
        if (r.combo->currentData().toInt() >= 0) return true;
    }
    return false;
}

int CustomizeTab::coveredHairFor(int fileIdx) const
{
    if (fileIdx < 0 || !m_source.partsForSubject) return -1;
    for (const fox::CatalogPart& p :
         m_source.partsForSubject(currentSubjectId(), QStringLiteral("hair")))
        if (p.modelFileIdx == fileIdx) return p.coveredFileIdx;
    return -1;
}

void CustomizeTab::setGearUnlocked(bool on)
{
    if (!m_gearUnlocked) return;
    // FOR THIS RUN ONLY. The toggled handler writes the box to QSettings, so a
    // single "--unlocked 1" screenshot would otherwise leave the switch ticked
    // in the GUI for good — the same rule the export flags in main.cpp follow.
    const bool b = m_gearUnlocked->blockSignals(true);
    m_gearUnlocked->setChecked(on);
    m_gearUnlocked->blockSignals(b);
}

bool CustomizeTab::gearRulesLocked() const
{
    return !m_gearUnlocked || !m_gearUnlocked->isChecked();
}

void CustomizeTab::applyGearExcludes(int changedRow)
{
    if (changedRow < 0 || changedRow >= m_weaponRows.size()) return;
    const WeaponSlotRow& changed = m_weaponRows[changedRow];
    if (!changed.combo || changed.isLook || !m_source.partsForSubject) return;

    // The CatalogPart behind a row's current selection, BY VALUE — an empty
    // gearId means "no gear item here". Resolved through the catalogue by
    // model file index; within one slot the catalogue lists a model once, so
    // the index identifies the item.
    const auto gearPartOf =
        [this](const WeaponSlotRow& r) -> fox::CatalogPart {
        if (!r.combo || r.isLook || r.combo->currentIndex() < 0) return {};
        const int fileIdx = r.combo->currentData().toInt();
        if (fileIdx < 0) return {};
        for (const fox::CatalogPart& p :
             m_source.partsForSubject(currentSubjectId(), r.slot))
            if (p.modelFileIdx == fileIdx && !p.gearId.isEmpty()) return p;
        return {};
    };
    const fox::CatalogPart chosen = gearPartOf(changed);
    if (chosen.gearId.isEmpty()) return;
    // UNLOCKED: describe, do not act. The rule is still real and is still
    // worth saying out loud — the overlap that follows is the game's own
    // reason for the rule — but nothing is equipped or unequipped.
    if (!gearRulesLocked()) {
        QStringList would;
        for (int i = 0; i < m_weaponRows.size(); ++i) {
            if (i == changedRow) continue;
            const fox::CatalogPart worn = gearPartOf(m_weaponRows[i]);
            if (worn.gearId.isEmpty()) continue;
            if (chosen.gearExclude.contains(worn.gearId)
                || worn.gearExclude.contains(chosen.gearId))
                would << worn.gearId;
        }
        if (!would.isEmpty())
            qInfo("customize: unlocked — %s would normally clear %s; left on, "
                  "so the two will overlap",
                  qUtf8Printable(chosen.gearId),
                  qUtf8Printable(would.join(QLatin1String(", "))));
        return;
    }

    QVector<int> mustEquipped;
    // ── Must: what this item cannot be worn WITHOUT ──────────────────────
    // One-directional and rare: the four sneaking-suit helmets name their own
    // suit body (ins_?01 → ins_?00) and the game will not show one without
    // the other. Applied BEFORE Exclude, because equipping the required body
    // is itself a slot change the Exclude pass then has to see.
    if (!chosen.gearMust.isEmpty())
        for (int i = 0; i < m_weaponRows.size(); ++i) {
            if (i == changedRow) continue;
            WeaponSlotRow& other = m_weaponRows[i];
            if (!other.combo || other.isLook) continue;
            const fox::CatalogPart worn = gearPartOf(other);
            if (!worn.gearId.isEmpty()
                && chosen.gearMust.contains(worn.gearId))
                continue;   // already wearing what is required
            // Is the required item one this slot offers? If not, this row is
            // not the one the rule is about.
            int wantFile = -1;
            QString wantId;
            for (const fox::CatalogPart& p :
                 m_source.partsForSubject(currentSubjectId(), other.slot))
                if (chosen.gearMust.contains(p.gearId)) {
                    wantFile = p.modelFileIdx;
                    wantId = p.gearId;
                    break;
                }
            if (wantFile < 0) continue;
            const bool b = other.combo->blockSignals(true);
            const bool ok = other.combo->selectPayload(wantFile);
            other.combo->refreshCurrentIcon();
            other.combo->blockSignals(b);
            if (ok) {
                // The dye follows the garment, exactly as it does when the
                // Exclude rule clears a slot.
                m_mgoColours.remove(other.slot);
                m_mgoColours.remove(companionColourKey(other.slot));
                qInfo("customize: %s cannot be worn without %s — equipped it "
                      "(the game's Must rule)",
                      qUtf8Printable(chosen.gearId), qUtf8Printable(wantId));
                mustEquipped << i;
            }
            // ONE row. The required item is a single garment; without this the
            // loop equips it into every row that happens to offer it.
            break;
        }
    // …and the item Must just put on has rules of its own. Signals are blocked
    // above (they have to be, or the row handler re-enters this function and
    // rebuilds mid-pass), so the cascade is applied here explicitly. Without
    // it, equipping a suit helmet dragged its suit body on and left the chest
    // garment that body excludes still worn — the exact state the rule exists
    // to prevent, produced by the rule itself.
    if (!m_applyingGearRules) {
        m_applyingGearRules = true;
        for (const int r : mustEquipped) applyGearExcludes(r);
        m_applyingGearRules = false;
    }

    const auto conflicts = [&chosen](const fox::CatalogPart& p) {
        // BOTH directions — the shipped lists are one-sided (inc_m00 names
        // cms_m01 while cms_m01 names no inc id), and either one means the
        // pair cannot be worn.
        return chosen.gearExclude.contains(p.gearId)
            || p.gearExclude.contains(chosen.gearId);
    };

    QStringList cleared;
    for (int i = 0; i < m_weaponRows.size(); ++i) {
        if (i == changedRow) continue;
        WeaponSlotRow& other = m_weaponRows[i];
        if (!other.combo || other.isLook) continue;
        const fox::CatalogPart worn = gearPartOf(other);
        if (worn.gearId.isEmpty() || !conflicts(worn)) continue;
        // Index 0 is "none" for Headgear, Chest and Accessory, but for Base
        // it is the DEFAULT part (the BDU) — and the BDU itself excludes the
        // cmn chest piece, so "clearing" Base to it can re-create a conflict
        // rather than resolve one. When row 0 is itself a conflicting gear
        // part, the row is left as chosen and the clash is logged: the
        // honest state is a visible overlap the next choice resolves, not
        // two rules fighting over one combo.
        bool zeroConflicts = false;
        {
            const QVariant zeroData = other.combo->itemData(0);
            const int zeroIdx = zeroData.isValid() ? zeroData.toInt() : -1;
            if (zeroIdx >= 0)
                for (const fox::CatalogPart& p :
                     m_source.partsForSubject(currentSubjectId(), other.slot))
                    if (p.modelFileIdx == zeroIdx && !p.gearId.isEmpty()
                        && conflicts(p)) {
                        zeroConflicts = true;
                        break;
                    }
        }
        if (zeroConflicts) {
            // Logged in BOTH shapes of this state — a worn part that cannot
            // be displaced, and a default that would re-create the clash —
            // because the visible overlap it leaves is exactly the thing the
            // log has to explain.
            qInfo("customize: %s conflicts with %s, and that slot's default "
                  "conflicts too — left as is, the two will overlap",
                  qUtf8Printable(chosen.gearId), qUtf8Printable(worn.gearId));
            continue;
        }
        const bool b = other.combo->blockSignals(true);
        other.combo->setCurrentIndex(0);
        other.combo->refreshCurrentIcon();
        other.combo->blockSignals(b);
        // The cleared slot's dye goes with its garment — the same
        // "changing the item discards the dye" rule the interactive path
        // applies, which this clearing would otherwise bypass (its signals
        // are blocked). Leaving it made a saved preset carry a colour for an
        // unworn slot, and on Base — whose row 0 is the BDU, a real garment
        // — it painted the PREVIOUS garment's dye onto the BDU.
        m_mgoColours.remove(other.slot);
        m_mgoColours.remove(companionColourKey(other.slot));
        cleared << worn.gearId;
    }
    if (!cleared.isEmpty())
        qInfo("customize: equipping %s cleared %s (the game's Exclude rule)",
              qUtf8Printable(chosen.gearId),
              qUtf8Printable(cleared.join(QLatin1String(", "))));
}

namespace {
// The GearConfig record behind a gear id, for the palette and defaults the
// colour rows need. The subject's own gender first — the female table's
// record can differ (her _m-id hats) — but ids are unique per table, so the
// other gender is a safe fallback for lookups made without a subject.
const fox::MgoGearItem* mgoItemById(const QString& id, bool female)
{
    const fox::MgoGearConfig& gc = fox::MgoGearConfig::instance();
    for (int pass = 0; pass < 2; ++pass) {
        const bool fem = pass == 0 ? female : !female;
        for (const fox::MgoGearCategory& cat : gc.categories(fem))
            for (const fox::MgoGearItem& item : cat.items)
                if (item.id == id) return &item;
    }
    return nullptr;
}
}  // namespace

// Show, hide and fill the per-item colour rows from what the gear slots
// currently hold. MGO dyes each item separately: the rows under a slot list
// THAT item's own Primary/Secondary palette from GearConfig.lua, row 0 being
// the item's default. A slot whose item changed loses its stored choice —
// the old colour was a choice about a different garment.
void CustomizeTab::fillGearColourRows()
{
    if (m_gearColourRows.isEmpty()) return;
    const bool female = lookSex() == fox::AvatarPresets::Sex::Women;
    const fox::MgoGearConfig& gc = fox::MgoGearConfig::instance();

    for (GearColourRow& row : m_gearColourRows) {
        if (!row.combo) continue;
        // The item this slot currently holds, by gear id — or, for a
        // companion row, the id of that item's SECOND half. A slot holding a
        // plain item has no second half, so its companion rows find no id, no
        // palette, and stay hidden.
        QString gearId;
        for (const WeaponSlotRow& sr : m_weaponRows) {
            if (sr.slot != row.slot || !sr.combo || sr.combo->currentIndex() < 0)
                continue;
            const int fileIdx = sr.combo->currentData().toInt();
            if (fileIdx < 0) break;
            if (m_source.partsForSubject)
                for (const fox::CatalogPart& p :
                     m_source.partsForSubject(currentSubjectId(), row.slot))
                    if (p.modelFileIdx == fileIdx) {
                        gearId = row.companion ? p.companionGearId : p.gearId;
                        break;
                    }
            break;
        }
        const fox::MgoGearItem* item =
            gearId.isEmpty() ? nullptr : mgoItemById(gearId, female);
        const QStringList palette = !item ? QStringList()
            : (row.channel == 0 ? item->primary : item->secondary);
        const QString defId = !item ? QString()
            : (row.channel == 0 ? item->defaultPrimary : item->defaultSecondary);

        // Invalidation on a NEW garment lives in onWeaponSlotChanged and in
        // applyGearExcludes, not here: this fill also runs while a preset
        // restores its slots, and wiping the choice whenever the item
        // differs from the previous one would wipe exactly the choice the
        // preset just loaded.
        const bool show = item && !palette.isEmpty();
        row.combo->setVisible(show);
        if (QWidget* lb = m_weaponRowsForm
                              ? m_weaponRowsForm->labelForField(row.combo)
                              : nullptr)
            lb->setVisible(show);
        const bool b = row.combo->blockSignals(true);
        row.combo->clear();
        if (show) {
            // WHAT A COLOUR ROW CAN SAY. Measured over the shipped
            // GearConfig.lua: 367 colour ids, and NOT ONE carries a
            // NameLangTag — the game ships no display names for colours or
            // camouflages, so no table can be found that would turn "com_c24"
            // into words. What the game does carry is About={ColorType=…},
            // "Solid" or "Pattern" (282 and 85), and the swatch itself. So the
            // row shows the type as its headline, the id beneath it, and the
            // chip beside it — which together is the whole of what the data
            // knows. Order is the game's own palette order and is not sorted.
            // Numbered WITHIN the type and within this item's own palette,
            // so the list reads "Solid 1 … Solid 43, Pattern 1 … Pattern 12"
            // instead of forty-three rows all headed "Solid". The number is
            // the browser's, not the game's — the id underneath is the ground
            // truth and is always shown.
            QHash<QString, int> seenOfType;
            const auto label = [&gc, &seenOfType](const QString& cid) {
                QString t = gc.colourType(cid);
                if (t.isEmpty()) return cid;
                return QStringLiteral("%1 %2").arg(t).arg(++seenOfType[t]);
            };
            // Row 0 is the default, but it is also a member of the palette, so
            // it must take its number from the palette's order rather than
            // from being first — otherwise every default is "Solid 1" and the
            // colour that really is first in the list gets number two.
            QHash<QString, QString> named;
            for (const QString& cid : palette) named.insert(cid, label(cid));
            if (!named.contains(defId)) named.insert(defId, label(defId));
            row.combo->addSwatchItem(
                QStringLiteral("— default — %1").arg(named.value(defId, defId)),
                defId, QString(), QString(), gc.colourSwatch(defId));
            for (const QString& cid : palette) {
                if (cid == defId) continue;   // already row 0
                row.combo->addSwatchItem(named.value(cid, cid), cid, QString(),
                                         cid, gc.colourSwatch(cid));
            }
            const QPair<QString, QString> chosen = m_mgoColours.value(
                row.companion ? companionColourKey(row.slot) : row.slot);
            const QString want = row.channel == 0 ? chosen.first : chosen.second;
            if (!want.isEmpty()) row.combo->selectPayload(want);
            row.combo->refreshCurrentIcon();
        }
        row.combo->blockSignals(b);
    }
}

void CustomizeTab::onGearColourChanged(int rowIdx)
{
    if (m_weaponRebuilding) return;
    if (rowIdx < 0 || rowIdx >= m_gearColourRows.size()) return;
    const GearColourRow& row = m_gearColourRows[rowIdx];
    if (!row.combo) return;
    const QString cid = row.combo->currentData().toString();
    const QString key =
        row.companion ? companionColourKey(row.slot) : row.slot;
    QPair<QString, QString>& slot = m_mgoColours[key];
    if (row.channel == 0) slot.first = cid;
    else slot.second = cid;
    if (slot.first.isEmpty() && slot.second.isEmpty())
        m_mgoColours.remove(key);
    row.combo->refreshCurrentIcon();

    // Retexture just the part this row dressed — the same camo-change path,
    // scoped to one part, with the look laid back over the top because the
    // pass replaces both texture sets wholesale.
    //
    // WHICH part depends on which half of the garment the row belongs to. A
    // Vest Color row dyes the COMPANION model, and reading the slot row's own
    // combo would hand the change to the base half instead — where the dye
    // does not apply, so the picture never changed and the choice looked
    // ignored.
    int fileIdx = -1;
    for (const WeaponSlotRow& sr : m_weaponRows) {
        if (sr.slot != row.slot || !sr.combo || sr.combo->currentIndex() < 0)
            continue;
        fileIdx = sr.combo->currentData().toInt();
        if (row.companion && fileIdx >= 0 && m_source.partsForSubject) {
            int comp = -1;
            for (const fox::CatalogPart& p :
                 m_source.partsForSubject(currentSubjectId(), sr.slot))
                if (p.modelFileIdx == fileIdx) { comp = p.companionFileIdx; break; }
            fileIdx = comp;
        }
        break;
    }
    const QString camo = m_weaponCamo ? m_weaponCamo->currentData().toString()
                                      : QString();
    for (int i = 0; i < m_parts.size(); ++i) {
        if (fileIdx < 0 || m_parts[i].fileIdx != fileIdx) continue;
        applyFovaToPart(i, camo);
        applyAvatarLookToPart(i);
        break;
    }
    rebuildScene();
}

QPair<QString, QString> CustomizeTab::mgoColoursForPart(int fileIdx) const
{
    if (fileIdx < 0 || m_mgoColours.isEmpty()) return {};
    for (const WeaponSlotRow& sr : m_weaponRows) {
        if (!sr.combo || sr.isLook || sr.combo->currentIndex() < 0) continue;
        if (!sr.slot.startsWith(QLatin1String("mgo_"))) continue;
        const int chosen = sr.combo->currentData().toInt();
        if (chosen == fileIdx) return m_mgoColours.value(sr.slot);
        // The garment's SECOND model asks about itself by its own file index
        // and must get the second half's dye, not the first half's — the two
        // halves are separately dyeable and answering with the wrong pair is
        // how a jacket ends up the colour of the trousers.
        //
        // Resolved through the CATALOGUE, not through companionPartIdx: this
        // is called from addPart(), which runs before the row can know where
        // in m_parts the companion landed, so a part-index test answered "no
        // dye" for the one load that needed it. A file index is also immune to
        // m_parts being renumbered by a removal.
        if (chosen < 0 || !m_source.partsForSubject) continue;
        for (const fox::CatalogPart& p :
             m_source.partsForSubject(currentSubjectId(), sr.slot))
            if (p.modelFileIdx == chosen && p.companionFileIdx == fileIdx)
                return m_mgoColours.value(companionColourKey(sr.slot));
    }
    return {};
}

int CustomizeTab::mgoColourFovaIndex(const QString& colourId)
{
    if (colourId.isEmpty()) return -1;
    const fox::ArchiveIndex& index = fox::ArchiveIndex::instance();
    const auto& files = index.files();
    if (m_mgoColourFv2Key != files.constData()
        || m_mgoColourFv2Count != files.size()) {
        m_mgoColourFv2Key = files.constData();
        m_mgoColourFv2Count = files.size();
        m_mgoColourFv2.clear();
        // Every colour id ships ONE .fv2 somewhere under the MGO fova tree
        // (common/ for the shared camouflages, body/head/chest for a
        // family's own dyes). MGO only: Survive's fova tree under
        // /Assets/ssd/ reuses names, the same cross-game trap as the gear
        // models.
        for (int i = 0; i < files.size(); ++i) {
            const fox::IndexedFile& f = files[i];
            if (!f.named) continue;
            if (!f.path.endsWith(QLatin1String(".fv2"), Qt::CaseInsensitive))
                continue;
            if (!f.path.startsWith(QLatin1String("/Assets/mgo/fova/chara/"),
                                   Qt::CaseInsensitive))
                continue;
            const QString id = f.path.section(QLatin1Char('/'), -1)
                                   .section(QLatin1Char('.'), 0, 0);
            const auto ex = m_mgoColourFv2.constFind(id);
            if (ex == m_mgoColourFv2.constEnd()
                || (!f.shadowed && files[ex.value()].shadowed))
                m_mgoColourFv2.insert(id, i);
        }
    }
    return m_mgoColourFv2.value(colourId, -1);
}

// A face preset names its hair as well as its head. Apply that to the Hair
// row so the two agree, exactly as picking the preset in-game does. Signals are
// blocked: the caller rebuilds once, afterwards.
// Read the currently selected face preset into m_look. Called on every
// rebuild, because the look has to survive a rebuild that was triggered by
// something else entirely — changing a jacket must not reset the skin tone.
void CustomizeTab::updateLookFromHead()
{
    m_lookActive = false;
    for (const WeaponSlotRow& r : m_weaponRows) {
        if (r.slot != QLatin1String("head") || !r.combo
            || r.combo->currentIndex() < 0)
            continue;
        const QVariant pv =
            r.combo->itemData(r.combo->currentIndex(), richcombo::PresetRole);
        const fox::AvatarPresets& ap = fox::AvatarPresets::instance();
        const QVector<fox::AvatarPreset>& tbl = ap.presets(lookSex());
        const int idx = pv.isValid() ? pv.toInt() : -1;
        const bool havePreset = idx >= 0 && idx < tbl.size();
        const fox::AvatarTextures& at = fox::AvatarTextures::instance();

        // NO PRESET IS NOT NO LOOK. This used to `return` the moment the head
        // row carried no preset index, leaving m_lookActive false — and that
        // one line switched off the ENTIRE avatar look for any page whose head
        // list is raw models rather than the game's numbered grid. The MGO
        // woman is exactly that page (avatar_presets_women is in none of the
        // shipped archives), and so is any head chosen from the un-numbered
        // tail of a grid that does not name it.
        //
        // What was lost was not just the preset's own choices: the look is
        // also what applies the face textures, the skin tone, the brows and
        // the facial hair, and what hides the bandanna welded to every avatar
        // head. All of it was off, which is why those heads came out
        // untextured, tone-locked, browless and wearing a headband.
        //
        // Without a preset the look starts NEUTRAL — the install's first tone
        // and wrinkle set, nothing else chosen — and the pass below then
        // applies whatever the appearance rows say. That is the same code path
        // the preset case uses for explicit choices, so the rows become direct
        // controls instead of being switched off.
        fox::AvatarLook look;
        // ESCAPE HATCH. FOXAB_NO_NEUTRAL_LOOK=1 restores the behaviour before
        // the neutral look existed: no preset, no look at all. It is here
        // because this change turns a system ON for pages that never had it,
        // and the difference only shows on an install that carries the maps —
        // so if it goes wrong on yours it should be one environment variable
        // to get back to known-good rather than a wait for the next build.
        static const bool kNoNeutral =
            qEnvironmentVariableIsSet("FOXAB_NO_NEUTRAL_LOOK");
        if (!havePreset && kNoNeutral) return;
        if (!havePreset) {
            look.skin = at.skins().value(0, 0);
            look.wrinkle = at.wrinkles().value(0, 0);
            look.hairColour = 0;
            look.browColour = 0;
            look.browShape = -1;
            look.beard = -1;
            look.beardWanted = false;
            look.decoType = -1;
            look.decoId = -1;
            look.skinChosen = false;
            m_look = look;
            m_lookActive = at.ok();
            break;
        }
        const fox::AvatarPreset& pr = tbl[idx];
        look.skinChosen = true;
        look.skin = at.skins().value(qMax(0, pr.skinColour), 0);
        look.wrinkle = at.wrinkles().value(0, 0);
        look.hairColour = pr.hairColour;
        look.browColour = pr.hairColour;   // brows share the hair colour set
        look.browShape = pr.browShape;
        // pr.beard is a FAMILY (0-4) and pr.beardVariant its digit; the look
        // carries a beardShapes() index, so convert rather than assign.
        look.beard = at.beardIndexOf(pr.beard, pr.beardVariant);
        // The preset's own answer to "is there a beard", kept separate from
        // whether this install could resolve one — see AvatarLook::beardWanted.
        look.beardWanted = pr.beard >= 0;
        // deco {type,id}: type 0 is a scar (gash), 1 a tattoo. Checked against
        // the shipped sets — preset 19 is {0,1} and avm_gash0_v01 is a cheek
        // scar, which is what preset 19 shows in game.
        look.decoType = pr.decoType;
        look.decoId = pr.decoId;
        look.eyeColourR = pr.eyeColourR;
        look.eyeColourL = pr.eyeColourL;
        look.eyeShadeR = pr.eyeShadeR;
        look.eyeShadeL = pr.eyeShadeL;
        m_look = look;
        m_lookActive = at.ok();
        break;
    }
    if (!m_lookActive) return;
    // Anything set deliberately wins over the preset's value.
    const fox::AvatarTextures& at = fox::AvatarTextures::instance();
    for (const WeaponSlotRow& r : m_weaponRows) {
        if (!r.isLook || !r.combo || r.combo->currentIndex() <= 0) continue;
        const int v = r.combo->currentData().toInt();
        if (r.slot == QLatin1String("look:skin")) {
            // The user picking a tone is a chosen tone, exactly as a preset's
            // is — see AvatarLook::skinChosen. Without this, choosing Skin
            // Colour on a head the numbered grid does not name retextured the
            // FACE and left the bare arms and legs on the tone their model
            // shipped, which is the head-that-does-not-match-the-body the
            // substitution exists to prevent.
            //
            // The flag is set by the SAME bounds test the value uses, not
            // beside it: an out-of-range row index leaves the tone untouched,
            // and claiming a choice that did not take effect would switch the
            // body substitution on over a tone nobody picked.
            if (v >= 0 && v < at.skins().size()) {
                m_look.skin = at.skins().at(v);
                m_look.skinChosen = true;
            }
        }
        else if (r.slot == QLatin1String("look:wrinkle"))
            m_look.wrinkle = at.wrinkles().value(v, m_look.wrinkle);
        else if (r.slot == QLatin1String("look:brow"))
            m_look.browShape = v;
        else if (r.slot == QLatin1String("look:beard")) {
            m_look.beard = v >= 1000 ? -1 : v;
            // A row that is not "from face preset" is the user speaking, so it
            // sets the intent too: 1000+ is the explicit clean-shaven entry.
            m_look.beardWanted = v < 1000;
        }
        else if (r.slot == QLatin1String("look:haircol")) {
            m_look.hairColour = v;
            m_look.browColour = v;
        } else if (r.slot == QLatin1String("look:eyecolR")
                   || r.slot == QLatin1String("look:eyecolL")) {
            // A deliberate choice sets the SHADE too. The row's chip is drawn
            // in the first shade the install ships, so leaving the preset's
            // shade in place would show one print in the list and render the
            // other on the model.
            const int sh = at.irisShades().value(0, 0);
            if (r.slot == QLatin1String("look:eyecolR")) {
                m_look.eyeColourR = v;
                m_look.eyeShadeR = sh;
            } else {
                m_look.eyeColourL = v;
                m_look.eyeShadeL = sh;
            }
        } else if (r.slot == QLatin1String("look:deco")) {
            if (v >= 1000) { m_look.decoType = -1; m_look.decoId = -1; }
            else { m_look.decoType = v / 100; m_look.decoId = v % 100; }
        }
    }
}

void CustomizeTab::applyFacePresetSideEffects(int headRow, bool reset)
{
    if (headRow < 0 || headRow >= m_weaponRows.size()) return;
    SearchableCombo* head = m_weaponRows[headRow].combo;
    if (!head || head->currentIndex() < 0) return;
    const QVariant pv = head->itemData(head->currentIndex(), richcombo::PresetRole);
    const int presetIdx = pv.isValid() ? pv.toInt() : -1;

    // EVERY appearance row goes back to "— from face preset —" first. A preset
    // is a whole character, so the one thing it must not do is inherit the
    // previous one's explicit choices: pick a beard under preset 2, switch to
    // preset 1, and without this the beard survives and the result is not the
    // preset the grid showed. Row 0 is "from the face preset" on every look
    // row by construction (see fillLookRows), so this hands control back
    // rather than blanking anything.
    //
    // THE RESET RUNS WITHOUT A PRESET TOO, and that is the point of the early
    // return having moved below it. A head row only carries a PresetRole when
    // the game's grid was applied to it, and the MGO woman's grid —
    // avatar_presets_women — is in none of the shipped archives, so her Face
    // row is raw head models and this whole function used to return on its
    // first line. Switching her head then left the previous head's hair, brow
    // and beard in place, which is exactly the "presets aren't clearing slots"
    // report. Changing the head is a change of character whether or not a
    // numbered grid named it.
    //
    // Signals stay blocked: onWeaponSlotChanged() called us and rebuilds once
    // when we return, and letting eight rows each trigger their own rebuild
    // would be both slow and a chance for a half-applied preset to render.
    if (reset)
        for (WeaponSlotRow& r : m_weaponRows) {
            if (!r.isLook || !r.combo || r.combo->currentIndex() == 0) continue;
            const bool b = r.combo->blockSignals(true);
            r.combo->setCurrentIndex(0);
            r.combo->refreshCurrentIcon();
            r.combo->blockSignals(b);
        }

    const fox::AvatarPresets& ap = fox::AvatarPresets::instance();
    const QVector<fox::AvatarPreset>& tbl = ap.presets(lookSex());
    if (presetIdx < 0 || presetIdx >= tbl.size()) {
        // No grid for this subject. The hair row still has to be cleared —
        // leaving it is the same bug the reset above exists to prevent, and
        // there is no preset to say which hairstyle should replace it.
        if (reset)
            for (WeaponSlotRow& r : m_weaponRows) {
                if (r.slot != QLatin1String("hair") || !r.combo) continue;
                const bool b = r.combo->blockSignals(true);
                r.combo->setCurrentIndex(0);
                r.combo->refreshCurrentIcon();
                r.combo->blockSignals(b);
                break;
            }
        return;
    }
    const fox::AvatarPreset& pr = tbl[presetIdx];

    for (WeaponSlotRow& r : m_weaponRows) {
        if (r.slot != QLatin1String("hair") || !r.combo) continue;
        const QString wantStem =
            fox::AvatarPresets::hairStemFor(pr.hairMesh, lookSex());
        const bool b = r.combo->blockSignals(true);
        if (wantStem.isEmpty()) {
            r.combo->setCurrentIndex(0);   // row 0 is the default / none
        } else {
            // Matched on the STYLE, not on the exact stem. The preset table
            // names the covered form ("avm_hair_a0_v0_cov") and the list now
            // shows one row per style under the plain name — so an exact
            // comparison stopped matching anything the day the two were
            // folded, and every face preset silently left the hairstyle
            // alone. Reducing both sides to the style key matches whichever
            // of the pair an install actually carries.
            static const QRegularExpression hairForm{
                QStringLiteral("_v([0-9])(?:_cov|[0-9])$")};
            const auto styleKey = [](const QString& stem) {
                const QRegularExpressionMatch m = hairForm.match(stem);
                return m.hasMatch() ? stem.left(m.capturedStart()) : stem;
            };
            const QString want = styleKey(wantStem);
            const auto& files = fox::ArchiveIndex::instance().files();
            for (int i = 0; i < r.combo->count(); ++i) {
                const int fi = r.combo->itemData(i, richcombo::PayloadRole).toInt();
                if (fi < 0 || fi >= files.size()) continue;
                const QString stem = files[fi].path.section(QLatin1Char('/'), -1)
                                         .section(QLatin1Char('.'), 0, 0);
                if (styleKey(stem).compare(want, Qt::CaseInsensitive) == 0) {
                    r.combo->setCurrentIndex(i);
                    break;
                }
            }
        }
        r.combo->refreshCurrentIcon();
        r.combo->blockSignals(b);
        break;
    }
}

// Which of the customize combo's four captions a variation belongs under.
//
// The split is the game's own naming, not a guess: a weapon or vehicle ships
// "camo_cNN" tables (the patterns) alongside "scol_cNN" tables (solid paint).
// Anything else — a model's own "cam"/"clv" base, a character's "vNN" slot, a
// skin tone — goes under its own caption rather than being filed as something
// it is not.
//
// Measured on 181 shipped MGO weapon tables (--fovacensus), because the two
// that land in MODEL VARIATION are not interchangeable and the numbers say so:
// "cam" and "clv" substitute DISJOINT texture sets, and the difference that
// matters is one role — "clv" writes Layer_Tex_SRGB and "cam" never does. So
// "cam" is the camouflage-CAPABLE base, deliberately leaving the layer slot
// free for a camo_cNN table to fill, and "clv" fills it itself.
CustomizeTab::CamoSection
CustomizeTab::camoSectionFor(const fox::CatalogVariation& v)
{
    const QString low = v.name.toLower();
    if (low.startsWith(QLatin1String("camo_c"))) return CamoSection::CamoPattern;
    if (low.startsWith(QLatin1String("scol_c"))) return CamoSection::BaseColor;
    if (v.path.contains(QLatin1String("_skin"), Qt::CaseInsensitive))
        return CamoSection::SkinTone;
    if (!camoIndexFor(v).isEmpty()) return CamoSection::CamoPattern;
    return CamoSection::ModelVariation;
}

const char* CustomizeTab::camoSectionCaption(CamoSection s)
{
    switch (s) {
        case CamoSection::CamoPattern:    return "CAMO PATTERN";
        case CamoSection::BaseColor:      return "BASE COLOR";
        case CamoSection::SkinTone:       return "SKIN TONE";
        case CamoSection::ModelVariation: return "MODEL VARIATION";
    }
    return "MODEL VARIATION";
}

void CustomizeTab::refreshWeaponCamoList()
{
    if (!m_weaponCamo) return;
    const bool wasBlocked = m_weaponCamo->blockSignals(true);
    // WHAT WAS SELECTED SURVIVES THE REBUILD. This list is rebuilt by fourteen
    // call sites — fitting a barrel is one of them — and clear() dropped the
    // choice every time, so a camouflage picked by hand quietly went back to
    // "— default —" the moment anything else on the weapon changed. Kept here
    // rather than at the callers for the same reason fillOpenPanels() is:
    // fourteen sites that each have to remember is thirteen chances to forget.
    const QString previous = m_weaponCamo->currentData().toString();
    m_weaponCamo->clear();
    // The customize screen's first colour row is the one that takes the paint
    // off again, and its chip is the game's own transparency grid.
    m_weaponCamo->addSwatchItem(
        QStringLiteral("— default —"),
        QStringLiteral("the model's own textures, nothing substituted"),
        QString(), QString(), QStringLiteral(kSwatchDir) + QStringLiteral("ui_cstm_default_alp"));

    const auto& files = ArchiveIndex::instance().files();
    const auto stemOf = [&](int idx) {
        return (idx >= 0 && idx < files.size())
            ? files[idx].path.section(QLatin1Char('/'), -1)
                  .section(QLatin1Char('.'), 0, 0)
            : QString();
    };
    QString stem;
    if (m_weaponVersion && m_weaponVersion->count() > 0) {
        const QVariant pv = m_weaponVersion->currentIndex() >= 0
            ? m_weaponVersion->currentData() : QVariant();
        stem = stemOf(pv.isValid() ? pv.toInt() : -1);
    }
    // Which models to read variations from. A weapon's receiver carries the
    // whole list — its parts share it. A CHARACTER does not: a Survive base
    // body has no tables at all, and the skin tones are authored per part
    // (arf0_main0_skin0_c00…c04 on the arm, bdf0_… on the body). So a
    // contextual category asks the base AND everything fitted, and the union
    // is the menu — which is also how the game applies one skin tone across
    // every part at once.
    QStringList stems;
    if (!stem.isEmpty()) stems << stem;
    if (m_source.contextual())
        for (const WeaponSlotRow& r : m_weaponRows) {
            if (!r.combo || r.combo->currentIndex() < 0) continue;
            const QVariant pv = r.combo->currentData();
            const QString ps = stemOf(pv.isValid() ? pv.toInt() : -1);
            if (!ps.isEmpty() && !stems.contains(ps)) stems << ps;
        }

    if (!stems.isEmpty()) {
        QHash<QString, fox::CatalogVariation> byName;
        QStringList names;
        for (const QString& st : stems)
            for (const fox::CatalogVariation& v : m_source.variationsFor(st))
                if (!byName.contains(v.name)) { byName.insert(v.name, v); names << v.name; }
        names.sort();

        // Two categories, exactly as the screen splits them — and the split is
        // the game's own naming, not a guess: a weapon or vehicle ships
        // "camo_cNN" tables (the patterns) alongside "scol_cNN" tables (solid
        // paint). Anything else — a model's own "cam"/"clv" base, a character's
        // "vNN" slot, a skin tone — goes under its own caption rather than
        // being filed as something it is not.
        QStringList camo, base, skin, other;
        for (const QString& n : names)
            switch (camoSectionFor(byName.value(n))) {
                case CamoSection::CamoPattern:    camo << n; break;
                case CamoSection::BaseColor:      base << n; break;
                case CamoSection::SkinTone:       skin << n; break;
                case CamoSection::ModelVariation: other << n; break;
            }
        const auto section = [&](const char* caption, const QStringList& list) {
            if (list.isEmpty()) return;
            m_weaponCamo->addHeaderItem(QLatin1String(caption));
            for (const QString& n : list) {
                const fox::CatalogVariation& v = byName[n];
                // "c12" is not a camouflage anyone can picture — the
                // development list names all 56 of them, and the same indices
                // are used on Snake's uniforms, on buddy gear and on vehicle
                // paint. The raw token stays on the second line so it is still
                // clear which asset suffix was chosen.
                const QString real = camoLabelFor(v);
                m_weaponCamo->addSwatchItem(real.isEmpty() ? n : real, n,
                                            v.path, n, swatchPathFor(v));
            }
        };
        section(camoSectionCaption(CamoSection::CamoPattern), camo);
        section(camoSectionCaption(CamoSection::BaseColor), base);
        section(camoSectionCaption(CamoSection::SkinTone), skin);
        section(camoSectionCaption(CamoSection::ModelVariation), other);
    }
    selectDefaultWeaponCamo(previous);
    m_weaponCamo->setEnabled(m_weaponCamo->count() > 1);
    m_weaponCamo->blockSignals(wasBlocked);
    m_weaponCamo->refreshCurrentIcon();
}

// ── --restalignsweep <tsv> ──────────────────────────────────────────────────
//
// Equip EVERY item of every slot the current subject has, one at a time, pose
// each on the clip that is already loaded, and write how far the item's own
// root bone landed from the bone it hangs off. Ninety-odd scenes, one file,
// one double-click — instead of one log line for one item somebody had to
// think to check, which is how "the accessory slot is still wrong" survived
// two batches that each fixed a real bug in it.
//
// It reads m_lastAlign, which setFrame fills on every pose. The residual is
// therefore the SAME number the viewport acted on, not a re-derivation of it.
//
// Run it with the scene already built and a clip already selected: the harness
// does --character, then --mtar/--clip/--frame, then this.
QString CustomizeTab::restAlignSweepReport(const QString& tsvPath)
{
    if (m_weaponRows.isEmpty())
        return QStringLiteral("no slots — build a subject first");
    if (!m_hasAnim)
        return QStringLiteral("no clip loaded — the whole point is the POSED "
                              "position, and at bind every part is right");
    QFile f(tsvPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return QStringLiteral("cannot write %1").arg(tsvPath);
    QTextStream out(&f);
    out << "slot\titem\tbones\trootBone\tdriven\tregime\texit\tanchorPart\t"
           "anchorBone\tdx\tdy\tdz\tresidual\tmeshResidual\tmeshVerts\t"
           "bindOffset\tmeshOffset\tverdict\n";

    const float frame = m_frame;
    int tested = 0, floating = 0, unmeasured = 0;
    QStringList worst;
    // The rows are walked by INDEX and the combo re-read each time: equipping
    // an item runs the game's own Exclude rules, which can clear another slot
    // and rebuild the rows underneath a held reference.
    for (int ri = 0; ri < m_weaponRows.size(); ++ri) {
        if (ri >= m_weaponRows.size()) break;
        SearchableCombo* combo = m_weaponRows[ri].combo;
        const QString slot = m_weaponRows[ri].slot;
        if (!combo || m_weaponRows[ri].isLook) continue;
        const int rows = combo->count();
        for (int ii = 1; ii < rows && ri < m_weaponRows.size(); ++ii) {
            if (combo->itemData(ii, richcombo::HeaderRole).toBool()) continue;
            {
                const bool b = combo->blockSignals(true);
                combo->setCurrentIndex(ii);
                combo->blockSignals(b);
            }
            rebuildWeapon();
            // rebuildWeapon() drops the pose; put the clip back on it. The
            // frame matters — a part can be right at frame 0 and a metre out
            // at 94, which is the entire shape of this bug.
            setFrame(frame);
            const int pi = m_weaponRows[ri].partIdx;
            if (pi < 0 || pi >= m_lastAlign.size()) continue;
            const PartAlign& a = m_lastAlign[pi];
            ++tested;
            QString verdict;
            // "host" is not a failure and must not read like one. An item that
            // is the LARGEST skeleton in the scene has nothing to be measured
            // against — on a full install the base body is the host and this
            // only happens for a torso in a scene with no base.
            if (a.regime == 'H')        { verdict = QStringLiteral("host — nothing larger to measure against"); ++unmeasured; }
            else if (a.residual < 0.0f) { verdict = QStringLiteral("NOT MEASURABLE — no part in the scene carries its root bone"); ++unmeasured; }
            else if (a.residual > 0.02f){ verdict = QStringLiteral("FLOATING (bone)"); ++floating;
                                          worst << QStringLiteral("%1 %2 bone %3 m  (mesh %4 m)")
                                                       .arg(slot, a.stem)
                                                       .arg(double(a.residual), 0, 'f', 4)
                                                       .arg(double(a.meshResidual), 0, 'f', 4); }
            // NO MESH VERDICT — and that is a deliberate omission, recorded
            // here so it is not "fixed" by someone adding one.
            //
            // meshResidual is reported as a COLUMN below, because the number
            // is worth having, but it must not decide pass or fail as it
            // stands: it compares the skinned centroid's offset from the
            // anchor bone against the offset the author drew, and for any
            // part the clip actually DEFORMS those two legitimately differ.
            // Thresholded at 2 cm it called 116 of 145 items displaced,
            // including every leg garment at 5-9 cm, which is a walk cycle
            // bending a knee and nothing else. Replacing a metric that is
            // blind to this fault with one that cries wolf about every limb
            // is not an improvement.
            //
            // The valid form of this test is at BIND, where nothing deforms —
            // and at bind a rigid translate moves bone and mesh together, so
            // it collapses back into the bone test above. Which means the
            // fault the screenshots show is NOT in the number: it is that the
            // bind-pose render goes through applyAttachTransforms' group
            // transform while the posed path folds translate(d) into the
            // palette, and nothing has ever measured the first one.
            else                          verdict = QStringLiteral("seated");
            out << slot << '\t' << a.stem << '\t' << a.bones << '\t'
                << a.rootBone << '\t' << a.driven << '\t'
                << alignRegimeName(a.regime) << '\t' << a.source << '\t'
                << a.anchorPart << '\t' << a.anchorBone << '\t'
                << QString::number(double(a.d.x()), 'f', 4) << '\t'
                << QString::number(double(a.d.y()), 'f', 4) << '\t'
                << QString::number(double(a.d.z()), 'f', 4) << '\t'
                << (a.residual < 0.0f ? QString()
                                      : QString::number(double(a.residual), 'f', 4))
                << '\t'
                << (a.meshResidual < 0.0f
                        ? QString()
                        : QString::number(double(a.meshResidual), 'f', 4))
                << '\t' << a.meshVerts << '\t'
                << QString::number(double(a.bindOffset.length()), 'f', 4) << '\t'
                << QString::number(double(a.meshOffset.length()), 'f', 4) << '\t'
                << verdict << '\n';
        }
        // Put the row back to "none" so the next slot is measured on its own
        // rather than on a character wearing everything tried before it.
        if (ri < m_weaponRows.size() && m_weaponRows[ri].combo) {
            const bool b = m_weaponRows[ri].combo->blockSignals(true);
            m_weaponRows[ri].combo->setCurrentIndex(0);
            m_weaponRows[ri].combo->blockSignals(b);
        }
    }
    f.close();

    qInfo("restalignsweep: %d item(s) tested at frame %.1f -> %s",
          tested, double(frame), qUtf8Printable(tsvPath));
    qInfo("restalignsweep: seated %d · FLOATING %d · not measured %d "
          "(an item that is itself the largest skeleton in the scene has "
          "nothing to measure against)",
          tested - floating - unmeasured, floating, unmeasured);
    for (const QString& w : worst)
        qInfo("restalignsweep:   FLOATING  %s", qUtf8Printable(w));
    if (floating == 0)
        qInfo("restalignsweep: every item measured lands within 2 cm of the "
              "bone it hangs on");
    return QStringLiteral("%1 tested, %2 floating, %3 not measurable")
        .arg(tested).arg(floating).arg(unmeasured);
}

// ── --camodump <tsv> ────────────────────────────────────────────────────────
//
// Every row refreshWeaponCamoList() would build, for every weapon subject and
// every variant of it, with the section it lands under. QUEUE 2 asks three
// things and all three are censuses rather than looks: does a weapon actually
// ship both "cam" and "clv"; are those the only two MODEL VARIATION entries;
// and what else is in the list. A combo popup is its own top-level window and
// never appears in a screenshot, so this is the check — the same shape as
// --filemenu and for the same reason.
//
// It reads the CATALOGUE, not the widget, so it needs no scene built and it
// covers every weapon in the install rather than the one on screen.
QString CustomizeTab::camoDumpReport(const QString& tsvPath)
{
    setBuilderCategory(1);   // weapons; the rule is scoped to them
    const auto& files = ArchiveIndex::instance().files();

    // Which SUBJECT owns each model stem, when the install has the chimera
    // packs that say so. A partial extract has the .fv2 tables and no packs,
    // and the census still has to run there — so the stems come from the
    // VARIATION catalogue and the subject is an annotation, not the key.
    QHash<QString, QString> subjectOfStem, groupOfStem;
    for (const fox::CatalogSubject& subj : m_source.subjects)
        for (const fox::CatalogPart& var : subj.variants) {
            if (var.modelFileIdx < 0 || var.modelFileIdx >= files.size()) continue;
            const QString stem = files[var.modelFileIdx].path
                                     .section(QLatin1Char('/'), -1)
                                     .section(QLatin1Char('.'), 0, 0);
            if (!stem.isEmpty() && !subjectOfStem.contains(stem)) {
                subjectOfStem.insert(stem, subj.id);
                groupOfStem.insert(stem, subj.groupName);
            }
        }

    QStringList stems = fox::WeaponCatalog::instance().variationStems();
    for (auto it = subjectOfStem.constBegin(); it != subjectOfStem.constEnd(); ++it)
        if (!stems.contains(it.key())) stems << it.key();
    stems.sort();
    if (stems.isEmpty())
        return QStringLiteral("no weapon variation tables in the indexed data");

    QFile f(tsvPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return QStringLiteral("cannot write %1").arg(tsvPath);
    QTextStream out(&f);
    out << "stem\tsubject\tgroup\tsection\tname\tlabel\tcamoIndex\tswatch\tpath\n";

    int rows = 0, withCam = 0, withClv = 0, withBoth = 0, withNeither = 0;
    QHash<QString, int> modelVariationNames;   // the census QUEUE 2 asks for
    QHash<int, int> perSection;
    for (const QString& stem : stems) {
        bool cam = false, clv = false;
        for (const fox::CatalogVariation& v : m_source.variationsFor(stem)) {
            const CamoSection sec = camoSectionFor(v);
            ++perSection[int(sec)];
            ++rows;
            if (sec == CamoSection::ModelVariation) {
                ++modelVariationNames[v.name.toLower()];
                if (v.name.compare(QLatin1String("cam"), Qt::CaseInsensitive) == 0)
                    cam = true;
                if (v.name.compare(QLatin1String("clv"), Qt::CaseInsensitive) == 0)
                    clv = true;
            }
            out << stem << '\t' << subjectOfStem.value(stem) << '\t'
                << groupOfStem.value(stem) << '\t' << camoSectionCaption(sec)
                << '\t' << v.name << '\t' << camoLabelFor(v) << '\t'
                << camoIndexFor(v) << '\t' << swatchPathFor(v) << '\t'
                << v.path << '\n';
        }
        if (cam && clv) ++withBoth;
        if (cam) ++withCam;
        if (clv) ++withClv;
        if (!cam && !clv) ++withNeither;
    }
    f.close();

    // The histogram is the answer, so it goes in the LOG as well as the file —
    // what settles QUEUE 2 is four numbers, and making someone open a
    // spreadsheet for four numbers is how a measurement stays unmeasured.
    qInfo("camodump: %lld model stem(s), %d row(s) -> %s", qint64(stems.size()),
          rows, qUtf8Printable(tsvPath));
    qInfo("camodump: sections — CAMO PATTERN %d · BASE COLOR %d · SKIN TONE %d "
          "· MODEL VARIATION %d",
          perSection.value(int(CamoSection::CamoPattern)),
          perSection.value(int(CamoSection::BaseColor)),
          perSection.value(int(CamoSection::SkinTone)),
          perSection.value(int(CamoSection::ModelVariation)));
    qInfo("camodump: stems shipping cam %d · clv %d · BOTH %d · NEITHER %d "
          "(of %lld)", withCam, withClv, withBoth, withNeither,
          qint64(stems.size()));
    QStringList names = modelVariationNames.keys();
    names.sort();
    for (const QString& n : names)
        qInfo("camodump:   MODEL VARIATION '%s' on %d stem(s)%s",
              qUtf8Printable(n), modelVariationNames[n],
              (n == QLatin1String("cam") || n == QLatin1String("clv"))
                  ? "" : "   <-- NOT cam/clv");
    if (m_source.subjects.isEmpty())
        qInfo("camodump: no chimera packs in this install, so the subject and "
              "group columns are empty — the census itself is unaffected");
    return QStringLiteral("%1 stem(s), %2 row(s); cam %3, clv %4, both %5, "
                          "neither %6")
        .arg(stems.size()).arg(rows).arg(withCam).arg(withClv)
        .arg(withBoth).arg(withNeither);
}

// ── --camodefault ───────────────────────────────────────────────────────────
//
// selectDefaultWeaponCamo() decides four things and the container can reach
// none of them through a scene: the weapon catalogue is keyed on
// /pack/collectible/chimera/…fpk packs, a partial extract has the .fv2 tables
// and no packs, so no weapon subject exists to build and the camo combo is
// never populated. The rule is still testable — it reads a list and returns a
// row — so the list is scripted here and the REAL widget and the REAL function
// answer it. Break any expectation below and this reports FAIL.
QString CustomizeTab::camoDefaultSelfTest()
{
    if (!m_weaponCamo) return QStringLiteral("no camo combo");
    const int savedCategory = m_builderCategory;
    struct Case {
        const char* what;
        int category;          // 1 = weapon, 2 = character
        QString previous;
        QStringList rows;      // variation names, in list order after row 0
        QString expect;        // "" = the "— default —" row
    };
    const QVector<Case> cases = {
        {"weapon, ships clv", 1, QString(),
         {QStringLiteral("cam"), QStringLiteral("clv"),
          QStringLiteral("camo_c12")}, QStringLiteral("clv")},
        {"weapon, no clv", 1, QString(),
         {QStringLiteral("def"), QStringLiteral("m01")}, QString()},
        {"weapon, previous wins over clv", 1, QStringLiteral("m01"),
         {QStringLiteral("clv"), QStringLiteral("m01")}, QStringLiteral("m01")},
        {"weapon, previous gone, falls to clv", 1, QStringLiteral("m68"),
         {QStringLiteral("clv"), QStringLiteral("m01")}, QStringLiteral("clv")},
        {"character, clv is not a default", 2, QString(),
         {QStringLiteral("cam"), QStringLiteral("clv")}, QString()},
    };
    int pass = 0, fail = 0;
    for (const Case& c : cases) {
        const bool b = m_weaponCamo->blockSignals(true);
        m_weaponCamo->clear();
        m_weaponCamo->addSwatchItem(QStringLiteral("— default —"), QString(),
                                    QString(), QString(), QString());
        for (const QString& r : c.rows)
            m_weaponCamo->addSwatchItem(r, r, QString(), r, QString());
        m_builderCategory = c.category;
        selectDefaultWeaponCamo(c.previous);
        const QString got = m_weaponCamo->currentData().toString();
        m_weaponCamo->blockSignals(b);
        const bool ok = got == c.expect;
        ok ? ++pass : ++fail;
        qInfo("camodefault: %-38s previous '%s' -> '%s' (expected '%s')  %s",
              c.what, qUtf8Printable(c.previous),
              qUtf8Printable(got.isEmpty() ? QStringLiteral("— default —") : got),
              qUtf8Printable(c.expect.isEmpty() ? QStringLiteral("— default —")
                                                : c.expect),
              ok ? "PASS" : "FAIL");
    }
    m_builderCategory = savedCategory;
    m_weaponCamo->clear();
    return QStringLiteral("%1 pass, %2 FAIL").arg(pass).arg(fail);
}

// Which row a freshly built list opens on.
//
// The user's rule: "weapon default camo variation should only show the two
// model variations both clv and cam, with clv being the default". The default
// half is here. The "only show" half is NOT done, and deliberately — the
// census says it would empty the section for most weapons and throw away art:
// of the 25 weapons in the reference pull only FOUR ship cam/clv at all, and
// the camo_cNN tables substitute 35 textures that appear in no other table in
// the set. Run --camodump on a full install before any row is deleted.
//
// Order: what was selected before wins, then clv, then the model's own
// textures. Preserving first is what stops a barrel change from silently
// repainting the gun, and it is also why "clv by default" does not fight the
// user — it only decides a list that has no previous answer.
void CustomizeTab::selectDefaultWeaponCamo(const QString& previous)
{
    if (!m_weaponCamo || m_weaponCamo->count() <= 1) return;
    const auto find = [&](const QString& want) {
        if (want.isEmpty()) return -1;
        for (int i = 1; i < m_weaponCamo->count(); ++i) {
            if (m_weaponCamo->itemData(i, richcombo::HeaderRole).toBool()) continue;
            if (m_weaponCamo->itemData(i).toString().compare(
                    want, Qt::CaseInsensitive) == 0)
                return i;
        }
        return -1;
    };
    int row = find(previous);
    // WEAPONS ONLY. refreshWeaponCamoList also serves characters and vehicles
    // through the contextual path, where "clv" means nothing and a skin tone
    // is what the first row should be — the user asked about weapons and the
    // rule is gated where they scoped it.
    if (row < 0 && m_builderCategory == 1) row = find(QStringLiteral("clv"));
    m_weaponCamo->setCurrentIndex(row > 0 ? row : 0);
}

// The camouflage index a variation carries ("camo_c12" → "c12") or, for a
// player FOVA slot, the one it SUBSTITUTES — read out of the table itself.
QString CustomizeTab::camoIndexFor(const fox::CatalogVariation& v)
{
    static const QRegularExpression tailRe{QStringLiteral("(^|_)c(\\d+)$")};
    const QRegularExpressionMatch m = tailRe.match(v.name.toLower());
    if (m.hasMatch())
        return QStringLiteral("c%1")
            .arg(m.captured(2).toInt(), 2, 10, QLatin1Char('0'));
    static const QRegularExpression slotRe{QStringLiteral("^v\\d+$")};
    if (v.fileIdx < 0 || !slotRe.match(v.name).hasMatch()) return {};
    const auto cached = m_camoIndexCache.constFind(v.fileIdx);
    if (cached != m_camoIndexCache.constEnd()) return cached.value();
    QString idx;
    const ArchiveIndex& index = ArchiveIndex::instance();
    if (v.fileIdx < index.files().size()) {
        fox::FovaFile fova;
        const QByteArray d = index.readFile(index.files()[v.fileIdx]);
        if (!d.isEmpty() && fova.parse(d))
            for (quint64 tex : fova.textures()) {
                QString path;
                if (!fox::HashResolver::instance().tryResolve(tex, &path)) continue;
                idx = fox::EquipCatalog::camoIndexFromTexture(path);
                if (!idx.isEmpty()) break;
            }
    }
    m_camoIndexCache.insert(v.fileIdx, idx);
    return idx;
}

// The customize screen's chip for one variation.
//
// The two swatch families are not interchangeable, and which is which is
// settled by counting rather than by looking: the dictionaries carry
// cm_camo3_c00…c17 (18) and cm_camo4_c00…c94 (95), and the catalogues offer
// vehicle camo_c00…c17 (18) and weapon camo_c00…c94 (95). The ranges match
// exactly, so vehicles are camo3 and weapons camo4. Characters resolve to the
// 56 player camouflage indices, which only camo4 covers.
//
// Solid paint ("scol_cNN") has no per-index chip in the data at all — the game
// ships one generic mark for it, and that is what is drawn.
QString CustomizeTab::swatchPathFor(const fox::CatalogVariation& v)
{
    const QString dir = QStringLiteral(kSwatchDir);
    if (v.name.startsWith(QLatin1String("scol_"), Qt::CaseInsensitive))
        return dir + QStringLiteral("ui_cstm_color_scol");
    const QString idx = camoIndexFor(v);
    if (idx.isEmpty()) return {};
    const QStringList families = m_builderCategory == 3
        ? QStringList{QStringLiteral("camo3"), QStringLiteral("camo4")}
        : QStringList{QStringLiteral("camo4"), QStringLiteral("camo3")};
    for (const QString& fam : families) {
        const QString p =
            dir + QStringLiteral("ui_%1_%2_lym").arg(fam, idx);
        if (ArchiveIndex::instance().findByHash(
                fox::hashFileNameWithExtension(p + QLatin1String(".ftex"))))
            return p;
    }
    return {};
}

QString CustomizeTab::camoLabelFor(const fox::CatalogVariation& v)
{
    const fox::EquipCatalog& equip = fox::EquipCatalog::instance();
    const QString direct = equip.camoName(v.name);
    if (!direct.isEmpty()) return direct;
    // Only the player FOVA slots ("v03") get their index read out of the table.
    // A weapon's "cam" table also substitutes a cNN texture — ar02's names
    // ar02_main0_cam_c00_bsm — but "cam" is the camouflage-CAPABLE base, with
    // the actual colour coming from a wfv_scol table on top, so calling it
    // OLIVE DRAB would be a claim the data does not support.
    // Skin tones share the cNN numbering with camouflage but are nothing to do
    // with it — calling a skin tone OLIVE DRAB would be nonsense. The file
    // itself says which it is (…_skin0_c02.fv2), so read that rather than the
    // index.
    if (v.path.contains(QLatin1String("_skin"), Qt::CaseInsensitive)) {
        // The index can sit in the variation name ("skin0_c02") or only in the
        // file it came from (…_skin0_c02.fv2, name "c02"), so try both and
        // allow the token to start the string rather than follow a separator.
        static const QRegularExpression toneRe{QStringLiteral("(^|_)c(\\d+)")};
        QRegularExpressionMatch tm = toneRe.match(v.name);
        if (!tm.hasMatch()) tm = toneRe.match(v.path.section(QLatin1Char('/'), -1));
        return tm.hasMatch()
            ? QStringLiteral("Skin tone %1").arg(tm.captured(2).toInt() + 1)
            : QStringLiteral("Skin tone");
    }
    // One walk of the table, not two: camoIndexFor() reads and caches the
    // substituted index, and the name is a lookup on top of it.
    return equip.camoName(camoIndexFor(v));
}

void CustomizeTab::refreshSlotLabels()
{
    if (!m_weaponRowsForm) return;
    for (const WeaponSlotRow& r : m_weaponRows) {
        if (!r.combo) continue;
        auto* label = qobject_cast<QLabel*>(
            m_weaponRowsForm->labelForField(r.combo));
        if (!label) continue;
        const bool empty = r.partIdx < 0;
        // A weapon part SEATS on a connect point; a character part is SKINNED
        // to the skeleton the whole character shares, so "fitted but not
        // placed" is a weapon idea and marking a head amber for it would be
        // reporting a fault that does not exist.
        // Weapons and vehicles seat parts on connect points; characters are
        // skinned to one shared skeleton. Test the CATEGORY, not the source
        // shape — "All other characters" is a character page too, and it is
        // not contextual.
        const bool seatingMatters =
            m_builderCategory == 1 || m_builderCategory == 3;
        QFont f = label->font();
        f.setBold(!empty && (r.seated || !seatingMatters));
        label->setFont(f);
        QPalette pal = label->palette();
        QColor c = palette().text().color();
        if (r.unusable) {
            c.setAlphaF(0.30);              // the weapon cannot take one
        } else if (empty) {
            c.setAlphaF(0.45);              // nothing fitted
        } else if (!r.seated && seatingMatters) {
            // Not an error dialog, but it must not read as normal either.
            c = QColor(0xC0, 0x6A, 0x00);
        }
        pal.setColor(QPalette::WindowText, c);
        label->setPalette(pal);
        label->setToolTip(
            r.unusable
                ? QStringLiteral("No build the game ships on this weapon uses "
                                 "a %1.").arg(prettySlot(r.slot).toLower())
                : (empty ? QStringLiteral("Nothing fitted.")
                         : (seatingMatters ? r.note
                                           : QStringLiteral(
                                                 "Skinned to the character's "
                                                 "own skeleton."))));
    }
}

void CustomizeTab::refreshWeaponColorList()
{
    if (!m_weaponColor) return;
    const bool blocked = m_weaponColor->blockSignals(true);
    const quint64 keep = m_gearColor;
    m_weaponColor->clear();
    m_weaponColor->addPlainItem(QStringLiteral("— as shipped —"),
                                QVariant(quint64(0)));

    const fox::LayerColorCatalog& lc = fox::LayerColorCatalog::instance();
    const auto addGroup = [&](const char* caption,
                              const QVector<fox::LayerSwatch>& set) {
        if (set.isEmpty()) return;
        m_weaponColor->addHeaderItem(QString::fromLatin1(caption));
        for (const fox::LayerSwatch& sw : set) {
            // The swatch's OWN texture is the chip. The customize screen's
            // ui_* chips exist for some families and not others, and drawing
            // the actual layer map means the chip is the colour that will be
            // applied rather than an artist's approximation of it.
            m_weaponColor->addSwatchItem(
                QStringLiteral("%1 %2").arg(sw.family.mid(3).toUpper(), sw.id),
                sw.path.section(QLatin1Char('/'), -1), sw.path,
                QVariant(sw.pathHash), sw.basePath);
        }
    };
    addGroup("SOLID COLOURS", lc.solids());
    addGroup("CAMO PATTERNS", lc.patterns());

    if (keep == 0 || !m_weaponColor->selectPayload(QVariant(keep)))
        m_weaponColor->setCurrentIndex(0);
    m_weaponColor->blockSignals(blocked);
    // One row, "— as shipped —", means this install carries no colour table.
    // The row then offers a choice of one, which is not a choice; hide it and
    // its label rather than leaving a dead control on every page.
    {
        const bool any = m_weaponColor->count() > 1;
        m_weaponColor->setVisible(any);
        if (QWidget* lb = m_weaponFooterForm
                              ? m_weaponFooterForm->labelForField(m_weaponColor)
                              : nullptr)
            lb->setVisible(any);
    }
    // The combo is the only place the choice is displayed, so the held value
    // has to follow it back down when the selection could not be restored.
    m_gearColor = m_weaponColor->currentData().toULongLong();
}

QString CustomizeTab::gearColorPath() const
{
    return m_gearColor ? layerSwatchPath(m_gearColor) : QString();
}

void CustomizeTab::setGearColorPath(const QString& swatchPath)
{
    const quint64 want = swatchPath.isEmpty() ? 0 : layerSwatchHash(swatchPath);
    // An EMPTY combo is not the same as "no such colour". The list is emptied
    // for the duration of a rescan, and reading the held value back off an
    // empty combo yields an invalid QVariant — which used to drop a perfectly
    // valid colour to 0, silently, in the middle of loading a preset.
    if (!m_weaponColor || m_weaponColor->count() == 0) {
        m_gearColor = want;
        return;
    }
    const bool b = m_weaponColor->blockSignals(true);
    if (want == 0 || !m_weaponColor->selectPayload(QVariant(want)))
        m_weaponColor->setCurrentIndex(0);
    m_weaponColor->blockSignals(b);
    // Read the held value back OFF the combo, so the two can never disagree
    // about what colour is selected.
    m_gearColor = m_weaponColor->currentData().toULongLong();
}

void CustomizeTab::syncPbrFromSettings()
{
    if (!m_view) return;
    const bool want = Config::pbrEnabled(Config::PbrView::Customize);
    if (m_view->pbrShading() == want) return;
    // Through the VIEWPORT, which owns this state now. The reload that fetches
    // the maps the previous mode did not load is not skipped by going this
    // way: the tab watches displayChanged for exactly that.
    m_view->setPbrShading(want);
}

void CustomizeTab::clearWeaponColor()
{
    m_gearColor = 0;
    if (!m_weaponColor) return;
    const bool b = m_weaponColor->blockSignals(true);
    m_weaponColor->clear();
    m_weaponColor->blockSignals(b);
}

bool CustomizeTab::selectGearColor(const QString& swatchStem)
{
    if (!m_weaponColor) return false;
    const fox::LayerColorCatalog& lc = fox::LayerColorCatalog::instance();
    // TWO passes, exact first. One pass that accepted either test let the
    // first SUBSTRING hit beat a later exact match: "cm_scol3_c00_lym" starts
    // the solids list and contains "c00", so asking for a camo "c00" silently
    // got a solid grey instead.
    const auto scan = [&](bool exact) -> quint64 {
        for (const QVector<fox::LayerSwatch>* set : {&lc.solids(), &lc.patterns()})
            for (const fox::LayerSwatch& sw : *set) {
                QString stem = sw.path.section(QLatin1Char('/'), -1);
                stem.truncate(stem.lastIndexOf(QLatin1Char('.')));
                const bool hit = exact
                    ? stem.compare(swatchStem, Qt::CaseInsensitive) == 0
                    : stem.contains(swatchStem, Qt::CaseInsensitive);
                if (hit) return sw.pathHash;
            }
        return 0;
    };
    quint64 want = scan(true);
    if (!want) want = scan(false);
    if (!want) return false;
    // Through the combo, not straight into m_gearColor: the combo is what the
    // user sees and what the preset writer reads back, so setting one without
    // the other would leave the two disagreeing.
    if (!m_weaponColor->selectPayload(QVariant(want))) return false;
    m_gearColor = want;
    return true;
}

void CustomizeTab::onWeaponColorChanged()
{
    if (m_weaponRebuilding) return;
    m_gearColor = m_weaponColor ? m_weaponColor->currentData().toULongLong() : 0;
    if (m_parts.isEmpty()) return;
    // Re-run the SAME path a camouflage change takes. applyFovaToPart() folds
    // the gear colour in, so the two compose instead of overwriting each other
    // — and the look has to be laid back over the top for the same reason it
    // does there: the camo pass replaces both texture sets wholesale.
    const QString name = m_weaponCamo ? m_weaponCamo->currentData().toString()
                                      : QString();
    for (int i = 0; i < m_parts.size(); ++i) {
        applyFovaToPart(i, name);
        applyAvatarLookToPart(i);
    }
    rebuildScene();
}

void CustomizeTab::onWeaponCamoChanged()
{
    if (m_weaponRebuilding || m_parts.isEmpty()) return;
    const QString name = m_weaponCamo->currentData().toString();
    for (int i = 0; i < m_parts.size(); ++i) {
        applyFovaToPart(i, name);
        // Same reason as in addPart: the camo pass replaces part.textures and
        // part.normalMaps outright, so without this a colour change wiped the
        // avatar's face for the rest of the session.
        applyAvatarLookToPart(i);
    }
    rebuildScene();
}

void CustomizeTab::rebuildWeapon()
{
    m_weaponRebuilding = true;

    // Start clean: the weapon builder owns the whole composition while it is
    // the active category, so a slot change rebuilds rather than accumulating.
    while (!m_parts.isEmpty()) removePartAt(m_parts.size() - 1);

    // Receiver first — it is the host every other part seats onto.
    // The host (receiver for a weapon, body for a character) comes from the
    // Version combo — everything else seats onto it.
    const QString fovaName =
        m_weaponCamo ? m_weaponCamo->currentData().toString() : QString();
    // Before ANY part is loaded: the avatar look decides which textures those
    // parts are decoded with.
    updateLookFromHead();
    m_lookNote.clear();
    int hostIdx = -1;
    const int hostFile = (m_weaponVersion && m_weaponVersion->count() > 0)
        ? m_weaponVersion->currentData().toInt()
        : -1;
    if (hostFile >= 0) {
        addPart(hostFile, fovaName);
        hostIdx = m_parts.isEmpty() ? -1 : 0;

    }

    QStringList seated, unseated, hostSpace;
    QSet<QString> usedCnps;          // "hostPartIndex/CNP_NAME"
    QHash<QString, int> slotPart;    // slot → index into m_parts
    for (int i = 0; i < m_weaponRows.size(); ++i) {
        WeaponSlotRow& row = m_weaponRows[i];
        row.partIdx = -1;
        row.companionPartIdx = -1;
        row.companionGearId.clear();
        row.seated = false;
        row.note.clear();
        // An appearance row carries an option index, not a file index. Reading
        // it as one would try to equip file 3 as a hat.
        if (row.isLook) continue;
        int fileIdx = row.combo->currentData().toInt();
        // Eye Shape replaces the head model the preset chose — it is the one
        // appearance option that is geometry rather than a texture.
        if (row.slot == QLatin1String("head")) {
            const int eye = lookEyeShapeFile();
            if (eye >= 0) fileIdx = eye;
        }
        // HAIR UNDER A HAT: the game's own answer to hair clipping through
        // headgear is a second MODEL, not a hidden mesh — every style ships as
        // "<style>_v00" and "<style>_v0_cov". The catalogue folds the pair into
        // one row; here is where the row decides which half is in the scene.
        // Nothing swaps on an install that ships only one of the two.
        if (row.slot == QLatin1String("hair") && fileIdx >= 0
            && headgearWorn()) {
            const int cov = coveredHairFor(fileIdx);
            if (cov >= 0) fileIdx = cov;
        }
        if (fileIdx < 0) continue;
        const int before = m_parts.size();
        addPart(fileIdx, fovaName);
        if (m_parts.size() == before) continue;   // load failed
        row.partIdx = m_parts.size() - 1;
        // THE SECOND HALF of a two-piece garment, equipped with the first and
        // never on its own. It shares the skeleton exactly as the first does,
        // so it needs no seating of its own; a load failure leaves the first
        // half standing rather than dropping the garment.
        row.companionPartIdx = -1;
        row.companionGearId.clear();
        if (m_source.partsForSubject)
            for (const fox::CatalogPart& p :
                 m_source.partsForSubject(currentSubjectId(), row.slot)) {
                if (p.modelFileIdx != fileIdx || p.companionFileIdx < 0)
                    continue;
                const int n = m_parts.size();
                addPart(p.companionFileIdx, fovaName);
                if (m_parts.size() > n) {
                    row.companionPartIdx = m_parts.size() - 1;
                    row.companionGearId = p.companionGearId;
                }
                break;
            }
        if (hostIdx < 0) continue;   // no receiver: parts sit at the origin
        // Seat it on the part the connect point actually belongs to — the
        // barrel for muzzles and foregrips, the muzzle for a suppressor — and
        // never twice on the same point. `slotPart` is filled as we go, and
        // the slot order puts every host before its dependants.
        bool ok = false;
        // Register as a possible host BEFORE trying to seat: a barrel that
        // failed to find its own point is still where the muzzle belongs.
        slotPart.insert(row.slot, row.partIdx);
        const QVector<fox::AttachOption> plan = m_source.attachPlanFor
            ? m_source.attachPlanFor(fox::EquipCatalog::baseSlot(row.slot))
            : QVector<fox::AttachOption>();
        if (plan.isEmpty()) continue;   // shares the skeleton instead (characters)
        for (const fox::AttachOption& opt : plan) {
            const int host = opt.hostSlot.isEmpty()
                ? hostIdx
                : slotPart.value(opt.hostSlot, -1);
            if (host < 0) continue;   // that host is not fitted
            const QString taken = QStringLiteral("%1/%2").arg(host).arg(opt.cnp);
            if (usedCnps.contains(taken)) continue;
            if (!attachPartTo(row.partIdx, host, opt.cnp)) continue;
            ok = true;
            usedCnps.insert(taken);
            row.seated = true;
            row.note = opt.hostSlot.isEmpty()
                ? QStringLiteral("seated at %1").arg(opt.cnp)
                : QStringLiteral("seated at %1 on the %2")
                      .arg(opt.cnp, opt.hostSlot);
            seated << QStringLiteral("%1→%2%3")
                          .arg(row.slot, opt.cnp,
                               opt.hostSlot.isEmpty()
                                   ? QString()
                                   : QStringLiteral(" (on %1)").arg(opt.hostSlot));
            break;
        }
        // A part with no connect points of its own is authored in the body's
        // own space and belongs exactly where it already is — the helicopter's
        // door armour has no .fcnp and sits correctly with no transform at
        // all. Calling that "no matching connect point" read as a failure when
        // nothing had failed.
        if (!ok) {
            if (row.partIdx >= 0 && row.partIdx < m_parts.size()
                && !m_parts[row.partIdx].hasFcnp) {
                hostSpace << row.slot;
                row.seated = true;   // where it already is IS the right place
                row.note = QStringLiteral(
                    "in the body's own space — this part carries no connect "
                    "points, so it needs no placing");
            } else {
                unseated << row.slot;
                row.note = QStringLiteral(
                    "fitted, but this %1 offers no connect point for it — the "
                    "part is sitting at the origin")
                    .arg(m_source.variantLabel.toLower());
            }
        }
    }

    // THE LOOK IS RE-APPLIED ONCE THE BUILD IS COMPLETE, and it has to be.
    // applyAvatarLookToPart runs inside addPart, so a part is asked what it
    // should look like at the moment it joins the scene — before the parts
    // that come after it exist. That is fine for anything decided by the part
    // itself and wrong for anything decided by the WHOLE build: the bandanna
    // welded to a head is switched on by an item equipped in a later row, so
    // the head was always asked before the answer could be known and the
    // headband never came back.
    //
    // Cheap enough to do unconditionally: it is one look pass over at most a
    // dozen parts, which is what reloadPartMaps already costs, and doing it
    // conditionally would mean keeping a second copy of the rule for what
    // "the build changed" means.
    if (m_lookActive)
        for (int i = 0; i < m_parts.size(); ++i) applyAvatarLookToPart(i);

    m_weaponRebuilding = false;
    // The closed combos show what is fitted, so refresh them once the build is
    // settled rather than on every intermediate selection.
    if (m_weaponVersion) m_weaponVersion->refreshCurrentIcon();
    if (m_weaponPick) m_weaponPick->refreshCurrentIcon();
    for (const WeaponSlotRow& r : m_weaponRows)
        // An unusable slot already carries the red cross; refreshing it would
        // wipe that, because the row has no part and so no icon stem.
        if (r.combo && !r.unusable) r.combo->refreshCurrentIcon();
    refreshSlotLabels();
    // A skeleton-only base is loaded for its bones and its animation, then
    // hidden: it is the naked body the game poses, and everything the player
    // actually wears is stacked on top of it, so drawing it just clips through
    // all of them. Set AFTER the rebuild — rebuildScene() clears group state.
    //
    // One caveat the data forces: the base body carries the character's plain
    // HEAD as well as its body, and Survive ships head presets for the female
    // avatar only. Hiding the base with no head part fitted leaves a headless
    // character, so the base stays visible until something replaces its head —
    // which for the female is true from the moment she is picked.
    bool headFitted = false;
    for (const WeaponSlotRow& r : m_weaponRows) {
        if (r.slot != QLatin1String("head") || !r.combo) continue;
        const QVariant pv = r.combo->currentIndex() >= 0 ? r.combo->currentData()
                                                         : QVariant();
        headFitted = pv.isValid() && pv.toInt() >= 0;
        break;
    }
    const bool baseIsSkeleton = m_source.variantHidden
        && m_source.variantHidden(currentSubjectId());
    // Both decisions name the base part explicitly, and neither is set unless
    // this build actually HAS a base part.
    // Kept for its head only: the base body is an untextured grey mannequin —
    // it has no skin map at all, which is why it read as a character with
    // "missing textures" — and every square inch of it that the clothing also
    // covers z-fights with the garment. So when it is only there for the head,
    // that is all that gets drawn.
    m_headOnlyPart = (baseIsSkeleton && !headFitted && hostIdx >= 0) ? hostIdx : -1;
    m_hideBasePart = (baseIsSkeleton && headFitted && hostIdx >= 0) ? hostIdx : -1;

    rebuildScene();

    QString msg;
    if (hostIdx < 0) {
        msg = QStringLiteral("Pick a %1 to start building.")
                  .arg(m_source.subjectLabel.toLower());
    } else {
        msg = QStringLiteral("%1 part(s)").arg(m_parts.size());
        if (!seated.isEmpty())
            msg += QStringLiteral(" · seated: %1").arg(seated.join(QStringLiteral(", ")));
        if (!hostSpace.isEmpty())
            msg += QStringLiteral(" · in the body's own space (no connect "
                                  "points of their own): %1")
                       .arg(hostSpace.join(QStringLiteral(", ")));
        if (!unseated.isEmpty())
            msg += QStringLiteral(" · no matching connect point on this %1 "
                                  "for: %2")
                       .arg(m_source.variantLabel.toLower(),
                            unseated.join(QStringLiteral(", ")));
    }
    if (m_lookActive && !m_lookNote.isEmpty())
        msg += QStringLiteral("  ·  face: ") + m_lookNote;
    if (m_headOnlyPart >= 0)
        msg += QStringLiteral("  ·  head from the base body: this install ships "
                              "no head model for this character, so the base "
                              "body's plain head is drawn on its own (it is an "
                              "untextured placeholder)");
    if (m_weaponInfo) m_weaponInfo->setText(msg);
}

QString CustomizeTab::buildWeaponFromSpec(const QString& spec, const QString& camo)
{
    return buildFromSpec(1, spec, camo);
}

QString CustomizeTab::buildFromSpec(int categoryIndex, const QString& spec,
                                    const QString& camo)
{
    if (!m_category) return QStringLiteral("builder UI not built");
    m_category->setCurrentIndex(categoryIndex);
    setBuilderCategory(categoryIndex);
    // A contextual category has no rows until a subject is chosen — that is
    // the whole point of it — so emptiness here is only a failure when the
    // category is a fixed-slot one.
    if (m_weaponRows.isEmpty()
        && !(m_source.contextual() && !m_source.subjects.isEmpty()))
        return QStringLiteral("no %1 slots in the indexed data")
            .arg(m_source.subjectLabel.isEmpty() ? QStringLiteral("builder")
                                                 : m_source.subjectLabel.toLower());

    // "hg07_main4,sight=st08" — the first field is the receiver, OR a family
    // id ("ar02"), which takes the weapon-first path the UI uses.
    const QStringList fields = spec.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (fields.isEmpty()) return QStringLiteral("empty spec");

    // Resolve the first field BEFORE the re-entrancy guard goes up: selecting a
    // subject has to fire onWeaponPicked() to populate the version combo, and
    // that handler is (correctly) a no-op while the guard is set.
    QString familyUsed;
    bool hostResolved = false;
    if (!fields[0].contains(QLatin1Char('='))) {
        const QString first = fields[0].trimmed();
        for (int i = 1; i < m_weaponPick->count(); ++i) {
            if (m_weaponPick->itemData(i).toString() != first) continue;
            m_weaponPick->setCurrentIndex(i);
            familyUsed = first;
            hostResolved = true;
            break;
        }
        // Not a subject id — treat it as the name of a host variant (a
        // receiver / body model) and find the subject that owns it. Resolved
        // against the CATALOGUE, not by walking the combo: stepping the base
        // list rebuilt the whole scene once per row tried.
        if (!hostResolved) {
            const QString needle = first.toLower();
            const auto& files = ArchiveIndex::instance().files();
            int hit = -1;
            for (const fox::CatalogSubject& f : m_source.subjects) {
                for (const fox::CatalogPart& r : f.variants) {
                    if (r.modelFileIdx < 0 || r.modelFileIdx >= files.size()) continue;
                    const QString hay =
                        (r.displayName + QLatin1Char(' ')
                         + files[r.modelFileIdx].path).toLower();
                    if (!hay.contains(needle)) continue;
                    hit = r.modelFileIdx;
                    break;
                }
                if (hit >= 0) break;
            }
            if (hit >= 0) {
                m_weaponRebuilding = true;
                hostResolved = selectHostVariant(hit);
                const int tier = currentTierPreset();
                if (hostResolved && tier >= 0) applyBuildParts(tier);
                m_weaponRebuilding = false;
                if (hostResolved) {
                    familyUsed = m_weaponPick->currentText();
                    // A star tier arrives with its whole build fitted; a plain
                    // subject still needs the defaults onWeaponPicked() would
                    // have applied, or its own head/barrel never load.
                    if (tier < 0)
                        applyFamilyDefaults(m_weaponPick->currentData().toString());
                    refreshSlotItems();
                }
            }
        }
    }

    // Match against the item's full haystack (name + file name + path), not the
    // displayed headline: the headline is tidied for reading ("hg07 main4"),
    // so a spec written with the asset's real name would never match it.
    const auto pick = [](SearchableCombo* c, const QString& filter) -> bool {
        for (int i = 1; i < c->count(); ++i)
            if (c->itemData(i, Qt::UserRole + 199).toString().contains(
                    filter.toLower())) {
                c->setCurrentIndex(i);
                return true;
            }
        return false;
    };

    QStringList missed;
    m_weaponRebuilding = true;
    for (int f = 0; f < fields.size(); ++f) {
        const QString field = fields[f].trimmed();
        if (f == 0 && !field.contains(QLatin1Char('='))) {
            if (!hostResolved) missed << field;
            continue;   // handled before the guard
        }
        QString slot, filter = field;
        if (f > 0 || field.contains(QLatin1Char('='))) {
            const int eq = field.indexOf(QLatin1Char('='));
            if (eq <= 0) { missed << field; continue; }
            slot = field.left(eq).trimmed();
            filter = field.mid(eq + 1).trimmed();
        }
        // "mgocolor:<slot>=<primaryId>+<secondaryId>" — a dye choice, so the
        // harness can exercise the per-item colour path. '+' between the two
        // ids, not the preset blob's ',': the spec grammar splits its fields
        // on commas before this code ever sees one. Either half may be left
        // empty ("+teb_c02" dyes only the secondary channel).
        if (slot.startsWith(QLatin1String("mgocolor:"))) {
            // "mgocolor:<slot>=…" dyes the garment; "mgocolor:<slot>:vest=…"
            // dyes the second model of a two-piece one. The companion's key
            // carries a control character no command line can type, so without
            // this spelling those two rows had no way in from the harness.
            QString gearSlot = slot.mid(9);
            if (gearSlot.endsWith(QLatin1String(":vest"))) {
                gearSlot.chop(5);
                gearSlot = companionColourKey(gearSlot);
            }
            const QString pri = filter.section(QLatin1Char('+'), 0, 0);
            const QString sec = filter.section(QLatin1Char('+'), 1, 1);
            if (!gearSlot.isEmpty() && (!pri.isEmpty() || !sec.isEmpty())) {
                m_mgoColours.insert(gearSlot, {pri, sec});
                continue;
            }
            missed << field;
            continue;
        }
        bool done = false;
        for (WeaponSlotRow& r : m_weaponRows) {
            if (r.slot.compare(slot, Qt::CaseInsensitive) != 0) continue;
            // "#12" selects the game's twelfth numbered preset. Twenty-eight
            // faces over seven head models cannot be addressed by model stem —
            // several of them share one — so the harness needs a way to name
            // the preset itself.
            if (filter.startsWith(QLatin1Char('#')) && r.combo) {
                const int wantPreset = filter.mid(1).toInt() - 1;
                for (int i = 0; i < r.combo->count(); ++i) {
                    const QVariant pv =
                        r.combo->itemData(i, richcombo::PresetRole);
                    if (pv.isValid() && pv.toInt() == wantPreset) {
                        r.combo->setCurrentIndex(i);
                        done = true;
                        break;
                    }
                }
                break;
            }
            done = pick(r.combo, filter);
            // The spec path mirrors the interactive one, Exclude rule
            // included: a spec that equips a suit body clears the hats the
            // game would clear, in the order the fields were given, so a
            // harness run lands on the state a user's clicks would. (The
            // guard is up, so the row handler will not run this itself.)
            if (done)
                applyGearExcludes(int(&r - m_weaponRows.data()));
            break;
        }
        if (!done) missed << field;
    }
    m_weaponRebuilding = false;

    refreshWeaponCamoList();
    if (!camo.isEmpty() && m_weaponCamo) {
        bool found = false;
        // Match the row's full haystack (name + raw token + path), not the
        // headline: the headline is now the camouflage's in-game NAME and the
        // asset token it came from sits on the second line. Captions are
        // skipped — they carry no payload and "camo" would hit one first.
        for (int i = 1; i < m_weaponCamo->count(); ++i) {
            if (m_weaponCamo->itemData(i, richcombo::HeaderRole).toBool()) continue;
            if (!m_weaponCamo->itemData(i, Qt::UserRole + 199)
                     .toString().contains(camo.toLower()))
                continue;
            m_weaponCamo->setCurrentIndex(i);
            found = true;
            break;
        }
        if (!found) missed << QStringLiteral("camo=") + camo;
    }
    // The spec path sets the combos with signals blocked, so the interactive
    // side-effects never ran. A face preset carries its hair with it either
    // way — otherwise a build made from a spec would not match the same build
    // made by hand.
    //
    // It runs with `reset` FALSE, and that is the whole point: the interactive
    // version puts every appearance row back to "from face preset", which is
    // right when a person picks a new face and wrong here, because the field
    // loop above has already applied this spec's own look:* fields. Resetting
    // now would wipe them and report success — the spec path would silently
    // stop reproducing the UI path it exists to reproduce.
    for (int i = 0; i < m_weaponRows.size(); ++i)
        if (m_weaponRows[i].slot == QLatin1String("head"))
            applyFacePresetSideEffects(i, false);
    // The colour rows follow the items the field loop just selected, and
    // display any mgocolor: choices it recorded.
    fillGearColourRows();
    rebuildWeapon();

    QString out = QStringLiteral("%1 slot(s), %2 part(s) built")
                      .arg(m_weaponRows.size())
                      .arg(m_parts.size());
    if (!familyUsed.isEmpty())
        out += QStringLiteral(", base=%1 tier=%2")
                   .arg(familyUsed, m_weaponVersion->currentText());
    if (m_weaponCamo && m_weaponCamo->currentIndex() > 0)
        out += QStringLiteral(", camo=%1")
                   .arg(m_weaponCamo->currentData().toString());
    if (!missed.isEmpty())
        out += QStringLiteral(", NOT matched: %1").arg(missed.join(QStringLiteral(" ")));
    if (m_weaponInfo) out += QStringLiteral(" | ") + m_weaponInfo->text();
    return out;
}

// ── Weapon / version / preset ───────────────────────────────────────────────

void CustomizeTab::refreshWeaponList()
{
    if (!m_weaponPick) return;
    const bool blocked = m_weaponPick->blockSignals(true);
    m_weaponPick->clear();
    m_weaponPick->addPlainItem(QStringLiteral("— none —"), QString());

    // The weapons the customize screen lists: the game's own named weapons,
    // grouped by class in the screen's order, each one selecting a star tier
    // below. Payload "n:<index into namedWeapons()>".
    const fox::EquipCatalog& eq = fox::EquipCatalog::instance();
    const bool named = m_builderCategory == 1 && !eq.namedWeapons().isEmpty();
    if (named) {
        const QVector<fox::EquipCatalog::NamedWeapon>& nw = eq.namedWeapons();
        QString lastClass;
        for (int i = 0; i < nw.size(); ++i) {
            if (nw[i].className != lastClass) {
                m_weaponPick->addHeaderItem(nw[i].className.toUpper());
                lastClass = nw[i].className;
            }
            const int tiers = nw[i].tiers.size();
            QString sub = QStringLiteral("%1 · %2 tier%3")
                              .arg(nw[i].className)
                              .arg(tiers)
                              .arg(tiers == 1 ? QString() : QStringLiteral("s"));
            QString stem, path;
            if (!nw[i].tiers.isEmpty()) {
                const fox::WeaponPreset& base = eq.presets()[nw[i].tiers.first().second];
                stem = base.stemFor(m_source.slotNames.value(0));
                const int fi = fileIdxForStem(stem);
                if (fi >= 0) path = ArchiveIndex::instance().files()[fi].path;
            }
            if (!stem.isEmpty()) sub += QStringLiteral(" · ") + stem;
            m_weaponPick->addRichItem(nw[i].name, sub, path,
                                      QStringLiteral("n:%1").arg(i), stem);
        }
        // Everything the development list never named still has to be
        // reachable: 183 receivers of the 475 in the packs carry a build, and
        // a browser that could not open the other 292 would be worse than the
        // game, not better.
        m_weaponPick->addHeaderItem(QStringLiteral("ALL RECEIVERS IN THE GAME DATA"));
    }

    QString lastClass;
    for (const fox::CatalogSubject& f : m_source.subjects) {
        // A separator row per group turns dozens of ids into a browsable list.
        if (f.groupName != lastClass) {
            m_weaponPick->addHeaderItem(f.groupName.toUpper());
            lastClass = f.groupName;
        }
        // A uniform has a real name ("BATTLE DRESS"); a weapon family does
        // not, so it keeps the class-and-id label.
        // A subject with a real name shows it; a uniform has one in the
        // development list; a weapon family has none, so it keeps the
        // class-and-id label.
        const QString suit = eq.suitName(f.id);
        const QString label = !f.displayName.isEmpty()
            ? f.displayName
            : (suit.isEmpty() ? QStringLiteral("%1 · %2").arg(f.groupName, f.id)
                              : suit);
        // A subject has no icon of its own; the game shows the thing itself,
        // so use its first variant's icon (the receiver, the body).
        // The GROUP IS THE HEADER above; repeating it on every row cost the
        // second line to a caption the reader has already had, and with
        // forty-seven characters in the list that is forty-seven copies of
        // "MGSV: The Phantom Pain — other characters (slots from model
        // names)". The id and the count are what the row alone can say.
        m_weaponPick->addRichItem(
            label,
            !f.displayName.isEmpty() || !suit.isEmpty()
                ? QStringLiteral("%1 — %2").arg(f.id, subjectSubtitle(f))
                : subjectSubtitle(f),
            QString(), f.id,
            f.variants.isEmpty() ? QString() : f.variants.first().displayName);
    }
    m_weaponPick->blockSignals(blocked);

    // The customize screen's own words when its data is there, the asset words
    // when it is not (Ground Zeroes and Survive ship no development tables, so
    // the list is receivers and there are no tiers to name).
    if (m_weaponPickLabel)
        m_weaponPickLabel->setText(named ? QStringLiteral("Base")
                                         : m_source.subjectLabel);
    if (m_weaponVersionLabel)
        m_weaponVersionLabel->setText(named ? QStringLiteral("Tier")
                                            : m_source.variantLabel);
    if (!named) {
        // Ground Zeroes and Survive ship no development tables, and the other
        // categories are not weapons at all: put the asset-side wording back,
        // or a visit to the Weapon page leaves star-tier text on the Character
        // page for the rest of the session.
        m_weaponPick->setToolTip(QStringLiteral(
            "Discovered from the game data and grouped by class. Picking one "
            "fills every slot that has a part of the same family.\n"
            "Type with the list open to search — it matches the name, the file "
            "name and the path."));
        m_weaponVersion->setToolTip(
            QStringLiteral("The %1 variants this %2 ships in the indexed data.")
                .arg(m_source.variantLabel.toLower(),
                     m_source.subjectLabel.toLower()));
    } else {
        m_weaponPick->setToolTip(QStringLiteral(
            "The weapons the game develops, by class and in the customize "
            "screen's own order — %1 of them, read out of the development "
            "tables. Below them, every receiver in the indexed data, including "
            "the ones the game never names.\n"
            "Type with the list open to search — it matches the name, the class "
            "and the model stem.").arg(eq.namedWeapons().size()));
        m_weaponVersion->setToolTip(QStringLiteral(
            "The star tiers of this weapon — the grades the development list "
            "ships it at. A tier is a WHOLE BUILD: choosing one re-fits every "
            "part, exactly as developing that grade does in game. The absolute "
            "grade is shown beside the stars because it runs to 11 across the "
            "tree and one weapon's grades can skip."));
    }
}

// Model stem → the file index the game would actually load. Built ONCE per
// index and cached: a full install is hundreds of thousands of files, the base
// list resolves 141 stems and a tier list up to eight more, and a scan apiece
// was seconds of freeze every time the category combo moved.
//
// Shadowed copies lose. A stem that only exists shadowed still resolves — a
// browser has to be able to show what is in the archives — but a live copy
// always wins, which is what every other producer of file indices in this
// panel (WeaponCatalog, EquipCatalog) already does.
const QHash<QString, int>& CustomizeTab::stemIndex() const
{
    const auto& files = ArchiveIndex::instance().files();
    if (m_stemIndexKey == files.constData() && m_stemIndexCount == files.size())
        return m_stemIndex;
    m_stemIndex.clear();
    m_stemIndex.reserve(files.size() / 4 + 16);
    for (int i = 0; i < files.size(); ++i) {
        const fox::IndexedFile& f = files[i];
        if (!f.path.endsWith(QLatin1String(".fmdl"), Qt::CaseInsensitive)) continue;
        const QString stem =
            f.path.section(QLatin1Char('/'), -1).section(QLatin1Char('.'), 0, 0);
        const auto it = m_stemIndex.constFind(stem);
        if (it == m_stemIndex.constEnd()) { m_stemIndex.insert(stem, i); continue; }
        // Already have one: replace it only if the incumbent is shadowed and
        // this one is not.
        if (f.shadowed) continue;
        if (files[it.value()].shadowed) m_stemIndex.insert(stem, i);
    }
    m_stemIndexKey = files.constData();
    m_stemIndexCount = files.size();
    return m_stemIndex;
}

QString CustomizeTab::pathForStem(const QString& stem) const
{
    const int i = fileIdxForStem(stem);
    const auto& files = ArchiveIndex::instance().files();
    return (i >= 0 && i < files.size()) ? files[i].path : QString();
}

int CustomizeTab::fileIdxForStem(const QString& stem) const
{
    if (stem.isEmpty()) return -1;
    return stemIndex().value(stem, -1);
}

// Which of the game's builds the selected version row IS, or -1 when the row
// is a plain asset variant.
int CustomizeTab::currentTierPreset() const
{
    if (!m_weaponVersion || m_weaponVersion->currentIndex() < 0) return -1;
    const QVariant v =
        m_weaponVersion->itemData(m_weaponVersion->currentIndex(), kPresetIdxRole);
    return v.isValid() ? v.toInt() : -1;
}

// The star tiers of one named weapon, in place of the asset-variant list. The
// row's PAYLOAD stays the receiver's file index, so everything downstream —
// the camo list, the slot narrowing, the saved presets — keeps reading the
// version combo exactly as it did when the list held bare receivers.
void CustomizeTab::fillTierList(int namedIdx)
{
    const fox::EquipCatalog& eq = fox::EquipCatalog::instance();
    if (namedIdx < 0 || namedIdx >= eq.namedWeapons().size()) return;
    const fox::EquipCatalog::NamedWeapon& w = eq.namedWeapons()[namedIdx];
    const bool vb = m_weaponVersion->blockSignals(true);
    m_weaponVersion->clear();
    for (int t = 0; t < w.tiers.size(); ++t) {
        const fox::WeaponPreset& p = eq.presets()[w.tiers[t].second];
        const QString stem = p.stemFor(m_source.slotNames.value(0));
        const int fileIdx = fileIdxForStem(stem);
        const QString path = fileIdx >= 0
            ? ArchiveIndex::instance().files()[fileIdx].path : QString();
        const int fitted = qMax(0, int(p.parts.size()) - 1);
        m_weaponVersion->addRichItem(
            QStringLiteral("%1  Grade %2")
                .arg(tierStars(t + 1, w.tiers.size()))
                .arg(p.grade),
            QStringLiteral("%1 · %2 part%3 fitted")
                .arg(stem.isEmpty() ? QStringLiteral("no receiver") : stem)
                .arg(fitted)
                .arg(fitted == 1 ? QString() : QStringLiteral("s")),
            path, fileIdx, stem);
        m_weaponVersion->setItemData(m_weaponVersion->count() - 1,
                                     w.tiers[t].second, kPresetIdxRole);
    }
    m_weaponVersion->setEnabled(m_weaponVersion->count() > 1);
    m_weaponVersion->blockSignals(vb);
}

void CustomizeTab::onWeaponPicked()
{
    if (m_weaponRebuilding) return;
    const QString key = m_weaponPick->currentData().toString();

    // A named weapon: the tier list, and the lowest tier's build applied — the
    // customize screen opens a weapon at its base grade, not empty.
    if (key.startsWith(QLatin1String("n:"))) {
        const int idx = key.mid(2).toInt();
        fillTierList(idx);
        m_weaponRebuilding = true;
        const QVariant pv0 = m_weaponVersion->count() > 0
            ? m_weaponVersion->itemData(0, kPresetIdxRole)
            : QVariant();
        const int preset = pv0.isValid() ? pv0.toInt() : -1;
        if (m_weaponVersion->count() > 0) {
            const bool b = m_weaponVersion->blockSignals(true);
            m_weaponVersion->setCurrentIndex(0);
            m_weaponVersion->blockSignals(b);
        }
        const QStringList missing = applyBuildParts(preset);
        m_weaponRebuilding = false;
        m_weaponVersion->refreshCurrentIcon();
        refreshWeaponCamoList();
        rebuildWeapon();
        reportBuild(preset, missing);
        return;
    }

    // Versions = this family's receivers.
    fillVariantList(key);
    if (m_source.contextual()) {
        // The rows themselves belong to the subject here, so rebuild them
        // before anything tries to fill them.
        m_currentSubjectId = key;
        rebuildSlotRows();
        refreshGearRuleControl();
        if (m_weaponVersion->count() > 0) {
            const bool b = m_weaponVersion->blockSignals(true);
            m_weaponVersion->setCurrentIndex(0);
            m_weaponVersion->blockSignals(b);
        }
        // Dress the character the way the game does with nothing chosen: an
        // empty Survive survivor is a naked base body, which is a canvas
        // rather than a starting point.
        // The defaults are row 0 of each slot now, so simply rebuilding the
        // lists dresses the character — and "Clear slots" puts it back.
        refreshSlotItems();
        // Variations are authored per model: rebuildWeapon() below hands the
        // current variation name to every part it loads, so the previous
        // character's camo must be off the list before it can be carried over.
        refreshWeaponCamoList();
        if (m_weaponInfo && m_source.subjectNote) {
            const QString note = m_source.subjectNote(key);
            if (!note.isEmpty()) m_weaponInfo->setText(note);
        }
        rebuildWeapon();
        return;
    }
    applyFamilyDefaults(key);
}

// The subject the panel is currently on — the payload of the base combo, with
// the named-weapon prefix stripped. Contextual categories key everything on it.
QString CustomizeTab::currentSubjectId() const
{
    if (!m_weaponPick) return m_currentSubjectId;
    const QString key = m_weaponPick->currentData().toString();
    return key.startsWith(QLatin1String("n:")) ? m_currentSubjectId : key;
}

// The bare asset variants of one subject — the receivers of a weapon family,
// the bodies of a character. Signals stay blocked: the caller decides what
// happens next.
void CustomizeTab::fillVariantList(const QString& familyId)
{
    if (!m_weaponVersion) return;
    const bool vb = m_weaponVersion->blockSignals(true);
    m_weaponVersion->clear();
    for (const fox::CatalogSubject& f : m_source.subjects) {
        if (f.id != familyId) continue;
        for (const fox::CatalogPart& r : f.variants) {
            const QString path = r.modelFileIdx >= 0
                    && r.modelFileIdx < ArchiveIndex::instance().files().size()
                ? ArchiveIndex::instance().files()[r.modelFileIdx].path
                : QString();
            m_weaponVersion->addRichItem(displayNameFor(r.displayName),
                                         path.section(QLatin1Char('/'), -1), path,
                                         r.modelFileIdx, r.displayName);
        }
        break;
    }
    m_weaponVersion->setEnabled(m_weaponVersion->count() > 1);
    m_weaponVersion->blockSignals(vb);
}

// Put the base and variant combos on the model with this file index, WITHOUT
// firing either handler — for the paths that are restoring a saved state and
// will fit the parts themselves. Walking the combo and letting the handlers
// run was both wrong (the guard those callers hold makes onWeaponPicked() a
// no-op, so the variant list was never refilled and the host was found only
// when it already happened to be listed) and slow (a full scene rebuild per
// row tried, now that the base list is 141 weapons plus every family).
bool CustomizeTab::selectHostVariant(int fileIdx)
{
    if (fileIdx < 0 || !m_weaponPick || !m_weaponVersion) return false;
    const auto& files = ArchiveIndex::instance().files();
    if (fileIdx >= files.size()) return false;
    // Failure has to leave the panel where it found it. The named branch moves
    // the base combo and refills the tier list BEFORE it can know whether the
    // variant is in there, and a caller that is told "not matched" then went on
    // to build whatever the half-finished selection happened to be.
    const QString wasBase = m_weaponPick->currentData().toString();
    const int wasVersion = m_weaponVersion->currentIndex();
    const auto restore = [&] {
        const bool pb = m_weaponPick->blockSignals(true);
        if (!wasBase.isEmpty()) m_weaponPick->selectPayload(wasBase);
        m_weaponPick->blockSignals(pb);
        if (wasBase.startsWith(QLatin1String("n:")))
            fillTierList(wasBase.mid(2).toInt());
        else
            fillVariantList(wasBase);
        const bool b = m_weaponVersion->blockSignals(true);
        if (wasVersion >= 0 && wasVersion < m_weaponVersion->count())
            m_weaponVersion->setCurrentIndex(wasVersion);
        m_weaponVersion->blockSignals(b);
    };
    const QString stem = files[fileIdx].path.section(QLatin1Char('/'), -1)
                             .section(QLatin1Char('.'), 0, 0);
    const QString hostSlot = m_source.slotNames.value(0);
    const fox::EquipCatalog& eq = fox::EquipCatalog::instance();

    // A named weapon that ships this receiver: the customize screen's own row.
    if (m_builderCategory == 1 && !stem.isEmpty()) {
        const QVector<fox::EquipCatalog::NamedWeapon>& nw = eq.namedWeapons();
        for (int n = 0; n < nw.size(); ++n) {
            bool mine = false;
            for (const QPair<int, int>& t : nw[n].tiers)
                if (eq.presets()[t.second].stemFor(hostSlot) == stem) { mine = true; break; }
            if (!mine) continue;
            const bool pb = m_weaponPick->blockSignals(true);
            const bool ok = m_weaponPick->selectPayload(QStringLiteral("n:%1").arg(n));
            m_weaponPick->blockSignals(pb);
            if (!ok) break;
            fillTierList(n);
            const bool b = m_weaponVersion->blockSignals(true);
            const bool got = m_weaponVersion->selectPayload(fileIdx);
            m_weaponVersion->blockSignals(b);
            if (got) return true;
            restore();
            break;   // named row but no tier row for it — try the family list
        }
    }
    // Otherwise the subject that owns this variant.
    for (const fox::CatalogSubject& f : m_source.subjects) {
        bool mine = false;
        for (const fox::CatalogPart& r : f.variants)
            if (r.modelFileIdx == fileIdx) { mine = true; break; }
        if (!mine) continue;
        const bool pb = m_weaponPick->blockSignals(true);
        m_weaponPick->selectPayload(f.id);
        m_weaponPick->blockSignals(pb);
        fillVariantList(f.id);
        // Signals are blocked here on purpose, so onWeaponPicked() will not
        // run — but in a contextual category the ROWS belong to the subject,
        // and leaving the previous character's rows in place makes every part
        // the caller is about to restore land in a slot that no longer exists.
        if (m_source.contextual() && m_currentSubjectId != f.id) {
            m_currentSubjectId = f.id;
            rebuildSlotRows();
            refreshGearRuleControl();
        }
        const bool b = m_weaponVersion->blockSignals(true);
        const bool got = m_weaponVersion->selectPayload(fileIdx);
        m_weaponVersion->blockSignals(b);
        if (!got) restore();
        return got;
    }
    return false;
}

void CustomizeTab::onWeaponVersionChanged()
{
    if (m_weaponVersion) m_weaponVersion->refreshCurrentIcon();
    if (m_weaponRebuilding) return;
    // A star tier carries a whole build, not just a receiver: changing tier on
    // the customize screen re-fits every part, which is the entire point of the
    // tiers. A plain asset variant swaps only the body and keeps what is
    // fitted — selecting a different version of a gun should not silently throw
    // away attachments you chose yourself.
    // An ABSENT role reads back as an invalid QVariant, and QVariant::toInt()
    // turns that into 0 — which is a perfectly good preset index. Test the
    // variant, never the integer.
    const int preset = currentTierPreset();
    if (preset >= 0) {
        m_weaponRebuilding = true;
        const QStringList missing = applyBuildParts(preset);
        m_weaponRebuilding = false;
        refreshWeaponCamoList();
        rebuildWeapon();
        reportBuild(preset, missing);
        return;
    }
    // Variations are authored per model, so the camo list belongs to whichever
    // receiver is selected.
    refreshWeaponCamoList();
    refreshSlotItems();
    rebuildWeapon();
}

void CustomizeTab::applyFamilyDefaults(const QString& familyId)
{
    // "The default version of the weapon with all default parts selected":
    // the family's first receiver, plus — for every other slot — the part that
    // belongs to THIS weapon rather than a generic one. Most weapons only own
    // parts for one or two slots (measured across the pack tree), so the rest
    // are cleared instead of being filled with something arbitrary.
    m_weaponRebuilding = true;
    m_pendingKeep.clear();
    if (!familyId.isEmpty())
        for (const WeaponSlotRow& r : m_weaponRows)
            if (const fox::CatalogPart* p = m_source.ownPartFor(familyId, r.slot))
                m_pendingKeep.insert(r.slot, p->modelFileIdx);
    refreshSlotItems();
    m_pendingKeep.clear();
    for (WeaponSlotRow& row : m_weaponRows) {
        if (!row.combo) continue;
        const bool b = row.combo->blockSignals(true);
        int want = -1;
        if (familyId.isEmpty()) {
            want = -1;
        } else if (const fox::CatalogPart* p = m_source.ownPartFor(familyId, row.slot)) {
            want = p->modelFileIdx;
        }
        row.combo->setCurrentIndex(0);
        if (want >= 0)
            for (int i = 1; i < row.combo->count(); ++i)
                if (row.combo->itemData(i).toInt() == want) { row.combo->setCurrentIndex(i); break; }
        row.combo->blockSignals(b);
    }
    m_weaponRebuilding = false;
    refreshWeaponCamoList();
    rebuildWeapon();
}

QString CustomizeTab::currentStemFor(const QString& slot) const
{
    const auto& files = ArchiveIndex::instance().files();
    const auto stemOf = [&](int idx) {
        return (idx >= 0 && idx < files.size())
            ? files[idx].path.section(QLatin1Char('/'), -1).section(QLatin1Char('.'), 0, 0)
            : QString();
    };
    // An empty combo's currentData() is an INVALID QVariant, and toInt() turns
    // that into 0 — a perfectly good file index. Reading it as one made the
    // first file in the archive stand in for whatever was not selected yet.
    const auto payloadOf = [](const QComboBox* c) {
        if (!c || c->currentIndex() < 0) return -1;
        const QVariant v = c->currentData();
        return v.isValid() ? v.toInt() : -1;
    };
    if (slot == m_source.slotNames.value(0)) {
        if (!m_weaponVersion || m_weaponVersion->count() == 0) return {};
        return stemOf(payloadOf(m_weaponVersion));
    }
    for (const WeaponSlotRow& r : m_weaponRows)
        if (r.slot == slot && r.combo) return stemOf(payloadOf(r.combo));
    return {};
}

// Fill every slot combo from the catalogue. When the compatibility switch is
// on and the game HAS a rule for the fitted receiver, the list is narrowed to
// the parts that rule allows — but an empty rule means "no data", never "no
// parts", and whatever is already fitted always stays in the list so a filter
// can never silently unequip something.
void CustomizeTab::refreshSlotItems()
{
    if (m_weaponRows.isEmpty()) return;
    const auto& files = ArchiveIndex::instance().files();
    const fox::EquipCatalog& equip = fox::EquipCatalog::instance();
    const bool narrow = m_builderCategory == 1 && m_weaponCompat
        && m_weaponCompat->isChecked() && equip.hasCompatibility();
    const QString receiverStem = currentStemFor(m_source.slotNames.value(0));
    const QString barrelStem = currentStemFor(QStringLiteral("barrel"));

    // What this subject wears with nothing chosen, resolved once per pass.
    m_slotDefaults.clear();
    if (m_source.defaultsFor) m_slotDefaults = m_source.defaultsFor(currentSubjectId());

    int narrowed = 0, noRule = 0;
    const fox::EquipCatalog& eq = fox::EquipCatalog::instance();
    // Weapon category ONLY. The development tables are weapon loadouts: asked
    // about a character, hasBuildsFor() answers for whatever receiver the
    // weapon page left behind and crosses out every slot on the page.
    const bool judgeable = m_builderCategory == 1 && eq.hasBuildsFor(receiverStem);
    for (WeaponSlotRow& row : m_weaponRows) {
        if (!row.combo || row.isLook) continue;   // appearance rows fill elsewhere
        // A weapon the game builds, with a slot no build of it ever fills, is a
        // slot that weapon cannot take — the red cross on the customize screen.
        row.unusable = judgeable && !eq.slotEverUsedOn(receiverStem, row.slot);
        if (row.unusable) {
            const bool b = row.combo->blockSignals(true);
            row.combo->clear();
            row.combo->addPlainItem(QStringLiteral("— not usable on this weapon —"),
                                    -1);
            // Give the DISABLED mode the same pixmap: Qt desaturates a
            // disabled icon by default, and the whole point of this one is
            // that it is red.
            const QPixmap cross =
                fox::IconCatalog::instance().crossedOut(richcombo::kClosedIconHeight);
            QIcon crossIcon;
            crossIcon.addPixmap(cross, QIcon::Normal);
            crossIcon.addPixmap(cross, QIcon::Disabled);
            row.combo->setItemIcon(0, crossIcon);
            row.combo->setIconSize(QSize(richcombo::kClosedIconHeight * 9 / 5,
                                         richcombo::kClosedIconHeight));
            row.combo->setCurrentIndex(0);
            row.combo->setEnabled(false);
            row.combo->setToolTip(QStringLiteral(
                "No build the game ships on this weapon uses a %1.")
                    .arg(prettySlot(row.slot).toLower()));
            row.combo->blockSignals(b);
            continue;
        }
        row.combo->setEnabled(true);
        // A character slot's "empty" is not nothing: a survivor with no torso
        // chosen still wears the T-shirt the game starts them in. Row 0 is that
        // part, so clearing a slot returns to the default instead of cutting a
        // hole in the character.
        const int slotDefault = m_slotDefaults.value(row.slot, -1);
        const QVariant keepVar = row.combo->currentIndex() >= 0
            ? row.combo->currentData() : QVariant();
        int keep = keepVar.isValid() ? keepVar.toInt() : -1;
        // A restore path clears every combo to "— none —" BEFORE narrowing and
        // selects afterwards, so "what is fitted stays in the list" protects
        // nothing there: with the filter on, a saved build's part could simply
        // vanish from its combo and the slot would come back empty. Whoever is
        // about to select a part says so here first.
        const int pending = m_pendingKeep.value(row.slot, -1);
        if (pending >= 0 && keep < 0) keep = pending;
        const QVector<fox::CatalogPart> parts = m_source.contextual()
            ? m_source.partsForSubject(currentSubjectId(), row.slot)
            : m_source.partsFor(fox::EquipCatalog::baseSlot(row.slot));
        QSet<QString> allowed;
        if (narrow) {
            allowed = equip.compatibleStems(row.slot, receiverStem, barrelStem);
            if (allowed.isEmpty()) ++noRule;
        }
        // A slot driven by the game's own numbered grid behaves differently in
        // one place: the list must show EVERY numbered entry, even where two of
        // them happen to share a model. Dropping the duplicates is right for a
        // parts list and wrong for a preset list — it silently deleted three of
        // Survive's twenty-eight faces.
        bool presetSlot = false;
        for (const fox::CatalogPart& p : parts)
            if (p.presetIndex >= 0) { presetSlot = true; break; }
        const bool b = row.combo->blockSignals(true);
        row.combo->clear();
        if (presetSlot && !parts.isEmpty()) {
            // Row 0 is the first preset, presented as the default.
            const fox::CatalogPart& p0 = parts.first();
            const fox::AvatarPresets& ap = fox::AvatarPresets::instance();
            const QVector<fox::AvatarPreset>& tbl0 = ap.presets(
                fox::AvatarPresets::sexOfStem(p0.id));
            const QString sub =
                (p0.presetIndex >= 0 && p0.presetIndex < tbl0.size())
                    ? tbl0[p0.presetIndex].describe()
                    : QString();
            const QString path0 =
                (p0.modelFileIdx >= 0 && p0.modelFileIdx < files.size())
                    ? files[p0.modelFileIdx].path : QString();
            row.combo->addSwatchItem(
                QStringLiteral("— default — %1").arg(p0.displayName), sub, path0,
                p0.modelFileIdx,
                fox::AvatarPresets::iconPathFor(p0.presetIndex,
                                                fox::AvatarPresets::sexOfStem(
                                                    p0.id)));
            row.combo->setItemData(0, p0.presetIndex, richcombo::PresetRole);
        } else if (slotDefault >= 0) {
            const QString dpath =
                (slotDefault < files.size()) ? files[slotDefault].path : QString();
            const QString dstem =
                dpath.section(QLatin1Char('/'), -1).section(QLatin1Char('.'), 0, 0);
            // The default row is a real item too, so it gets the item's own
            // name and icon when the catalogue has them. Without this the one
            // row every MGO slot always shows was the only one still reading as
            // a file stem.
            QString dname = displayNameFor(dstem), dicon;
            for (const fox::CatalogPart& p : parts)
                if (p.modelFileIdx == slotDefault) {
                    if (!p.displayName.isEmpty())
                        dname = p.gearId.isEmpty() || p.displayName == p.gearId
                            ? displayNameFor(p.displayName)
                            : p.displayName;
                    dicon = p.gearIcon;
                    break;
                }
            if (!dicon.isEmpty())
                row.combo->addSwatchItem(
                    QStringLiteral("— default — %1").arg(dname),
                    QStringLiteral("what the game starts this character in"),
                    dpath, slotDefault, dicon);
            else
                row.combo->addRichItem(
                    QStringLiteral("— default — %1").arg(dname),
                    QStringLiteral("what the game starts this character in"),
                    dpath, slotDefault, dstem);
        } else {
            row.combo->addPlainItem(QStringLiteral("— none —"), -1);
        }
        int shown = 0;
        for (const fox::CatalogPart& p : parts) {
            const QString path = (p.modelFileIdx >= 0 && p.modelFileIdx < files.size())
                ? files[p.modelFileIdx].path
                : QString();
            if (presetSlot ? (p.presetIndex == parts.first().presetIndex)
                           : (p.modelFileIdx == slotDefault))
                continue;   // already row 0
            if (!allowed.isEmpty() && p.modelFileIdx != keep
                && !allowed.contains(p.displayName))
                continue;
            if (row.slot == QLatin1String("hair")
                && (fox::AvatarPresets::instance().ok(
                        fox::AvatarPresets::Sex::Women)
                    || fox::AvatarPresets::instance().ok(
                        fox::AvatarPresets::Sex::Men))) {
                // The AVATAR screen's own hairstyle thumbnails. The icon is
                // chosen by the STYLE LETTER in the model stem
                // (avf_hair_<a|b|c|d>0_v0_cov -> 0…3), not by the row's
                // position: a filtered or reordered list must not slide every
                // thumbnail one place along. Bald is tile 0 and the styles
                // follow it, in both genders' trees.
                const QString hstem = path.section(QLatin1Char('/'), -1);
                row.combo->addSwatchItem(
                    displayNameFor(p.displayName), hstem, path, p.modelFileIdx,
                    fox::AvatarPresets::hairIconPath(
                        hairStyleIndexOf(hstem),
                        fox::AvatarPresets::sexOfStem(hstem)));
            } else if (p.presetIndex >= 0) {
                // A preset row is the game's own grid cell: its thumbnail, its
                // number, and one line saying what it actually sets.
                const fox::AvatarPresets& ap = fox::AvatarPresets::instance();
                const QString sub =
                    p.presetIndex < ap.presets(
                        fox::AvatarPresets::sexOfStem(p.id)).size()
                        ? ap.presets(fox::AvatarPresets::sexOfStem(p.id))
                              [p.presetIndex].describe()
                        : path.section(QLatin1Char('/'), -1);
                row.combo->addSwatchItem(
                    p.displayName, sub, path, p.modelFileIdx,
                    fox::AvatarPresets::iconPathFor(
                        p.presetIndex,
                        fox::AvatarPresets::sexOfStem(p.id)));
                row.combo->setItemData(row.combo->count() - 1, p.presetIndex,
                                       richcombo::PresetRole);
            } else if (!p.gearId.isEmpty()) {
                // An MGO gear item has an icon of its own, named by
                // GearConfig.lua. It is a UI texture addressed by path, not a
                // model stem, so it goes through the swatch row — the same one
                // the colour chips use — rather than addRichItem's stem lookup,
                // which would find nothing for a gear item and draw nothing.
                // A name that came out of the language table is FINAL: it is
                // already what the player reads on the game's own screen, and
                // putting it through displayNameFor would run the stem tidier
                // over it (chop "_def", underscores to spaces). Only the
                // fallback — where displayName IS the gear id, because no
                // language table resolved it — wants tidying.
                const QString headline = p.displayName == p.gearId
                    ? displayNameFor(p.displayName)
                    : p.displayName;
                // An item with no icon of its own keeps the STEM lookup rather
                // than getting a swatch row with an empty path: addSwatchItem
                // with nothing to draw sets no icon stem either, so such a row
                // would lose the icon the old path could still find for it.
                if (!p.gearIcon.isEmpty())
                    row.combo->addSwatchItem(headline,
                                             path.section(QLatin1Char('/'), -1),
                                             path, p.modelFileIdx, p.gearIcon);
                else
                    row.combo->addRichItem(headline,
                                           path.section(QLatin1Char('/'), -1),
                                           path, p.modelFileIdx, p.displayName);
            } else {
                row.combo->addRichItem(displayNameFor(p.displayName),
                                       path.section(QLatin1Char('/'), -1), path,
                                       p.modelFileIdx, p.displayName);
            }
            ++shown;
        }
        // A filter that leaves a usable slot with nothing in it is worse than
        // no filter: the rule is about parts this install may not carry, so
        // fall back to the whole list rather than an empty combo.
        if (!allowed.isEmpty() && shown == 0 && !parts.isEmpty()) {
            for (const fox::CatalogPart& p : parts) {
                const QString path = (p.modelFileIdx >= 0 && p.modelFileIdx < files.size())
                    ? files[p.modelFileIdx].path : QString();
                row.combo->addRichItem(displayNameFor(p.displayName),
                                       path.section(QLatin1Char('/'), -1), path,
                                       p.modelFileIdx, p.displayName);
            }
            shown = parts.size();
            allowed.clear();
            ++noRule;
        }
        if (!allowed.isEmpty() && shown < parts.size()) ++narrowed;
        // Three different states, and telling them apart is the whole
        // difference between a filter that works and one that looks broken:
        // switched off, switched on with a rule, switched on with NOTHING
        // known about this weapon.
        if (!narrow)
            row.combo->setToolTip(
                QStringLiteral("%1 part(s) in this install").arg(parts.size()));
        else if (allowed.isEmpty())
            row.combo->setToolTip(
                QStringLiteral("All %1 shown. The game's data says nothing "
                               "about a %2 on this receiver — neither the "
                               "inclusion tables nor any build it ships — so "
                               "there is no rule to filter by.")
                    .arg(parts.size())
                    .arg(prettySlot(row.slot).toLower()));
        else
            row.combo->setToolTip(
                QStringLiteral("%1 of %2 part(s). The rest are not in this "
                               "receiver's inclusion list and no build the game "
                               "ships fits one here.")
                    .arg(shown).arg(parts.size()));
        if (keep >= 0) row.combo->selectPayload(keep);
        row.combo->blockSignals(b);
    }
    // Say what the switch is actually doing on THIS weapon. Four receivers in
    // a full install are unknown to both the inclusion tables and the
    // development list, and on those the honest answer is that there is
    // nothing to filter by — which should read as an answer, not as a bug.
    if (m_weaponCompat) {
        QString tip = kCompatTip;
        if (narrow) {
            tip += QStringLiteral("\n\nOn this weapon: %1 slot(s) narrowed")
                       .arg(narrowed);
            tip += noRule > 0
                ? QStringLiteral(", %1 left whole because the game's data "
                                 "carries no rule for them.").arg(noRule)
                : QStringLiteral(".");
            // Test the claim rather than inferring it from a zero: narrowed
            // is also 0 when every slot is crossed out (the loop never reaches
            // the counters) and when every rule happens to allow everything.
            if (!eq.knowsReceiver(receiverStem) && !eq.hasBuildsFor(receiverStem))
                tip += QStringLiteral("\nThis receiver is in neither the "
                                      "inclusion tables nor the development "
                                      "list, so there is nothing to filter by.");
        }
        m_weaponCompat->setToolTip(tip);
    }
    // The colour rows track the slots they sit under.
    fillGearColourRows();
}

// Fill every slot from one of the game's builds, leaving the base and the tier
// to the caller — this is the half shared by picking a star tier in the weapons
// list and picking a row in the full preset list. Assumes the rebuild guard is
// already up. Returns the stems this install does not carry: reporting them is
// how you find out a chunk is missing, rather than silently getting less gun.
QStringList CustomizeTab::applyBuildParts(int presetIndex)
{
    const fox::EquipCatalog& equip = fox::EquipCatalog::instance();
    if (presetIndex < 0 || presetIndex >= equip.presets().size()) {
        // No build to fit is still an instruction: leave the previous weapon's
        // parts on the new receiver and the scene builds a chimera.
        for (WeaponSlotRow& r : m_weaponRows)
            if (r.combo) {
                const bool b = r.combo->blockSignals(true);
                r.combo->setCurrentIndex(0);
                r.combo->blockSignals(b);
            }
        refreshSlotItems();
        return {};
    }
    const fox::WeaponPreset& preset = equip.presets()[presetIndex];
    const auto& files = ArchiveIndex::instance().files();

    // Stem → file index, in ONE pass: a full index is hundreds of thousands of
    // files and a build names up to eleven parts.
    QHash<QString, int> wanted;
    for (const auto& sp : preset.parts) wanted.insert(sp.second, -1);
    for (int i = 0; i < files.size(); ++i) {
        const QString stem =
            files[i].path.section(QLatin1Char('/'), -1).section(QLatin1Char('.'), 0, 0);
        const auto it = wanted.find(stem);
        if (it != wanted.end() && it.value() < 0
            && files[i].path.endsWith(QLatin1String(".fmdl"), Qt::CaseInsensitive))
            it.value() = i;
    }

    // Slot lists depend on the receiver, so narrow them before selecting into
    // them — otherwise a compatible part could be missing from the list.
    m_pendingKeep.clear();
    for (const WeaponSlotRow& r : m_weaponRows) {
        const QString stem = preset.stemFor(r.slot);
        if (!stem.isEmpty()) m_pendingKeep.insert(r.slot, wanted.value(stem, -1));
    }
    refreshSlotItems();
    QStringList missing;
    for (WeaponSlotRow& r : m_weaponRows) {
        if (!r.combo) continue;
        const bool b = r.combo->blockSignals(true);
        r.combo->setCurrentIndex(0);
        const QString stem = preset.stemFor(r.slot);
        if (!stem.isEmpty()) {
            const int idx = wanted.value(stem, -1);
            if (idx < 0 || !r.combo->selectPayload(idx)) missing << stem;
        }
        r.combo->blockSignals(b);
    }
    // The muzzle slots are keyed on the BARREL, which only became known in the
    // loop above — narrow once more now that it is fitted.
    refreshSlotItems();
    m_pendingKeep.clear();
    for (WeaponSlotRow& r : m_weaponRows)
        if (r.combo) r.combo->refreshCurrentIcon();
    return missing;
}

// Append which build is on screen to the info line, after rebuildWeapon() has
// written its own summary there.
void CustomizeTab::reportBuild(int presetIndex, const QStringList& missing)
{
    const fox::EquipCatalog& equip = fox::EquipCatalog::instance();
    if (!m_weaponInfo || presetIndex < 0 || presetIndex >= equip.presets().size())
        return;
    const fox::WeaponPreset& preset = equip.presets()[presetIndex];
    QString msg = m_weaponInfo->text();
    msg += QStringLiteral("  ·  %1").arg(preset.label());
    if (!preset.category.isEmpty())
        msg += QStringLiteral(" (%1)")
                   .arg(fox::EquipCatalog::classDisplayName(preset.category));
    if (!missing.isEmpty())
        msg += QStringLiteral("  ·  not in this install: %1")
                   .arg(missing.join(QStringLiteral(", ")));
    m_weaponInfo->setText(msg);
}

// One of the game's own builds, chosen from the full preset list: select the
// weapon and the tier it belongs to, then fit its parts.
void CustomizeTab::applyGamePreset(int presetIndex)
{
    const fox::EquipCatalog& equip = fox::EquipCatalog::instance();
    if (presetIndex < 0 || presetIndex >= equip.presets().size()) return;
    const fox::WeaponPreset& preset = equip.presets()[presetIndex];
    const QString hostSlot = m_source.slotNames.value(0);
    const QString receiver = preset.stemFor(hostSlot);

    m_weaponRebuilding = true;
    // Prefer the named row — that is the weapon this build IS, and landing on
    // it keeps the weapons list showing the same thing as the preset line.
    bool placed = false;
    const QVector<fox::EquipCatalog::NamedWeapon>& nw = equip.namedWeapons();
    for (int n = 0; n < nw.size() && !placed; ++n) {
        for (const QPair<int, int>& t : nw[n].tiers) {
            if (t.second != presetIndex) continue;
            if (!m_weaponPick->selectPayload(QStringLiteral("n:%1").arg(n))) break;
            fillTierList(n);
            for (int v = 0; v < m_weaponVersion->count(); ++v) {
                const QVariant pv = m_weaponVersion->itemData(v, kPresetIdxRole);
                if (!pv.isValid() || pv.toInt() != presetIndex) continue;
                const bool b = m_weaponVersion->blockSignals(true);
                m_weaponVersion->setCurrentIndex(v);
                m_weaponVersion->blockSignals(b);
                placed = true;
                break;
            }
            break;
        }
    }
    // A build whose name never resolved (3 of 435) still has a receiver, so
    // fall back to the family row and the bare variant list.
    if (!placed && !receiver.isEmpty()) {
        const QString family = receiver.section(QLatin1Char('_'), 0, 0);
        for (int i = 1; i < m_weaponPick->count(); ++i) {
            if (m_weaponPick->itemData(i).toString() != family) continue;
            m_weaponPick->setCurrentIndex(i);
            m_weaponRebuilding = false;
            onWeaponPicked();       // fills Version + the family's own parts
            m_weaponRebuilding = true;
            break;
        }
        const int hostFile = fileIdxForStem(receiver);
        if (hostFile >= 0 && m_weaponVersion) {
            const bool b = m_weaponVersion->blockSignals(true);
            m_weaponVersion->selectPayload(hostFile);
            m_weaponVersion->blockSignals(b);
        }
    }
    const QStringList missing = applyBuildParts(presetIndex);
    m_weaponRebuilding = false;

    m_weaponPick->refreshCurrentIcon();
    m_weaponVersion->refreshCurrentIcon();
    refreshWeaponCamoList();
    rebuildWeapon();
    reportBuild(presetIndex, missing);
}

// Presets are the user's own builds, stored as slot→part-PATH so they survive
// an index rebuild (the same reason saved outfits store paths, not indices).
void CustomizeTab::refreshWeaponPresets()
{
    if (!m_weaponPreset) return;
    const bool b = m_weaponPreset->blockSignals(true);
    m_weaponPreset->clear();
    m_weaponPreset->addPlainItem(QStringLiteral("— none —"), QString());

    QSettings s;
    s.beginGroup(presetGroup());
    const QStringList mine = s.childKeys();
    s.endGroup();
    if (!mine.isEmpty()) {
        m_weaponPreset->insertSeparator(m_weaponPreset->count());
        for (const QString& k : mine)
            m_weaponPreset->addRichItem(k, QStringLiteral("your saved build"),
                                        QString(), QStringLiteral("u:") + k);
    }

    // The game's own builds. Every weapon, every grade, with the exact parts
    // the development tables give it — Weapon category only; they are weapon
    // loadouts and mean nothing to a character.
    const fox::EquipCatalog& equip = fox::EquipCatalog::instance();
    if (m_builderCategory != 1) {
        m_weaponPreset->blockSignals(b);
        m_weaponPreset->setToolTip(QStringLiteral("Your saved builds."));
        return;
    }
    const QVector<fox::WeaponPreset>& all = equip.presets();
    // Several builds can share a name AND a grade — the game ships more than
    // one loadout at the same tier (they differ in the parts, which the second
    // line shows). Tag those with the weapon id so the list has no two rows a
    // person cannot tell apart.
    QHash<QString, int> labelUses;
    for (const fox::WeaponPreset& p : all) ++labelUses[p.label()];
    QString lastName;
    for (int i = 0; i < all.size(); ++i) {
        const fox::WeaponPreset& p = all[i];
        if (p.name != lastName) {
            m_weaponPreset->insertSeparator(m_weaponPreset->count());
            lastName = p.name;
        }
        QStringList summary;
        for (const auto& sp : p.parts)
            if (sp.first != m_source.slotNames.value(0)) summary << sp.second;
        const QString label = labelUses.value(p.label()) > 1
            ? QStringLiteral("%1 · %2").arg(p.label(), p.wpId)
            : p.label();
        m_weaponPreset->addRichItem(
            label,
            p.category.isEmpty() ? QStringLiteral("game build")
                                 : QStringLiteral("game build · %1").arg(p.category),
            summary.join(QStringLiteral(", ")), QStringLiteral("g:%1").arg(i));
    }
    m_weaponPreset->blockSignals(b);
    m_weaponPreset->setToolTip(
        equip.hasPresets()
            ? QStringLiteral("%1 build(s) from the game's development tables, "
                             "plus your own. Type with the list open to search.")
                  .arg(all.size())
            : QStringLiteral("Your saved builds. The game's own development "
                             "tables are not in the configured folders, so its "
                             "weapon grades cannot be listed."));
}

void CustomizeTab::saveWeaponPreset()
{
    if (m_parts.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Save preset"),
                                 QStringLiteral("Build a weapon first."));
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("Save preset"), QStringLiteral("Preset name:"),
        QLineEdit::Normal,
        m_weaponPick ? m_weaponPick->currentText() : QString(),
        &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    saveWeaponPresetAs(name);
}

// The same save with the name supplied rather than asked for. Split out so a
// harness run can save one: the preset blob is USER DATA with a grammar of its
// own, and until this existed there was no way to round-trip it — which is
// exactly how two fields came to be written in a form that only read back
// correctly by accident.
// How a part is NAMED in the scene description, for the per-part fields.
//
// Its slot when it came from a slot row, because a slot survives the part being
// reloaded at a different index. But not every part has one: the subject's own
// body arrives through the __host row and slotOfPart() returns empty for it, so
// keying on the slot alone meant a submesh switched off on the character's own
// body was captured by nothing — not by undo, not by a preset, not by an
// outfit. That is most of a TPP character.
//
// The fallback is the part's PATH behind a '#', which no slot id starts with.
// A path is what the rest of this grammar already uses to survive a reindex,
// and it identifies the body as stably as a slot identifies a garment.
QString CustomizeTab::partStateKey(int partIdx) const
{
    if (partIdx < 0 || partIdx >= m_parts.size()) return {};
    const QString slot = slotOfPart(partIdx);
    if (!slot.isEmpty()) return slot;
    const QString path = m_parts[partIdx].path;
    return path.isEmpty() ? QString() : QLatin1Char('#') + path;
}

// ── ONE serialisation of "the scene as the user built it" ──────────────────
// This grammar was the preset system's private business and is now the only
// description of an authored scene in the tool. Presets write it, outfits write
// it, and the undo stack is a list of it.
//
// It exists because there were TWO spellings and the smaller one silently lost
// data: an outfit stored part paths plus a gear colour and nothing else, so
// saving one threw away every variation, dye and appearance row the user had
// chosen — and it did so quietly, because reloading it produced a scene that
// looked plausible. Anything that can describe a built scene goes through here
// now, so a field added for one of them is a field all three gain.
QStringList CustomizeTab::captureSceneFields() const
{
    const auto& files = ArchiveIndex::instance().files();
    // An empty combo yields an INVALID QVariant, which toInt() reports as 0 —
    // saving the first file in the archive as if it had been chosen.
    const auto payloadOf = [](const QComboBox* c) {
        if (!c || c->currentIndex() < 0) return -1;
        const QVariant v = c->currentData();
        return v.isValid() ? v.toInt() : -1;
    };
    QStringList fields;
    for (const WeaponSlotRow& r : m_weaponRows) {
        if (!r.combo) continue;
        const int idx = payloadOf(r.combo);
        // An APPEARANCE row's payload is an option index into a texture set —
        // which skin tone, which iris colour — and never a file index. Writing
        // it out as files[idx].path recorded whichever asset happened to sit at
        // that position in the index, and it round-tripped only by accident:
        // the path resolved back to the same number until the next rescan moved
        // it, and then the eye colour quietly changed. Written as "#<option>",
        // which a path can never look like because a path starts with '/'.
        if (r.isLook) {
            if (r.combo->currentIndex() > 0 && idx >= 0)
                fields << r.slot + QStringLiteral("=#") + QString::number(idx);
            continue;
        }
        if (idx < 0 || idx >= files.size()) continue;
        QString field = r.slot + QLatin1Char('=') + files[idx].path;
        // A face PRESET row is not identified by its model: the game's grid has
        // twenty-eight rows over eight heads, so three of them are the same
        // .fmdl and the path alone always reloaded as the lowest-numbered of
        // them — taking that preset's hair, brows, beard and eyes with it. The
        // preset number is appended after a '#', which no asset path contains.
        const QVariant pr =
            r.combo->itemData(r.combo->currentIndex(), richcombo::PresetRole);
        if (pr.isValid() && pr.toInt() >= 0)
            field += QLatin1Char('#') + QString::number(pr.toInt());
        fields << field;
    }
    if (m_weaponVersion && m_weaponVersion->count() > 0) {
        const int hv = payloadOf(m_weaponVersion);
        if (hv >= 0 && hv < files.size())
            fields << QStringLiteral("__host=") + files[hv].path;
    }
    if (m_weaponCamo && !m_weaponCamo->currentData().toString().isEmpty())
        fields << QStringLiteral("camo=") + m_weaponCamo->currentData().toString();
    // The gear colour is saved as the swatch's ASSET PATH, not its hash: a
    // preset outlives the index it was made against, and a bare hash would be
    // meaningless if the install changed. It is resolved back on load.
    {
        const QString swatchPath = gearColorPath();
        if (!swatchPath.isEmpty())
            fields << QStringLiteral("gearcolor=") + swatchPath;
    }
    // MGO's per-item colours, one field per dyed slot:
    //   mgocolor:<slot>=<primaryId>,<secondaryId>
    // Colour IDS, not paths — they are GearConfig.lua's own vocabulary and
    // survive a reindex by construction. Either half may be empty.
    for (auto it = m_mgoColours.constBegin(); it != m_mgoColours.constEnd();
         ++it)
        if (!it.value().first.isEmpty() || !it.value().second.isEmpty())
            fields << QStringLiteral("mgocolor:%1=%2,%3")
                          .arg(it.key(), it.value().first, it.value().second);
    // ── Per-PART state, keyed by the slot that part came from ────────────
    // The grammar was keyed by slot and carried only what a slot row chose, so
    // three kinds of authored state were captured by nothing: a variation, an
    // attachment, and a switched-off submesh. Presets and outfits lost them
    // too — an outfit reloaded its parts and quietly dropped the variation the
    // user had picked on each of them.
    //
    // Keyed by SLOT rather than by part index, because a part index is a
    // position in m_parts and that shifts the moment anything is unequipped;
    // slotOfPart() is the join the rest of this grammar already uses.
    for (int i = 0; i < m_parts.size(); ++i) {
        const Part& part = m_parts[i];
        const QString slot = partStateKey(i);
        if (slot.isEmpty()) continue;
        if (!part.fovaName.isEmpty())
            fields << QStringLiteral("fova:%1=%2").arg(slot, part.fovaName);
        // An attachment names its HOST BY SLOT for the same reason. The cnp is
        // a name out of the host's .fcnp, so it survives a reindex as it is.
        if (part.attachPart >= 0 && part.attachPart < m_parts.size()) {
            const QString hostSlot = partStateKey(part.attachPart);
            if (!hostSlot.isEmpty())
                fields << QStringLiteral("attach:%1=%2@%3")
                              .arg(slot, hostSlot, part.attachCnp);
        }
        if (!part.hiddenGroups.isEmpty()) {
            QList<int> g = part.hiddenGroups.values();
            std::sort(g.begin(), g.end());
            QStringList ids;
            for (int v : g) ids << QString::number(v);
            fields << QStringLiteral("hidegroup:%1=%2")
                          .arg(slot, ids.join(QLatin1Char(',')));
        }
    }
    // Switched-off submeshes, translated from SCENE ids to each part's OWN
    // mesh index — the same translation the .glb exporter does, and for the
    // same reason. A scene id is a position in a layout rebuildScene decides,
    // so it changes when anything is equipped or removed; a part-local index
    // does not. Storing the scene id would have produced a state that replayed
    // correctly only until the user equipped one more thing.
    if (m_view) {
        QHash<QString, QList<int>> perSlot;
        for (const int sceneMesh : m_view->hiddenMeshes()) {
            const auto own = m_meshOwner.constFind(sceneMesh);
            if (own == m_meshOwner.constEnd()) continue;
            // own->first is (part index, attachment index); only a part's own
            // meshes are addressable by slot, and an attachment's belong to
            // the variation that brought it and die with it.
            if (own->first.second != -1) continue;
            const QString slot = partStateKey(own->first.first);
            if (slot.isEmpty()) continue;
            perSlot[slot].append(own->second);
        }
        // NOT named `slots` — that is a Qt macro that expands to nothing, and
        // the error it produces ("expected unqualified-id before '=' token")
        // names neither Qt nor the macro. Convention 15; verify-src checks it.
        QStringList slotNames = perSlot.keys();
        slotNames.sort();
        for (const QString& slot : slotNames) {
            QList<int> v = perSlot[slot];
            std::sort(v.begin(), v.end());
            QStringList ids;
            for (int x : v) ids << QString::number(x);
            fields << QStringLiteral("hidemesh:%1=%2")
                          .arg(slot, ids.join(QLatin1Char(',')));
        }
    }
    // Sorted, because two states that describe the same scene must compare
    // equal. m_mgoColours is a QHash and yields its keys in a different order
    // per run, which would make the undo stack push a "change" every time the
    // scene was merely rebuilt — convention 11, and it would have been
    // invisible until someone noticed undo needed pressing twice.
    fields.sort();
    return fields;
}

QString CustomizeTab::saveWeaponPresetAs(const QString& name)
{
    if (name.trimmed().isEmpty()) return QStringLiteral("empty preset name");
    // QSettings reads '/' as a group separator, so a name containing one would
    // silently become a nested group that refreshWeaponPresets() never lists.
    QString key = name.trimmed();
    key.replace(QLatin1Char('/'), QLatin1Char('-'));
    key.replace(QLatin1Char('\\'), QLatin1Char('-'));
    key.truncate(120);

    const QStringList fields = captureSceneFields();
    QSettings s;
    s.setValue(presetGroup() + QLatin1Char('/') + key, fields.join(QLatin1Char('|')));
    refreshWeaponPresets();
    const bool sb = m_weaponPreset->blockSignals(true);
    m_weaponPreset->selectPayload(QStringLiteral("u:") + key);
    m_weaponPreset->blockSignals(sb);
    return QStringLiteral("saved \"%1\" to [%2] — %3")
        .arg(key, presetGroup(), fields.join(QLatin1Char('|')));
}

// ── Undo (§15) ─────────────────────────────────────────────────────────────
// Called once every time the scene has finished settling. It does not know or
// care what changed — it compares the authored description against the last
// one and pushes a step only if they differ, which is what makes a burst of
// clicks one step and a non-authored rebuild no step at all.
void CustomizeTab::noteSceneSettled()
{
    // Our own applySceneFields is mid-flight: the intermediate rebuilds it
    // triggers are not user authorship and must not become steps.
    if (m_undoApplying) return;
    const QStringList now = captureSceneFields();
    if (!m_undoArmed) {
        // The first settle that produces an ACTUAL SCENE is the baseline, not a
        // step. Arming on the first settle full stop was not enough: the tab
        // settles once while still empty, so building the subject became step
        // one and the first Ctrl+Z emptied the page. Waiting for a non-empty
        // description makes "the page as it opened" the floor of the stack.
        if (now.isEmpty()) return;
        m_undoBaseline = now;
        m_undoArmed = true;
        return;
    }
    if (now == m_undoBaseline) return;
    m_undoStack.append(m_undoBaseline);
    // Thirty deep, as the template asks and as D4's wardrobe keeps. Dropping
    // from the FRONT: the oldest state is the one worth losing.
    while (m_undoStack.size() > kUndoDepth) m_undoStack.removeFirst();
    // A new authored change invalidates the redo branch. Standard, and the
    // alternative — keeping it — offers the user a "redo" that would splice
    // two different histories together.
    m_redoStack.clear();
    m_undoBaseline = now;
}

void CustomizeTab::undoScene()
{
    if (m_undoStack.isEmpty()) {
        setStatus(QStringLiteral("Nothing to undo"));
        return;
    }
    const QStringList target = m_undoStack.takeLast();
    m_redoStack.append(m_undoBaseline);
    while (m_redoStack.size() > kUndoDepth) m_redoStack.removeFirst();
    m_undoApplying = true;
    applySceneFields(target);
    m_undoApplying = false;
    // The baseline moves with us, so the NEXT authored change measures its
    // difference from where undo left the scene and not from where the user
    // was before it.
    m_undoBaseline = captureSceneFields();
    setStatus(QStringLiteral("Undo — %1 step(s) back, %2 forward")
                  .arg(m_undoStack.size())
                  .arg(m_redoStack.size()));
}

void CustomizeTab::redoScene()
{
    if (m_redoStack.isEmpty()) {
        setStatus(QStringLiteral("Nothing to redo"));
        return;
    }
    const QStringList target = m_redoStack.takeLast();
    m_undoStack.append(m_undoBaseline);
    while (m_undoStack.size() > kUndoDepth) m_undoStack.removeFirst();
    m_undoApplying = true;
    applySceneFields(target);
    m_undoApplying = false;
    m_undoBaseline = captureSceneFields();
    setStatus(QStringLiteral("Redo — %1 step(s) back, %2 forward")
                  .arg(m_undoStack.size())
                  .arg(m_redoStack.size()));
}

// --undoseq equip:<slot>=<substring>;fova:<slot>=<name>;undo;undo;redo
//
// An undo stack is invisible in a screenshot, exactly as the selection rules
// were, so it gets the same treatment --selseq got: a scripted sequence that
// prints the state after every step. The comparison that matters is that the
// state after N undos is character-for-character the state that was N steps
// ago — which a human clicking a viewport cannot check and this can.
QString CustomizeTab::undoSeqReport(const QString& seq)
{
    QStringList out;
    const auto snap = [this](const QString& tag) {
        return QStringLiteral("%1  undo=%2 redo=%3  %4")
            .arg(tag, QString::number(m_undoStack.size()),
                 QString::number(m_redoStack.size()),
                 captureSceneFields().join(QLatin1Char('|')));
    };
    out << snap(QStringLiteral("start "));
    // Every state as it is produced, so the replay can be checked against the
    // history rather than against a description of it.
    QVector<QStringList> history;
    history.append(captureSceneFields());

    for (const QString& rawStep : seq.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QString step = rawStep.trimmed();
        if (step.isEmpty()) continue;
        if (step == QLatin1String("undo")) {
            undoScene();
            out << snap(QStringLiteral("undo  "));
            continue;
        }
        if (step == QLatin1String("redo")) {
            redoScene();
            out << snap(QStringLiteral("redo  "));
            continue;
        }
        // Anything else is a build spec field, applied through the same
        // grammar everything else uses.
        QStringList fields = captureSceneFields();
        const QString slot = step.section(QLatin1Char('='), 0, 0);
        // Replace this slot's field rather than appending a second one.
        for (int i = fields.size() - 1; i >= 0; --i)
            if (fields[i].section(QLatin1Char('='), 0, 0) == slot)
                fields.removeAt(i);
        fields << step;
        fields.sort();
        // No noteSceneSettled() here: applySceneFields pushes its own single
        // step. Calling it again pushed a second, empty one.
        applySceneFields(fields);
        history.append(captureSceneFields());
        out << snap(QStringLiteral("apply "));
    }
    return out.join(QLatin1Char('\n'));
}

void CustomizeTab::loadWeaponPreset(const QString& name)
{
    QSettings s;
    const QString blob =
        s.value(presetGroup() + QLatin1Char('/') + name).toString();
    // An absent or empty preset must not wipe the scene. The guard lives HERE,
    // on "there is no such preset", and NOT in applySceneFields — an empty
    // field list is a perfectly good scene description meaning "nothing
    // equipped", and undo has to be able to apply it. It was in the wrong
    // place, and --undoseq caught it on its first run: the state printed after
    // an undo was identical to the state before it.
    if (blob.isEmpty()) return;
    applySceneFields(blob.split(QLatin1Char('|'), Qt::SkipEmptyParts));
}

// The other half of captureSceneFields(). Everything that rebuilds a scene
// from a description goes through here — a preset, an outfit, and every step
// of the undo stack — so there is one parser for one grammar.
//
// ATOMIC with respect to the undo stack. Applying a description rebuilds the
// scene more than once — the slot rows settle, and the per-part state is
// applied after that because it needs parts to exist — and every one of those
// settles would otherwise become its own undo step. --undoseq caught it as a
// stack that grew by two on one change; the brief's own wording is that
// loading an outfit is "ONE undo step, not twenty".
//
// The flag is saved and restored rather than set and cleared, so undoScene()
// and redoScene() — which set it themselves, precisely so their replay pushes
// nothing — still get no step out of the call they make here.
void CustomizeTab::applySceneFields(const QStringList& blobFields)
{
    const bool wasApplying = m_undoApplying;
    m_undoApplying = true;
    applySceneFieldsImpl(blobFields);
    m_undoApplying = wasApplying;
    if (!wasApplying) noteSceneSettled();
}

void CustomizeTab::applySceneFieldsImpl(const QStringList& blobFields)
{
    const auto& files = ArchiveIndex::instance().files();

    m_weaponRebuilding = true;
    for (WeaponSlotRow& r : m_weaponRows)
        if (r.combo) {
            const bool b = r.combo->blockSignals(true);
            r.combo->setCurrentIndex(0);
            r.combo->blockSignals(b);
        }
    // The preset's dye choices replace the current ones outright — including
    // "none of them": a preset saved undyed loads undyed. Parsed into a
    // local first; see the mgocolor: branch below for why.
    QHash<QString, QPair<QString, QString>> loadedColours;
    QHash<QString, QString> loadedFova;
    QHash<QString, QPair<QString, QString>> loadedAttach;   // slot -> (hostSlot, cnp)
    QHash<QString, QSet<int>> loadedHideGroup;
    QHash<QString, QSet<int>> loadedHideMesh;
    QString camo;
    QString wantColorPath;
    QStringList missing;
    // Collect the wanted paths first and resolve them in ONE pass: a full game
    // index is hundreds of thousands of files and a preset has up to ten
    // fields, so a scan per field is a visible stall.
    QHash<QString, int> wanted;

    // Parsed once, here, rather than re-split in each of the three passes
    // below — the field grammar now has two optional parts and keeping three
    // copies of the parser in step is how they drift.
    //
    //   slot=<path>            a part, as it always was
    //   slot=<path>#<preset>   a part that IS one of the game's face presets
    //   slot=#<option>         an appearance row's option index
    //
    // An old blob has none of the '#' forms and parses exactly as before.
    struct Field {
        QString slot;
        QString path;      // empty for an appearance row
        int option = -1;   // >= 0 only for an appearance row
        int preset = -1;   // >= 0 when the row named a face preset
    };
    QVector<Field> parsed;
    for (const QString& raw : blobFields) {
        const int eq = raw.indexOf(QLatin1Char('='));
        if (eq <= 0) continue;
        Field f;
        f.slot = raw.left(eq);
        const QString value = raw.mid(eq + 1);
        // "mgocolor:<slot>=<primaryId>,<secondaryId>" — a dye choice, not a
        // part. Held LOCALLY until the field loop is done: restoring the
        // __host below goes through rebuildSlotRows() when the preset's
        // subject is not the one on screen, and that clears m_mgoColours —
        // writing the choices into the member here meant a cross-subject
        // load restored the garments and silently lost every dye.
        // ── Per-part state, held locally for the same reason the dyes are ──
        // Every one of these names a SLOT, and the slot's part does not exist
        // until the rows below have been restored and rebuildWeapon() has run.
        // Applied at the end, once there is something to apply them to.
        if (f.slot.startsWith(QLatin1String("fova:"))) {
            loadedFova.insert(f.slot.mid(5), value);
            continue;
        }
        if (f.slot.startsWith(QLatin1String("attach:"))) {
            const QString host = value.section(QLatin1Char('@'), 0, 0);
            const QString cnp = value.section(QLatin1Char('@'), 1);
            if (!host.isEmpty()) loadedAttach.insert(f.slot.mid(7), {host, cnp});
            continue;
        }
        if (f.slot.startsWith(QLatin1String("hidegroup:"))
            || f.slot.startsWith(QLatin1String("hidemesh:"))) {
            const bool group = f.slot.startsWith(QLatin1String("hidegroup:"));
            const QString slot = f.slot.mid(group ? 10 : 9);
            QSet<int> ids;
            for (const QString& t :
                 value.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                bool ok = false;
                const int v = t.toInt(&ok);
                if (ok) ids.insert(v);
            }
            if (group) loadedHideGroup.insert(slot, ids);
            else loadedHideMesh.insert(slot, ids);
            continue;
        }
        if (f.slot.startsWith(QLatin1String("mgocolor:"))) {
            const QString gearSlot = f.slot.mid(9);
            const QString pri = value.section(QLatin1Char(','), 0, 0);
            const QString sec = value.section(QLatin1Char(','), 1, 1);
            if (!gearSlot.isEmpty() && (!pri.isEmpty() || !sec.isEmpty()))
                loadedColours.insert(gearSlot, {pri, sec});
            continue;
        }
        if (value.startsWith(QLatin1Char('#'))) {
            bool ok = false;
            const int v = value.mid(1).toInt(&ok);
            if (!ok) continue;
            f.option = v;
        } else {
            f.path = value;
            const int hash = value.lastIndexOf(QLatin1Char('#'));
            if (hash > 0) {
                bool ok = false;
                const int v = value.mid(hash + 1).toInt(&ok);
                if (ok) { f.preset = v; f.path = value.left(hash); }
            }
        }
        parsed.append(f);
    }

    for (const Field& f : parsed) {
        if (f.path.isEmpty()) continue;
        if (f.slot != QLatin1String("camo")
            && f.slot != QLatin1String("gearcolor"))
            wanted.insert(f.path, -1);
    }
    for (int i = 0; i < files.size(); ++i) {
        const auto it = wanted.find(files[i].path);
        if (it != wanted.end() && it.value() < 0) it.value() = i;
    }
    // Tell the compatibility filter what is coming before it runs, or a saved
    // part the game's own builds never used would be filtered out of its combo
    // and the slot would silently come back empty.
    m_pendingKeep.clear();
    for (const Field& f : parsed) {
        if (f.path.isEmpty()) continue;   // an appearance row narrows nothing
        if (f.slot == QLatin1String("camo") || f.slot == QLatin1String("__host")
            || f.slot == QLatin1String("gearcolor"))
            continue;
        m_pendingKeep.insert(f.slot, wanted.value(f.path, -1));
    }
    // The HOST goes first, whatever order the blob lists it in. Choosing it
    // calls refreshSlotItems(), which rebuilds every slot combo from scratch —
    // so a part selected before that point had its selection thrown away.
    // saveWeaponPreset appends __host after the rows, so the blob always lists
    // it last and every saved part was being wiped by the host that followed
    // it. The symptom was subtle rather than empty: the combo came back
    // populated and sitting on whatever its first row was.
    std::stable_partition(parsed.begin(), parsed.end(), [](const Field& f) {
        return f.slot == QLatin1String("__host");
    });
    for (const Field& f : parsed) {
        const QString slot = f.slot, path = f.path;
        // An appearance row: select the saved OPTION on it and nothing else.
        // These are filled from the face preset when absent, so a row the blob
        // does not mention is already at "— from face preset —" from the reset
        // at the top of this function.
        if (f.option >= 0) {
            for (WeaponSlotRow& r : m_weaponRows) {
                if (r.slot != slot || !r.combo || !r.isLook) continue;
                const bool b = r.combo->blockSignals(true);
                r.combo->selectPayload(f.option);
                r.combo->blockSignals(b);
                break;
            }
            continue;
        }
        if (slot == QLatin1String("camo")) { camo = path; continue; }
        if (slot == QLatin1String("gearcolor")) {
            // Resolved through the catalogue rather than by hashing the path,
            // so a swatch this install does not carry leaves the colour unset
            // instead of pointing at a texture that is not there.
            if (layerSwatchHash(path) == 0) missing << path.section(QLatin1Char('/'), -1);
            else wantColorPath = path;
            continue;
        }
        const int fileIdx = wanted.value(path, -1);
        if (fileIdx < 0) { missing << path.section(QLatin1Char('/'), -1); continue; }
        if (slot == QLatin1String("__host")) {
            // Select the subject that owns this variant, then the variant. The
            // saved parts follow below, so nothing here may fit any of its own.
            if (!selectHostVariant(fileIdx))
                missing << path.section(QLatin1Char('/'), -1);
            // The slot lists are narrowed against the receiver; a saved part
            // has to be IN its list before it can be selected into it.
            refreshSlotItems();
            continue;
        }
        // A preset blob records the slot ID, and the MGO avatar's face slot was
        // renamed from "face" to "head" so the appearance rows could find it.
        // Nothing in the blob says which subject it belongs to, so an old MGO
        // preset would have matched no row at all — and because a part only
        // counts as "missing" when its PATH fails to resolve, it would have
        // been dropped in silence, leaving the face on whatever the first row
        // happened to be while the rest of the preset loaded fine.
        //
        // Aliased rather than migrated: the blobs are shared across every
        // subject and Snake's own face slot is still called "face", so
        // rewriting them on disk would break his. The alias only fires when
        // there is no "face" row to take it.
        QString wantSlot = slot;
        if (wantSlot == QLatin1String("face")) {
            bool haveFace = false;
            for (const WeaponSlotRow& r : m_weaponRows)
                if (r.slot == QLatin1String("face")) { haveFace = true; break; }
            if (!haveFace) wantSlot = QStringLiteral("head");
        }
        for (WeaponSlotRow& r : m_weaponRows) {
            if (r.slot != wantSlot || !r.combo) continue;
            const bool b = r.combo->blockSignals(true);
            // The PRESET number first when the blob carries one, because the
            // model does not identify the row: several presets share a head,
            // and selecting by model always lands on the first of them.
            bool got = false;
            if (f.preset >= 0) {
                for (int i = 0; i < r.combo->count(); ++i) {
                    const QVariant pv =
                        r.combo->itemData(i, richcombo::PresetRole);
                    if (pv.isValid() && pv.toInt() == f.preset) {
                        r.combo->setCurrentIndex(i);
                        got = true;
                        break;
                    }
                }
            }
            // A part that is in the install but not in this combo is a real
            // outcome — say so rather than leaving the slot quietly empty.
            if (!got && !r.combo->selectPayload(fileIdx))
                missing << path.section(QLatin1Char('/'), -1);
            r.combo->blockSignals(b);
            break;
        }
    }
    m_pendingKeep.clear();
    m_weaponRebuilding = false;

    // An empty path is a statement, not a no-op: a preset saved with the gear
    // as shipped must CLEAR a colour left over from the last one, exactly as
    // an absent slot clears that slot above.
    setGearColorPath(wantColorPath);

    refreshWeaponCamoList();
    // AN ABSENT camo= FIELD IS A STATEMENT, not a gap — the same rule the gear
    // colour above already follows. captureSceneFields writes the field only
    // when a real variation is chosen, so a scene saved on "— default —" has
    // none; leaving the selection alone here used to be harmless because a
    // freshly built list always landed on row 0 anyway. It no longer does:
    // refreshWeaponCamoList now opens a weapon on "clv". Without this, every
    // preset and outfit saved before today would reopen repainted, which is
    // the round-trip trap and it is silent.
    if (m_weaponCamo) {
        const bool b = m_weaponCamo->blockSignals(true);
        int row = 0;
        if (!camo.isEmpty())
            for (int i = 1; i < m_weaponCamo->count(); ++i)
                if (m_weaponCamo->itemData(i).toString() == camo) { row = i; break; }
        m_weaponCamo->setCurrentIndex(row);
        m_weaponCamo->blockSignals(b);
    }
    // The dye choices land AFTER every slot is restored — the host restore
    // above may have rebuilt the rows (and cleared the member) on a
    // cross-subject load — then the colour rows follow the restored slots
    // and show them.
    m_mgoColours = loadedColours;
    fillGearColourRows();
    rebuildWeapon();

    // ── Per-part state, now that the parts exist ─────────────────────────
    // Slot → part index, built once. slotOfPart() is the other direction and
    // asking it per lookup would be a scan of m_parts per field.
    if (!loadedFova.isEmpty() || !loadedAttach.isEmpty()
        || !loadedHideGroup.isEmpty() || !loadedHideMesh.isEmpty()) {
        QHash<QString, int> partOfSlot;
        for (int i = 0; i < m_parts.size(); ++i) {
            const QString sl = partStateKey(i);
            if (!sl.isEmpty() && !partOfSlot.contains(sl))
                partOfSlot.insert(sl, i);
        }
        bool touched = false;
        // Variations first: applyFovaToPart reloads BOTH texture sets
        // wholesale and rewrites the part's fova-hidden groups, so anything
        // done before it on the same part is thrown away (see the ORDER
        // MATTERS note in rebuildScene's neighbourhood).
        for (auto it = loadedFova.constBegin(); it != loadedFova.constEnd(); ++it) {
            const int pi = partOfSlot.value(it.key(), -1);
            if (pi < 0) continue;
            applyFovaToPart(pi, it.value());
            touched = true;
        }
        for (auto it = loadedAttach.constBegin(); it != loadedAttach.constEnd();
             ++it) {
            const int pi = partOfSlot.value(it.key(), -1);
            const int hi = partOfSlot.value(it.value().first, -1);
            // attachPartTo refuses a cycle and a host with no such connect
            // point, and its refusal is the right answer here too: a saved
            // attachment whose host is no longer fitted simply does not happen.
            if (pi >= 0 && hi >= 0 && attachPartTo(pi, hi, it.value().second))
                touched = true;
        }
        for (auto it = loadedHideGroup.constBegin();
             it != loadedHideGroup.constEnd(); ++it) {
            const int pi = partOfSlot.value(it.key(), -1);
            if (pi < 0) continue;
            m_parts[pi].hiddenGroups = it.value();
            touched = true;
        }
        if (touched) rebuildScene();

        // Submeshes LAST and after the rebuild, because they are addressed by
        // scene mesh id and rebuildScene is what assigns those. m_meshOwner is
        // the map it records on the way; this is that map read backwards.
        if (!loadedHideMesh.isEmpty() && m_view) {
            for (const int sceneMesh : m_view->hiddenMeshes())
                m_view->setMeshVisible(sceneMesh, true);
            for (auto own = m_meshOwner.constBegin();
                 own != m_meshOwner.constEnd(); ++own) {
                if (own.value().first.second != -1) continue;   // attachment
                const QString sl = partStateKey(own.value().first.first);
                if (sl.isEmpty()) continue;
                const auto want = loadedHideMesh.constFind(sl);
                if (want == loadedHideMesh.constEnd()) continue;
                if (want->contains(own.value().second))
                    m_view->setMeshVisible(own.key(), false);
            }
            if (m_sceneTree) m_sceneTree->setHiddenLeaves(m_view->hiddenMeshes());
        }
    }
    if (!missing.isEmpty() && m_weaponInfo)
        m_weaponInfo->setText(
            m_weaponInfo->text()
            + QStringLiteral("  ·  not in this install: %1")
                  .arg(missing.join(QStringLiteral(", "))));
}

QString CustomizeTab::applyUserPresetFromSpec(const QString& name, bool compatOnly)
{
    if (!m_category) return QStringLiteral("builder UI not built");
    // Whatever category is already showing, when it is one that HAS presets.
    // This used to force category 1, so a harness run could only ever load a
    // weapon preset — a character or vehicle preset was looked up in the wrong
    // QSettings group and came back empty, which made the whole class of them
    // untestable. Only fall back to the weapon builder from a category with no
    // preset list of its own.
    const int cur = m_category->currentIndex();
    const bool hasPresets = cur == 1 || cur == 2 || cur == 3 || cur == 4;
    if (!hasPresets) {
        m_category->setCurrentIndex(1);
        setBuilderCategory(1);
    }
    if (m_weaponCompat && m_weaponCompat->isChecked() != compatOnly)
        m_weaponCompat->setChecked(compatOnly);
    loadWeaponPreset(name);
    QStringList got;
    for (const WeaponSlotRow& r : m_weaponRows) {
        if (!r.combo) continue;
        const QVariant v = r.combo->currentIndex() >= 0 ? r.combo->currentData()
                                                        : QVariant();
        if (v.isValid() && v.toInt() >= 0) got << r.slot;
    }
    return QStringLiteral("user preset \"%1\" (compat %2) — slots filled: %3 | %4")
        .arg(name, compatOnly ? QStringLiteral("on") : QStringLiteral("off"),
             got.isEmpty() ? QStringLiteral("(none)") : got.join(QStringLiteral(", ")),
             m_weaponInfo ? m_weaponInfo->text() : QString());
}

QString CustomizeTab::applyPresetFromSpec(const QString& presetFilter,
                                         bool compatOnly)
{
    if (!m_category) return QStringLiteral("builder UI not built");
    // The combo drives the stack; setting only one of the two leaves the tab
    // showing the free-form page while the weapon panel does the work.
    if (m_category->currentIndex() != 1) {
        m_category->setCurrentIndex(1);
        setBuilderCategory(1);
    }
    if (m_weaponRows.isEmpty())
        return QStringLiteral("no %1 slots in the indexed data")
            .arg(m_source.subjectLabel.isEmpty() ? QStringLiteral("builder")
                                                 : m_source.subjectLabel.toLower());
    if (m_weaponCompat && compatOnly && !m_weaponCompat->isChecked())
        m_weaponCompat->setChecked(true);   // toggled → refreshSlotItems()

    QString result;
    if (!presetFilter.isEmpty()) {
        const fox::EquipCatalog& equip = fox::EquipCatalog::instance();
        int found = -1;
        for (int i = 0; i < equip.presets().size(); ++i)
            if (equip.presets()[i].label().contains(presetFilter, Qt::CaseInsensitive)) {
                found = i;
                break;
            }
        if (found < 0)
            return QStringLiteral("preset \"%1\" NOT matched (%2 build(s) known)")
                .arg(presetFilter)
                .arg(equip.presets().size());
        const bool b = m_weaponPreset->blockSignals(true);
        m_weaponPreset->selectPayload(QStringLiteral("g:%1").arg(found));
        m_weaponPreset->blockSignals(b);
        applyGamePreset(found);
        const fox::WeaponPreset& p = equip.presets()[found];
        QStringList got;
        for (const WeaponSlotRow& r : m_weaponRows)
            if (r.combo && r.combo->currentData().toInt() >= 0)
                got << r.slot;
        result = QStringLiteral("preset \"%1\" — %2 part(s) named, slots filled: %3")
                     .arg(p.label())
                     .arg(p.parts.size())
                     .arg(got.isEmpty() ? QStringLiteral("(none)")
                                        : got.join(QStringLiteral(", ")));
    }
    QStringList counts;
    for (const WeaponSlotRow& r : m_weaponRows)
        if (r.combo) counts << QStringLiteral("%1=%2").arg(r.slot).arg(r.combo->count() - 1);
    return result + QStringLiteral(" | offered: %1").arg(counts.join(QStringLiteral(" ")));
}

void CustomizeTab::deleteWeaponPreset()
{
    if (!m_weaponPreset || m_weaponPreset->currentIndex() <= 0) return;
    const QString tag = m_weaponPreset->currentData().toString();
    // Only your own builds are yours to delete — the game's are read-only.
    if (!tag.startsWith(QLatin1String("u:"))) {
        QMessageBox::information(
            this, QStringLiteral("Delete preset"),
            QStringLiteral("That is one of the game's own builds, read out of "
                           "its development tables. Only builds you saved can "
                           "be deleted."));
        return;
    }
    QSettings s;
    s.remove(presetGroup() + QLatin1Char('/') + tag.mid(2));
    refreshWeaponPresets();
}

QString CustomizeTab::presetGroup() const
{
    if (m_builderCategory == 2) return QStringLiteral("characterPresets");
    if (m_builderCategory == 3) return QStringLiteral("vehiclePresets");
    // "All other characters" is its own category and needs its own group: it
    // fell through to the weapon group, so a saved weapon appeared in the
    // character list and loading it drove a receiver in as a body.
    if (m_builderCategory == 4) return QStringLiteral("otherCharacterPresets");
    return QStringLiteral("weaponPresets");
}

void CustomizeTab::setBuilderCategory(int categoryIndex)
{
    m_builderCategory = categoryIndex;
    // The subject id belongs to the category that was showing; carrying it
    // across would build the previous category's rows under the new one.
    m_currentSubjectId.clear();
    // So do the base-body decisions. The free-form composer calls
    // rebuildScene() directly, and a stale head-only filter there would delete
    // every triangle of the first model added that is not part of a head.
    m_headOnlyPart = -1;
    m_hideBasePart = -1;
    // 0 = free-form character composer (the original page), 1 = Weapon builder,
    // 2 = Character builder. Both builder categories drive the same panel.
    if (categoryIndex == 1) m_source = WeaponCatalog::instance().builderSource();
    else if (categoryIndex == 2) m_source = fox::PlayerCatalog::instance().builderSource();
    else if (categoryIndex == 4) m_source = fox::CharacterCatalog::instance().builderSource();
    else if (categoryIndex == 3) m_source = fox::MechaCatalog::instance().builderSource();
    else m_source = fox::BuilderSource();
    if (m_weaponPickLabel) m_weaponPickLabel->setText(m_source.subjectLabel);
    if (m_weaponVersionLabel) m_weaponVersionLabel->setText(m_source.variantLabel);
    // The compatibility rules and the shipped builds are weapon data. Showing
    // either on the Character page offered a switch that did nothing and a
    // preset list that would have swapped the model for a gun.
    if (m_weaponCompat) m_weaponCompat->setVisible(categoryIndex == 1);
    // …and the Unlocked switch is the mirror image: the Exclude and Must rules
    // are character data, so on the Weapon and Vehicle pages the box is inert.
    // A visible switch that does nothing is the same small lie either way.
    refreshGearRuleControl();
    if (m_gameRow) m_gameRow->setVisible(categoryIndex != 0);
    refreshWeaponCatalogue();
}

bool CustomizeTab::openBuilderPopup(const QString& which)
{
    SearchableCombo* target = nullptr;
    if (which == QLatin1String("subject")) target = m_weaponPick;
    else if (which == QLatin1String("variant")) target = m_weaponVersion;
    else if (which == QLatin1String("preset")) target = m_weaponPreset;
    else if (which == QLatin1String("camo") || which == QLatin1String("color"))
        target = m_weaponCamo;
    else if (which.startsWith(QLatin1String("gearcolor:"))) {
        // "gearcolor:<slot>:<channel>" — the per-item dye rows, which are not
        // in m_weaponRows and so were unreachable from the harness. Channel
        // defaults to 0 (Primary); 1 is Secondary.
        // Channels 0 and 1 are the garment's own; 2 and 3 are the second
        // model's ("Vest Color 1/2"). Without the second pair the two rows a
        // two-piece garment adds had no automated coverage at all.
        const QString rest = which.mid(10);
        const QString slot = rest.section(QLatin1Char(':'), 0, 0);
        const int want = rest.contains(QLatin1Char(':'))
            ? rest.section(QLatin1Char(':'), 1, 1).toInt() : 0;
        const bool wantCompanion = want >= 2;
        const int ch = want % 2;
        for (const GearColourRow& r : m_gearColourRows)
            if (r.channel == ch && r.companion == wantCompanion
                && r.slot.compare(slot, Qt::CaseInsensitive) == 0) {
                target = r.combo;
                break;
            }
    } else
        for (const WeaponSlotRow& r : m_weaponRows)
            if (r.slot.compare(which, Qt::CaseInsensitive) == 0) { target = r.combo; break; }
    if (!target) return false;
    target->showPopup();
    // Optional "slot:query" form drives the type-to-search path too.
    return true;
}

bool CustomizeTab::hoverBuilderPopupRow(int row)
{
    // The preview is driven by real mouse-move events on the popup's viewport,
    // so the harness has to deliver one rather than call the preview directly —
    // otherwise the test proves the popup works and not the wiring.
    for (SearchableCombo* c : m_weaponPanel
             ? m_weaponPanel->findChildren<SearchableCombo*>()
             : QList<SearchableCombo*>()) {
        QAbstractItemView* v = c->view();
        if (!v || !v->isVisible()) continue;
        const QModelIndex ix = c->model()->index(row, 0);
        if (!ix.isValid()) return false;
        const QRect r = v->visualRect(ix);
        const QPoint p = r.isValid() ? r.center() : QPoint(20, 20);
        QMouseEvent me(QEvent::MouseMove, QPointF(p),
                       QPointF(v->viewport()->mapToGlobal(p)), Qt::NoButton,
                       Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(v->viewport(), &me);
        return true;
    }
    return false;
}

bool CustomizeTab::typeIntoBuilderPopup(const QString& text)
{
    QWidget* w = QApplication::focusWidget();
    for (const QChar& c : text) {
        QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, QString(c));
        QApplication::sendEvent(w ? w : this, &press);
    }
    return true;
}
