#pragma once
#include <dawn/webgpu.h>
#include <atomic>
#include <cstdint>
#include <functional>

namespace wgnull {
enum class ObjKind { Generic, Instance, Adapter, Device, Queue, Buffer, Texture, TextureView, Surface,
                     RenderPipeline, ComputePipeline };
struct Obj {
  explicit Obj(ObjKind k) : kind(k) {}
  std::atomic<int> rc{1};
  ObjKind kind;
  Obj* ref = nullptr;   // owned reference (queue of a device, texture of a view)
  Obj* ref2 = nullptr;  // second reference (adapter of a device), not released automatically
  void* data = nullptr; // buffer backing store
  uint64_t size = 0;
  uint64_t usage = 0;
  bool mapped = false;
  uint32_t width = 0, height = 0, depth = 0, mips = 0, samples = 0, format = 0, dimension = 0;
};
Obj* make(ObjKind kind);
void addref(void* h);
void release(void* h);
WGPUFuture defer(WGPUCallbackMode mode, std::function<void()> fn);
void flush_spontaneous();
// Called on every present; the Vita port hooks this to pace frames / show status.
void on_present();
} // namespace wgnull
