// webgpu_vita: plays recorded command buffers back through vitaGL.
// Runs on whichever thread calls wgpuQueueSubmit / wgpuSurfacePresent (Aurora's
// render worker); vitaGL is initialised on the first submit, on that thread.
//
// Conventions: vitaGL is built with HAVE_UNFLIPPED_FBOS, so render targets and the
// display both keep row 0 at the top, like WebGPU. WebGPU viewports/scissors are
// top-left based and are converted to GL's bottom-left ones here. Shaders write
// WebGPU clip-space depth (0..1); they convert it to GL's -1..1 themselves.
#include "wv.hpp"
#include "mp_zone.h"
#include "wv_gx.hpp"

#include <vitaGL.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>

#include <cstdio>
#include <map>
#include <mutex>

namespace wgnull {
void on_present(); // vita_main.cpp: frame counter
}

namespace wv {
namespace {
bool g_glInit = false;

std::mutex g_deleteMutex;
std::vector<uint32_t> g_deleteTextures;

struct Upload {
  Ref<Texture> tex;
  uint32_t mip, x, y, layer, w, h;
  std::vector<uint8_t> data; // tightly packed rows
};
std::mutex g_uploadMutex;
std::vector<Upload> g_uploads;

std::map<std::pair<uint32_t, uint32_t>, GLuint> g_fbos; // (color tex, depth tex) -> FBO
uint64_t g_frame = 0;
uint32_t g_drawsThisFrame = 0, g_skippedThisFrame = 0;

struct GlFormat {
  GLenum internal, format, type;
  uint32_t bpp; // bytes per pixel (0 = unsupported)
  bool depth;
};

GlFormat gl_format(WGPUTextureFormat f) {
  switch (f) {
  case WGPUTextureFormat_RGBA8Unorm:
  case WGPUTextureFormat_RGBA8UnormSrgb:
  case WGPUTextureFormat_BGRA8Unorm:
  case WGPUTextureFormat_BGRA8UnormSrgb:
    return {GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, 4, false};
  case WGPUTextureFormat_R8Unorm:
    return {GL_RED, GL_RED, GL_UNSIGNED_BYTE, 1, false};
  case WGPUTextureFormat_RG8Unorm:
    return {GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 2, false};
  case WGPUTextureFormat_RGBA16Float:
    return {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 8, false};
  case WGPUTextureFormat_Depth16Unorm:
  case WGPUTextureFormat_Depth24Plus:
  case WGPUTextureFormat_Depth24PlusStencil8:
  case WGPUTextureFormat_Depth32Float:
  case WGPUTextureFormat_Depth32FloatStencil8:
    return {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, 4, true};
  default:
    return {0, 0, 0, 0, false};
  }
}

void gl_init() {
  if (g_glInit) return;
  // Keep some of each memory pool for the rest of the port: 8 MB of video memory for
  // the hardware movie decoder (vita_thp_async.cpp), RAM for the runtime shader compiler.
  vglSetupShaderPatcher(4 * 1024 * 1024, 2 * 1024 * 1024, 2 * 1024 * 1024);
  // 8 MB of video memory and 7 MB of physically contiguous RAM stay free for the
  // hardware movie decoder (vita_thp_async.cpp).
  if (vglInitWithCustomThreshold(0x100000, 960, 544, 24 * 1024 * 1024, 8 * 1024 * 1024, 7 * 1024 * 1024,
                                 0x8C6000 /* SCE_KERNEL_MAX_MAIN_CDIALOG_MEM_SIZE */, SCE_GXM_MULTISAMPLE_NONE)) {
    // vitaGL returns true only when it had to fall back to a smaller screen size.
    log("vitaGL fell back to a smaller screen size");
  }
  glClearColor(0, 0, 0, 1);
  for (int i = 0; i < 2; ++i) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    vglSwapBuffers(GL_FALSE);
  }
  g_glInit = true;
  log("vitaGL initialised");
}

// Allocates the GL storage of a texture (level 0; one GL texture per array layer).
void ensure_texture(Texture* t) {
  if (t->glAllocated || t->surface) return;
  t->glAllocated = true;
  const GlFormat fmt = gl_format(t->format);
  if (fmt.bpp == 0) {
    log("texture '%s': format %d not supported", t->label.c_str(), static_cast<int>(t->format));
    return;
  }
  const uint32_t layers = t->dimension == WGPUTextureDimension_3D ? 1 : t->layers;
  t->gl.resize(layers);
  glGenTextures(static_cast<GLsizei>(layers), t->gl.data());
  for (uint32_t i = 0; i < layers; ++i) {
    glBindTexture(GL_TEXTURE_2D, t->gl[i]);
    glTexImage2D(GL_TEXTURE_2D, 0, fmt.internal, t->width, t->height, 0, fmt.format, fmt.type, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
}

void apply_upload(Upload& u) {
  Texture* t = u.tex.get();
  ensure_texture(t);
  if (u.mip != 0) return; // only the base level is kept for now
  if (t->gl.empty() || u.layer >= t->gl.size()) return;
  const GlFormat fmt = gl_format(t->format);
  glBindTexture(GL_TEXTURE_2D, t->gl[u.layer]);
  glTexSubImage2D(GL_TEXTURE_2D, 0, u.x, u.y, u.w, u.h, fmt.format, fmt.type, u.data.data());
}

void flush_uploads() {
  std::vector<Upload> uploads;
  {
    std::lock_guard lk(g_uploadMutex);
    uploads.swap(g_uploads);
  }
  for (auto& u : uploads) apply_upload(u);
  std::vector<uint32_t> del;
  {
    std::lock_guard lk(g_deleteMutex);
    del.swap(g_deleteTextures);
  }
  if (!del.empty()) {
    for (auto it = g_fbos.begin(); it != g_fbos.end();) {
      bool gone = false;
      for (uint32_t n : del) gone |= it->first.first == n || it->first.second == n;
      if (gone) {
        glDeleteFramebuffers(1, &it->second);
        it = g_fbos.erase(it);
      } else {
        ++it;
      }
    }
    glDeleteTextures(static_cast<GLsizei>(del.size()), del.data());
  }
}

GLuint fbo_for(uint32_t color, uint32_t depth) {
  auto key = std::make_pair(color, depth);
  auto it = g_fbos.find(key);
  if (it != g_fbos.end()) return it->second;
  GLuint fbo = 0;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  if (color) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
  if (depth) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
  g_fbos.emplace(key, fbo);
  return fbo;
}

GLenum gl_compare(WGPUCompareFunction f) {
  switch (f) {
  case WGPUCompareFunction_Never: return GL_NEVER;
  case WGPUCompareFunction_Less: return GL_LESS;
  case WGPUCompareFunction_Equal: return GL_EQUAL;
  case WGPUCompareFunction_LessEqual: return GL_LEQUAL;
  case WGPUCompareFunction_Greater: return GL_GREATER;
  case WGPUCompareFunction_NotEqual: return GL_NOTEQUAL;
  case WGPUCompareFunction_GreaterEqual: return GL_GEQUAL;
  default: return GL_ALWAYS;
  }
}

GLenum gl_blend_factor(WGPUBlendFactor f) {
  switch (f) {
  case WGPUBlendFactor_Zero: return GL_ZERO;
  case WGPUBlendFactor_One: return GL_ONE;
  case WGPUBlendFactor_Src: return GL_SRC_COLOR;
  case WGPUBlendFactor_OneMinusSrc: return GL_ONE_MINUS_SRC_COLOR;
  case WGPUBlendFactor_SrcAlpha: return GL_SRC_ALPHA;
  case WGPUBlendFactor_OneMinusSrcAlpha: return GL_ONE_MINUS_SRC_ALPHA;
  case WGPUBlendFactor_Dst: return GL_DST_COLOR;
  case WGPUBlendFactor_OneMinusDst: return GL_ONE_MINUS_DST_COLOR;
  case WGPUBlendFactor_DstAlpha: return GL_DST_ALPHA;
  case WGPUBlendFactor_OneMinusDstAlpha: return GL_ONE_MINUS_DST_ALPHA;
  case WGPUBlendFactor_SrcAlphaSaturated: return GL_SRC_ALPHA_SATURATE;
  // vitaGL has no constant-colour blending; the one user (EFB Clear) passes the colour as a uniform.
  default: return GL_ONE;
  }
}

GLenum gl_blend_op(WGPUBlendOperation o) {
  switch (o) {
  case WGPUBlendOperation_Subtract: return GL_FUNC_SUBTRACT;
  case WGPUBlendOperation_ReverseSubtract: return GL_FUNC_REVERSE_SUBTRACT;
  case WGPUBlendOperation_Min: return GL_MIN;
  case WGPUBlendOperation_Max: return GL_MAX;
  default: return GL_FUNC_ADD;
  }
}

// Pass-level state.
DrawState g_st;
bool g_inPass = false;
bool g_passDrawable = false;

bool g_trace = false; // one frame of pass/viewport tracing (frame 600)

void set_viewport(float x, float y, float w, float h, float mn, float mx) {
  if (g_trace) log("trace:   viewport %.1f,%.1f %.1fx%.1f", x, y, w, h);
  const float H = static_cast<float>(g_st.targetHeight);
  glViewport(static_cast<GLint>(x), static_cast<GLint>(H - y - h), static_cast<GLsizei>(w), static_cast<GLsizei>(h));
  glDepthRangef(mn, mx);
}

void set_scissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  if (g_trace) log("trace:   scissor %u,%u %ux%u", x, y, w, h);
  const int H = static_cast<int>(g_st.targetHeight);
  glEnable(GL_SCISSOR_TEST);
  glScissor(static_cast<GLint>(x), H - static_cast<GLint>(y) - static_cast<GLint>(h), static_cast<GLsizei>(w),
            static_cast<GLsizei>(h));
}

void begin_pass(Cmd& c) {
  g_inPass = true;
  g_passDrawable = false;
  g_st.pipeline = nullptr;
  for (auto& g : g_st.groups) g = nullptr;
  Texture* color = c.color ? c.color->view->tex.get() : nullptr;
  Texture* depth = c.depth ? c.depth->view->tex.get() : nullptr;
  if (!color) {
    // Depth-only passes (the sun's shadow map) aren't supported yet.
    return;
  }
  if (g_trace)
    log("trace: pass '%s' -> '%s' %ux%u%s%s", c.color->view->label.c_str(), color->label.c_str(), color->width,
        color->height, color->surface ? " (screen)" : "", depth ? " +depth" : "");
  g_st.targetWidth = color->width;
  g_st.targetHeight = color->height;
  g_st.targetFormat = color->format;
  g_st.targetIsSurface = color->surface;
  g_st.targetHasDepth = depth != nullptr;
  if (color->surface) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  } else {
    ensure_texture(color);
    if (depth) ensure_texture(depth);
    const uint32_t layer = c.color->view->baseLayer;
    const uint32_t cn = layer < color->gl.size() ? color->gl[layer] : 0;
    const uint32_t dn = depth && !depth->gl.empty() ? depth->gl[0] : 0;
    if (!cn) return;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_for(cn, dn));
  }
  g_passDrawable = true;
  glDisable(GL_SCISSOR_TEST);
  set_viewport(0, 0, static_cast<float>(g_st.targetWidth), static_cast<float>(g_st.targetHeight), 0.f, 1.f);
  GLbitfield clear = 0;
  if (c.color->load == WGPULoadOp_Clear) {
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(c.color->clear[0], c.color->clear[1], c.color->clear[2], c.color->clear[3]);
    clear |= GL_COLOR_BUFFER_BIT;
  }
  if (c.depth && c.depth->depthLoad == WGPULoadOp_Clear) {
    glDepthMask(GL_TRUE);
    glClearDepthf(c.depth->depthClear);
    clear |= GL_DEPTH_BUFFER_BIT;
  }
  if (clear) glClear(clear);
}

void draw(Cmd& c, bool indexed) {
  ++g_drawsThisFrame;
  if (!g_passDrawable || !g_st.pipeline) {
    ++g_skippedThisFrame;
    return;
  }
  RenderPipeline* p = g_st.pipeline;
  if (g_trace) {
    static RenderPipeline* last = nullptr;
    if (p != last) log("trace:   draw with '%s'%s", p->label.c_str(), p->gx ? " (GX)" : p->special ? " (special)" : "");
    last = p;
    if (p->special && g_st.groups[0])
      for (const auto& e : g_st.groups[0]->entries)
        if (e.view && e.view->tex)
          log("trace:     reads '%s' %ux%u", e.view->tex->label.c_str(), e.view->tex->width, e.view->tex->height);
  }
  if (p->gx) {
    if (indexed)
      gx_draw(g_st, 0, c.u[1], 0, true, c.u[0], c.u[2], static_cast<int32_t>(c.u[3]));
    else
      gx_draw(g_st, c.u[0], c.u[1], c.u[2], false, 0, 0, 0);
  } else if (p->special && !indexed) {
    special_draw(g_st, c.u[0], c.u[2]);
  } else {
    ++g_skippedThisFrame;
  }
}

// A rectangle of one texture drawn into another (no blending, depth or scissor).
GLuint g_blitProg = 0;
GLint g_blitTex = -1;
bool g_blitTried = false;
bool gpu_copy(GLuint srcTex, uint32_t sw, uint32_t sh, uint32_t sx, uint32_t sy, GLuint dstTex, uint32_t dh,
              uint32_t dx, uint32_t dy, uint32_t w, uint32_t h) {
  if (!g_blitTried) {
    g_blitTried = true;
    static const char* vsSrc =
        "void main(float2 aPos, float2 aUv, float4 out gl_Position : POSITION, float2 out vUv : TEXCOORD0) {\n"
        "  gl_Position = float4(aPos, 0.0, 1.0);\n  vUv = aUv;\n}\n";
    static const char* fsSrc =
        "uniform sampler2D uTex;\nfloat4 main(float2 vUv : TEXCOORD0) : COLOR { return tex2D(uTex, vUv); }\n";
    GLuint vs = glCreateShader(GL_CG_VERTEX_SHADER_EXT), fs = glCreateShader(GL_CG_FRAGMENT_SHADER_EXT);
    glShaderSource(vs, 1, &vsSrc, nullptr);
    glShaderSource(fs, 1, &fsSrc, nullptr);
    glCompileShader(vs);
    glCompileShader(fs);
    GLint okv = 0, okf = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &okv);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &okf);
    if (okv && okf) {
      g_blitProg = glCreateProgram();
      glAttachShader(g_blitProg, vs);
      glAttachShader(g_blitProg, fs);
      glBindAttribLocation(g_blitProg, 0, "aPos");
      glBindAttribLocation(g_blitProg, 1, "aUv");
      glLinkProgram(g_blitProg);
      g_blitTex = glGetUniformLocation(g_blitProg, "uTex");
      glUseProgram(g_blitProg);
      glUniform1i(g_blitTex, 0);
      log("texture copies drawn on the GPU");
    } else {
      log("texture copy program failed to compile; copies read back instead");
    }
  }
  if (!g_blitProg) return false;
  const float u0 = static_cast<float>(sx) / sw, u1 = static_cast<float>(sx + w) / sw;
  const float v0 = static_cast<float>(sy) / sh, v1 = static_cast<float>(sy + h) / sh;
  const float pos[8] = {-1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f, -1.f};
  const float uv[8] = {u0, v0, u0, v1, u1, v0, u1, v1};
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_for(dstTex, 0));
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_CULL_FACE);
  glDisable(GL_POLYGON_OFFSET_FILL);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  // Destination rows are counted from the top, as set_viewport does.
  glViewport(static_cast<GLint>(dx), static_cast<GLint>(dh) - static_cast<GLint>(dy) - static_cast<GLint>(h),
             static_cast<GLsizei>(w), static_cast<GLsizei>(h));
  glUseProgram(g_blitProg);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, srcTex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(0);
  glDisableVertexAttribArray(1);
  return true;
}

void copy_texture_to_texture(Cmd& c) {
  Texture* src = c.src.tex.get();
  Texture* dst = c.dst.tex.get();
  if (c.src.mip != 0 || c.dst.mip != 0) return;
  ensure_texture(src);
  ensure_texture(dst);
  if (src->surface || src->gl.empty() || dst->gl.empty()) return;
  const uint32_t w = c.u[0], h = c.u[1];
  const GLuint srcTex = src->gl[c.src.z < src->gl.size() ? c.src.z : 0];
  const GLuint dstTex = dst->gl[c.dst.z < dst->gl.size() ? c.dst.z : 0];
  // Colour copies (the EFB copies effects read back: the charged shot's warp, heat
  // haze...) are a textured draw into the destination: the GPU orders it after the
  // source's rendering. A read back through glReadPixels saw the source before the
  // GPU had finished it (a black or stale picture) and stalled the GPU to read it.
  if (!gl_format(src->format).depth && !gl_format(dst->format).depth && srcTex != dstTex && w && h &&
      gpu_copy(srcTex, src->width, src->height, c.src.x, c.src.y, dstTex, dst->height, c.dst.x, c.dst.y, w, h))
    return;
  // Read through an FBO on the source, copy into the destination texture.
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_for(srcTex, 0));
  glBindTexture(GL_TEXTURE_2D, dst->gl[c.dst.z < dst->gl.size() ? c.dst.z : 0]);
  // GL's read origin is bottom-left: source row y (from the top) is GL row H - y - h.
  glCopyTexSubImage2D(GL_TEXTURE_2D, 0, c.dst.x, c.dst.y, c.src.x, src->height - c.src.y - h, w, h);
}

void buffer_to_texture(Cmd& c) {
  auto* b = as<Buffer>(c.obj.get());
  Texture* t = c.dst.tex.get();
  const GlFormat fmt = gl_format(t->format);
  if (!fmt.bpp || !b->data) return;
  Upload u;
  u.tex = c.dst.tex;
  u.mip = c.dst.mip;
  u.x = c.dst.x;
  u.y = c.dst.y;
  u.layer = c.dst.z;
  u.w = c.u[0];
  u.h = c.u[1];
  const uint32_t row = u.w * fmt.bpp;
  const uint32_t bpr = c.u[3] ? c.u[3] : row;
  u.data.resize(static_cast<size_t>(row) * u.h);
  for (uint32_t y = 0; y < u.h; ++y) std::memcpy(u.data.data() + y * row, b->data + c.off + y * bpr, row);
  apply_upload(u);
}
} // namespace

bool gl_ready() { return g_glInit; }

void gl_defer_delete_textures(std::vector<uint32_t>&& names) {
  std::lock_guard lk(g_deleteMutex);
  g_deleteTextures.insert(g_deleteTextures.end(), names.begin(), names.end());
}

void queue_texture_write(Texture* tex, uint32_t mip, uint32_t x, uint32_t y, uint32_t layer, uint32_t w, uint32_t h,
                         const void* data, uint32_t bytesPerRow, uint32_t rowsPerImage, uint32_t layers) {
  const GlFormat fmt = gl_format(tex->format);
  if (!fmt.bpp) return;
  const uint32_t row = w * fmt.bpp;
  const uint32_t bpr = bytesPerRow && bytesPerRow != WGPU_COPY_STRIDE_UNDEFINED ? bytesPerRow : row;
  const uint32_t rpi = rowsPerImage && rowsPerImage != WGPU_COPY_STRIDE_UNDEFINED ? rowsPerImage : h;
  std::vector<Upload> ups;
  for (uint32_t l = 0; l < (layers ? layers : 1); ++l) {
    Upload u;
    u.tex = Ref<Texture>(tex);
    u.mip = mip;
    u.x = x;
    u.y = y;
    u.layer = layer + l;
    u.w = w;
    u.h = h;
    u.data.resize(static_cast<size_t>(row) * h);
    const auto* src = static_cast<const uint8_t*>(data) + static_cast<size_t>(l) * bpr * rpi;
    for (uint32_t r = 0; r < h; ++r) std::memcpy(u.data.data() + r * row, src + static_cast<size_t>(r) * bpr, row);
    ups.push_back(std::move(u));
  }
  std::lock_guard lk(g_uploadMutex);
  for (auto& u : ups) g_uploads.push_back(std::move(u));
}

uint32_t gl_texture_for(TextureView* view) {
  if (!view) return 0;
  Texture* t = view->tex.get();
  ensure_texture(t);
  if (t->gl.empty()) return 0;
  return t->gl[view->baseLayer < t->gl.size() ? view->baseLayer : 0];
}

void apply_sampler(Sampler* s, bool) {
  auto wrap = [](WGPUAddressMode m) {
    return m == WGPUAddressMode_Repeat ? GL_REPEAT : m == WGPUAddressMode_MirrorRepeat ? GL_MIRRORED_REPEAT
                                                                                       : GL_CLAMP_TO_EDGE;
  };
  if (!s) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return;
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap(s->addressU));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap(s->addressV));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s->min == WGPUFilterMode_Linear ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s->mag == WGPUFilterMode_Linear ? GL_LINEAR : GL_NEAREST);
}

void apply_pipeline_state(const RenderPipeline& p, const DrawState& st) {
  MP_ZONE("wv::apply_pipeline_state");
  // Depth
  if (p.hasDepth && st.targetHasDepth) {
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(gl_compare(p.depthCompare));
    glDepthMask(p.depthWrite ? GL_TRUE : GL_FALSE);
  } else {
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
  }
  if (p.hasDepth && (p.depthBias != 0.f || p.depthBiasSlope != 0.f)) {
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(p.depthBiasSlope, p.depthBias);
  } else {
    glDisable(GL_POLYGON_OFFSET_FILL);
  }
  // Culling
  if (p.cull == WGPUCullMode_None || p.cull == WGPUCullMode_Undefined) {
    glDisable(GL_CULL_FACE);
  } else {
    glEnable(GL_CULL_FACE);
    glFrontFace(p.frontFace == WGPUFrontFace_CW ? GL_CW : GL_CCW);
    glCullFace(p.cull == WGPUCullMode_Front ? GL_FRONT : GL_BACK);
  }
  // Blending
  if (p.blend) {
    glEnable(GL_BLEND);
    glBlendEquationSeparate(gl_blend_op(p.color.op), gl_blend_op(p.alpha.op));
    glBlendFuncSeparate(gl_blend_factor(p.color.src), gl_blend_factor(p.color.dst), gl_blend_factor(p.alpha.src),
                        gl_blend_factor(p.alpha.dst));
  } else {
    glDisable(GL_BLEND);
  }
  const uint32_t m = p.writeMask;
  glColorMask((m & WGPUColorWriteMask_Red) != 0, (m & WGPUColorWriteMask_Green) != 0,
              (m & WGPUColorWriteMask_Blue) != 0, (m & WGPUColorWriteMask_Alpha) != 0);
}

// Where the render worker's time goes (us since the last frame log line), by command.
constexpr int kOpCount = static_cast<int>(Op::WriteBuffer) + 1;
uint64_t g_tOp[kOpCount] = {}, g_bytesOp[kOpCount] = {};
uint64_t g_tUploads = 0, g_tSwap = 0, g_tExecute = 0, g_lastLog = 0;
const char* const kOpNames[kOpCount] = {"begin pass", "end pass", "set pipeline", "set bind group",
                                        "set vertex buffer", "set index buffer", "viewport", "scissor",
                                        "blend constant", "stencil ref", "immediates", "draw", "draw indexed",
                                        "buffer copy", "buffer->texture", "texture copy", "texture->buffer",
                                        "clear buffer", "write buffer"};

void execute(std::vector<Cmd>& cmds) {
  MP_ZONE("wv::execute");
  gl_init();
  const uint64_t tBegin = sceKernelGetProcessTimeWide();
  flush_uploads();
  uint64_t t = sceKernelGetProcessTimeWide();
  g_tUploads += t - tBegin;
  for (auto& c : cmds) {
    const int opIndex = static_cast<int>(c.op);
    if (c.op == Op::CopyBufferToBuffer || c.op == Op::WriteBuffer || c.op == Op::ClearBuffer)
      g_bytesOp[opIndex] += c.size;
    switch (c.op) {
    case Op::BeginRenderPass:
      begin_pass(c);
      break;
    case Op::EndRenderPass:
      g_inPass = false;
      g_passDrawable = false;
      break;
    case Op::SetPipeline:
      g_st.pipeline = as<RenderPipeline>(c.obj.get());
      break;
    case Op::SetBindGroup:
      if (c.u[0] < 4) {
        g_st.groups[c.u[0]] = c.obj ? as<BindGroup>(c.obj.get()) : nullptr;
        g_st.dynOffsets[c.u[0]] = c.dyn;
      }
      break;
    case Op::SetVertexBuffer:
      if (c.u[0] < 8) {
        g_st.vertexBuffers[c.u[0]] = c.obj ? as<Buffer>(c.obj.get()) : nullptr;
        g_st.vertexOffsets[c.u[0]] = c.off;
      }
      break;
    case Op::SetIndexBuffer:
      g_st.indexBuffer = c.obj ? as<Buffer>(c.obj.get()) : nullptr;
      g_st.indexOffset = c.off;
      g_st.indexFormat = static_cast<WGPUIndexFormat>(c.u[0]);
      break;
    case Op::SetViewport:
      if (g_passDrawable) set_viewport(c.f[0], c.f[1], c.f[2], c.f[3], c.f[4], c.f[5]);
      break;
    case Op::SetScissor:
      if (g_passDrawable) set_scissor(c.u[0], c.u[1], c.u[2], c.u[3]);
      break;
    case Op::SetBlendConstant:
      std::memcpy(g_st.blendConstant, c.f, sizeof(g_st.blendConstant));
      break;
    case Op::SetStencilRef:
      break;
    case Op::SetImmediates:
      if (c.u[0] + c.u[1] <= sizeof(g_st.immediates)) std::memcpy(g_st.immediates + c.u[0], c.dyn.data(), c.u[1]);
      break;
    case Op::Draw:
      draw(c, false);
      break;
    case Op::DrawIndexed:
      draw(c, true);
      break;
    case Op::CopyTextureToTexture:
      copy_texture_to_texture(c);
      break;
    case Op::CopyBufferToTexture:
      buffer_to_texture(c);
      break;
    case Op::CopyTextureToBuffer:
      break; // GPU readbacks (screenshots, depth peeks): not supported
    case Op::CopyBufferToBuffer: {
      auto* s = as<Buffer>(c.obj.get());
      if (s->data && c.buf2->data && c.off + c.size <= s->size && c.off2 + c.size <= c.buf2->size) {
        // Different buffers never overlap: memcpy is the fast NEON path.
        if (s != c.buf2.get())
          std::memcpy(c.buf2->data + c.off2, s->data + c.off, c.size);
        else
          std::memmove(c.buf2->data + c.off2, s->data + c.off, c.size);
      }
      break;
    }
    case Op::ClearBuffer: {
      auto* b = as<Buffer>(c.obj.get());
      const uint64_t size = c.size == WGPU_WHOLE_SIZE ? b->size - c.off : c.size;
      if (b->data && c.off + size <= b->size) std::memset(b->data + c.off, 0, size);
      break;
    }
    case Op::WriteBuffer: {
      auto* b = as<Buffer>(c.obj.get());
      if (b->data && c.off + c.size <= b->size) std::memcpy(b->data + c.off, c.dyn.data(), c.size);
      break;
    }
    }
    const uint64_t now = sceKernelGetProcessTimeWide();
    g_tOp[opIndex] += now - t;
    t = now;
  }
  g_tExecute += t - tBegin;
}

void present() {
  if (!g_glInit) return;
  const uint64_t tSwap = sceKernelGetProcessTimeWide();
  vglSwapBuffers(GL_FALSE);
  g_tSwap += sceKernelGetProcessTimeWide() - tSwap;
  ++g_frame;
  gx_end_frame();
  g_trace = g_frame == 600;
  if (g_frame <= 3 || g_frame % 300 == 0) {
    uint32_t gxDrawn, gxUnsupported, gxOther;
    gx_frame_stats(gxDrawn, gxUnsupported, gxOther);
    log("frame %llu: %u draws (%u skipped) | GameCube draws since last line: %u drawn, %u not supported yet, %u "
        "failed",
        static_cast<unsigned long long>(g_frame), g_drawsThisFrame, g_skippedThisFrame, gxDrawn, gxUnsupported,
        gxOther);
    // The render worker's time per frame, by what it was doing.
    const uint64_t now = sceKernelGetProcessTimeWide();
    const uint32_t frames = g_frame <= 3 ? 1 : 300;
    if (g_lastLog != 0) {
      char line[1024];
      int n = std::snprintf(line, sizeof line,
                            "render worker, ms per frame: %.1f wall, %.1f playing commands back (uploads %.1f",
                            (now - g_lastLog) / 1000.0 / frames, g_tExecute / 1000.0 / frames,
                            g_tUploads / 1000.0 / frames);
      for (int k = 0; k < kOpCount; ++k) {
        const double ms = g_tOp[k] / 1000.0 / frames;
        if (ms >= 0.1 && n < static_cast<int>(sizeof line) - 64) {
          n += std::snprintf(line + n, sizeof line - n, ", %s %.1f", kOpNames[k], ms);
          if (g_bytesOp[k] != 0) n += std::snprintf(line + n, sizeof line - n, " (%.0f KB)", g_bytesOp[k] / 1024.0 / frames);
        }
      }
      std::snprintf(line + n, sizeof line - n, "), %.1f swapping", g_tSwap / 1000.0 / frames);
      log("%s", line);
    }
    g_lastLog = now;
    for (int k = 0; k < kOpCount; ++k) g_tOp[k] = g_bytesOp[k] = 0;
    g_tUploads = g_tSwap = g_tExecute = 0;
  }
  g_drawsThisFrame = g_skippedThisFrame = 0;
  wgnull::on_present();
}
} // namespace wv
