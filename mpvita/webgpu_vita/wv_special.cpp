// webgpu_vita: Aurora's own (non-GameCube) pipelines, recognised by label and
// drawn with small hand-written Cg programs. Everything here is a full-screen
// triangle: Aurora draws these with Draw(3) and positions from vertex_index.
//   XFB Copy / Premultiplied Copy  EFB -> screen
//   Present Resample               EFB -> screen-sized texture (plain bilinear here)
//   EFB Clear                      writes the blend constant
//   TexCopyConv <format>           EFB copies converted to a GameCube texture format
//                                  (gfx/tex_copy_conv.cpp), with its uv transform
//   TexPaletteConv FromFloat8/4    an EFB copy's values looked up in a palette
//                                  (gfx/tex_palette_conv.cpp)
#include "wv.hpp"
#include "mp_zone.h"

#include <vitaGL.h>

#include <cstring>
#include <string>

namespace wv {

enum class SpecialKind {
  Copy,       // texture -> target; `opaque` forces alpha to 1
  Resample,   // as Copy (Aurora's filter options are not reproduced)
  Clear,      // EFB Clear Pipeline: the blend constant
  CopyConv,   // TexCopyConv: body converts the sample; uv transform in binding 2
  Palette,    // TexPaletteConv: index from the red channel, colour from the TLUT (binding 2)
};

struct Program {
  GLuint prog = 0;
  GLint uTex = -1, uTlut = -1, uTlutW = -1, uOpaque = -1, uColor = -1, uXf = -1;
  bool tried = false;
};

struct SpecialPipeline {
  SpecialKind kind;
  uint32_t textureBinding;
  uint32_t samplerBinding;
  bool opaque = false;
  std::string body; // Cg: from float4 s (the sample) return the output
  Program prog;
};

namespace {
constexpr const char* kVs = R"(
uniform float4 uXf;
void main(float2 aPos, float2 aUv, float4 out gl_Position : POSITION, float2 out vUv : TEXCOORD0) {
  gl_Position = float4(aPos, 0.0, 1.0);
  vUv = aUv * uXf.zw + uXf.xy;
}
)";

constexpr const char* kFsClear = R"(
float4 main(float2 vUv : TEXCOORD0, uniform float4 uColor) : COLOR {
  return uColor;
}
)";

std::string fs_for(const SpecialPipeline& sp) {
  std::string fs = "uniform sampler2D uTex;\n";
  if (sp.kind == SpecialKind::Palette) fs += "uniform sampler2D uTlut;\nuniform float uTlutW;\n";
  fs += "float intensity(float3 c) { return dot(c, float3(0.257, 0.504, 0.098)) + 16.0 / 255.0; }\n"
        "float q4(float v) { return floor(v * 16.0) / 15.0; }\n"
        "float4 main(float2 vUv : TEXCOORD0) : COLOR {\n  float4 s = tex2D(uTex, vUv);\n  ";
  fs += sp.body;
  fs += "\n}\n";
  return fs;
}

GLuint compile(GLenum type, const char* src, const char* what) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char buf[512] = {};
    glGetShaderInfoLog(s, sizeof buf, nullptr, buf);
    log("shader %s failed to compile: %s", what, buf);
    return 0;
  }
  return s;
}

bool build(SpecialPipeline& sp, const char* what) {
  Program& p = sp.prog;
  if (p.tried) return p.prog != 0;
  p.tried = true;
  const std::string fs = sp.kind == SpecialKind::Clear ? std::string(kFsClear) : fs_for(sp);
  GLuint vs = compile(GL_CG_VERTEX_SHADER_EXT, kVs, "fullscreen vs");
  GLuint f = compile(GL_CG_FRAGMENT_SHADER_EXT, fs.c_str(), what);
  if (!vs || !f) return false;
  p.prog = glCreateProgram();
  glAttachShader(p.prog, vs);
  glAttachShader(p.prog, f);
  glBindAttribLocation(p.prog, 0, "aPos");
  glBindAttribLocation(p.prog, 1, "aUv");
  glLinkProgram(p.prog);
  p.uTex = glGetUniformLocation(p.prog, "uTex");
  p.uTlut = glGetUniformLocation(p.prog, "uTlut");
  p.uTlutW = glGetUniformLocation(p.prog, "uTlutW");
  p.uOpaque = glGetUniformLocation(p.prog, "uOpaque");
  p.uColor = glGetUniformLocation(p.prog, "uColor");
  p.uXf = glGetUniformLocation(p.prog, "uXf");
  log("built %s program", what);
  return true;
}

// The full-screen triangle WebGPU's vertex_index tables describe.
const float kPos[6] = {-1.f, 1.f, -1.f, -3.f, 3.f, 1.f};
const float kUv[6] = {0.f, 0.f, 0.f, 2.f, 2.f, 0.f};

// TexCopyConv formats: the conversion of the sample s (gfx/tex_copy_conv.cpp).
struct ConvBody {
  const char* suffix;
  const char* body;
};
const ConvBody kConv[] = {
    {"Blit", "return s;"},
    {"I4", "float i = q4(intensity(s.rgb)); return float4(i, i, i, i);"},
    {"I8", "float i = intensity(s.rgb); return float4(i, i, i, i);"},
    {"IA4", "float i = q4(intensity(s.rgb)); return float4(i, i, i, q4(s.a));"},
    {"IA8", "float i = intensity(s.rgb); return float4(i, i, i, s.a);"},
    {"RGB565", "return float4(s.rgb, 1.0);"},
    {"R4", "float r = q4(s.r); return float4(r, r, r, r);"},
    {"RA4", "float r = q4(s.r); return float4(r, r, r, q4(s.a));"},
    {"RA8", "return float4(s.r, s.r, s.r, s.a);"},
    {"A8", "return float4(s.a, s.a, s.a, s.a);"},
    {"R8", "return float4(s.r, s.r, s.r, s.r);"},
    {"G8", "return float4(s.g, s.g, s.g, s.g);"},
    {"B8", "return float4(s.b, s.b, s.b, s.b);"},
    {"RG8", "return float4(s.r, s.r, s.r, s.g);"},
    {"GB8", "return float4(s.g, s.g, s.g, s.b);"},
};
} // namespace

SpecialPipeline* special_for(const RenderPipeline& p) {
  const std::string& l = p.label;
  auto make = [](SpecialKind k, uint32_t tex, uint32_t samp) {
    auto* sp = new SpecialPipeline;
    sp->kind = k;
    sp->textureBinding = tex;
    sp->samplerBinding = samp;
    return sp;
  };
  if (l == "XFB Copy Pipeline" || l == "XFB Premultiplied Alpha Copy Pipeline") {
    auto* sp = make(SpecialKind::Copy, 1, 0);
    sp->opaque = l == "XFB Copy Pipeline";
    sp->body = sp->opaque ? "return float4(s.rgb, 1.0);" : "return s;";
    return sp;
  }
  if (p.moduleLabel == "Present Resample Module") {
    auto* sp = make(SpecialKind::Resample, 2, 1);
    sp->body = "return s;";
    return sp;
  }
  if (l.rfind("EFB Clear Pipeline", 0) == 0) return make(SpecialKind::Clear, 0, 0);
  static const std::string kConvPrefix = "TexCopyConv ";
  if (l.rfind(kConvPrefix, 0) == 0) {
    const std::string fmt = l.substr(kConvPrefix.size());
    for (const auto& c : kConv) {
      if (fmt == c.suffix) {
        auto* sp = make(SpecialKind::CopyConv, 1, 0);
        sp->body = c.body;
        return sp;
      }
    }
    return nullptr; // depth formats, HDR/linear domains: not on the Vita
  }
  if (l == "TexPaletteConv FromFloat8" || l == "TexPaletteConv FromFloat4") {
    auto* sp = make(SpecialKind::Palette, 1, 0);
    sp->body = l.back() == '8' ? "float idx = floor(s.r * 255.0 + 0.5);"
                                 "\n  return tex2D(uTlut, float2((idx + 0.5) / uTlutW, 0.5));"
                               : "float idx = floor(s.r * 15.0 + 0.5);"
                                 "\n  return tex2D(uTlut, float2((idx + 0.5) / uTlutW, 0.5));";
    return sp;
  }
  return nullptr;
}

void special_draw(DrawState& st, uint32_t vertexCount, uint32_t firstVertex) {
  MP_ZONE("wv::special_draw");
  RenderPipeline* p = st.pipeline;
  SpecialPipeline* sp = p->special;
  if (vertexCount != 3 || firstVertex != 0) return;
  if (!build(*sp, p->label.c_str())) return;
  Program& prog = sp->prog;
  glUseProgram(prog.prog);
  apply_pipeline_state(*p, st);
  float xf[4] = {0.f, 0.f, 1.f, 1.f};
  if (sp->kind == SpecialKind::Clear) {
    glDisable(GL_BLEND);
    glUniform4fv(prog.uColor, 1, st.blendConstant);
  } else {
    BindGroup* g = st.groups[0];
    if (!g) return;
    const BindEntry* te = g->find(sp->textureBinding);
    const BindEntry* se = g->find(sp->samplerBinding);
    const uint32_t tex = te ? gl_texture_for(te->view.get()) : 0;
    if (!tex) return;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    apply_sampler(se ? se->sampler.get() : nullptr, false);
    glUniform1i(prog.uTex, 0);
    if (sp->kind == SpecialKind::CopyConv) {
      // Binding 2: the uv transform (offset, scale).
      const BindEntry* ue = g->find(2);
      if (ue && ue->buffer && ue->buffer->data && ue->offset + 16 <= ue->buffer->size)
        std::memcpy(xf, ue->buffer->data + ue->offset, 16);
    } else if (sp->kind == SpecialKind::Palette) {
      const BindEntry* le = g->find(2);
      const uint32_t tlut = le ? gl_texture_for(le->view.get()) : 0;
      if (!tlut) return;
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, tlut);
      apply_sampler(nullptr, false); // nearest
      glActiveTexture(GL_TEXTURE0);
      glUniform1i(prog.uTlut, 1);
      glUniform1f(prog.uTlutW, static_cast<float>(le->view->tex->width));
    }
  }
  if (prog.uXf >= 0) glUniform4fv(prog.uXf, 1, xf);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, kPos);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, kUv);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glDisableVertexAttribArray(0);
  glDisableVertexAttribArray(1);
}
} // namespace wv
