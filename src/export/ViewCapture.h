// ViewCapture.h — the viewport as a picture, and as a turn.
//
// Two things a model browser is asked for constantly and this one could not
// do: "send me a shot of that" and "send me a turnaround". Both were a
// screen-capture tool and a video editor away, which means both were done at
// whatever size the window happened to be, with the toolbar in shot.
//
// This grabs the GL surface itself — no toolbar, no N-panel, no window
// chrome — and for a turn it steps the camera a whole revolution and writes
// either an animated GIF or a numbered PNG sequence. The GIF encoder is the
// one from D4AssetBrowser; see export/GifEncoder.h for why it is a copy.
//
// Nothing here knows about characters, parts or archives: it takes a viewport
// and a path. The tabs own the "what should it be called" question, because
// only they know what is in the scene.
#pragma once
#include <QString>

#include <functional>

#include "util/PartMenu.h"

#include "export/ExportOptions.h"   // fox::CaptureOptions

class GLModelWidget;
class QMenu;
class QWidget;

namespace fox {

// ── Who opens the export settings ───────────────────────────────────────────
// The viewport's right-click menu offers "Export settings…", and template §10
// puts those settings in Settings ▸ Export rather than in a modal of their
// own. This menu is built in export/ and must not reach into app/ to open the
// application's settings dialog, so the application installs the opener once
// at startup and the menu calls it.
//
// Unset — a harness run that builds a viewport without a main window — the
// entry is simply absent, which is better than an entry that does nothing.
void setExportSettingsOpener(std::function<void()> fn);
bool hasExportSettingsOpener();
void openExportSettings();

// The viewport as a PNG. Returns false and fills `error` on failure.
bool captureStill(GLModelWidget* view, const QString& pngPath,
                  QString* error = nullptr);

// One full revolution to `gifPath` (and, with alsoFrames, a PNG sequence
// beside it). Blocks: a 36-frame turn of a normal-sized viewport is 36 renders
// and one quantization pass, and a progress dialog for that would be more
// interruption than information. It REFUSES rather than blocks for a long time
// when the frames would not fit in memory — the frame count times the
// viewport's DEVICE pixels, held twice over, is checked before the first
// render and reported with the numbers in it.
bool captureTurntable(GLModelWidget* view, const QString& gifPath,
                      const CaptureOptions& opts, QString* error = nullptr);

// Ask for a path and settings, then do it. `suggestedName` is the stem the
// file dialog opens on — the tab's own name for what is in the scene.
// Returns the file written, or an empty string if the user cancelled.
//
// Both remember the folder as the shared export directory, so saving a
// screenshot moves where the next .glb export opens. That is one folder for
// everything this tool writes, which is the behaviour it already had.
QString captureStillInteractive(QWidget* parent, GLModelWidget* view,
                                const QString& suggestedName);
QString captureTurntableInteractive(QWidget* parent, GLModelWidget* view,
                                    const QString& suggestedName);

// The three viewport entries every tab's Export menu should carry, added in one
// call so the wording and the order cannot drift between tabs: save an image,
// record a turntable, and open the .glb export settings. `enabled` is the
// tab's own "is there anything in the viewport" test.
// Record the LOADED CLIP as a GIF. The camera stays put; what changes is the
// pose. The frames come from the viewport's animation frame provider, which
// the owning tab installs — only the tab knows what a clip is or how to step
// one, and only the viewport knows how to render.
QString captureAnimationInteractive(QWidget* parent, GLModelWidget* view,
                                    const QString& suggestedName);

// The shared encode: crop, scale, palette, write, and the optional PNGs. A
// turntable and an animation differ only in where their frames come from.
bool encodeGif(GLModelWidget* view, QVector<QImage>& frames,
               const QString& gifPath, const CaptureOptions& opts,
               QString* error = nullptr);

void addViewportCaptureActions(QMenu* menu, QWidget* parent,
                               GLModelWidget* view, bool enabled);

// The viewport as a picture, on the CLIPBOARD. The one thing a screenshot is
// most often wanted for is pasting it somewhere, and going through a file
// dialog to do that is three steps too many.
bool copyViewportToClipboard(GLModelWidget* view, QString* error = nullptr);

// Right-click a viewport and get a menu about WHAT YOU CLICKED.
//
// It used to be a menu about the viewport in general: a wireframe switch, a
// skeleton switch, a twelve-entry debug-channel submenu and an "open the view
// panel" entry — every one of them a duplicate of a control sitting on the
// viewport's own bar a few pixels from the cursor, and none of them anything
// to do with the right-click. What is left is the part under the pointer
// (frame, select, hide, isolate), the two camera actions with no other home,
// and the captures.
//
// One builder for all three viewports (Models, Customize, the Files preview),
// for the same reason the popovers are one class: a menu forked per tab
// drifts, and the drift shows up as "that entry exists on the other page".
//
// `pageMenu` lets the OWNING PAGE put its entries at the top of this same
// menu, and hands it the picked submesh so it can title the menu and offer
// "Export part" — only the page can resolve a submesh to a material name. It
// exists so a page does not install a second handler on the same signal, which
// is two menus in a row rather than one menu with more in it. -1 means the
// click landed on empty space.
// `pageContext` lets the OWNING PAGE fill in what only it can resolve — the
// model's name, path and hash, the part's material name, and the export
// callbacks. The viewport supplies the parts and the visibility. Both halves
// then go through partmenu::build(), which is the one implementation of §4.
//
// It replaced a hook that let the page APPEND its own rows, and every page
// that used it had written its own header and its own export entry — three
// spellings of the block the builder now owns.
// Harness only (--partmenu): the viewport's right-click builds its menu, logs
// the shape, and returns without raising it — exec() spins a nested event loop
// that a headless run never leaves. §4 is a shape and this is how it is checked.
bool partMenuDumpOnly();
void setPartMenuDumpOnly(bool on);

void installViewportContextMenu(
    GLModelWidget* view,
    std::function<void(partmenu::Context&, int)> pageContext = {});

}  // namespace fox
