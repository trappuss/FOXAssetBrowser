// ExportNotifier.h — one place every export path reports itself (template §9).
//
// Ported from D4AssetBrowser's `app/ExportNotifier.h`. Any tab calls
// `ExportNotifier::instance().notify(summary, folder)` after a successful
// export; MainWindow listens and shows ONE consistent report with a "Show in
// folder" action — so every export path says what it wrote and where, the same
// way, instead of the mix this tool had: a status-bar line in the Models tab,
// a label under the Customize builder, a silent success in the preview pane
// and nothing at all from the shared context-menu actions.
//
// `folder` is the directory to reveal; omit it for an export with no single
// home. `text` is a short human summary — what was written, how many, and
// anything true of the FILE that the user could not otherwise tell.
#pragma once
#include <QObject>
#include <QString>
#include <QStringList>

#include "export/ExportOptions.h"

namespace fox {

class ExportNotifier : public QObject {
    Q_OBJECT
public:
    static ExportNotifier& instance()
    {
        static ExportNotifier n;
        return n;
    }

    void notify(const QString& text, const QString& folder = QString())
    {
        emit exported(text, folder);
    }

    // ── "Which convention is this .glb in?" ─────────────────────────────
    // A one-line tail for a MODEL export's report: "  ·  Y up, scale ×1, with
    // skeleton". The single most common question about an exported file is why
    // an import came in rotated or a hundred times too big, and the answer is
    // a setting the user changed once and forgot.
    //
    // Takes the RESOLVED options rather than reading QSettings, and that is the
    // design: an option's setting and its EFFECT are not always the same thing
    // — an animated export overrides "no skeleton", because glTF animates node
    // transforms and a file with no joints has nothing to animate — so
    // reporting the raw key would describe a file that was not written.
    //
    // Deliberately silent about anything that only SOME paths honour. A shared
    // line that confidently named an option the file does not have would be
    // worse than saying nothing; a path that knows such a fact appends it
    // itself.
    static QString glbOptionsLine(const fox::ExportOptions& opt)
    {
        QStringList parts;
        parts << (opt.zUp ? QStringLiteral("Z up") : QStringLiteral("Y up"));
        if (opt.scale != 1.0)
            parts << QStringLiteral("scale x%1").arg(opt.scale, 0, 'g', 4);
        parts << (opt.skeleton ? QStringLiteral("with skeleton")
                               : QStringLiteral("no rig"));
        if (opt.connectPoints) parts << QStringLiteral("connect points");
        return QStringLiteral("  ·  ") + parts.join(QStringLiteral(", "));
    }

signals:
    void exported(const QString& text, const QString& folder);

private:
    ExportNotifier() = default;
};

}  // namespace fox
