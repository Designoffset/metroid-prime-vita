// webgpu_vita: shared declarations of the GX draw path (wv_gx.cpp) and its Cg
// generator (wv_cg.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gx/gx.hpp"
#include "gx/pipeline.hpp"

namespace wv {

// Vertex attributes the generated vertex shaders take (fixed GL locations).
enum : int {
  kAttrPos = 0, // view-space position (CPU: position matrix applied)
  kAttrNrm,     // view-space normal
  kAttrClr0,
  kAttrClr1,
  kAttrTc0, // generated texture coordinates 0..7 (s, t, q)
  kAttrCount = kAttrTc0 + 8,
};

// GPU-transform programs read the raw (model-space) attributes instead, at these
// locations: the matrices are applied in the vertex shader.
enum : int {
  kRawPos = 0, // float3
  kRawNrm,     // float3
  kRawClr0,    // float4
  kRawClr1,    // float4
  kRawUv0,     // float2 x 8
  kRawPn = kRawUv0 + 8, // float: the PNMTXIDX byte
  kRawCount,
};

// Where each part of Aurora's per-draw uniform block is, in vec4 slots (-1: absent).
struct GxLayout {
  int proj = -1, postex = -1, nrm = -1, lights = -1, lightState = -1, ptTex = -1, fog = -1;
  int tevReg[4] = {-1, -1, -1, -1};
  int kcolor[4] = {-1, -1, -1, -1};
  int chanAmb[4] = {-1, -1, -1, -1}; // COLOR0, COLOR1, ALPHA0, ALPHA1
  int chanMat[4] = {-1, -1, -1, -1};
  int sizeBias[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  int tcScale = -1, indMtx = -1; // texcoord scales (8), indirect matrices (6)
  // The Vita's compact matrix block (ShaderInfo::vitaCompact): position and normal
  // matrices (10 each when pnFull, else the draw's one), one matrix and one post matrix
  // per texcoord that uses them. postex/nrm/ptTex are -1 then.
  bool compact = false, pnFull = false;
  int posM = -1, nrmM = -1;
  int texM[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  int postM[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  int slots = 0;
};

struct GxProgram {
  unsigned prog = 0;
  bool failed = false;
  uint32_t attrs = 0; // bit per kAttr*
  bool gpu = false;    // matrices applied in the vertex shader (raw inputs, kRaw*)
  bool gpuPn = false;  // per-vertex position matrix index
  uint32_t gpuIn = 0;  // bit per kRaw*
  int uPosM = -1, uNrmM = -1, uTexM = -1, uPost = -1;
  int uTcS = -1, uIndM = -1, uSzB = -1; // indirect texturing
  int texRows = 0, postRows = 0;
  int uProj = -1, uLights = -1, uLightEn = -1, uChan = -1, uTev = -1, uKc = -1, uFog = -1;
  int uTex[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  // Last values uploaded per uniform (GL keeps them per program): unchanged ones are skipped.
  std::vector<float> last[16];
};

struct GxPipelineInfo {
  aurora::gx::PipelineConfig config;
  aurora::gx::ShaderInfo info;
  GxLayout layout;
  bool unsupported = false;
  GxProgram* program = nullptr; // shared between pipelines with the same ShaderConfig
  uint64_t attrsHash = 0; // of config.shaderConfig.attrs (the resident fast path's key)
  bool attrsHashed = false;
};

struct CgSources {
  std::string vs, fs;
  uint32_t attrs = 0;
  bool gpu = false, gpuPn = false;
  uint32_t gpuIn = 0;
  int texRows = 0, postRows = 0; // uTexM / uPost sizes (vec4 rows)
};
// gpu: generate the GPU-transform vertex shader when the config allows it (no
// emboss, no per-vertex texture matrices); `gpu` in the result says whether it did.
CgSources generate_cg(const aurora::gx::ShaderConfig& config, const aurora::gx::ShaderInfo& info, bool gpu);

// Draw counts since the last call (for the frame log).
void gx_frame_stats(uint32_t& drawn, uint32_t& unsupported, uint32_t& other);
void gx_end_frame();
} // namespace wv
