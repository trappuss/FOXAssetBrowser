// GameId.h — which game an indexed file belongs to, and which of them the
// builder is currently drawing from.
//
// One install can hold four of them at once. The Phantom Pain and MGO 3 ship in
// the same folder (MGO's archives are the "mgo_*" ones and its assets live
// under /Assets/mgo/), Ground Zeroes is a separate .g0s archive set with its own
// hash scheme, and Metal Gear Survive is a separate game whose assets live under
// /Assets/ssd/. Measured on the shipped dictionaries: 338,791 /Assets/tpp paths,
// 10,091 /Assets/ssd, 2,444 /Assets/mgo, and every Ground Zeroes path is under
// /Assets/tpp but reached through a .g0s archive.
//
// So the asset path answers the question for a NAMED file, the archive answers
// it for an unnamed one, and the archive's own answer is a majority vote over
// the named files it carries — not a guess from its file name.
#pragma once
#include <QString>
#include <QStringList>

namespace fox {

enum class GameId : quint8 {
    Unknown = 0,
    Tpp,          // MGSV: The Phantom Pain
    Mgo,          // Metal Gear Online 3 (ships inside TPP)
    GroundZeroes, // MGSV: Ground Zeroes
    Survive,      // Metal Gear Survive
};
constexpr int kGameCount = 5;

// "TPP" / "MGO3" / "GZ" / "Survive" — short enough for a checkbox.
const char* gameShortName(GameId g);
// "MGSV: The Phantom Pain" — for tooltips and reports.
const char* gameLongName(GameId g);
// The game an /Assets/… path belongs to, or Unknown when it says nothing.
GameId gameForAssetPath(const QString& path);

// Which games the builder currently draws from. The archive index stays whole —
// the Files tab still shows everything — this only narrows the catalogues, so
// a Survive part can never be offered on a Phantom Pain character.
//
// The generation counter is what the catalogues fold into their cache key, so a
// toggle rebuilds them exactly the way a rescan does.
class GameFilter {
public:
    static GameFilter& instance();

    bool enabled(GameId g) const;
    void setEnabled(GameId g, bool on);
    int generation() const { return m_generation; }
    // "TPP, MGO3" — the enabled games, for a status line.
    QString describe() const;

private:
    GameFilter();
    bool m_on[kGameCount];
    int m_generation = 0;
};

}  // namespace fox
