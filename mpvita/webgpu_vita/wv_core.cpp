// webgpu_vita: object creation, async plumbing and command recording.
// Playback through vitaGL is in wv_exec.cpp.
#include "wv.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>

namespace wv {

void addref(const void* h) {
  if (h) as<Obj>(h)->rc.fetch_add(1, std::memory_order_relaxed);
}

void release(const void* h) {
  if (!h) return;
  auto* o = as<Obj>(h);
  if (o->rc.fetch_sub(1, std::memory_order_acq_rel) == 1) delete o;
}

void log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::fputs("wv: ", stderr);
  std::vfprintf(stderr, fmt, ap);
  std::fputc('\n', stderr);
  va_end(ap);
}

static std::string str(WGPUStringView s) {
  if (!s.data) return {};
  return s.length == WGPU_STRLEN ? std::string(s.data) : std::string(s.data, s.length);
}

// ---- deferred callbacks (futures) -------------------------------------------
struct Pending {
  uint64_t id;
  WGPUCallbackMode mode;
  std::function<void()> fn;
};
static std::mutex g_pendingMutex;
static std::deque<Pending> g_pending;
static uint64_t g_nextFuture = 1;

static WGPUFuture defer(WGPUCallbackMode mode, std::function<void()> fn) {
  std::lock_guard lk(g_pendingMutex);
  const uint64_t id = g_nextFuture++;
  g_pending.push_back({id, mode, std::move(fn)});
  return WGPUFuture{id};
}

template <typename Pred>
static void run_where(Pred pred) {
  std::vector<std::function<void()>> torun;
  {
    std::lock_guard lk(g_pendingMutex);
    for (auto it = g_pending.begin(); it != g_pending.end();) {
      if (pred(*it)) {
        torun.push_back(std::move(it->fn));
        it = g_pending.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& f : torun) f();
}

static void flush_spontaneous() {
  run_where([](const Pending& p) { return p.mode == WGPUCallbackMode_AllowSpontaneous; });
}

struct Device : Obj {
  Device() : Obj(Kind::Device) {}
  Obj* queue = nullptr;
  Obj* adapter = nullptr;
};

Texture::~Texture() {
  // GL objects are freed on the GL thread (wv_exec.cpp keeps a free list).
  extern void gl_defer_delete_textures(std::vector<uint32_t>&& names);
  if (!gl.empty()) gl_defer_delete_textures(std::move(gl));
}

RenderPipeline::~RenderPipeline() {
  if (gx) gx_free(gx);
}
} // namespace wv

using namespace wv;

static WGPUStringView sv(const char* s) { return WGPUStringView{s, std::strlen(s)}; }

static void fill_limits(WGPULimits* l) {
  if (!l) return;
  WGPUChainedStruct* chain = l->nextInChain;
  std::memset(l, 0, sizeof(*l));
  l->nextInChain = chain;
  l->maxTextureDimension1D = 4096;
  l->maxTextureDimension2D = 4096;
  l->maxTextureDimension3D = 256;
  l->maxTextureArrayLayers = 256;
  l->maxBindGroups = 4;
  l->maxBindGroupsPlusVertexBuffers = 24;
  l->maxBindingsPerBindGroup = 1000;
  l->maxDynamicUniformBuffersPerPipelineLayout = 8;
  l->maxDynamicStorageBuffersPerPipelineLayout = 4;
  l->maxSampledTexturesPerShaderStage = 16;
  l->maxSamplersPerShaderStage = 16;
  l->maxStorageBuffersPerShaderStage = 8;
  l->maxStorageTexturesPerShaderStage = 4;
  l->maxUniformBuffersPerShaderStage = 12;
  l->maxUniformBufferBindingSize = 65536;
  l->maxStorageBufferBindingSize = 64u << 20;
  l->minUniformBufferOffsetAlignment = 256;
  l->minStorageBufferOffsetAlignment = 256;
  l->maxVertexBuffers = 8;
  l->maxBufferSize = 128u << 20;
  l->maxVertexAttributes = 16;
  l->maxVertexBufferArrayStride = 2048;
  l->maxInterStageShaderVariables = 16;
  l->maxColorAttachments = 8;
  l->maxColorAttachmentBytesPerSample = 32;
  l->maxComputeWorkgroupStorageSize = 16384;
  l->maxComputeInvocationsPerWorkgroup = 256;
  l->maxComputeWorkgroupSizeX = 256;
  l->maxComputeWorkgroupSizeY = 256;
  l->maxComputeWorkgroupSizeZ = 64;
  l->maxComputeWorkgroupsPerDimension = 65535;
}

static Cmd& push(CommandEncoder* e, Op op) {
  e->cmds.emplace_back();
  e->cmds.back().op = op;
  return e->cmds.back();
}
static CommandEncoder* enc_of(WGPURenderPassEncoder p) { return as<RenderPassEncoder>(p)->enc.get(); }

extern "C" {

// ---- instance / adapter / device ---------------------------------------------
WGPUInstance wgpuCreateInstance(WGPUInstanceDescriptor const*) { return (WGPUInstance) new Obj(Kind::Instance); }

void wgpuGetInstanceFeatures(WGPUSupportedInstanceFeatures* features) {
  static const WGPUInstanceFeatureName k[] = {WGPUInstanceFeatureName_TimedWaitAny};
  if (features) {
    features->featureCount = 1;
    features->features = k;
  }
}
WGPUBool wgpuHasInstanceFeature(WGPUInstanceFeatureName f) { return f == WGPUInstanceFeatureName_TimedWaitAny; }
WGPUStatus wgpuGetInstanceLimits(WGPUInstanceLimits* limits) {
  if (limits) limits->timedWaitAnyMaxCount = 64;
  return WGPUStatus_Success;
}

WGPUFuture wgpuInstanceRequestAdapter(WGPUInstance, WGPURequestAdapterOptions const*, WGPURequestAdapterCallbackInfo ci) {
  auto* adapter = (WGPUAdapter) new Obj(Kind::Adapter);
  return defer(ci.mode, [ci, adapter] {
    if (ci.callback) ci.callback(WGPURequestAdapterStatus_Success, adapter, sv(""), ci.userdata1, ci.userdata2);
  });
}

WGPUFuture wgpuAdapterRequestDevice(WGPUAdapter adapter, WGPUDeviceDescriptor const*, WGPURequestDeviceCallbackInfo ci) {
  auto* dev = new Device;
  dev->queue = new Obj(Kind::Queue);
  addref(adapter);
  dev->adapter = as<Obj>(adapter);
  return defer(ci.mode, [ci, dev] {
    if (ci.callback) ci.callback(WGPURequestDeviceStatus_Success, (WGPUDevice)dev, sv(""), ci.userdata1, ci.userdata2);
  });
}

WGPUWaitStatus wgpuInstanceWaitAny(WGPUInstance, size_t count, WGPUFutureWaitInfo* futures, uint64_t) {
  if (count == 0 || futures == nullptr) return WGPUWaitStatus_Success;
  run_where([&](const Pending& p) {
    for (size_t i = 0; i < count; ++i)
      if (futures[i].future.id == p.id) return true;
    return false;
  });
  for (size_t i = 0; i < count; ++i) futures[i].completed = true;
  return WGPUWaitStatus_Success;
}

void wgpuInstanceProcessEvents(WGPUInstance) {
  run_where([](const Pending& p) { return p.mode != WGPUCallbackMode_WaitAnyOnly; });
}
void wgpuDeviceTick(WGPUDevice) { flush_spontaneous(); }

WGPUStatus wgpuAdapterGetInfo(WGPUAdapter, WGPUAdapterInfo* info) {
  if (!info) return WGPUStatus_Error;
  WGPUChainedStruct* chain = info->nextInChain;
  std::memset(info, 0, sizeof(*info));
  info->nextInChain = chain;
  info->vendor = sv("Sony");
  info->architecture = sv("SGX543MP4+");
  info->device = sv("PlayStation Vita (vitaGL)");
  info->description = sv("webgpu_vita");
  // Null keeps Aurora's desktop-only paths (driver quirks, timestamp queries) switched off.
  info->backendType = WGPUBackendType_Null;
  info->adapterType = WGPUAdapterType_IntegratedGPU;
  return WGPUStatus_Success;
}
void wgpuAdapterInfoFreeMembers(WGPUAdapterInfo) {}

WGPUStatus wgpuAdapterGetLimits(WGPUAdapter, WGPULimits* limits) {
  fill_limits(limits);
  return WGPUStatus_Success;
}
WGPUStatus wgpuDeviceGetLimits(WGPUDevice, WGPULimits* limits) {
  fill_limits(limits);
  return WGPUStatus_Success;
}
void wgpuAdapterGetFeatures(WGPUAdapter, WGPUSupportedFeatures* f) {
  if (f) {
    f->featureCount = 0;
    f->features = nullptr;
  }
}
void wgpuDeviceGetFeatures(WGPUDevice, WGPUSupportedFeatures* f) {
  if (f) {
    f->featureCount = 0;
    f->features = nullptr;
  }
}
void wgpuSupportedFeaturesFreeMembers(WGPUSupportedFeatures) {}
WGPUBool wgpuAdapterHasFeature(WGPUAdapter, WGPUFeatureName) { return false; }
WGPUBool wgpuDeviceHasFeature(WGPUDevice, WGPUFeatureName) { return false; }

WGPUQueue wgpuDeviceGetQueue(WGPUDevice device) {
  auto* q = as<Device>(device)->queue;
  addref(q);
  return (WGPUQueue)q;
}
WGPUAdapter wgpuDeviceGetAdapter(WGPUDevice device) {
  auto* a = as<Device>(device)->adapter;
  addref(a);
  return (WGPUAdapter)a;
}

// ---- buffers -------------------------------------------------------------------
WGPUBuffer wgpuDeviceCreateBuffer(WGPUDevice, WGPUBufferDescriptor const* d) {
  auto* b = new Buffer;
  b->size = d ? d->size : 0;
  b->usage = d ? d->usage : 0;
  b->label = d ? str(d->label) : "";
  // Everything is CPU memory: the GX path reads vertex, index, uniform and storage data directly.
  b->data = static_cast<uint8_t*>(std::calloc(1, b->size ? static_cast<size_t>(b->size) : 1));
  b->mapped = d && d->mappedAtCreation;
  return (WGPUBuffer)b;
}
void* wgpuBufferGetMappedRange(WGPUBuffer buffer, size_t offset, size_t) {
  auto* b = as<Buffer>(buffer);
  return b && b->data ? b->data + offset : nullptr;
}
void const* wgpuBufferGetConstMappedRange(WGPUBuffer buffer, size_t offset, size_t size) {
  return wgpuBufferGetMappedRange(buffer, offset, size);
}
WGPUStatus wgpuBufferWriteMappedRange(WGPUBuffer buffer, size_t offset, void const* data, size_t size) {
  auto* b = as<Buffer>(buffer);
  if (!b || !b->data || offset + size > b->size) return WGPUStatus_Error;
  std::memcpy(b->data + offset, data, size);
  return WGPUStatus_Success;
}
WGPUStatus wgpuBufferReadMappedRange(WGPUBuffer buffer, size_t offset, void* data, size_t size) {
  auto* b = as<Buffer>(buffer);
  if (!b || !b->data || offset + size > b->size) return WGPUStatus_Error;
  std::memcpy(data, b->data + offset, size);
  return WGPUStatus_Success;
}
WGPUFuture wgpuBufferMapAsync(WGPUBuffer buffer, WGPUMapMode, size_t, size_t, WGPUBufferMapCallbackInfo ci) {
  auto* b = as<Buffer>(buffer);
  addref(b);
  return defer(ci.mode, [ci, b] {
    b->mapped = true;
    if (ci.callback) ci.callback(WGPUMapAsyncStatus_Success, sv(""), ci.userdata1, ci.userdata2);
    release(b);
  });
}
void wgpuBufferUnmap(WGPUBuffer buffer) {
  if (buffer) as<Buffer>(buffer)->mapped = false;
}
void wgpuBufferDestroy(WGPUBuffer) {}
WGPUBufferMapState wgpuBufferGetMapState(WGPUBuffer buffer) {
  return buffer && as<Buffer>(buffer)->mapped ? WGPUBufferMapState_Mapped : WGPUBufferMapState_Unmapped;
}
uint64_t wgpuBufferGetSize(WGPUBuffer buffer) { return buffer ? as<Buffer>(buffer)->size : 0; }
WGPUBufferUsage wgpuBufferGetUsage(WGPUBuffer buffer) { return buffer ? as<Buffer>(buffer)->usage : 0; }
void wgpuQueueWriteBuffer(WGPUQueue, WGPUBuffer buffer, uint64_t offset, void const* data, size_t size) {
  auto* b = as<Buffer>(buffer);
  if (b && b->data && data && offset + size <= b->size) std::memcpy(b->data + offset, data, size);
}

// ---- textures ------------------------------------------------------------------
WGPUTexture wgpuDeviceCreateTexture(WGPUDevice, WGPUTextureDescriptor const* d) {
  auto* t = new Texture;
  if (d) {
    t->label = str(d->label);
    t->width = d->size.width;
    t->height = d->size.height;
    t->layers = d->size.depthOrArrayLayers ? d->size.depthOrArrayLayers : 1;
    t->mips = d->mipLevelCount ? d->mipLevelCount : 1;
    t->samples = d->sampleCount ? d->sampleCount : 1;
    t->format = d->format;
    t->dimension = d->dimension;
    t->usage = d->usage;
  }
  return (WGPUTexture)t;
}
uint32_t wgpuTextureGetWidth(WGPUTexture t) { return as<Texture>(t)->width; }
uint32_t wgpuTextureGetHeight(WGPUTexture t) { return as<Texture>(t)->height; }
uint32_t wgpuTextureGetDepthOrArrayLayers(WGPUTexture t) { return as<Texture>(t)->layers; }
uint32_t wgpuTextureGetMipLevelCount(WGPUTexture t) { return as<Texture>(t)->mips; }
uint32_t wgpuTextureGetSampleCount(WGPUTexture t) { return as<Texture>(t)->samples; }
WGPUTextureFormat wgpuTextureGetFormat(WGPUTexture t) { return as<Texture>(t)->format; }
WGPUTextureDimension wgpuTextureGetDimension(WGPUTexture t) { return as<Texture>(t)->dimension; }
WGPUTextureUsage wgpuTextureGetUsage(WGPUTexture t) { return as<Texture>(t)->usage; }
void wgpuTextureDestroy(WGPUTexture) {}

WGPUTextureView wgpuTextureCreateView(WGPUTexture t, WGPUTextureViewDescriptor const* d) {
  auto* tex = as<Texture>(t);
  auto* v = new TextureView;
  v->tex = Ref<Texture>(tex);
  v->format = tex->format;
  v->dimension = tex->layers > 1 ? WGPUTextureViewDimension_2DArray : WGPUTextureViewDimension_2D;
  v->mipCount = tex->mips;
  v->layerCount = tex->layers;
  if (d) {
    v->label = str(d->label);
    if (d->format != WGPUTextureFormat_Undefined) v->format = d->format;
    if (d->dimension != WGPUTextureViewDimension_Undefined) v->dimension = d->dimension;
    v->baseMip = d->baseMipLevel;
    v->mipCount = d->mipLevelCount == WGPU_MIP_LEVEL_COUNT_UNDEFINED ? tex->mips - d->baseMipLevel : d->mipLevelCount;
    v->baseLayer = d->baseArrayLayer;
    v->layerCount =
        d->arrayLayerCount == WGPU_ARRAY_LAYER_COUNT_UNDEFINED ? tex->layers - d->baseArrayLayer : d->arrayLayerCount;
  }
  return (WGPUTextureView)v;
}

void wgpuQueueWriteTexture(WGPUQueue, WGPUTexelCopyTextureInfo const* dst, void const* data, size_t,
                           WGPUTexelCopyBufferLayout const* layout, WGPUExtent3D const* size) {
  if (!dst || !data || !layout || !size) return;
  wv::queue_texture_write(as<Texture>(dst->texture), dst->mipLevel, dst->origin.x, dst->origin.y, dst->origin.z,
                          size->width, size->height, static_cast<const uint8_t*>(data) + layout->offset,
                          layout->bytesPerRow, layout->rowsPerImage, size->depthOrArrayLayers);
}

// ---- samplers, bind groups, layouts, modules ------------------------------------
WGPUSampler wgpuDeviceCreateSampler(WGPUDevice, WGPUSamplerDescriptor const* d) {
  auto* s = new Sampler;
  if (d) {
    s->label = str(d->label);
    s->addressU = d->addressModeU;
    s->addressV = d->addressModeV;
    s->mag = d->magFilter;
    s->min = d->minFilter;
    s->mip = d->mipmapFilter;
    s->lodMin = d->lodMinClamp;
    s->lodMax = d->lodMaxClamp;
    s->maxAnisotropy = d->maxAnisotropy;
  }
  return (WGPUSampler)s;
}

WGPUBindGroupLayout wgpuDeviceCreateBindGroupLayout(WGPUDevice, WGPUBindGroupLayoutDescriptor const*) {
  return (WGPUBindGroupLayout) new BindGroupLayout;
}
WGPUPipelineLayout wgpuDeviceCreatePipelineLayout(WGPUDevice, WGPUPipelineLayoutDescriptor const*) {
  return (WGPUPipelineLayout) new PipelineLayout;
}
WGPUBindGroupLayout wgpuRenderPipelineGetBindGroupLayout(WGPURenderPipeline, uint32_t) {
  return (WGPUBindGroupLayout) new BindGroupLayout;
}

WGPUBindGroup wgpuDeviceCreateBindGroup(WGPUDevice, WGPUBindGroupDescriptor const* d) {
  auto* g = new BindGroup;
  if (d) {
    g->label = str(d->label);
    g->entries.reserve(d->entryCount);
    for (size_t i = 0; i < d->entryCount; ++i) {
      const auto& e = d->entries[i];
      BindEntry be;
      be.binding = e.binding;
      if (e.buffer) be.buffer = Ref<Buffer>(as<Buffer>(e.buffer));
      be.offset = e.offset;
      be.size = e.size;
      if (e.sampler) be.sampler = Ref<Sampler>(as<Sampler>(e.sampler));
      if (e.textureView) be.view = Ref<TextureView>(as<TextureView>(e.textureView));
      g->entries.push_back(std::move(be));
    }
  }
  return (WGPUBindGroup)g;
}

WGPUShaderModule wgpuDeviceCreateShaderModule(WGPUDevice, WGPUShaderModuleDescriptor const* d) {
  auto* m = new ShaderModule;
  if (d) {
    m->label = str(d->label);
    for (const WGPUChainedStruct* c = d->nextInChain; c; c = c->next)
      if (c->sType == WGPUSType_ShaderSourceWGSL)
        m->code = str(reinterpret_cast<const WGPUShaderSourceWGSL*>(c)->code);
  }
  return (WGPUShaderModule)m;
}
WGPUShaderModule wgpuDeviceCreateErrorShaderModule(WGPUDevice, WGPUShaderModuleDescriptor const*, WGPUStringView) {
  return (WGPUShaderModule) new ShaderModule;
}

static RenderPipeline* make_render_pipeline(WGPURenderPipelineDescriptor const* d) {
  auto* p = new RenderPipeline;
  p->gx = wv::gx_take_pending_pipeline();
  if (!d) return p;
  p->label = str(d->label);
  p->vsEntry = str(d->vertex.entryPoint);
  if (d->vertex.module) p->moduleLabel = as<ShaderModule>(d->vertex.module)->label;
  p->topology = d->primitive.topology;
  p->cull = d->primitive.cullMode;
  p->frontFace = d->primitive.frontFace == WGPUFrontFace_Undefined ? WGPUFrontFace_CCW : d->primitive.frontFace;
  if (d->depthStencil) {
    p->hasDepth = true;
    p->depthWrite = d->depthStencil->depthWriteEnabled == WGPUOptionalBool_True;
    p->depthCompare = d->depthStencil->depthCompare;
    p->depthBias = static_cast<float>(d->depthStencil->depthBias);
    p->depthBiasSlope = d->depthStencil->depthBiasSlopeScale;
  }
  if (d->fragment) {
    p->hasFragment = true;
    p->fsEntry = str(d->fragment->entryPoint);
    if (d->fragment->targetCount > 0) {
      const auto& t = d->fragment->targets[0];
      p->colorFormat = t.format;
      p->writeMask = t.writeMask;
      if (t.blend) {
        p->blend = true;
        p->color = {t.blend->color.operation, t.blend->color.srcFactor, t.blend->color.dstFactor};
        p->alpha = {t.blend->alpha.operation, t.blend->alpha.srcFactor, t.blend->alpha.dstFactor};
      }
    }
  }
  if (!p->gx) {
    p->special = wv::special_for(*p);
    if (!p->special) wv::log("pipeline '%s' (%s/%s) has no Vita version; its draws are skipped", p->label.c_str(),
                             p->vsEntry.c_str(), p->fsEntry.c_str());
  }
  return p;
}
WGPURenderPipeline wgpuDeviceCreateRenderPipeline(WGPUDevice, WGPURenderPipelineDescriptor const* d) {
  return (WGPURenderPipeline)make_render_pipeline(d);
}
WGPUFuture wgpuDeviceCreateRenderPipelineAsync(WGPUDevice, WGPURenderPipelineDescriptor const* d,
                                               WGPUCreateRenderPipelineAsyncCallbackInfo ci) {
  auto* p = (WGPURenderPipeline)make_render_pipeline(d);
  return defer(ci.mode, [ci, p] {
    if (ci.callback) ci.callback(WGPUCreatePipelineAsyncStatus_Success, p, sv(""), ci.userdata1, ci.userdata2);
  });
}
WGPUFuture wgpuDeviceCreateComputePipelineAsync(WGPUDevice, WGPUComputePipelineDescriptor const*,
                                                WGPUCreateComputePipelineAsyncCallbackInfo ci) {
  auto* p = (WGPUComputePipeline) new Obj(Kind::ComputePipeline);
  return defer(ci.mode, [ci, p] {
    if (ci.callback) ci.callback(WGPUCreatePipelineAsyncStatus_Success, p, sv(""), ci.userdata1, ci.userdata2);
  });
}
WGPUFuture wgpuShaderModuleGetCompilationInfo(WGPUShaderModule, WGPUCompilationInfoCallbackInfo ci) {
  return defer(ci.mode, [ci] {
    WGPUCompilationInfo info{};
    if (ci.callback) ci.callback(WGPUCompilationInfoRequestStatus_Success, &info, ci.userdata1, ci.userdata2);
  });
}

// ---- command encoding ------------------------------------------------------------
WGPUCommandEncoder wgpuDeviceCreateCommandEncoder(WGPUDevice, WGPUCommandEncoderDescriptor const*) {
  return (WGPUCommandEncoder) new CommandEncoder;
}

WGPURenderPassEncoder wgpuCommandEncoderBeginRenderPass(WGPUCommandEncoder e, WGPURenderPassDescriptor const* d) {
  auto* enc = as<CommandEncoder>(e);
  auto& c = push(enc, Op::BeginRenderPass);
  if (d && d->colorAttachmentCount > 0 && d->colorAttachments[0].view) {
    const auto& a = d->colorAttachments[0];
    c.color = std::make_shared<PassColor>();
    c.color->view = Ref<TextureView>(as<TextureView>(a.view));
    if (a.resolveTarget) c.color->resolve = Ref<TextureView>(as<TextureView>(a.resolveTarget));
    c.color->load = a.loadOp;
    c.color->store = a.storeOp;
    c.color->clear[0] = static_cast<float>(a.clearValue.r);
    c.color->clear[1] = static_cast<float>(a.clearValue.g);
    c.color->clear[2] = static_cast<float>(a.clearValue.b);
    c.color->clear[3] = static_cast<float>(a.clearValue.a);
  }
  if (d && d->depthStencilAttachment && d->depthStencilAttachment->view) {
    const auto& a = *d->depthStencilAttachment;
    c.depth = std::make_shared<PassDepth>();
    c.depth->view = Ref<TextureView>(as<TextureView>(a.view));
    c.depth->depthLoad = a.depthLoadOp;
    c.depth->stencilLoad = a.stencilLoadOp;
    c.depth->depthClear = a.depthClearValue;
    c.depth->stencilClear = a.stencilClearValue;
  }
  auto* p = new RenderPassEncoder;
  p->enc = Ref<CommandEncoder>(enc);
  return (WGPURenderPassEncoder)p;
}
void wgpuRenderPassEncoderEnd(WGPURenderPassEncoder p) { push(enc_of(p), Op::EndRenderPass); }

void wgpuRenderPassEncoderSetPipeline(WGPURenderPassEncoder p, WGPURenderPipeline pipeline) {
  auto& c = push(enc_of(p), Op::SetPipeline);
  c.obj = Ref<Obj>(as<Obj>(pipeline));
}
void wgpuRenderPassEncoderSetBindGroup(WGPURenderPassEncoder p, uint32_t index, WGPUBindGroup group, size_t count,
                                       uint32_t const* offsets) {
  auto& c = push(enc_of(p), Op::SetBindGroup);
  c.u[0] = index;
  if (group) c.obj = Ref<Obj>(as<Obj>(group));
  if (count && offsets) c.dyn.assign(offsets, offsets + count);
}
void wgpuRenderPassEncoderSetVertexBuffer(WGPURenderPassEncoder p, uint32_t slot, WGPUBuffer buffer, uint64_t offset,
                                          uint64_t size) {
  auto& c = push(enc_of(p), Op::SetVertexBuffer);
  c.u[0] = slot;
  if (buffer) c.obj = Ref<Obj>(as<Obj>(buffer));
  c.off = offset;
  c.size = size;
}
void wgpuRenderPassEncoderSetIndexBuffer(WGPURenderPassEncoder p, WGPUBuffer buffer, WGPUIndexFormat format,
                                         uint64_t offset, uint64_t size) {
  auto& c = push(enc_of(p), Op::SetIndexBuffer);
  c.u[0] = format;
  if (buffer) c.obj = Ref<Obj>(as<Obj>(buffer));
  c.off = offset;
  c.size = size;
}
void wgpuRenderPassEncoderSetViewport(WGPURenderPassEncoder p, float x, float y, float w, float h, float mn, float mx) {
  auto& c = push(enc_of(p), Op::SetViewport);
  c.f[0] = x;
  c.f[1] = y;
  c.f[2] = w;
  c.f[3] = h;
  c.f[4] = mn;
  c.f[5] = mx;
}
void wgpuRenderPassEncoderSetScissorRect(WGPURenderPassEncoder p, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  auto& c = push(enc_of(p), Op::SetScissor);
  c.u[0] = x;
  c.u[1] = y;
  c.u[2] = w;
  c.u[3] = h;
}
void wgpuRenderPassEncoderSetBlendConstant(WGPURenderPassEncoder p, WGPUColor const* color) {
  auto& c = push(enc_of(p), Op::SetBlendConstant);
  if (color) {
    c.f[0] = static_cast<float>(color->r);
    c.f[1] = static_cast<float>(color->g);
    c.f[2] = static_cast<float>(color->b);
    c.f[3] = static_cast<float>(color->a);
  }
}
void wgpuRenderPassEncoderSetStencilReference(WGPURenderPassEncoder p, uint32_t ref) {
  push(enc_of(p), Op::SetStencilRef).u[0] = ref;
}
void wgpuRenderPassEncoderSetImmediates(WGPURenderPassEncoder p, uint32_t offset, void const* data, size_t size) {
  auto& c = push(enc_of(p), Op::SetImmediates);
  c.u[0] = offset;
  c.u[1] = static_cast<uint32_t>(size);
  c.dyn.resize((size + 3) / 4);
  std::memcpy(c.dyn.data(), data, size);
}
void wgpuRenderPassEncoderDraw(WGPURenderPassEncoder p, uint32_t vertexCount, uint32_t instanceCount,
                               uint32_t firstVertex, uint32_t firstInstance) {
  auto& c = push(enc_of(p), Op::Draw);
  c.u[0] = vertexCount;
  c.u[1] = instanceCount;
  c.u[2] = firstVertex;
  c.u[3] = firstInstance;
}
void wgpuRenderPassEncoderDrawIndexed(WGPURenderPassEncoder p, uint32_t indexCount, uint32_t instanceCount,
                                      uint32_t firstIndex, int32_t baseVertex, uint32_t firstInstance) {
  auto& c = push(enc_of(p), Op::DrawIndexed);
  c.u[0] = indexCount;
  c.u[1] = instanceCount;
  c.u[2] = firstIndex;
  c.u[3] = static_cast<uint32_t>(baseVertex);
  c.u[4] = firstInstance;
}

static void copy_loc(TexelLoc& l, WGPUTexelCopyTextureInfo const* t) {
  l.tex = Ref<Texture>(as<Texture>(t->texture));
  l.mip = t->mipLevel;
  l.x = t->origin.x;
  l.y = t->origin.y;
  l.z = t->origin.z;
}
void wgpuCommandEncoderCopyTextureToTexture(WGPUCommandEncoder e, WGPUTexelCopyTextureInfo const* src,
                                            WGPUTexelCopyTextureInfo const* dst, WGPUExtent3D const* size) {
  auto& c = push(as<CommandEncoder>(e), Op::CopyTextureToTexture);
  copy_loc(c.src, src);
  copy_loc(c.dst, dst);
  c.u[0] = size->width;
  c.u[1] = size->height;
  c.u[2] = size->depthOrArrayLayers;
}
void wgpuCommandEncoderCopyBufferToTexture(WGPUCommandEncoder e, WGPUTexelCopyBufferInfo const* src,
                                           WGPUTexelCopyTextureInfo const* dst, WGPUExtent3D const* size) {
  auto& c = push(as<CommandEncoder>(e), Op::CopyBufferToTexture);
  c.obj = Ref<Obj>(as<Obj>(src->buffer));
  c.off = src->layout.offset;
  c.u[3] = src->layout.bytesPerRow;
  c.u[4] = src->layout.rowsPerImage;
  copy_loc(c.dst, dst);
  c.u[0] = size->width;
  c.u[1] = size->height;
  c.u[2] = size->depthOrArrayLayers;
}
void wgpuCommandEncoderCopyTextureToBuffer(WGPUCommandEncoder e, WGPUTexelCopyTextureInfo const* src,
                                           WGPUTexelCopyBufferInfo const* dst, WGPUExtent3D const* size) {
  auto& c = push(as<CommandEncoder>(e), Op::CopyTextureToBuffer);
  copy_loc(c.src, src);
  c.obj = Ref<Obj>(as<Obj>(dst->buffer));
  c.off = dst->layout.offset;
  c.u[3] = dst->layout.bytesPerRow;
  c.u[0] = size->width;
  c.u[1] = size->height;
}
void wgpuCommandEncoderCopyBufferToBuffer(WGPUCommandEncoder e, WGPUBuffer src, uint64_t srcOffset, WGPUBuffer dst,
                                          uint64_t dstOffset, uint64_t size) {
  auto& c = push(as<CommandEncoder>(e), Op::CopyBufferToBuffer);
  c.obj = Ref<Obj>(as<Obj>(src));
  c.buf2 = Ref<Buffer>(as<Buffer>(dst));
  c.off = srcOffset;
  c.off2 = dstOffset;
  c.size = size;
}
void wgpuCommandEncoderClearBuffer(WGPUCommandEncoder e, WGPUBuffer buffer, uint64_t offset, uint64_t size) {
  auto& c = push(as<CommandEncoder>(e), Op::ClearBuffer);
  c.obj = Ref<Obj>(as<Obj>(buffer));
  c.off = offset;
  c.size = size;
}
void wgpuCommandEncoderWriteBuffer(WGPUCommandEncoder e, WGPUBuffer buffer, uint64_t offset, void const* data,
                                   size_t size) {
  auto& c = push(as<CommandEncoder>(e), Op::WriteBuffer);
  c.obj = Ref<Obj>(as<Obj>(buffer));
  c.off = offset;
  c.size = size;
  c.dyn.resize((size + 3) / 4);
  std::memcpy(c.dyn.data(), data, size);
}
WGPUCommandBuffer wgpuCommandEncoderFinish(WGPUCommandEncoder e, WGPUCommandBufferDescriptor const*) {
  auto* cb = new CommandBuffer;
  cb->cmds = std::move(as<CommandEncoder>(e)->cmds);
  return (WGPUCommandBuffer)cb;
}

void wgpuQueueSubmit(WGPUQueue, size_t count, WGPUCommandBuffer const* buffers) {
  for (size_t i = 0; i < count; ++i) wv::execute(as<CommandBuffer>(buffers[i])->cmds);
  flush_spontaneous();
}
WGPUFuture wgpuQueueOnSubmittedWorkDone(WGPUQueue, WGPUQueueWorkDoneCallbackInfo ci) {
  return defer(ci.mode, [ci] {
    if (ci.callback) ci.callback(WGPUQueueWorkDoneStatus_Success, sv(""), ci.userdata1, ci.userdata2);
  });
}
WGPUFuture wgpuDevicePopErrorScope(WGPUDevice, WGPUPopErrorScopeCallbackInfo ci) {
  return defer(ci.mode, [ci] {
    if (ci.callback)
      ci.callback(WGPUPopErrorScopeStatus_Success, WGPUErrorType_NoError, sv(""), ci.userdata1, ci.userdata2);
  });
}

// ---- surface -------------------------------------------------------------------------
struct SurfaceObj : Obj {
  SurfaceObj() : Obj(Kind::Surface) {}
  uint32_t width = 960, height = 544;
  WGPUTextureFormat format = WGPUTextureFormat_RGBA8Unorm;
};
WGPUSurface wgpuInstanceCreateSurface(WGPUInstance, WGPUSurfaceDescriptor const*) {
  return (WGPUSurface) new SurfaceObj;
}
WGPUStatus wgpuSurfaceGetCapabilities(WGPUSurface, WGPUAdapter, WGPUSurfaceCapabilities* caps) {
  static const WGPUTextureFormat formats[] = {WGPUTextureFormat_RGBA8Unorm};
  static const WGPUPresentMode modes[] = {WGPUPresentMode_Fifo};
  static const WGPUCompositeAlphaMode alpha[] = {WGPUCompositeAlphaMode_Opaque};
  if (!caps) return WGPUStatus_Error;
  caps->usages = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopyDst;
  caps->formatCount = 1;
  caps->formats = formats;
  caps->presentModeCount = 1;
  caps->presentModes = modes;
  caps->alphaModeCount = 1;
  caps->alphaModes = alpha;
  return WGPUStatus_Success;
}
void wgpuSurfaceCapabilitiesFreeMembers(WGPUSurfaceCapabilities) {}
void wgpuSurfaceConfigure(WGPUSurface surface, WGPUSurfaceConfiguration const* c) {
  auto* s = as<SurfaceObj>(surface);
  if (c) {
    s->width = c->width;
    s->height = c->height;
    s->format = c->format;
  }
}
void wgpuSurfaceUnconfigure(WGPUSurface) {}
void wgpuSurfaceGetCurrentTexture(WGPUSurface surface, WGPUSurfaceTexture* out) {
  auto* s = as<SurfaceObj>(surface);
  auto* t = new Texture;
  t->width = s->width;
  t->height = s->height;
  t->format = s->format;
  t->usage = WGPUTextureUsage_RenderAttachment;
  t->surface = true;
  out->texture = (WGPUTexture)t;
  out->status = WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal;
}
WGPUStatus wgpuSurfacePresent(WGPUSurface) {
  wv::present();
  return WGPUStatus_Success;
}

} // extern "C"
