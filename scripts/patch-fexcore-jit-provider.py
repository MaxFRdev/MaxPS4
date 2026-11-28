#!/usr/bin/env python3
"""Replace only the pinned iOS allocator with MaxPS4's real prepared-region pool.

All edits are validated before writing; repeated application must match exactly.
No generic FEX ABI or replacement CPU backend is introduced.
"""
from pathlib import Path
import sys

TAG = '// MaxPS4 authorized debugger region provider'
ALLOCATOR = 'FEXCore/Source/Utils/Allocator.cpp'
BACKEND = 'FEXCore/Source/Interface/Core/CPUBackend.cpp'
CORE = 'FEXCore/Source/Interface/Core/Core.cpp'
HEADER = 'FEXCore/Source/Interface/Core/CPUBackend.h'
START = '#if defined(__APPLE__) && TARGET_OS_IPHONE\n// iOS JIT allocation:'
END = '#endif\n} // namespace FEXCore::Allocator'
PROVIDER = '''#if defined(__APPLE__) && TARGET_OS_IPHONE
// MaxPS4 authorized debugger region provider
void* iOSJITAlloc(size_t Size) { return maxps4_jit_allocate(Size); }
bool iOSJITFreeIfOwned(void* Address, size_t Size) {
  const auto Result = maxps4_jit_free(Address, Size);
  if (Result < 0) std::abort();
  return Result == 1;
}
void* GetExecutableAddress(void* Address) { return maxps4_jit_executable(Address); }
void* GetWritableAddress(void* Address) { return maxps4_jit_writable(Address); }
iOSJITAddressKind iOSJITDescribeAddress(void* Address, uintptr_t* Base, size_t* Offset, size_t* Size) {
  return maxps4_jit_describe(Address, Base, Offset, Size) ?
    iOSJITAddressKind::LiveAllocation : iOSJITAddressKind::NotTracked;
}
'''
OLD_BACKEND = '''#else
    Ptr = static_cast<uint8_t*>(FEXCore::Allocator::VirtualAlloc(Size, true));
    LOGMAN_THROW_A_FMT(!!Ptr, "Couldn't allocate code buffer");'''
NEW_BACKEND = '''#elif defined(__APPLE__) && TARGET_OS_IPHONE
    // MaxPS4 authorized debugger region provider: native 16 KiB write guard
    // is installed before initialization; no late RX request after detach.
    Ptr = static_cast<uint8_t*>(FEXCore::Allocator::VirtualAlloc(Size, true));
    if (!Ptr || Ptr == reinterpret_cast<void*>(-1)) throw std::bad_alloc();
#else
    Ptr = static_cast<uint8_t*>(FEXCore::Allocator::VirtualAlloc(Size, true));
    LOGMAN_THROW_A_FMT(!!Ptr, "Couldn't allocate code buffer");'''


# Keep partially created FEX state owned while an allocator error unwinds.
LIFETIME_TAG = '// MaxPS4 checked prepared-region lifetime'
EXTRA = {
    CORE: [
        ('  FEXCore::Core::InternalThreadState* Thread = new FEXCore::Core::InternalThreadState {\n    .CTX = this,\n  };',
         '''  auto ThreadOwner = std::unique_ptr<FEXCore::Core::InternalThreadState>(
    new FEXCore::Core::InternalThreadState {.CTX = this});
  auto* Thread = ThreadOwner.get();'''),
        ('  return Thread;\n}\n\nvoid ContextImpl::DestroyThread',
         '  return ThreadOwner.release();\n}\n\nvoid ContextImpl::DestroyThread'),
    ],
    BACKEND: [
        # The prepared pool holds ONE code buffer, of MAX_CODE_SIZE: a larger
        # one can't be allocated later (CoD filled 16 MiB during its start).
        ('  static constexpr size_t INITIAL_CODE_SIZE = 1024 * 1024 * 16;',
         '''#if defined(__APPLE__) && TARGET_OS_IPHONE
  static constexpr size_t INITIAL_CODE_SIZE = 1024 * 1024 * 512;
#else
  static constexpr size_t INITIAL_CODE_SIZE = 1024 * 1024 * 16;
#endif'''),
        ('    LookupCache = fextl::make_unique<GuestToHostMap>();',
         '''    try {
      LookupCache = fextl::make_unique<GuestToHostMap>();
    } catch (...) {
      FEXCore::Allocator::VirtualFree(Ptr, AllocatedSize);
      throw;
    }'''),
        ('      uintptr_t LastPageAddr = AlignDown(BufferExecBase + Buffer.AllocatedSize - 1, FEXCore::Utils::FEX_PAGE_SIZE);',
         '      uintptr_t LastPageAddr = BufferExecBase + Buffer.UsableSize();'),
    ],
    HEADER: [
        ('      return AllocatedSize - FEXCore::Utils::FEX_PAGE_SIZE;',
         '''#if defined(__APPLE__) && TARGET_OS_IPHONE
      // The prepared pool guards a HOST page (16 KiB on iPhone).
      return maxps4_jit_usable_size(Ptr, AllocatedSize);
#else
      return AllocatedSize - FEXCore::Utils::FEX_PAGE_SIZE;
#endif'''),
    ],
}

def adapt(root):
    paths = {name: root / name for name in (ALLOCATOR, BACKEND, CORE, HEADER)}
    texts = {name: path.read_text() for name, path in paths.items()}
    allocator = texts[ALLOCATOR]
    if TAG in allocator:
        if allocator.count(PROVIDER) != 1 or allocator.count('#include "maxps4_jit_memory.h"') != 1:
            raise ValueError('Applied iOS allocator provider diverged')
    else:
        if allocator.count(START) != 1 or allocator.count(END) != 1:
            raise ValueError('Pinned iOS allocator context diverged')
        start = allocator.index(START)
        end = allocator.index(END, start)
        original = allocator[start:end]
        if original.count('void* iOSJITAlloc(size_t Size)') != 1 or 'BreakGetJITMapping' not in original:
            raise ValueError('Pinned iOS allocator implementation diverged')
        texts[ALLOCATOR] = '#include "maxps4_jit_memory.h"\n' + allocator[:start] + PROVIDER + allocator[end:]
    backend = texts[BACKEND]
    if TAG in backend:
        if backend.count(NEW_BACKEND) != 1:
            raise ValueError('Applied code-cache provider diverged')
    else:
        if backend.count(OLD_BACKEND) != 1:
            raise ValueError('Pinned code-cache allocation context diverged')
        texts[BACKEND] = '#include <new>\n' + backend.replace(OLD_BACKEND, NEW_BACKEND, 1)
    for name, replacements in EXTRA.items():
        value = texts[name]
        if LIFETIME_TAG in value:
            if any(value.count(new) != 1 for _, new in replacements):
                raise ValueError(f'Applied lifetime context diverged: {name}')
        else:
            for old, new in replacements:
                if value.count(old) != 1:
                    raise ValueError(f'Pinned lifetime context diverged: {name}')
                value = value.replace(old, new, 1)
            value = LIFETIME_TAG + '\n' + value
            if name == HEADER:
                value = '#include "maxps4_jit_memory.h"\n' + value
            if name == CORE:
                value = '#include <memory>\n' + value
        texts[name] = value
    for name, path in paths.items():
        path.write_text(texts[name])


if __name__ == '__main__':
    adapt(Path(sys.argv[1]))
