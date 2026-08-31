// ViewEnvironment.h — the viewport's lighting rig, as data.
//
// GLModelWidget's shader has always lit with a three-part rig: a KEY
// directional light, a dim second FILL (40° off the key with the default rig,
// not opposite it — the old comment said opposite and the numbers do not), and
// a hemisphere ENV (sky above, ground below) that also drives the analytic
// environment specular the reflection mask reads against. Those four colours
// were compile-time constants in the fragment shader and the key's direction
// was a literal in paintGL, which meant the one thing a PBR viewport most
// needs — moving the light and changing what the surface is standing in —
// could not be done at all. A roughness map only reads on a highlight, and
// a highlight you cannot move is a highlight you cannot judge.
//
// So the rig is a struct, the shader takes it as uniforms, and this header
// carries the named presets. The FIRST preset reproduces the old constants
// exactly, so a build that never touches the panel renders what it always did.
//
// Intensity is folded into the colours on the way to the shader rather than
// passed as a separate scalar: the shader multiplies them by nothing else, so
// two uniforms where one will do is two chances for them to disagree.
#pragma once
#include <QColor>
#include <QString>
#include <QVector>
#include <QVector3D>

#include "index/GameId.h"

namespace fox {

struct ViewEnvironment {
    QString id;
    QString name;
    QString note;              // one line: what this is for
    // Set on the four PER-GAME rigs and left Unknown on the neutral ones. A
    // Fox Engine install holds up to four games whose art direction is not
    // the same — Ground Zeroes is a rain-lit night, TPP's Afghanistan is a
    // hard high sun, MGO's arenas are flat and even so players read each
    // other, Survive is a dust-filtered overcast — and judging a Survive
    // model under TPP's sun says more about the rig than about the model.
    // "Auto" follows the game the scene's own files came out of.
    GameId game = GameId::Unknown;
    QVector3D key{1.00f, 0.97f, 0.92f};
    QVector3D fill{0.30f, 0.36f, 0.46f};
    // Where the FILL comes from. Every shipped preset leaves it where the
    // shader's literal put it; it is a field rather than a second literal so
    // that "the rig is data" is true of the whole rig and not of four fifths
    // of it. Normalized on the way to the uniform, not here.
    QVector3D fillDir{0.5f, 0.2f, 0.7f};
    QVector3D sky{0.34f, 0.38f, 0.46f};
    QVector3D ground{0.16f, 0.15f, 0.14f};
    QColor background{33, 36, 41};   // 0.13/0.14/0.16 — the old glClearColor
    float exposure = 1.0f;

    // Every environment the panel offers, "Default" first. Static-storage
    // QVector built on first use, so no global-initialisation order to trip on.
    static const QVector<ViewEnvironment>& presets();
    // nullptr when the id is not one of them — callers fall back to presets()
    // .first() rather than to a default-constructed one, so an unknown id in a
    // saved setting cannot silently produce a rig no preset describes.
    static const ViewEnvironment* find(const QString& id);
    // The rig for a game, or nullptr for Unknown / a game with no rig. Callers
    // fall back to presets().first(), never to a default-constructed one.
    static const ViewEnvironment* forGame(GameId g);
    // The id the panel and the saved setting use for "follow the scene's
    // game". Not a preset — find() deliberately does not resolve it, so a
    // caller that forgets to handle it falls back to Default rather than to
    // something that looks like a rig and is not one.
    static QString autoId() { return QStringLiteral("auto"); }
};

// The direction the key light travelled before it was steerable. Kept as the
// default and as the origin of the azimuth/elevation the panel edits, so a
// scene does not swing round the first time this build is opened. Not
// normalized here — GLModelWidget normalizes on the way to the uniform, which
// is what the old literal did too.
inline QVector3D legacyKeyDirection() { return QVector3D(-0.4f, -0.8f, -0.45f); }

// The per-channel debug views. 0 is "off"; the rest replace the shaded result
// with one input to it, which is the only way to see what a map is actually
// doing rather than what the lighting makes of it. Kept here rather than in
// GLModelWidget so the panel and the shader agree on the numbering in one
// place — the values ARE the uDebug uniform.
enum class DebugView : int {
    Off = 0,
    Albedo,          // base colour, after the layer composite, unlit
    Normal,          // world-space shading normal, normal map included
    Roughness,       // SRM green
    ReflectionMask,  // SRM blue
    Occlusion,       // SRM red
    Translucency,    // TRM, or the material preset's own value
    Metalness,       // derived from the FMTT preset's F0
    Uv,              // fract(uv) — seams, wrapping and mirrored islands
    LightingOnly,    // the full rig on a flat grey albedo
};
const char* debugViewName(DebugView v);
const char* debugViewNote(DebugView v);
// Every view in menu order, Off first.
QVector<DebugView> debugViews();

}  // namespace fox
