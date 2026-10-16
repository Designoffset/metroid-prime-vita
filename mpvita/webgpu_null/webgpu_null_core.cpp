// Null WebGPU implementation: accepts every call, draws nothing.
// Lets Aurora's GX/gfx layers run unmodified on the Vita while the real GXM
// renderer is developed. Objects are refcounted; buffers own CPU memory so
// mapping works; textures remember their descriptor so getters are correct.
#include "webgpu_null.hpp"

#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace wgnull {

Obj* make(ObjKind kind) { return new Obj(kind); }

void addref(void* h) {
  if (h) static_cast<Obj*>(h)->rc.fetch_add(1, std::memory_order_relaxed);
}

void release(void* h) {
  if (!h) return;
  auto* o = static_cast<Obj*>(h);
  if (o->rc.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    std::free(o->data);
    if (o->ref) release(o->ref);
    delete o;
  }
}

// ---- deferred callbacks ----------------------------------------------------
struct Pending {
  uint64_t id;
  WGPUCallbackMode mode;
  std::function<void()> fn;
};
static std::mutex g_mutex;
static std::deque<Pending> g_pending;
static uint64_t g_nextFuture = 1;

WGPUFuture defer(WGPUCallbackMode mode, std::function<void()> fn) {
  std::lock_guard lk(g_mutex);
  const uint64_t id = g_nextFuture++;
  g_pending.push_back({id, mode, std::move(fn)});
  return WGPUFuture{id};
}

// Run pending callbacks matching `pred`. Callbacks run without the lock held
// (they may schedule more work).
template <typename Pred>
static void run_where(Pred pred) {
  std::vector<std::function<void()>> torun;
  {
    std::lock_guard lk(g_mutex);
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

void flush_spontaneous() {
  run_where([](const Pending& p) { return p.mode == WGPUCallbackMode_AllowSpontaneous; });
}

} // namespace wgnull

using namespace wgnull;

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

extern "C" {

WGPUInstance wgpuCreateInstance(WGPUInstanceDescriptor const*) { return (WGPUInstance)make(ObjKind::Instance); }

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
  auto* adapter = (WGPUAdapter)make(ObjKind::Adapter);
  return defer(ci.mode, [ci, adapter] {
    if (ci.callback) ci.callback(WGPURequestAdapterStatus_Success, adapter, sv(""), ci.userdata1, ci.userdata2);
  });
}

WGPUFuture wgpuAdapterRequestDevice(WGPUAdapter adapter, WGPUDeviceDescriptor const*, WGPURequestDeviceCallbackInfo ci) {
  auto* dev = make(ObjKind::Device);
  auto* queue = make(ObjKind::Queue);
  dev->ref = queue;
  addref(adapter);
  dev->ref2 = static_cast<Obj*>((void*)adapter);
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

WGPUStatus wgpuAdapterGetInfo(WGPUAdapter, WGPUAdapterInfo* info) {
  if (!info) return WGPUStatus_Error;
  WGPUChainedStruct* chain = info->nextInChain;
  std::memset(info, 0, sizeof(*info));
  info->nextInChain = chain;
  info->vendor = sv("Sony");
  info->architecture = sv("SGX543MP4+");
  info->device = sv("PlayStation Vita (null renderer)");
  info->description = sv("null");
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
  auto* q = static_cast<Obj*>((void*)device)->ref;
  addref(q);
  return (WGPUQueue)q;
}
WGPUAdapter wgpuDeviceGetAdapter(WGPUDevice device) {
  auto* a = static_cast<Obj*>((void*)device)->ref2;
  addref(a);
  return (WGPUAdapter)a;
}

// ---- buffers ----------------------------------------------------------------
WGPUBuffer wgpuDeviceCreateBuffer(WGPUDevice, WGPUBufferDescriptor const* d) {
  auto* b = make(ObjKind::Buffer);
  b->size = d ? d->size : 0;
  b->usage = d ? d->usage : 0;
  // Only buffers the CPU maps get memory; GPU-side buffers cost nothing (their
  // contents are never read back in the null renderer).
  const bool mappable = d && (d->mappedAtCreation || (d->usage & (WGPUBufferUsage_MapWrite | WGPUBufferUsage_MapRead)));
  b->data = mappable ? std::calloc(1, b->size ? (size_t)b->size : 1) : nullptr;
  b->mapped = d && d->mappedAtCreation;
  return (WGPUBuffer)b;
}
void* wgpuBufferGetMappedRange(WGPUBuffer buffer, size_t offset, size_t) {
  auto* b = static_cast<Obj*>((void*)buffer);
  return b && b->data ? static_cast<uint8_t*>(b->data) + offset : nullptr;
}
void const* wgpuBufferGetConstMappedRange(WGPUBuffer buffer, size_t offset, size_t size) {
  return wgpuBufferGetMappedRange(buffer, offset, size);
}
WGPUStatus wgpuBufferWriteMappedRange(WGPUBuffer buffer, size_t offset, void const* data, size_t size) {
  auto* b = static_cast<Obj*>((void*)buffer);
  if (!b || !b->data || offset + size > b->size) return WGPUStatus_Error;
  std::memcpy(static_cast<uint8_t*>(b->data) + offset, data, size);
  return WGPUStatus_Success;
}
WGPUStatus wgpuBufferReadMappedRange(WGPUBuffer buffer, size_t offset, void* data, size_t size) {
  auto* b = static_cast<Obj*>((void*)buffer);
  if (!b || !b->data || offset + size > b->size) return WGPUStatus_Error;
  std::memcpy(data, static_cast<uint8_t*>(b->data) + offset, size);
  return WGPUStatus_Success;
}
WGPUFuture wgpuBufferMapAsync(WGPUBuffer buffer, WGPUMapMode, size_t, size_t, WGPUBufferMapCallbackInfo ci) {
  auto* b = static_cast<Obj*>((void*)buffer);
  addref(b);
  return defer(ci.mode, [ci, b] {
    b->mapped = true;
    if (ci.callback) ci.callback(WGPUMapAsyncStatus_Success, sv(""), ci.userdata1, ci.userdata2);
    release(b);
  });
}
void wgpuBufferUnmap(WGPUBuffer buffer) {
  if (buffer) static_cast<Obj*>((void*)buffer)->mapped = false;
}
WGPUBufferMapState wgpuBufferGetMapState(WGPUBuffer buffer) {
  return buffer && static_cast<Obj*>((void*)buffer)->mapped ? WGPUBufferMapState_Mapped : WGPUBufferMapState_Unmapped;
}
uint64_t wgpuBufferGetSize(WGPUBuffer buffer) { return buffer ? static_cast<Obj*>((void*)buffer)->size : 0; }
WGPUBufferUsage wgpuBufferGetUsage(WGPUBuffer buffer) { return buffer ? static_cast<Obj*>((void*)buffer)->usage : 0; }
void wgpuQueueWriteBuffer(WGPUQueue, WGPUBuffer buffer, uint64_t offset, void const* data, size_t size) {
  auto* b = static_cast<Obj*>((void*)buffer);
  if (b && b->data && data && offset + size <= b->size) std::memcpy(static_cast<uint8_t*>(b->data) + offset, data, size);
}

// ---- textures ---------------------------------------------------------------
WGPUTexture wgpuDeviceCreateTexture(WGPUDevice, WGPUTextureDescriptor const* d) {
  auto* t = make(ObjKind::Texture);
  if (d) {
    t->width = d->size.width;
    t->height = d->size.height;
    t->depth = d->size.depthOrArrayLayers;
    t->mips = d->mipLevelCount;
    t->samples = d->sampleCount;
    t->format = d->format;
    t->dimension = d->dimension;
    t->usage = d->usage;
  }
  return (WGPUTexture)t;
}
#define TEX(h) static_cast<Obj*>((void*)(h))
uint32_t wgpuTextureGetWidth(WGPUTexture t) { return TEX(t)->width; }
uint32_t wgpuTextureGetHeight(WGPUTexture t) { return TEX(t)->height; }
uint32_t wgpuTextureGetDepthOrArrayLayers(WGPUTexture t) { return TEX(t)->depth; }
uint32_t wgpuTextureGetMipLevelCount(WGPUTexture t) { return TEX(t)->mips; }
uint32_t wgpuTextureGetSampleCount(WGPUTexture t) { return TEX(t)->samples; }
WGPUTextureFormat wgpuTextureGetFormat(WGPUTexture t) { return (WGPUTextureFormat)TEX(t)->format; }
WGPUTextureDimension wgpuTextureGetDimension(WGPUTexture t) { return (WGPUTextureDimension)TEX(t)->dimension; }
WGPUTextureUsage wgpuTextureGetUsage(WGPUTexture t) { return TEX(t)->usage; }
WGPUTextureView wgpuTextureCreateView(WGPUTexture t, WGPUTextureViewDescriptor const*) {
  auto* v = make(ObjKind::TextureView);
  addref(t);
  v->ref = TEX(t);
  return (WGPUTextureView)v;
}

// ---- surface ----------------------------------------------------------------
WGPUSurface wgpuInstanceCreateSurface(WGPUInstance, WGPUSurfaceDescriptor const*) {
  auto* s = make(ObjKind::Surface);
  s->width = 960;
  s->height = 544;
  s->format = WGPUTextureFormat_BGRA8Unorm;
  return (WGPUSurface)s;
}
WGPUStatus wgpuSurfaceGetCapabilities(WGPUSurface, WGPUAdapter, WGPUSurfaceCapabilities* caps) {
  static const WGPUTextureFormat formats[] = {WGPUTextureFormat_BGRA8Unorm, WGPUTextureFormat_RGBA8Unorm};
  static const WGPUPresentMode modes[] = {WGPUPresentMode_Fifo};
  static const WGPUCompositeAlphaMode alpha[] = {WGPUCompositeAlphaMode_Opaque};
  if (!caps) return WGPUStatus_Error;
  caps->usages = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopyDst;
  caps->formatCount = 2;
  caps->formats = formats;
  caps->presentModeCount = 1;
  caps->presentModes = modes;
  caps->alphaModeCount = 1;
  caps->alphaModes = alpha;
  return WGPUStatus_Success;
}
void wgpuSurfaceCapabilitiesFreeMembers(WGPUSurfaceCapabilities) {}
void wgpuSurfaceConfigure(WGPUSurface surface, WGPUSurfaceConfiguration const* c) {
  auto* s = static_cast<Obj*>((void*)surface);
  if (c) {
    s->width = c->width;
    s->height = c->height;
    s->format = c->format;
  }
}
void wgpuSurfaceUnconfigure(WGPUSurface) {}
void wgpuSurfaceGetCurrentTexture(WGPUSurface surface, WGPUSurfaceTexture* out) {
  auto* s = static_cast<Obj*>((void*)surface);
  auto* t = make(ObjKind::Texture);
  t->width = s->width;
  t->height = s->height;
  t->depth = 1;
  t->mips = 1;
  t->samples = 1;
  t->format = s->format;
  t->dimension = WGPUTextureDimension_2D;
  t->usage = WGPUTextureUsage_RenderAttachment;
  out->texture = (WGPUTexture)t;
  out->status = WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal;
}
WGPUStatus wgpuSurfacePresent(WGPUSurface) {
  wgnull::on_present();
  return WGPUStatus_Success;
}

// ---- async completions ------------------------------------------------------
WGPUFuture wgpuQueueOnSubmittedWorkDone(WGPUQueue, WGPUQueueWorkDoneCallbackInfo ci) {
  return defer(ci.mode, [ci] {
    if (ci.callback) ci.callback(WGPUQueueWorkDoneStatus_Success, sv(""), ci.userdata1, ci.userdata2);
  });
}
void wgpuQueueSubmit(WGPUQueue, size_t, WGPUCommandBuffer const*) { flush_spontaneous(); }

WGPUFuture wgpuDevicePopErrorScope(WGPUDevice, WGPUPopErrorScopeCallbackInfo ci) {
  return defer(ci.mode, [ci] {
    if (ci.callback) ci.callback(WGPUPopErrorScopeStatus_Success, WGPUErrorType_NoError, sv(""), ci.userdata1, ci.userdata2);
  });
}
WGPUFuture wgpuDeviceCreateRenderPipelineAsync(WGPUDevice, WGPURenderPipelineDescriptor const*,
                                               WGPUCreateRenderPipelineAsyncCallbackInfo ci) {
  auto* p = (WGPURenderPipeline)make(ObjKind::RenderPipeline);
  return defer(ci.mode, [ci, p] {
    if (ci.callback) ci.callback(WGPUCreatePipelineAsyncStatus_Success, p, sv(""), ci.userdata1, ci.userdata2);
  });
}
WGPUFuture wgpuDeviceCreateComputePipelineAsync(WGPUDevice, WGPUComputePipelineDescriptor const*,
                                                WGPUCreateComputePipelineAsyncCallbackInfo ci) {
  auto* p = (WGPUComputePipeline)make(ObjKind::ComputePipeline);
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

} // extern "C"

namespace wgnull {
__attribute__((weak)) void on_present() {}
}
