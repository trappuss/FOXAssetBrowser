// PreviewPane.h — THE contextual viewer: hand it any indexed file and it
// switches to the right presentation seamlessly — 3D viewport for models,
// zoomable 2D viewer for textures, text for scripts/configs, an audio panel
// for .wem, hex for everything else. Shared by the Files tab (and anything
// else that wants "just show me this file").
#pragma once
#include <QWidget>

#include "audio/WemFile.h"
#include "preview/ModelLoader.h"

class GLModelWidget;
class ImageView;
class StringsPanel;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QToolButton;
class WaveformWidget;

class PreviewPane : public QWidget {
    Q_OBJECT
public:
    explicit PreviewPane(QWidget* parent = nullptr);

    // Show an ArchiveIndex file. Chooses the viewer by extension/content.
    void showFile(int fileIdx);
    void clear();

    // The STRINGS panel (template §6), which is what a .lng2 previews as. Held
    // here rather than built per file: it caches every table it has parsed and
    // the label set it built, and both are expensive enough that rebuilding
    // them on every click through a folder of language tables would be felt.
    // The Files tab reaches it to keep its table combo and the file tree in
    // step, and the devshot harness drives it.
    StringsPanel* stringsPanel() const { return m_stringsPage; }
    // Whether the strings panel is the page currently on screen. The Export
    // menu asks, so it only offers a TSV of the strings when strings are what
    // the user is looking at.
    bool showingStrings() const;

    // The image viewer itself, for a tab that wants to hang its own controls
    // off it — the Textures tab's channel strip mirrors and drives it. Handed
    // out rather than proxied, because a proxy for six calls is six chances for
    // the proxy and the view to disagree about state.
    ImageView* imageView() const { return m_imageView; }

    // ── §6: the controls that belong in the tab's panel column ──────────
    // The audio transport, as a standalone widget for the column to place. It
    // is NOT also on the audio page — this is a move, not a copy. Enabled only
    // when there is something decodable in hand, and it says why when there is
    // not.
    QWidget* transportSection() const { return m_transportSection; }

    // ── Volume slices (template §7's "cubemap/array face selector") ──────
    // Fox writes a volume texture as a depth>1 DDS and never as a cubemap, so
    // the thing to step through here is SLICES. 1 for everything ordinary.
    int sliceCount() const;
    int currentSlice() const { return m_slice; }
    // Re-decode the texture already in hand at another slice. Cheap: the DDS
    // is assembled once and kept, so this is a decode, not another read.
    void showSlice(int slice);

Q_SIGNALS:
    // A different FILE is on screen (not a different slice of the same one).
    // The Textures tab rebuilds its slice control from this.
    void sourceChanged();

public:

private:
    void showImagePage(const QImage& img, const QString& caption);
    void showTextPage(const QString& text);
    void showHexPage(const QByteArray& data);
    void exportImage(bool asPng);
    void exportModelGlb();
    void showAudioPage(int fileIdx, const QByteArray& data);
    // Enabled state and its explanation together — see the definition.
    void setTransportState(bool enabled, const QString& note);
    // Decode m_wemData to wav (PCM re-wrap or vgmstream), cache + waveform.
    // eager = short vgmstream timeout (page-show path; Play retries longer).
    bool ensureWavDecoded(bool eager = false);

    QStackedWidget* m_stack = nullptr;
    QLabel* m_status = nullptr;

    // pages
    QLabel* m_emptyPage = nullptr;
    QWidget* m_imagePage = nullptr;
    ImageView* m_imageView = nullptr;
    QWidget* m_modelPage = nullptr;
    GLModelWidget* m_modelView = nullptr;
    QPlainTextEdit* m_textPage = nullptr;
    StringsPanel* m_stringsPage = nullptr;
    QWidget* m_audioPage = nullptr;
    QWidget* m_transportSection = nullptr;
    QLabel* m_transportNote = nullptr;
    QLabel* m_audioInfo = nullptr;
    WaveformWidget* m_waveform = nullptr;
    QPushButton* m_playBtn = nullptr;
    QPushButton* m_stopBtn = nullptr;
    QPushButton* m_saveWavBtn = nullptr;
    QByteArray m_wemData;
    QByteArray m_wavData;      // decoded, lazily
    bool m_wavFailed = false;  // decode failed — don't retry every click
    audio::WemInfo m_wemInfo;

    int m_currentFile = -1;
    modelload::LoadedModel m_loaded;   // kept for glb export
    QByteArray m_lastDds;              // kept for DDS export
    QString m_lastBaseName;
    QString m_lastCaption;   // without the slice suffix
    int m_slice = 0;
};
