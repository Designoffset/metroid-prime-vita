// webgpu_vita: GameCube (GX) draws.
//
// gx::build_pipeline (Aurora) calls wv_gx_note_pipeline_config just before
// creating each GX pipeline; the next wgpuDeviceCreateRenderPipeline on that
// thread takes the config. Aurora's WGSL pulls vertices from raw GameCube
// vertex data with storage buffers, which the Vita can't do, so a GX draw here:
//   1. reads the draw's vertices on the CPU (the same fetch rules as Aurora's
//      WGSL: direct or indexed attributes, big-endian, fixed point) and does
//      the per-vertex matrix work there: position and normal through the
//      vertex's position matrix, and texture-coordinate generation;
//   2. draws them with a Cg program generated from the pipeline's ShaderConfig
//      (wv_cg.cpp): projection and GX lighting in the vertex shader, the TEV
//      stages, fog and the alpha test in the fragment shader.
// Not drawn yet (counted in the frame log): lines and points, PBR materials,
// the shadow-map pass.
#include "wv.hpp"
#include "mp_zone.h"
#include "wv_gx.hpp"

#include <vitaGL.h>
#include <psp2/kernel/processmgr.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include <xxhash.h>

#include "gx/gx.hpp"
#include "gx/pipeline.hpp"
#include "gx/shader_info.hpp"
#include "gfx/resources.hpp"

namespace wv {

using aurora::gx::AttrConfig;
using aurora::gx::DrawImmediateData;
using aurora::gx::PipelineConfig;
using aurora::gx::ShaderConfig;
using aurora::gx::ShaderInfo;

namespace {
thread_local GxPipelineInfo* t_pending = nullptr;

// ---------------------------------------------------------------- uniform layout
// Mirrors fill_uniform (gx/shader_info.cpp): where each block starts, in vec4 slots.
void build_layout(GxPipelineInfo& p) {
  const ShaderConfig& sc = p.config.shaderConfig;
  const ShaderInfo& info = p.info;
  GxLayout& L = p.layout;
  int s = 1; // render + logical viewport sizes
  if (info.lineMode != 0) s += 1;
  L.proj = s;
  s += 4;
  if (info.vitaCompact) {
    L.compact = true;
    L.pnFull = info.vitaPnFull;
    L.posM = s;
    s += L.pnFull ? 30 : 3;
    L.nrmM = s;
    s += L.pnFull ? 30 : 3;
    for (int i = 0; i < 8; ++i)
      if ((info.vitaTexMask >> i) & 1) L.texM[i] = s, s += 3;
    for (int i = 0; i < 8; ++i)
      if ((info.vitaPostMask >> i) & 1) L.postM[i] = s, s += 3;
  } else {
    L.postex = s; // 10 position matrices, then 10 texture matrices; 3 rows each
    s += 20 * 3;
    L.nrm = s;
    s += 10 * 3;
  }
  for (int i = 0; i < 4; ++i) {
    L.tevReg[i] = info.loadsTevReg.test(i) ? s++ : -1;
  }
  if (info.lightingEnabled) {
    L.lights = s;
    s += 8 * 5;
    L.lightState = s++;
  }
  for (int i = 0; i < 2; ++i) {
    if (!info.sampledColorChannels.test(i)) continue;
    const auto& cc = sc.colorChannels[i];
    const auto& cca = sc.colorChannels[i + 2];
    if (cc.lightingEnabled && cc.ambSrc == GX_SRC_REG) L.chanAmb[i] = s++;
    if (cc.matSrc == GX_SRC_REG) L.chanMat[i] = s++;
    if (cca.lightingEnabled && cca.ambSrc == GX_SRC_REG) L.chanAmb[i + 2] = s++;
    if (cca.matSrc == GX_SRC_REG) L.chanMat[i + 2] = s++;
  }
  for (int i = 0; i < 4; ++i) {
    L.kcolor[i] = info.sampledKColors.test(i) ? s++ : -1;
  }
  if (info.usesPbr) {
    s += 3 + 27 + 8 * 4 + 8 + 4 + 1 + 3;
  }
  if (info.usesHdrTone) s += 3;
  if (info.usesHudDyin) s += 1;
  if (info.usesPTTexMtx.any()) {
    L.ptTex = s;
    s += 20 * 3;
  }
  if (info.usesFog) {
    L.fog = s;
    s += 5;
  }
  if (info.usesVolFog) s += 4;
  L.tcScale = s;
  s += 8; // texcoord scales
  if (info.usedIndTexMtxs.any()) {
    L.indMtx = s;
    s += 6;
  }
  for (int i = 0; i < 8; ++i) {
    if (info.sampledTextures.test(i)) L.sizeBias[i] = s++;
  }
  L.slots = s;
}

const char* unsupported_reason(const GxPipelineInfo& p) {
  const ShaderConfig& sc = p.config.shaderConfig;
  if (p.config.shadowPass != 0) return "shadow-map pass";
  if (sc.lineMode != 0) return "lines/points";
  if (p.info.usesPbr) return "PBR material";
  return nullptr;
}

// ---------------------------------------------------------------- vertex fetch
struct Mem {
  const uint8_t* p = nullptr;
  uint32_t size = 0;
};

inline uint32_t rd8(const Mem& m, uint32_t o) { return o < m.size ? m.p[o] : 0; }
inline uint32_t rd16(const Mem& m, uint32_t o, bool le) {
  const uint32_t a = rd8(m, o), b = rd8(m, o + 1);
  return le ? (a | b << 8) : (a << 8 | b);
}
inline uint32_t rd24(const Mem& m, uint32_t o, bool le) {
  const uint32_t a = rd8(m, o), b = rd8(m, o + 1), c = rd8(m, o + 2);
  return le ? (a | b << 8 | c << 16) : (a << 16 | b << 8 | c);
}
inline uint32_t rd32(const Mem& m, uint32_t o, bool le) {
  if (o + 4 > m.size) return 0;
  uint32_t v;
  std::memcpy(&v, m.p + o, 4);
  return le ? v : __builtin_bswap32(v);
}
inline float rdf(const Mem& m, uint32_t o, bool le) {
  const uint32_t v = rd32(m, o, le);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// One component of a non-colour attribute (GX_U8..GX_F32).
inline float comp(const Mem& m, uint32_t o, const AttrConfig& a, uint32_t i, bool le) {
  const float scale = 1.f / static_cast<float>(1u << a.frac);
  switch (a.compType) {
  case GX_U8:
    return rd8(m, o + i) * scale;
  case GX_S8:
    return static_cast<int8_t>(rd8(m, o + i)) * scale;
  case GX_U16:
    return rd16(m, o + i * 2, le) * scale;
  case GX_S16:
    return static_cast<int16_t>(rd16(m, o + i * 2, le)) * scale;
  case GX_F32:
    return rdf(m, o + i * 4, le);
  default:
    return 0.f;
  }
}

inline void color(const Mem& m, uint32_t o, uint8_t type, bool le, float* c) {
  switch (type) {
  case GX_RGB565: {
    const uint32_t v = rd16(m, o, le);
    c[0] = ((v >> 11) & 0x1F) / 31.f;
    c[1] = ((v >> 5) & 0x3F) / 63.f;
    c[2] = (v & 0x1F) / 31.f;
    c[3] = 1.f;
    break;
  }
  case GX_RGB8:
  case GX_RGBX8:
    c[0] = rd8(m, o) / 255.f;
    c[1] = rd8(m, o + 1) / 255.f;
    c[2] = rd8(m, o + 2) / 255.f;
    c[3] = 1.f;
    break;
  case GX_RGBA4: {
    const uint32_t v = rd16(m, o, le);
    c[0] = ((v >> 12) & 0xF) / 15.f;
    c[1] = ((v >> 8) & 0xF) / 15.f;
    c[2] = ((v >> 4) & 0xF) / 15.f;
    c[3] = (v & 0xF) / 15.f;
    break;
  }
  case GX_RGBA6: {
    const uint32_t v = rd24(m, o, le);
    c[0] = ((v >> 18) & 0x3F) / 63.f;
    c[1] = ((v >> 12) & 0x3F) / 63.f;
    c[2] = ((v >> 6) & 0x3F) / 63.f;
    c[3] = (v & 0x3F) / 63.f;
    break;
  }
  case GX_RGBA8:
  default:
    c[0] = rd8(m, o) / 255.f;
    c[1] = rd8(m, o + 1) / 255.f;
    c[2] = rd8(m, o + 2) / 255.f;
    c[3] = rd8(m, o + 3) / 255.f;
    break;
  }
}

struct Fetch {
  Mem vbuf, abuf;
  const DrawImmediateData* imm;
  uint32_t stride;
};

// Where attribute `attr` of vertex `vidx` lives (Aurora's attr_address).
inline bool attr_addr(const Fetch& f, const AttrConfig& a, int attr, uint32_t vidx, const Mem*& m, uint32_t& off,
                      bool& le) {
  const uint32_t base = f.imm->vtxStart + vidx * f.stride + a.offset;
  if (a.attrType == GX_INDEX8 || a.attrType == GX_INDEX16) {
    const uint32_t idx = a.attrType == GX_INDEX8 ? rd8(f.vbuf, base) : rd16(f.vbuf, base, false);
    const int slot = attr - GX_VA_POS;
    if (slot < 0 || slot >= 12) return false;
    m = &f.abuf;
    off = f.imm->arrayStart[slot] + idx * a.stride;
    le = a.le;
    return true;
  }
  if (a.attrType == GX_DIRECT) {
    m = &f.vbuf;
    off = base;
    le = false;
    return true;
  }
  return false;
}

inline void load_vec(const Fetch& f, const AttrConfig& a, int attr, uint32_t vidx, float* out, int n) {
  const Mem* m;
  uint32_t off;
  bool le;
  if (!attr_addr(f, a, attr, vidx, m, off, le)) return;
  const int cnt = a.cnt < n ? a.cnt : n;
  for (int i = 0; i < cnt; ++i) out[i] = comp(*m, off, a, i, le);
}

inline void load_color(const Fetch& f, const AttrConfig& a, int attr, uint32_t vidx, float* out) {
  const Mem* m;
  uint32_t off;
  bool le;
  if (!attr_addr(f, a, attr, vidx, m, off, le)) return;
  color(*m, off, a.compType, le, out);
}

inline uint32_t load_u8_attr(const Fetch& f, const AttrConfig& a, int attr, uint32_t vidx) {
  const Mem* m;
  uint32_t off;
  bool le;
  if (!attr_addr(f, a, attr, vidx, m, off, le)) return 0;
  return rd8(*m, off);
}

// rows: a 3x4 matrix as three vec4 rows; out = rows * (v, w)
inline void mul34(const float* rows, const float* v, float w, float* out) {
  for (int r = 0; r < 3; ++r) {
    const float* R = rows + r * 4;
    out[r] = R[0] * v[0] + R[1] * v[1] + R[2] * v[2] + R[3] * w;
  }
}
inline void mul34v4(const float* rows, const float* v, float* out) {
  for (int r = 0; r < 3; ++r) {
    const float* R = rows + r * 4;
    out[r] = R[0] * v[0] + R[1] * v[1] + R[2] * v[2] + R[3] * v[3];
  }
}

// Fast per-draw readers: the attribute's byte location resolved once per draw and
// a decoder chosen for its component type, count and byte order.
using DecodeFn = void (*)(const uint8_t* p, float* out, float scale);

template <int N>
void dec_u8(const uint8_t* p, float* o, float s) {
  for (int i = 0; i < N; ++i) o[i] = p[i] * s;
}
template <int N>
void dec_s8(const uint8_t* p, float* o, float s) {
  for (int i = 0; i < N; ++i) o[i] = static_cast<int8_t>(p[i]) * s;
}
template <int N, bool LE>
void dec_u16(const uint8_t* p, float* o, float s) {
  for (int i = 0; i < N; ++i)
    o[i] = (LE ? (p[2 * i] | p[2 * i + 1] << 8) : (p[2 * i] << 8 | p[2 * i + 1])) * s;
}
template <int N, bool LE>
void dec_s16(const uint8_t* p, float* o, float s) {
  for (int i = 0; i < N; ++i)
    o[i] = static_cast<int16_t>(LE ? (p[2 * i] | p[2 * i + 1] << 8) : (p[2 * i] << 8 | p[2 * i + 1])) * s;
}
template <int N, bool LE>
void dec_f32(const uint8_t* p, float* o, float) {
  for (int i = 0; i < N; ++i) {
    uint32_t v;
    std::memcpy(&v, p + 4 * i, 4);
    if (!LE) v = __builtin_bswap32(v);
    std::memcpy(o + i, &v, 4);
  }
}
template <bool LE>
void dec_color(const uint8_t* p, float* c, float type) {
  const int t = static_cast<int>(type);
  Mem m{p, 4};
  color(m, 0, static_cast<uint8_t>(t), LE, c);
}

template <int N>
DecodeFn pick(uint8_t compType, bool le) {
  switch (compType) {
  case GX_U8:
    return dec_u8<N>;
  case GX_S8:
    return dec_s8<N>;
  case GX_U16:
    return le ? dec_u16<N, true> : dec_u16<N, false>;
  case GX_S16:
    return le ? dec_s16<N, true> : dec_s16<N, false>;
  case GX_F32:
    return le ? dec_f32<N, true> : dec_f32<N, false>;
  default:
    return nullptr;
  }
}

struct Reader {
  int kind = 0; // 0 none, 1 direct, 2 index8, 3 index16
  const uint8_t* vbase = nullptr; // vbuf + vtxStart + offset
  uint32_t vstride = 0;
  uint32_t vlimit = 0;            // vertices readable directly from vbuf
  const uint8_t* arr = nullptr;   // array data (indexed)
  uint32_t arrStride = 0, arrCount = 0;
  DecodeFn fn = nullptr;
  float scale = 1.f;
  uint32_t need = 0;              // bytes the decoder reads
};

void setup_common(Reader& r, const Fetch& f, const AttrConfig& a, int attr) {
  r.kind = 0;
  if (a.attrType == GX_NONE) return;
  const uint64_t base = static_cast<uint64_t>(f.imm->vtxStart) + a.offset;
  if (f.stride == 0 || base >= f.vbuf.size) return;
  r.vbase = f.vbuf.p + base;
  r.vstride = f.stride;
  r.vlimit = static_cast<uint32_t>((f.vbuf.size - base) / f.stride);
  if (a.attrType == GX_DIRECT) {
    r.kind = 1;
  } else {
    const int slotIdx = attr - GX_VA_POS;
    if (slotIdx < 0 || slotIdx >= 12) return;
    const uint32_t start = f.imm->arrayStart[slotIdx];
    if (start >= f.abuf.size || a.stride == 0) return;
    r.arr = f.abuf.p + start;
    r.arrStride = a.stride;
    r.arrCount = (f.abuf.size - start) / a.stride;
    r.kind = a.attrType == GX_INDEX8 ? 2 : 3;
  }
}

void setup_reader(Reader& r, const Fetch& f, const AttrConfig& a, int attr, int maxN, bool isColor) {
  setup_common(r, f, a, attr);
  if (r.kind == 0) return;
  if (isColor) {
    const bool le = r.kind != 1 && a.le;
    r.fn = le ? dec_color<true> : dec_color<false>;
    r.scale = a.compType;
    r.need = 4;
    return;
  }
  const bool le = r.kind != 1 && a.le;
  int n = a.cnt < maxN ? a.cnt : maxN;
  if (attr == GX_VA_NRM && a.cnt > 3) n = 3;
  DecodeFn fn = nullptr;
  switch (n) {
  case 1:
    fn = pick<1>(a.compType, le);
    break;
  case 2:
    fn = pick<2>(a.compType, le);
    break;
  default:
    fn = pick<3>(a.compType, le);
    break;
  }
  if (!fn) {
    r.kind = 0;
    return;
  }
  r.fn = fn;
  r.scale = 1.f / static_cast<float>(1u << a.frac);
  static const uint32_t kBytes[] = {1, 1, 2, 2, 4};
  r.need = n * (a.compType <= GX_F32 ? kBytes[a.compType] : 4);
}

void setup_u8_reader(Reader& r, const Fetch& f, const AttrConfig& a) {
  setup_common(r, f, a, GX_VA_PNMTXIDX); // matrix indices are always direct
  if (r.kind != 1) r.kind = 0;
}

inline const uint8_t* locate(const Reader& r, uint32_t vidx) {
  if (vidx >= r.vlimit) return nullptr;
  const uint8_t* v = r.vbase + vidx * r.vstride;
  if (r.kind == 1) return v;
  const uint32_t idx = r.kind == 2 ? v[0] : (v[0] << 8 | v[1]);
  if (idx >= r.arrCount) return nullptr;
  return r.arr + idx * r.arrStride;
}

inline void read_vec(const Reader& r, uint32_t vidx, float* out) {
  if (r.kind == 0) return;
  const uint8_t* p = locate(r, vidx);
  if (p) r.fn(p, out, r.scale);
}

inline uint32_t read_u8(const Reader& r, uint32_t vidx) {
  return vidx < r.vlimit ? r.vbase[vidx * r.vstride] : 0;
}

// ---------------------------------------------------------------- decoded-vertex cache
// A draw's attributes before any matrix (positions, normals, colours, texture uvs,
// matrix indices), keyed by a hash of the bytes they come from: the draw's own
// vertex data and the array ranges its indices reach. Room geometry is the same
// from frame to frame, so most draws skip the decode and only run the transforms.
// GPU-transform programs keep the decoded vertices (and the draw's rebased
// indices) in GL buffers instead: drawn straight from video memory each frame.
struct RawEntry {
  std::vector<float> data;
  uint32_t lastFrame = 0;
  GLuint vbo = 0, ibo = 0;
  uint32_t count = 0, nIdx = 0;
  uint32_t in = 0; // the raw layout (kRaw* bits) the buffer was built with
  size_t bytes = 0;
};
std::unordered_map<uint64_t, RawEntry> g_rawCache;
std::unordered_map<uint64_t, uint32_t> g_seen; // GPU-path keys seen once, by frame
std::unordered_map<uint64_t, uint64_t> g_fast; // resident draw (where it reads) -> its g_rawCache key
uint64_t g_fastHits = 0;
uint64_t g_statDraws = 0, g_statSameState = 0; // draws sharing the previous draw's pipeline+textures
size_t g_rawBytes = 0;
uint32_t g_gxFrame = 0;
uint64_t g_cacheHits = 0, g_cacheMisses = 0, g_gpuDraws = 0;
constexpr size_t kRawBudget = 24u << 20;

void raw_cache_trim() {
  if (g_rawBytes <= kRawBudget) return;
  // Drop what the last few frames did not use, oldest first by age bands.
  // GL buffers are dropped only when a few frames old (the GPU may still read
  // the last frames' buffers).
  for (uint32_t age : {120u, 30u, 8u, 3u}) {
    for (auto it = g_rawCache.begin(); it != g_rawCache.end();) {
      if (g_gxFrame - it->second.lastFrame >= age) {
        g_rawBytes -= it->second.bytes;
        if (it->second.vbo) glDeleteBuffers(1, &it->second.vbo);
        if (it->second.ibo) glDeleteBuffers(1, &it->second.ibo);
        it = g_rawCache.erase(it);
      } else {
        ++it;
      }
    }
    if (g_rawBytes <= kRawBudget * 3 / 4) return;
  }
}

// ---------------------------------------------------------------- programs
std::mutex g_progMutex;
std::unordered_map<std::string, GxProgram*> g_programs; // by ShaderConfig bytes

GLuint compile(GLenum type, const std::string& src, bool& ok) {
  GLuint s = glCreateShader(type);
  const char* p = src.c_str();
  glShaderSource(s, 1, &p, nullptr);
  glCompileShader(s);
  GLint st = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &st);
  if (!st) {
    char buf[1024] = {};
    glGetShaderInfoLog(s, sizeof buf, nullptr, buf);
    log("gx: %s shader failed to compile: %s", type == GL_CG_VERTEX_SHADER_EXT ? "vertex" : "fragment", buf);
    log("gx: source:\n%s", src.c_str());
    ok = false;
  }
  return s;
}

GxProgram* program_for(GxPipelineInfo& p) {
  if (p.program) return p.program;
  const ShaderConfig& sc = p.config.shaderConfig;
  std::string key(reinterpret_cast<const char*>(&sc), sizeof sc);
  auto it = g_programs.find(key);
  if (it != g_programs.end()) {
    p.program = it->second;
    return p.program;
  }
  auto* prog = new GxProgram;
  g_programs.emplace(std::move(key), prog);
  p.program = prog;

  // ux0:data/MetroidPrime/gx_cpu (any content) forces the CPU path: an A/B switch.
  static const bool kGpu = [] {
    if (getenv("MP_GX_CPU")) return false;
    FILE* fl = fopen("ux0:data/MetroidPrime/gx_cpu", "rb");
    if (fl) {
      fclose(fl);
      log("gx: gx_cpu file present: CPU vertex path");
      return false;
    }
    return true;
  }();
  CgSources src = generate_cg(sc, p.info, kGpu);
  prog->attrs = src.attrs;
  prog->gpu = src.gpu;
  prog->gpuPn = src.gpuPn;
  prog->gpuIn = src.gpuIn;
  prog->texRows = src.texRows;
  prog->postRows = src.postRows;
  bool ok = true;
  GLuint vs = compile(GL_CG_VERTEX_SHADER_EXT, src.vs, ok);
  if (!ok && src.gpu) {
    // The GPU-transform shader did not compile: this config keeps the CPU path.
    static int fallbacks = 0;
    if (++fallbacks <= 5) log("gx: GPU-transform vertex shader failed; using the CPU path for this program");
    src = generate_cg(sc, p.info, false);
    prog->gpu = false;
    ok = true;
    vs = compile(GL_CG_VERTEX_SHADER_EXT, src.vs, ok);
  }
  const GLuint fs = compile(GL_CG_FRAGMENT_SHADER_EXT, src.fs, ok);
  if (!ok) {
    prog->failed = true;
    return prog;
  }
  prog->prog = glCreateProgram();
  glAttachShader(prog->prog, vs);
  glAttachShader(prog->prog, fs);
  static const char* kAttrNames[kAttrCount] = {"aPos", "aNrm", "aClr0", "aClr1", "aTc0", "aTc1",
                                               "aTc2", "aTc3", "aTc4", "aTc5", "aTc6", "aTc7"};
  static const char* kRawNames[kRawCount] = {"rPos", "rNrm", "rClr0", "rClr1", "rUv0", "rUv1", "rUv2",
                                             "rUv3", "rUv4", "rUv5", "rUv6", "rUv7", "rPn"};
  if (prog->gpu) {
    for (int a = 0; a < kRawCount; ++a)
      if (prog->gpuIn & (1u << a)) glBindAttribLocation(prog->prog, a, kRawNames[a]);
  } else {
    for (int a = 0; a < kAttrCount; ++a)
      if (prog->attrs & (1u << a)) glBindAttribLocation(prog->prog, a, kAttrNames[a]);
  }
  glLinkProgram(prog->prog);
  prog->uPosM = glGetUniformLocation(prog->prog, "uPosM");
  prog->uNrmM = glGetUniformLocation(prog->prog, "uNrmM");
  prog->uTexM = glGetUniformLocation(prog->prog, "uTexM");
  prog->uPost = glGetUniformLocation(prog->prog, "uPost");
  prog->uTcS = glGetUniformLocation(prog->prog, "uTcS");
  prog->uIndM = glGetUniformLocation(prog->prog, "uIndM");
  prog->uSzB = glGetUniformLocation(prog->prog, "uSzB");
  prog->uProj = glGetUniformLocation(prog->prog, "uProj");
  prog->uLights = glGetUniformLocation(prog->prog, "uLights");
  prog->uLightEn = glGetUniformLocation(prog->prog, "uLightEn");
  prog->uChan = glGetUniformLocation(prog->prog, "uChan");
  prog->uTev = glGetUniformLocation(prog->prog, "uTev");
  prog->uKc = glGetUniformLocation(prog->prog, "uKc");
  prog->uFog = glGetUniformLocation(prog->prog, "uFog");
  for (int i = 0; i < 8; ++i) {
    char name[8] = "uTex0";
    name[4] = static_cast<char>('0' + i);
    prog->uTex[i] = glGetUniformLocation(prog->prog, name);
  }
  static int built = 0;
  static int builtGpu = 0;
  if (prog->gpu) ++builtGpu;
  if (++built <= 5 || built % 50 == 0) log("gx: built program %d (%d with GPU transforms)", built, builtGpu);
  return prog;
}

uint32_t g_drawn = 0, g_skippedUnsupported = 0, g_skippedOther = 0;
// Time spent in gx_draw's parts since the last stats line (us), and vertices decoded.
uint64_t g_tDecode = 0, g_tState = 0, g_tDraw = 0, g_vertices = 0;
std::vector<float> g_verts;
std::vector<uint16_t> g_indices;
} // namespace

GxPipelineInfo* gx_take_pending_pipeline() {
  GxPipelineInfo* p = t_pending;
  t_pending = nullptr;
  return p;
}

void gx_free(GxPipelineInfo* info) { delete info; }

void gx_end_frame() { ++g_gxFrame; }

void gx_frame_stats(uint32_t& drawn, uint32_t& unsupported, uint32_t& other) {
  drawn = g_drawn;
  unsupported = g_skippedUnsupported;
  other = g_skippedOther;
  if (g_drawn != 0)
    log("gx: since last line: vertex decode %.1f ms, uniforms+state+textures %.1f ms, GL draw calls %.1f ms; %llu "
        "vertices (%.0f per draw)",
        g_tDecode / 1000.0, g_tState / 1000.0, g_tDraw / 1000.0, static_cast<unsigned long long>(g_vertices),
        static_cast<double>(g_vertices) / g_drawn);
  if (g_cacheHits + g_cacheMisses != 0)
    log("gx: vertex cache: %.0f%% of draws reused, %.1f MB held; %.0f%% of draws transformed on the GPU",
        100.0 * g_cacheHits / (g_cacheHits + g_cacheMisses), g_rawBytes / 1048576.0,
        g_drawn ? 100.0 * g_gpuDraws / g_drawn : 0.0);
  if (g_drawn) log("gx: %.0f%% of draws found by their resident place", 100.0 * g_fastHits / g_drawn);
  g_fastHits = 0;
  if (g_statDraws)
    log("gx: %.0f%% of draws share the previous draw's pipeline and textures (batchable)",
        100.0 * g_statSameState / g_statDraws);
  g_statDraws = g_statSameState = 0;
  g_cacheHits = g_cacheMisses = 0;
  g_gpuDraws = 0;
  g_drawn = g_skippedUnsupported = g_skippedOther = 0;
  g_tDecode = g_tState = g_tDraw = g_vertices = 0;
}

// The draw's pipeline state, uniforms (only what changed for the program) and textures.
static void set_gx_state(DrawState& st, GxProgram* prog, const GxLayout& L, const float* U) {
  MP_ZONE("wv::set_gx_state");
  auto slot = [U](int s) { return U + s * 4; };
  // ---- state and uniforms
  glUseProgram(prog->prog);
  apply_pipeline_state(*st.pipeline, st);
  // Bring-up: face culling off until its winding is confirmed on the Vita (a wrong
  // winding would hide everything; drawing both sides only costs a little).
  glDisable(GL_CULL_FACE);

  static const float kZero[4 * 40] = {};
  // Uploads only what changed since this program's last draw: most draws share the
  // lights, projection and colours of the one before.
  auto upload = [prog](int cache, int loc, int count, const float* data) {
    std::vector<float>& last = prog->last[cache];
    const size_t n = static_cast<size_t>(count) * 4;
    if (last.size() == n && std::memcmp(last.data(), data, n * 4) == 0) return;
    last.assign(data, data + n);
    glUniform4fv(loc, count, data);
  };
  if (prog->uProj >= 0) upload(0, prog->uProj, 4, slot(L.proj));
  if (prog->uLights >= 0) upload(1, prog->uLights, 40, L.lights >= 0 ? slot(L.lights) : kZero);
  if (prog->uLightEn >= 0) {
    // Per channel (COLOR0, COLOR1, ALPHA0, ALPHA1) and light: x = 1 when the light is on.
    // Packed: channel c, light i at flat index c * 8 + i (uLightEn[8]).
    float en[8 * 4] = {};
    if (L.lightState >= 0) {
      const auto* masks = reinterpret_cast<const uint32_t*>(slot(L.lightState));
      for (int c = 0; c < 4; ++c)
        for (int i = 0; i < 8; ++i) en[c * 8 + i] = (masks[c] >> i) & 1 ? 1.f : 0.f;
    }
    upload(2, prog->uLightEn, 8, en);
  }
  if (prog->uChan >= 0) {
    float ch[8 * 4] = {};
    for (int c = 0; c < 4; ++c) {
      if (L.chanAmb[c] >= 0) std::memcpy(ch + c * 8, slot(L.chanAmb[c]), 16);
      if (L.chanMat[c] >= 0) std::memcpy(ch + c * 8 + 4, slot(L.chanMat[c]), 16);
    }
    upload(3, prog->uChan, 8, ch);
  }
  if (prog->uTev >= 0) {
    float t[16] = {};
    for (int i = 0; i < 4; ++i)
      if (L.tevReg[i] >= 0) std::memcpy(t + i * 4, slot(L.tevReg[i]), 16);
    upload(4, prog->uTev, 4, t);
  }
  if (prog->uKc >= 0) {
    float k[16] = {};
    for (int i = 0; i < 4; ++i)
      if (L.kcolor[i] >= 0) std::memcpy(k + i * 4, slot(L.kcolor[i]), 16);
    upload(5, prog->uKc, 4, k);
  }
  if (prog->uFog >= 0 && L.fog >= 0) upload(6, prog->uFog, 2, slot(L.fog));
  // Indirect texturing: texcoord scales, indirect matrices, texture sizes by map.
  if (prog->uTcS >= 0 && L.tcScale >= 0) upload(12, prog->uTcS, 8, slot(L.tcScale));
  if (prog->uIndM >= 0 && L.indMtx >= 0) upload(13, prog->uIndM, 6, slot(L.indMtx));
  if (prog->uSzB >= 0) {
    float sb[8 * 4] = {};
    for (int i = 0; i < 8; ++i)
      if (L.sizeBias[i] >= 0) std::memcpy(sb + i * 4, slot(L.sizeBias[i]), 16);
    upload(14, prog->uSzB, 8, sb);
  }

  // ---- textures: texture map i at group 2 binding 2i, its sampler at 2i + 1
  BindGroup* g2 = st.groups[2];
  for (int i = 0; i < 8; ++i) {
    if (prog->uTex[i] < 0) continue;
    GLuint tex = 0;
    const BindEntry* te = g2 ? g2->find(2 * i) : nullptr;
    const BindEntry* se = g2 ? g2->find(2 * i + 1) : nullptr;
    if (te && te->view) tex = gl_texture_for(te->view.get());
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, tex);
    apply_sampler(se ? se->sampler.get() : nullptr, false);
  }
  if (prog->last[7].empty()) {
    // Sampler i reads texture unit i: set once per program.
    for (int i = 0; i < 8; ++i)
      if (prog->uTex[i] >= 0) glUniform1i(prog->uTex[i], i);
    prog->last[7].push_back(1.f);
  }
  glActiveTexture(GL_TEXTURE0);
}

// The GPU-transform draw of one vertex set: from its GL buffers (e), or from CPU
// memory (verts/idx) for one-off geometry. State, uniforms, matrices, then the draw.
static void gpu_submit(DrawState& st, GxPipelineInfo& p, GxProgram* prog, const float* U,
                       const DrawImmediateData& imm, RawEntry* e, const float* verts, const uint16_t* idx,
                       uint32_t nIdx, uint32_t count, uint64_t tDecoded) {
  MP_ZONE("wv::gpu_submit");
  static const int kN[kRawCount] = {3, 3, 4, 4, 2, 2, 2, 2, 2, 2, 2, 2, 1};
  const ShaderConfig& sc = p.config.shaderConfig;
  const ShaderInfo& info = p.info;
  const GxLayout& L = p.layout;
  auto slot = [U](int s) { return U + s * 4; };
  const float* postex = L.postex >= 0 ? slot(L.postex) : nullptr;
  const float* nrmMtx = L.nrm >= 0 ? slot(L.nrm) : nullptr;
  const float* ptTex = L.ptTex >= 0 ? slot(L.ptTex) : nullptr;
  int o[kRawCount];
  int rs = 0;
  for (int a = 0; a < kRawCount; ++a) {
    o[a] = -1;
    if (prog->gpuIn & (1u << a)) o[a] = rs, rs += kN[a];
  }
  set_gx_state(st, prog, L, U);
  auto upload = [prog](int cache, int loc, int n, const float* data) {
    if (loc < 0) return;
    std::vector<float>& last = prog->last[cache];
    const size_t k = static_cast<size_t>(n) * 4;
    if (last.size() == k && std::memcmp(last.data(), data, k * 4) == 0) return;
    last.assign(data, data + k);
    glUniform4fv(loc, n, data);
  };
  const uint32_t fixedPn = imm.currentPnMtx > 9 ? 0 : imm.currentPnMtx;
  if (L.compact) {
    // Already the rows the shader takes (the draw's matrix, or all 10 with indices).
    const int n = L.pnFull ? 30 : 3;
    upload(8, prog->uPosM, n, slot(L.posM));
    upload(9, prog->uNrmM, n, slot(L.nrmM));
  } else if (prog->gpuPn) {
    upload(8, prog->uPosM, 30, postex);
    upload(9, prog->uNrmM, 30, nrmMtx);
  } else {
    upload(8, prog->uPosM, 3, postex + fixedPn * 12);
    upload(9, prog->uNrmM, 3, nrmMtx + fixedPn * 12);
  }
  if (prog->uTexM >= 0 || prog->uPost >= 0) {
    static const float kIdent[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    float tm[24 * 4], pt[24 * 4];
    for (int i = 0; i < 8; ++i) {
      std::memcpy(tm + i * 12, kIdent, sizeof kIdent);
      std::memcpy(pt + i * 12, kIdent, sizeof kIdent);
      if (!info.sampledTexCoords.test(i)) continue;
      const auto& tcg = sc.tcgs[i];
      if (L.compact) {
        if (L.texM[i] >= 0) std::memcpy(tm + i * 12, slot(L.texM[i]), 48);
        if (L.postM[i] >= 0) std::memcpy(pt + i * 12, slot(L.postM[i]), 48);
        continue;
      }
      if ((tcg.type == GX_TG_MTX2x4 || tcg.type == GX_TG_MTX3x4) && tcg.mtx != GX_IDENTITY && tcg.mtx / 3 < 20)
        std::memcpy(tm + i * 12, postex + (tcg.mtx / 3) * 12, 48);
      if (tcg.postMtx != GX_PTIDENTITY && ptTex) {
        const int pm = (tcg.postMtx - GX_PTTEXMTX0) / 3;
        if (pm >= 0 && pm < 20) std::memcpy(pt + i * 12, ptTex + pm * 12, 48);
      }
    }
    if (prog->texRows) upload(10, prog->uTexM, prog->texRows, tm);
    if (prog->postRows) upload(11, prog->uPost, prog->postRows, pt);
  }
  const uint64_t tState = sceKernelGetProcessTimeWide();
  g_tState += tState - tDecoded;

  if (e) glBindBuffer(GL_ARRAY_BUFFER, e->vbo);
  for (int a = 0; a < kRawCount; ++a) {
    if (o[a] >= 0) {
      glEnableVertexAttribArray(a);
      if (e)
        glVertexAttribPointer(a, kN[a], GL_FLOAT, GL_FALSE, rs * 4,
                              reinterpret_cast<const void*>(static_cast<uintptr_t>(o[a]) * 4));
      else
        glVertexAttribPointer(a, kN[a], GL_FLOAT, GL_FALSE, rs * 4, verts + o[a]);
    } else {
      glDisableVertexAttribArray(a);
    }
  }
  if (nIdx && e) {
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, e->ibo);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(nIdx), GL_UNSIGNED_SHORT, nullptr);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  } else if (nIdx) {
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(nIdx), GL_UNSIGNED_SHORT, idx);
  } else {
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(count));
  }
  for (int a = 0; a < kRawCount; ++a)
    if (o[a] >= 0) glDisableVertexAttribArray(a);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  g_tDraw += sceKernelGetProcessTimeWide() - tState;
  ++g_drawn;
  ++g_gpuDraws;
}

void gx_draw(DrawState& st, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, bool indexed,
             uint32_t indexCount, uint32_t firstIndex, int32_t baseVertex) {
  MP_ZONE("wv::gx_draw");
  GxPipelineInfo& p = *st.pipeline->gx;
  if (p.unsupported) {
    ++g_skippedUnsupported;
    return;
  }
  {
    // Batching potential: a draw with the previous draw's pipeline and textures.
    static const RenderPipeline* lastPipe = nullptr;
    static const BindGroup* lastTex = nullptr;
    ++g_statDraws;
    if (st.pipeline == lastPipe && st.groups[2] == lastTex) ++g_statSameState;
    lastPipe = st.pipeline;
    lastTex = st.groups[2];
  }
  if (instanceCount == 0) return;
  const ShaderConfig& sc = p.config.shaderConfig;
  const ShaderInfo& info = p.info;
  const GxLayout& L = p.layout;

  // Buffers: group 0 = vertex data (b0) and arrays (b1); group 1 = uniforms (dynamic offset).
  BindGroup* g0 = st.groups[0];
  BindGroup* g1 = st.groups[1];
  if (!g0 || !g1) {
    ++g_skippedOther;
    return;
  }
  const BindEntry* vb = g0->find(0);
  const BindEntry* ab = g0->find(1);
  const BindEntry* ub = g1->find(0);
  if (!vb || !vb->buffer || !vb->buffer->data || !ub || !ub->buffer || !ub->buffer->data) {
    ++g_skippedOther;
    return;
  }
  auto mem_of = [](const BindEntry* e) {
    Mem m;
    if (!e || !e->buffer || !e->buffer->data || e->offset >= e->buffer->size) return m;
    m.p = e->buffer->data + e->offset;
    const uint64_t avail = e->buffer->size - e->offset;
    const uint64_t sz = e->size == 0 || e->size == WGPU_WHOLE_SIZE || e->size > avail ? avail : e->size;
    m.size = static_cast<uint32_t>(sz);
    return m;
  };
  const uint64_t uoff = ub->offset + (st.dynOffsets[1].empty() ? 0 : st.dynOffsets[1][0]);
  if (uoff + static_cast<uint64_t>(L.slots) * 16 > ub->buffer->size) {
    ++g_skippedOther;
    return;
  }
  const float* U = reinterpret_cast<const float*>(ub->buffer->data + uoff);
  auto slot = [U](int s) { return U + s * 4; };

  DrawImmediateData imm;
  std::memcpy(&imm, st.immediates, sizeof imm);
  Fetch f{mem_of(vb), mem_of(ab), &imm, sc.vtxStride};

  // ---- Fast path: resident room geometry. Aurora keeps a retained display list's
  // vertices and indices at fixed places in its resident regions, so the draw is
  // known by where it reads (and its arrays' hashes): no fingerprint of its bytes,
  // no index scan, straight to the GL buffers built the first time.
  uint64_t fastKey = 0;
  const uint64_t tFast = sceKernelGetProcessTimeWide();
  if (indexed && L.compact && st.indexBuffer && st.indexBuffer->data && st.indexFormat == WGPUIndexFormat_Uint16) {
    static const aurora::gfx::Range rv = aurora::gfx::resident_region(aurora::gfx::ResidentBuffer::Vertex);
    static const aurora::gfx::Range ri = aurora::gfx::resident_region(aurora::gfx::ResidentBuffer::Index);
    const uint64_t vAbs = vb->offset + imm.vtxStart;
    const uint64_t iAbs = st.indexOffset + static_cast<uint64_t>(firstIndex) * 2;
    if (rv.size != 0 && ri.size != 0 && vAbs >= rv.offset && vAbs < static_cast<uint64_t>(rv.offset) + rv.size &&
        iAbs >= ri.offset && iAbs < static_cast<uint64_t>(ri.offset) + ri.size) {
      if (!p.attrsHashed) {
        p.attrsHash = XXH3_64bits(sc.attrs.data(), sizeof(sc.attrs));
        p.attrsHashed = true;
      }
      GxProgram* fprog = program_for(p);
      uint32_t k[24];
      int n = 0;
      // The shader's raw layout is part of the key: a lit pipeline (the charging gun's
      // light) wants normals the unlit pipeline's buffer of the same geometry lacks.
      k[n++] = fprog ? fprog->gpuIn : 0;
      k[n++] = static_cast<uint32_t>(vAbs);
      k[n++] = static_cast<uint32_t>(iAbs);
      k[n++] = indexCount;
      k[n++] = static_cast<uint32_t>(baseVertex);
      k[n++] = sc.vtxStride;
      k[n++] = static_cast<uint32_t>(p.attrsHash);
      k[n++] = static_cast<uint32_t>(p.attrsHash >> 32);
      for (int a = 0; a < 12; ++a) k[n++] = imm.arrayHash[a];
      fastKey = XXH3_64bits(k, n * sizeof(uint32_t));
      if (fprog && !fprog->failed && fprog->prog && fprog->gpu) {
        auto fit = g_fast.find(fastKey);
        if (fit != g_fast.end()) {
          auto it = g_rawCache.find(fit->second);
          if (it != g_rawCache.end() && it->second.vbo && it->second.nIdx == indexCount &&
              it->second.in == fprog->gpuIn) {
            it->second.lastFrame = g_gxFrame;
            ++g_cacheHits;
            ++g_fastHits;
            const uint64_t tDecoded = sceKernelGetProcessTimeWide();
            g_tDecode += tDecoded - tFast;
            gpu_submit(st, p, fprog, U, imm, &it->second, nullptr, nullptr, indexCount, it->second.count, tDecoded);
            return;
          }
          g_fast.erase(fit);
        }
      }
    }
  }

  // Indices, rebased to the vertex range they use.
  uint32_t first = 0, count = 0;
  g_indices.clear();
  if (indexed) {
    if (!st.indexBuffer || !st.indexBuffer->data || st.indexFormat != WGPUIndexFormat_Uint16) {
      ++g_skippedOther;
      return;
    }
    const uint64_t ioff = st.indexOffset + static_cast<uint64_t>(firstIndex) * 2;
    if (ioff + static_cast<uint64_t>(indexCount) * 2 > st.indexBuffer->size || indexCount == 0) {
      ++g_skippedOther;
      return;
    }
    const auto* idx = reinterpret_cast<const uint16_t*>(st.indexBuffer->data + ioff);
    uint32_t lo = 0xFFFFFFFF, hi = 0;
    for (uint32_t i = 0; i < indexCount; ++i) {
      const uint32_t v = idx[i];
      lo = v < lo ? v : lo;
      hi = v > hi ? v : hi;
    }
    first = static_cast<uint32_t>(static_cast<int32_t>(lo) + baseVertex);
    count = hi - lo + 1;
    g_indices.resize(indexCount);
    for (uint32_t i = 0; i < indexCount; ++i) g_indices[i] = static_cast<uint16_t>(idx[i] - lo);
  } else {
    first = firstVertex;
    count = vertexCount;
  }
  if (count == 0 || count > 65536) {
    ++g_skippedOther;
    return;
  }

  GxProgram* prog = program_for(p);
  if (!prog || prog->failed || !prog->prog) {
    ++g_skippedOther;
    return;
  }

  const uint64_t tStart = sceKernelGetProcessTimeWide();
  g_vertices += count;
  // ---- vertices
  const uint32_t attrs = prog->attrs;
  int offs[kAttrCount];
  int stride = 0;
  static const int kSize[kAttrCount] = {3, 3, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3};
  for (int a = 0; a < kAttrCount; ++a) {
    offs[a] = -1;
    if (attrs & (1u << a)) {
      offs[a] = stride;
      stride += kSize[a];
    }
  }
  const float* postex = L.postex >= 0 ? slot(L.postex) : nullptr;
  const float* nrmMtx = L.nrm >= 0 ? slot(L.nrm) : nullptr;
  const float* ptTex = L.ptTex >= 0 ? slot(L.ptTex) : nullptr;
  const auto& A = sc.attrs;

  // Readers set up once per draw: where each attribute's bytes are and how to decode them.
  Reader rPos, rNrm, rClr[2], rTex[8], rPn, rTm[8];
  setup_reader(rPos, f, A[GX_VA_POS], GX_VA_POS, 3, false);
  setup_reader(rNrm, f, A[GX_VA_NRM], GX_VA_NRM, 3, false);
  for (int c = 0; c < 2; ++c) setup_reader(rClr[c], f, A[GX_VA_CLR0 + c], GX_VA_CLR0 + c, 4, true);
  for (int t = 0; t < 8; ++t) setup_reader(rTex[t], f, A[GX_VA_TEX0 + t], GX_VA_TEX0 + t, 2, false);
  setup_u8_reader(rPn, f, A[GX_VA_PNMTXIDX]);
  for (int t = 0; t < 8; ++t) setup_u8_reader(rTm[t], f, A[GX_VA_TEX0MTXIDX + t]);

  if (prog->gpu) {
    // ---- GPU transforms: raw attributes in a GL buffer (decoded once), matrices
    // applied by the vertex shader.
    static const int kN[kRawCount] = {3, 3, 4, 4, 2, 2, 2, 2, 2, 2, 2, 2, 1};
    const uint32_t in = prog->gpuIn;
    int o[kRawCount];
    int rs = 0;
    for (int a = 0; a < kRawCount; ++a) {
      o[a] = -1;
      if (in & (1u << a)) o[a] = rs, rs += kN[a];
    }
    const uint32_t nIdx = indexed ? static_cast<uint32_t>(g_indices.size()) : 0;
    uint64_t key = 0;
    {
      // One-shot hashes on the stack: the layout and the arrays' hashes (Aurora took
      // them when it pushed each array: DrawImmediateData::arrayHash), then the
      // draw's own vertex bytes and indices.
      uint32_t meta[20];
      int nm = 0;
      meta[nm++] = static_cast<uint32_t>(rs);
      meta[nm++] = sc.vtxStride;
      meta[nm++] = count;
      meta[nm++] = in;
      meta[nm++] = nIdx;
      meta[nm++] = 0x67707531u;
      const uint64_t ah = XXH3_64bits(A.data(), sizeof(A));
      meta[nm++] = static_cast<uint32_t>(ah);
      meta[nm++] = static_cast<uint32_t>(ah >> 32);
      auto arr = [&](const Reader& r, int attr) {
        if (r.kind == 2 || r.kind == 3) meta[nm++] = imm.arrayHash[attr - GX_VA_POS];
      };
      arr(rPos, GX_VA_POS);
      if (o[kRawNrm] >= 0) arr(rNrm, GX_VA_NRM);
      for (int c = 0; c < 2; ++c)
        if (o[kRawClr0 + c] >= 0) arr(rClr[c], GX_VA_CLR0 + c);
      for (int t = 0; t < 8; ++t)
        if (o[kRawUv0 + t] >= 0) arr(rTex[t], GX_VA_TEX0 + t);
      key = XXH3_64bits(meta, nm * sizeof(uint32_t));
      const uint64_t vStart = static_cast<uint64_t>(imm.vtxStart) + static_cast<uint64_t>(first) * sc.vtxStride;
      const uint64_t vLen = static_cast<uint64_t>(count) * sc.vtxStride;
      if (vStart + vLen <= f.vbuf.size) key = XXH3_64bits_withSeed(f.vbuf.p + vStart, vLen, key);
      if (nIdx) key = XXH3_64bits_withSeed(g_indices.data(), nIdx * 2, key);
    }
    // Geometry gets GL buffers once it has been seen in an earlier frame too (room
    // geometry); one-off geometry (effects, particles, text) is drawn from CPU
    // memory, which vitaGL copies per draw: no buffer churn while shooting.
    RawEntry* e = nullptr;
    static std::vector<float> scratch;
    auto it = g_rawCache.find(key);
    if (it != g_rawCache.end() && it->second.vbo && it->second.count == count && it->second.nIdx == nIdx &&
        it->second.in == in) {
      e = &it->second;
      ++g_cacheHits;
    } else {
      ++g_cacheMisses;
      scratch.assign(static_cast<size_t>(count) * rs, 0.f);
      float* r = scratch.data();
      for (uint32_t n = 0; n < count; ++n, r += rs) {
        const uint32_t vidx = first + n;
        read_vec(rPos, vidx, r + o[kRawPos]);
        if (o[kRawNrm] >= 0) {
          r[o[kRawNrm]] = 1.f;
          read_vec(rNrm, vidx, r + o[kRawNrm]);
        }
        for (int c = 0; c < 2; ++c)
          if (o[kRawClr0 + c] >= 0) read_vec(rClr[c], vidx, r + o[kRawClr0 + c]);
        for (int t = 0; t < 8; ++t)
          if (o[kRawUv0 + t] >= 0) read_vec(rTex[t], vidx, r + o[kRawUv0 + t]);
        if (o[kRawPn] >= 0) r[o[kRawPn]] = static_cast<float>(read_u8(rPn, vidx));
      }
      auto seen = g_seen.find(key);
      if (seen != g_seen.end() && seen->second != g_gxFrame) {
        g_seen.erase(seen);
        if (it != g_rawCache.end()) {
          g_rawBytes -= it->second.bytes;
          if (it->second.vbo) glDeleteBuffers(1, &it->second.vbo);
          if (it->second.ibo) glDeleteBuffers(1, &it->second.ibo);
          g_rawCache.erase(it);
        }
        RawEntry ne;
        while (glGetError() != GL_NO_ERROR) {
        }
        glGenBuffers(1, &ne.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, ne.vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(scratch.size() * 4), scratch.data(), GL_STATIC_DRAW);
        if (nIdx) {
          glGenBuffers(1, &ne.ibo);
          glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ne.ibo);
          glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(nIdx * 2), g_indices.data(), GL_STATIC_DRAW);
          glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        if (glGetError() != GL_NO_ERROR) {
          // Out of GPU memory: drop the buffers and draw this one from CPU memory.
          static int fails = 0;
          if (++fails <= 5) log("gx: GL buffer allocation failed (%u bytes); drawing from CPU memory",
                                static_cast<unsigned>(scratch.size() * 4 + nIdx * 2));
          if (ne.vbo) glDeleteBuffers(1, &ne.vbo);
          if (ne.ibo) glDeleteBuffers(1, &ne.ibo);
        } else {
          ne.count = count;
          ne.nIdx = nIdx;
          ne.in = in;
          ne.bytes = scratch.size() * 4 + nIdx * 2;
          g_rawBytes += ne.bytes;
          e = &g_rawCache.emplace(key, std::move(ne)).first->second;
        }
      } else {
        g_seen[key] = g_gxFrame;
        if (g_seen.size() > 16384) {
          for (auto s2 = g_seen.begin(); s2 != g_seen.end();)
            s2 = g_gxFrame - s2->second > 30 ? g_seen.erase(s2) : std::next(s2);
        }
      }
    }
    if (e) {
      e->lastFrame = g_gxFrame;
      raw_cache_trim(); // never drops this frame's entries
      if (fastKey) {
        if (g_fast.size() > 32768) g_fast.clear();
        g_fast[fastKey] = key;
      }
    }
    const uint64_t tDecoded = sceKernelGetProcessTimeWide();
    g_tDecode += tDecoded - tStart;

    gpu_submit(st, p, prog, U, imm, e, scratch.data(), g_indices.data(), nIdx, count, tDecoded);
    return;
  }
  g_verts.resize(static_cast<size_t>(count) * stride);
  const bool needNrm = offs[kAttrNrm] >= 0;

  // Texgen plans for the sampled coordinates.
  struct TexPlan {
    int out = -1;        // float offset in the vertex
    int src = 0;         // 0 tex attribute, 1 position, 2 normal, 3 colour 0, 4 colour 1, 5 none, 6 emboss
    int texAttr = 0, emboss = 0;
    int mtx = -1;        // constant matrix index (postex), or -1
    bool perVertexMtx = false, normalize = false, mtx2x4 = true, keepQ = false;
    const float* post = nullptr;
    const float* mtxPtr = nullptr; // the constant matrix
  } plans[8];
  int nPlans = 0;
  for (int i = 0; i < 8; ++i) {
    if (!info.sampledTexCoords.test(i)) continue;
    const auto& tcg = sc.tcgs[i];
    TexPlan& tp = plans[nPlans++];
    tp.out = offs[kAttrTc0 + i];
    if (tcg.type >= GX_TG_BUMP0 && tcg.type <= GX_TG_BUMP7) {
      tp.src = 6;
      tp.emboss = tcg.embossSrc < 8 ? tcg.embossSrc : 0;
      continue;
    }
    if (tcg.src >= GX_TG_TEX0 && tcg.src <= GX_TG_TEX7) {
      tp.src = 0;
      tp.texAttr = tcg.src - GX_TG_TEX0;
    } else if (tcg.src == GX_TG_POS) {
      tp.src = 1;
    } else if (tcg.src == GX_TG_NRM || tcg.src == GX_TG_BINRM || tcg.src == GX_TG_TANGENT) {
      tp.src = 2;
    } else if (tcg.src == GX_TG_COLOR0 || tcg.src == GX_TG_COLOR1) {
      tp.src = tcg.src == GX_TG_COLOR1 ? 4 : 3;
    } else {
      tp.src = 5;
    }
    if (tcg.type == GX_TG_MTX2x4 || tcg.type == GX_TG_MTX3x4) {
      tp.mtx2x4 = tcg.type == GX_TG_MTX2x4;
      if (A[GX_VA_TEX0MTXIDX + i].attrType != GX_NONE)
        tp.perVertexMtx = true;
      else if (tcg.mtx != GX_IDENTITY && tcg.mtx / 3 < 20)
        tp.mtx = tcg.mtx / 3;
    }
    tp.keepQ = tcg.type == GX_TG_MTX3x4;
    tp.normalize = tcg.normalize;
    if (L.compact) {
      tp.post = L.postM[i] >= 0 ? slot(L.postM[i]) : nullptr;
      tp.mtxPtr = L.texM[i] >= 0 ? slot(L.texM[i]) : nullptr;
    } else {
      if (tcg.postMtx != GX_PTIDENTITY && ptTex) {
        const int pm = (tcg.postMtx - GX_PTTEXMTX0) / 3;
        if (pm >= 0 && pm < 20) tp.post = ptTex + pm * 12;
      }
      if (tp.mtx >= 0) tp.mtxPtr = postex + tp.mtx * 12;
    }
  }
  // texture-coordinate index of each plan (for emboss lookups)
  int planCoord[8], np = 0;
  for (int i = 0; i < 8; ++i)
    if (info.sampledTexCoords.test(i)) planCoord[np++] = i;

  const uint32_t fixedPn = imm.currentPnMtx > 9 ? 0 : imm.currentPnMtx;

  // ---- raw (pre-matrix) attributes: from the cache, or decoded now
  bool texUsed[8] = {}, tmUsed[8] = {};
  for (int k = 0; k < nPlans; ++k) {
    if (plans[k].src == 0) texUsed[plans[k].texAttr] = true;
    if (plans[k].perVertexMtx) tmUsed[planCoord[k]] = true;
  }
  const bool rawNrm = rNrm.kind != 0;
  int rs = 0;
  const int oPos = rs;
  rs += 3;
  const int oNrm = rawNrm ? rs : -1;
  if (rawNrm) rs += 3;
  int oClr[2] = {-1, -1}, oUv[8], oTm[8];
  for (int c = 0; c < 2; ++c)
    if (rClr[c].kind) oClr[c] = rs, rs += 4;
  for (int t = 0; t < 8; ++t) {
    oUv[t] = -1;
    if (texUsed[t] && rTex[t].kind) oUv[t] = rs, rs += 2;
  }
  const int oPn = rPn.kind ? rs : -1;
  if (rPn.kind) rs += 1;
  for (int t = 0; t < 8; ++t) {
    oTm[t] = -1;
    if (tmUsed[t] && rTm[t].kind) oTm[t] = rs, rs += 1;
  }

  // Key: the vertex bytes, every array range the indices reach, and the layout.
  uint64_t key = 0;
  {
    uint32_t meta[20];
    int nm = 0;
    meta[nm++] = static_cast<uint32_t>(rs);
    meta[nm++] = sc.vtxStride;
    meta[nm++] = count;
    meta[nm++] = static_cast<uint32_t>(attrs);
    const uint64_t ah = XXH3_64bits(A.data(), sizeof(A));
    meta[nm++] = static_cast<uint32_t>(ah);
    meta[nm++] = static_cast<uint32_t>(ah >> 32);
    auto arr = [&](const Reader& r, int attr) {
      if (r.kind == 2 || r.kind == 3) meta[nm++] = imm.arrayHash[attr - GX_VA_POS];
    };
    arr(rPos, GX_VA_POS);
    if (rawNrm) arr(rNrm, GX_VA_NRM);
    for (int c = 0; c < 2; ++c)
      if (oClr[c] >= 0) arr(rClr[c], GX_VA_CLR0 + c);
    for (int t = 0; t < 8; ++t)
      if (oUv[t] >= 0) arr(rTex[t], GX_VA_TEX0 + t);
    key = XXH3_64bits(meta, nm * sizeof(uint32_t));
    const uint64_t vStart = static_cast<uint64_t>(imm.vtxStart) + static_cast<uint64_t>(first) * sc.vtxStride;
    const uint64_t vLen = static_cast<uint64_t>(count) * sc.vtxStride;
    if (vStart + vLen <= f.vbuf.size) key = XXH3_64bits_withSeed(f.vbuf.p + vStart, vLen, key);
  }
  const float* raw = nullptr;
  auto cached = g_rawCache.find(key);
  if (cached != g_rawCache.end() && cached->second.data.size() == static_cast<size_t>(count) * rs) {
    cached->second.lastFrame = g_gxFrame;
    raw = cached->second.data.data();
    ++g_cacheHits;
  } else {
    ++g_cacheMisses;
    RawEntry& e = g_rawCache[key];
    g_rawBytes -= e.bytes;
    e.data.assign(static_cast<size_t>(count) * rs, 0.f);
    e.lastFrame = g_gxFrame;
    e.bytes = e.data.size() * sizeof(float);
    g_rawBytes += e.bytes;
    float* r = e.data.data();
    for (uint32_t n = 0; n < count; ++n, r += rs) {
      const uint32_t vidx = first + n;
      read_vec(rPos, vidx, r + oPos);
      if (oNrm >= 0) {
        r[oNrm] = 1.f;
        read_vec(rNrm, vidx, r + oNrm);
      }
      for (int c = 0; c < 2; ++c)
        if (oClr[c] >= 0) read_vec(rClr[c], vidx, r + oClr[c]);
      for (int t = 0; t < 8; ++t)
        if (oUv[t] >= 0) read_vec(rTex[t], vidx, r + oUv[t]);
      if (oPn >= 0) r[oPn] = static_cast<float>(read_u8(rPn, vidx));
      for (int t = 0; t < 8; ++t)
        if (oTm[t] >= 0) r[oTm[t]] = static_cast<float>(read_u8(rTm[t], vidx));
    }
    // The trim never drops entries used this frame (age >= 1), and erasing other
    // elements leaves `e` valid.
    raw_cache_trim();
    raw = e.data.data();
  }

  // ---- transforms (camera-dependent: every draw)
  float* out = g_verts.data();
  const float* rv = raw;
  for (uint32_t n = 0; n < count; ++n, out += stride, rv += rs) {
    uint32_t pn = fixedPn;
    if (oPn >= 0) {
      pn = static_cast<uint32_t>(rv[oPn]) / 3;
      if (pn > 9) pn = 0;
    }
    const float* pos = rv + oPos;
    static const float kDefNrm[3] = {1.f, 0.f, 0.f};
    const float* nrm = oNrm >= 0 ? rv + oNrm : kDefNrm;
    static const float kZeroClr[4] = {0.f, 0.f, 0.f, 0.f};
    const float* clr0 = oClr[0] >= 0 ? rv + oClr[0] : kZeroClr;
    const float* clr1 = oClr[1] >= 0 ? rv + oClr[1] : kZeroClr;

    const float* P = L.compact ? slot(L.posM) + (L.pnFull ? pn * 12 : 0) : postex + pn * 12;
    const float* N = L.compact ? slot(L.nrmM) + (L.pnFull ? pn * 12 : 0) : nrmMtx + pn * 12;
    mul34(P, pos, 1.f, out + offs[kAttrPos]);
    if (needNrm) {
      float* mn = out + offs[kAttrNrm];
      mul34(N, nrm, 0.f, mn);
      const float len2 = mn[0] * mn[0] + mn[1] * mn[1] + mn[2] * mn[2];
      if (len2 > 1e-10f) {
        const float inv = 1.f / __builtin_sqrtf(len2);
        mn[0] *= inv;
        mn[1] *= inv;
        mn[2] *= inv;
      }
    }
    if (offs[kAttrClr0] >= 0) std::memcpy(out + offs[kAttrClr0], clr0, 16);
    if (offs[kAttrClr1] >= 0) std::memcpy(out + offs[kAttrClr1], clr1, 16);

    for (int k = 0; k < nPlans; ++k) {
      const TexPlan& tp = plans[k];
      float* o = out + tp.out;
      if (tp.src == 6) {
        const int so = offs[kAttrTc0 + tp.emboss];
        if (so >= 0 && so < tp.out) {
          o[0] = out[so];
          o[1] = out[so + 1];
        } else {
          o[0] = o[1] = 0.f;
        }
        o[2] = 1.f;
        continue;
      }
      float tc[4] = {0, 0, 1, 1};
      switch (tp.src) {
      case 0:
        if (oUv[tp.texAttr] >= 0) {
          tc[0] = rv[oUv[tp.texAttr]];
          tc[1] = rv[oUv[tp.texAttr] + 1];
        }
        break;
      case 1:
        tc[0] = pos[0], tc[1] = pos[1], tc[2] = pos[2];
        break;
      case 2:
        tc[0] = nrm[0], tc[1] = nrm[1], tc[2] = nrm[2];
        break;
      case 3:
        std::memcpy(tc, clr0, 16);
        break;
      case 4:
        std::memcpy(tc, clr1, 16);
        break;
      default:
        break;
      }
      const float* mp = tp.mtxPtr;
      if (tp.perVertexMtx) {
        const int o2 = oTm[planCoord[k]];
        int m = o2 >= 0 ? static_cast<int>(rv[o2]) / 3 : -1;
        mp = m >= 0 && m < 20 ? postex + m * 12 : nullptr;
      }
      float tmp[3];
      if (mp) {
        mul34v4(mp, tc, tmp);
      } else {
        tmp[0] = tc[0], tmp[1] = tc[1], tmp[2] = tc[2];
      }
      if (tp.mtx2x4) tmp[2] = 1.f;
      if (tp.normalize) {
        const float l2 = tmp[0] * tmp[0] + tmp[1] * tmp[1] + tmp[2] * tmp[2];
        if (l2 > 0.f) {
          const float inv = 1.f / __builtin_sqrtf(l2);
          tmp[0] *= inv, tmp[1] *= inv, tmp[2] *= inv;
        }
      }
      if (tp.post) {
        mul34(tp.post, tmp, 1.f, o);
      } else {
        o[0] = tmp[0], o[1] = tmp[1], o[2] = tmp[2];
      }
      if (!tp.keepQ) o[2] = 1.f;
    }
  }

  const uint64_t tDecoded = sceKernelGetProcessTimeWide();
  g_tDecode += tDecoded - tStart;
  set_gx_state(st, prog, L, U);
  const uint64_t tState = sceKernelGetProcessTimeWide();
  g_tState += tState - tDecoded;

  // ---- draw
  for (int a = 0; a < kAttrCount; ++a) {
    if (offs[a] >= 0) {
      glEnableVertexAttribArray(a);
      glVertexAttribPointer(a, kSize[a], GL_FLOAT, GL_FALSE, stride * 4, g_verts.data() + offs[a]);
    } else {
      glDisableVertexAttribArray(a);
    }
  }
  if (indexed)
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(g_indices.size()), GL_UNSIGNED_SHORT, g_indices.data());
  else
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(count));
  for (int a = 0; a < kAttrCount; ++a)
    if (offs[a] >= 0) glDisableVertexAttribArray(a);
  g_tDraw += sceKernelGetProcessTimeWide() - tState;
  ++g_drawn;
}
} // namespace wv

// Called by Aurora's gx::build_pipeline (gx.cpp, __vita__ patch).
void wv_gx_note_pipeline_config(const aurora::gx::PipelineConfig& config) {
  delete wv::t_pending;
  auto* p = new wv::GxPipelineInfo;
  p->config = config;
  p->info = aurora::gx::build_shader_info(config.shaderConfig);
  wv::build_layout(*p);
  if (const char* why = wv::unsupported_reason(*p)) {
    p->unsupported = true;
    static int logged = 0;
    if (++logged <= 10) wv::log("gx: pipeline not drawn yet (%s)", why);
  }
  wv::t_pending = p;
}
