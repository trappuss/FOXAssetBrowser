// GLModelWidget.cpp — see GLModelWidget.h.
#include "gl/GLModelWidget.h"

#include <QContextMenuEvent>

#include <algorithm>

#include "app/Hotkeys.h"

#include <QMouseEvent>
#include <QTimer>
#include <QOpenGLContext>
#include <QOpenGLTexture>
#include <QOpenGLFramebufferObject>
#include <QSet>
#include <QtMath>
#include <QWheelEvent>
#include <cmath>

namespace {

const char* kVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aTangent;   // xyz tangent, w bitangent sign
uniform mat4 uMvp;
uniform mat4 uModel;   // rigid per-group transform (attachments)
// SELECTION SILHOUETTE. Shifts the projected vertex by a whole number of screen
// pixels so the outline has CONSTANT width whatever the model's triangle
// density or distance. Multiplying by w cancels the perspective divide, so the
// offset is measured in NDC after it — which is what makes it pixels. Zero for
// every other pass.
uniform vec2 uNdcOffset;
out vec3 vNormal;
out vec2 vUv;
out vec4 vTangent;
out vec3 vWorld;
void main() {
    vNormal = mat3(uModel) * aNormal;
    vTangent = vec4(mat3(uModel) * aTangent.xyz, aTangent.w);
    vUv = aUv;
    vec4 wp = uModel * vec4(aPos, 1.0);
    vWorld = wp.xyz;
    gl_Position = uMvp * wp;
    gl_Position.xy += uNdcOffset * gl_Position.w;
}
)";

// Punch-through threshold, shared by the shader and the mip builder below:
// the two MUST agree or minified cutouts erode away.
constexpr float kAlphaCutoffValue = 0.35f;

const char* kFrag = R"(#version 330 core
#define kAlphaCutoff 0.35
in vec3 vNormal;
in vec2 vUv;
in vec4 vTangent;
uniform sampler2D uTex;
uniform sampler2D uNrm;
uniform sampler2D uSrm;    // R = AO, G = roughness, B = reflection mask
uniform sampler2D uTrm;    // greyscale subsurface amount
uniform sampler2D uLayer;  // runtime colour swatch (or camo pattern)
uniform sampler2D uLayerMask;
uniform sampler2D uMtm;    // region map: which of four materials each texel is
uniform int uHasMtm;
uniform int uMaterialTypes;
// Per region: (F0, translucency, anisotropy, unused) and the specular tint.
uniform vec4 uPresetA[4];
uniform vec3 uPresetSpec[4];
uniform vec2 uLayerRepeat;
uniform vec2 uLayerShift;
uniform sampler2D uSubNrm;   // detail normal laid over the base one
uniform vec2 uSubRepeat;
uniform float uSubBlend;     // 0 = none, which is also 17 materials' own value
// The rim. Strength from Incidence_Color's w, exponent from
// Incidence_Roughness' x, and zero on every material whose shader does not
// name Incidence — see ModelLoader for why the parameter alone is not enough.
uniform float uIncidence;
uniform vec3 uIncidenceTint;
uniform float uIncidencePower;
// HAIR: the strand highlight. uShift is a fitted per-model strand map, not a
// tiling pattern, so it is sampled at 1:1 like the base map beside it.
uniform sampler2D uShift;
uniform int uHair;          // 1 = fox3DDF_Hair: use the anisotropic lobe
uniform int uHasShift;
uniform float uHairExp;     // Anistropic_Diffusion, 16..64
uniform float uHairShift;   // HairShiftScale
uniform int uHairTangentU;  // 1 = read the strand along U instead of V
uniform int uHasTex;
uniform int uHasNrm;
uniform int uHasSrm;
uniform int uHasTrm;
uniform int uLayerMode;    // 0 none, 1 multiply, 2 blend
uniform int uSkin;
uniform int uNoMetal;   // skin / hair / cloth / eye: never metallic
uniform int uUnlit;
uniform int uPbr;          // 0 = the old lambert path, unchanged
uniform vec3 uLightDir;
uniform vec3 uEye;
// THE RIG, as uniforms rather than as the constants these used to be. See
// ViewEnvironment.h: key, fill, and the sky/ground hemisphere that also feeds
// the analytic environment specular. Intensity is already folded in.
uniform vec3 uKeyCol;
uniform vec3 uFillCol;
uniform vec3 uFillDir;     // direction the FILL travels, world space
uniform vec3 uSkyCol;
uniform vec3 uGroundCol;
uniform float uExposure;
// Per-channel debug view. 0 = off; the numbering is fox::DebugView.
uniform int uDebug;
// ── The pick pass ───────────────────────────────────────────────────────────
// 1 = draw every fragment as a flat id colour instead of shading it. Rendering
// the scene once into an offscreen buffer and reading one pixel is how a click
// finds the submesh under it; doing it in the SAME shader is what makes it work
// on a posed model, where the CPU-side vertices are in bind space and only the
// GPU knows where the skin actually ended up.
uniform int uPick;
uniform vec3 uPickColor;
in vec3 vWorld;
out vec4 fragColor;

// GGX / Trowbridge-Reitz, Smith height-correlated visibility, Schlick
// fresnel. Fox is a physically based renderer (its own documentation says so),
// so a Blinn half-vector power would be the wrong shape at both ends of the
// roughness range even after fitting an exponent.
float distGGX(float ndh, float a)
{
    float a2 = a * a;
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / max(3.14159265 * d * d, 1e-7);
}
float visSmith(float ndv, float ndl, float a)
{
    float a2 = a * a;
    float lv = ndl * sqrt(ndv * ndv * (1.0 - a2) + a2);
    float ll = ndv * sqrt(ndl * ndl * (1.0 - a2) + a2);
    return 0.5 / max(lv + ll, 1e-7);
}

// Normal maps arrive already unswizzled to plain RGB (the loader converts Fox's
// DXT5nm form, where x sits in ALPHA and y in GREEN, before any resize).
void main() {
    if (uPick == 1) {
        // The alpha cutout is honoured, or the transparent half of a hair card
        // picks the hair instead of whatever is behind it — which is exactly
        // the case someone is trying to click past.
        if (uHasTex == 1 && texture(uTex, vUv).a < kAlphaCutoff) discard;
        fragColor = vec4(uPickColor, 1.0);
        return;
    }
    // Guarded like every other normalize in this shader. A mesh with a zero
    // authored normal — they exist — otherwise NaNs the whole fragment on
    // both the PBR and the flat path, and a NaN fragment is a bright speck.
    vec3 n = dot(vNormal, vNormal) > 1e-12 ? normalize(vNormal) : vec3(0.0, 1.0, 0.0);
    if (uHasNrm == 1) {
        // Gram-Schmidt. Guard the length AFTER projection, not before: a unit
        // tangent that happens to be parallel to the interpolated normal
        // survives a pre-check and then normalizes a zero vector to NaN, which
        // renders as an unlit patch.
        vec3 tp = vTangent.xyz - n * dot(n, vTangent.xyz);
        if (dot(tp, tp) > 1e-8) {
            vec3 t = normalize(tp);
            vec3 b = cross(n, t) * (vTangent.w < 0.0 ? -1.0 : 1.0);
            vec3 m = texture(uNrm, vUv).rgb * 2.0 - 1.0;
            // The SUB-NORMAL, tiled at its own rate. This is the fabric weave
            // and the leather grain: the fatigues carry a cloth normal at 50x
            // while everything else on the material stays at 1:1.
            //
            // Combined by adding the detail's tangent-space slope to the base
            // one and renormalising ("UDN" blending). That is the standard
            // detail-normal combine and it behaves correctly at both ends —
            // weight 0 leaves the base untouched, weight 1 applies the detail
            // in full — but it is OUR choice of formula, not something the
            // data states. Fox's own combine is not documented anywhere I
            // could check and the shipped material only gives a weight.
            if (uSubBlend > 0.0) {
                vec3 sm = texture(uSubNrm, vUv * uSubRepeat).rgb * 2.0 - 1.0;
                m = vec3(m.xy + sm.xy * uSubBlend, m.z);
            }
            vec3 nn = mat3(t, b, n) * m;
            if (dot(nn, nn) > 1e-8) n = normalize(nn);
        }
    }
    vec4 t = uHasTex == 1 ? texture(uTex, vUv) : vec4(0.62, 0.62, 0.66, 1.0);
    if (t.a < kAlphaCutoff) discard;   // punch-through for hair/eyelash/decals
    vec3 albedo = t.rgb;

    // Runtime colouring. The base map of a customizable piece of gear ships
    // WHITE and the game multiplies a flat colour swatch through the layer
    // mask — which is why an uncoloured export looks bleached. The mask is
    // greyscale in every shipped file, so .r is the whole signal.
    if (uLayerMode != 0) {
        // The LAYER tiles; the MASK does not. The mask is a fitted per-model
        // map saying where on this garment the colour applies, and repeating
        // it would scatter the very region it defines.
        vec3 lay = texture(uLayer, vUv * uLayerRepeat + uLayerShift).rgb;
        float lm = texture(uLayerMask, vUv).r;
        albedo = uLayerMode == 1 ? albedo * mix(vec3(1.0), lay, lm)
                                 : mix(albedo, lay, lm);
    }

    // An UNLIT material has no shading to strip and no SRM to show, so most of
    // the debug views have no answer for it. The two that do — the geometry
    // ones — are answered; everything else gets the material's own colour,
    // which is what it draws anyway. Exposure applies only to the shaded view,
    // for the same reason it does not apply to a debug view anywhere else: a
    // channel readout that has been through the exposure control is not a
    // readout of the channel.
    if (uUnlit == 1) {
        if (uDebug == 2) { fragColor = vec4(n * 0.5 + 0.5, 1.0); return; }
        if (uDebug == 8) {
            fragColor = vec4(fract(vUv.x), fract(vUv.y), 0.0, 1.0);
            return;
        }
        fragColor = vec4(albedo * (uDebug == 0 ? uExposure : 1.0), 1.0);
        return;
    }

    // The flat path stays exactly what it was, plus exposure. It is skipped
    // when a debug view is on: those read the PBR inputs, and answering "show
    // me the roughness" with a lambert render of the base colour would be a
    // lie in the one mode whose whole job is not to lie.
    if (uPbr == 0 && uDebug == 0) {
        float diff = max(dot(n, -uLightDir), 0.0);
        fragColor = vec4(albedo * (0.35 + 0.65 * diff) * uExposure, 1.0);
        return;
    }

    // Surface parameters. Every default here is the value a material with no
    // SRM had implicitly under the old path, so a material that loses its SRM
    // does not change appearance — it just stops being modulated.
    // Which of the material's up-to-four regions this texel belongs to.
    //
    // Fox quantises the region into a greyscale map at four levels. Measured
    // across the shipped MTMs the value centres are 33, 96, 159 and 222 —
    // exactly (2k+1)*255/8 — so the decode is floor(v * 4), clamped because
    // the top band's own texels reach 255 and BC compression spreads each
    // centre by a few values either way.
    // Clamped to 3, NOT to uMaterialTypes - 1. Measured across the shipped
    // data, every multi-material material names all four MatParamIndex slots
    // whether its shader is 2MT, 3MT or 4MT — so a 2MT material whose MTM uses
    // band 3 has a real preset for band 3, and folding that band down into
    // region 1 would have drawn it with the wrong one.
    int region = 0;
    if (uHasMtm == 1) {
        float m = texture(uMtm, vUv).r;
        region = int(clamp(floor(m * 4.0), 0.0, 3.0));
    }
    vec3 specTint = uPresetSpec[region];
    float presetF0 = uPresetA[region].x;
    float presetTrans = uPresetA[region].y;
    float presetAniso = uPresetA[region].z;

    float ao = 1.0, rough = 0.55, reflMask = 0.0;
    if (uHasSrm == 1) {
        vec3 srm = texture(uSrm, vUv).rgb;
        ao = srm.r;
        rough = clamp(srm.g, 0.045, 1.0);
        reflMask = srm.b;
    }
    float a = rough * rough;

    vec3 eyeVec = uEye - vWorld;
    vec3 v = dot(eyeVec, eyeVec) > 1e-12 ? normalize(eyeVec) : n;
    vec3 l = -uLightDir;
    // Guard the half vector like the tangent above is guarded. When the view
    // direction is exactly opposite the key light, l + v is the zero vector
    // and normalize() yields NaN, which propagates through the whole specular
    // term and out to the framebuffer as a bright speck.
    //
    // The fallback SUPPRESSES the highlight rather than substituting one.
    // Falling back to h = n means ndh = 1, which is the GGX PEAK — on a back
    // face (nothing here enables culling) that turns the guard into the very
    // blowout it was written to remove.
    vec3 hv = l + v;
    float hOk = dot(hv, hv) > 1e-8 ? 1.0 : 0.0;
    vec3 h = hOk > 0.0 ? normalize(hv) : n;
    float ndl = max(dot(n, l), 0.0);
    float ndv = max(dot(n, v), 1e-4);
    float ndh = max(dot(n, h), 0.0);
    float vdh = max(dot(v, h), 0.0);

    // Reflectance comes from the FMTT preset this region selects, tinted by
    // the preset's own specular colour — that is where a metal gets both its
    // strength and its hue. Measured: preset 100 (the commonest index in the
    // shipped models) is F0 0.0392, the canonical dielectric 0.04; 164 is
    // 0.886 with a gold tint, 165 is 0.945 white, 3 is 0.871 copper.
    //
    // The SRM's blue channel lifts it further. It never exceeds 0.63 anywhere
    // in the shipped data and 61% of texels are exactly 0, so it is a
    // reflectivity BOOST on a sparse set of surfaces (eyes, vehicle panels,
    // metal fittings) rather than a metalness map — treating it as metalness
    // would tint the diffuse away on those and look wrong.
    //
    // A HIGH F0 is a metal, and a metal has no diffuse. That is not a switch
    // the data carries, so it is derived from F0 itself and eased rather than
    // stepped, so a mid-range preset does not pop between the two models.
    vec3 baseF0 = vec3(presetF0) * specTint;
    vec3 f0 = mix(baseF0, max(baseF0, vec3(0.16)),
                  clamp(reflMask * 1.6, 0.0, 1.0));
    // …and only for the families that can BE metal. Skin, hair, cloth and
    // eyes never are, whatever preset they select, and killing their diffuse
    // left a face as a dark specular shell with its translucency computed and
    // then multiplied to nothing. It also has to stay off the layer composite
    // above, which is albedo — a colourable part with a metal preset would
    // otherwise have its chosen colour multiplied away.
    float metalness = uNoMetal == 1 ? 0.0 : smoothstep(0.25, 0.6, presetF0);
    vec3 fres = f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0);
    // With ndv floored at 1e-4 and roughness at 0.045 the GGX peak reaches
    // ~1e8 at a grazing angle, and this codebase measures eye roughness at
    // 0.13 — so an eyeball seen almost edge-on blew out to white. The bound
    // is applied to the ASSEMBLED term below, after its fresnel and its gain:
    // clamping the bare BRDF and then multiplying by 2.5 leaves a ceiling of
    // 20x full white, which is not a bound on anything that matters.
    float spec = distGGX(ndh, a) * visSmith(ndv, ndl, a) * hOk;

    // Subsurface: wrap the diffuse term round the terminator by the
    // translucency amount. Skin materials get a floor so a face still reads
    // as skin on the models that ship no TRM at all.
    float trans = uHasTrm == 1 ? texture(uTrm, vUv).r : presetTrans;
    if (uSkin == 1) trans = max(trans, 0.25);

    // ── Per-channel debug views ─────────────────────────────────────────────
    // Placed HERE because this is the first point at which every input to the
    // shading is settled and none of the shading itself has happened yet:
    // albedo has been through the layer composite, the SRM has been unpacked,
    // and the preset-derived values are resolved. Each view returns the raw
    // input, unlit and un-exposed — a debug view that went through the rig
    // would be a picture of the rig.
    //
    // Lighting-only is the exception and does not return: it replaces the
    // albedo with flat grey and falls through to the real shading, which is
    // the point of it.
    if (uDebug != 0) {
        if (uDebug == 1) { fragColor = vec4(albedo, 1.0); return; }
        if (uDebug == 2) { fragColor = vec4(n * 0.5 + 0.5, 1.0); return; }
        if (uDebug == 3) { fragColor = vec4(vec3(rough), 1.0); return; }
        if (uDebug == 4) { fragColor = vec4(vec3(reflMask), 1.0); return; }
        if (uDebug == 5) { fragColor = vec4(vec3(ao), 1.0); return; }
        if (uDebug == 6) { fragColor = vec4(vec3(trans), 1.0); return; }
        if (uDebug == 7) { fragColor = vec4(vec3(metalness), 1.0); return; }
        if (uDebug == 8) {
            fragColor = vec4(fract(vUv.x), fract(vUv.y), 0.0, 1.0);
            return;
        }
        // LIGHTING ONLY means the lighting, not the lighting on this
        // material: a gold part left with its metalness and its specular tint
        // renders as a dark gold shell, which is a picture of the material
        // after all. Albedo, metalness and tint all go.
        if (uDebug == 9) {
            albedo = vec3(0.6);
            metalness = 0.0;
            specTint = vec3(1.0);
            // The canonical dielectric, and `fres` rebuilt from it — fres was
            // assembled from the material's own F0 forty lines up, so
            // replacing f0 alone would leave the highlight carrying the metal
            // it is meant to have taken away.
            f0 = vec3(0.04);
            fres = f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0);
        }
    }

    float wrap = clamp(trans, 0.0, 1.0) * 0.5;
    float diff = wrap > 0.0
        ? max((dot(n, l) + wrap) / ((1.0 + wrap) * (1.0 + wrap)), 0.0)
        : ndl;

    // A three-part rig, because one directional light and a constant ambient
    // is what the old path already was and it cannot show what an SRM is for:
    // roughness only reads on a specular highlight, and a reflection mask only
    // reads against something to reflect.
    //
    //  KEY   the same direction the flat path used, so a scene does not swing
    //        round when the setting is toggled.
    //  FILL  a dim opposite-side light, cool, so the shadow side keeps form.
    //  ENV   a hemisphere ambient (sky above, ground below) that also feeds an
    //        analytic environment specular. This is the term the reflection
    //        mask drives, and it is what makes a visor read as glass and a
    //        rubber strap read as rubber.
    vec3 kKey = uKeyCol;
    vec3 kFill = uFillCol;
    vec3 kSky = uSkyCol;
    vec3 kGround = uGroundCol;

    // The fill points along -uFillDir, the same convention as the key. It
    // used to be a literal (0.5, 0.2, 0.7) POINTING AT the surface, which is
    // the opposite convention — kept identical by having the C++ side hand in
    // the negated vector, so the default rig is bit-for-bit what it was.
    float fillNdl = max(dot(n, -uFillDir), 0.0);
    vec3 hemi = mix(kGround, kSky, n.y * 0.5 + 0.5) * ao;

    // Split-sum-style environment term: a smooth surface concentrates the
    // whole hemisphere into a highlight, a rough one spreads it back out into
    // the ambient it already has. (1-rough)^2 is the falloff.
    float gloss = (1.0 - rough) * (1.0 - rough);
    vec3 envF = f0 + (max(vec3(1.0 - rough), f0) - f0) * pow(1.0 - ndv, 5.0);
    vec3 envSpec = kSky * envF * gloss * ao * (0.35 + 1.4 * reflMask);
    // A metal's environment reflection carries its colour too — this is what
    // makes gold read as gold rather than as a bright white surface.
    envSpec *= mix(vec3(1.0), specTint, metalness);

    // 2.0 is a bright highlight on an LDR target and still reads as a
    // highlight rather than a hole punched in the image.
    vec3 specTerm = min(kKey * spec * fres * ndl * 2.5 * ao, vec3(2.0));

    // HAIR — Kajiya-Kay, replacing the round GGX lobe with one stretched along
    // the strand. This is what makes hair read as hair: a head of hair lit by
    // an isotropic highlight is a brown helmet, and every hair material in the
    // game was getting exactly that, because a hair material binds no normal
    // map and so never even entered the tangent-frame branch above.
    //
    // The strand direction is taken as the BITANGENT — the V axis — which is
    // the ordinary convention for hair-card UV layouts. Only two shift maps
    // decode in this install, which is not enough to prove the axis from the
    // data, so FOXAB_HAIR_TANGENT_U=1 flips it to U for anyone checking
    // against a complete install.
    //
    // The exponent is the material's own Anistropic_Diffusion. The shift map
    // does two jobs, and both follow from it being an unsigned strand mask:
    // it displaces the lobe along the normal (so the highlight sits where the
    // strands are rather than in a band across the whole head), and it gates
    // the lobe's strength, so the black 87% of the map produces no highlight.
    if (uHair == 1) {
        vec3 tp = vTangent.xyz - n * dot(n, vTangent.xyz);
        if (dot(tp, tp) > 1e-8) {
            vec3 tu = normalize(tp);
            vec3 tv = cross(n, tu) * (vTangent.w < 0.0 ? -1.0 : 1.0);
            vec3 strand = uHairTangentU == 1 ? tu : tv;
            float sh = uHasShift == 1 ? texture(uShift, vUv).r : 1.0;
            vec3 st = normalize(strand + n * (sh * uHairShift));
            float tdh = dot(st, h);
            float sinTH = sqrt(max(1.0 - tdh * tdh, 0.0));
            // ndl, not the raw lobe: a strand facing away from the light must
            // not catch a highlight, and hOk suppresses the degenerate half
            // vector exactly as the isotropic term above does.
            float aniso = pow(sinTH, max(uHairExp, 1.0)) * ndl * hOk;
            // Strength from the FMTT preset, not from a number chosen here.
            // The hair presets carry AnisotropicRoughness 0.898 and an F0 of
            // 0.1608 — four times the dielectric — and `fres` is already built
            // from that F0, so the lobe is as strong as the table says and no
            // stronger. uPresetAniso is what stops this from firing on a hair
            // material that points at a preset with no anisotropy at all.
            specTerm += kKey * fres * aniso * sh * presetAniso * 1.6 * ao;
        }
    }
    // Energy conservation: a metal reflects rather than scatters, so its
    // diffuse goes away as its F0 rises.
    vec3 out3 = albedo * (1.0 - metalness)
                    * (hemi + kKey * diff * 0.85 + kFill * fillNdl * 0.20)
              + specTerm + envSpec;

    // INCIDENCE — a Schlick-shaped rim, brightest where the surface turns
    // away from the eye. The reading is OURS: the data gives a white colour
    // with a strength in w and a companion "roughness" scalar living in
    // 0.5..6, and a white weight of a tenth to a half paired with an exponent
    // in that range is a Fresnel rim and very little else. Fox's own
    // formulation is not documented anywhere checkable, so this is stated as
    // an interpretation rather than as the engine's arithmetic.
    //
    // ADDED, not mixed, and scaled by the ambient occlusion as well as by the
    // sky colour: a rim that ignored AO lit the inside of a collar and the
    // crease of an eyelid, which is exactly where a real grazing-angle term
    // does not go. Multiplying by the hemisphere's own colour keeps it from
    // reading as a second, whiter light — and by the material's OWN tint on
    // top, which is what a third of these materials carry and what the first
    // version of this discarded.
    if (uIncidence > 0.0) {
        float rim = pow(1.0 - ndv, uIncidencePower) * uIncidence;
        out3 += kSky * uIncidenceTint * rim * ao;
    }
    fragColor = vec4(out3 * uExposure, 1.0);
}
)";

const char* kLineVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMvp;
void main() { gl_Position = uMvp * vec4(aPos, 1.0); }
)";

const char* kLineFrag = R"(#version 330 core
uniform vec4 uColor;
out vec4 fragColor;
void main() { fragColor = uColor; }
)";

// Mip chain that preserves ALPHA COVERAGE (Castano's method).
//
// Box-filtering an alpha-cutout texture pulls the average alpha down, so at
// normal viewing distance every texel of a hair/eyelash/decal sheet falls
// below the shader's cutoff and the geometry vanishes — which reads as "the
// model lost its textures" while the .glb export (no mips, no cutout erosion)
// looks fine. Rescaling each level's alpha so the fraction of texels above
// the cutoff matches level 0 keeps cutouts intact at every distance.
QVector<QImage> buildCoverageMips(const QImage& base, float cutoff)
{
    QVector<QImage> mips;
    mips.append(base);
    const float ref = cutoff * 255.0f;

    const auto coverage = [&](const QImage& im, float scale) {
        qint64 above = 0, total = 0;
        for (int y = 0; y < im.height(); ++y) {
            const uchar* row = im.constScanLine(y);
            for (int x = 0; x < im.width(); ++x, ++total)
                if (row[x * 4 + 3] * scale >= ref) ++above;
        }
        return total ? double(above) / double(total) : 0.0;
    };

    const double target = coverage(base, 1.0f);
    QImage prev = base;
    while (prev.width() > 1 || prev.height() > 1) {
        QImage next = prev
                          .scaled(qMax(1, prev.width() / 2), qMax(1, prev.height() / 2),
                                  Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                          .convertToFormat(QImage::Format_RGBA8888);
        if (target > 0.0 && target < 1.0) {
            // Bisect an alpha gain that restores level 0's coverage.
            float lo = 1.0f, hi = 16.0f;
            for (int it = 0; it < 12; ++it) {
                const float mid = 0.5f * (lo + hi);
                if (coverage(next, mid) < target) lo = mid;
                else hi = mid;
            }
            const float gain = 0.5f * (lo + hi);
            if (gain > 1.01f) {
                for (int y = 0; y < next.height(); ++y) {
                    uchar* row = next.scanLine(y);
                    for (int x = 0; x < next.width(); ++x) {
                        const float a = row[x * 4 + 3] * gain;
                        row[x * 4 + 3] = uchar(qBound(0.0f, a, 255.0f));
                    }
                }
            }
        }
        mips.append(next);
        prev = next;
    }
    return mips;
}

// Upload one texture, using coverage-preserving mips when the image actually
// carries cutout alpha and plain generated mips otherwise.
QOpenGLTexture* makeTexture(const QImage& src)
{
    const QImage img = src.format() == QImage::Format_RGBA8888
        ? src
        : src.convertToFormat(QImage::Format_RGBA8888);

    bool cutout = false;
    for (int y = 0; y < img.height() && !cutout; y += 2) {
        const uchar* row = img.constScanLine(y);
        for (int x = 0; x < img.width(); x += 2)
            if (row[x * 4 + 3] < uchar(kAlphaCutoffValue * 255.0f)) { cutout = true; break; }
    }
    if (!cutout)
        return new QOpenGLTexture(img, QOpenGLTexture::GenerateMipMaps);

    const QVector<QImage> mips = buildCoverageMips(img, kAlphaCutoffValue);
    auto* t = new QOpenGLTexture(QOpenGLTexture::Target2D);
    t->setFormat(QOpenGLTexture::RGBA8_UNorm);
    t->setSize(img.width(), img.height());
    t->setMipLevels(mips.size());
    t->allocateStorage(QOpenGLTexture::RGBA, QOpenGLTexture::UInt8);
    for (int level = 0; level < mips.size(); ++level)
        t->setData(level, QOpenGLTexture::RGBA, QOpenGLTexture::UInt8,
                   mips[level].constBits());
    return t;
}

}  // namespace

namespace {
void anglesFromDirection(const QVector3D& travelDir, float* azDeg, float* elDeg);
}  // namespace

GLModelWidget::GLModelWidget(QWidget* parent) : QOpenGLWidget(parent)
{
    setMinimumSize(320, 320);
    setFocusPolicy(Qt::ClickFocus);
    // The starting key angles ARE the direction this viewport used to
    // hard-code, converted rather than re-chosen. anglesFromDirection is
    // defined further down the file with the rest of the rig.
    anglesFromDirection(fox::legacyKeyDirection(), &m_keyAz, &m_keyEl);
    installViewportShortcuts();
}

GLModelWidget::~GLModelWidget()
{
    makeCurrent();
    destroyGpu();
    destroyOverlayGpu();
    doneCurrent();
}

void GLModelWidget::destroyOverlayGpu()
{
    if (m_gridVao) { m_gridVao->destroy(); delete m_gridVao; m_gridVao = nullptr; }
    m_gridVbo.destroy();
    m_gridVertexCount = 0;
    if (m_axisVao) { m_axisVao->destroy(); delete m_axisVao; m_axisVao = nullptr; }
    m_axisVbo.destroy();
    if (m_cnpVao) { m_cnpVao->destroy(); delete m_cnpVao; m_cnpVao = nullptr; }
    m_cnpVbo.destroy();
    m_cnpVertexCount = 0;
    m_cnpDirty = true;   // rebuilt on the next overlay pass
}

void GLModelWidget::setModel(QVector<GLMeshUpload> meshes, QVector<QImage> textures,
                             GLSkeletonUpload skeleton, QVector<QImage> normalMaps,
                             QVector<GLPbrMaterial> pbr)
{
    m_pendingMeshes = std::move(meshes);
    m_pendingTextures = std::move(textures);
    m_pendingNormalMaps = std::move(normalMaps);
    m_pendingPbr = std::move(pbr);
    m_pendingSkeleton = std::move(skeleton);
    m_dirty = true;
    m_groupVisible.clear();
    m_meshVisible.clear();
    m_groupTransform.clear();
    m_pose.clear();
    m_poseSkelLines.clear();
    m_poseDirty = false;

    // What the statistics overlay reports, counted ONCE here rather than per
    // frame. Materials are counted as the distinct slots the meshes actually
    // use, not as the length of the texture set: a set padded to the material
    // count would report materials nothing in this scene draws with.
    m_statMeshes = int(m_pendingMeshes.size());
    m_statTris = 0;
    {
        // NOT named `slots` — that is a Qt macro and a local of that name
        // fails with "declaration does not declare anything", which names
        // neither Qt nor the macro. verify-src.py checks for exactly this.
        QSet<int> usedSlots;
        for (const GLMeshUpload& m : m_pendingMeshes) {
            m_statTris += int(m.indices.size() / 3);
            if (m.materialSlot >= 0) usedSlots.insert(m.materialSlot);
        }
        m_statMaterials = usedSlots.size();
    }
    // The bone labels. Held as points rather than as the flat float array the
    // upload carries, because every use of them is per bone.
    m_boneNames = m_pendingSkeleton.boneNames;
    m_boneBind.clear();
    m_boneBind.reserve(m_boneNames.size());
    for (int i = 0; i + 2 < m_pendingSkeleton.bonePositions.size(); i += 3)
        m_boneBind.append(QVector3D(m_pendingSkeleton.bonePositions[i],
                                    m_pendingSkeleton.bonePositions[i + 1],
                                    m_pendingSkeleton.bonePositions[i + 2]));
    m_bonePosed.clear();
    m_statBones = m_boneBind.size();
    // The sockets belong to the model that has just been replaced.
    m_cnp.clear();
    m_cnpVertexCount = 0;
    m_cnpDirty = true;

    // Frame the model: bounding sphere from positions.
    QVector3D mn(1e9f, 1e9f, 1e9f), mx(-1e9f, -1e9f, -1e9f);
    bool any = false;
    for (const GLMeshUpload& m : m_pendingMeshes) {
        for (int i = 0; i + 2 < m.interleaved.size(); i += kVertexFloats) {
            const QVector3D p(m.interleaved[i], m.interleaved[i + 1],
                              m.interleaved[i + 2]);
            mn.setX(qMin(mn.x(), p.x())); mn.setY(qMin(mn.y(), p.y()));
            mn.setZ(qMin(mn.z(), p.z()));
            mx.setX(qMax(mx.x(), p.x())); mx.setY(qMax(mx.y(), p.y()));
            mx.setZ(qMax(mx.z(), p.z()));
            any = true;
        }
    }
    if (any) {
        m_sceneCenter = (mn + mx) * 0.5f;
        m_sceneRadius = qMax(0.05f, (mx - mn).length() * 0.5f);
    } else {
        m_sceneCenter = QVector3D(0, 1, 0);
        m_sceneRadius = 1.5f;
    }
    // Frame the FIRST scene and then leave the camera alone. Re-framing on
    // every setModel() meant that changing a jacket, a colour or a texture
    // threw away whatever angle and zoom the user had set, which is most of
    // what looking at a model consists of.
    //
    // The one exception is a scene of a wildly different size: keeping the
    // camera when a rifle follows a helicopter leaves the rifle a dot, or the
    // camera inside it. A factor of six is far outside anything a character's
    // own parts vary by, so swapping parts never trips it.
    if (any) {
        const float ratio = m_lastFramedRadius > 0.0f
            ? m_sceneRadius / m_lastFramedRadius
            : 0.0f;
        // Size is not the only way a scene can be "somewhere else". Two models
        // of similar size authored at different world origins would leave the
        // camera orbiting the last one's centre and the new one off screen, so
        // the distance the centre moved counts too — measured in radii, so the
        // test means the same thing for a pistol and for a helicopter.
        const float moved = (m_sceneCenter - m_center).length()
            / qMax(0.05f, m_sceneRadius);
        // "Fit each model as it loads" (Settings, and the Camera page) makes
        // this unconditional: every scene is framed. Off, the heuristic above
        // stands — reframe only when the new scene is a different SIZE or is
        // somewhere else entirely, so stepping through variants of one thing
        // does not jog the camera between them.
        if (m_autoFit || !m_haveScene || ratio > 6.0f || ratio < (1.0f / 6.0f)
            || moved > 3.0f) {
            resetCamera();
            m_lastFramedRadius = m_sceneRadius;
        }
        m_haveScene = true;
    }
    // An EMPTY scene deliberately does not clear m_haveScene. Rebuilding a
    // character clears and refills the widget, and treating the empty moment in
    // between as "no scene yet" would re-frame on every single change — exactly
    // the behaviour this is here to stop.
    update();
    emit sceneChanged();
}

void GLModelWidget::clearModel()
{
    setModel({}, {}, {}, {}, {});
}

void GLModelWidget::setGroupVisible(int groupId, bool visible)
{
    m_groupVisible[groupId] = visible;
    update();
}

QList<int> GLModelWidget::meshIds() const
{
    QList<int> out;
    out.reserve(m_meshes.size());
    for (const MeshGpu& m : m_meshes)
        if (m.meshId >= 0 && !out.contains(m.meshId)) out.append(m.meshId);
    std::sort(out.begin(), out.end());
    return out;
}

void GLModelWidget::setMeshVisible(int meshId, bool visible)
{
    if (meshId < 0) return;
    m_meshVisible[meshId] = visible;
    update();
}

QSet<int> GLModelWidget::hiddenMeshes() const
{
    // Only the ids explicitly switched OFF. Absence from the map means "never
    // touched", which the draw loop treats as visible, so it must not be
    // reported as hidden here either.
    QSet<int> out;
    for (auto it = m_meshVisible.constBegin(); it != m_meshVisible.constEnd(); ++it)
        if (!it.value()) out.insert(it.key());
    return out;
}

void GLModelWidget::clearMeshVisibility()
{
    m_meshVisible.clear();
    update();
}

void GLModelWidget::setGroupTransform(int groupId, const QMatrix4x4& m)
{
    m_groupTransform[groupId] = m;
    update();
}

void GLModelWidget::clearGroupTransforms()
{
    m_groupTransform.clear();
    update();
}

const char* shadingModeName(ShadingMode m)
{
    switch (m) {
        case ShadingMode::Wireframe: return "Wireframe";
        case ShadingMode::Flat:      return "Flat";
        case ShadingMode::Shaded:    return "Shaded";
        case ShadingMode::Rendered:  return "Rendered";
    }
    return "Shaded";
}

// ASCII ONLY, deliberately. These are `const char*` and every reader in the
// application turns them into a QString with fromLatin1(), which is the
// convention debugViewName/debugViewNote beside them already follow. An em
// dash in the source is UTF-8, and UTF-8 read as Latin-1 puts two mojibake
// characters in the middle of a tooltip — which is exactly what the first
// version of this text did, and it was visible in a screenshot before it was
// visible in any test.
const char* shadingModeNote(ShadingMode m)
{
    switch (m) {
        case ShadingMode::Wireframe:
            return "Polygon edges only: the topology, and nothing else.";
        case ShadingMode::Flat:
            return "Base colour, unlit. What the texture holds before any "
                   "light touches it.";
        case ShadingMode::Shaded:
            return "Lit with base colour and the normal map: the lighting "
                   "this tool has always done, and the honest comparison for "
                   "Rendered.";
        case ShadingMode::Rendered:
            return "The full material: the SRM (ambient occlusion, roughness, "
                   "reflection mask), translucency, the FMTT preset's F0 and "
                   "the environment rig. Fox has no metalness MAP at all; that "
                   "is the format, not a missing file.";
    }
    return "";
}

void GLModelWidget::setShadingMode(ShadingMode m)
{
    if (m_shading == m) return;
    // Remembered here as well as in setWireframe, so a mode chosen from the
    // shading balls is what a later wireframe round trip comes back to.
    if (m != ShadingMode::Wireframe) m_shadingBeforeWire = m;
    m_shading = m;
    // The two older booleans are VIEWS of this, not separate state. Kept in
    // step here rather than at every call site (template §3.1).
    m_wireframe = (m == ShadingMode::Wireframe);
    m_pbrShading = (m == ShadingMode::Rendered);
    update();
    emit shadingModeChanged(m);
    emit displayChanged();
}

void GLModelWidget::setShowGrid(bool on)
{
    if (m_showGrid == on) return;
    m_showGrid = on;
    update();
    emit displayChanged();
}

void GLModelWidget::setShowAxes(bool on)
{
    if (m_showAxes == on) return;
    m_showAxes = on;
    update();
    emit displayChanged();
}

void GLModelWidget::setShowStats(bool on)
{
    if (m_showStats == on) return;
    m_showStats = on;
    update();
    emit displayChanged();
}

void GLModelWidget::setShowBoneNames(bool on)
{
    if (m_showBoneNames == on) return;
    m_showBoneNames = on;
    update();
    emit displayChanged();
}

void GLModelWidget::setShowConnectPoints(bool on)
{
    if (m_showConnectPoints == on) return;
    m_showConnectPoints = on;
    update();
    emit displayChanged();
}

void GLModelWidget::setConnectPoints(const QVector<GLConnectPoint>& points)
{
    m_cnp = points;
    m_cnpDirty = true;
    update();
    emit sceneChanged();
}

QString GLModelWidget::statsText() const
{
    // What is in the SCENE, counted at upload — not what is on screen this
    // frame. A statistics overlay that changed when a submesh was unticked
    // would be answering a different question from the one it is asked.
    return QStringLiteral("%1 mesh%2 · %3 triangle%4 · %5 material%6 · "
                          "%7 bone%8\n%9")
        .arg(m_statMeshes)
        .arg(m_statMeshes == 1 ? QString() : QStringLiteral("es"))
        .arg(QLocale().toString(m_statTris))
        .arg(m_statTris == 1 ? QString() : QStringLiteral("s"))
        .arg(m_statMaterials)
        .arg(m_statMaterials == 1 ? QString() : QStringLiteral("s"))
        .arg(m_statBones)
        .arg(m_statBones == 1 ? QString() : QStringLiteral("s"))
        .arg(QString::fromLatin1(shadingModeName(m_shading)));
}

bool GLModelWidget::projectToScreen(const QVector3D& world, QPointF* out) const
{
    const QMatrix4x4 mvp = viewProj();
    const QVector4D clip = mvp * QVector4D(world, 1.0f);
    if (clip.w() <= 1e-6f) return false;          // behind the eye
    const QVector3D ndc = clip.toVector3D() / clip.w();
    if (ndc.x() < -1.2f || ndc.x() > 1.2f || ndc.y() < -1.2f || ndc.y() > 1.2f)
        return false;
    if (out)
        *out = QPointF((ndc.x() * 0.5f + 0.5f) * width(),
                       (1.0f - (ndc.y() * 0.5f + 0.5f)) * height());
    return true;
}

QVector<QPair<QString, QPointF>> GLModelWidget::boneLabelsOnScreen() const
{
    QVector<QPair<QString, QPointF>> out;
    if (!m_showBoneNames) return out;
    const QVector<QVector3D>& src = m_bonePosed.isEmpty() ? m_boneBind : m_bonePosed;
    const int n = qMin(src.size(), m_boneNames.size());
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        if (m_boneNames[i].isEmpty()) continue;
        QPointF p;
        if (projectToScreen(src[i], &p)) out.append({m_boneNames[i], p});
    }
    return out;
}

QVector<QPair<QString, QPointF>> GLModelWidget::connectLabelsOnScreen() const
{
    QVector<QPair<QString, QPointF>> out;
    if (!m_showConnectPoints) return out;
    out.reserve(m_cnp.size());
    for (const GLConnectPoint& c : m_cnp) {
        // A point hung off a bone follows that bone, exactly as it does in the
        // export: the connect point is authored in the bone's BIND frame and
        // that frame is a pure translation, so the posed position is the bone's
        // posed frame applied to the record unchanged.
        QVector3D at = c.pos;
        if (c.bone >= 0 && c.bone < m_bonePosed.size() && c.bone < m_boneBind.size())
            at = c.pos + (m_bonePosed[c.bone] - m_boneBind[c.bone]);
        QPointF p;
        if (projectToScreen(at, &p)) out.append({c.name, p});
    }
    return out;
}

void GLModelWidget::setWireframe(bool on)
{
    // Through the mode, so the balls, the toolbar glyph and the popover cannot
    // disagree. Turning wireframe OFF puts back the mode it was turned on
    // FROM — not a mode derived from whether the scene has maps. Deriving it
    // was wrong twice: it turned a deliberate Shaded (the PBR box unticked on
    // a scene whose maps are still in hand) back into Rendered, so the box and
    // the viewport then disagreed; and it lost a Flat selection entirely.
    if (on) {
        if (m_shading != ShadingMode::Wireframe) m_shadingBeforeWire = m_shading;
        setShadingMode(ShadingMode::Wireframe);
        return;
    }
    if (m_shading == ShadingMode::Wireframe) {
        setShadingMode(m_shadingBeforeWire == ShadingMode::Wireframe
                           ? ShadingMode::Shaded
                           : m_shadingBeforeWire);
        return;
    }
    if (m_wireframe == on) return;
    m_wireframe = on;
    update();
    emit displayChanged();
}

void GLModelWidget::setShowSkeleton(bool on)
{
    if (m_showSkeleton == on) return;
    m_showSkeleton = on;
    update();
    emit displayChanged();
}

void GLModelWidget::setNormalMapping(bool on)
{
    m_normalMapping = on;
    update();
}

void GLModelWidget::setPbrShading(bool on)
{
    // The same argument as setWireframe: PBR-on-or-off IS the Rendered/Shaded
    // pair of the shading modes, and keeping it as a separate boolean was how
    // a viewport could be "Rendered" on the balls and flat on the screen.
    // Wireframe and Flat are left alone — they are not lit at all, so this
    // switch has nothing to say about them and silently dropping out of
    // wireframe because a settings replay touched PBR would be worse than
    // ignoring it.
    if (m_shading == ShadingMode::Wireframe || m_shading == ShadingMode::Flat) {
        m_pbrShading = on;
        update();
        return;
    }
    setShadingMode(on ? ShadingMode::Rendered : ShadingMode::Shaded);
}

bool GLModelWidget::hasNormalMaps() const
{
    for (const QImage& img : m_pendingNormalMaps)
        if (!img.isNull()) return true;
    return false;
}

void GLModelWidget::applyPose(const QVector<animmath::Mat4>& skin,
                              const QVector<float>& skeletonLines)
{
    m_pose = skin;
    m_poseSkelLines = skeletonLines;
    m_poseDirty = true;
    update();
}

void GLModelWidget::clearPose()
{
    m_pose.clear();
    m_poseSkelLines.clear();
    m_poseDirty = true;
    update();
}

void GLModelWidget::centerOn(const QVector3D& center, float radius)
{
    m_sceneCenter = center;
    m_sceneRadius = qMax(0.05f, radius);
    resetCamera();
    // This IS a framing, so record it — otherwise the next setModel() measures
    // its "has the scale changed" test against a radius the camera is no
    // longer framed to.
    m_lastFramedRadius = m_sceneRadius;
    m_haveScene = true;
}

void GLModelWidget::resetCamera()
{
    m_center = m_sceneCenter;
    m_dist = m_sceneRadius * 2.4f;
    m_yaw = 45.0f;
    m_pitch = 18.0f;
    update();
    emit cameraChanged();
}

// ── The lighting rig ────────────────────────────────────────────────────────
//
// The key light is stored as azimuth/elevation rather than as a vector so the
// panel's two sliders are the state, not a derived view of it: driving
// elevation to the pole and back through a vector would lose the azimuth on
// the way. The starting pair is DERIVED from the direction this viewport used
// to hard-code, so "default" is the same light it always was rather than a
// pair of round numbers that happen to look similar.
namespace {
void anglesFromDirection(const QVector3D& travelDir, float* azDeg, float* elDeg)
{
    // travelDir is the direction the light TRAVELS; the angles describe where
    // it comes FROM, which is the way a person thinks about a light.
    const QVector3D toLight = -travelDir.normalized();
    *elDeg = qRadiansToDegrees(std::asin(qBound(-1.0f, toLight.y(), 1.0f)));
    *azDeg = qRadiansToDegrees(std::atan2(toLight.x(), toLight.z()));
}
}  // namespace

QVector3D GLModelWidget::keyDirection() const
{
    if (m_keyFollowsCamera) {
        // Over the viewer's shoulder, lifted a little and pushed to one side,
        // so the highlight is not dead centre and the form does not go
        // completely flat. Measured only in the sense that these two offsets
        // are the smallest that keep a sphere from reading as a disc.
        const QVector3D eye = eyePosition();
        QVector3D toEye = eye - m_center;
        if (toEye.lengthSquared() < 1e-12f) toEye = QVector3D(0, 0, 1);
        toEye.normalize();
        const QVector3D right =
            QVector3D::crossProduct(QVector3D(0, 1, 0), toEye).normalized();
        const QVector3D from =
            (toEye + right * 0.45f + QVector3D(0, 0.35f, 0)).normalized();
        return -from;
    }
    const float az = qDegreesToRadians(m_keyAz);
    const float el = qDegreesToRadians(m_keyEl);
    const float ce = std::cos(el);
    const QVector3D from(ce * std::sin(az), std::sin(el), ce * std::cos(az));
    return -from;
}

void GLModelWidget::setEnvironment(const fox::ViewEnvironment& env)
{
    // An explicit pick ends Auto. The alternative — leaving Auto on and having
    // the next loaded model quietly undo the choice — is the kind of control
    // that looks broken rather than clever.
    m_envAuto = false;
    m_env = env;
    m_exposure = env.exposure;
    update();
}

void GLModelWidget::setEnvironmentAuto(bool on)
{
    if (m_envAuto == on) return;
    m_envAuto = on;
    if (on) applyAutoEnvironment();
    emit displayChanged();
}

void GLModelWidget::setSceneGame(fox::GameId g)
{
    if (m_sceneGame == g) return;
    m_sceneGame = g;
    if (m_envAuto) applyAutoEnvironment();
}

void GLModelWidget::applyAutoEnvironment()
{
    const fox::ViewEnvironment* e = fox::ViewEnvironment::forGame(m_sceneGame);
    // No rig for this game — a mixed scene, or a file the index could not
    // place. Default is the honest answer; silently keeping the previous
    // game's rig would be a lie about what you are looking at.
    if (!e) e = &fox::ViewEnvironment::presets().first();
    // NOTHING TO DO when the rig is already the right one. This runs on every
    // scene load, and taking the environment's exposure each time threw away
    // whatever the user had dialled in — silently, and only in Auto mode,
    // which is what would make it read as a bug rather than as a policy. A
    // rig CHANGE still carries its own exposure, exactly as an explicit pick
    // does.
    if (m_env.id == e->id) return;
    m_env = *e;
    m_exposure = e->exposure;
    update();
    emit displayChanged();
}

void GLModelWidget::setKeyAngles(float azimuthDeg, float elevationDeg)
{
    // Azimuth wraps, elevation clamps: a light directly overhead is a real
    // rig, a light past the pole is the same rig described twice.
    m_keyAz = std::fmod(azimuthDeg, 360.0f);
    if (m_keyAz < -180.0f) m_keyAz += 360.0f;
    if (m_keyAz > 180.0f) m_keyAz -= 360.0f;
    m_keyEl = qBound(-89.0f, elevationDeg, 89.0f);
    update();
}

void GLModelWidget::setKeyFollowsCamera(bool on)
{
    m_keyFollowsCamera = on;
    update();
}

void GLModelWidget::setKeyIntensity(float k)
{
    m_keyGain = qBound(0.0f, k, 4.0f);
    update();
}

void GLModelWidget::setAmbientIntensity(float k)
{
    m_ambientGain = qBound(0.0f, k, 4.0f);
    update();
}

void GLModelWidget::setExposure(float e)
{
    m_exposure = qBound(0.05f, e, 4.0f);
    update();
}

void GLModelWidget::setBackgroundColor(const QColor& c)
{
    m_bgOverride = c;
    update();
}

QColor GLModelWidget::backgroundColor() const
{
    return m_bgOverride.isValid() ? m_bgOverride : m_env.background;
}

void GLModelWidget::setDebugView(fox::DebugView v)
{
    m_debug = v;
    update();
}

void GLModelWidget::setTurntable(bool on, float degPerSec)
{
    // Clamped to the range the panel's slider offers. A caller (the devshot
    // harness) that asks for 200°/s would otherwise spin at a speed the slider
    // cannot represent, and the next thing that syncs the panel would silently
    // snap it back to 120.
    m_turnSpeed = qBound(-120.0f, degPerSec, 120.0f);
    if (on == m_turntable) {
        if (m_turntable) m_turnClock.restart();
        return;
    }
    m_turntable = on;
    if (!on) {
        if (m_turnTimer) m_turnTimer->stop();
        return;
    }
    if (!m_turnTimer) {
        m_turnTimer = new QTimer(this);
        // ~60 Hz. The STEP is taken from a clock rather than from the
        // interval, so a viewport that cannot keep up turns at the same rate
        // as one that can — it just does it in fewer, larger steps.
        m_turnTimer->setInterval(16);
        connect(m_turnTimer, &QTimer::timeout, this, [this] {
            const qint64 ms = m_turnClock.restart();
            if (ms <= 0) return;
            m_yaw = std::fmod(m_yaw + m_turnSpeed * float(ms) * 0.001f, 360.0f);
            update();
            emit cameraChanged();
        });
    }
    m_turnClock.restart();
    m_turnTimer->start();
}

QImage GLModelWidget::grabViewport()
{
    // grabFramebuffer() makes the context current, renders and reads back. It
    // must not be called from inside paintGL — nothing here does.
    return grabFramebuffer();
}

void GLModelWidget::setTransparentBackground(bool on)
{
    const float want = on ? 0.0f : 1.0f;
    if (qFuzzyCompare(m_clearAlpha, want)) return;
    m_clearAlpha = want;
    update();
}

QImage GLModelWidget::renderAtSize(int w, int h)
{
    // A CAP, because this is reachable from a settings value: 8192 square is
    // 256 MB of RGBA before Qt makes its own copy, and a driver that refuses
    // the allocation returns an invalid FBO rather than telling anyone why.
    w = qBound(16, w, 8192);
    h = qBound(16, h, 8192);
    if (!context() || !isValid()) return {};
    makeCurrent();
    QOpenGLFramebufferObjectFormat fmt;
    fmt.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
    fmt.setSamples(0);   // no MSAA: a multisample FBO cannot be read directly
    QOpenGLFramebufferObject fbo(w, h, fmt);
    if (!fbo.isValid()) {
        doneCurrent();
        return {};
    }
    // The viewport and the projection follow the FBO, not the widget — the
    // aspect ratio has to come from the surface being drawn into or a
    // non-square still comes out stretched.
    // A SCOPE GUARD, not a straight line. This project builds with /EHa and
    // installs an SEH translator, so a driver fault inside paintGL's uploads
    // becomes a C++ exception — and unwinding past the two assignments below
    // would leave the off-screen aspect ratio in force for every subsequent
    // ON-SCREEN frame, with no way to clear it short of another successful
    // capture. Three side effects, one guard.
    struct Restore {
        GLModelWidget* w;
        ~Restore()
        {
            w->m_renderW = 0;
            w->m_renderH = 0;
            w->doneCurrent();
        }
    } restore{this};
    m_renderW = w;
    m_renderH = h;
    fbo.bind();
    glViewport(0, 0, w, h);
    paintGL();
    fbo.release();
    return fbo.toImage();
}

QVector<QImage> GLModelWidget::renderTurntable(int frames)
{
    QVector<QImage> out;
    const int n = qBound(2, frames, 360);
    out.reserve(n);
    // The turntable timer would fight the loop for the yaw. Stopped for the
    // duration and put back exactly as it was — the speed is passed BACK IN on
    // both calls, because setTurntable's own default argument is 22 and
    // simplifying either of these to setTurntable(false) would silently reset
    // a speed the user had chosen.
    const bool wasSpinning = m_turntable;
    if (wasSpinning) setTurntable(false, m_turnSpeed);
    const float wasYaw = m_yaw;
    for (int i = 0; i < n; ++i) {
        m_yaw = std::fmod(wasYaw + 360.0f * float(i) / float(n), 360.0f);
        out.append(grabFramebuffer());
    }
    m_yaw = wasYaw;
    if (wasSpinning) setTurntable(true, m_turnSpeed);
    update();
    // The camera is back where it was, so nothing downstream needs telling —
    // but a panel showing a live yaw watched it move, so say it settled.
    emit cameraChanged();
    return out;
}

void GLModelWidget::initializeGL()
{
    initializeOpenGLFunctions();
    glEnable(GL_DEPTH_TEST);

    m_prog = std::make_unique<QOpenGLShaderProgram>();
    m_prog->addShaderFromSourceCode(QOpenGLShader::Vertex, kVert);
    m_prog->addShaderFromSourceCode(QOpenGLShader::Fragment, kFrag);
    if (!m_prog->link())
        qWarning("gl: mesh shader link failed: %s", qUtf8Printable(m_prog->log()));
    // Resolve the per-region uniforms once. A driver that optimises an unused
    // element away returns -1, which the draw loop skips rather than passing
    // to glUniform.
    for (int r = 0; r < 4; ++r) {
        m_locPresetA[r] = m_prog->uniformLocation(
            QStringLiteral("uPresetA[%1]").arg(r));
        m_locPresetSpec[r] = m_prog->uniformLocation(
            QStringLiteral("uPresetSpec[%1]").arg(r));
    }

    m_lineProg = std::make_unique<QOpenGLShaderProgram>();
    m_lineProg->addShaderFromSourceCode(QOpenGLShader::Vertex, kLineVert);
    m_lineProg->addShaderFromSourceCode(QOpenGLShader::Fragment, kLineFrag);
    if (!m_lineProg->link())
        qWarning("gl: line shader link failed: %s", qUtf8Printable(m_lineProg->log()));

    // ── Overlay geometry (template §5) ──────────────────────────────────
    // Torn down first: initializeGL runs again when the context is re-created,
    // and QOpenGLBuffer::create() is a no-op on a buffer that already exists,
    // so a second run would otherwise attach a new VAO to a dead buffer id.
    destroyOverlayGpu();
    // Both are UNIT sized and scaled into place per draw by folding the scale
    // into the MVP. A grid rebuilt on every scene change would be 164 floats
    // of upload for a shape that never actually changes.
    {
        constexpr int kHalf = 10;          // 21 lines each way
        QVector<float> g;
        g.reserve((2 * kHalf + 1) * 4 * 3);
        for (int i = -kHalf; i <= kHalf; ++i) {
            const float t = float(i) / float(kHalf);
            g << t << 0.0f << -1.0f << t << 0.0f << 1.0f;
            g << -1.0f << 0.0f << t << 1.0f << 0.0f << t;
        }
        m_gridVao = new QOpenGLVertexArrayObject;
        m_gridVao->create();
        m_gridVao->bind();
        m_gridVbo.create();
        m_gridVbo.bind();
        m_gridVbo.allocate(g.constData(), int(g.size() * sizeof(float)));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
        m_gridVao->release();
        m_gridVertexCount = int(g.size() / 3);
    }
    {
        // Three segments from the origin, drawn one colour at a time — X red,
        // Y green, Z blue, the convention every 3D tool shares.
        const QVector<float> a{0, 0, 0, 1, 0, 0,
                               0, 0, 0, 0, 1, 0,
                               0, 0, 0, 0, 0, 1};
        m_axisVao = new QOpenGLVertexArrayObject;
        m_axisVao->create();
        m_axisVao->bind();
        m_axisVbo.create();
        m_axisVbo.bind();
        m_axisVbo.allocate(a.constData(), int(a.size() * sizeof(float)));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
        m_axisVao->release();
    }
}

void GLModelWidget::destroyGpu()
{
    for (MeshGpu& m : m_meshes) {
        if (m.vao) { m.vao->destroy(); delete m.vao; }
        m.vbo.destroy();
        m.ibo.destroy();
    }
    m_meshes.clear();
    m_hasPbr = false;
    for (QOpenGLTexture* t : m_textures) delete t;
    m_textures.clear();
    for (QOpenGLTexture* t : m_normalTextures) delete t;
    m_normalTextures.clear();
    for (QOpenGLTexture* t : m_materialTextures) delete t;
    m_materialTextures.clear();
    for (QOpenGLTexture* t : m_translucentTextures) delete t;
    m_translucentTextures.clear();
    for (QOpenGLTexture* t : m_layerTextures) delete t;
    m_layerTextures.clear();
    for (QOpenGLTexture* t : m_layerMaskTextures) delete t;
    m_layerMaskTextures.clear();
    for (QOpenGLTexture* t : m_matParamTextures) delete t;
    m_matParamTextures.clear();
    for (QOpenGLTexture* t : m_subNormalTextures) delete t;
    m_subNormalTextures.clear();
    for (QOpenGLTexture* t : m_shiftTextures) delete t;
    m_shiftTextures.clear();
    m_pbr.clear();
    if (m_skelVao) {
        m_skelVao->destroy();
        delete m_skelVao;
        m_skelVao = nullptr;
    }
    m_skelVbo.destroy();
    m_skelVertexCount = 0;
}

void GLModelWidget::uploadPending()
{
    destroyGpu();

    for (int si = 0; si < m_pendingMeshes.size(); ++si) {
        const GLMeshUpload& src = m_pendingMeshes[si];
        if (src.interleaved.isEmpty() || src.indices.isEmpty()) continue;
        MeshGpu m;
        m.srcIndex = si;
        m.vao = new QOpenGLVertexArrayObject;
        m.vao->create();
        m.vao->bind();
        m.vbo.create();
        m.vbo.bind();
        if (!src.joints.isEmpty())
            m.vbo.setUsagePattern(QOpenGLBuffer::DynamicDraw);
        m.vbo.allocate(src.interleaved.constData(),
                       static_cast<int>(src.interleaved.size() * sizeof(float)));
        m.ibo.create();
        m.ibo.bind();
        m.ibo.allocate(src.indices.constData(),
                       static_cast<int>(src.indices.size() * sizeof(quint32)));
        const int stride = kVertexFloats * sizeof(float);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(3 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(6 * sizeof(float)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(8 * sizeof(float)));
        m.vao->release();
        m.indexCount = static_cast<int>(src.indices.size());
        m.materialSlot = src.materialSlot;
        m.groupId = src.groupId;
        m.meshId = src.meshId;
        m_meshes.append(m);
    }

    // ANISOTROPIC FILTERING. Trilinear mips fix the shimmer on a tiled map but
    // they fix it by blurring along BOTH axes, and a surface seen at a grazing
    // angle is minified along one axis and not the other — so a garment whose
    // cloth weave tiles 100x reads as grey mush from any angle that is not
    // face-on. That is precisely the case this renderer now has, since the
    // plain UV-repeat pair turned out to be the sub-normal's on every non-layer
    // material that carries it.
    //
    // Queried, not assumed: the extension is core only in GL 4.6 and this
    // renderer targets 3.3, and a driver that does not have it must not be
    // handed the parameter at all. Reported once so an install that silently
    // has no anisotropy is visible rather than merely disappointing.
    if (m_maxAniso < 0.0f) {
        m_maxAniso = 1.0f;
        QOpenGLContext* ctx = QOpenGLContext::currentContext();
        // The EXT string, and only that one. Qt's setMaximumAnisotropy gates
        // on GL_EXT_texture_filter_anisotropic alone, so accepting the ARB
        // spelling here would log "anisotropic filtering on", apply nothing,
        // and have Qt warn once per texture per upload — hundreds of lines on
        // a composed character.
        const bool have = ctx
            && ctx->hasExtension(QByteArrayLiteral("GL_EXT_texture_filter_anisotropic"));
        if (have && !qEnvironmentVariableIsSet("FOXAB_NO_ANISO")) {
            GLfloat cap = 1.0f;
            glGetFloatv(0x84FF /* GL_MAX_TEXTURE_MAX_ANISOTROPY */, &cap);
            // 8 is the usual quality/bandwidth knee and is well inside every
            // desktop driver's cap; going to the reported maximum buys very
            // little and costs fill rate on exactly the surfaces that are
            // already the most expensive ones in the scene.
            m_maxAniso = qBound(1.0f, float(cap), 8.0f);
        }
        qInfo("gl: anisotropic filtering %s (max %.0fx)",
              m_maxAniso > 1.0f ? "on" : "OFF", double(m_maxAniso));
    }
    const auto tune = [this](QOpenGLTexture* t) {
        if (!t) return;
        t->setMinificationFilter(QOpenGLTexture::LinearMipMapLinear);
        t->setMagnificationFilter(QOpenGLTexture::Linear);
        t->setWrapMode(QOpenGLTexture::Repeat);
        if (m_maxAniso > 1.0f) t->setMaximumAnisotropy(m_maxAniso);
    };

    m_textures.resize(m_pendingTextures.size());
    for (int i = 0; i < m_pendingTextures.size(); ++i) {
        m_textures[i] = nullptr;
        const QImage& img = m_pendingTextures[i];
        if (img.isNull()) continue;
        // NO vertical flip: FMDL UVs are top-origin (DirectX/glTF style) and
        // Qt uploads QImage row 0 to v=0, so an unmirrored upload samples
        // exactly like the .glb export (verified with a 4-row test texture and
        // against Blender renders of our own exports). Mirroring here flipped
        // every texture vertically — the viewport disagreed with the export.
        auto* t = makeTexture(img);
        tune(t);
        m_textures[i] = t;
    }

    // Normal maps: plain GPU mips (no coverage-preserving pass — these carry no
    // cutout alpha, they are RGB by the time they get here) and the same
    // top-origin upload as the base map.
    m_normalTextures.resize(m_pendingNormalMaps.size());
    for (int i = 0; i < m_pendingNormalMaps.size(); ++i) {
        m_normalTextures[i] = nullptr;
        const QImage& img = m_pendingNormalMaps[i];
        if (img.isNull()) continue;
        auto* t = new QOpenGLTexture(img.convertToFormat(QImage::Format_RGB888),
                                     QOpenGLTexture::GenerateMipMaps);
        tune(t);
        m_normalTextures[i] = t;
    }

    // The PBR set. Padded to the texture count so every lookup below can index
    // by material slot without a second bounds rule: a caller that supplies
    // fewer PBR entries than materials (or none at all) gets default-
    // constructed ones, which read as "no maps, no layer, not skin".
    // NOTE for anyone adding context-loss recovery: uploadPending() runs only
    // when m_dirty is set, and only setModel() sets it, so it cannot run twice
    // for one scene — which is what makes it safe to drop the CPU-side PBR
    // images below. initializeGL() does NOT set m_dirty today (a re-created
    // context already re-uploads nothing); if that is ever changed, the base
    // and normal maps would come back and the PBR maps would not, because they
    // are the only set that is freed. Stop freeing them first.
    m_hasPbr = !m_pendingPbr.isEmpty();
    m_pbr = m_pendingPbr;
    m_pbr.resize(qMax(m_pendingTextures.size(), m_pendingPbr.size()));
    const auto uploadAux = [&tune](const QImage& img) -> QOpenGLTexture* {
        if (img.isNull()) return nullptr;
        auto* t = new QOpenGLTexture(img.convertToFormat(QImage::Format_RGB888),
                                     QOpenGLTexture::GenerateMipMaps);
        tune(t);
        return t;
    };
    m_materialTextures.resize(m_pbr.size());
    m_translucentTextures.resize(m_pbr.size());
    m_layerTextures.resize(m_pbr.size());
    m_layerMaskTextures.resize(m_pbr.size());
    m_matParamTextures.resize(m_pbr.size());
    m_subNormalTextures.resize(m_pbr.size());
    m_shiftTextures.resize(m_pbr.size());
    for (int i = 0; i < m_pbr.size(); ++i) {
        m_materialTextures[i] = uploadAux(m_pbr[i].material);
        m_translucentTextures[i] = uploadAux(m_pbr[i].translucent);
        m_layerTextures[i] = uploadAux(m_pbr[i].layer);
        m_layerMaskTextures[i] = uploadAux(m_pbr[i].layerMask);
        m_subNormalTextures[i] = uploadAux(m_pbr[i].subNormal);
        m_shiftTextures[i] = uploadAux(m_pbr[i].shift);
        // NEAREST and no mips, unlike every other aux map. Linear filtering
        // of a region index invents regions between the real ones, and mip
        // minification averages whole neighbourhoods — so a model drifting
        // into the distance would drift toward the middle regions instead of
        // resolving to the ones it is made of.
        m_matParamTextures[i] = nullptr;
        if (!m_pbr[i].matParamMap.isNull()) {
            // DontGenerateMipMaps explicitly: QOpenGLTexture's QImage
            // constructor defaults to GENERATING them, so the comment above
            // was describing an intent the code did not carry out — every MTM
            // was paying a full mip chain and a glGenerateMipmap for levels
            // the Nearest filter can never sample.
            auto* t = new QOpenGLTexture(
                m_pbr[i].matParamMap.convertToFormat(QImage::Format_RGB888),
                QOpenGLTexture::DontGenerateMipMaps);
            t->setMinificationFilter(QOpenGLTexture::Nearest);
            t->setMagnificationFilter(QOpenGLTexture::Nearest);
            t->setWrapMode(QOpenGLTexture::Repeat);
            m_matParamTextures[i] = t;
        }
        // Drop the CPU copies. Nothing below reads them again — the draw loop
        // uses only the flags — and four 512x512 RGB888 maps is ~3 MB per
        // material, so a composed character with forty material slots was
        // holding over a hundred megabytes of pixels for nothing. Both
        // vectors have to be cleared: QImage is implicitly shared, so freeing
        // one copy while the other still refers to the same buffer frees
        // nothing at all.
        m_pbr[i].material = QImage();
        m_pbr[i].translucent = QImage();
        m_pbr[i].layer = QImage();
        m_pbr[i].layerMask = QImage();
        m_pbr[i].matParamMap = QImage();
        m_pbr[i].subNormal = QImage();
        m_pbr[i].shift = QImage();
        if (i < m_pendingPbr.size()) {
            m_pendingPbr[i].material = QImage();
            m_pendingPbr[i].translucent = QImage();
            m_pendingPbr[i].layer = QImage();
            m_pendingPbr[i].layerMask = QImage();
            m_pendingPbr[i].matParamMap = QImage();
            m_pendingPbr[i].subNormal = QImage();
            m_pendingPbr[i].shift = QImage();
        }
    }

    if (!m_pendingSkeleton.lines.isEmpty()) {
        m_skelVao = new QOpenGLVertexArrayObject;
        m_skelVao->create();
        m_skelVao->bind();
        m_skelVbo.create();
        m_skelVbo.bind();
        m_skelVbo.allocate(m_pendingSkeleton.lines.constData(),
                           static_cast<int>(m_pendingSkeleton.lines.size()
                                            * sizeof(float)));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
        m_skelVao->release();
        m_skelVertexCount = static_cast<int>(m_pendingSkeleton.lines.size() / 3);
    }
    m_dirty = false;
    if (!m_pose.isEmpty()) m_poseDirty = true;   // repose freshly uploaded VBOs
}

// CPU-skin every weighted mesh into its VBO (context must be current).
void GLModelWidget::applyPoseBuffers()
{
    using animmath::Mat4;
    using animmath::Vec3;

    for (MeshGpu& m : m_meshes) {
        if (m.srcIndex < 0 || m.srcIndex >= m_pendingMeshes.size()) continue;
        const GLMeshUpload& src = m_pendingMeshes[m.srcIndex];
        if (src.joints.isEmpty() || src.weights.isEmpty()) continue;
        const int vertCount = static_cast<int>(src.interleaved.size() / kVertexFloats);
        if (src.joints.size() < vertCount * 4) continue;

        if (m_pose.isEmpty()) {
            // Bind pose: restore the original buffer.
            m.vbo.bind();
            m.vbo.write(0, src.interleaved.constData(),
                        static_cast<int>(src.interleaved.size() * sizeof(float)));
            m.vbo.release();
            continue;
        }

        m_skinScratch = src.interleaved;   // uv untouched
        const int boneCount = m_pose.size();
        for (int v = 0; v < vertCount; ++v) {
            const float* in = src.interleaved.constData() + v * kVertexFloats;
            float* out = m_skinScratch.data() + v * kVertexFloats;
            // Blend the 4 influences into one matrix (rows 0-2 cols 0-2 + row 3).
            float B[4][3] = {};
            bool anyW = false;
            for (int k = 0; k < 4; ++k) {
                const float w = src.weights[v * 4 + k];
                if (w <= 0.0f) continue;
                const int j = src.joints[v * 4 + k];
                if (j < 0 || j >= boneCount) continue;
                const Mat4& S = m_pose[j];
                for (int rr = 0; rr < 4; ++rr)
                    for (int cc = 0; cc < 3; ++cc) B[rr][cc] += S.m[rr][cc] * w;
                anyW = true;
            }
            if (!anyW) continue;
            const float px = in[0], py = in[1], pz = in[2];
            out[0] = px * B[0][0] + py * B[1][0] + pz * B[2][0] + B[3][0];
            out[1] = px * B[0][1] + py * B[1][1] + pz * B[2][1] + B[3][1];
            out[2] = px * B[0][2] + py * B[1][2] + pz * B[2][2] + B[3][2];
            const float nx = in[3], ny = in[4], nz = in[5];
            float ox = nx * B[0][0] + ny * B[1][0] + nz * B[2][0];
            float oy = nx * B[0][1] + ny * B[1][1] + nz * B[2][1];
            float oz = nx * B[0][2] + ny * B[1][2] + nz * B[2][2];
            const float len = std::sqrt(ox * ox + oy * oy + oz * oz);
            if (len > 1e-8f) { ox /= len; oy /= len; oz /= len; }
            out[3] = ox; out[4] = oy; out[5] = oz;
            // The tangent has to ride the same skin matrix as the normal, or
            // the normal-mapped lighting stays frozen in bind pose while the
            // mesh moves. w (bitangent sign) is a handedness flag — copied.
            const float tx = in[8], ty = in[9], tz = in[10];
            float qx = tx * B[0][0] + ty * B[1][0] + tz * B[2][0];
            float qy = tx * B[0][1] + ty * B[1][1] + tz * B[2][1];
            float qz = tx * B[0][2] + ty * B[1][2] + tz * B[2][2];
            const float tlen = std::sqrt(qx * qx + qy * qy + qz * qz);
            if (tlen > 1e-8f) { qx /= tlen; qy /= tlen; qz /= tlen; }
            out[8] = qx; out[9] = qy; out[10] = qz;
        }
        m.vbo.bind();
        m.vbo.write(0, m_skinScratch.constData(),
                    static_cast<int>(m_skinScratch.size() * sizeof(float)));
        m.vbo.release();
    }

    // Skeleton overlay: reposed lines when provided, bind lines otherwise.
    if (m_skelVao) {
        const QVector<float>& lines = !m_pose.isEmpty() && !m_poseSkelLines.isEmpty()
            ? m_poseSkelLines
            : m_pendingSkeleton.lines;
        if (!lines.isEmpty()) {
            m_skelVbo.bind();
            const int bytes = static_cast<int>(lines.size() * sizeof(float));
            if (lines.size() == m_pendingSkeleton.lines.size())
                m_skelVbo.write(0, lines.constData(), bytes);
            else
                m_skelVbo.allocate(lines.constData(), bytes);
            m_skelVbo.release();
            m_skelVertexCount = static_cast<int>(lines.size() / 3);
        }
    }
    // Where every bone ENDED UP, for the labels and for the sockets that hang
    // off them. Derived rather than passed in: the palette holds
    // translate(-bindWorld) · animWorld, so translate(+bindWorld) · palette[b]
    // recovers the bone's world frame exactly, with no second pass through the
    // solver and nothing for a caller to keep in step (MGO_FACTS, 8c/8e).
    m_bonePosed.clear();
    if (!m_pose.isEmpty() && !m_boneBind.isEmpty()) {
        const int n = qMin(m_boneBind.size(), m_pose.size());
        m_bonePosed.reserve(n);
        for (int b = 0; b < n; ++b) {
            const animmath::Vec3 w = animmath::transform(
                animmath::Vec3(m_boneBind[b].x(), m_boneBind[b].y(),
                               m_boneBind[b].z()),
                m_pose[b]);
            m_bonePosed.append(QVector3D(w.x, w.y, w.z));
        }
    }
    // The crosses are drawn at world positions, so they have to move with it.
    if (!m_cnp.isEmpty()) m_cnpDirty = true;
    m_poseDirty = false;
}

QVector3D GLModelWidget::eyePosition() const
{
    const float yawR = qDegreesToRadians(m_yaw);
    const float pitchR = qDegreesToRadians(m_pitch);
    return m_center
        + QVector3D(std::cos(pitchR) * std::sin(yawR), std::sin(pitchR),
                    std::cos(pitchR) * std::cos(yawR))
              * m_dist;
}

QMatrix4x4 GLModelWidget::viewProj() const
{
    const QVector3D eye = eyePosition();
    QMatrix4x4 view;
    view.lookAt(eye, m_center, QVector3D(0, 1, 0));
    QMatrix4x4 proj;
    // The aspect comes from the SURFACE being drawn into, which is usually
    // the widget and is not while renderAtSize() has an off-screen buffer
    // bound. Taking it from width()/height() there stretched every still
    // rendered at a shape other than the window's.
    const float aspect = (m_renderH > 0)
        ? float(m_renderW) / float(m_renderH)
        : (height() > 0 ? float(width()) / float(height()) : 1.0f);
    if (m_ortho) {
        // ORTHOGRAPHIC, the projection an axis view is for: parallel edges stay
        // parallel, so "is this symmetrical" and "do these two parts line up"
        // become answerable by eye. The half-height is derived from the
        // distance and the SAME 45° field, so switching projection does not
        // change how big the model is on screen — the gizmo's double-click
        // would otherwise look like a zoom.
        const float halfH = m_dist * std::tan(qDegreesToRadians(m_fov / 2.0f));
        const float halfW = halfH * aspect;
        // The near plane goes NEGATIVE. An ortho box that starts at the eye
        // clips away everything between the eye and the centre, which for a
        // model centred on m_center is its whole front half.
        proj.ortho(-halfW, halfW, -halfH, halfH, -m_dist * 40.0f,
                   m_dist * 40.0f);
    } else {
        proj.perspective(m_fov, aspect, qMax(0.001f, m_dist * 0.01f),
                         m_dist * 40.0f);
    }
    return proj * view;
}

void GLModelWidget::paintGL()
{
    const QColor bg = backgroundColor();
    // The PICK PASS clears to black, which is id 0, which is "nothing" — so a
    // click on empty space reads as empty space rather than as whatever the
    // background colour happens to encode.
    if (m_picking) glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    else
    // The clear's ALPHA is what a transparent capture needs: every fragment
    // this shader writes is opaque, so the only pixels that can carry alpha 0
    // are the ones nothing drew over — which is exactly the background. Left
    // at 1 for normal drawing; a capture asks for 0 around itself.
    glClearColor(float(bg.redF()), float(bg.greenF()), float(bg.blueF()),
                 m_clearAlpha);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (m_dirty) uploadPending();
    // Pose before the meshes-empty bail: a bone-only model still needs its
    // skeleton overlay reposed.
    if (m_poseDirty) applyPoseBuffers();
    if (!m_prog) return;

    const QMatrix4x4 mvp = viewProj();

    // Never wireframe in the pick pass: a wireframe pick would only hit the
    // handful of pixels the edges land on and read as empty space everywhere
    // else, so clicking a part you can plainly see would do nothing.
    glPolygonMode(GL_FRONT_AND_BACK,
                  (m_wireframe && !m_picking) ? GL_LINE : GL_FILL);
    m_prog->bind();
    m_prog->setUniformValue("uPick", m_picking ? 1 : 0);
    m_prog->setUniformValue("uMvp", mvp);
    m_prog->setUniformValue("uLightDir", keyDirection());
    // The rig. Intensity is folded into the colours here — see
    // ViewEnvironment.h for why that is one uniform rather than two.
    m_prog->setUniformValue("uKeyCol", m_env.key * m_keyGain);
    m_prog->setUniformValue("uFillCol", m_env.fill * m_ambientGain);
    m_prog->setUniformValue("uSkyCol", m_env.sky * m_ambientGain);
    m_prog->setUniformValue("uGroundCol", m_env.ground * m_ambientGain);
    // The fill's direction, negated on the way in: the shader takes a TRAVEL
    // direction for both lights, and the literal it replaced pointed AT the
    // surface. Same vector, stated in the shader's own convention.
    m_prog->setUniformValue("uFillDir", -m_env.fillDir.normalized());
    m_prog->setUniformValue("uExposure", m_exposure);
    // FLAT is the albedo channel with no channel chosen. uDebug == 1 already
    // means "base colour, unlit" in this shader, so the Flat mode is a
    // selection of an existing path rather than a new one — no uniform, no
    // branch, and it cannot drift from what Albedo shows. A channel the user
    // actually picked always wins: a channel view replaces the shaded result
    // outright, so the mode underneath it has nothing left to say.
    const int debugUniform = m_debug != fox::DebugView::Off
        ? int(m_debug)
        : (m_shading == ShadingMode::Flat ? int(fox::DebugView::Albedo) : 0);
    m_prog->setUniformValue("uDebug", debugUniform);
    m_prog->setUniformValue("uTex", 0);
    m_prog->setUniformValue("uNrm", 1);
    m_prog->setUniformValue("uSrm", 2);
    m_prog->setUniformValue("uTrm", 3);
    m_prog->setUniformValue("uLayer", 4);
    m_prog->setUniformValue("uLayerMask", 5);
    m_prog->setUniformValue("uMtm", 6);
    m_prog->setUniformValue("uSubNrm", 7);
    m_prog->setUniformValue("uShift", 8);
    m_prog->setUniformValue("uEye", eyePosition());
    static const QMatrix4x4 kIdentity;
    for (int mi = 0; mi < m_meshes.size(); ++mi) {
        const MeshGpu& m = m_meshes[mi];
        if (m_groupVisible.value(m.groupId, true) == false) continue;
        if (m_meshVisible.value(m.meshId, true) == false) continue;
        if (m_picking) {
            // The LOOP INDEX, not the meshId — a submesh with no id of its own
            // (meshId -1) still has to occlude the ones behind it, and encoding
            // -1 would make it transparent to the pick. +1 so that 0 stays
            // reserved for "nothing".
            const int id = mi + 1;
            m_prog->setUniformValue(
                "uPickColor",
                QVector3D(((id >> 16) & 0xFF) / 255.0f, ((id >> 8) & 0xFF) / 255.0f,
                          (id & 0xFF) / 255.0f));
        }
        m_prog->setUniformValue("uModel",
                                m_groupTransform.value(m.groupId, kIdentity));
        QOpenGLTexture* tex =
            m.materialSlot >= 0 && m.materialSlot < m_textures.size()
                ? m_textures[m.materialSlot]
                : nullptr;
        m_prog->setUniformValue("uHasTex", tex ? 1 : 0);
        if (tex) tex->bind(0);
        QOpenGLTexture* nrm =
            m_normalMapping && m.materialSlot >= 0
                    && m.materialSlot < m_normalTextures.size()
                ? m_normalTextures[m.materialSlot]
                : nullptr;
        m_prog->setUniformValue("uHasNrm", nrm ? 1 : 0);
        if (nrm) nrm->bind(1);

        // The PBR half. A slot with no entry (basic load, or a mesh whose
        // materialSlot is -1) falls back to the default GLPbrMaterial, which
        // turns every branch in the shader off — so the fragment program takes
        // the same path it did before any of this existed.
        static const GLPbrMaterial kNoPbr;
        const bool haveSlot = m.materialSlot >= 0 && m.materialSlot < m_pbr.size();
        const GLPbrMaterial& pm = haveSlot ? m_pbr[m.materialSlot] : kNoPbr;
        QOpenGLTexture* srm = haveSlot ? m_materialTextures[m.materialSlot] : nullptr;
        QOpenGLTexture* trm = haveSlot ? m_translucentTextures[m.materialSlot] : nullptr;
        QOpenGLTexture* lay = haveSlot ? m_layerTextures[m.materialSlot] : nullptr;
        QOpenGLTexture* lmk = haveSlot ? m_layerMaskTextures[m.materialSlot] : nullptr;
        m_prog->setUniformValue("uHasSrm", srm ? 1 : 0);
        m_prog->setUniformValue("uHasTrm", trm ? 1 : 0);
        if (srm) srm->bind(2);
        if (trm) trm->bind(3);
        // The layer composite needs BOTH halves. With a swatch and no mask
        // there is nothing to say where the colour applies, and multiplying it
        // everywhere would flood the whole surface with it.
        const int layerMode = (lay && lmk)
            ? (pm.layerMul ? 1 : (pm.layerBlend ? 2 : 0))
            : 0;
        m_prog->setUniformValue("uLayerMode", layerMode);
        m_prog->setUniformValue("uLayerRepeat",
                                QVector2D(pm.layerRepeat[0], pm.layerRepeat[1]));
        m_prog->setUniformValue("uLayerShift",
                                QVector2D(pm.layerShift[0], pm.layerShift[1]));
        if (layerMode) { lay->bind(4); lmk->bind(5); }
        QOpenGLTexture* mtm = haveSlot ? m_matParamTextures[m.materialSlot] : nullptr;
        m_prog->setUniformValue("uHasMtm", mtm ? 1 : 0);
        if (mtm) mtm->bind(6);
        // The detail normal. Bound only when the material asked for it AND the
        // base normal is on: the sub-normal perturbs the base one's slope, so
        // with normal mapping switched off there is nothing for it to perturb.
        QOpenGLTexture* sub =
            haveSlot ? m_subNormalTextures[m.materialSlot] : nullptr;
        const bool useSub = sub && nrm && pm.subBlend > 0.0f;
        m_prog->setUniformValue("uSubBlend", useSub ? pm.subBlend : 0.0f);
        m_prog->setUniformValue("uSubRepeat",
                                QVector2D(pm.subRepeat[0], pm.subRepeat[1]));
        if (useSub) sub->bind(7);
        // The rim needs no texture — it is two scalars, already zero on every
        // material whose shader does not name Incidence, so no gate is needed
        // here beyond passing them through.
        m_prog->setUniformValue("uIncidence", pm.incidence);
        m_prog->setUniformValue("uIncidenceTint",
                                QVector3D(pm.incidenceTint[0],
                                          pm.incidenceTint[1],
                                          pm.incidenceTint[2]));
        m_prog->setUniformValue("uIncidencePower", pm.incidencePower);
        // The hair lobe. Set for EVERY draw, not only for hair: a uniform left
        // over from the previous material is how one material comes to be
        // shaded with another's parameters, and this shader has been bitten by
        // exactly that before.
        QOpenGLTexture* shf =
            haveSlot ? m_shiftTextures[m.materialSlot] : nullptr;
        m_prog->setUniformValue("uHair", pm.hair ? 1 : 0);
        m_prog->setUniformValue("uHasShift", shf ? 1 : 0);
        m_prog->setUniformValue("uHairExp", pm.hairExponent);
        m_prog->setUniformValue("uHairShift", pm.hairShift);
        static const int kHairTanU =
            qEnvironmentVariableIsSet("FOXAB_HAIR_TANGENT_U") ? 1 : 0;
        m_prog->setUniformValue("uHairTangentU", kHairTanU);
        if (shf) shf->bind(8);
        m_prog->setUniformValue("uMaterialTypes", pm.materialTypes);
        // The four regions' reflectance. Set every frame per material rather
        // than cached: it is sixteen floats, and a stale set is a material
        // wearing another material's metal.
        for (int r = 0; r < 4; ++r) {
            if (m_locPresetA[r] >= 0)
                m_prog->setUniformValue(
                    m_locPresetA[r],
                    QVector4D(pm.presetF0[r], pm.presetTrans[r],
                              pm.presetAniso[r], 0.0f));
            if (m_locPresetSpec[r] >= 0)
                m_prog->setUniformValue(
                    m_locPresetSpec[r],
                    QVector3D(pm.presetSpec[r][0], pm.presetSpec[r][1],
                              pm.presetSpec[r][2]));
        }
        m_prog->setUniformValue("uSkin", pm.skin ? 1 : 0);
        m_prog->setUniformValue("uNoMetal", pm.noMetal ? 1 : 0);
        m_prog->setUniformValue("uUnlit", pm.unlit ? 1 : 0);
        // Full PBR lighting is on exactly when this scene was loaded with the
        // full map set. Deciding it per material — "has an SRM" — would light
        // one half of a character with GGX and the other with the old lambert,
        // and the seam between them is very visible on a face.
        m_prog->setUniformValue("uPbr", (m_hasPbr && m_pbrShading) ? 1 : 0);
        m.vao->bind();
        glDrawElements(GL_TRIANGLES, m.indexCount, GL_UNSIGNED_INT, nullptr);
        m.vao->release();
    }
    drawSelectionOutline();
    m_prog->release();
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    // No overlays in the pick pass: a grid line or a bone under the cursor
    // would be picked instead of the mesh it is drawn over.
    if (m_lineProg && !m_picking) drawOverlays(mvp);
}

// Everything the line shader draws on top of the scene: the ground grid, the
// axis triad, the skeleton and the connect-point crosses. One function so the
// depth-test dance and the shader bind happen once rather than four times, and
// so the drawing ORDER is written down: grid first (it belongs behind
// everything), then the model's own annotations.
void GLModelWidget::drawOverlays(const QMatrix4x4& mvp)
{
    if (m_cnpDirty) rebuildConnectPointLines();
    const bool anything = m_showGrid || m_showAxes || m_showSkeleton
        || (m_showConnectPoints && m_cnpVertexCount > 0);
    if (!anything) return;

    m_lineProg->bind();

    // The grid is sized to the scene, not to the world: a 1-metre grid under a
    // 30-centimetre pistol is a solid floor, and under a horse it is a hairline.
    // Rounded to a power of ten so the squares are a readable unit rather than
    // whatever the model's bounding sphere happened to be.
    if (m_showGrid && m_gridVao) {
        const float r = qMax(0.05f, m_sceneRadius);
        float step = std::pow(10.0f, std::floor(std::log10(r)));
        if (r / step < 2.0f) step *= 0.5f;
        const float extent = step * 10.0f;
        QMatrix4x4 m = mvp;
        m.translate(m_sceneCenter.x() - std::fmod(m_sceneCenter.x(), step), 0.0f,
                    m_sceneCenter.z() - std::fmod(m_sceneCenter.z(), step));
        m.scale(extent, 1.0f, extent);
        m_lineProg->setUniformValue("uMvp", m);
        // Behind the model, and dim: this is a reference, not content.
        m_lineProg->setUniformValue("uColor", QVector4D(0.5f, 0.5f, 0.55f, 1.0f));
        m_gridVao->bind();
        glDrawArrays(GL_LINES, 0, m_gridVertexCount);
        m_gridVao->release();
    }

    // From here on, ON TOP of the model — an annotation you cannot see because
    // the thing it annotates is in front of it is not an annotation.
    glDisable(GL_DEPTH_TEST);

    if (m_showAxes && m_axisVao) {
        const float len = qMax(0.05f, m_sceneRadius) * 0.5f;
        QMatrix4x4 m = mvp;
        m.scale(len, len, len);
        m_lineProg->setUniformValue("uMvp", m);
        m_axisVao->bind();
        const QVector4D cols[3] = {{0.90f, 0.25f, 0.25f, 1.0f},
                                   {0.35f, 0.85f, 0.35f, 1.0f},
                                   {0.35f, 0.55f, 0.95f, 1.0f}};
        for (int i = 0; i < 3; ++i) {
            m_lineProg->setUniformValue("uColor", cols[i]);
            glDrawArrays(GL_LINES, i * 2, 2);
        }
        m_axisVao->release();
    }

    if (m_showSkeleton && m_skelVao) {
        m_lineProg->setUniformValue("uMvp", mvp);
        m_lineProg->setUniformValue("uColor", QVector4D(1.0f, 0.62f, 0.1f, 1.0f));
        m_skelVao->bind();
        glDrawArrays(GL_LINES, 0, m_skelVertexCount);
        m_skelVao->release();
    }

    if (m_showConnectPoints && m_cnpVao && m_cnpVertexCount > 0) {
        m_lineProg->setUniformValue("uMvp", mvp);
        m_lineProg->setUniformValue("uColor", QVector4D(0.35f, 0.95f, 0.85f, 1.0f));
        m_cnpVao->bind();
        glDrawArrays(GL_LINES, 0, m_cnpVertexCount);
        m_cnpVao->release();
    }

    glEnable(GL_DEPTH_TEST);
    m_lineProg->release();
}

// Three crossed segments per point, sized to the scene so they stay visible on
// a horse and do not swallow a pistol. Rebuilt when the set changes OR when the
// pose moves, because a socket hung off a bone travels with it.
void GLModelWidget::rebuildConnectPointLines()
{
    m_cnpDirty = false;
    QVector<float> v;
    const float k = qMax(0.01f, m_sceneRadius * 0.035f);
    v.reserve(m_cnp.size() * 18);
    for (const GLConnectPoint& c : m_cnp) {
        QVector3D at = c.pos;
        if (c.bone >= 0 && c.bone < m_bonePosed.size() && c.bone < m_boneBind.size())
            at = c.pos + (m_bonePosed[c.bone] - m_boneBind[c.bone]);
        v << at.x() - k << at.y() << at.z() << at.x() + k << at.y() << at.z();
        v << at.x() << at.y() - k << at.z() << at.x() << at.y() + k << at.z();
        v << at.x() << at.y() << at.z() - k << at.x() << at.y() << at.z() + k;
    }
    m_cnpVertexCount = int(v.size() / 3);
    if (v.isEmpty()) return;
    if (!m_cnpVao) {
        m_cnpVao = new QOpenGLVertexArrayObject;
        m_cnpVao->create();
        m_cnpVao->bind();
        m_cnpVbo.create();
        m_cnpVbo.bind();
        m_cnpVbo.allocate(v.constData(), int(v.size() * sizeof(float)));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
        m_cnpVao->release();
        return;
    }
    m_cnpVao->bind();
    m_cnpVbo.bind();
    m_cnpVbo.allocate(v.constData(), int(v.size() * sizeof(float)));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    m_cnpVbo.release();
    m_cnpVao->release();
}

void GLModelWidget::resizeGL(int, int) {}

// ── Picking, the keyboard and fullscreen (template §5) ──────────────────────

int GLModelWidget::pickMeshAt(const QPoint& pos)
{
    if (!isValid() || !m_prog) return -1;
    makeCurrent();
    // FLUSH THE PENDING SCENE FIRST. Meshes are uploaded lazily inside
    // paintGL, so between "load this model" and the first frame m_meshes is
    // empty — and a pick in that window found nothing and reported empty
    // space. It is a narrow window for a person and a certainty for a script,
    // which is how it was found.
    if (m_dirty) uploadPending();
    if (m_poseDirty) applyPoseBuffers();
    if (m_meshes.isEmpty()) {
        doneCurrent();
        return -1;
    }
    const qreal dpr = devicePixelRatioF();
    const int w = qMax(1, int(width() * dpr));
    const int h = qMax(1, int(height() * dpr));
    // The SAME draw path, into an offscreen buffer. Rendering the scene twice
    // is cheap next to being right about a posed skin, and it means the pick
    // can never disagree with what is on screen about which mesh is in front.
    QOpenGLFramebufferObject fbo(w, h, QOpenGLFramebufferObject::Depth);
    if (!fbo.isValid() || !fbo.bind()) {
        doneCurrent();
        return -1;
    }
    glViewport(0, 0, w, h);
    // RAII, for the reason renderAtSize() states a few hundred lines up: this
    // project builds with /EHa and installs an SEH translator, so a driver
    // fault inside paintGL's uploads becomes a C++ exception — and unwinding
    // past a bare `m_picking = false` would leave EVERY subsequent on-screen
    // frame drawing flat id colours on black, with nothing in the UI able to
    // clear it.
    struct Restore {
        GLModelWidget* w;
        ~Restore() { w->m_picking = false; w->doneCurrent(); }
    } restore{this};
    m_picking = true;
    paintGL();

    const int px = qBound(0, int(pos.x() * dpr), w - 1);
    // GL's origin is bottom-left and the widget's is top-left.
    const int py = qBound(0, h - 1 - int(pos.y() * dpr), h - 1);
    quint8 rgba[4] = {0, 0, 0, 0};
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(px, py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    fbo.release();
    // Repaint the real frame. QOpenGLWidget sets its own glViewport before
    // every paintGL, so the FBO-sized one does not survive — but the widget
    // still has to be told to draw again.
    update();

    const int id = (int(rgba[0]) << 16) | (int(rgba[1]) << 8) | int(rgba[2]);
    if (id <= 0 || id > m_meshes.size()) return -1;
    return m_meshes[id - 1].meshId;
}

// The two outline colours. Named here so the shader call sites cannot drift
// apart, and matched to what the tool already used: warm for the selection
// (which persists), cool blue for the set a context menu is about (which goes
// away with the menu). D4 uses red and the same blue; the warm here stays the
// orange this viewport shipped with, because it is the one the user has
// already seen and approved and the pair reads at least as well.
const QVector3D GLModelWidget::kSelectColor{1.00f, 0.62f, 0.16f};
const QVector3D GLModelWidget::kContextColor{0.25f, 0.60f, 1.00f};

void GLModelWidget::setPickedMesh(int meshId)
{
    // REPLACES the selection. This is a plain click, and a plain click in
    // every list in this application replaces rather than adds.
    QSet<int> one;
    if (meshId >= 0) one.insert(meshId);
    setSelectedMeshes(one, meshId);
}

void GLModelWidget::setSelectedMeshes(const QSet<int>& ids, int active)
{
    // The ACTIVE one has to be a member, or "." frames something that is not
    // outlined and the INFO panel describes a submesh nothing points at.
    int act = active;
    if (act < 0 || !ids.contains(act))
        act = ids.isEmpty() ? -1 : *ids.constBegin();
    if (m_selected == ids && m_picked == act) return;
    m_selected = ids;
    m_picked = act;
    update();
}

void GLModelWidget::addToSelection(int meshId)
{
    if (meshId < 0 || m_selected.contains(meshId)) {
        // Already there: still make it ACTIVE. Shift-clicking a part that is
        // already selected is how anyone says "and frame this one".
        if (meshId >= 0 && m_picked != meshId) { m_picked = meshId; update(); }
        return;
    }
    m_selected.insert(meshId);
    m_picked = meshId;
    update();
}

void GLModelWidget::removeFromSelection(int meshId)
{
    if (meshId < 0 || !m_selected.remove(meshId)) return;
    if (m_picked == meshId)
        m_picked = m_selected.isEmpty() ? -1 : *m_selected.constBegin();
    update();
}

void GLModelWidget::toggleInSelection(int meshId)
{
    if (meshId < 0) return;
    if (m_selected.contains(meshId)) removeFromSelection(meshId);
    else addToSelection(meshId);
}

void GLModelWidget::clearSelection()
{
    if (m_selected.isEmpty() && m_picked < 0) return;
    m_selected.clear();
    m_picked = -1;
    update();
}

QString GLModelWidget::selectionForShot() const
{
    QList<int> ids(m_selected.constBegin(), m_selected.constEnd());
    std::sort(ids.begin(), ids.end());
    QStringList out;
    for (int i : ids) out << QString::number(i);
    return QStringLiteral("{%1} active %2")
        .arg(out.join(QLatin1Char(',')))
        .arg(m_picked);
}

void GLModelWidget::setShowSelection(bool on)
{
    if (m_showSelection == on) return;
    m_showSelection = on;
    update();
}

void GLModelWidget::frameMesh(int meshId)
{
    // -1, or a submesh whose source geometry is gone, frames the whole scene —
    // which is what "frame nothing in particular" should do.
    if (meshId < 0) {
        resetCamera();
        return;
    }
    QVector3D lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    bool any = false;
    for (const MeshGpu& m : m_meshes) {
        if (m.meshId != meshId) continue;
        if (m.srcIndex < 0 || m.srcIndex >= m_pendingMeshes.size()) continue;
        // THROUGH THE GROUP TRANSFORM, which is what paintGL draws these
        // vertices through. A part seated on a connect point — every attachment
        // in the Customize tab — is drawn somewhere its source vertices know
        // nothing about, so framing the raw bind positions flew the camera to
        // the world origin and left the part off screen.
        const QMatrix4x4 xf = m_groupTransform.value(m.groupId, QMatrix4x4());
        const QVector<float>& v = m_pendingMeshes[m.srcIndex].interleaved;
        for (int i = 0; i + 2 < v.size(); i += kVertexFloats) {
            const QVector3D p = xf.map(QVector3D(v[i], v[i + 1], v[i + 2]));
            lo.setX(qMin(lo.x(), p.x())); hi.setX(qMax(hi.x(), p.x()));
            lo.setY(qMin(lo.y(), p.y())); hi.setY(qMax(hi.y(), p.y()));
            lo.setZ(qMin(lo.z(), p.z())); hi.setZ(qMax(hi.z(), p.z()));
            any = true;
        }
    }
    if (!any) {
        resetCamera();
        return;
    }
    // BIND-SPACE bounds. Framing a posed mesh from its bind geometry puts the
    // camera in the right neighbourhood rather than exactly on it, which is
    // the honest limit of doing this without reading the skin back off the GPU
    // — and is still far better than not framing at all.
    const QVector3D c = (lo + hi) * 0.5f;
    const float r = qMax(0.001f, (hi - lo).length() * 0.5f);
    // THE CAMERA ONLY. centerOn() redefines the SCENE — m_sceneCenter,
    // m_sceneRadius, m_lastFramedRadius — and m_sceneRadius is what the wheel
    // clamps against (0.05x .. 40x). Framing an eyelash through it left a
    // character whose maximum zoom-out was closer than the distance that had
    // framed it, with middle-click "reset the camera" resetting to the eyelash
    // and no key able to recover. Framing a part is a camera move; it is not a
    // statement about how big the scene is.
    m_center = c;
    m_dist = qBound(m_sceneRadius * 0.05f, r * 2.4f, m_sceneRadius * 40.0f);
    Q_EMIT cameraChanged();
    update();
}

void GLModelWidget::setViewportFullscreen(bool on)
{
    if (m_fullscreen == on) return;
    m_fullscreen = on;
    Q_EMIT fullscreenChanged(on);
    update();
}

void GLModelWidget::setShowHelp(bool on)
{
    if (m_showHelp == on) return;
    m_showHelp = on;
    update();
}

// THE WHOLE SELECTION, not just the active one. Selecting four parts and
// pressing H hiding one of them is the sort of thing that reads as the key
// being broken.
void GLModelWidget::hidePicked()
{
    if (m_selected.isEmpty()) return;
    for (int id : m_selected) setMeshVisible(id, false);
    Q_EMIT meshVisibilityChanged();
}

void GLModelWidget::unhideAll()
{
    clearMeshVisibility();
    Q_EMIT meshVisibilityChanged();
}

void GLModelWidget::isolatePicked()
{
    if (m_selected.isEmpty()) return;
    for (const MeshGpu& m : m_meshes)
        if (m.meshId >= 0)
            setMeshVisible(m.meshId, m_selected.contains(m.meshId));
    Q_EMIT meshVisibilityChanged();
}

// SHIFT adds, CTRL toggles, a plain click replaces. The same three gestures as
// every list in this application, which is the point — a viewport whose
// selection rules are its own is a viewport you have to learn separately.
//
// On a DOUBLE-click as well as a single one: the first click of a double-click
// has already been delivered as a press, so handling only the double-click
// would make the modifiers work on the second click and not the first, and
// handling only the single one would make double-clicking a part deselect it.
void GLModelWidget::applyPickGesture(const QPoint& at, Qt::KeyboardModifiers mods)
{
    const int id = pickMeshAt(at);
    if (mods & Qt::ControlModifier) {
        toggleInSelection(id);
    } else if (mods & Qt::ShiftModifier) {
        addToSelection(id);
    } else {
        // A plain click on EMPTY SPACE clears, rather than leaving the last
        // selection outlined over nothing in particular.
        setPickedMesh(id);
    }
    Q_EMIT meshPicked(m_picked);
}

void GLModelWidget::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) {
        QOpenGLWidget::mouseDoubleClickEvent(e);
        return;
    }
    applyPickGesture(e->pos(), e->modifiers());
    e->accept();
}

// The keys themselves are QActions built from app/Hotkeys.h — see
// installViewportShortcuts(). Only Escape is handled here, because Escape is
// not a binding anyone should be able to move: it is the way OUT, and a way out
// that can be rebound is a way out that can be lost.
void GLModelWidget::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape) {
        // It LEAVES fullscreen and closes the help, and does nothing at all
        // otherwise — a viewport that swallows Escape is a viewport whose
        // search box cannot be cleared from the keyboard.
        if (m_showHelp) { setShowHelp(false); e->accept(); return; }
        if (m_fullscreen) { setViewportFullscreen(false); e->accept(); return; }
    }
    QOpenGLWidget::keyPressEvent(e);
}

// One QAction per registry row, scoped to this widget. Rebuilt whenever the
// bindings change, so Settings ▸ Hotkeys takes effect without a restart — the
// same contract every other shortcut in this application has.
void GLModelWidget::installViewportShortcuts()
{
    for (QAction* a : m_viewActions) { removeAction(a); delete a; }
    m_viewActions.clear();
    struct Row { const char* key; void (GLModelWidget::*fn)(); };
    static const Row kRows[] = {
        {"hotkeys/viewHide", &GLModelWidget::hidePicked},
        {"hotkeys/viewShowAll", &GLModelWidget::unhideAll},
        {"hotkeys/viewIsolate", &GLModelWidget::isolatePicked},
        {"hotkeys/viewFrame", &GLModelWidget::framePicked},
        {"hotkeys/viewFullscreen", &GLModelWidget::toggleFullscreen},
        {"hotkeys/viewHelp", &GLModelWidget::toggleHelp},
    };
    for (const Row& r : kRows) {
        const QString key = QString::fromLatin1(r.key);
        // Fullscreen belongs to a viewport whose HOST can actually do it. The
        // Files tab's preview has no fullscreen, and a key that set the flag
        // there raised an exit button over a pane that had not changed size.
        if (key == QLatin1String("hotkeys/viewFullscreen") && !m_fullscreenSupported)
            continue;
        const QKeySequence seq = Hotkeys::seq(key);
        if (seq.isEmpty()) continue;   // deliberately unbound is a real state
        auto* a = new QAction(this);
        a->setShortcut(seq);
        // WidgetWithChildren, not Window: two viewports exist at once in this
        // application and only the focused one should answer.
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        const auto fn = r.fn;
        connect(a, &QAction::triggered, this, [this, fn] { (this->*fn)(); });
        addAction(a);
        m_viewActions.append(a);
    }
}

void GLModelWidget::framePicked() { frameMesh(m_picked); }
void GLModelWidget::toggleFullscreen() { setViewportFullscreen(!m_fullscreen); }
void GLModelWidget::toggleHelp() { setShowHelp(!m_showHelp); }

void GLModelWidget::setFullscreenSupported(bool on)
{
    if (m_fullscreenSupported == on) return;
    m_fullscreenSupported = on;
    installViewportShortcuts();
}

void GLModelWidget::mousePressEvent(QMouseEvent* e)
{
    m_lastMouse = e->pos();
    if (e->button() == Qt::MiddleButton) {
        m_midPress = e->pos();
        m_midDragged = false;
    }
    if (e->button() == Qt::RightButton) {
        m_rightPress = e->pos();
        m_rightDragged = false;
    }
    // A modified LEFT click selects without waiting for a double-click. An
    // unmodified one does not: a bare left press is an orbit, and selecting on
    // it would make every camera move a selection change.
    if (e->button() == Qt::LeftButton
        && (e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) {
        applyPickGesture(e->pos(), e->modifiers());
        e->accept();
        return;
    }
}

void GLModelWidget::mouseReleaseEvent(QMouseEvent* e)
{
    // Middle CLICK resets the view; middle DRAG still pans, which is what the
    // middle button has always done here, so the two must not be confused. A
    // few pixels of travel while clicking is a click.
    if (e->button() == Qt::MiddleButton && !m_midDragged)
        resetCamera();
}

void GLModelWidget::mouseMoveEvent(QMouseEvent* e)
{
    const QPoint d = e->pos() - m_lastMouse;
    m_lastMouse = e->pos();
    if (e->buttons() & Qt::LeftButton) {
        m_yaw -= d.x() * 0.5f;
        m_pitch = qBound(-89.0f, m_pitch + d.y() * 0.5f, 89.0f);
        update();
        emit cameraChanged();
    } else if (e->buttons() & (Qt::MiddleButton | Qt::RightButton)) {
        if ((e->buttons() & Qt::MiddleButton)
            && (e->pos() - m_midPress).manhattanLength() > 4)
            m_midDragged = true;
        // The same test for the RIGHT button, and for a bigger reason: right
        // is the pan button AND the menu button, so every attempt to move the
        // camera ended with a context menu in the way. Blender's rule is the
        // one people already know — a right button that MOVED was a drag, and
        // a drag is not a click.
        if ((e->buttons() & Qt::RightButton)
            && (e->pos() - m_rightPress).manhattanLength() > 4)
            m_rightDragged = true;
        // Pan in view plane.
        const float yawR = qDegreesToRadians(m_yaw);
        const QVector3D right(std::cos(yawR), 0, -std::sin(yawR));
        const QVector3D up(0, 1, 0);
        const float scale = m_dist * 0.0022f;
        m_center += (-right * d.x() + up * d.y()) * scale;
        update();
        emit cameraChanged();
    }
}

// The context-menu event is where the drag test is spent, rather than in
// mouseReleaseEvent, because the policy here is CustomContextMenu: Qt's
// QWidget::event() turns QEvent::ContextMenu straight into
// customContextMenuRequested without ever calling contextMenuEvent(), so there
// is no virtual to override further down. Swallowing the event here is the one
// place that reaches every platform's idea of when the menu is due — press on
// X11, release on Windows.
bool GLModelWidget::event(QEvent* e)
{
    if (e->type() == QEvent::ContextMenu && m_rightDragged) {
        m_rightDragged = false;   // the next right-click is a click again
        e->accept();
        return true;
    }
    return QOpenGLWidget::event(e);
}

GLModelWidget::RightDragResult GLModelWidget::testRightDrag(const QPoint& from,
                                                            const QPoint& to)
{
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(from), QPointF(from),
                      Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    mousePressEvent(&press);
    QMouseEvent move(QEvent::MouseMove, QPointF(to), QPointF(to),
                     Qt::NoButton, Qt::RightButton, Qt::NoModifier);
    mouseMoveEvent(&move);
    if (!m_rightDragged) return RightDragResult::Click;   // a click gets it
    // The real event, through the real override. Returning true here IS the
    // swallow: QWidget::event() would otherwise have emitted
    // customContextMenuRequested, and the menu that follows blocks.
    QContextMenuEvent ctx(QContextMenuEvent::Mouse, to, mapToGlobal(to));
    return event(&ctx) ? RightDragResult::DragSwallowed
                       : RightDragResult::DragLeaked;
}

// ── The selection silhouette (template §5, ported from D4AssetBrowser) ──────
//
// What was here was a second WIREFRAME draw of the selected submesh. Its
// apparent thickness scaled with triangle density: on a dense head every edge
// drew and the "outline" read as a solid tinted blob, which is exactly the
// thing an outline exists not to be — it hid the material you selected the
// part in order to look at.
//
// D4's approach, and now this one: stamp the part's screen footprint into the
// STENCIL buffer, then redraw it as a flat colour offset by a few screen
// pixels in eight directions with the stencil test rejecting the interior.
// What survives is a constant-width ring hugging the silhouette, whatever the
// mesh density and whatever the distance.
//
// Screen-space offsets rather than a normal-pushed hull for a reason that
// applies here too: Fox meshes have split normals at UV seams, and a hull
// grown along the normals tears open along every one of them.
//
// Depth test OFF, so a part buried inside the body still shows — which is most
// of why anyone selects a part from the list rather than by clicking it.
//
// A STANDALONE pass after the draw loop, which an earlier attempt got wrong:
// it must re-bind uModel per mesh, because that is the group transform an
// attachment rides on. (It does NOT need bone state — this viewport poses on
// the CPU into the vertex buffer.)
void GLModelWidget::drawSelectionOutline()
{
    if (m_picking || !m_showSelection) return;
    if (m_selected.isEmpty() && m_context.isEmpty()) return;

    // Which meshes get which colour. The CONTEXT set wins: right-clicking a
    // selection is the user asking "the menu is about these", and drawing them
    // in the selection colour would make the two indistinguishable at exactly
    // the moment the difference matters.
    QVector<int> warm, cool;   // warm = selected, cool = right-clicked
    for (int i = 0; i < m_meshes.size(); ++i) {
        const MeshGpu& m = m_meshes[i];
        // The SAME two visibility tests the draw loop applies. An outline
        // around a hidden part is a selection you cannot see the subject of,
        // and with the depth test off it would hang in mid-air.
        if (m.meshId < 0 || !m.vao) continue;
        if (m_groupVisible.value(m.groupId, true) == false) continue;
        if (m_meshVisible.value(m.meshId, true) == false) continue;
        if (m_context.contains(m.meshId)) cool.append(i);
        else if (m_selected.contains(m.meshId)) warm.append(i);
    }
    if (warm.isEmpty() && cool.isEmpty()) return;

    // Is there a stencil attachment at all? QOpenGLWidget renders into its own
    // FBO and a driver may hand back a context whose format differs from the
    // one requested, so this is asked of the BOUND framebuffer rather than
    // assumed from the surface format.
    //
    // The attachment enum DIFFERS by target: GL_STENCIL is only legal for the
    // default framebuffer and GL_STENCIL_ATTACHMENT only for a named one.
    // Asking the wrong one raises GL_INVALID_OPERATION and leaves the result
    // untouched — which reads as "no stencil" and silently disables the whole
    // pass.
    GLint drawFbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
    GLint stencilAttach = GL_NONE;
    glGetFramebufferAttachmentParameteriv(
        GL_DRAW_FRAMEBUFFER, drawFbo == 0 ? GL_STENCIL : GL_STENCIL_ATTACHMENT,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &stencilAttach);
    const bool haveStencil = (stencilAttach != GL_NONE);

    // WHAT THIS PASS CHANGES, and what it must therefore put back. Getting
    // this wrong is not a subtle bug: the first version restored CULL_FACE by
    // enabling it, and this renderer never enables it — Fox models are drawn
    // two-sided — so one selection turned back-face culling on for the rest of
    // the session and every model after it came up full of black holes.
    // Measured on the very first screenshot of the pass.
    //
    // So the exit below restores the state this renderer actually keeps:
    // depth test on, depth writes on, culling OFF, blending OFF.
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);   // stamp the whole projected shape, both faces
    glDisable(GL_BLEND);
    m_prog->setUniformValue("uPick", 1);
    static const QMatrix4x4 kIdentity;

    auto drawGroup = [&](const QVector<int>& group) {
        for (int i : group) {
            const MeshGpu& m = m_meshes[i];
            m_prog->setUniformValue(
                "uModel", m_groupTransform.value(m.groupId, kIdentity));
            // The base texture stays bound so the shader honours the alpha
            // cutout: without it the outline of a hair card traces the card
            // RECTANGLE rather than the strands anyone can actually see.
            QOpenGLTexture* tex =
                m.materialSlot >= 0 && m.materialSlot < m_textures.size()
                    ? m_textures[m.materialSlot]
                    : nullptr;
            m_prog->setUniformValue("uHasTex", tex ? 1 : 0);
            if (tex) tex->bind(0);
            m.vao->bind();
            glDrawElements(GL_TRIANGLES, m.indexCount, GL_UNSIGNED_INT, nullptr);
            m.vao->release();
        }
    };

    if (haveStencil) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);   // wireframe mode would
        glEnable(GL_STENCIL_TEST);                   // stamp only the edges
        glStencilMask(0xFF);
        // Half-width in DEVICE pixels, so it looks the same on a hi-dpi
        // display rather than thinning out on one.
        const float r = 2.0f * float(devicePixelRatioF());
        const float w = float(qMax(1, m_renderW > 0 ? m_renderW : width()));
        const float h = float(qMax(1, m_renderH > 0 ? m_renderH : height()));
        const float sx = 2.0f * r / w;
        const float sy = 2.0f * r / h;
        static const float kDir[8][2] = {
            {1.0f, 0.0f},     {-1.0f, 0.0f},     {0.0f, 1.0f},
            {0.0f, -1.0f},    {0.707f, 0.707f},  {-0.707f, 0.707f},
            {0.707f, -0.707f}, {-0.707f, -0.707f},
        };
        // Warm first, cool second: each group gets a fresh mask, so the
        // right-click colour paints over the selection colour where two
        // outlined parts overlap on screen.
        for (int pass = 0; pass < 2; ++pass) {
            const QVector<int>& group = pass == 0 ? warm : cool;
            if (group.isEmpty()) continue;
            glClear(GL_STENCIL_BUFFER_BIT);
            // A — stamp the footprint into stencil with colour writes off.
            glStencilFunc(GL_ALWAYS, 1, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            m_prog->setUniformValue("uNdcOffset", QVector2D(0.0f, 0.0f));
            drawGroup(group);
            // B — the ring: only OUTSIDE the stamped footprint. Overlapping
            // jitters rewrite the same opaque colour, so no bookkeeping.
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            m_prog->setUniformValue("uPickColor",
                                    pass == 0 ? kSelectColor : kContextColor);
            for (const auto& d : kDir) {
                m_prog->setUniformValue("uNdcOffset",
                                        QVector2D(d[0] * sx, d[1] * sy));
                drawGroup(group);
            }
        }
        m_prog->setUniformValue("uNdcOffset", QVector2D(0.0f, 0.0f));
        glDisable(GL_STENCIL_TEST);
    } else {
        // No stencil attachment. The jittered draws would paint eight solid
        // copies of the part, so fall back to the wireframe this replaced —
        // worse, and still a visible selection, which is the point.
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glLineWidth(1.0f);
        m_prog->setUniformValue("uPickColor", kSelectColor);
        drawGroup(warm);
        m_prog->setUniformValue("uPickColor", kContextColor);
        drawGroup(cool);
    }

    m_prog->setUniformValue("uPick", 0);
    m_prog->setUniformValue("uNdcOffset", QVector2D(0.0f, 0.0f));
    glPolygonMode(GL_FRONT_AND_BACK, m_wireframe ? GL_LINE : GL_FILL);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_CULL_FACE);   // NOT enable — see the note above
    glDisable(GL_BLEND);
}

void GLModelWidget::setFieldOfView(float degrees)
{
    const float f = qBound(15.0f, degrees, 100.0f);
    if (qFuzzyCompare(m_fov, f)) return;
    m_fov = f;
    update();
    Q_EMIT cameraChanged();
}

void GLModelWidget::setOrthographic(bool on)
{
    if (m_ortho == on) return;
    m_ortho = on;
    update();
    Q_EMIT cameraChanged();
}

// Point the camera down one axis, keeping the distance and the centre. The
// gizmo's click; also what a "front/side/top view" shortcut would call.
//
// The yaw/pitch pair is what this camera IS — there is no free orientation to
// set — so an axis becomes two angles. Pitch ±90 is the clamp's own limit
// (±89 elsewhere) and is deliberately allowed here: a top view IS straight
// down, and 89° would leave a visible tilt that reads as a bug. The orbit
// handler clamps again on the next drag, which is where the gimbal actually
// matters.
void GLModelWidget::viewAlongAxis(int axis, bool negative)
{
    switch (axis) {
        case 0: m_yaw = negative ? -90.0f : 90.0f; m_pitch = 0.0f; break;
        case 1: m_yaw = 0.0f; m_pitch = negative ? -90.0f : 90.0f; break;
        case 2: m_yaw = negative ? 180.0f : 0.0f; m_pitch = 0.0f; break;
        default: return;
    }
    update();
    Q_EMIT cameraChanged();
}

void GLModelWidget::setContextMeshes(const QSet<int>& ids)
{
    if (m_context == ids) return;
    m_context = ids;
    update();
}

void GLModelWidget::wheelEvent(QWheelEvent* e)
{
    const float steps = e->angleDelta().y() / 120.0f;
    m_dist *= std::pow(0.88f, steps);
    m_dist = qBound(m_sceneRadius * 0.05f, m_dist, m_sceneRadius * 40.0f);
    update();
    emit cameraChanged();
}
