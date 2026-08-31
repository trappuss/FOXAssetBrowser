// FoxMaterial.cpp — see FoxMaterial.h.
#include "fox/FoxMaterial.h"

namespace fox {

MaterialModel classifyShader(const QString& shaderName)
{
    MaterialModel m;
    // An unresolved shader arrives as "0x<hash>". Reading feature tokens out
    // of a hex string would be nonsense, so leave the neutral default: that is
    // exactly the base+normal reading the viewport used before this existed.
    if (shaderName.isEmpty() || shaderName.startsWith(QLatin1String("0x")))
        return m;

    const QString& s = shaderName;
    const auto has = [&s](const char* tok) {
        return s.contains(QLatin1String(tok), Qt::CaseSensitive);
    };

    // Family. Tested most specific first: "MetalicBacteriaHair" is a bacteria
    // shader, not a hair one, and "Skin_LayerMul" is skin. Order matters.
    if (has("MetalicBacteria"))   m.kind = ShaderKind::MetalicBacteria;
    else if (has("_Skin"))        m.kind = ShaderKind::Skin;
    else if (has("_Eye"))         m.kind = ShaderKind::Eye;
    else if (has("_Hair"))        m.kind = ShaderKind::Hair;
    else if (has("_Cloth"))       m.kind = ShaderKind::Cloth;
    else if (has("Glass"))        m.kind = ShaderKind::Glass;
    // "Constant" is unlit; "Lambert" is NOT — it is diffuse-only lighting, so
    // folding it in here would render a lit surface fullbright. It falls
    // through to Other, which shades normally.
    else if (has("Constant"))     m.kind = ShaderKind::Constant;
    else if (has("_Blin"))        m.kind = ShaderKind::Blin;
    else                          m.kind = ShaderKind::Other;

    // Features. "LayerMul" and "LayerBl" are distinct and never both present;
    // testing LayerMul first means a "LayerBl" name cannot set both flags,
    // which would leave the shader with two conflicting composites.
    if (has("LayerMul"))      m.layerMul = true;
    else if (has("LayerBl"))  m.layerBlend = true;
    m.dirty     = has("Dirty");
    m.subNormal = has("SubNorm");
    m.tension   = has("Tension");
    m.incidence = has("Incidence");

    // Pipeline. "3DFW" is the forward pass (glass, unlit, additive); "3DDC" is
    // the deferred CUTOUT pass. Hair is punch-through whichever pass it is in.
    m.forward     = has("3DFW") || has("3DNDW");
    m.alphaCutout = has("3DDC") || m.kind == ShaderKind::Hair;

    // Region count. The MTM indexes an FMTT preset per region, so a 4MT
    // material is four surfaces sharing one mesh. Consumed by loadPbrMaps(),
    // which reads the material's MatParamIndex_0..3 into four presets, and by
    // the shader, which picks between them per texel.
    if (has("_4MT"))      m.materialTypes = 4;
    else if (has("_3MT")) m.materialTypes = 3;
    else if (has("_2MT")) m.materialTypes = 2;

    return m;
}

}  // namespace fox
