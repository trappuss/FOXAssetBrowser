#include "export/ViewCapture.h"

#include <QAction>
#include <QActionGroup>
#include <QPointer>
#include <QClipboard>
#include <QGuiApplication>
#include <QCheckBox>
#include <QMenu>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QImage>
#include <QRect>
#include <QLabel>
#include <QMessageBox>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QVector>
#include <QVariant>
#include <cstring>
#include <cmath>

#include "app/Config.h"
#include "util/PartMenu.h"
#include "export/ExportOptions.h"
#include "export/GifEncoder.h"
#include "app/Hotkeys.h"
#include "gl/GLModelWidget.h"
#include "view/ViewportBar.h"
#include "view/ViewportPanel.h"

namespace fox {
namespace {

// Tightly-packed RGBA, which is what the encoder takes. The format conversion
// is the reason for the copy, not padding — Format_RGBA8888 is already
// 4*width per line — but the source can arrive in any format grabFramebuffer
// felt like.
std::vector<uint8_t> toRgba(const QImage& src)
{
    const QImage img = src.convertToFormat(QImage::Format_RGBA8888);
    std::vector<uint8_t> out(size_t(img.width()) * size_t(img.height()) * 4);
    for (int y = 0; y < img.height(); ++y)
        memcpy(out.data() + size_t(y) * size_t(img.width()) * 4,
               img.constScanLine(y), size_t(img.width()) * 4);
    return out;
}

QString framePath(const QString& gifPath, int i)
{
    const QFileInfo fi(gifPath);
    return fi.dir().filePath(QStringLiteral("%1_%2.png")
                                 .arg(fi.completeBaseName())
                                 .arg(i, 4, 10, QLatin1Char('0')));
}

}  // namespace

bool captureStill(GLModelWidget* view, const QString& pngPath, QString* error)
{
    if (!view) {
        if (error) *error = QStringLiteral("no viewport");
        return false;
    }
    const CaptureOptions o = loadCaptureOptions();
    QImage img = view->grabViewport();
    if (img.isNull()) {
        if (error) *error = QStringLiteral("the viewport rendered nothing");
        return false;
    }
    // ABOVE 100% the scene is rendered again at the larger size rather than
    // upscaled — an upscaled screenshot is exactly what nobody wants when they
    // ask for a bigger one. Below 100% a plain smooth scale is right, because
    // there is no more detail to be had than was on screen.
    if (o.imageScale > 100) {
        // DEVICE pixels, which is what grabViewport() already returned. Taking
        // the widget's logical size here made "150%" produce a SMALLER file
        // than 100% on any display with a scale factor above 1.5 — the
        // discontinuity at 99%→101% being the tell.
        const double dpr = view->devicePixelRatioF();
        const int w = qRound(view->width() * dpr * o.imageScale / 100.0);
        const int h = qRound(view->height() * dpr * o.imageScale / 100.0);
        const QImage big = view->renderAtSize(w, h);
        if (!big.isNull()) img = big;
        else
            qWarning("capture: could not render at %dx%d — saving the "
                     "viewport's own size instead", w, h);
    } else if (o.imageScale < 100) {
        img = img.scaled(qMax(16, img.width() * o.imageScale / 100),
                         qMax(16, img.height() * o.imageScale / 100),
                         Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    // The CONTAINER comes from the path the user chose, not from the setting:
    // the setting decides what the dialog offers, and once a name is on screen
    // the extension on it is the promise. A path with no extension we know
    // falls back to the setting.
    QString fmt = QFileInfo(pngPath).suffix().toLower();
    if (fmt != QLatin1String("png") && fmt != QLatin1String("jpg")
        && fmt != QLatin1String("jpeg") && fmt != QLatin1String("webp"))
        fmt = o.imageFormat;
    const int quality = (fmt == QLatin1String("png")) ? -1 : o.imageQuality;
    if (!img.save(pngPath, fmt.toUpper().toLatin1().constData(), quality)) {
        if (error)
            *error = QStringLiteral("could not write %1").arg(pngPath);
        return false;
    }
    qInfo("capture: %s (%dx%d, %s%s)", qUtf8Printable(pngPath), img.width(),
          img.height(), qUtf8Printable(fmt),
          quality >= 0 ? qUtf8Printable(QStringLiteral(" q%1").arg(quality)) : "");
    return true;
}

namespace {

// The union of every frame's non-background content, plus a margin. ONE
// rectangle for the whole turn, not one per frame: cropping each frame to its
// own bounds makes the model swim about inside a shrinking box, which is worse
// than the empty air it was trying to remove.
QRect contentBoundsOf(const QVector<QImage>& frames, QRgb bg)
{
    // FOUR EDGE SCANS per frame, not a full sweep. The full sweep was
    // frames x pixels three-channel comparisons on the GUI thread between the
    // render and the encode — 36 frames of a maximised HiDPI viewport is a few
    // hundred million of them with the window unresponsive. Walking in from
    // each edge and stopping at the first row or column that carries content
    // answers the same question and touches a fraction of the pixels, because
    // the interesting rows are the ones nearest the edges.
    QRect box;
    // A tolerance, not equality: the shader's own dither and the 8-bit round
    // trip move a flat background by a unit or two, and an exact test then
    // finds "content" in every corner.
    const auto isBg = [bg](QRgb p) {
        return qAbs(qRed(p) - qRed(bg)) <= 3 && qAbs(qGreen(p) - qGreen(bg)) <= 3
            && qAbs(qBlue(p) - qBlue(bg)) <= 3;
    };
    for (const QImage& f : frames) {
        if (f.isNull()) continue;
        const QImage im = f.convertToFormat(QImage::Format_RGB32);
        const int W = im.width(), H = im.height();
        const auto rowHasContent = [&](int y) {
            const QRgb* row = reinterpret_cast<const QRgb*>(im.constScanLine(y));
            for (int x = 0; x < W; ++x)
                if (!isBg(row[x])) return true;
            return false;
        };
        const auto colHasContent = [&](int x, int y0, int y1) {
            for (int y = y0; y <= y1; ++y)
                if (!isBg(reinterpret_cast<const QRgb*>(im.constScanLine(y))[x]))
                    return true;
            return false;
        };
        int top = 0;
        while (top < H && !rowHasContent(top)) ++top;
        if (top >= H) continue;                 // this frame is all background
        int bottom = H - 1;
        while (bottom > top && !rowHasContent(bottom)) --bottom;
        int left = 0;
        while (left < W && !colHasContent(left, top, bottom)) ++left;
        int right = W - 1;
        while (right > left && !colHasContent(right, top, bottom)) --right;
        box = box.united(QRect(left, top, right - left + 1, bottom - top + 1));
    }
    return box;
}

// Frames scaled to a percentage, keeping every frame the same size — the
// encoder rejects a set that is not.
void scaleFrames(QVector<QImage>& frames, int pct)
{
    if (pct >= 100 || frames.isEmpty() || frames.first().isNull()) return;
    const int w = qMax(16, frames.first().width() * pct / 100);
    const int h = qMax(16, frames.first().height() * pct / 100);
    for (QImage& f : frames)
        f = f.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

// Encode, and when a size budget is asked for, keep re-encoding until it fits.
// Ported from D4AssetBrowser's ExportCapture, whose comments explain why the
// order is what it is; the short version is that all three of these were wrong
// in the obvious implementation:
//
//  · CUTTING COLOURS CAN MAKE THE FILE BIGGER, because the dither amplitude is
//    scaled to palette coarseness — so the optimiser's first lever fought
//    itself. Dither is a lever of its own here, tried before resolution.
//  · SHIPPING THE LAST ATTEMPT rather than the best one means a pass that came
//    out larger is what the user gets.
//  · NIBBLING at the resolution gives up long before a file several times over
//    budget is anywhere near it. Size is close to linear in pixel count, so one
//    aimed pass at sqrt(target/actual) lands near the target.
//
// Frames are captured once; a retry only re-encodes.
bool encodeWithBudget(std::vector<std::vector<uint8_t>>& rgba, int& w, int& h,
                      const CaptureOptions& opts, std::vector<uint8_t>* out,
                      int* usedColors, bool* usedDither)
{
    const int trans = opts.transparent ? 128 : -1;
    const qint64 target = qint64(qBound(1, opts.targetMB, 200)) * 1024 * 1024;
    std::vector<uint8_t> bytes, best;
    int bestColors = opts.colors;
    bool bestDither = opts.dither;
    int bestW = w, bestH = h;

    const auto attempt = [&](int colors, bool dither) -> qint64 {
        if (!GifEncoder::encodeToBuffer(bytes, rgba, w, h, opts.delayCs, true,
                                        trans, colors, dither))
            return -1;
        if (best.empty() || bytes.size() < best.size()) {
            best = bytes;
            bestColors = colors;
            bestDither = dither;
            bestW = w;
            bestH = h;
        }
        return qint64(bytes.size());
    };

    qint64 sz = attempt(opts.colors, opts.dither);
    if (sz < 0) return false;
    if (usedColors) *usedColors = opts.colors;
    if (usedDither) *usedDither = opts.dither;
    if (!opts.optimize || sz <= target) {
        *out = bytes;
        return true;
    }

    int colors = opts.colors;
    while (sz > target && colors > 32) {
        colors = qMax(32, colors * 3 / 4);
        sz = attempt(colors, opts.dither);
        if (sz < 0) return false;
    }
    if (sz > target && opts.dither) {
        sz = attempt(colors, false);
        if (sz < 0) return false;
    }
    const bool ditherNow = (sz <= target) ? bestDither : false;
    for (int pass = 0; pass < 5 && sz > target; ++pass) {
        const double ratio = double(target) / double(sz);
        // 0.93 of the ideal so a slight underestimate still lands under
        // budget; floored so one pass cannot collapse the image, capped so a
        // pass always makes progress.
        double k = qBound(0.35, std::sqrt(ratio) * 0.93, 0.92);
        // ONE factor for both axes, clamped so NEITHER lands under the floor.
        // Flooring the two independently squashes a tall narrow crop a little
        // more on every pass — the width pins at 96 while the height keeps
        // shrinking — and the model comes out distorted.
        k = qMax(k, qMax(96.0 / double(w), 96.0 / double(h)));
        const int nw = qMax(96, int(w * k));
        const int nh = qMax(96, int(h * k));
        // OR, not AND: once either axis has bottomed out there is nothing left
        // to take without distorting the picture.
        if (nw >= w || nh >= h) break;
        for (std::vector<uint8_t>& f : rgba) {
            QImage im(reinterpret_cast<const uchar*>(f.data()), w, h,
                      QImage::Format_RGBA8888);
            const QImage sc = im.scaled(nw, nh, Qt::IgnoreAspectRatio,
                                        Qt::SmoothTransformation)
                                  .convertToFormat(QImage::Format_RGBA8888);
            std::vector<uint8_t> nb(size_t(nw) * size_t(nh) * 4u);
            for (int y = 0; y < nh; ++y)
                std::memcpy(nb.data() + size_t(y) * size_t(nw) * 4u,
                            sc.constScanLine(y), size_t(nw) * 4u);
            f = std::move(nb);
        }
        w = nw;
        h = nh;
        sz = attempt(colors, ditherNow);
        if (sz < 0) return false;
    }
    w = bestW;
    h = bestH;
    // What was ACTUALLY used, not what was asked for — the summary line below
    // printed the requested palette while the optimiser had quietly settled on
    // a quarter of it.
    if (usedColors) *usedColors = bestColors;
    if (usedDither) *usedDither = bestDither;
    qInfo("gif: %s — %.2f MB against a %.2f MB target, %dx%d, %d colour(s), "
          "dither %s",
          (!best.empty() && qint64(best.size()) <= target)
              ? "target met"
              : "TARGET NOT REACHABLE, shipping the smallest encode",
          double(best.size()) / (1024.0 * 1024.0),
          double(target) / (1024.0 * 1024.0), bestW, bestH, bestColors,
          bestDither ? "on" : "off");
    *out = best;
    return true;
}

}  // namespace

bool captureTurntable(GLModelWidget* view, const QString& gifPath,
                      const CaptureOptions& opts, QString* error)
{
    if (!view) {
        if (error) *error = QStringLiteral("no viewport");
        return false;
    }
    // ── The size guard, BEFORE anything is rendered ─────────────────────
    // grabFramebuffer() returns DEVICE pixels, so a maximised viewport on a
    // HiDPI screen is four times the area the widget reports. At 360 frames
    // that is tens of gigabytes held twice over — once as QImages and once as
    // the packed RGBA the encoder takes — and the first sign of it would be
    // the process dying. The encoder's own stated comfort zone is ~600x600 and
    // ~120 frames; this refuses well above that rather than at it, because a
    // 900px turn of 36 frames is an ordinary thing to ask for.
    const qint64 px = qint64(view->width()) * qint64(view->height())
        * qint64(view->devicePixelRatioF() * view->devicePixelRatioF() + 0.5);
    const qint64 bytes = px * 4 * qint64(qBound(2, opts.frames, 360)) * 2;
    constexpr qint64 kBudget = 1024LL * 1024 * 1024;   // 1 GiB, both copies
    if (bytes > kBudget) {
        if (error)
            *error = QStringLiteral(
                         "that turn would need about %1 GB of memory (%2 "
                         "frames at %3x%4 device pixels). Make the viewport "
                         "smaller or ask for fewer frames.")
                         .arg(double(bytes) / (1024.0 * 1024.0 * 1024.0), 0,
                              'f', 1)
                         .arg(opts.frames)
                         .arg(qRound(view->width() * view->devicePixelRatioF()))
                         .arg(qRound(view->height() * view->devicePixelRatioF()));
        return false;
    }

    // Transparency is a property of the RENDER, not of the encode: every
    // fragment the shaders write is opaque, so without a clear alpha of 0
    // there is nothing for the encoder's threshold to find. Set around the
    // capture and cleared after, so the on-screen viewport is untouched.
    if (opts.transparent) view->setTransparentBackground(true);
    QVector<QImage> frames = view->renderTurntable(opts.frames);
    if (opts.transparent) view->setTransparentBackground(false);
    return encodeGif(view, frames, gifPath, opts, error);
}

// The ENCODE half, split out so a turntable and an animation share it. They
// differ only in where the frames come from — one spins the camera, the other
// steps a clip — and everything after that (the crop, the scale, the palette,
// the optional PNGs, the report) is the same work and was worth exactly one
// copy of.
bool encodeGif(GLModelWidget* view, QVector<QImage>& frames,
               const QString& gifPath, const CaptureOptions& opts,
               QString* error)
{
    if (!view) {
        if (error) *error = QStringLiteral("no viewport");
        return false;
    }
    if (frames.isEmpty() || frames.first().isNull()) {
        if (error) *error = QStringLiteral("the viewport rendered nothing");
        return false;
    }
    // CROP FIRST, then scale: cropping removes pixels the scale would
    // otherwise spend its budget on, and doing it the other way round throws
    // away resolution on the model to keep resolution on the empty air.
    if (opts.cropToModel) {
        const QColor bg = view->backgroundColor();
        QRect box = contentBoundsOf(frames, bg.rgb());
        if (box.isValid()) {
            const int margin = qMax(4, qMin(box.width(), box.height()) / 20);
            box.adjust(-margin, -margin, margin, margin);
            box = box.intersected(frames.first().rect());
            // GIF frames are byte-packed rows; an odd width is legal but a
            // 1-pixel box is not something anyone asked for.
            if (box.width() >= 16 && box.height() >= 16) {
                for (QImage& f : frames) f = f.copy(box);
                qInfo("capture: cropped to the model — %dx%d", box.width(),
                      box.height());
            }
        }
    }
    scaleFrames(frames, opts.scalePct);
    const int w0 = frames.first().width();
    const int h0 = frames.first().height();
    int w = w0;
    int h = h0;

    std::vector<std::vector<uint8_t>> rgba;
    rgba.reserve(size_t(frames.size()));
    for (const QImage& f : frames) {
        // Every frame has to be the same size or the encoder rejects the set.
        // They are all one grabFramebuffer() of one unchanging widget, so this
        // is a guard against a resize landing mid-capture rather than an
        // expected case.
        if (f.width() != w || f.height() != h) {
            if (error)
                *error = QStringLiteral(
                    "the viewport changed size during the capture");
            return false;
        }
        rgba.push_back(toRgba(f));
    }

    int pngWritten = 0, pngFailed = 0, leftovers = 0;
    if (opts.alsoFrames) {
        for (int i = 0; i < frames.size(); ++i) {
            if (frames[i].save(framePath(gifPath, i), "PNG")) ++pngWritten;
            else ++pngFailed;
        }
        // A SHORTER turn than last time leaves the old turn's higher-numbered
        // frames sitting beside the new ones, and anything importing the
        // sequence picks up a mixture. Counted and said rather than deleted:
        // this writes into a folder the user chose, and quietly removing files
        // out of it is not this tool's business.
        for (int i = frames.size(); i < 512; ++i) {
            if (!QFile::exists(framePath(gifPath, i))) break;
            ++leftovers;
        }
    }

    // Opaque: the viewport clears to the environment's background, so there is
    // no alpha to key on and asking for 1-bit transparency would only cost a
    // palette entry.
    //
    // Encoded to MEMORY and written through QFile, not through the encoder's
    // own fopen: that takes a narrow std::string, which on Windows is read in
    // the ANSI codepage — so a turn saved into C:\Users\Игорь\ failed, or
    // landed under a mojibake name, while the PNG beside it (written by Qt)
    // was fine.
    std::vector<uint8_t> gif;
    int usedColors = opts.colors;
    bool usedDither = opts.dither;
    if (!encodeWithBudget(rgba, w, h, opts, &gif, &usedColors, &usedDither)) {
        if (error)
            *error = QStringLiteral("the GIF encoder failed on %1 frame(s) at "
                                    "%2x%3")
                         .arg(frames.size())
                         .arg(w)
                         .arg(h);
        return false;
    }
    QFile out(gifPath);
    if (!out.open(QIODevice::WriteOnly)
        || out.write(reinterpret_cast<const char*>(gif.data()),
                     qint64(gif.size()))
               != qint64(gif.size())
        || !out.flush()) {
        if (error)
            *error = QStringLiteral("could not write %1: %2")
                         .arg(gifPath, out.errorString());
        return false;
    }
    out.close();

    qInfo("capture: %s — %lld frame(s), %dx%d, %d cs/frame, %d colour(s), "
          "dither %s, %lld KB%s",
          qUtf8Printable(gifPath), qint64(frames.size()), w, h, opts.delayCs,
          usedColors, usedDither ? "on" : "off", qint64(gif.size() / 1024),
          opts.alsoFrames
              ? qUtf8Printable(QStringLiteral(" · %1 PNG(s) beside it%2%3")
                               .arg(pngWritten)
                               .arg(pngFailed ? QStringLiteral(", %1 FAILED")
                                                    .arg(pngFailed)
                                              : QString())
                               .arg(leftovers
                                        ? QStringLiteral(", %1 frame file(s) "
                                                         "from a longer "
                                                         "earlier turn are "
                                                         "still there")
                                              .arg(leftovers)
                                        : QString()))
              : "");
    return true;
}

QString captureStillInteractive(QWidget* parent, GLModelWidget* view,
                                const QString& suggestedName)
{
    // The dialog's default extension and filter come from the SETTING, so the
    // two cannot disagree — a setting of "webp" that still opened on ".png"
    // would silently be overridden by the name every single time.
    const CaptureOptions co = loadCaptureOptions();
    const QString ext = co.imageFormat;
    const QString filter =
        QStringLiteral("%1 image (*.%2);;PNG image (*.png);;"
                       "JPEG image (*.jpg);;WebP image (*.webp)")
            .arg(ext.toUpper(), ext);
    const QString out = QFileDialog::getSaveFileName(
        parent, QStringLiteral("Save viewport image"),
        QDir(Config::exportDir())
            .filePath(suggestedName + QLatin1Char('.') + ext),
        filter);
    if (out.isEmpty()) return {};
    Config::setExportDir(QFileInfo(out).absolutePath());
    QString err;
    if (!captureStill(view, out, &err)) {
        QMessageBox::warning(parent, QStringLiteral("Save image"), err);
        return {};
    }
    return out;
}

QString captureTurntableInteractive(QWidget* parent, GLModelWidget* view,
                                    const QString& suggestedName)
{
    // The settings first, then the path: the frame count changes how long the
    // whole thing takes and how big the file is, and asking for a name before
    // asking what is being made gets the order backwards.
    CaptureOptions o = loadCaptureOptions();

    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("Turntable"));
    auto* v = new QVBoxLayout(&dlg);
    auto* f = new QFormLayout();
    auto* frames = new QSpinBox(&dlg);
    frames->setRange(2, 360);
    frames->setValue(o.frames);
    frames->setToolTip(QStringLiteral(
        "Images in one full revolution. 36 is a step of ten degrees."));
    f->addRow(QStringLiteral("Frames"), frames);
    auto* delay = new QSpinBox(&dlg);
    delay->setRange(1, 100);
    delay->setValue(o.delayCs);
    delay->setSuffix(QStringLiteral(" cs"));
    delay->setToolTip(QStringLiteral(
        "Centiseconds per frame — the GIF format's own unit. 4 is 25 fps."));
    f->addRow(QStringLiteral("Frame delay"), delay);
    auto* colors = new QSpinBox(&dlg);
    colors->setRange(2, 256);
    colors->setValue(o.colors);
    colors->setToolTip(QStringLiteral(
        "GIF palette size. 256 is the format's maximum; a shaded model needs "
        "most of them."));
    f->addRow(QStringLiteral("Colours"), colors);
    v->addLayout(f);
    auto* dither = new QCheckBox(QStringLiteral("Dither"), &dlg);
    dither->setChecked(o.dither);
    dither->setToolTip(QStringLiteral(
        "Ordered (Bayer) dither. The pattern depends only on pixel position, "
        "so it is identical in every frame — it breaks up palette banding "
        "without making the surface crawl."));
    v->addWidget(dither);
    auto* alsoFrames =
        new QCheckBox(QStringLiteral("Also write the frames as PNGs"), &dlg);
    alsoFrames->setChecked(o.alsoFrames);
    v->addWidget(alsoFrames);
    auto* note = new QLabel(&dlg);
    note->setWordWrap(true);
    note->setText(QStringLiteral(
        "Captured at the viewport's current size, from the camera where it is "
        "now — so frame the model first. The turn is around Y; the elevation "
        "and distance stay where you left them."));
    v->addWidget(note);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted) return {};

    o.frames = frames->value();
    o.delayCs = delay->value();
    o.colors = colors->value();
    o.dither = dither->isChecked();
    o.alsoFrames = alsoFrames->isChecked();

    const QString out = QFileDialog::getSaveFileName(
        parent, QStringLiteral("Save turntable"),
        QDir(Config::exportDir())
            .filePath(suggestedName + QStringLiteral(".gif")),
        QStringLiteral("Animated GIF (*.gif)"));
    // Persisted only once the user has committed to making one. Saving on OK
    // meant backing out at the file dialog still changed the settings.
    if (out.isEmpty()) return {};
    saveCaptureOptions(o);
    Config::setExportDir(QFileInfo(out).absolutePath());
    QString err;
    if (!captureTurntable(view, out, o, &err)) {
        QMessageBox::warning(parent, QStringLiteral("Turntable"), err);
        return {};
    }
    return out;
}

QString captureAnimationInteractive(QWidget* parent, GLModelWidget* view,
                                    const QString& suggestedName)
{
    if (!view) return {};
    auto provider = view->animationFrameProvider();
    if (!provider) {
        QMessageBox::information(
            parent, QStringLiteral("Animation GIF"),
            QStringLiteral(
                "This viewport has no clip to record. Load a model and pick a "
                "clip in the animation bar first."));
        return {};
    }
    CaptureOptions o = loadCaptureOptions();

    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("Animation GIF"));
    auto* v = new QVBoxLayout(&dlg);
    auto* f = new QFormLayout();
    auto* frames = new QSpinBox(&dlg);
    frames->setRange(2, 360);
    frames->setValue(qBound(2, o.frames, 360));
    frames->setToolTip(QStringLiteral(
        "How many frames to sample across the clip. Fewer than the clip has "
        "steps through it; more than it has repeats poses, so this is capped "
        "at the clip's own length by the tab that records it."));
    f->addRow(QStringLiteral("Frames"), frames);
    auto* delay = new QSpinBox(&dlg);
    delay->setRange(1, 100);
    delay->setValue(o.delayCs);
    delay->setSuffix(QStringLiteral(" cs"));
    delay->setToolTip(QStringLiteral(
        "Centiseconds per frame — the GIF format's own unit. 4 is 25 fps, "
        "which is close to how this viewport plays a clip back."));
    f->addRow(QStringLiteral("Frame delay"), delay);
    auto* colors = new QSpinBox(&dlg);
    colors->setRange(2, 256);
    colors->setValue(o.colors);
    f->addRow(QStringLiteral("Colours"), colors);
    v->addLayout(f);
    auto* dither = new QCheckBox(QStringLiteral("Dither"), &dlg);
    dither->setChecked(o.dither);
    v->addWidget(dither);
    auto* alsoFrames =
        new QCheckBox(QStringLiteral("Also write the frames as PNGs"), &dlg);
    alsoFrames->setChecked(o.alsoFrames);
    v->addWidget(alsoFrames);
    auto* note = new QLabel(&dlg);
    note->setWordWrap(true);
    note->setText(QStringLiteral(
        "The CLIP that is loaded, sampled evenly from its first frame to its "
        "last, from the camera where it is now — so frame the model first. "
        "The camera does not move; unlike a turntable, what changes is the "
        "pose.\n\nThe clip is put back where you left it afterwards."));
    v->addWidget(note);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted) return {};

    o.frames = frames->value();
    o.delayCs = delay->value();
    o.colors = colors->value();
    o.dither = dither->isChecked();
    o.alsoFrames = alsoFrames->isChecked();

    const QString out = QFileDialog::getSaveFileName(
        parent, QStringLiteral("Save animation"),
        QDir(Config::exportDir()).filePath(suggestedName + QStringLiteral(".gif")),
        QStringLiteral("Animated GIF (*.gif)"));
    if (out.isEmpty()) return {};
    saveCaptureOptions(o);
    Config::setExportDir(QFileInfo(out).absolutePath());

    if (o.transparent) view->setTransparentBackground(true);
    QVector<QImage> shots = provider(o.frames);
    if (o.transparent) view->setTransparentBackground(false);
    QString err;
    if (!encodeGif(view, shots, out, o, &err)) {
        QMessageBox::warning(parent, QStringLiteral("Animation GIF"), err);
        return {};
    }
    return out;
}

void addViewportCaptureActions(QMenu* menu, QWidget* parent,
                               GLModelWidget* view, bool enabled)
{
    if (!menu) return;
    menu->addSeparator();
    // The stem the dialogs open on. Whatever the tab published on the viewport
    // — see ViewportPanel::suggestedName, which reads the same property, so
    // the menu and the panel name a capture the same way.
    const QString stem =
        view ? view->property("foxabCaptureName").toString().trimmed()
             : QString();
    // An unset property converts to 0, which is a valid file index — absent
    // has to mean -1 or a nameless viewport borrows file 0's game and hash.
    const QVariant capIdx =
        view ? view->property("foxabCaptureFileIdx") : QVariant();
    const QString name =
        stem.isEmpty()
            ? QStringLiteral("viewport")
            : templatedStem(stem, capIdx.isValid() ? capIdx.toInt() : -1);
    QAction* still = menu->addAction(
        QStringLiteral("Save viewport image…"), parent,
        [parent, view, name] { captureStillInteractive(parent, view, name); });
    still->setEnabled(enabled && view);
    Hotkeys::Role::set(still, Hotkeys::Role::saveImage());
    QAction* turn = menu->addAction(
        QStringLiteral("Turntable GIF…"), parent, [parent, view, name] {
            captureTurntableInteractive(parent, view, name);
        });
    turn->setEnabled(enabled && view);
    Hotkeys::Role::set(turn, Hotkeys::Role::turntable());
    // THE SAME ACTION THE CAMERA PAGE OFFERS, from the same builder — §12's
    // rule is that an object offers the same actions wherever it is reached,
    // and a "Save animation GIF…" that existed only inside a popover page was
    // the one capture the right-click menu did not know about. Enabled on the
    // same condition the page uses: whether the tab installed a provider.
    QAction* anim = menu->addAction(
        QStringLiteral("Save animation GIF…"), parent, [parent, view, name] {
            captureAnimationInteractive(parent, view, name);
        });
    anim->setEnabled(enabled && view && bool(view->animationFrameProvider()));
    Hotkeys::Role::set(anim, Hotkeys::Role::animGif());
    if (hasExportSettingsOpener())
        menu->addAction(QStringLiteral("Export settings…"), parent,
                        [] { openExportSettings(); });
}

namespace {
// File-scope rather than a member of anything: there is one application and
// one settings dialog, and passing an opener down through every menu builder
// would be a parameter that is the same on every call.
std::function<void()> g_exportSettingsOpener;
}  // namespace

void setExportSettingsOpener(std::function<void()> fn)
{
    g_exportSettingsOpener = std::move(fn);
}

bool hasExportSettingsOpener() { return bool(g_exportSettingsOpener); }

void openExportSettings()
{
    if (g_exportSettingsOpener) g_exportSettingsOpener();
}

bool copyViewportToClipboard(GLModelWidget* view, QString* error)
{
    if (!view) {
        if (error) *error = QStringLiteral("no viewport");
        return false;
    }
    const QImage img = view->grabViewport();
    if (img.isNull()) {
        if (error) *error = QStringLiteral("the viewport gave back nothing");
        return false;
    }
    QGuiApplication::clipboard()->setImage(img);
    qInfo("capture: %dx%d to the clipboard", img.width(), img.height());
    return true;
}

// Harness only: build the part menu, log its shape, and do NOT raise it.
static bool g_partMenuDumpOnly = false;
bool partMenuDumpOnly() { return g_partMenuDumpOnly; }
void setPartMenuDumpOnly(bool on) { g_partMenuDumpOnly = on; }

void installViewportContextMenu(
    GLModelWidget* view,
    std::function<void(partmenu::Context&, int)> pageContext)
{
    if (!view) return;
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(
        view, &QWidget::customContextMenuRequested, view,
        [guard = QPointer<GLModelWidget>(view), pageContext](const QPoint& at) {
            // exec() below spins a NESTED event loop. The connection's context
            // object protects the emit, not a body already running, so the
            // viewport is re-checked on the far side of that loop as well.
            GLModelWidget* view = guard;
            if (!view) return;
            const bool has = view->hasGeometry();

            // WHAT WAS RIGHT-CLICKED. The menu is about that, and a menu that
            // does not know what is under the cursor can only offer things
            // that are true everywhere — which is how this one came to carry
            // the wireframe switch, the skeleton switch and a twelve-entry
            // debug-channel submenu, none of which had anything to do with
            // the right-click. All three live on the viewport's own bar and
            // its Graphics popover, a few pixels above the cursor.
            const int part = has ? view->pickMeshAt(at) : -1;

            // ── What a right-click MEANS here ───────────────────────────
            // Three cases, and they are the three the user asked for:
            //
            //  · on a part that is part of the current SELECTION — the whole
            //    selection becomes the menu's subject and turns blue. The menu
            //    then acts on all of it, which is what "right-click these
            //    four and export them" has to do.
            //  · on a part that is NOT selected — that one alone becomes the
            //    subject, in blue. The selection is left alone: a right-click
            //    asks a question about something, it does not re-aim what you
            //    had already chosen.
            //  · on the part that is the ONLY thing selected, or on empty
            //    space — the selection is DROPPED. Right-clicking the thing
            //    you just selected is how people say "never mind", and there
            //    was no other way to deselect without hiding something.
            const QSet<int> sel = view->selectedMeshes();
            QSet<int> subject;
            if (part < 0 || (sel.size() == 1 && sel.contains(part))) {
                view->clearSelection();
                Q_EMIT view->meshPicked(-1);
            } else if (sel.contains(part)) {
                subject = sel;
            } else {
                subject.insert(part);
            }
            view->setContextMeshes(subject);

            QMenu m(view);
            const bool hasPart = part >= 0 && !subject.isEmpty();
            const bool hidden = view->hiddenMeshes().contains(part);
            const int subjectCount = subject.size();

            // ── §4's blocks, from THE shared builder ────────────────────
            // Header, export, copy, this-part, all-parts. Written out here
            // once and in SceneTree::showContextMenu once, with a comment in
            // the second pointing at the first — which is a duplicate wearing
            // a cross-reference. util/PartMenu.h is the one implementation.
            //
            // What only this side can resolve is filled in; what it cannot is
            // left null and simply does not appear (§0, law 3). The viewport
            // knows the parts and the visibility; the PAGE knows the model's
            // name, path and hash, and supplies them through pageMenu's own
            // context below.
            {
                partmenu::Context ctx;
                ctx.subject = subject;
                ctx.activePart = subject.isEmpty() ? -1 : part;
                ctx.hasGeometry = has;
                ctx.anyHidden = !view->hiddenMeshes().isEmpty();
                ctx.subjectHidden = hidden;
                // The page names it: only the owning tab can resolve a
                // submesh to its material name or the scene to a file, and a
                // header reading "part 12" is not worth the row it costs.
                if (pageContext) pageContext(ctx, ctx.activePart);
                if (has) {
                    ctx.framePart = [view, part] { view->frameMesh(part); };
                    ctx.selectPart = [view, subject, part] {
                        view->setSelectedMeshes(subject, part);
                        Q_EMIT view->meshPicked(part);
                    };
                    ctx.setHidden = [view, subject](bool hide) {
                        for (int id : subject) view->setMeshVisible(id, !hide);
                        Q_EMIT view->meshVisibilityChanged();
                    };
                    ctx.isolatePart = [view, subject, part] {
                        view->setSelectedMeshes(subject, part);
                        view->isolatePicked();
                    };
                    ctx.showAll = [view] { view->unhideAll(); };
                    // §4's All-parts block, complete. It was Show-all alone,
                    // because hiddenMeshes() only knows the half already off;
                    // meshIds() is the full set and Hide all / Invert fall out
                    // of it. A right-click on empty space gets all three.
                    ctx.hideAll = [view] {
                        for (int id : view->meshIds())
                            view->setMeshVisible(id, false);
                        Q_EMIT view->meshVisibilityChanged();
                    };
                    ctx.invert = [view] {
                        for (int id : view->meshIds())
                            view->setMeshVisible(id, !view->meshVisible(id));
                        Q_EMIT view->meshVisibilityChanged();
                    };
                }
                partmenu::build(&m, ctx);
            }

            // ── The view ────────────────────────────────────────────────
            // Two entries, both about the camera, both things that have no
            // other home on the viewport. Everything else that used to be
            // here — shading, overlays, channels, the panel — has one.
            if (has) {
                // Only if the menu does not already end in one. The builder
                // closes its last block with a separator, and adding a second
                // here drew "--- ---" with nothing between them.
                if (!m.actions().isEmpty() && !m.actions().last()->isSeparator())
                    m.addSeparator();
                m.addAction(QStringLiteral("Reset view"), view,
                            [view] { view->resetCamera(); });
                m.addAction(QStringLiteral("Copy image to clipboard"), view,
                            [view] {
                                QString err;
                                if (!copyViewportToClipboard(view, &err))
                                    qWarning("capture: %s", qUtf8Printable(err));
                            });
            }
            addViewportCaptureActions(&m, view, view, has);

            // Always on and bounded: one line naming what the menu holds.
            // A part menu cannot be photographed — it is its own top-level
            // window — and §4 is a SHAPE, so the shape goes in the log where
            // it can be checked. Same play as --filemenu and --menudump.
            {
                QStringList shape;
                for (QAction* a : m.actions())
                    shape << (a->isSeparator()
                                  ? QStringLiteral("---")
                                  : a->text() + (a->isEnabled()
                                                     ? QString()
                                                     : QStringLiteral(" (off)")));
                qInfo("partmenu: part %d, subject %lld — %s", part,
                      static_cast<long long>(subject.size()),
                      qUtf8Printable(shape.join(QLatin1String(" | "))));
            }
            // The harness stops here: exec() spins a nested loop that a
            // headless run never leaves, so the shape is logged (above) and
            // the menu is not raised. One branch rather than a second builder
            // — a dump of a menu built differently is a dump of nothing.
            if (partMenuDumpOnly()) { view->setContextMeshes({}); return; }
            if (!m.isEmpty()) m.exec(view->mapToGlobal(at));
            // Fires however the menu closed — action chosen, Esc, click-away —
            // so the transient outline never outlives the menu that put it up.
            view->setContextMeshes({});
        });
}

}  // namespace fox
