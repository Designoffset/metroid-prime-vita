// webgpu_vita: Cg programs for GameCube draws, generated from Aurora's
// ShaderConfig. The rules follow Aurora's WGSL generator (gx/shader.cpp):
//   vertex:   projection (WebGPU depth 0..w turned into GL's -w..w), GX colour
//             channels with per-vertex lighting;
//   fragment: texture samples with the TEV swap tables, the TEV stages
//             (including their 8-bit overflow), fog and the alpha test.
// Position, normal and texture coordinates arrive already transformed by the
// CPU (wv_gx.cpp) or in the vertex shader. Emboss offsets, PBR and the HD extras
// are not reproduced.
#include "wv.hpp"
#include "wv_gx.hpp"

#include <cstdarg>
#include <cstdio>
#include <string>

namespace wv {

using namespace aurora::gx;

namespace {

std::string num(int v) { return std::to_string(v); }

std::string fmtf(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmtf(const char* f, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

bool is_alpha_bump(GXChannelID id) { return id == GX_ALPHA_BUMP || id == GX_ALPHA_BUMPN; }

int channel_index(GXChannelID id) {
  switch (id) {
  case GX_COLOR0:
  case GX_ALPHA0:
  case GX_COLOR0A0:
    return 0;
  case GX_COLOR1:
  case GX_ALPHA1:
  case GX_COLOR1A1:
    return 1;
  default:
    return -1;
  }
}

const char* chan_comp(GXTevColorChan c) {
  switch (c) {
  case GX_CH_RED:
    return "r";
  case GX_CH_GREEN:
    return "g";
  case GX_CH_BLUE:
    return "b";
  case GX_CH_ALPHA:
    return "a";
  default:
    return "r";
  }
}

bool uses_texture_sample(const TevStage& s) {
  if (s.texMapId == GX_TEXMAP_NULL) return false;
  const auto& c = s.colorPass;
  const auto& a = s.alphaPass;
  return c.a == GX_CC_TEXC || c.a == GX_CC_TEXA || c.b == GX_CC_TEXC || c.b == GX_CC_TEXA || c.c == GX_CC_TEXC ||
         c.c == GX_CC_TEXA || c.d == GX_CC_TEXC || c.d == GX_CC_TEXA || a.a == GX_CA_TEXA || a.b == GX_CA_TEXA ||
         a.c == GX_CA_TEXA || a.d == GX_CA_TEXA;
}

std::string kcolor_c(GXTevKColorSel sel) {
  switch (sel) {
  case GX_TEV_KCSEL_8_8:
    return "f3(1.0)";
  case GX_TEV_KCSEL_7_8:
    return "f3(7.0/8.0)";
  case GX_TEV_KCSEL_6_8:
    return "f3(6.0/8.0)";
  case GX_TEV_KCSEL_5_8:
    return "f3(5.0/8.0)";
  case GX_TEV_KCSEL_4_8:
    return "f3(4.0/8.0)";
  case GX_TEV_KCSEL_3_8:
    return "f3(3.0/8.0)";
  case GX_TEV_KCSEL_2_8:
    return "f3(2.0/8.0)";
  case GX_TEV_KCSEL_1_8:
    return "f3(1.0/8.0)";
  default:
    break;
  }
  if (sel >= GX_TEV_KCSEL_K0 && sel <= GX_TEV_KCSEL_K3) return fmtf("uKc[%d].rgb", sel - GX_TEV_KCSEL_K0);
  if (sel >= GX_TEV_KCSEL_K0_R && sel <= GX_TEV_KCSEL_K3_A) {
    // K0_R, K1_R, K2_R, K3_R, K0_G, ... (four per component)
    const int k = (sel - GX_TEV_KCSEL_K0_R) % 4;
    const int comp = (sel - GX_TEV_KCSEL_K0_R) / 4;
    return fmtf("f3(uKc[%d].%c)", k, "rgba"[comp]);
  }
  return "f3(1.0)";
}

std::string kcolor_a(GXTevKAlphaSel sel) {
  switch (sel) {
  case GX_TEV_KASEL_8_8:
    return "1.0";
  case GX_TEV_KASEL_7_8:
    return "(7.0/8.0)";
  case GX_TEV_KASEL_6_8:
    return "(6.0/8.0)";
  case GX_TEV_KASEL_5_8:
    return "(5.0/8.0)";
  case GX_TEV_KASEL_4_8:
    return "(4.0/8.0)";
  case GX_TEV_KASEL_3_8:
    return "(3.0/8.0)";
  case GX_TEV_KASEL_2_8:
    return "(2.0/8.0)";
  case GX_TEV_KASEL_1_8:
    return "(1.0/8.0)";
  default:
    break;
  }
  if (sel >= GX_TEV_KASEL_K0_R && sel <= GX_TEV_KASEL_K3_A) {
    const int k = (sel - GX_TEV_KASEL_K0_R) % 4;
    const int comp = (sel - GX_TEV_KASEL_K0_R) / 4;
    return fmtf("uKc[%d].%c", k, "rgba"[comp]);
  }
  return "1.0";
}

std::string color_arg(GXTevColorArg arg, int idx, const ShaderConfig& c, const TevStage& s) {
  switch (arg) {
  case GX_CC_CPREV:
    return "prev.rgb";
  case GX_CC_APREV:
    return "f3(prev.a)";
  case GX_CC_C0:
    return "tevreg0.rgb";
  case GX_CC_A0:
    return "f3(tevreg0.a)";
  case GX_CC_C1:
    return "tevreg1.rgb";
  case GX_CC_A1:
    return "f3(tevreg1.a)";
  case GX_CC_C2:
    return "tevreg2.rgb";
  case GX_CC_A2:
    return "f3(tevreg2.a)";
  case GX_CC_TEXC: {
    if (s.texMapId == GX_TEXMAP_NULL) return "f3(1.0)";
    const auto& sw = c.tevSwapTable[s.tevSwapTex];
    return fmtf("sampled%d.%s%s%s", idx, chan_comp(sw.red), chan_comp(sw.green), chan_comp(sw.blue));
  }
  case GX_CC_TEXA: {
    if (s.texMapId == GX_TEXMAP_NULL) return "f3(1.0)";
    const auto& sw = c.tevSwapTable[s.tevSwapTex];
    return fmtf("f3(sampled%d.%s)", idx, chan_comp(sw.alpha));
  }
  case GX_CC_RASC: {
    const int ch = channel_index(s.channelId);
    if (ch < 0 || is_alpha_bump(s.channelId)) return "f3(0.0)";
    const auto& sw = c.tevSwapTable[s.tevSwapRas];
    return fmtf("rast%d.%s%s%s", ch, chan_comp(sw.red), chan_comp(sw.green), chan_comp(sw.blue));
  }
  case GX_CC_RASA: {
    const int ch = channel_index(s.channelId);
    if (ch < 0 || is_alpha_bump(s.channelId)) return "f3(0.0)";
    const auto& sw = c.tevSwapTable[s.tevSwapRas];
    return fmtf("f3(rast%d.%s)", ch, chan_comp(sw.alpha));
  }
  case GX_CC_ONE:
    return "f3(1.0)";
  case GX_CC_HALF:
    return "f3(0.5)";
  case GX_CC_KONST:
    return kcolor_c(s.kcSel);
  case GX_CC_ZERO:
  default:
    return "f3(0.0)";
  }
}

std::string alpha_arg(GXTevAlphaArg arg, int idx, const ShaderConfig& c, const TevStage& s) {
  switch (arg) {
  case GX_CA_APREV:
    return "prev.a";
  case GX_CA_A0:
    return "tevreg0.a";
  case GX_CA_A1:
    return "tevreg1.a";
  case GX_CA_A2:
    return "tevreg2.a";
  case GX_CA_TEXA: {
    if (s.texMapId == GX_TEXMAP_NULL) return "1.0";
    const auto& sw = c.tevSwapTable[s.tevSwapTex];
    return fmtf("sampled%d.%s", idx, chan_comp(sw.alpha));
  }
  case GX_CA_RASA: {
    const int ch = channel_index(s.channelId);
    if (ch < 0 || is_alpha_bump(s.channelId)) return "0.0";
    const auto& sw = c.tevSwapTable[s.tevSwapRas];
    return fmtf("rast%d.%s", ch, chan_comp(sw.alpha));
  }
  case GX_CA_KONST:
    return kcolor_a(s.kaSel);
  case GX_CA_ZERO:
  default:
    return "0.0";
  }
}

bool color_arg_normalized(GXTevColorArg arg, const bool* cn, const bool* an) {
  switch (arg) {
  case GX_CC_CPREV:
    return cn[0];
  case GX_CC_APREV:
    return an[0];
  case GX_CC_C0:
    return cn[1];
  case GX_CC_A0:
    return an[1];
  case GX_CC_C1:
    return cn[2];
  case GX_CC_A1:
    return an[2];
  case GX_CC_C2:
    return cn[3];
  case GX_CC_A2:
    return an[3];
  default:
    return true;
  }
}

bool alpha_arg_normalized(GXTevAlphaArg arg, const bool* an) {
  switch (arg) {
  case GX_CA_APREV:
    return an[0];
  case GX_CA_A0:
    return an[1];
  case GX_CA_A1:
    return an[2];
  case GX_CA_A2:
    return an[3];
  default:
    return true;
  }
}

const char* tev_bias(GXTevBias b) {
  switch (b) {
  case GX_TB_ADDHALF:
    return " + 0.5";
  case GX_TB_SUBHALF:
    return " - 0.5";
  default:
    return "";
  }
}

const char* tev_scale(GXTevScale s) {
  switch (s) {
  case GX_CS_SCALE_2:
    return " * 2.0";
  case GX_CS_SCALE_4:
    return " * 4.0";
  case GX_CS_DIVIDE_2:
    return " / 2.0";
  default:
    return "";
  }
}

// a, b, c, d as expressions; zero is the type's zero ("f3(0.0)" or "0.0").
std::string tev_op(GXTevOp op, GXTevBias bias, GXTevScale scale, const std::string& a, const std::string& b,
                   const std::string& c, const std::string& d, const char* zero, bool isColor) {
  const std::string A = "(" + a + ")", B = "(" + b + ")", C = "(" + c + ")", D = "(" + d + ")";
  auto sel = [&](const std::string& cond) { return "((" + cond + ") ? " + C + " : " + zero + ") + " + D; };
  switch (op) {
  case GX_TEV_ADD:
  case GX_TEV_SUB:
    return std::string("((") + (op == GX_TEV_SUB ? "-" : "") + "lerp(" + A + ", " + B + ", " + C + ") + " + D + ")" +
           tev_bias(bias) + ")" + tev_scale(scale);
  case GX_TEV_COMP_R8_GT:
    return sel("round(" + A + ".r * 255.0) > round(" + B + ".r * 255.0)");
  case GX_TEV_COMP_R8_EQ:
    return sel("round(" + A + ".r * 255.0) == round(" + B + ".r * 255.0)");
  case GX_TEV_COMP_GR16_GT:
    return sel("round(dot(" + A + ".rg * 255.0, float2(1.0, 256.0))) > round(dot(" + B +
               ".rg * 255.0, float2(1.0, 256.0)))");
  case GX_TEV_COMP_GR16_EQ:
    return sel("round(dot(" + A + ".rg * 255.0, float2(1.0, 256.0))) == round(dot(" + B +
               ".rg * 255.0, float2(1.0, 256.0)))");
  case GX_TEV_COMP_BGR24_GT:
    return sel("round(dot(" + A + ".rgb * 255.0, float3(1.0, 256.0, 65536.0))) > round(dot(" + B +
               ".rgb * 255.0, float3(1.0, 256.0, 65536.0)))");
  case GX_TEV_COMP_BGR24_EQ:
    return sel("round(dot(" + A + ".rgb * 255.0, float3(1.0, 256.0, 65536.0))) == round(dot(" + B +
               ".rgb * 255.0, float3(1.0, 256.0, 65536.0)))");
  case GX_TEV_COMP_RGB8_GT: // alpha: GX_TEV_COMP_A8_GT
    return sel("round(" + A + " * 255.0) > round(" + B + " * 255.0)");
  case GX_TEV_COMP_RGB8_EQ: // alpha: GX_TEV_COMP_A8_EQ
    return sel("round(" + A + " * 255.0) == round(" + B + " * 255.0)");
  default:
    (void)isColor;
    return D;
  }
}

// ---------------------------------------------------------------- alpha compare
struct Cmp {
  std::string expr;
  int constant = -1; // 0/1 when known
};
Cmp cmp_const(bool v) { return {v ? "true" : "false", v ? 1 : 0}; }
Cmp cmp_not(const Cmp& e) { return e.constant != -1 ? cmp_const(e.constant == 0) : Cmp{"!" + e.expr, -1}; }
Cmp cmp_and(const Cmp& l, const Cmp& r) {
  if (l.constant == 0 || r.constant == 0) return cmp_const(false);
  if (l.constant == 1) return r;
  if (r.constant == 1) return l;
  return {"(" + l.expr + " && " + r.expr + ")", -1};
}
Cmp cmp_or(const Cmp& l, const Cmp& r) {
  if (l.constant == 1 || r.constant == 1) return cmp_const(true);
  if (l.constant == 0) return r;
  if (r.constant == 0) return l;
  return {"(" + l.expr + " || " + r.expr + ")", -1};
}
Cmp cmp_xor(const Cmp& l, const Cmp& r) {
  if (l.constant != -1 && r.constant != -1) return cmp_const(l.constant != r.constant);
  if (l.constant == 0) return r;
  if (r.constant == 0) return l;
  if (l.constant == 1) return cmp_not(r);
  if (r.constant == 1) return cmp_not(l);
  return {"(" + l.expr + " != " + r.expr + ")", -1};
}
Cmp cmp_one(GXCompare comp, uint32_t ref) {
  const std::string r = num(static_cast<int>(ref)) + ".0";
  switch (comp) {
  case GX_NEVER:
    return cmp_const(false);
  case GX_LESS:
    return ref == 0 ? cmp_const(false) : Cmp{"(alphaCompare < " + r + ")", -1};
  case GX_LEQUAL:
    return ref == 255 ? cmp_const(true) : Cmp{"(alphaCompare <= " + r + ")", -1};
  case GX_EQUAL:
    return {"(alphaCompare == " + r + ")", -1};
  case GX_NEQUAL:
    return {"(alphaCompare != " + r + ")", -1};
  case GX_GEQUAL:
    return ref == 0 ? cmp_const(true) : Cmp{"(alphaCompare >= " + r + ")", -1};
  case GX_GREATER:
    return ref == 255 ? cmp_const(false) : Cmp{"(alphaCompare > " + r + ")", -1};
  case GX_ALWAYS:
  default:
    return cmp_const(true);
  }
}
Cmp alpha_discard(const ShaderConfig& c) {
  const Cmp a = cmp_one(c.alphaCompare.comp0, c.alphaCompare.ref0 & 0xFF);
  const Cmp b = cmp_one(c.alphaCompare.comp1, c.alphaCompare.ref1 & 0xFF);
  Cmp pass;
  switch (c.alphaCompare.op) {
  case GX_AOP_OR:
    pass = cmp_or(a, b);
    break;
  case GX_AOP_XOR:
    pass = cmp_xor(a, b);
    break;
  case GX_AOP_XNOR:
    pass = cmp_not(cmp_xor(a, b));
    break;
  case GX_AOP_AND:
  default:
    pass = cmp_and(a, b);
    break;
  }
  return cmp_not(pass);
}

// ---------------------------------------------------------------- indirect texturing
// The indirect stages' samples (0..255 per channel, .abg as Aurora reads them) and
// the running t_TexCoord, as in Aurora's gx/shader.cpp.
const char* ind_scale(GXIndTexScale sc) {
  static const char* k[] = {"1.0", "0.5", "0.25", "0.125", "0.0625", "0.03125", "0.015625", "0.0078125", "0.00390625"};
  const int i = static_cast<int>(sc);
  return i >= 0 && i < 9 ? k[i] : "1.0";
}

std::string indirect_code(const ShaderConfig& c, const ShaderInfo& info, uint32_t attrs) {
  std::string fs;
  for (int i = 0; i < static_cast<int>(info.usedIndStages.size()); ++i) {
    if (!info.usedIndStages.test(i) || i >= static_cast<int>(c.numIndStages)) continue;
    const IndStage& is = c.indStages[i];
    const int tc = static_cast<int>(is.texCoordId), map = static_cast<int>(is.texMapId);
    if (tc < 0 || tc > 7 || map < 0 || map > 7 || !(attrs & (1u << (kAttrTc0 + tc)))) {
      fs += fmtf("\n  float3 indc%d = float3(0.0, 0.0, 0.0);", i);
      continue;
    }
    fs += fmtf("\n  float3 indc%d = 255.0 * tex2D(uTex%d, tc%d * uTcS[%d].xy * float2(%s, %s) / uSzB[%d].xy).abg;", i,
               map, tc, tc, ind_scale(is.scaleS), ind_scale(is.scaleT), map);
  }
  fs += "\n  float2 tTexCoord = float2(0.0, 0.0);";
  return fs;
}

std::string wrap_comp(GXIndTexWrap w, const std::string& v) {
  switch (w) {
  case GX_ITW_256: return "fmod(" + v + ", 256.0)";
  case GX_ITW_128: return "fmod(" + v + ", 128.0)";
  case GX_ITW_64: return "fmod(" + v + ", 64.0)";
  case GX_ITW_32: return "fmod(" + v + ", 32.0)";
  case GX_ITW_16: return "fmod(" + v + ", 16.0)";
  case GX_ITW_0: return "0.0";
  default: return v;
  }
}

std::string indirect_stage_code(const ShaderConfig& c, int i, uint32_t attrs) {
  const TevStage& s = c.tevStages[i];
  std::string fs = fmtf("\n  // TEV stage %d indirect", i);
  const bool needsIndirectCoord = s.indTexMtxId != GX_ITM_OFF;
  const bool hasIndirectStage = s.indTexStage < c.numIndStages;
  const int tc = static_cast<int>(s.texCoordId);
  const bool hasBaseCoord = s.texCoordId != GX_TEXCOORD_NULL && s.texMapId != GX_TEXMAP_NULL &&
                            tc >= 0 && tc < 8 && (attrs & (1u << (kAttrTc0 + tc)));
  const int map = static_cast<int>(s.texMapId);
  std::string offset;
  if (needsIndirectCoord && hasIndirectStage) {
    const int is = static_cast<int>(s.indTexStage);
    const char* shift = s.indTexFormat == GX_ITF_5 ? " / 8.0" : s.indTexFormat == GX_ITF_4 ? " / 16.0"
                        : s.indTexFormat == GX_ITF_3 ? " / 32.0" : "";
    if (*shift)
      fs += fmtf("\n  float3 ind%d_coord = floor(indc%d%s);", i, is, shift);
    else
      fs += fmtf("\n  float3 ind%d_coord = indc%d;", i, is);
    if (s.indTexBiasSel != GX_ITB_NONE) {
      const auto b = s.indTexBiasSel;
      const bool bs = b == GX_ITB_S || b == GX_ITB_ST || b == GX_ITB_SU || b == GX_ITB_STU;
      const bool bt = b == GX_ITB_T || b == GX_ITB_ST || b == GX_ITB_TU || b == GX_ITB_STU;
      const bool bu = b == GX_ITB_U || b == GX_ITB_SU || b == GX_ITB_TU || b == GX_ITB_STU;
      fs += fmtf("\n  ind%d_coord = ind%d_coord + float3(%s, %s, %s) * %s;", i, i, bs ? "1.0" : "0.0",
                 bt ? "1.0" : "0.0", bu ? "1.0" : "0.0", s.indTexFormat == GX_ITF_8 ? "-128.0" : "1.0");
    }
    if (s.indTexMtxId >= GX_ITM_0 && s.indTexMtxId <= GX_ITM_2) {
      const int m = s.indTexMtxId - GX_ITM_0;
      offset = fmtf("float2(dot(float3(uIndM[%d].xz, uIndM[%d].x), ind%d_coord), "
                    "dot(float3(uIndM[%d].yw, uIndM[%d].y), ind%d_coord)) * uIndM[%d].z",
                    m * 2, m * 2 + 1, i, m * 2, m * 2 + 1, i, m * 2 + 1);
    } else if (s.indTexMtxId >= GX_ITM_S0 && s.indTexMtxId <= GX_ITM_S2 && hasBaseCoord) {
      const int m = s.indTexMtxId - GX_ITM_S0;
      offset = fmtf("tc%d * uTcS[%d].xy * ind%d_coord.x * uIndM[%d].z / 256.0", tc, tc, i, m * 2 + 1);
    } else if (s.indTexMtxId >= GX_ITM_T0 && s.indTexMtxId <= GX_ITM_T2 && hasBaseCoord) {
      const int m = s.indTexMtxId - GX_ITM_T0;
      offset = fmtf("tc%d * uTcS[%d].xy * ind%d_coord.y * uIndM[%d].z / 256.0", tc, tc, i, m * 2 + 1);
    }
  }
  const bool simple = s.indTexMtxId == GX_ITM_OFF && !s.indTexAddPrev;
  std::string base;
  if (hasBaseCoord) {
    if (simple) {
      base = fmtf("tc%d", tc);
    } else {
      fs += fmtf("\n  float2 ind%d_texel = tc%d * uTcS[%d].xy;", i, tc, tc);
      base = fmtf("ind%d_texel", i);
    }
  }
  std::string wrapped = base;
  if (!base.empty() && (s.indTexWrapS != GX_ITW_OFF || s.indTexWrapT != GX_ITW_OFF))
    wrapped = "float2(" + wrap_comp(s.indTexWrapS, base + ".x") + ", " + wrap_comp(s.indTexWrapT, base + ".y") + ")";
  std::string fin;
  if (!wrapped.empty() && !offset.empty())
    fin = wrapped + " + (" + offset + ")";
  else if (!wrapped.empty())
    fin = wrapped;
  else
    fin = offset;
  if (!fin.empty()) fs += (s.indTexAddPrev ? "\n  tTexCoord += " : "\n  tTexCoord = ") + fin + ";";
  if (s.texMapId != GX_TEXMAP_NULL && map >= 0 && map < 8) {
    if (simple)
      fs += fmtf("\n  float2 ind%d_uv = tTexCoord;", i);
    else
      fs += fmtf("\n  float2 ind%d_uv = tTexCoord / uSzB[%d].xy;", i, map);
  }
  return fs;
}

// ---------------------------------------------------------------- lighting
// One GX channel (ch: 0 COLOR0, 1 COLOR1, 2 ALPHA0, 3 ALPHA1) into `target`.
std::string channel_code(const ShaderConfig& c, int ch, const std::string& target, bool alpha) {
  const ColorChannelConfig& cc = c.colorChannels[ch];
  const int vtxColor = ch & 1;
  const bool hasVtx = c.attrs[GX_VA_CLR0 + vtxColor].attrType != GX_NONE;
  const std::string vtx = hasVtx ? fmtf("aClr%d", vtxColor) : "float4(0.0, 0.0, 0.0, 0.0)";
  const std::string mat = cc.matSrc == GX_SRC_VTX ? vtx : fmtf("uChan[%d]", ch * 2 + 1);
  const char* sw = alpha ? ".a" : ".rgb";
  if (!cc.lightingEnabled) return "\n  " + target + " = (" + mat + ")" + sw + ";";
  const std::string amb = cc.ambSrc == GX_SRC_VTX ? vtx : fmtf("uChan[%d]", ch * 2);

  std::string attn;
  if (cc.attnFn == GX_AF_SPOT) {
    attn = "\n      float cosine = max(0.0, dot(ld, uLights[li + 1].xyz));"
           "\n      float cosAttn = dot(uLights[li + 3].xyz, float3(1.0, cosine, cosine * cosine));"
           "\n      float distAttn = dot(uLights[li + 4].xyz, float3(1.0, dist, dist2));"
           "\n      float attn = max(0.0, cosAttn / distAttn);";
  } else if (cc.attnFn == GX_AF_SPEC) {
    attn = std::string("\n      float a0 = dot(aNrm, ld) >= 0.0 ? max(0.0, dot(aNrm, uLights[li + 1].xyz)) : 0.0;"
                       "\n      float cosAttn = dot(uLights[li + 3].xyz, float3(1.0, a0, a0 * a0));") +
           (cc.diffFn != GX_DF_NONE
                ? "\n      float distAttn = max(0.0, dot(normalize(uLights[li + 4].xyz), float3(1.0, a0, a0 * a0)));"
                : "\n      float distAttn = max(0.0, dot(uLights[li + 4].xyz, float3(1.0, a0, a0 * a0)));") +
           "\n      float attn = max(0.0, cosAttn / distAttn);";
  } else {
    attn = "\n      float attn = 1.0;";
  }
  std::string diff;
  if (cc.diffFn == GX_DF_SIGN)
    diff = "dot(ld, aNrm)";
  else if (cc.diffFn == GX_DF_CLAMP)
    diff = "max(0.0, dot(ld, aNrm))";
  else
    diff = "1.0";

  // Unrolled over the 8 lights: the enable flags are packed four to a vector
  // (uLightEn[8]: channel ch, light i at flat index ch * 8 + i).
  std::string out = fmtf("\n  {\n    float4 lig = %s;", amb.c_str());
  static const char kComp[4] = {'x', 'y', 'z', 'w'};
  for (int i = 0; i < 8; ++i) {
    const int flat = ch * 8 + i;
    std::string blk = attn;
    for (size_t pos; (pos = blk.find("li + ")) != std::string::npos;) {
      const int add = blk[pos + 5] - '0';
      blk.replace(pos, 6, std::to_string(i * 5 + add));
    }
    out += fmtf("\n    if (uLightEn[%d].%c > 0.5) {\n      float3 ld = uLights[%d].xyz - aPos;"
                "\n      float dist2 = dot(ld, ld);\n      float dist = sqrt(dist2);\n      ld = ld / dist;",
                flat / 4, kComp[flat % 4], i * 5) +
           blk + "\n      float diff = " + diff + ";\n      lig = lig + attn * diff * uLights[" +
           std::to_string(i * 5 + 2) + "];\n    }";
  }
  return out + "\n    " + target + " = ((" + mat + ") * saturate(lig))" + sw + ";\n  }";
}
} // namespace

// The GPU-transform vertex shader: raw attributes in, the position/normal matrices
// (uPosM, uNrmM), texture matrices (uTexM, 3 rows per coordinate) and post
// matrices (uPost) applied here. The same maths as gx_draw's CPU path.
static bool gpu_vs(const ShaderConfig& c, const ShaderInfo& info, uint32_t attrs, const bool chanUsed[2], CgSources& out) {
  for (int i = 0; i < 8; ++i) {
    if (!info.sampledTexCoords.test(i)) continue;
    const auto& tcg = c.tcgs[i];
    if (tcg.type >= GX_TG_BUMP0 && tcg.type <= GX_TG_BUMP7) return false;
    if ((tcg.type == GX_TG_MTX2x4 || tcg.type == GX_TG_MTX3x4) && c.attrs[GX_VA_TEX0MTXIDX + i].attrType != GX_NONE)
      return false;
  }
  const bool hasNrmAttr = c.attrs[GX_VA_NRM].attrType != GX_NONE;
  const bool hasPn = c.attrs[GX_VA_PNMTXIDX].attrType != GX_NONE;
  bool needNrm = (attrs & (1u << kAttrNrm)) != 0;
  bool needClr[2] = {(attrs & (1u << kAttrClr0)) != 0, (attrs & (1u << kAttrClr1)) != 0};
  bool needUv[8] = {};
  bool anyTex = false, anyPost = false;
  for (int i = 0; i < 8; ++i) {
    if (!info.sampledTexCoords.test(i)) continue;
    anyTex = true;
    const auto& tcg = c.tcgs[i];
    if (tcg.src >= GX_TG_TEX0 && tcg.src <= GX_TG_TEX7) needUv[tcg.src - GX_TG_TEX0] = true;
    else if (tcg.src == GX_TG_NRM || tcg.src == GX_TG_BINRM || tcg.src == GX_TG_TANGENT) needNrm = true;
    else if (tcg.src == GX_TG_COLOR0) needClr[0] = c.attrs[GX_VA_CLR0].attrType != GX_NONE;
    else if (tcg.src == GX_TG_COLOR1) needClr[1] = c.attrs[GX_VA_CLR1].attrType != GX_NONE;
    if (tcg.postMtx != GX_PTIDENTITY) anyPost = true;
  }
  uint32_t in = 1u << kRawPos;
  if (needNrm && hasNrmAttr) in |= 1u << kRawNrm;
  for (int i = 0; i < 2; ++i)
    if (needClr[i]) in |= 1u << (kRawClr0 + i);
  for (int t = 0; t < 8; ++t)
    if (needUv[t]) in |= 1u << (kRawUv0 + t);
  if (hasPn) in |= 1u << kRawPn;

  const int nMtx = hasPn ? 30 : 3;
  std::string vs = "uniform float4 uProj[4];\n";
  vs += fmtf("uniform float4 uPosM[%d];\n", nMtx);
  if (needNrm) vs += fmtf("uniform float4 uNrmM[%d];\n", nMtx);
  // Sized to the highest texcoord that uses a matrix (3 rows each).
  int texRows = 0, postRows = 0;
  for (int i = 0; i < 8; ++i) {
    if (!info.sampledTexCoords.test(i)) continue;
    const auto& tcg = c.tcgs[i];
    const bool isMtx = tcg.type == GX_TG_MTX2x4 || tcg.type == GX_TG_MTX3x4;
    if (isMtx && tcg.mtx != GX_IDENTITY && tcg.mtx / 3 < 20) texRows = (i + 1) * 3;
    if (tcg.postMtx != GX_PTIDENTITY) postRows = (i + 1) * 3;
  }
  if (texRows) vs += fmtf("uniform float4 uTexM[%d];\n", texRows);
  if (postRows) vs += fmtf("uniform float4 uPost[%d];\n", postRows);
  out.texRows = texRows;
  out.postRows = postRows;
  if (chanUsed[0] || chanUsed[1]) vs += "uniform float4 uChan[8];\n";
  if (info.lightingEnabled) vs += "uniform float4 uLights[40];\nuniform float4 uLightEn[8];\n";
  vs += "void main(float3 rPos";
  if (in & (1u << kRawNrm)) vs += ", float3 rNrm";
  for (int i = 0; i < 2; ++i)
    if (in & (1u << (kRawClr0 + i))) vs += fmtf(", float4 rClr%d", i);
  for (int t = 0; t < 8; ++t)
    if (in & (1u << (kRawUv0 + t))) vs += fmtf(", float2 rUv%d", t);
  if (hasPn) vs += ", float rPn";
  vs += ",\n  float4 out gl_Position : POSITION, float2 out vZw : TEXCOORD8";
  for (int i = 0; i < 2; ++i)
    if (chanUsed[i]) vs += fmtf(", float4 out vCol%d : COLOR%d", i, i);
  for (int i = 0; i < 8; ++i)
    if (attrs & (1u << (kAttrTc0 + i))) vs += fmtf(", float3 out vTc%d : TEXCOORD%d", i, i);
  vs += ")\n{\n";
  if (hasPn)
    vs += "  int b = int(floor(rPn / 3.0 + 0.001));\n  if (b > 9) b = 0;\n  b = b * 3;\n";
  else
    vs += "  int b = 0;\n";
  vs += "  float4 mp = float4(rPos, 1.0);\n"
        "  float3 aPos = float3(dot(uPosM[b], mp), dot(uPosM[b + 1], mp), dot(uPosM[b + 2], mp));\n";
  if (needNrm) {
    vs += (in & (1u << kRawNrm)) ? "  float3 nraw = rNrm;\n" : "  float3 nraw = float3(1.0, 0.0, 0.0);\n";
    vs += "  float4 mn = float4(nraw, 0.0);\n"
          "  float3 aNrm = float3(dot(uNrmM[b], mn), dot(uNrmM[b + 1], mn), dot(uNrmM[b + 2], mn));\n"
          "  float nl2 = dot(aNrm, aNrm);\n"
          "  if (nl2 > 1e-10) aNrm = aNrm * rsqrt(nl2);\n";
  }
  for (int i = 0; i < 2; ++i)
    if (in & (1u << (kRawClr0 + i))) vs += fmtf("  float4 aClr%d = rClr%d;\n", i, i);
  vs += "  float4 p = float4(aPos, 1.0);\n"
        "  float4 clip = float4(dot(uProj[0], p), dot(uProj[1], p), dot(uProj[2], p), dot(uProj[3], p));\n"
        "  vZw = clip.zw;\n"
        "  gl_Position = float4(clip.x, clip.y, 2.0 * clip.z - clip.w, clip.w);";
  for (int i = 0; i < 2; ++i) {
    if (!chanUsed[i]) continue;
    vs += fmtf("\n  float4 col%d = float4(0.0, 0.0, 0.0, 0.0);", i);
    vs += channel_code(c, i, fmtf("col%d.rgb", i), false);
    vs += channel_code(c, i + 2, fmtf("col%d.a", i), true);
    vs += fmtf("\n  vCol%d = col%d;", i, i);
  }
  for (int i = 0; i < 8; ++i) {
    if (!(attrs & (1u << (kAttrTc0 + i)))) continue;
    const auto& tcg = c.tcgs[i];
    std::string src = "float4(0.0, 0.0, 1.0, 1.0)";
    if (tcg.src >= GX_TG_TEX0 && tcg.src <= GX_TG_TEX7)
      src = fmtf("float4(rUv%d, 1.0, 1.0)", tcg.src - GX_TG_TEX0);
    else if (tcg.src == GX_TG_POS)
      src = "float4(rPos, 1.0)";
    else if (tcg.src == GX_TG_NRM || tcg.src == GX_TG_BINRM || tcg.src == GX_TG_TANGENT)
      src = "float4(nraw, 1.0)";
    else if (tcg.src == GX_TG_COLOR0 || tcg.src == GX_TG_COLOR1) {
      const int ci = tcg.src == GX_TG_COLOR1 ? 1 : 0;
      if (in & (1u << (kRawClr0 + ci))) src = fmtf("rClr%d", ci);
      else src = "float4(0.0, 0.0, 0.0, 0.0)";
    }
    const bool isMtx = tcg.type == GX_TG_MTX2x4 || tcg.type == GX_TG_MTX3x4;
    const bool useMtx = isMtx && tcg.mtx != GX_IDENTITY && tcg.mtx / 3 < 20;
    vs += fmtf("\n  {\n    float4 tc = %s;\n", src.c_str());
    if (useMtx)
      vs += fmtf("    float3 t = float3(dot(uTexM[%d], tc), dot(uTexM[%d], tc), dot(uTexM[%d], tc));\n", i * 3,
                 i * 3 + 1, i * 3 + 2);
    else
      vs += "    float3 t = tc.xyz;\n";
    if (tcg.type != GX_TG_MTX3x4) vs += "    t.z = 1.0;\n";
    if (tcg.normalize) vs += "    float tl2 = dot(t, t);\n    if (tl2 > 0.0) t = t * rsqrt(tl2);\n";
    if (tcg.postMtx != GX_PTIDENTITY) {
      vs += fmtf("    float4 t4 = float4(t, 1.0);\n"
                 "    t = float3(dot(uPost[%d], t4), dot(uPost[%d], t4), dot(uPost[%d], t4));\n",
                 i * 3, i * 3 + 1, i * 3 + 2);
    }
    if (tcg.type != GX_TG_MTX3x4) vs += "    t.z = 1.0;\n";
    vs += fmtf("    vTc%d = t;\n  }", i);
  }
  vs += "\n}\n";
  out.vs = std::move(vs);
  out.gpu = true;
  out.gpuPn = hasPn;
  out.gpuIn = in;
  return true;
}

CgSources generate_cg(const ShaderConfig& c, const ShaderInfo& info, bool gpu) {
  CgSources out;
  uint32_t attrs = 1u << kAttrPos;
  if (info.lightingEnabled) attrs |= 1u << kAttrNrm;
  bool chanUsed[2] = {false, false};
  for (int i = 0; i < 2; ++i) {
    if (!info.sampledColorChannels.test(i)) continue;
    chanUsed[i] = true;
    for (int ch : {i, i + 2}) {
      const auto& cc = c.colorChannels[ch];
      const bool vtx = cc.matSrc == GX_SRC_VTX || (cc.lightingEnabled && cc.ambSrc == GX_SRC_VTX);
      if (vtx && c.attrs[GX_VA_CLR0 + i].attrType != GX_NONE) attrs |= 1u << (kAttrClr0 + i);
    }
  }
  for (int i = 0; i < 8; ++i)
    if (info.sampledTexCoords.test(i)) attrs |= 1u << (kAttrTc0 + i);
  out.attrs = attrs;

  // ---------------------------------------------------------------- vertex
  if (!gpu || !gpu_vs(c, info, attrs, chanUsed, out)) {
  std::string vs = "uniform float4 uProj[4];\n";
  const bool anyChan = chanUsed[0] || chanUsed[1];
  if (anyChan) vs += "uniform float4 uChan[8];\n";
  if (info.lightingEnabled) vs += "uniform float4 uLights[40];\nuniform float4 uLightEn[8];\n";
  vs += "void main(float3 aPos";
  if (attrs & (1u << kAttrNrm)) vs += ", float3 aNrm";
  for (int i = 0; i < 2; ++i)
    if (attrs & (1u << (kAttrClr0 + i))) vs += fmtf(", float4 aClr%d", i);
  for (int i = 0; i < 8; ++i)
    if (attrs & (1u << (kAttrTc0 + i))) vs += fmtf(", float3 aTc%d", i);
  vs += ",\n  float4 out gl_Position : POSITION, float2 out vZw : TEXCOORD8";
  for (int i = 0; i < 2; ++i)
    if (chanUsed[i]) vs += fmtf(", float4 out vCol%d : COLOR%d", i, i);
  for (int i = 0; i < 8; ++i)
    if (attrs & (1u << (kAttrTc0 + i))) vs += fmtf(", float3 out vTc%d : TEXCOORD%d", i, i);
  vs += ")\n{\n  float4 p = float4(aPos, 1.0);\n"
        "  float4 clip = float4(dot(uProj[0], p), dot(uProj[1], p), dot(uProj[2], p), dot(uProj[3], p));\n"
        "  vZw = clip.zw;\n"
        "  gl_Position = float4(clip.x, clip.y, 2.0 * clip.z - clip.w, clip.w);";
  for (int i = 0; i < 2; ++i) {
    if (!chanUsed[i]) continue;
    vs += fmtf("\n  float4 col%d = float4(0.0, 0.0, 0.0, 0.0);", i);
    vs += channel_code(c, i, fmtf("col%d.rgb", i), false);
    vs += channel_code(c, i + 2, fmtf("col%d.a", i), true);
    vs += fmtf("\n  vCol%d = col%d;", i, i);
  }
  for (int i = 0; i < 8; ++i)
    if (attrs & (1u << (kAttrTc0 + i))) vs += fmtf("\n  vTc%d = aTc%d;", i, i);
  vs += "\n}\n";
  out.vs = std::move(vs);
  }

  // ---------------------------------------------------------------- fragment
  std::string fs;
  for (int i = 0; i < 8; ++i)
    if (info.sampledTextures.test(i)) fs += fmtf("uniform sampler2D uTex%d;\n", i);
  fs += "uniform float4 uTev[4];\nuniform float4 uKc[4];\n";
  if (info.usesFog) fs += "uniform float4 uFog[2];\n";
  // Indirect texturing (Aurora's gx/shader.cpp): texcoord scales, the indirect
  // matrices (2 vec4 each) and the texture sizes, by map.
  bool anyIndirect = false;
  for (int i = 0; i < static_cast<int>(c.tevStageCount); ++i) {
    const TevStage& s = c.tevStages[i];
    if (s.indTexMtxId != GX_ITM_OFF || s.indTexWrapS != GX_ITW_OFF || s.indTexWrapT != GX_ITW_OFF || s.indTexAddPrev)
      anyIndirect = true;
  }
  if (anyIndirect) {
    fs += "uniform float4 uTcS[8];\nuniform float4 uSzB[8];\n";
    if (info.usedIndTexMtxs.any()) fs += "uniform float4 uIndM[6];\n";
  }
  fs += "float3 f3(float x) { return float3(x, x, x); }\n"
        "float ovf1(float x) { float b = x * 255.0; return (b - floor(b / 256.0) * 256.0) / 255.0; }\n"
        "float3 ovf3(float3 x) { float3 b = x * 255.0; return (b - floor(b / 256.0) * 256.0) / 255.0; }\n"
        "float4 main(float2 vZw : TEXCOORD8";
  for (int i = 0; i < 2; ++i)
    if (chanUsed[i]) fs += fmtf(", float4 vCol%d : COLOR%d", i, i);
  for (int i = 0; i < 8; ++i)
    if (attrs & (1u << (kAttrTc0 + i))) fs += fmtf(", float3 vTc%d : TEXCOORD%d", i, i);
  fs += ") : COLOR\n{";
  for (int i = 0; i < 2; ++i)
    fs += chanUsed[i] ? fmtf("\n  float4 rast%d = vCol%d;", i, i) : fmtf("\n  float4 rast%d = float4(0.0, 0.0, 0.0, 0.0);", i);
  for (int i = 0; i < 8; ++i) {
    if (!(attrs & (1u << (kAttrTc0 + i)))) continue;
    if (c.tcgs[i].type == GX_TG_MTX3x4)
      fs += fmtf("\n  float2 tc%d = vTc%d.xy / vTc%d.z;", i, i, i);
    else
      fs += fmtf("\n  float2 tc%d = vTc%d.xy;", i, i);
  }
  if (anyIndirect) fs += indirect_code(c, info, attrs);
  for (int i = 0; i < static_cast<int>(c.tevStageCount); ++i) {
    const TevStage& s = c.tevStages[i];
    const bool needsTevTexCoord =
        s.indTexMtxId != GX_ITM_OFF || s.indTexWrapS != GX_ITW_OFF || s.indTexWrapT != GX_ITW_OFF || s.indTexAddPrev;
    if (needsTevTexCoord && anyIndirect) fs += indirect_stage_code(c, i, attrs);
    if (!uses_texture_sample(s) || s.texCoordId == GX_TEXCOORD_NULL) continue;
    if (!(attrs & (1u << (kAttrTc0 + s.texCoordId)))) {
      fs += fmtf("\n  float4 sampled%d = float4(1.0, 1.0, 1.0, 1.0);", i);
      continue;
    }
    if (needsTevTexCoord && anyIndirect && s.texMapId != GX_TEXMAP_NULL) {
      fs += fmtf("\n  float4 sampled%d = tex2D(uTex%d, ind%d_uv);", i, static_cast<int>(s.texMapId), i);
      continue;
    }
    fs += fmtf("\n  float4 sampled%d = tex2D(uTex%d, tc%d);", i, static_cast<int>(s.texMapId),
               static_cast<int>(s.texCoordId));
  }
  fs += info.loadsTevReg.test(0) ? "\n  float4 prev = uTev[0];" : "\n  float4 prev = float4(0.0, 0.0, 0.0, 0.0);";
  for (int r = 1; r < 4; ++r)
    fs += info.loadsTevReg.test(r) ? fmtf("\n  float4 tevreg%d = uTev[%d];", r - 1, r)
                                   : fmtf("\n  float4 tevreg%d = float4(0.0, 0.0, 0.0, 0.0);", r - 1);

  static const char* kReg[4] = {"prev", "tevreg0", "tevreg1", "tevreg2"};
  bool cn[4] = {false, false, false, false}, an[4] = {false, false, false, false};
  for (int i = 0; i < static_cast<int>(c.tevStageCount); ++i) {
    const TevStage& s = c.tevStages[i];
    {
      auto arg = [&](GXTevColorArg a) {
        std::string v = color_arg(a, i, c, s);
        return color_arg_normalized(a, cn, an) ? v : "ovf3(" + v + ")";
      };
      std::string op = tev_op(s.colorOp.op, s.colorOp.bias, s.colorOp.scale, arg(s.colorPass.a), arg(s.colorPass.b),
                              arg(s.colorPass.c), color_arg(s.colorPass.d, i, c, s), "f3(0.0)", true);
      op = s.colorOp.clamp ? "saturate(" + op + ")" : "clamp(" + op + ", f3(-4.0), f3(4.0))";
      fs += fmtf("\n  // TEV stage %d", i) + "\n  " + kReg[s.colorOp.outReg] + ".rgb = " + op + ";";
      cn[s.colorOp.outReg] = s.colorOp.clamp;
    }
    {
      auto arg = [&](GXTevAlphaArg a) {
        std::string v = alpha_arg(a, i, c, s);
        return alpha_arg_normalized(a, an) ? v : "ovf1(" + v + ")";
      };
      std::string op = tev_op(s.alphaOp.op, s.alphaOp.bias, s.alphaOp.scale, arg(s.alphaPass.a), arg(s.alphaPass.b),
                              arg(s.alphaPass.c), alpha_arg(s.alphaPass.d, i, c, s), "0.0", false);
      op = s.alphaOp.clamp ? "saturate(" + op + ")" : "clamp(" + op + ", -4.0, 4.0)";
      fs += "\n  " + std::string(kReg[s.alphaOp.outReg]) + ".a = " + op + ";";
      an[s.alphaOp.outReg] = s.alphaOp.clamp;
    }
  }
  bool prevColorNorm = true, prevAlphaNorm = true;
  if (c.tevStageCount > 0) {
    const TevStage& last = c.tevStages[c.tevStageCount - 1];
    prevColorNorm = cn[last.colorOp.outReg];
    prevAlphaNorm = an[last.alphaOp.outReg];
    if (last.colorOp.outReg != 0) fs += std::string("\n  prev.rgb = ") + kReg[last.colorOp.outReg] + ".rgb;";
    if (last.alphaOp.outReg != 0) fs += std::string("\n  prev.a = ") + kReg[last.alphaOp.outReg] + ".a;";
  }

  if (info.usesFog) {
    fs += "\n  // Fog (reversed depth: 1 - window z)\n  float fogDepth = 1.0 - vZw.x / vZw.y;";
    if ((c.fogType & 0x08) != 0)
      fs += "\n  float fogBase = uFog[1].x * fogDepth;";
    else
      fs += "\n  float fogBase = uFog[1].x / (uFog[1].y - fogDepth);";
    fs += "\n  float fogF = saturate(fogBase - uFog[1].z);";
    switch (c.fogType) {
    case GX_FOG_PERSP_EXP:
    case GX_FOG_ORTHO_EXP:
      fs += "\n  float fogZ = 1.0 - exp2(-8.0 * fogF);";
      break;
    case GX_FOG_PERSP_EXP2:
    case GX_FOG_ORTHO_EXP2:
      fs += "\n  float fogZ = 1.0 - exp2(-8.0 * fogF * fogF);";
      break;
    case GX_FOG_PERSP_REVEXP:
    case GX_FOG_ORTHO_REVEXP:
      fs += "\n  float fogZ = exp2(-8.0 * (1.0 - fogF));";
      break;
    case GX_FOG_PERSP_REVEXP2:
    case GX_FOG_ORTHO_REVEXP2:
      fs += "\n  fogF = 1.0 - fogF;\n  float fogZ = exp2(-8.0 * fogF * fogF);";
      break;
    default:
      fs += "\n  float fogZ = fogF;";
      break;
    }
    fs += "\n  prev.rgb = lerp(prev.rgb, uFog[0].rgb, saturate(fogZ));";
  }
  if (!prevColorNorm && !prevAlphaNorm)
    fs += "\n  prev = float4(ovf3(prev.rgb), ovf1(prev.a));";
  else if (!prevColorNorm)
    fs += "\n  prev.rgb = ovf3(prev.rgb);";
  else if (!prevAlphaNorm)
    fs += "\n  prev.a = ovf1(prev.a);";

  if (c.alphaCompare) {
    const Cmp d = alpha_discard(c);
    if (d.constant == 1) {
      fs += "\n  discard;";
    } else if (d.constant != 0) {
      fs += "\n  float alphaCompare = round(saturate(prev.a) * 255.0);";
      fs += "\n  if (" + d.expr + ") discard;";
    }
  }
  fs += "\n  return prev;\n}\n";
  out.fs = std::move(fs);
  return out;
}
} // namespace wv
