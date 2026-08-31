// PreviewPane.cpp — see PreviewPane.h.
#include "preview/PreviewPane.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPaintEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtEndian>

#include "app/Config.h"
#include "util/MenuText.h"
#include "app/ExportNotifier.h"
#include "export/ExportOptions.h"
#include "view/ViewGlyphs.h"
#include "view/ViewportBar.h"
#include "view/ViewportPanel.h"
#include "fox/BcDecode.h"
#include "fox/Fox2File.h"
#include "fox/Fox2Refs.h"
#include "fox/FrigFile.h"
#include "fox/LangFile.h"
#include "view/StringsPanel.h"
#include "fox/FtexFile.h"
#include "fox/MtarFile.h"
#include "gl/GLModelWidget.h"
#include "index/ArchiveIndex.h"
#include "model/GlbExporter.h"
#include "preview/ImageView.h"
#include <QSignalBlocker>
#include "util/Extract.h"

using fox::ArchiveIndex;
using fox::IndexedFile;

// Min/max-per-column PCM16 waveform display (file scope — MSVC rejects member
// definitions in anonymous namespaces).
class WaveformWidget : public QWidget {
public:
    explicit WaveformWidget(QWidget* parent = nullptr) : QWidget(parent)
    {
        setMinimumHeight(120);
    }

    // Mixed-mono PCM16 samples (empty clears).
    void setSamples(QVector<qint16> samples)
    {
        m_samples = std::move(samples);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(28, 30, 34));
        if (m_samples.isEmpty()) {
            p.setPen(QColor(120, 120, 128));
            p.drawText(rect(), Qt::AlignCenter, QStringLiteral("no waveform"));
            return;
        }
        const int w = qMax(1, width());
        const int h = height();
        const float mid = h * 0.5f;
        p.setPen(QColor(80, 160, 235));
        const qsizetype n = m_samples.size();
        for (int x = 0; x < w; ++x) {
            const qsizetype a = n * x / w;
            const qsizetype b = qMax(a + 1, n * (x + 1) / w);
            qint16 mn = 32767, mx = -32768;
            for (qsizetype i = a; i < b && i < n; ++i) {
                mn = qMin(mn, m_samples[i]);
                mx = qMax(mx, m_samples[i]);
            }
            const float y0 = mid - mx / 32768.0f * mid;
            const float y1 = mid - mn / 32768.0f * mid;
            p.drawLine(QPointF(x, y0), QPointF(x, qMax(y1, y0 + 1.0f)));
        }
    }

private:
    QVector<qint16> m_samples;
};

namespace {

// Standard PCM16 wav → mixed-mono samples for the waveform (capped).
QVector<qint16> wavToMonoSamples(const QByteArray& wav)
{
    QVector<qint16> out;
    const audio::WemInfo w = audio::parseWem(wav);   // wav parses the same way
    if (!w.riff || w.bitsPerSample != 16 || w.channels == 0 || w.dataSize < 2)
        return out;
    const qsizetype frames =
        qMin<qsizetype>(w.dataSize / (2 * w.channels), 2000000);
    out.reserve(frames);
    const char* d = wav.constData() + w.dataOffset;
    for (qsizetype f = 0; f < frames; ++f) {
        qint32 acc = 0;
        for (int c = 0; c < w.channels; ++c)
            acc += qFromLittleEndian<qint16>(d + (f * w.channels + c) * 2);
        out.append(static_cast<qint16>(acc / w.channels));
    }
    return out;
}

QString hexDump(const QByteArray& data, int maxBytes)
{
    QString hex;
    const int n = static_cast<int>(qMin<qint64>(data.size(), maxBytes));
    hex.reserve(n * 4);
    for (int i = 0; i < n; i += 16) {
        QString line = QStringLiteral("%1  ").arg(i, 6, 16, QLatin1Char('0'));
        QString ascii;
        for (int j = i; j < i + 16 && j < n; ++j) {
            const quint8 b = static_cast<quint8>(data.at(j));
            line += QStringLiteral("%1 ").arg(b, 2, 16, QLatin1Char('0'));
            ascii += (b >= 32 && b < 127) ? QLatin1Char(static_cast<char>(b))
                                          : QLatin1Char('.');
        }
        hex += line + QStringLiteral("  ") + ascii + QLatin1Char('\n');
    }
    if (data.size() > maxBytes)
        hex += QStringLiteral("… %1 more bytes\n").arg(data.size() - maxBytes);
    return hex;
}

}  // namespace

PreviewPane::PreviewPane(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_stack = new QStackedWidget(this);
    layout->addWidget(m_stack, 1);
    m_status = new QLabel(this);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_status);

    // empty
    m_emptyPage = new QLabel(QStringLiteral("Select a file to preview"), m_stack);
    m_emptyPage->setAlignment(Qt::AlignCenter);
    m_stack->addWidget(m_emptyPage);

    // image
    m_imagePage = new QWidget(m_stack);
    {
        auto* v = new QVBoxLayout(m_imagePage);
        v->setContentsMargins(0, 0, 0, 0);
        m_imageView = new ImageView(m_imagePage);
        m_imageView->setAlphaBackground(Config::textureAlphaBg());

        // ── THE TOP BAR IS GONE, and nothing replaced it in place ────────
        // It carried RGB · R · G · B · A, Alpha BG, Fit, 1:1, Export DDS… and
        // Export PNG… — nine controls above every image, and every one of them
        // was a second way to do something that already had a first:
        //
        //   RGB/R/G/B/A  the CHANNEL STRIP beside the image is the same control
        //                with a thumbnail per channel, driven both ways.
        //   Export DDS…  the context menu on the image, and the Export menu.
        //   Export PNG…  Exports belong on a menu; a button per format on the
        //                page does not scale to the next format.
        //   Fit / 1:1    a middle-click on the image now fits it — the gesture
        //                the viewport already uses to reset its camera, so the
        //                two views of an asset reset the same way.
        //   Alpha BG     Settings ▸ Interface. It is a preference about how
        //                every texture is drawn, not a per-image action, and it
        //                was the only thing on that bar that persisted.
        //
        // What remains above the image is nothing at all, which is the point:
        // the image gets the height back.
        v->addWidget(m_imageView, 1);
        connect(m_imageView, &ImageView::statusText, m_status, &QLabel::setText);

        // Right-click on the image: the same export set as the buttons.
        m_imageView->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_imageView, &QWidget::customContextMenuRequested, this,
                [this](const QPoint& pos) {
                    if (m_imageView->image().isNull()) return;
                    QMenu menu(this);
                    menu.addAction(QStringLiteral("Export DDS…"), this,
                                   [this] { exportImage(false); });
                    menu.addAction(QStringLiteral("Export PNG…"), this,
                                   [this] { exportImage(true); });
                    menu.addAction(QStringLiteral("Copy image"), this, [this] {
                        QApplication::clipboard()->setImage(m_imageView->image());
                    });
                    menu.exec(m_imageView->mapToGlobal(pos));
                });
    }
    m_stack->addWidget(m_imagePage);

    // model
    m_modelPage = new QWidget(m_stack);
    {
        auto* v = new QVBoxLayout(m_modelPage);
        v->setContentsMargins(0, 0, 0, 0);
        auto* bar = new QHBoxLayout();
        m_modelView = new GLModelWidget(m_modelPage);
        // The same glyphs as the Models and Customize toolbars — this pane's
        // buttons used to be abbreviations ("Wire", "Bones") because there was
        // no room for the words. There is room for an icon.
        auto* wire = foxglyph::glyphToggle(m_modelPage, 0,
                                           QStringLiteral("Wireframe"),
                                           QString());
        connect(wire, &QToolButton::toggled, m_modelView, &GLModelWidget::setWireframe);
        bar->addWidget(wire);
        auto* skel = foxglyph::glyphToggle(m_modelPage, 1,
                                           QStringLiteral("Skeleton"),
                                           QString());
        connect(skel, &QToolButton::toggled, m_modelView, [this](bool on) {
            // Through the gate, the same rule the two model tabs follow.
            if (fox::ViewportBar* bar = fox::viewportBarFor(m_modelView))
                bar->setOverlay(QStringLiteral("skeleton"), on);
            else
                m_modelView->setShowSkeleton(on);
        });
        bar->addWidget(skel);
        auto* nrm = foxglyph::glyphToggle(
            m_modelPage, 2, QStringLiteral("Normal maps"),
            QStringLiteral("Tangent-space normal mapping"));
        nrm->setChecked(true);
        connect(nrm, &QToolButton::toggled, m_modelView,
                &GLModelWidget::setNormalMapping);
        bar->addWidget(nrm);
        auto* reset = foxglyph::glyphIcon(m_modelPage, 6,
                                          QStringLiteral("Reset view"),
                                          QString());
        connect(reset, &QToolButton::clicked, m_modelView, &GLModelWidget::resetCamera);
        bar->addWidget(reset);
        auto* glbBtn = foxglyph::glyphAction(
            m_modelPage, 7, QStringLiteral("Export .glb…"), QString());
        connect(glbBtn, &QToolButton::clicked, this, &PreviewPane::exportModelGlb);
        bar->addWidget(glbBtn);
        bar->addStretch(1);
        // After the stretch, like the Models and Customize toolbars: the
        // panel toggle lives at the far right, where the panel it opens is.
        // The page's own entry goes INTO the shared menu. It used to be a
        // second handler on the same signal, which popped a second menu the
        // moment the first was dismissed.
        fox::attachViewportPanel(
            m_modelView, [this](partmenu::Context& ctx, int) {
                // The preview has no parts panel and no per-part export, so it
                // fills the model half of §4's context and leaves the part half
                // empty — those rows are then not offered rather than offered
                // and dead (§0, law 3).
                if (!m_loaded.ok) return;
                ctx.modelName =
                    m_modelView->property("foxabCaptureName").toString();
                const auto& files = fox::ArchiveIndex::instance().files();
                if (m_currentFile >= 0 && m_currentFile < files.size()) {
                    ctx.filePath = files[m_currentFile].path;
                    ctx.fileHash = QStringLiteral("0x%1")
                                       .arg(files[m_currentFile].hash, 16, 16,
                                            QLatin1Char('0'));
                }
                ctx.exportModel = [this] { exportModelGlb(); };
            });
        fox::followDisplayState(m_modelView, wire, skel);
        v->addLayout(bar);
        v->addWidget(m_modelView, 1);

    }
    m_stack->addWidget(m_modelPage);

    // text/hex
    m_textPage = new QPlainTextEdit(m_stack);
    m_textPage->setReadOnly(true);
    m_textPage->setFont(QFont(QStringLiteral("Consolas"), 9));
    m_stack->addWidget(m_textPage);

    // The STRINGS panel. A language table is a file whose content is a table
    // of strings, so its preview is that table — with the filter, the label
    // column and the TSV export that used to live in a tab of their own.
    m_stringsPage = new StringsPanel(m_stack);
    m_stack->addWidget(m_stringsPage);

    // audio
    //
    // The TRANSPORT is not on this page. §6 puts the controls around the
    // preview into the tab's panel column, and a transport that also sat here
    // would be two live spellings of one control — the defect this project
    // keeps catching. It is built as a standalone widget (transportSection())
    // that the column places; the page keeps the waveform and the format note,
    // which are the content rather than the controls.
    //
    // Parented to `this` and not to a page, because a widget the column has
    // reparented must not be destroyed when the stack changes page.
    m_transportSection = new QWidget(this);
    // HIDDEN until a column takes it. It is parented to the PreviewPane so its
    // lifetime is one object's problem, and a widget parented to a visible
    // widget WITHOUT being in a layout is drawn at its parent's top-left — so
    // on the Textures tab, which embeds a PreviewPane and has no panel for the
    // transport, "Not an audio file — nothing to play." was painted over the
    // corner of every texture. NPanel::addPanel shows it when it adds it.
    m_transportSection->hide();
    {
        auto* tv = new QVBoxLayout(m_transportSection);
        tv->setContentsMargins(6, 4, 6, 6);
        m_playBtn = new QPushButton(QStringLiteral("▶ Play"), m_transportSection);
        m_stopBtn = new QPushButton(QStringLiteral("■ Stop"), m_transportSection);
        m_saveWavBtn =
            new QPushButton(QStringLiteral("Save .wav…"), m_transportSection);
        // A GRID, not a row. Three buttons side by side need more width than a
        // panel column has: at 268 px "Save .wav…" was drawn PAST the panel's
        // right edge and read "Save .wa". That is the same failure as the
        // elided search box and the clipped popover, and like both of those it
        // was invisible in every log and obvious in the screenshot.
        //
        // Play and Stop share the top row because they are a pair; the save
        // takes the row below, where it has the whole width and cannot clip
        // however narrow the column is dragged.
        auto* grid = new QGridLayout();
        grid->setContentsMargins(0, 0, 0, 0);
        grid->addWidget(m_playBtn, 0, 0);
        grid->addWidget(m_stopBtn, 0, 1);
        grid->addWidget(m_saveWavBtn, 1, 0, 1, 2);
        for (QPushButton* b : {m_playBtn, m_stopBtn, m_saveWavBtn})
            b->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        tv->addLayout(grid);
        m_transportNote = new QLabel(m_transportSection);
        m_transportNote->setWordWrap(true);
        tv->addWidget(m_transportNote);
        tv->addStretch(1);
    }
    m_audioPage = new QWidget(m_stack);
    {
        auto* v = new QVBoxLayout(m_audioPage);
        m_waveform = new WaveformWidget(m_audioPage);
        v->addWidget(m_waveform, 1);
        m_audioInfo = new QLabel(m_audioPage);
        m_audioInfo->setWordWrap(true);
        m_audioInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->addWidget(m_audioInfo);

        connect(m_playBtn, &QPushButton::clicked, this, [this] {
            if (ensureWavDecoded()) audio::playWav(m_wavData);
        });
        connect(m_stopBtn, &QPushButton::clicked, this,
                [] { audio::stopPlayback(); });
        connect(m_saveWavBtn, &QPushButton::clicked, this, [this] {
            if (!ensureWavDecoded()) return;
            QString base = m_lastBaseName;
            if (base.endsWith(QLatin1String(".wem"))) base.chop(4);
            const QString out = QFileDialog::getSaveFileName(
                this, QStringLiteral("Save wav"),
                QDir(Config::exportDir()).filePath(base + QStringLiteral(".wav")),
                QStringLiteral("WAVE audio (*.wav)"));
            if (out.isEmpty()) return;
            Config::setExportDir(QFileInfo(out).absolutePath());
            QSaveFile f(out);
            if (f.open(QIODevice::WriteOnly) && f.write(m_wavData) == m_wavData.size())
                f.commit();
        });
    }
    m_stack->addWidget(m_audioPage);
}

bool PreviewPane::showingStrings() const
{
    return m_stack && m_stringsPage && m_stack->currentWidget() == m_stringsPage;
}

int PreviewPane::sliceCount() const
{
    return m_lastDds.isEmpty() ? 1 : fox::bc::ddsSliceCount(m_lastDds);
}

void PreviewPane::showSlice(int slice)
{
    const int n = sliceCount();
    if (m_lastDds.isEmpty() || slice < 0 || slice >= n || slice == m_slice)
        return;
    const QImage img = fox::bc::decodeDdsSlice(m_lastDds, slice);
    if (img.isNull()) return;
    m_slice = slice;
    // The CHANNEL survives, because stepping through slices to compare them is
    // the whole point and re-choosing the channel each step would defeat it.
    const ImageView::Channel keep = m_imageView->channel();
    showImagePage(img, n > 1 ? QStringLiteral("%1 — slice %2 of %3")
                                   .arg(m_lastCaption)
                                   .arg(slice + 1)
                                   .arg(n)
                             : m_lastCaption);
    m_imageView->setChannel(keep);
}

void PreviewPane::clear()
{
    m_currentFile = -1;
    m_stack->setCurrentWidget(m_emptyPage);
    m_status->clear();
    m_imageView->clear();   // see showTextPage
}

void PreviewPane::showImagePage(const QImage& img, const QString& caption)
{
    m_imageView->setImage(img, caption);
    m_stack->setCurrentWidget(m_imagePage);
}

void PreviewPane::showTextPage(const QString& text)
{
    // The image view is a sibling page, not a child of this one, so it keeps
    // whatever it last held unless it is told. Its channel strip watches it.
    m_imageView->clear();
    m_textPage->setPlainText(text);
    m_stack->setCurrentWidget(m_textPage);
}

void PreviewPane::showHexPage(const QByteArray& data)
{
    // The image view is a sibling page, not a child of this one, so it keeps
    // whatever it last held unless it is told. Its channel strip watches it.
    m_imageView->clear();
    m_textPage->setPlainText(hexDump(data, 4096));
    m_stack->setCurrentWidget(m_textPage);
}

// The transport's enabled state and the sentence that explains it, together,
// from one place. They were set apart and drifted immediately: a non-audio
// file left the buttons from the previous .wem and no note at all. The panel
// says WHY it is dead rather than only being dead — three greyed buttons and
// no sentence is the state someone files a bug about.
void PreviewPane::setTransportState(bool enabled, const QString& note)
{
    if (m_playBtn) m_playBtn->setEnabled(enabled);
    if (m_stopBtn) m_stopBtn->setEnabled(enabled);
    if (m_saveWavBtn) m_saveWavBtn->setEnabled(enabled);
    if (m_transportNote) m_transportNote->setText(note);
}

void PreviewPane::showFile(int fileIdx)
{
    // The transport belongs to the COLUMN now, so it is on screen whatever the
    // preview is showing. Reset here, on every file, and let the audio path
    // below re-enable it — otherwise it kept the last .wem's state while a
    // model was on screen: three live-looking buttons and no sentence saying
    // what they would act on.
    setTransportState(false, QStringLiteral("Not an audio file — nothing to "
                                            "play."));

    // ANNOUNCED ON THE WAY OUT, not on the way in. Emitting first meant the
    // slice control asked sliceCount() while m_lastDds still held the PREVIOUS
    // file — so a volume texture never showed the control, and a 2D texture
    // after one did, offering slices it does not have.
    struct Announce {
        PreviewPane* p;
        ~Announce() { Q_EMIT p->sourceChanged(); }
    } announce{this};
    m_currentFile = fileIdx;
    m_lastDds.clear();
    if (fileIdx < 0) {
        clear();
        return;
    }
    const ArchiveIndex& index = ArchiveIndex::instance();
    const IndexedFile& f = index.files()[fileIdx];
    const QString ext = ArchiveIndex::extensionOf(f);
    m_lastBaseName = f.path.section(QLatin1Char('/'), -1);
    m_status->setText(f.path);

    if (ext == QLatin1String("ftex")) {
        QString err;
        int missing = 0;
        m_lastDds = extract::assembleFtexToDds(f, &err, &missing);
        m_slice = 0;
        const QImage img = fox::bc::decodeDds(m_lastDds);
        if (!img.isNull()) {
            fox::FtexFile t;
            const QByteArray raw = index.readFile(f);
            QString caption = m_lastBaseName;
            if (t.parse(raw))
                caption = QStringLiteral("%1 — %2%3")
                              .arg(m_lastBaseName, t.describe(),
                                   missing ? QStringLiteral(" (%1 mips streamed away)")
                                                 .arg(missing)
                                           : QString());
            m_lastCaption = caption;
            const int slices = fox::bc::ddsSliceCount(m_lastDds);
            if (slices > 1)
                caption += QStringLiteral(" — slice 1 of %1").arg(slices);
            showImagePage(img, caption);
        } else {
            showTextPage(QStringLiteral("Texture preview failed: %1").arg(err));
        }
        return;
    }

    if (ext == QLatin1String("fmdl")) {
        m_loaded = modelload::load(fileIdx,
                                   Config::pbrEnabled(Config::PbrView::Files)
                                       ? modelload::PbrMode::Full
                                       : modelload::PbrMode::Basic);
        if (m_loaded.ok) {
            // Which game this file came out of, for the Auto environment.
            {
                const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
                if (fileIdx >= 0 && fileIdx < ix.files().size())
                    m_modelView->setSceneGame(ix.gameOf(ix.files()[fileIdx]));
            }
            m_modelView->setModel(m_loaded.uploads, m_loaded.textures,
                                  m_loaded.skeleton, m_loaded.normalMaps,
                                  m_loaded.pbr);
            // The stem a capture from this viewport should be named with —
            // the same property the Models and Customize viewports publish.
            {
                QString cap = m_lastBaseName;
                const int dot = cap.lastIndexOf(QLatin1Char('.'));
                if (dot > 0) cap.truncate(dot);
                m_modelView->setProperty("foxabCaptureName", cap);
                m_modelView->setProperty("foxabCaptureFileIdx", m_currentFile);
            }
            m_status->setText(
                QStringLiteral("%1 — %2 · %3 base textures · %4 normal maps")
                    .arg(f.path, m_loaded.model.describe())
                    .arg(m_loaded.texturesFound)
                    .arg(m_loaded.normalMapsFound));
            m_stack->setCurrentWidget(m_modelPage);
        } else {
            showTextPage(QStringLiteral("Model load failed: %1").arg(m_loaded.error));
        }
        return;
    }

    // Everything else needs the bytes (bounded).
    if (f.size > 128u * 1024 * 1024) {
        showTextPage(QStringLiteral("%1\n\n%2 bytes — too large to preview; use Extract.")
                         .arg(f.path)
                         .arg(f.size));
        return;
    }
    const QByteArray data = index.readFile(f);
    if (data.isEmpty() && f.size != 0) {
        showTextPage(QStringLiteral("Could not read entry."));
        return;
    }

    if (ext == QLatin1String("wem")) {
        showAudioPage(fileIdx, data);
        return;
    }

    if (ext == QLatin1String("mtar")) {
        // Animation archive: header + clip listing (play clips on a model in
        // the Models or Customize tab's animation bar).
        fox::MtarFile mtar;
        if (mtar.parse(data)) {
            QString t = QStringLiteral("%1\nMTAR v%2 — %3 clips%4\n")
                            .arg(f.path)
                            .arg(mtar.version())
                            .arg(mtar.clips().size())
                            .arg(mtar.isV2()
                                     ? QStringLiteral(
                                           " — shared layout: %1 units, %2 segments")
                                           .arg(mtar.layout().unitCount)
                                           .arg(mtar.layout().segmentCount)
                                     : QStringLiteral(" (v1 / Ground Zeroes)"));
            t += QStringLiteral(
                "Pick this archive in the Models tab's animation bar to play "
                "clips on a model.\n\n");
            const int listMax = qMin(2000, static_cast<int>(mtar.clips().size()));
            for (int i = 0; i < listMax; ++i)
                t += QStringLiteral("%1  %2\n")
                         .arg(i, 5)
                         .arg(mtar.clips()[i].name);
            if (mtar.clips().size() > listMax)
                t += QStringLiteral("… %1 more\n").arg(mtar.clips().size() - listMax);
            showTextPage(t);
        } else {
            showTextPage(QStringLiteral("MTAR parse failed: %1")
                             .arg(mtar.errorString()));
        }
        return;
    }

    if (fox::Fox2File::isFox2(data)) {
        // Fox2 entity binary (.parts/.fox2/.vfsm/…): full entity → property
        // dump with names and paths resolved from the file's own string table.
        fox::Fox2File fx;
        if (fx.parse(data)) {
            QString t = QStringLiteral("%1\nFox2 entity file — %2 entities\n\n")
                            .arg(f.path)
                            .arg(fx.entities().size());
            for (const fox::Fox2Entity& e : fx.entities()) {
                t += QStringLiteral("entity %1 (v%2)\n")
                         .arg(e.className)
                         .arg(e.version);
                const auto dump = [&](const QVector<fox::Fox2Property>& ps) {
                    for (const fox::Fox2Property& p : ps) {
                        t += QStringLiteral("  %1 : %2 = ")
                                 .arg(p.name, p.typeName());
                        const int n = qMin(6, p.values.size());
                        for (int i = 0; i < n; ++i) {
                            if (i) t += QStringLiteral(", ");
                            if (i < p.mapKeys.size())
                                t += p.mapKeys[i] + QLatin1Char('=');
                            t += p.valueText(i);
                        }
                        if (p.values.size() > n)
                            t += QStringLiteral(" … (%1 total)")
                                     .arg(p.values.size());
                        t += QLatin1Char('\n');
                    }
                };
                dump(e.statics);
                dump(e.dynamics);
                t += QLatin1Char('\n');
            }
            showTextPage(t);
        } else {
            // Fall back to the reference summary when the parse fails.
            const QStringList refs = fox::fox2AssetPaths(data);
            QString t =
                QStringLiteral("%1\nFox2 entity file (parse failed: %2) — "
                               "%3 asset reference(s)\n\n")
                    .arg(f.path, fx.errorString())
                    .arg(refs.size());
            for (const QString& r : refs) t += r + QLatin1Char('\n');
            showTextPage(t);
        }
        return;
    }

    if (ext == QLatin1String("frig")) {
        fox::FrigFile frig;
        if (frig.parse(data)) {
            QString t = QStringLiteral("%1\nFox rig — %2 rig units, %3 bound bones\n\n")
                            .arg(f.path)
                            .arg(frig.rigUnitCount())
                            .arg(frig.bones().size());
            static const char* kTypeNames[] = {
                "None", "Root", "Orientation", "TwoBone", "LocalOrientation",
                "LocalTransform", "ThreeBoneLikeTwoBone", "Transform", "Arm",
                "LocalTransformSrt", "AnimalLeg", "MultiLocalOrientation",
                "TwoBoneTrans"};
            for (int i = 0; i < frig.units().size(); ++i) {
                const int ty = static_cast<int>(frig.units()[i].type);
                t += QStringLiteral("unit %1  %2\n")
                         .arg(i, 3)
                         .arg(QLatin1String(
                             ty >= 0 && ty <= 12 ? kTypeNames[ty] : "?"));
            }
            showTextPage(t);
        } else {
            showTextPage(QStringLiteral("frig parse failed."));
        }
        return;
    }

    // A language table previews as its own strings rather than as a hex dump.
    // The extension is split at the FIRST dot, so it arrives here as
    // "eng.lng2" / "jpn.lng2" and not as "lng2" — testing for the bare word
    // was what made this look unreachable the first time.
    if (ext.endsWith(QLatin1String("lng2"))) {
        // The panel, not a text dump. It has the filter, the label column,
        // "All tables" and the TSV export; the dump had none of them and told
        // the reader to go and find a tab that no longer exists.
        if (m_stringsPage && m_stringsPage->showTableFile(fileIdx)) {
            m_stack->setCurrentWidget(m_stringsPage);
            m_status->setText(f.path);
            return;
        }
        // Not in the panel's list — the index was rescanned under it, or this
        // is a .lng2 the language-table sweep did not pick up. Say what
        // happened rather than showing an empty panel.
        fox::LangFile lg;
        showTextPage(lg.parse(data)
                         ? QStringLiteral("%1\n%2\n\nThis table is not in the "
                                          "panel's list — rescan the archives.")
                               .arg(f.path, lg.describe())
                         : QStringLiteral("%1\nNot a readable .lng2: %2")
                               .arg(f.path, lg.errorString()));
        return;
    }

    const bool textual = ext == QLatin1String("lua") || ext == QLatin1String("txt")
        || ext == QLatin1String("xml") || ext == QLatin1String("json")
        || ext == QLatin1String("inf") || ext == QLatin1String("info");
    if (textual) {
        showTextPage(QString::fromUtf8(data.left(512 * 1024)));
        return;
    }
    showHexPage(data);
}

void PreviewPane::showAudioPage(int, const QByteArray& data)
{
    // The image view is a sibling page, not a child of this one, so it keeps
    // whatever it last held unless it is told. Its channel strip watches it.
    m_imageView->clear();
    audio::stopPlayback();
    m_wemData = data;
    m_wavData.clear();
    m_wavFailed = false;
    m_wemInfo = audio::parseWem(data);
    m_waveform->setSamples({});

    QString info = QStringLiteral("<b>%1</b>  (%2 KB)<br>")
                       .arg(m_lastBaseName)
                       .arg(data.size() / 1024);
    const bool vgm = !audio::vgmstreamPath().isEmpty();
    if (m_wemInfo.riff) {
        info += QStringLiteral("%1 — %2 channel(s), %3 Hz")
                    .arg(m_wemInfo.codec)
                    .arg(m_wemInfo.channels)
                    .arg(m_wemInfo.sampleRate);
        if (m_wemInfo.durationSec > 0)
            info += QStringLiteral(", %1 s").arg(m_wemInfo.durationSec, 0, 'f', 2);
        info += QStringLiteral("<br>");
        if (m_wemInfo.isPcm()
            || (vgm && data.size() <= 2 * 1024 * 1024)) {
            // Decode eagerly for the waveform. PCM re-wrap is free; vgmstream
            // gets a SHORT timeout here so a garbage wem (sbp sizes are
            // gap-derived) can't freeze list navigation — Play retries with
            // the full timeout.
            ensureWavDecoded(/*eager=*/true);
        } else if (!vgm) {
            info += QStringLiteral(
                "<br>Packed Wwise codec — for in-app playback and .wav export, "
                "place <code>vgmstream-cli.exe</code> in a <code>tools</code> "
                "folder beside FOXAssetBrowser.exe. "
                "(Extract the .wem and convert externally otherwise.)");
        }
    } else {
        info += QStringLiteral("Not a RIFF container.");
    }
    m_audioInfo->setText(info);
    const bool canPlay = m_wemInfo.riff && (m_wemInfo.isPcm() || vgm);
    setTransportState(canPlay,
                      canPlay ? QStringLiteral("Playing the decoded .wav.")
                              : QStringLiteral("This .wem could not be decoded "
                                               "— no vgmstream, or a codec it "
                                               "does not handle."));
    m_stack->setCurrentWidget(m_audioPage);
}

bool PreviewPane::ensureWavDecoded(bool eager)
{
    if (!m_wavData.isEmpty()) return true;
    if (m_wavFailed && !eager) return false;   // full-timeout attempt already failed
    if (!m_wemInfo.riff || m_wemData.isEmpty()) return false;
    if (m_wemInfo.isPcm()) {
        m_wavData = audio::wemToWav(m_wemData, m_wemInfo);
    } else {
        QString err;
        m_wavData = audio::convertWithVgmstream(m_wemData, &err,
                                                eager ? 3000 : 20000);
        if (m_wavData.isEmpty()) {
            if (!eager) {
                m_wavFailed = true;
                m_audioInfo->setText(m_audioInfo->text()
                                     + QStringLiteral("<br><i>%1</i>").arg(err));
            }
            return false;
        }
    }
    m_waveform->setSamples(wavToMonoSamples(m_wavData));
    return !m_wavData.isEmpty();
}

void PreviewPane::exportImage(bool asPng)
{
    if (m_imageView->image().isNull()) return;
    QString base = m_lastBaseName;
    if (base.endsWith(QLatin1String(".ftex"))) base.chop(5);
    base = fox::templatedStem(base, m_currentFile);
    const QString filter = asPng ? QStringLiteral("PNG image (*.png)")
                                 : QStringLiteral("DDS texture (*.dds)");
    const QString out = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export texture"),
        QDir(Config::exportDir())
            .filePath(base + (asPng ? QStringLiteral(".png") : QStringLiteral(".dds"))),
        filter);
    if (out.isEmpty()) return;
    Config::setExportDir(QFileInfo(out).absolutePath());
    bool ok = false;
    if (asPng) {
        ok = m_imageView->image().save(out, "PNG");
    } else if (!m_lastDds.isEmpty()) {
        QSaveFile fdds(out);
        if (fdds.open(QIODevice::WriteOnly)) {
            fdds.write(m_lastDds);
            ok = fdds.commit();
        }
    }
    if (ok)
        fox::ExportNotifier::instance().notify(
            QStringLiteral("Exported %1").arg(QFileInfo(out).fileName()),
            QFileInfo(out).absolutePath());
    m_status->setText(ok ? QStringLiteral("Exported %1").arg(out)
                         : QStringLiteral("Export failed"));
}

void PreviewPane::exportModelGlb()
{
    if (!m_loaded.ok) return;
    QString base = m_lastBaseName;
    if (base.endsWith(QLatin1String(".fmdl"))) base.chop(5);
    base = fox::templatedStem(base, m_currentFile);
    const QString out = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export glTF binary"),
        QDir(Config::exportDir()).filePath(base + QStringLiteral(".glb")),
        QStringLiteral("glTF binary (*.glb)"));
    if (out.isEmpty()) return;
    Config::setExportDir(QFileInfo(out).absolutePath());
    QString err;
    const fox::ExportOptions eo = fox::loadExportOptions();
    qInfo("export: %s", qUtf8Printable(eo.describe()));
    const bool ok = glb::exportGlb(m_loaded.model, m_loaded.textures, out, &err,
                                   nullptr, &m_loaded.normalMaps,
                                   fox::sceneOptionsFrom(eo));
    if (ok)
        fox::ExportNotifier::instance().notify(
            QStringLiteral("Exported %1").arg(QFileInfo(out).fileName()),
            QFileInfo(out).absolutePath());
    m_status->setText(ok ? QStringLiteral("Exported %1").arg(out)
                         : QStringLiteral("Export failed: %1").arg(err));
}
