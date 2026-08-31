// GLModelWidget.h — the shared 3D viewport: orbit/pan/zoom camera, textured or
// flat-shaded meshes, wireframe and skeleton overlays. Static bind-pose view
// (FMDL vertex buffers are already in model space).
//
// GL 3.3 core through Qt's function wrappers; textures arrive as pre-decoded
// RGBA QImages (the CPU DXT decode in fox::bc), so no compressed uploads ever
// reach the driver — that path crashed an NVIDIA driver in the D4 project.
#pragma once
#include <QColor>
#include <QElapsedTimer>
#include <QImage>
#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QSet>
#include <QVector>

#include <functional>
#include <memory>

class QTimer;

#include "anim/AnimMath.h"
#include "gl/ViewEnvironment.h"

class QOpenGLTexture;

// Floats per interleaved vertex: pos3 + normal3 + uv2 + tangent4. Every place
// that walks the buffer (upload, bounding box, CPU skinning) uses this, so the
// stride can never drift out of sync with the attribute pointers.
constexpr int kVertexFloats = 12;

// One renderable mesh: interleaved [pos3 normal3 uv2 tangent4] + u16/u32 indices.
// joints/weights (4 per vertex, skeleton-space indices) are carried for
// CPU-skinned animation playback; empty = rigid mesh.
struct GLMeshUpload {
    QVector<float> interleaved;   // kVertexFloats per vertex
    QVector<quint32> indices;
    QVector<quint16> joints;
    QVector<float> weights;
    int materialSlot = -1;        // index into the texture set
    int groupId = -1;             // parts filtering
    // A stable id for THIS submesh, independent of groupId.
    //
    // groupId is coarse and means different things in different tabs — a mesh
    // group in the Models tab, a whole equipped part in the Customize tab —
    // and both of those meanings are load-bearing elsewhere (group transforms
    // seat attachments by it). The scene tree needs to switch one submesh off
    // without disturbing either, so it gets an id of its own. -1 = not
    // individually addressable, which is how every existing caller behaves.
    int meshId = -1;
};

// The PBR half of one material slot, beyond base colour and normal map.
//
// Fox packs its surface parameters into an SRM ("SpecularMap_Tex_LIN") whose
// three channels are, measured across 768 shipped SRM textures:
//   R = ambient occlusion   (67% of texels above 0.88, long tail down)
//   G = roughness           (a bell centred near 0.55, never 0 — an eye reads
//                            0.13, a gun 0.48, cloth 0.82)
//   B = reflection mask     (61% exactly 0, never above 0.63 — a sparse mask
//                            for environment reflection, not a metalness map)
// TRM is a greyscale subsurface amount. LAYER + LAYERMASK are the runtime
// colouring pair: the layer is a flat colour swatch (or a camo pattern) and
// the mask says where on the surface it applies. `layerMul` / `layerBlend`
// come from the material's SHADER NAME, not from which textures happen to be
// bound, because a model can carry a layer pair its shader never reads.
//
// Every image here may be null; the shader falls back to a sane constant for
// each one independently, so a partial set is not a broken material.
struct GLPbrMaterial {
    QImage material;      // SRM
    QImage translucent;   // TRM
    QImage layer;         // Layer_Tex_SRGB
    QImage layerMask;     // LayerMask_Tex_LIN
    QImage subNormal;     // SubNormalMap_Tex_NRM — detail normal, unswizzled
    // The asset each of the four ACTUALLY resolved to, in the same order.
    // Not the path the model declares: a variation or a gear colour rebinds
    // the slot, so the model's own reference is the thing that was replaced.
    // A debug panel that showed the declared path next to the substituted
    // image would be describing the one case it exists to explain.
    // Empty where nothing was loaded. Costs four QStrings per material and
    // nothing at all when they are not filled in.
    QString materialSource, translucentSource, layerSource, layerMaskSource;
    QString subNormalSource;
    // Which role the `material` slot actually came from. Normally the SRM's
    // SpecularMap_Tex_LIN, but a material that binds no SRM falls back to a
    // plain RoughnessMap_Tex_LIN — whose single channel is roughness, not the
    // SRM's occlusion/roughness/reflection triple. A debug panel that captions
    // one as the other is worse than one that shows nothing.
    quint32 materialRole = 0;

    // The MTM: which of the four regions each texel belongs to. Fox packs the
    // region into a greyscale map quantised to four levels, measured centres
    // 33 / 96 / 159 / 222 — exactly (2k+1)*255/8, so the decode is
    // floor(v * 4) clamped to 0..3.
    QImage matParamMap;
    QString matParamMapSource;
    // The four FMTT presets this material selects, resolved from its
    // MatParamIndex_0..3 parameters. Region 0 applies everywhere when there is
    // no MTM, which is the single-material case and by far the common one.
    //   presetF0[i]      Fresnel reflectance at normal incidence
    //   presetSpec[i]    specular tint (metals are coloured, dielectrics white)
    //   presetTrans[i]   translucency
    // Defaults are the dielectric 0.04 / white / 0, which is what this
    // renderer assumed before the table existed — so a material with no
    // parameters, or an install with no .fmtt, renders exactly as before.
    float presetF0[4] = {0.04f, 0.04f, 0.04f, 0.04f};
    float presetSpec[4][3] = {{1, 1, 1}, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}};
    float presetTrans[4] = {0, 0, 0, 0};
    // AnisotropicRoughness, per region. Measured: 18 of the 256 presets carry
    // a non-zero one and they fall into NINE identical pairs exactly 100 apart
    // (29/129, 36/136, 38/138, 39/139, 45/145, 55/155, 62/162, 66/166,
    // 67/167) — the table is two banks of the same materials. The hair
    // materials select 129 and 29, which are the same preset in both banks:
    // F0 0.1608 (four times the dielectric, and that IS hair's specular
    // strength) with AnisotropicRoughness 0.898. Two of the other pairs are
    // gold and silver at 0.898, which is brushed metal — a path this renderer
    // does not have, so the value is consumed by the hair lobe only.
    float presetAniso[4] = {0, 0, 0, 0};
    // What MatParamIndex_0..3 actually said, for the debug panel. -1 = the
    // material does not carry that slot; 256 is the game's own "none".
    int presetIndex[4] = {-1, -1, -1, -1};
    int materialTypes = 1;   // 1, 2, 3 or 4, from the shader name

    // How the runtime colour LAYER is tiled across the surface, from the
    // material's URepeat_UV / VRepeat_UV / UShift_UV / VShift_UV parameters.
    //
    // The repeat pair is a GENERAL UV control that is OVERLOADED by shader
    // family, so it is applied here to the layer ONLY. Measured over 3,394
    // materials in TPP/GZ/MGO3/Survive, 743 of which carry URepeat_UV:
    //
    //   layer-family shaders  641 carry it — 393 at 5, 83 at 3, 40 at 4,
    //                         49 at 1; exactly ONE at 50 or above
    //   every other shader    102 carry it — 23 at 50 or above, up to 150
    //
    // A rate of 5 tiles a camo swatch across a garment. A rate of 100 does
    // not — that is a fibre/dirt detail rate — and it occurs only where there
    // is NO layer for it to be about.
    //
    // The decisive case is dds0_main1_def, the Diamond Dogs fatigues, which
    // carries both readings on one model:
    //
    //   mat 0  Cloth_Dirty (no layer)      URepeat_UV 100
    //   mat 2  Blin_LayerMul_SubNorm_Dirty URepeat_UV 1, SubNorm 50
    //
    // On the material that HAS a layer the plain pair drops to 1 and the 50
    // moves to the separately-named URepeat_SubNorm_UV; on the material with
    // no layer the detail rate sits in the plain pair itself. Corpus-wide all
    // 184 materials carrying URepeat_SubNorm_UV are layer-family and all 184
    // also carry the plain pair — none carries the SubNorm pair alone — so
    // the two names are genuinely two controls, not one aliased.
    //
    // What the plain pair drives on a NON-layer shader used to be open. It is
    // not any more, and the rule turned out to be exclusive with no exceptions
    // in either direction:
    //
    //   HAS a layer      the plain pair tiles the LAYER, and a sub-normal (if
    //                    the material has one) is tiled by the separately
    //                    named URepeat_SubNorm_UV. All 184 materials carrying
    //                    that named pair are layer-family, and all 184 also
    //                    carry the plain pair — the two names coexist and mean
    //                    different things.
    //   NO layer         the plain pair tiles the SUB-NORMAL. All 31 non-layer
    //                    materials that declare a SubNormalMap_Tex_NRM and
    //                    carry the plain pair bind NO SubNorm-named pair —
    //                    with nothing to disambiguate from, the plain name is
    //                    what the authoring tool wrote.
    //
    // dds0_main1_def carries both readings on one model, which is what settled
    // it: mat 0 (Cloth_Dirty, no layer) puts 100 in the plain pair, while
    // mat 2 (LayerMul_SubNorm) drops the plain pair to 1 and moves its 50 into
    // URepeat_SubNorm_UV. See ModelLoader for where the rule is applied.
    //
    // HAIR is the one population left, and it is now a NEGATIVE result rather
    // than an open question. 51 fox3DDF_Hair materials carry the plain pair;
    // none binds a layer or a sub-normal; their roles are only Base /
    // Specular / Shift / Translucent; and their values are 1.0 on 30 and 5.0
    // on 21, never higher. The obvious remaining candidate was Shift_Tex_LIN,
    // and decoding the shipped shift maps rules it out: they are FITTED maps
    // in the base map's own UV layout — sna2's is an atlas of hair-card
    // clumps, rai0's a full-height streak sheet — and tiling a fitted atlas
    // five times would scatter the very cards it defines, which is the same
    // argument that keeps the layer MASK at 1:1. Base, Specular and
    // Translucent are fitted for the same reason.
    //
    // So on hair the plain pair tiles nothing this renderer can identify, and
    // the likeliest reading is that it is simply not read — which is what 157
    // of the 184 SubNorm pairs and 1,270-odd of the Incidence parameters are
    // doing too. 21 materials, left alone.
    //
    // The layer MASK stays at 1:1 with the rest. It says WHERE on this model
    // the colour applies — measured, it is a fitted per-model map like the
    // base — so tiling it would scatter the region it is there to define.
    //
    // SHIFT: every one of the 150 shift values in the shipped data is 0.0, so
    // nothing distinguishes vUv * repeat + shift from (vUv + shift) * repeat.
    // The shader uses the former. It is a coin flip that costs nothing today
    // and would need real data (a mod, or a FOVA table) to settle.
    float layerRepeat[2] = {1.0f, 1.0f};
    float layerShift[2] = {0.0f, 0.0f};

    // The SUB-NORMAL: a detail normal tiled over the base one, which is what
    // makes cloth read as woven and leather as grained rather than as a smooth
    // surface with a picture on it. Its own tiling pair — the fatigues tile
    // their weave 50x while the colour layer above stays at 1:1 — and its own
    // blend weight, which 17 of the 78 materials that bind one set to ZERO.
    // Bound and switched off is a real state and it is honoured.
    float subRepeat[2] = {1.0f, 1.0f};
    float subBlend = 0.0f;

    // The rim term: strength (Incidence_Color.w) and its exponent
    // (Incidence_Roughness.x). Zero strength is the neutral and is what a
    // material that names neither parameter gets.
    // The rim: strength (Incidence_Color.w), its TINT (Incidence_Color.rgb)
    // and its exponent (Incidence_Roughness.x). The tint is real and I got it
    // wrong first time round — measured over the 110 parameter rows on shaders
    // that actually name Incidence, only 74 are pure white. The other 36 carry
    // a real colour: 0.93 grey on 18, 0.856 grey on 14, a cool (0.98,0.98,1.0)
    // on 2, and two warm skin/hair rims at (1.0,0.91,0.80) and
    // (1.0,0.898,0.80). Reading only the w threw all of that away and lit
    // every rim with the sky colour instead.
    float incidence = 0.0f;
    float incidenceTint[3] = {1.0f, 1.0f, 1.0f};
    float incidencePower = 4.0f;

    // HAIR. The one family with a lighting model of its own — an anisotropic
    // highlight that runs ALONG the strand rather than a round GGX lobe, which
    // is the difference between hair and a brown plastic helmet.
    //
    // `shift` is Shift_Tex_LIN: measured, it is a FITTED map in the base map's
    // own UV layout — sna2's is an atlas of hair-card clumps, rai0's is a
    // full-height streak sheet — and it is mostly black (mean 32 and 42 of
    // 255) with bright strand streaks, not the mid-grey a signed ±0.5 shift
    // map would be. So it is read UNSIGNED: 0 is "no shifted highlight here",
    // bright is "highlight here". That reading is ours; the data gives a map
    // and a scale and does not say which convention it is in.
    QImage shift;
    QString shiftSource;
    float hairExponent = 16.0f;   // Anistropic_Diffusion — 16 on 39 of 51
    float hairShift = 1.0f;       // HairShiftScale
    bool hair = false;            // fox3DDF_Hair*
    bool layerMul = false;
    bool layerBlend = false;
    bool skin = false;    // wrap lighting + translucency
    // Skin, hair, cloth and eyes are never metallic whatever preset they
    // select, so the energy-conservation term that removes a metal's diffuse
    // must not run on them.
    bool noMetal = false;
    bool unlit = false;   // fox3DFW_Constant*: emit the base map as-is
};

struct GLSkeletonUpload {
    QVector<float> lines;         // pairs of xyz endpoints
    // One entry per BONE, in bone order — not per line, which skips the roots.
    // The order is what makes the bone-name overlay possible without a second
    // API: a posed scene's combined palette is the parts' palettes
    // concatenated in this same order, and `translate(+bindPos) · palette[b]`
    // is the bone's world frame exactly (MGO_FACTS: the palette and the world
    // frame are one translation apart), so the widget can place a label at a
    // posed bone without anyone passing it one.
    QVector<QString> boneNames;
    QVector<float> bonePositions;   // xyz per bone, BIND world
};

// One connect point, for the hardpoints overlay. Fox's ".fcnp" sockets are
// this engine's hardpoints, and the tabs already load them for the composer
// and the exporter — the viewport just needs to be told.
struct GLConnectPoint {
    QString name;
    QVector3D pos;      // bind world
    int bone = -1;      // index into GLSkeletonUpload::boneNames, or -1
};

// The four viewport shading modes (template §5), in the order the balls are
// drawn. Mapped onto what Fox actually has:
//   Wireframe  polygon lines
//   Flat       unlit base colour — the shader's albedo channel, which is what
//              "solid, no lighting" means when every surface is textured
//   Shaded     the lit result WITHOUT the PBR map set: base colour + normal
//              map, the lighting this tool has always done
//   Rendered   the full set — SRM (AO/roughness/reflection), translucency,
//              the FMTT F0 preset, the environment rig
// Rendered needs maps that are only fetched when the owning tab asks for
// them, which is why choosing it emits shadingModeChanged rather than the
// widget quietly loading files it knows nothing about.
enum class ShadingMode : int { Wireframe = 0, Flat, Shaded, Rendered };
const char* shadingModeName(ShadingMode m);
const char* shadingModeNote(ShadingMode m);

class QAction;

class GLModelWidget : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT
public:
    explicit GLModelWidget(QWidget* parent = nullptr);
    ~GLModelWidget() override;

    // Replace the scene. `textures` maps material slot → RGBA image (may hold
    // null images — those meshes draw flat-shaded). `normalMaps` is the
    // parallel set of Fox DXT5nm tangent-space normal maps (optional).
    void setModel(QVector<GLMeshUpload> meshes, QVector<QImage> textures,
                  GLSkeletonUpload skeleton, QVector<QImage> normalMaps = {},
                  // Full-PBR maps, one per material slot. An EMPTY vector is
                  // the basic path: base colour + normal map only, exactly as
                  // before this parameter existed. A short vector is padded,
                  // so a caller that only knows about some materials is safe.
                  QVector<GLPbrMaterial> pbr = {});
    void clearModel();

    // ── Shading mode (template §5) ──────────────────────────────────────
    // ONE piece of state, not three booleans. setWireframe() and
    // setPbrShading() are still here and still mean what they meant — they
    // move this — because a second key for the same state is the bug §3.1 is
    // about, and the toolbar toggles, the popover and the shading balls all
    // drive the same viewport.
    void setShadingMode(ShadingMode m);
    ShadingMode shadingMode() const { return m_shading; }

    // ── Overlays (template §5) ──────────────────────────────────────────
    // Every one of these is state the VIEWPORT holds and the owning tab gates:
    // see ViewportOverlays in view/ViewportBar.h for the master switch
    // (template §3.3). Nothing outside that gate should call these.
    void setShowGrid(bool on);
    bool showGrid() const { return m_showGrid; }
    void setShowAxes(bool on);
    bool showAxes() const { return m_showAxes; }
    void setShowStats(bool on);
    bool showStats() const { return m_showStats; }
    void setShowBoneNames(bool on);
    bool showBoneNames() const { return m_showBoneNames; }
    void setShowConnectPoints(bool on);
    bool showConnectPoints() const { return m_showConnectPoints; }

    // The scene's connect points, for the hardpoints overlay. Empty clears it.
    void setConnectPoints(const QVector<GLConnectPoint>& points);
    bool hasConnectPoints() const { return !m_cnp.isEmpty(); }

    // ── What the HUD paints ─────────────────────────────────────────────
    // Text cannot be drawn by the line shader and QPainter cannot be mixed
    // into paintGL without resetting the state every other draw depends on,
    // so the text overlays are a transparent child widget (view/ViewportHud)
    // that asks for these three.
    QString statsText() const;
    // Screen-space label positions, already projected and culled to the
    // viewport. Empty when the overlay is off or there is nothing to label.
    QVector<QPair<QString, QPointF>> boneLabelsOnScreen() const;
    QVector<QPair<QString, QPointF>> connectLabelsOnScreen() const;

    void setGroupVisible(int groupId, bool visible);
    // Per-SUBMESH visibility, by GLMeshUpload::meshId. Independent of the
    // group switches: a mesh is drawn only when its group is on AND its own
    // id is not switched off, so the scene tree and the parts list can both be
    // right at once without either having to know about the other.
    void setMeshVisible(int meshId, bool visible);
    bool meshVisible(int meshId) const { return m_meshVisible.value(meshId, true); }
    // Every mesh id currently uploaded. §4's All-parts block needs Hide all
    // and Invert, and both need the full set — hiddenMeshes() only ever knew
    // the half that was already off, so the viewport's menu could offer
    // "Show all" and nothing else.
    QList<int> meshIds() const;
    // Which scene submesh ids are currently switched OFF. The exporters ask,
    // so a .glb matches the viewport that produced it — a submesh unticked in
    // the tree was invisible on screen and present in the file.
    QSet<int> hiddenMeshes() const;
    void clearMeshVisibility();
    // Rigid per-group model transform (attachments: an item seated at a
    // connect point follows its parent bone through this). Identity default.
    void setGroupTransform(int groupId, const QMatrix4x4& m);
    void clearGroupTransforms();
    void setWireframe(bool on);
    bool wireframe() const { return m_wireframe; }
    void setShowSkeleton(bool on);
    bool showSkeleton() const { return m_showSkeleton; }
    // Normal mapping on/off (on by default; nothing happens for materials with
    // no normal map or meshes with no tangents).
    void setNormalMapping(bool on);
    bool normalMapping() const { return m_normalMapping; }
    // True when at least one material in the current scene has a normal map.
    bool hasNormalMaps() const;

    // Physically based shading on/off for THIS viewport, live — no reload.
    //
    // Separate from the Settings switch on purpose. The setting decides
    // whether the extra maps are LOADED at all (a cost paid once, per tab);
    // this decides whether the ones already in hand are used to light the
    // surface. Toggling it flips between the flat lambert this viewport
    // always had and full PBR on the identical scene, which is the only way
    // to see what a map is actually doing.
    //
    // Turning it on with no maps loaded does nothing — hasPbrMaps() is the
    // honest test, and callers should disable their control when it is false
    // rather than offer a switch that silently has no effect.
    void setPbrShading(bool on);
    bool pbrShading() const { return m_pbrShading; }
    // Deliberately reads m_pendingPbr and NOT m_hasPbr: m_hasPbr is set inside
    // uploadPending(), which runs lazily on the next paint, so a caller asking
    // straight after setModel() would get the previous scene's answer.
    // m_pendingPbr keeps its length after upload (only the images are freed).
    bool hasPbrMaps() const { return !m_pendingPbr.isEmpty(); }
    void resetCamera();

    // ── The lighting rig, the background, the debug views and the turntable ──
    //
    // All four are viewport state, not scene state: nothing here reloads a
    // model or touches a material, so every one of them is a repaint. That is
    // the point — a roughness map can only be judged by moving the light on
    // the scene already on screen.
    //
    // The defaults reproduce this viewport exactly as it was before any of it
    // existed: the "default" environment IS the shader's former constants, the
    // key light starts on fox::legacyKeyDirection(), exposure is 1, the debug
    // view is off and the turntable is stopped. A build that never opens the
    // panel renders what it always did.
    void setEnvironment(const fox::ViewEnvironment& env);
    const fox::ViewEnvironment& environment() const { return m_env; }
    // AUTO: follow whichever game the scene's own files came out of. An
    // install can hold four games with four different looks, and one rig for
    // all of them says more about the rig than about the model. While this is
    // on, setSceneGame() drives the environment; setEnvironment() turns it
    // off, because an explicit choice must not be overwritten by the next
    // model that loads.
    void setEnvironmentAuto(bool on);
    bool environmentAuto() const { return m_envAuto; }
    // Which game the scene is. The tabs know this — the index votes a game per
    // archive — and tell the viewport, which is the only place that can then
    // apply a rig without every tab knowing about rigs.
    void setSceneGame(fox::GameId g);
    fox::GameId sceneGame() const { return m_sceneGame; }
    // Key-light direction as the panel edits it: azimuth around Y (degrees,
    // 0 = towards +Z, increasing towards +X) and elevation above the horizon.
    // Both are stored, not derived from a vector, so dragging elevation to 90
    // and back does not lose the azimuth to a degenerate pole.
    void setKeyAngles(float azimuthDeg, float elevationDeg);
    float keyAzimuth() const { return m_keyAz; }
    float keyElevation() const { return m_keyEl; }
    // The key light rides the camera instead of the world. Useful for reading
    // a normal map (the highlight stays where you are looking) and useless for
    // judging form, which is why it is off by default and a switch rather
    // than the behaviour.
    void setKeyFollowsCamera(bool on);
    bool keyFollowsCamera() const { return m_keyFollowsCamera; }
    // Multipliers on the environment's own colours. 1.0 = the preset as
    // authored; both clamp to [0, 4].
    void setKeyIntensity(float k);
    float keyIntensity() const { return m_keyGain; }
    void setAmbientIntensity(float k);
    float ambientIntensity() const { return m_ambientGain; }
    void setExposure(float e);
    float exposure() const { return m_exposure; }
    // Overrides the environment's own background. An invalid QColor clears the
    // override and hands the background back to the environment.
    void setBackgroundColor(const QColor& c);
    QColor backgroundColor() const;

    void setDebugView(fox::DebugView v);
    fox::DebugView debugView() const { return m_debug; }

    // Where the orbit camera is, for a panel that shows it. Read-only on
    // purpose: the camera is moved by resetCamera(), centerOn(), the mouse and
    // the turntable, and a fifth entry point that set yaw and pitch directly
    // would be a fifth thing to keep consistent with cameraChanged().
    // ── Projection (the gizmo's double-click) ───────────────────────────
    // Orthographic or perspective, for the same camera. The half-height is
    // derived from the distance and the same 45° field, so the switch does not
    // change how big the model looks — only whether parallel edges converge.
    void setOrthographic(bool on);
    bool orthographic() const { return m_ortho; }
    void toggleOrthographic() { setOrthographic(!m_ortho); }
    // Point the camera down one axis: 0 X, 1 Y, 2 Z. `negative` looks from the
    // other side. Keeps the centre and the distance.
    void viewAlongAxis(int axis, bool negative);

    float cameraYaw() const { return m_yaw; }
    float cameraPitch() const { return m_pitch; }
    float cameraDistance() const { return m_dist; }

    // Auto-orbit. `degPerSec` is signed and clamped to ±120 (the range the
    // panel's slider offers); the timer is created on first use and stopped
    // (not merely idle) when off, so a viewport nobody is spinning costs
    // nothing. A mouse drag still moves the camera while it runs.
    void setTurntable(bool on, float degPerSec = 22.0f);

    // ── Capture ─────────────────────────────────────────────────────────
    // The viewport as an image, at its own size — what is on screen and
    // nothing else (no toolbar, no panel: the panel is a child widget and
    // grabFramebuffer() reads the GL surface, not the widget tree).
    QImage grabViewport();
    // The scene RENDERED at an arbitrary size, for a still bigger than the
    // window. Not an upscale: it draws into an off-screen framebuffer at the
    // asked-for size, so the extra pixels carry extra detail — which is the
    // only reason to ask for them. Returns a null image if the FBO cannot be
    // made (a size the driver refuses, or no current context).
    QImage renderAtSize(int w, int h);
    // Clear with alpha 0 instead of 1, so a capture can carry a real
    // transparent background. Every fragment the shaders write is opaque, so
    // this is the ONLY thing that can make a captured pixel transparent —
    // without it a "transparent" GIF is opaque and, because transparency
    // disables the encoder's inter-frame differencing, larger as well.
    void setTransparentBackground(bool on);
    // Is there anything to capture? True between a setModel() with meshes and
    // the clearModel() that empties it. Not m_haveScene, which deliberately
    // survives the empty moment inside a rebuild so the camera is not
    // re-framed — that is the wrong question for "would a screenshot be blank".
    bool hasGeometry() const
    {
        return !m_pendingMeshes.isEmpty() || !m_meshes.isEmpty();
    }
    // One full turn, `frames` images, WITHOUT disturbing the camera the user
    // set: the yaw is stepped, each step rendered, and the original restored
    // before returning. This is the only thing that writes the camera from
    // outside — hence a method that owns the whole loop rather than a yaw
    // setter, which would be a fifth entry point to keep consistent with
    // cameraChanged().
    //
    // Renders at the widget's DEVICE-pixel size — grabFramebuffer() reads the
    // GL surface, which on a HiDPI screen is larger than the widget reports.
    // `frames` is clamped to 2…360; a turntable of one frame is a screenshot
    // and 360 is a degree apiece. The caller is responsible for the total: see
    // the memory guard in fox::captureTurntable.
    QVector<QImage> renderTurntable(int frames);
    bool turntable() const { return m_turntable; }
    float turntableSpeed() const { return m_turnSpeed; }

signals:
    // The shading mode moved. The owning tab listens because Rendered needs
    // the PBR map set and only the tab knows how to fetch it.
    void shadingModeChanged(ShadingMode mode);
    // A new scene was uploaded (or the old one cleared). What a viewport
    // OFFERS depends on the scene — the per-channel debug views are only
    // truthful when the material set was loaded with its maps — so anything
    // showing those has to be told when the answer can change.
    void sceneChanged();
    // A display switch changed, whoever changed it. Two things can now drive
    // wireframe and skeleton — the tab's own toolbar and the N-panel — and
    // without this the two would silently disagree about what is on.
    void displayChanged();
    // The camera moved, whichever moved it — drag, wheel, reset or turntable.
    // The panel shows live yaw/pitch/distance; without a signal it would have
    // to poll on a timer of its own.
    void cameraChanged();

public:

    // Animation: CPU-skin every mesh that carries joints/weights with one skin
    // matrix per skeleton bone (row-vector: skinned = v · M), and optionally
    // replace the skeleton overlay lines with reposed ones. Meshes without
    // weights stay in bind pose. clearPose() restores bind pose everywhere.
    void applyPose(const QVector<animmath::Mat4>& skin,
                   const QVector<float>& skeletonLines = {});
    void clearPose();
    // Reframe the orbit camera on an arbitrary bounding sphere (e.g. the posed
    // skeleton after a clip loads, since root motion leaves bind-pose framing).
    void centerOn(const QVector3D& center, float radius);

    // ── Picking and the keyboard (template §5) ──────────────────────────
    // Which submesh is under this widget position, or -1. Rendered, not
    // ray-cast: the CPU-side vertices are in BIND space and only the GPU knows
    // where a posed skin ended up, so a ray against the source geometry would
    // pick the wrong part of anything animated — which is most of what anyone
    // wants to click.
    int pickMeshAt(const QPoint& pos);
    // ── Selection (template §4, §5) ─────────────────────────────────────
    // A SET, not one id. Shift and Ctrl click in the viewport add to it, the
    // parts panel's own multi-select drives it, and every one of them is drawn
    // with the silhouette outline. m_picked is the ACTIVE one — the last
    // added — and is what "." frames and what the INFO panel describes; it is
    // always a member of the set, or -1.
    int pickedMesh() const { return m_picked; }
    void setPickedMesh(int meshId);            // replaces the whole selection
    const QSet<int>& selectedMeshes() const { return m_selected; }
    void setSelectedMeshes(const QSet<int>& ids, int active = -1);
    void addToSelection(int meshId);
    void removeFromSelection(int meshId);
    void toggleInSelection(int meshId);
    void clearSelection();

    // The set a context menu is ABOUT, outlined in a different colour and
    // cleared when the menu closes. Right-clicking INSIDE the selection makes
    // the whole selection the context, which is what "the menu acts on all of
    // these" has to look like.
    void setContextMeshes(const QSet<int>& ids);
    const QSet<int>& contextMeshes() const { return m_context; }

    // The selection silhouette is an OVERLAY and goes through the master gate
    // like every other one (§3.3) — nothing outside ViewportBar::reapply()
    // should call this.
    void setShowSelection(bool on);
    bool showSelection() const { return m_showSelection; }
    // The part a context menu is ABOUT, outlined in a different colour from
    // the selection and cleared when the menu closes. Two marks, two meanings:
    // a right-click that reused the selection outline read as "the selection
    // moved", and then the menu acted on something else.
    // Whether the right button has travelled far enough since it went down to
    // count as a pan rather than a click. This is the input to the
    // context-menu swallow in event(), and it is exposed because the swallow
    // itself cannot be measured from a harness — the un-swallowed path opens a
    // blocking menu. See MainWindow's --rightdrag.
    bool rightDragged() const { return m_rightDragged; }
    // Dev harness: press the right button at `from`, drag to `to`, and report
    // what happens to the context-menu event that follows. Sends real events
    // through the real handlers, so it tests what the mouse does.
    //
    // THREE outcomes, not two: a click (the menu is allowed, which is right),
    // a drag that swallowed it (the fix working), and a drag that did NOT
    // (the bug). A bool could not tell the first from the third, and the first
    // reading of this harness reported "dragged NO, menu swallowed" for a
    // 40px drag — because event() clears the flag as it swallows, so asking
    // afterwards always answered no.
    enum class RightDragResult { Click, DragSwallowed, DragLeaked };
    RightDragResult testRightDrag(const QPoint& from, const QPoint& to);
    // Dev harness: run one selection gesture at a point, through the real
    // handler. `gesture` is "pick", "ctrl" or "shift". Selection rules are
    // entirely invisible in a screenshot — an outline shows WHICH parts are
    // selected and says nothing about whether Ctrl toggled or replaced.
    void testPickGesture(const QPoint& at, Qt::KeyboardModifiers mods)
    {
        applyPickGesture(at, mods);
    }
    // Dev harness: the selection as a sorted, printable list.
    QString selectionForShot() const;
    // ── The animation frame provider (§9's GIF ladder) ──────────────────
    // A hook, not a feature this widget implements: only the OWNING TAB knows
    // what a clip is, how long it is and how to pose the model at a given
    // frame — and only this widget knows how to render. The tab installs a
    // function that steps the clip and returns rendered frames; the Camera
    // page offers "Save animation GIF…" exactly when one is installed.
    using AnimFrameProvider = std::function<QVector<QImage>(int frames)>;
    void setAnimationFrameProvider(AnimFrameProvider fn)
    {
        m_animFrames = std::move(fn);
        Q_EMIT sceneChanged();   // the Camera page re-reads its buttons
    }
    const AnimFrameProvider& animationFrameProvider() const
    {
        return m_animFrames;
    }

    // ── Field of view (the Camera page) ─────────────────────────────────
    // Degrees, vertical. 45 is what this viewport always used; the ortho
    // half-height is derived from the SAME number, so switching projection
    // does not change how big the model looks.
    void setFieldOfView(float degrees);
    // Frame every new scene, rather than keeping the camera unless the scene
    // has changed size or moved. The setting lives in Config; the viewport is
    // told rather than reading it, because this widget knows nothing about
    // QSettings and every viewport in the application comes through
    // attachViewportPanel.
    void setAutoFit(bool on) { m_autoFit = on; }
    bool autoFit() const { return m_autoFit; }
    float fieldOfView() const { return m_fov; }

    // Frame the camera on one submesh (-1 = the whole scene).
    void frameMesh(int meshId);
    // Fullscreen, with the floating exit button §5 insists on — "never trap the
    // user behind a hotkey they didn't read a tooltip for".
    void setViewportFullscreen(bool on);
    bool viewportFullscreen() const { return m_fullscreen; }
    // The shortcut list, as an overlay. F1.
    void setShowHelp(bool on);
    bool showHelp() const { return m_showHelp; }
    // Hide the SELECTED submeshes / show everything / hide everything
    // except them. All three act on the whole selection, not on one id. The
    // three H keys, and the only place that knows what they mean.
    void hidePicked();
    void unhideAll();
    void isolatePicked();
    void framePicked();
    void toggleFullscreen();
    void toggleHelp();
    // Whether this viewport's HOST implements fullscreen. False by default, so
    // the Files tab's preview does not offer a key that would set a flag
    // nothing acts on.
    void setFullscreenSupported(bool on);
    // Rebuild the viewport's shortcuts from app/Hotkeys.h. Called at
    // construction and whenever the settings dialog changes a binding.
    void installViewportShortcuts();


Q_SIGNALS:
    // A submesh was picked in the viewport (double-click), or -1 for empty
    // space. The parts tree follows it — §4's "selecting a part node selects it
    // in the viewport and vice versa", from the viewport side.
    void meshPicked(int meshId);
    // Per-submesh visibility changed FROM THE VIEWPORT (the H keys), so the
    // tree that also shows it can follow rather than drift.
    void meshVisibilityChanged();
    void fullscreenChanged(bool on);



protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;
protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    bool event(QEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;

private:
    struct MeshGpu {
        QOpenGLVertexArrayObject* vao = nullptr;
        QOpenGLBuffer vbo{QOpenGLBuffer::VertexBuffer};
        QOpenGLBuffer ibo{QOpenGLBuffer::IndexBuffer};
        int indexCount = 0;
        int materialSlot = -1;
        int groupId = -1;
        int meshId = -1;
        int srcIndex = -1;   // index into m_pendingMeshes (bind-space source)
    };

    void uploadPending();
    void applyPoseBuffers();
    // The line-shader overlay pass — see the definition.
    void drawOverlays(const QMatrix4x4& mvp);
    void rebuildConnectPointLines();
    // The overlay VAO/VBOs, which — unlike the skeleton's — are built ONCE in
    // initializeGL and must therefore NOT be torn down by destroyGpu(), which
    // runs on every scene upload. Destroyed by the destructor, and by
    // initializeGL before it rebuilds them: Qt runs initializeGL again when
    // the widget's context is re-created (a reparent, a context loss), and
    // QOpenGLBuffer::create() returns early on an already-created buffer, so
    // without this the second run wires a fresh VAO to a dead context's
    // buffer id and the grid draws nothing.
    void destroyOverlayGpu();
    void destroyGpu();
    QMatrix4x4 viewProj() const;
    // Camera position in world space — the view vector the PBR specular
    // term needs, and the same value viewProj() looks from.
    QVector3D eyePosition() const;

    // Pending CPU-side scene (uploaded lazily inside paintGL with a context).
    QVector<GLMeshUpload> m_pendingMeshes;
    QVector<QImage> m_pendingTextures;
    QVector<QImage> m_pendingNormalMaps;
    QVector<GLPbrMaterial> m_pendingPbr;
    GLSkeletonUpload m_pendingSkeleton;
    bool m_dirty = false;

    QVector<MeshGpu> m_meshes;
    QVector<QOpenGLTexture*> m_textures;
    QVector<QOpenGLTexture*> m_normalTextures;
    // Parallel to m_textures. Null entries are normal — see GLPbrMaterial.
    QVector<QOpenGLTexture*> m_materialTextures;
    QVector<QOpenGLTexture*> m_translucentTextures;
    QVector<QOpenGLTexture*> m_layerTextures;
    QVector<QOpenGLTexture*> m_layerMaskTextures;
    QVector<QOpenGLTexture*> m_matParamTextures;
    QVector<QOpenGLTexture*> m_subNormalTextures;
    QVector<QOpenGLTexture*> m_shiftTextures;
    // Anisotropy cap, resolved once against the live context. -1 = not asked
    // yet; 1 = asked and the driver does not offer it (or it was switched off).
    float m_maxAniso = -1.0f;
    QVector<GLPbrMaterial> m_pbr;
    // Whether this scene was loaded with the full map set. NOT derivable from
    // m_pbr, which is padded to the material count and so is non-empty even
    // for a basic load — reading emptiness there would silently light every
    // basic scene with PBR defaults instead of the lambert path it asked for.
    bool m_hasPbr = false;
    QOpenGLVertexArrayObject* m_skelVao = nullptr;
    QOpenGLBuffer m_skelVbo{QOpenGLBuffer::VertexBuffer};
    int m_skelVertexCount = 0;

    std::unique_ptr<QOpenGLShaderProgram> m_prog;
    std::unique_ptr<QOpenGLShaderProgram> m_lineProg;
    // Resolved once after link. setUniformValue(const char*, …) looks the
    // location up BY NAME on every call, and these eight are set per material
    // per frame — on a composed character that was nineteen thousand name
    // lookups and thirty-eight thousand string allocations a second, inside
    // paintGL.
    int m_locPresetA[4] = {-1, -1, -1, -1};
    int m_locPresetSpec[4] = {-1, -1, -1, -1};

    QHash<int, bool> m_groupVisible;
    QHash<int, bool> m_meshVisible;
    QHash<int, QMatrix4x4> m_groupTransform;
    bool m_wireframe = false;
    bool m_showSkeleton = false;
    ShadingMode m_shading = ShadingMode::Rendered;
    // What Wireframe was turned on FROM, so turning it off puts back what the
    // user chose rather than a guess. Guessing from m_hasPbr turned a
    // deliberate Shaded (PBR unticked on a scene whose maps are loaded) back
    // into Rendered on a wireframe round trip, and lost Flat outright.
    ShadingMode m_shadingBeforeWire = ShadingMode::Rendered;
    bool m_showGrid = false;
    bool m_showAxes = false;
    bool m_showStats = false;
    bool m_showBoneNames = false;
    bool m_showConnectPoints = false;
    int m_picked = -1;
    QSet<int> m_selected;
    QSet<int> m_context;
    bool m_showSelection = true;
    // The silhouette pass. A standalone pass after the draw loop; see the
    // definition for why that is safe here and was not in the first attempt.
    void drawSelectionOutline();
    // Shift adds, Ctrl toggles, a plain click replaces. Shared by the press
    // handler and the double-click handler so the two cannot diverge.
    void applyPickGesture(const QPoint& at, Qt::KeyboardModifiers mods);
    static const QVector3D kSelectColor;
    static const QVector3D kContextColor;
    QPoint m_rightPress;
    // True once the right button has travelled far enough to be a pan rather
    // than a click. Read by event() to swallow the context-menu event.
    bool m_rightDragged = false;
    bool m_fullscreen = false;
    bool m_showHelp = false;
    // Set only while the pick pass is rendering, so paintGL can take the flat
    // path without a second copy of the draw loop.
    bool m_picking = false;
    bool m_fullscreenSupported = false;
    QVector<QAction*> m_viewActions;
    QVector<GLConnectPoint> m_cnp;
    // Line geometry for the grid, the axis triad and the connect-point
    // crosses. Built in client memory and uploaded with the rest; the grid and
    // the triad are UNIT-sized and scaled per draw, so a scene change never
    // has to rebuild them.
    QOpenGLVertexArrayObject* m_gridVao = nullptr;
    QOpenGLBuffer m_gridVbo{QOpenGLBuffer::VertexBuffer};
    int m_gridVertexCount = 0;
    QOpenGLVertexArrayObject* m_axisVao = nullptr;
    QOpenGLBuffer m_axisVbo{QOpenGLBuffer::VertexBuffer};
    QOpenGLVertexArrayObject* m_cnpVao = nullptr;
    QOpenGLBuffer m_cnpVbo{QOpenGLBuffer::VertexBuffer};
    int m_cnpVertexCount = 0;
    bool m_cnpDirty = false;
    // Scene totals for the statistics overlay, counted once at upload.
    int m_statMeshes = 0, m_statTris = 0, m_statBones = 0, m_statMaterials = 0;
    // Where a bone label goes, at the CURRENT pose. Rebuilt with the pose.
    QVector<QVector3D> m_bonePosed;
    // The bind positions, kept so the posed ones can be recomputed.
    QVector<QVector3D> m_boneBind;
    QVector<QString> m_boneNames;
    // Project one world point to widget coordinates. Returns false when it is
    // behind the camera or off screen, which is the only sane thing to do with
    // a label: drawing it clamped to an edge is worse than not drawing it.
    bool projectToScreen(const QVector3D& world, QPointF* out) const;
    bool m_normalMapping = true;
    // Whether to LIGHT with the PBR model. Independent of m_hasPbr, which
    // says whether there are maps to light with at all.
    bool m_pbrShading = true;

    // Current pose (empty = bind). Applied lazily in paintGL.
    QVector<animmath::Mat4> m_pose;
    QVector<float> m_poseSkelLines;
    bool m_poseDirty = false;
    QVector<float> m_skinScratch;   // reused per-mesh skinned buffer

    // ── The rig ─────────────────────────────────────────────────────────
    fox::ViewEnvironment m_env = fox::ViewEnvironment::presets().first();
    // Non-zero only while renderAtSize() is drawing into its own buffer; the
    // projection's aspect ratio reads these in preference to the widget's.
    int m_renderW = 0, m_renderH = 0;
    float m_clearAlpha = 1.0f;
    bool m_envAuto = false;
    fox::GameId m_sceneGame = fox::GameId::Unknown;
    void applyAutoEnvironment();
    float m_keyAz = 0.0f, m_keyEl = 0.0f;   // set from legacyKeyDirection()
    bool m_keyFollowsCamera = false;
    float m_keyGain = 1.0f, m_ambientGain = 1.0f, m_exposure = 1.0f;
    QColor m_bgOverride;                    // invalid = use the environment's
    fox::DebugView m_debug = fox::DebugView::Off;
    bool m_turntable = false;
    float m_turnSpeed = 22.0f;
    QTimer* m_turnTimer = nullptr;
    QElapsedTimer m_turnClock;
    // The key light's world direction, from m_keyAz/m_keyEl — or from the
    // camera when m_keyFollowsCamera. Recomputed per frame rather than cached,
    // because the camera-follow case changes with every orbit.
    QVector3D keyDirection() const;

    // Orbit camera.
    float m_yaw = 45.0f, m_pitch = 20.0f, m_dist = 3.0f;
    bool m_ortho = false;
    float m_fov = 45.0f;
    bool m_autoFit = true;
    AnimFrameProvider m_animFrames;
    QVector3D m_center{0, 1, 0};
    QVector3D m_sceneCenter{0, 1, 0};
    float m_sceneRadius = 1.5f;
    QPoint m_lastMouse;
    // Middle button: a DRAG pans, a CLICK resets the view. Telling them apart
    // needs the press position and whether the pointer actually moved.
    QPoint m_midPress;
    bool m_midDragged = false;
    // Whether anything has ever been shown. The camera is framed to the first
    // scene and then left alone — changing a part or a texture must not throw
    // away the angle the user set.
    bool m_haveScene = false;
    float m_lastFramedRadius = 0.0f;
};
