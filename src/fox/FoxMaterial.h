// FoxMaterial.h — what a Fox Engine material actually IS, so the viewport can
// shade one the way the game does instead of pasting its base map on a lambert.
//
// Two facts drive everything here, both MEASURED against the shipped archives
// (866 models, 12,796 texture references across TPP, MGO and Survive) rather
// than assumed:
//
//   1. Every texture reference names a ROLE — "Base_Tex_SRGB",
//      "SpecularMap_Tex_LIN", "Layer_Tex_SRGB" — addressed by StrCode32. There
//      are 29 distinct roles in the shipped data and all 29 resolve to real
//      names from the FMDL-Studio dictionary now shipped in dict/.
//   2. Every material names a SHADER, also by hash, and the shader NAME is a
//      feature list: "fox3DDF_Blin_LayerMul_SubNorm_Dirty_LNM" says Blinn
//      base + layer-multiply colouring + a sub normal map + a dirt pass. All
//      82 shaders in the shipped data resolve. Nothing here guesses from
//      texture presence — the shader is the authority, the textures follow.
//
// The channel meanings below are measured, not documented. FtexTool's own
// header comment says an SRM is R=AO, G=SpecularAlbedo, B=Roughness; that is
// contradicted by every sample in the archives (an eyelash would be a mirror,
// stone would be a mirror, cloth would be a mirror). What the data shows is:
//
//   SRM  R = ambient occlusion   67% of texels sit above 0.88 with a long
//                                tail down — the shape of baked AO.
//        G = roughness           a bell centred near 0.55, never 0. Eye 0.13,
//                                gun 0.48, cloth 0.82, stone 0.73.
//        B = reflection mask     61% exactly 0, tail to ~0.5, never above
//                                0.63 — a sparse mask, not a metalness map.
//                                Highest on eyes and vehicle panels.
//   TRM  greyscale translucency (subsurface amount), mostly near 0.
//   DTM  NOT a full-colour dirt overlay, which is what it looks like and what
//        this comment used to claim. Decoded across the shipped Dirty
//        materials its three channels are UNCORRELATED greyscale masks packed
//        into RGB (|corr| <= 0.21 between any pair, and no two channels are
//        ever equal on a texel) — three separate coverage maps, not one
//        coloured stain. Deliberately NOT composited: all 1409 Dirty
//        materials bind one, and NOT ONE of them carries a strength, a colour
//        or a combine parameter, so there is nothing in the data saying which
//        mask means what or how much of it to lay on. It is surfaced channel
//        by channel in the material inspector instead of guessed at.
//   MTM  greyscale, quantised to a few bands — a material-ID map indexing the
//        FMTT preset table. Not consumed here yet.
//   LYM/LBM  greyscale layer blend weight; the colour itself is a separate
//        flat Layer_Tex_SRGB swatch. See LayerColors.h.
#pragma once
#include <QString>
#include <cstdint>

namespace fox {

// The texture roles this renderer knows how to consume, by StrCode32 of the
// role name. Values are measured, not derived at runtime, so a role can be
// tested for without hashing a string per material per frame.
namespace texrole {
constexpr quint32 kBase          = 0x7be81b61u;  // Base_Tex_SRGB
constexpr quint32 kBaseLin       = 0x3387a4dfu;  // Base_Tex_LIN
constexpr quint32 kBase2         = 0x219bf687u;  // Base_Tex2_SRGB
constexpr quint32 kNormal        = 0x05511ae0u;  // NormalMap_Tex_NRM
constexpr quint32 kSpecular      = 0x6b98b10eu;  // SpecularMap_Tex_LIN (the SRM)
constexpr quint32 kTranslucent   = 0xdd44f70au;  // Translucent_Tex_LIN
constexpr quint32 kLayer         = 0x9826dbe6u;  // Layer_Tex_SRGB
constexpr quint32 kLayerMask     = 0xf91a8b72u;  // LayerMask_Tex_LIN
constexpr quint32 kDirty         = 0x7bcbb6cbu;  // Dirty_Tex_LIN
constexpr quint32 kMask          = 0xb75cd480u;  // Mask_Tex_LIN
constexpr quint32 kSubNormal     = 0x5122ceeeu;  // SubNormalMap_Tex_NRM
constexpr quint32 kSubNormalMask = 0x41d44583u;  // SubNormalMask_Tex_LIN
constexpr quint32 kTensionSubNrm = 0xf0a48d2fu;  // TensionSubNormalMap_Tex_NRM
constexpr quint32 kInternalNrm   = 0x78da2c55u;  // InternalNormalMap_Tex_NRM
constexpr quint32 kMatParamMap   = 0x2dfd5885u;  // MatParamMap_Tex_LIN (the MTM)
// MatParamIndex_0..3 — NOT textures. These are named float4 parameters on the
// material whose x holds a row number in the FMTT preset table, one per region
// of a 2MT/3MT/4MT surface.
// URepeat_UV / VRepeat_UV / UShift_UV / VShift_UV — also NOT textures, also
// named float4 parameters with the scalar in x. UV tiling, and WHICH map it
// tiles depends on the material: the colour layer when there is one, the
// sub-normal when there is not. URepeat_SubNorm_UV is a separate control
// naming the sub-normal explicitly, and it appears only on layer-family
// materials — all 184 of them — which is exactly what you would expect if its
// job is to disambiguate from a plain pair that is already spoken for. See
// GLPbrMaterial::layerRepeat for the full measurement and for the 21 hair
// materials that remain unexplained.
constexpr quint32 kURepeatUv = 0xfe2ca129u;
constexpr quint32 kVRepeatUv = 0xfa3697b2u;
constexpr quint32 kUShiftUv  = 0x5e0e16ebu;
constexpr quint32 kVShiftUv  = 0x962e408eu;

// ── WHAT A ROLE IS FOR, IN ONE OR TWO WORDS ──────────────────────────────
// "Base_Tex_SRGB" and "SpecularMap_Tex_LIN" are the names the FILE uses, and
// they are the right thing to copy and to export under. They are the wrong
// thing to READ down a column: the user asked for the outliner to say what
// each texture is responsible for — base colour, normal, roughness — beside
// its name, which is a question about the role's MEANING.
//
// Fox's vocabulary, not another engine's. The SRM really is three maps in one
// and calling it "roughness" would be a lie the rest of this tool has gone to
// some trouble not to tell (see the channel measurements above).
inline QString roleDisplayName(quint32 hash, const QString& raw)
{
    switch (hash) {
        case kBase: case kBaseLin: case kBase2: return QStringLiteral("BASE COLOUR");
        case kNormal:              return QStringLiteral("NORMAL");
        case kSpecular:            return QStringLiteral("AO · ROUGH · REFL");
        case kTranslucent:         return QStringLiteral("TRANSLUCENCY");
        case kLayer:               return QStringLiteral("LAYER COLOUR");
        case kLayerMask:           return QStringLiteral("LAYER MASK");
        case kDirty:               return QStringLiteral("DIRT MASKS");
        case kMask:                return QStringLiteral("MASK");
        case kSubNormal:           return QStringLiteral("SUB NORMAL");
        case kSubNormalMask:       return QStringLiteral("SUB NORMAL MASK");
        case kTensionSubNrm:       return QStringLiteral("TENSION NORMAL");
        case kInternalNrm:         return QStringLiteral("INTERNAL NORMAL");
        case kMatParamMap:         return QStringLiteral("MATERIAL ID");
        default: break;
    }
    // An unknown role still reads better with the plumbing filed off: the
    // "_Tex_SRGB" / "_Tex_LIN" / "_Tex_NRM" suffix is the colour space, which
    // is on the texture itself and not what the slot is FOR.
    QString s = raw;
    const int cut = s.indexOf(QStringLiteral("_Tex"));
    if (cut > 0) s.truncate(cut);
    s.replace(QLatin1Char('_'), QLatin1Char(' '));
    return s.isEmpty() ? QStringLiteral("TEXTURE") : s.toUpper();
}

constexpr quint32 kMatParamIndex[4] = {
    0xe38487ffu, 0x89662077u, 0x065f2f0eu, 0x99a419a6u,
};

// The SUB-NORMAL's own controls — a detail normal laid over the base one, and
// the reason cloth reads as woven rather than smooth. Its own tiling pair,
// separate from the plain URepeat_UV that tiles the colour layer, and a blend
// weight that every one of the 78 materials binding a real sub-normal carries.
//
// Measured across TPP/GZ/MGO3/Survive:
//   every material that binds a real SubNormalMap_Tex_NRM also carries
//       SubNormal_Blend, and 17 of them set it to ZERO — a sub-normal bound
//       and deliberately switched off, which is honoured
//   27  materials both bind a sub-normal AND carry the SubNorm tiling pair;
//       all 27 are layer-family
//  184  materials carry the SubNorm tiling pair at all, so 157 of them tile
//       nothing — the same over-declaration every other parameter here shows.
//       All 184 are layer-family and all 184 also carry the plain pair.
//   31  non-layer materials declare a sub-normal and carry the PLAIN pair
//       instead, with no SubNorm-named pair anywhere on them
// The Shift pair is named in the dictionary and appears on NO shipped material,
// so it is not read.
constexpr quint32 kSubNormalBlend  = 0xb3c232eeu;   // SubNormal_Blend
constexpr quint32 kURepeatSubNorm  = 0x3b612a0cu;   // URepeat_SubNorm_UV
constexpr quint32 kVRepeatSubNorm  = 0xdcd58b04u;   // VRepeat_SubNorm_UV

// The INCIDENCE pair — a rim term, brightest where the surface turns away from
// the eye. Read off the shipped data rather than assumed, and note how far
// apart the two populations are:
//
//   1362 materials carry Incidence_Color and 1370 carry Incidence_Roughness,
//     but only 89 name "Incidence" in their SHADER. The parameters are
//     over-declared exactly as the tiling and sub-normal ones are, so the
//     shader is the gate — applying the rim on parameter presence alone would
//     have put one on every second surface in the game.
//
// Over the ones the shader does select (every one carries both parameters):
//   Incidence_Color      w is the STRENGTH — 0.25 on 45, 0.35 on 12, 0.2 and
//                        0.15 on 6, 0.5 on 5, then 0.1 / 0.3 / 1.0 on 4 each,
//                        and ZERO on 3, which is a rim declared and switched
//                        off. rgb is a real TINT and is NOT always white: of
//                        the 110 rows on Incidence-naming shaders, 74 are
//                        (1,1,1) and 36 are not — 0.93 grey on 18, 0.856 grey
//                        on 14, a cool (0.98,0.98,1.0) on 2, and two warm
//                        rims at (1.0,0.91,0.80) and (1.0,0.898,0.80). An
//                        earlier note here claimed white on every one; that
//                        count had been taken over the whole 1,362-material
//                        population, which is dominated by materials carrying
//                        the parameter on a shader that never reads it.
//   Incidence_Roughness  a lone scalar in x: 4.0 on 40, 5.0 on 26, 0.5 on 12,
//                        3.0 and 6.0 on 4, 2.0 on 2, 3.5 on 1. The 0.5 is a
//                        BROAD lift rather than a thin edge and is why the
//                        loader's lower bound sits below 1.
// They land on skin and cloth — the MGO avatar heads, the bd* bodies, the dl*
// suits, the horse and the dogs — which is where a grazing-angle sheen goes.
//
// A white weight of a tenth to a half with a companion exponent in that range
// is a Schlick-style rim and very little else. That is how it is applied —
// see the shader — and the reading is stated there as OURS rather than as
// something the data spells out.
constexpr quint32 kIncidenceColor = 0xd1661df2u;    // Incidence_Color
constexpr quint32 kIncidenceRough = 0x6fd70c2cu;    // Incidence_Roughness
constexpr quint32 kMetalicLayer  = 0x9bf3c078u;  // MetalicLayer_Tex_LIN
constexpr quint32 kMetalicBact   = 0x7862339du;  // MetalicBacteria_Tex_LIN
constexpr quint32 kViewReflect   = 0x617f5782u;  // ViewReflection_Tex_LIN
constexpr quint32 kLensHeight    = 0x5cb96475u;  // LensHeight_Tex_LIN
constexpr quint32 kRoughness     = 0x17f2f724u;  // RoughnessMap_Tex_LIN
constexpr quint32 kShift         = 0xdfde8edau;  // Shift_Tex_LIN — hair strands

// HAIR. fox3DDF_Hair is the one shader family with a lighting model of its
// own, and all 51 shipped hair materials carry the same four things: a
// Shift_Tex_LIN, HairShiftScale, Anistropic_Diffusion and
// Anistropic_MainLightDir. Their bound roles are ONLY Base / SpecularMap /
// Shift / Translucent — no normal map, no layer, no sub-normal — so a hair
// material rendered through the ordinary Blinn path has nothing to make it
// look like hair at all.
//
// Measured over all 51:
//   Anistropic_Diffusion     a lone scalar in x: 16 on 39, then 30 on 5, 50 on
//                            3, and one each of 32, 60, 64. A specular
//                            exponent, and squarely in Kajiya-Kay territory.
//   HairShiftScale           1.0 on 30, 0.5 on 20, 20.0 on one.
//   Anistropic_MainLightDir  a direction: (0,0,1) on 37, (0,1,-1) on 6,
//                            (0,0,1) w=1 on 3, (0,1,0) on 2, (0,1,-0.7) on 2,
//                            zero on one. NOT applied — every one of those is
//                            a plausible WORLD light direction and none is a
//                            plausible tangent-space strand direction, so the
//                            reading is that hair is lit from a fixed
//                            direction of its own. Honouring that would light
//                            a character's hair from somewhere other than the
//                            face under it, which is worse than not honouring
//                            it. See GLPbrMaterial::hairExponent.
constexpr quint32 kAnisoDiffusion = 0x601feb94u;  // Anistropic_Diffusion
constexpr quint32 kAnisoLightDir  = 0x3f1e486au;  // Anistropic_MainLightDir
constexpr quint32 kHairShiftScale = 0xca88efe5u;  // HairShiftScale
}  // namespace texrole

// The shading model a material asks for, read off the shader NAME.
enum class ShaderKind {
    Blin,               // fox3DDF_Blin_* — the ordinary PBR surface
    Skin,               // fox3DDF_Skin_* — translucency, tension normals
    Cloth,              // fox3DDF_Cloth_*
    Hair,               // fox3DDF_Hair*
    Eye,                // fox3DDF_Eye_* — its own reflection texture
    Glass,              // fox3DFW_Glass* — transparent
    Constant,           // fox3DFW_Constant* — unlit
    MetalicBacteria,    // tpp3DDF_MetalicBacteria* — Survive's crystal growth
    Other
};

// Everything the viewport needs to know about one material, derived from the
// shader name alone. A material whose shader did NOT resolve comes back as
// { Blin, no features } — the neutral reading, which is what the old
// base+normal path already assumed, so an unresolved shader can never render
// worse than it did before this existed.
struct MaterialModel {
    ShaderKind kind = ShaderKind::Blin;
    bool layerMul = false;   // "LayerMul": the layer swatch MULTIPLIES the base
    bool layerBlend = false; // "LayerBl": the layer swatch REPLACES it by mask
    bool dirty = false;      // "Dirty"/"RustDirty": carries a dirt overlay
    bool subNormal = false;  // "SubNorm"
    bool tension = false;    // "Tension"
    bool incidence = false;  // "Incidence": fresnel-weighted term
    bool forward = false;    // "3DFW": forward-lit (glass, unlit, additive)
    bool alphaCutout = false;// "3DDC"/"3DDF_Hair": punch-through geometry
    int materialTypes = 1;   // "_2MT_"/"_3MT_"/"_4MT_" regions via the MTM
    // True when this material takes a runtime colour at all — which is what
    // makes a piece of gear "colour customizable" and why it ships white.
    bool colourable() const { return layerMul || layerBlend; }
};

// Classify by shader name. Case-sensitive on the Fox spelling ("Blin",
// "LayerMul", "SubNorm") because those are the literal tokens in the names.
MaterialModel classifyShader(const QString& shaderName);

}  // namespace fox
