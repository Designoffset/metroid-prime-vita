// webgpu_vita: the WebGPU C API implemented on the PS Vita with vitaGL.
//
// Aurora (the port's GameCube graphics layer) records frames through WebGPU.
// This layer keeps that API: objects are plain C++ structs, command encoders
// record a list of commands, and wgpuQueueSubmit plays them back through
// vitaGL on the thread that submits (Aurora's render worker), which is also
// the only thread that ever touches GL.
//
// Shaders: Aurora's WGSL can't run on the Vita (the GameCube draws decode raw
// vertex data in the shader with storage buffers and integer ops). GameCube
// pipelines (gx::build_pipeline) hand us their gx::PipelineConfig instead; we
// decode their vertices on the CPU (wv_gx.cpp) and generate Cg shaders for
// them (wv_cg.cpp). Aurora's other pipelines are recognised by label and get
// hand-written Cg (wv_special.cpp); unknown ones are skipped and logged.
#pragma once

#include <dawn/webgpu.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wv {

enum class Kind : uint8_t {
  Generic,
  Instance,
  Adapter,
  Device,
  Queue,
  Buffer,
  Texture,
  TextureView,
  Sampler,
  BindGroupLayout,
  BindGroup,
  PipelineLayout,
  ShaderModule,
  RenderPipeline,
  ComputePipeline,
  CommandEncoder,
  RenderPassEncoder,
  ComputePassEncoder,
  CommandBuffer,
  Surface,
  QuerySet,
};

struct Obj {
  explicit Obj(Kind k) : kind(k) {}
  virtual ~Obj() = default;
  std::atomic<int> rc{1};
  Kind kind;
  std::string label;
};

void addref(const void* h);
void release(const void* h);
template <typename T>
T* as(const void* h) {
  return static_cast<T*>(const_cast<Obj*>(static_cast<const Obj*>(h)));
}

// A reference-holding pointer for objects kept by commands and bind groups.
template <typename T>
struct Ref {
  T* p = nullptr;
  Ref() = default;
  explicit Ref(T* x) : p(x) { addref(p); }
  Ref(const Ref& o) : p(o.p) { addref(p); }
  Ref(Ref&& o) noexcept : p(o.p) { o.p = nullptr; }
  Ref& operator=(Ref o) noexcept {
    std::swap(p, o.p);
    return *this;
  }
  ~Ref() { release(p); }
  T* operator->() const { return p; }
  T* get() const { return p; }
  explicit operator bool() const { return p != nullptr; }
};

struct Buffer : Obj {
  Buffer() : Obj(Kind::Buffer) {}
  ~Buffer() override { std::free(data); }
  uint8_t* data = nullptr; // every buffer lives in CPU memory
  uint64_t size = 0;
  uint64_t usage = 0;
  bool mapped = false;
  // Index buffers: a GL buffer object would need the data copied each frame anyway; we pass pointers.
};

struct Texture : Obj {
  Texture() : Obj(Kind::Texture) {}
  ~Texture() override;
  uint32_t width = 0, height = 0, layers = 1, mips = 1, samples = 1;
  WGPUTextureFormat format = WGPUTextureFormat_Undefined;
  WGPUTextureDimension dimension = WGPUTextureDimension_2D;
  uint64_t usage = 0;
  bool surface = false; // the swapchain image: GL's default framebuffer
  // GL objects (GL thread only). One GL texture per array layer (vitaGL has no 2D arrays).
  std::vector<uint32_t> gl;
  bool glAllocated = false;
};

struct TextureView : Obj {
  TextureView() : Obj(Kind::TextureView) {}
  Ref<Texture> tex;
  WGPUTextureFormat format = WGPUTextureFormat_Undefined;
  WGPUTextureViewDimension dimension = WGPUTextureViewDimension_2D;
  uint32_t baseMip = 0, mipCount = 1, baseLayer = 0, layerCount = 1;
};

struct Sampler : Obj {
  Sampler() : Obj(Kind::Sampler) {}
  WGPUAddressMode addressU = WGPUAddressMode_ClampToEdge, addressV = WGPUAddressMode_ClampToEdge;
  WGPUFilterMode mag = WGPUFilterMode_Nearest, min = WGPUFilterMode_Nearest;
  WGPUMipmapFilterMode mip = WGPUMipmapFilterMode_Nearest;
  float lodMin = 0.f, lodMax = 32.f;
  uint16_t maxAnisotropy = 1;
};

struct BindGroupLayout : Obj {
  BindGroupLayout() : Obj(Kind::BindGroupLayout) {}
};

struct BindEntry {
  uint32_t binding = 0;
  Ref<Buffer> buffer;
  uint64_t offset = 0, size = 0;
  Ref<Sampler> sampler;
  Ref<TextureView> view;
};

struct BindGroup : Obj {
  BindGroup() : Obj(Kind::BindGroup) {}
  std::vector<BindEntry> entries;
  const BindEntry* find(uint32_t binding) const {
    for (const auto& e : entries)
      if (e.binding == binding) return &e;
    return nullptr;
  }
};

struct PipelineLayout : Obj {
  PipelineLayout() : Obj(Kind::PipelineLayout) {}
};

struct ShaderModule : Obj {
  ShaderModule() : Obj(Kind::ShaderModule) {}
  std::string code;
};

struct GxPipelineInfo; // wv_gx.cpp
struct SpecialPipeline; // wv_special.cpp

struct BlendComp {
  WGPUBlendOperation op = WGPUBlendOperation_Add;
  WGPUBlendFactor src = WGPUBlendFactor_One, dst = WGPUBlendFactor_Zero;
};

struct RenderPipeline : Obj {
  RenderPipeline() : Obj(Kind::RenderPipeline) {}
  ~RenderPipeline() override;
  std::string vsEntry, fsEntry, moduleLabel;
  WGPUPrimitiveTopology topology = WGPUPrimitiveTopology_TriangleList;
  WGPUCullMode cull = WGPUCullMode_None;
  WGPUFrontFace frontFace = WGPUFrontFace_CCW;
  bool hasDepth = false, depthWrite = false;
  WGPUCompareFunction depthCompare = WGPUCompareFunction_Always;
  float depthBias = 0.f, depthBiasSlope = 0.f;
  bool hasFragment = false, blend = false;
  BlendComp color, alpha;
  uint32_t writeMask = WGPUColorWriteMask_All;
  WGPUTextureFormat colorFormat = WGPUTextureFormat_Undefined;
  // What runs it: a GameCube pipeline, a known Aurora pipeline, or nothing.
  GxPipelineInfo* gx = nullptr;
  SpecialPipeline* special = nullptr;
};

// ---------------------------------------------------------------- commands
enum class Op : uint8_t {
  BeginRenderPass,
  EndRenderPass,
  SetPipeline,
  SetBindGroup,
  SetVertexBuffer,
  SetIndexBuffer,
  SetViewport,
  SetScissor,
  SetBlendConstant,
  SetStencilRef,
  SetImmediates,
  Draw,
  DrawIndexed,
  CopyBufferToBuffer,
  CopyBufferToTexture,
  CopyTextureToTexture,
  CopyTextureToBuffer,
  ClearBuffer,
  WriteBuffer,
};

struct PassColor {
  Ref<TextureView> view, resolve;
  WGPULoadOp load = WGPULoadOp_Load;
  WGPUStoreOp store = WGPUStoreOp_Store;
  float clear[4] = {0, 0, 0, 0};
};
struct PassDepth {
  Ref<TextureView> view;
  WGPULoadOp depthLoad = WGPULoadOp_Load, stencilLoad = WGPULoadOp_Load;
  float depthClear = 1.f;
  uint32_t stencilClear = 0;
};

struct TexelLoc {
  Ref<Texture> tex;
  uint32_t mip = 0, x = 0, y = 0, z = 0;
};

struct Cmd {
  Op op;
  // Generic payload (meaning depends on op).
  uint32_t u[8] = {};
  float f[6] = {};
  uint64_t off = 0, off2 = 0, size = 0;
  Ref<Obj> obj;          // pipeline / bind group / buffer
  Ref<Buffer> buf2;      // second buffer for copies
  std::vector<uint32_t> dyn; // dynamic offsets / immediates data / write data
  std::shared_ptr<PassColor> color;
  std::shared_ptr<PassDepth> depth;
  TexelLoc src, dst;
};

struct CommandEncoder : Obj {
  CommandEncoder() : Obj(Kind::CommandEncoder) {}
  std::vector<Cmd> cmds;
};

struct RenderPassEncoder : Obj {
  RenderPassEncoder() : Obj(Kind::RenderPassEncoder) {}
  Ref<CommandEncoder> enc;
};

struct CommandBuffer : Obj {
  CommandBuffer() : Obj(Kind::CommandBuffer) {}
  std::vector<Cmd> cmds;
};

// ---------------------------------------------------------------- execution (wv_exec.cpp)
void execute(std::vector<Cmd>& cmds);
void present();
// Texture uploads that may arrive from any thread; applied on the GL thread before the next submit.
void queue_texture_write(Texture* tex, uint32_t mip, uint32_t x, uint32_t y, uint32_t layer, uint32_t w, uint32_t h,
                         const void* data, uint32_t bytesPerRow, uint32_t rowsPerImage, uint32_t layers);
bool gl_ready();
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Current pass state the draw handlers need.
struct DrawState {
  RenderPipeline* pipeline = nullptr;
  BindGroup* groups[4] = {};
  std::vector<uint32_t> dynOffsets[4];
  Buffer* vertexBuffers[8] = {};
  uint64_t vertexOffsets[8] = {};
  Buffer* indexBuffer = nullptr;
  uint64_t indexOffset = 0;
  WGPUIndexFormat indexFormat = WGPUIndexFormat_Uint16;
  uint8_t immediates[128] = {};
  uint32_t targetWidth = 0, targetHeight = 0;
  WGPUTextureFormat targetFormat = WGPUTextureFormat_Undefined;
  bool targetIsSurface = false;
  bool targetHasDepth = false;
  float blendConstant[4] = {0, 0, 0, 0};
};

// GL helpers shared by the GX and special paths (wv_exec.cpp).
uint32_t gl_texture_for(TextureView* view);
void apply_sampler(Sampler* s, bool hasMips);
void apply_pipeline_state(const RenderPipeline& p, const DrawState& st);

// GX pipelines (wv_gx.cpp)
GxPipelineInfo* gx_take_pending_pipeline(); // set by gx::build_pipeline just before CreateRenderPipeline
void gx_free(GxPipelineInfo* info);
void gx_draw(DrawState& st, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, bool indexed,
             uint32_t indexCount, uint32_t firstIndex, int32_t baseVertex);

// Known Aurora pipelines (wv_special.cpp)
SpecialPipeline* special_for(const RenderPipeline& p);
void special_draw(DrawState& st, uint32_t vertexCount, uint32_t firstVertex);
} // namespace wv
