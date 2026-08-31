#include "app/Config.h"

#include <QSettings>
#include <QtGlobal>

#include "app/AppPaths.h"

// One setting, one key — every accessor pair reads and writes the SAME key,
// and nothing else touches these keys.
namespace {
const char kGameDirs[] = "paths/gameDirs";
const char kDictDir[] = "paths/dictDir";
const char kDeepScan[] = "index/deepScan";
const char kExportDir[] = "paths/exportDir";
const char kModDir[] = "paths/modDir";
const char kPbrFiles[] = "render/pbrFiles";
const char kPbrModels[] = "render/pbrModels";
const char kPbrCustomize[] = "render/pbrCustomize";

const char* pbrKey(Config::PbrView v)
{
    switch (v) {
    case Config::PbrView::Files: return kPbrFiles;
    case Config::PbrView::Models: return kPbrModels;
    case Config::PbrView::Customize: return kPbrCustomize;
    }
    return kPbrModels;
}

// Session-only CLI overrides (never written to disk).
QStringList g_sessionGameDirs;
QString g_sessionDictDir;
bool g_haveSessionGameDirs = false;
QString g_sessionModDir;
bool g_haveSessionModDir = false;
QString g_sessionExportDir;
bool g_haveSessionExportDir = false;
}  // namespace

QStringList Config::gameDirs()
{
    if (g_haveSessionGameDirs) return g_sessionGameDirs;
    return QSettings().value(QLatin1String(kGameDirs)).toStringList();
}

void Config::setGameDirs(const QStringList& dirs)
{
    QSettings().setValue(QLatin1String(kGameDirs), dirs);
}

QString Config::dictDir()
{
    if (!g_sessionDictDir.isEmpty()) return g_sessionDictDir;
    const QString d = QSettings().value(QLatin1String(kDictDir)).toString();
    return d.isEmpty() ? AppPaths::dictDir() : d;
}

void Config::setDictDir(const QString& dir)
{
    QSettings().setValue(QLatin1String(kDictDir), dir);
}

bool Config::textureAlphaBg()
{
    return QSettings().value(QStringLiteral("textures/alphaBg"), true).toBool();
}

void Config::setTextureAlphaBg(bool on)
{
    QSettings().setValue(QStringLiteral("textures/alphaBg"), on);
}

bool Config::deepScan()
{
    return QSettings().value(QLatin1String(kDeepScan), true).toBool();
}

void Config::setDeepScan(bool on)
{
    QSettings().setValue(QLatin1String(kDeepScan), on);
}

bool Config::pbrEnabled(PbrView view)
{
    // Default ON for the two viewports where a model is being looked at, and
    // OFF for the Files preview, which is a flick-through: four extra CPU
    // texture decodes per material is the difference between a preview that
    // keeps up with the arrow keys and one that does not.
    const bool def = view != PbrView::Files;
    return QSettings().value(QLatin1String(pbrKey(view)), def).toBool();
}

void Config::setPbrEnabled(PbrView view, bool on)
{
    QSettings().setValue(QLatin1String(pbrKey(view)), on);
}

QString Config::viewEnvironment()
{
    return QSettings()
        .value(QStringLiteral("view/environment"), QStringLiteral("default"))
        .toString();
}

void Config::setViewEnvironment(const QString& id)
{
    QSettings().setValue(QStringLiteral("view/environment"), id);
}

double Config::viewExposure()
{
    // Clamped on the way OUT as well as on the way in: a hand-edited settings
    // file is the one input this class cannot validate at write time.
    const double e = QSettings().value(QStringLiteral("view/exposure"), 0.0)
                         .toDouble();
    if (e <= 0.0) return 0.0;   // 0 = "use the environment's own"
    return qBound(0.05, e, 4.0);
}

void Config::setViewExposure(double e)
{
    // Clamped HERE as well, so the stored value is one the reader will hand
    // back unchanged. Writing 0.03 and reading 0.05 left the dialog and the
    // file permanently disagreeing about what the setting was.
    QSettings().setValue(QStringLiteral("view/exposure"),
                         e <= 0.0 ? 0.0 : qBound(0.05, e, 4.0));
}

bool Config::viewAutoFit()
{
    return QSettings().value(QStringLiteral("view/autoFit"), true).toBool();
}

void Config::setViewAutoFit(bool on)
{
    QSettings().setValue(QStringLiteral("view/autoFit"), on);
}

bool Config::rememberViewport()
{
    return QSettings().value(QStringLiteral("view/remember"), false).toBool();
}

void Config::setRememberViewport(bool on)
{
    QSettings().setValue(QStringLiteral("view/remember"), on);
}

bool Config::viewPanelOpen()
{
    return QSettings().value(QStringLiteral("view/panelOpen"), false).toBool();
}

void Config::setViewPanelOpen(bool on)
{
    QSettings().setValue(QStringLiteral("view/panelOpen"), on);
}

QString Config::exportDir()
{
    if (g_haveSessionExportDir) return g_sessionExportDir;
    return QSettings().value(QLatin1String(kExportDir)).toString();
}

void Config::setExportDir(const QString& dir)
{
    // A session override wins for READS and also absorbs the write, so a
    // harness run cannot rewrite the user's remembered folder by exporting.
    if (g_haveSessionExportDir) { g_sessionExportDir = dir; return; }
    QSettings().setValue(QLatin1String(kExportDir), dir);
}

void Config::setSessionExportDir(const QString& dir)
{
    g_sessionExportDir = dir;
    g_haveSessionExportDir = true;
}

QString Config::modDir()
{
    if (g_haveSessionModDir) return g_sessionModDir;
    return QSettings().value(QLatin1String(kModDir)).toString();
}

void Config::setSessionModDir(const QString& dir)
{
    g_sessionModDir = dir;
    g_haveSessionModDir = true;
}

void Config::setModDir(const QString& dir)
{
    QSettings().setValue(QLatin1String(kModDir), dir);
}

void Config::setSessionGameDirs(const QStringList& dirs)
{
    g_sessionGameDirs = dirs;
    g_haveSessionGameDirs = true;
}

void Config::setSessionDictDir(const QString& dir)
{
    g_sessionDictDir = dir;
}

namespace { QString g_looseDir; }

void Config::setSessionLooseDir(const QString& dir) { g_looseDir = dir; }
QString Config::sessionLooseDir() { return g_looseDir; }
