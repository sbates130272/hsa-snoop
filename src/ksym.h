// ksym.h - Resolve an AQL kernel_object handle to a human-readable kernel name.
//
// The kernel_object in a dispatch packet is the runtime address of the kernel
// descriptor (the ELF symbol "<mangled-name>.kd") inside an AMDGPU code object
// loaded in the target process. We locate AMDGPU (EM_AMDGPU) ELF images in the
// process address space and walk their dynamic symbol tables from memory to
// build a kernel_object -> name map. Best-effort: falls back to a hex label.
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace hsasnoop {

class KernelSymbolResolver {
 public:
  explicit KernelSymbolResolver(int pid) : pid_(pid) {}

  // Returns a demangled kernel name for kernel_object, or "kernel_0x..." if it
  // cannot be resolved. Results are cached; a miss triggers a rescan of the
  // process's loaded code objects.
  std::string Resolve(uint64_t kernel_object);

 private:
  void ScanCodeObjects();

  int pid_;
  std::mutex mu_;
  std::unordered_map<uint64_t, std::string> cache_;  // kernel_object -> name
  int scanned_generation_ = 0;
};

std::string Demangle(const std::string& mangled);

}  // namespace hsasnoop
