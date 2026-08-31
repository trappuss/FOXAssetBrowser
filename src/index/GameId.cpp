// GameId.cpp — see GameId.h.
#include "index/GameId.h"

#include <QSettings>

namespace fox {

const char* gameShortName(GameId g)
{
    switch (g) {
        case GameId::Tpp: return "TPP";
        case GameId::Mgo: return "MGO3";
        case GameId::GroundZeroes: return "GZ";
        case GameId::Survive: return "Survive";
        default: return "Other";
    }
}

const char* gameLongName(GameId g)
{
    switch (g) {
        case GameId::Tpp: return "MGSV: The Phantom Pain";
        case GameId::Mgo: return "Metal Gear Online 3";
        case GameId::GroundZeroes: return "MGSV: Ground Zeroes";
        case GameId::Survive: return "Metal Gear Survive";
        default: return "Unrecognised";
    }
}

GameId gameForAssetPath(const QString& path)
{
    // Longest-prefix first: /Assets/tpptest is not /Assets/tpp.
    if (path.startsWith(QLatin1String("/Assets/ssd/"), Qt::CaseInsensitive))
        return GameId::Survive;
    if (path.startsWith(QLatin1String("/Assets/mgo/"), Qt::CaseInsensitive))
        return GameId::Mgo;
    if (path.startsWith(QLatin1String("/Assets/tpp/"), Qt::CaseInsensitive))
        return GameId::Tpp;
    return GameId::Unknown;
}

GameFilter& GameFilter::instance()
{
    static GameFilter f;
    return f;
}

GameFilter::GameFilter()
{
    QSettings s;
    s.beginGroup(QStringLiteral("games"));
    for (int i = 0; i < kGameCount; ++i)
        m_on[i] = s.value(QString::fromLatin1(gameShortName(GameId(i))), true).toBool();
    s.endGroup();
    // "Other" is everything the rules could not place. Leaving it on by default
    // means an install this build has never seen degrades to showing its assets
    // rather than hiding them.
    m_on[int(GameId::Unknown)] = true;
}

bool GameFilter::enabled(GameId g) const
{
    const int i = int(g);
    return i >= 0 && i < kGameCount ? m_on[i] : true;
}

void GameFilter::setEnabled(GameId g, bool on)
{
    const int i = int(g);
    if (i < 0 || i >= kGameCount || m_on[i] == on) return;
    m_on[i] = on;
    ++m_generation;
    QSettings s;
    s.beginGroup(QStringLiteral("games"));
    s.setValue(QString::fromLatin1(gameShortName(g)), on);
    s.endGroup();
}


QString GameFilter::describe() const
{
    QStringList on;
    for (int i = 1; i < kGameCount; ++i)
        if (m_on[i]) on << QString::fromLatin1(gameShortName(GameId(i)));
    return on.isEmpty() ? QStringLiteral("none") : on.join(QStringLiteral(", "));
}

}  // namespace fox
