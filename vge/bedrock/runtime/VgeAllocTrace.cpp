#include "VgeAllocTrace.h"

#include <windows.h>
#include <winternl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "mozilla/StackWalk.h"
#include "nsThreadUtils.h"
#include "nsWindowsDllInterceptor.h"
#include "prenv.h"

namespace vge::bedrock {

namespace {

constexpr size_t kMinimumSize = 64 * 1024;
constexpr size_t kPageSize = 4096;
constexpr size_t kSlotBits = 18;
constexpr size_t kSlots = size_t(1) << kSlotBits;
constexpr size_t kMaxEntries = kSlots - kSlots / 8;
constexpr uintptr_t kTombstone = 1;
constexpr size_t kFrames = 32;
const HANDLE kCurrentProcess = HANDLE(-1);

enum class Kind : uint32_t { Commit, Lock, Ddi };

struct Entry {
  uintptr_t base;
  uintptr_t allocationBase;
  size_t size;
  Kind kind;
  uint32_t frameCount;
  const char* label;
  void* frames[kFrames];
};

using NtAllocateFn = NTSTATUS(NTAPI*)(HANDLE, PVOID*, ULONG_PTR, PSIZE_T, ULONG,
                                      ULONG);
using NtAllocateExFn = NTSTATUS(NTAPI*)(HANDLE, PVOID*, PSIZE_T, ULONG, ULONG,
                                        void*, ULONG);
using NtFreeFn = NTSTATUS(NTAPI*)(HANDLE, PVOID*, PSIZE_T, ULONG);
using NtLock2Fn = NTSTATUS(APIENTRY*)(void*);

struct Lock2Args {
  uint32_t device;
  uint32_t allocation;
  uint32_t flags;
  void* data;
};

mozilla::WindowsDllInterceptor sNtdll;
mozilla::WindowsDllInterceptor sWin32u;
mozilla::WindowsDllInterceptor::FuncHookType<NtAllocateFn> sAllocateStub;
mozilla::WindowsDllInterceptor::FuncHookType<NtAllocateExFn> sAllocateExStub;
mozilla::WindowsDllInterceptor::FuncHookType<NtFreeFn> sFreeStub;
mozilla::WindowsDllInterceptor::FuncHookType<NtLock2Fn> sLock2Stub;
mozilla::WindowsDllInterceptor::FuncHookType<NtLock2Fn> sLockStub;

SRWLOCK sLock = SRWLOCK_INIT;
Entry* sEntries = nullptr;
size_t sCount = 0;
size_t sDropped = 0;

size_t SlotIndex(uintptr_t aBase) {
  return size_t(((aBase >> 12) * 0x9E3779B97F4A7C15ull) >> (64 - kSlotBits));
}

void Record(uintptr_t aBase, size_t aSize, Kind aKind,
            const char* aLabel = nullptr) {
  MEMORY_BASIC_INFORMATION info;
  uintptr_t allocationBase = aBase;
  if (VirtualQuery(reinterpret_cast<void*>(aBase), &info, sizeof(info))) {
    allocationBase = uintptr_t(info.AllocationBase);
  }
  Entry entry;
  entry.base = aBase;
  entry.allocationBase = allocationBase;
  entry.size = aSize;
  entry.kind = aKind;
  entry.label = aLabel;
  entry.frameCount = RtlCaptureStackBackTrace(2, kFrames, entry.frames, nullptr);
  AcquireSRWLockExclusive(&sLock);
  Entry* reusable = nullptr;
  for (size_t i = SlotIndex(aBase);; i = (i + 1) & (kSlots - 1)) {
    Entry& slot = sEntries[i];
    if (slot.base == aBase) {
      slot = entry;
      ReleaseSRWLockExclusive(&sLock);
      return;
    }
    if (slot.base == kTombstone && !reusable) {
      reusable = &slot;
    }
    if (slot.base == 0) {
      if (sCount < kMaxEntries) {
        *(reusable ? reusable : &slot) = entry;
        sCount++;
      } else {
        sDropped++;
      }
      ReleaseSRWLockExclusive(&sLock);
      return;
    }
  }
}

void EraseRange(uintptr_t aBase, size_t aSize) {
  AcquireSRWLockExclusive(&sLock);
  for (uintptr_t page = aBase; page < aBase + aSize; page += kPageSize) {
    for (size_t i = SlotIndex(page);; i = (i + 1) & (kSlots - 1)) {
      Entry& slot = sEntries[i];
      if (slot.base == 0) {
        break;
      }
      if (slot.base == page) {
        slot.base = kTombstone;
        sCount--;
        break;
      }
    }
  }
  ReleaseSRWLockExclusive(&sLock);
}

void EraseAllocation(uintptr_t aBase) {
  uintptr_t cursor = aBase;
  MEMORY_BASIC_INFORMATION info;
  while (VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) &&
         uintptr_t(info.AllocationBase) == aBase) {
    if (info.State == MEM_COMMIT) {
      EraseRange(uintptr_t(info.BaseAddress), info.RegionSize);
    }
    cursor = uintptr_t(info.BaseAddress) + info.RegionSize;
  }
}

bool IsSelf(HANDLE aProcess) {
  return aProcess == kCurrentProcess ||
         GetProcessId(aProcess) == GetCurrentProcessId();
}

NTSTATUS NTAPI AllocateHook(HANDLE aProcess, PVOID* aBase, ULONG_PTR aZeroBits,
                            PSIZE_T aSize, ULONG aType, ULONG aProtect) {
  NTSTATUS status =
      sAllocateStub(aProcess, aBase, aZeroBits, aSize, aType, aProtect);
  if (status >= 0 && IsSelf(aProcess) && (aType & MEM_COMMIT)) {
    Record(uintptr_t(*aBase), *aSize, Kind::Commit);
  }
  return status;
}

NTSTATUS NTAPI AllocateExHook(HANDLE aProcess, PVOID* aBase, PSIZE_T aSize,
                              ULONG aType, ULONG aProtect, void* aParameters,
                              ULONG aCount) {
  NTSTATUS status =
      sAllocateExStub(aProcess, aBase, aSize, aType, aProtect, aParameters, aCount);
  if (status >= 0 && IsSelf(aProcess) && (aType & MEM_COMMIT)) {
    Record(uintptr_t(*aBase), *aSize, Kind::Commit);
  }
  return status;
}

NTSTATUS NTAPI FreeHook(HANDLE aProcess, PVOID* aBase, PSIZE_T aSize,
                        ULONG aType) {
  uintptr_t base = aBase ? uintptr_t(*aBase) : 0;
  size_t size = aSize ? *aSize : 0;
  bool own = IsSelf(aProcess) && base;
  if (own && (aType & MEM_RELEASE)) {
    EraseAllocation(base);
  }
  NTSTATUS status = sFreeStub(aProcess, aBase, aSize, aType);
  if (status >= 0 && own && (aType & MEM_DECOMMIT)) {
    EraseRange(base & ~(kPageSize - 1), size);
  }
  return status;
}

NTSTATUS APIENTRY LockHook(void* aArgs) {
  NTSTATUS status = sLockStub(aArgs);
  if (status >= 0) {
    uintptr_t data = *reinterpret_cast<uintptr_t*>(static_cast<char*>(aArgs) + 24);
    MEMORY_BASIC_INFORMATION info;
    if (data && VirtualQuery(reinterpret_cast<void*>(data), &info, sizeof(info)) &&
        info.RegionSize >= kMinimumSize) {
      Record(uintptr_t(info.BaseAddress), info.RegionSize, Kind::Lock);
    }
  }
  return status;
}

NTSTATUS APIENTRY Lock2Hook(void* aArgs) {
  NTSTATUS status = sLock2Stub(aArgs);
  if (status >= 0) {
    uintptr_t data = uintptr_t(static_cast<Lock2Args*>(aArgs)->data);
    MEMORY_BASIC_INFORMATION info;
    if (data && VirtualQuery(reinterpret_cast<void*>(data), &info, sizeof(info)) &&
        info.RegionSize >= kMinimumSize) {
      Record(uintptr_t(info.BaseAddress), info.RegionSize, Kind::Lock);
    }
  }
  return status;
}

bool ReadableRegion(uintptr_t aAddress, MEMORY_BASIC_INFORMATION* aInfo) {
  constexpr uintptr_t kUserEnd = uintptr_t(1) << 47;
  return aAddress >= 0x10000 && aAddress < kUserEnd &&
         VirtualQuery(reinterpret_cast<void*>(aAddress), aInfo, sizeof(*aInfo)) &&
         aInfo->State == MEM_COMMIT && (aInfo->Protect & PAGE_READWRITE);
}

void ScanArguments(uintptr_t aAddress, const char* aName, int aDepth) {
  MEMORY_BASIC_INFORMATION info;
  if (!ReadableRegion(aAddress, &info) || (aAddress & 7)) {
    return;
  }
  uintptr_t regionEnd = uintptr_t(info.BaseAddress) + info.RegionSize;
  size_t words = std::min<size_t>(32, (regionEnd - aAddress) / 8);
  auto* values = reinterpret_cast<uintptr_t*>(aAddress);
  for (size_t i = 0; i < words; i++) {
    MEMORY_BASIC_INFORMATION target;
    if (!ReadableRegion(values[i], &target) || target.Type != MEM_PRIVATE) {
      continue;
    }
    if (target.RegionSize >= kMinimumSize) {
      Record(uintptr_t(target.BaseAddress), target.RegionSize, Kind::Ddi, aName);
    } else if (aDepth > 0) {
      ScanArguments(values[i], aName, aDepth - 1);
    }
  }
}

using DdiFn = NTSTATUS(APIENTRY*)(void*, void*, void*, void*);

template <size_t N>
struct DdiHook {
  static inline mozilla::WindowsDllInterceptor::FuncHookType<DdiFn> stub;
  static inline const char* name;
  static NTSTATUS APIENTRY Hook(void* a, void* b, void* c, void* d) {
    NTSTATUS status = stub(a, b, c, d);
    if (status >= 0) {
      ScanArguments(uintptr_t(a), name, 1);
    }
    return status;
  }
};

template <size_t... N>
void InstallDdiHooks(const std::vector<const char*>& aNames,
                     std::index_sequence<N...>) {
  ((N < aNames.size()
        ? (DdiHook<N>::name = aNames[N],
           (void)DdiHook<N>::stub.Set(sWin32u, aNames[N], &DdiHook<N>::Hook))
        : void()),
   ...);
}

std::vector<const char*> DdiExports() {
  std::vector<const char*> names;
  auto* base = reinterpret_cast<char*>(GetModuleHandleW(L"win32u.dll"));
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
      base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
  auto* exports = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(
      base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT]
                 .VirtualAddress);
  auto* rvas = reinterpret_cast<DWORD*>(base + exports->AddressOfNames);
  for (DWORD i = 0; i < exports->NumberOfNames; i++) {
    const char* name = base + rvas[i];
    if (!strncmp(name, "NtGdiDdDDI", 10) && strcmp(name, "NtGdiDdDDILock") &&
        strcmp(name, "NtGdiDdDDILock2")) {
      names.push_back(name);
    }
  }
  return names;
}

const std::string& Symbol(void* aAddress) {
  static std::map<void*, std::string> cache;
  auto found = cache.find(aAddress);
  if (found != cache.end()) {
    return found->second;
  }
  MozCodeAddressDetails details;
  MozDescribeCodeAddress(aAddress, &details);
  std::string name = details.function;
  if (name.empty()) {
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "%s+0x%llx", details.library,
             static_cast<unsigned long long>(details.loffset));
    name = buffer;
  }
  return cache.emplace(aAddress, std::move(name)).first->second;
}

bool IsAllocatorFrame(const std::string& aName) {
  static const char* const kPrefixes[] = {
      "pages_",     "arena_",    "BaseAllocator", "MozJemalloc", "Page",
      "MozVirtual", "malloc",    "calloc",        "realloc",     "moz_x",
      "operator ",  "std::_All", "js_arena",      "js_pod",      "VirtualAlloc"};
  for (const char* prefix : kPrefixes) {
    if (aName.compare(0, strlen(prefix), prefix) == 0) {
      return true;
    }
  }
  return false;
}

bool IsDriverFrame(const std::string& aName) {
  static const char* const kPrefixes[] = {
      "NVAPI",           "NVENC",  "NVDEV", "D3D11Core", "CreateDirect3D",
      "SetDependency",   "D3DKMT", "NtGdi", "nvwgf",     "nvgpucomp"};
  for (const char* prefix : kPrefixes) {
    if (aName.compare(0, strlen(prefix), prefix) == 0) {
      return true;
    }
  }
  return false;
}

size_t ResidentBytes(uintptr_t aBase, size_t aSize) {
  struct Query {
    void* address;
    uintptr_t attributes;
  };
  using QueryFn = BOOL(WINAPI*)(HANDLE, void*, DWORD);
  static const auto query = reinterpret_cast<QueryFn>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32QueryWorkingSetEx"));
  constexpr size_t kPage = 4096;
  constexpr size_t kBatch = 2048;
  Query pages[kBatch];
  size_t resident = 0;
  for (size_t offset = 0; offset < aSize;) {
    size_t count = std::min(kBatch, (aSize - offset) / kPage);
    if (!count) {
      break;
    }
    for (size_t i = 0; i < count; i++) {
      pages[i].address = reinterpret_cast<void*>(aBase + offset + i * kPage);
      pages[i].attributes = 0;
    }
    if (query(GetCurrentProcess(), pages, DWORD(count * sizeof(Query)))) {
      for (size_t i = 0; i < count; i++) {
        resident += (pages[i].attributes & 1) ? kPage : 0;
      }
    }
    offset += count * kPage;
  }
  return resident;
}

void WriteReport(const char* aPath) {
  std::vector<Entry> entries;
  entries.reserve(sCount * 2 + 4096);
  AcquireSRWLockShared(&sLock);
  for (size_t i = 0; i < kSlots && entries.size() < entries.capacity(); i++) {
    if (sEntries[i].base > kTombstone) {
      entries.push_back(sEntries[i]);
    }
  }
  size_t dropped = sDropped;
  ReleaseSRWLockShared(&sLock);

  struct Group {
    size_t bytes = 0;
    size_t resident = 0;
    size_t count = 0;
  };
  std::map<std::string, Group> groups;
  size_t tracked = 0;
  size_t trackedResident = 0;
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    return a.base < b.base;
  });
  std::vector<Entry> ddi;
  std::copy_if(entries.begin(), entries.end(), std::back_inserter(ddi),
               [](const Entry& e) { return e.kind == Kind::Ddi; });
  entries.erase(std::remove_if(entries.begin(), entries.end(),
                               [](const Entry& e) { return e.kind == Kind::Ddi; }),
                entries.end());
  std::vector<Entry> mapped;
  mapped.reserve(entries.size());
  std::vector<size_t> residents;
  residents.reserve(entries.size());
  uintptr_t coveredEnd = 0;
  for (Entry entry : entries) {
    uintptr_t entryEnd = entry.base + entry.size;
    if (entry.base < coveredEnd) {
      if (entryEnd <= coveredEnd) {
        continue;
      }
      entry.base = coveredEnd;
      entry.size = entryEnd - coveredEnd;
    }
    MEMORY_BASIC_INFORMATION region;
    if (entry.kind == Kind::Lock &&
        (!VirtualQuery(reinterpret_cast<void*>(entry.base), &region,
                       sizeof(region)) ||
         region.State != MEM_COMMIT || region.Type != MEM_PRIVATE)) {
      continue;
    }
    mapped.push_back(entry);
    size_t resident = ResidentBytes(entry.base, entry.size);
    coveredEnd = entryEnd;
    residents.push_back(resident);
    tracked += entry.size;
    trackedResident += resident;
  }
  entries = std::move(mapped);

  size_t untrackedTotal = 0;
  std::map<size_t, std::pair<size_t, size_t>> untracked;
  struct Unknown {
    MEMORY_BASIC_INFORMATION info;
    size_t resident;
    const Entry* owner;
  };
  std::vector<Unknown> unknown;
  uintptr_t cursor = 0;
  MEMORY_BASIC_INFORMATION info;
  while (VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info))) {
    uintptr_t base = uintptr_t(info.BaseAddress);
    uintptr_t end = base + info.RegionSize;
    if (info.State == MEM_COMMIT && info.Type == MEM_PRIVATE &&
        info.RegionSize >= kMinimumSize &&
        info.AllocationBase != static_cast<void*>(sEntries)) {
      uintptr_t position = base;
      size_t resident = 0;
      auto next = std::upper_bound(
          entries.begin(), entries.end(), base,
          [](uintptr_t value, const Entry& e) { return value < e.base; });
      if (next != entries.begin()) {
        --next;
      }
      for (; next != entries.end() && next->base < end; ++next) {
        const Entry& entry = *next;
        uintptr_t from = std::max(entry.base, base);
        uintptr_t to = std::min(entry.base + entry.size, end);
        if (to <= from) {
          continue;
        }
        if (from > position) {
          resident += ResidentBytes(position, from - position);
        }
        position = std::max(position, to);
      }
      if (position < end) {
        resident += ResidentBytes(position, end - position);
      }
      if (resident) {
        untrackedTotal += resident;
        const Entry* owner = nullptr;
        for (const Entry& candidate : ddi) {
          if (candidate.base < end && candidate.base + candidate.size > base) {
            owner = &candidate;
            break;
          }
        }
        if (resident >= 1048576) {
          unknown.push_back({info, resident, owner});
        }
        auto& bucket = untracked[info.RegionSize / 1048576];
        bucket.first += resident;
        bucket.second++;
      }
    }
    cursor = end;
  }
  for (size_t index = 0; index < entries.size(); index++) {
    const Entry& entry = entries[index];
    std::string key = entry.kind == Kind::Commit ? "COMMIT" : "LOCK";
    uint32_t used = 0;
    for (uint32_t i = 0; i < entry.frameCount && used < 8; i++) {
      std::string name = Symbol(entry.frames[i]);
      if (used == 0 && IsAllocatorFrame(name)) {
        continue;
      }
      if (IsDriverFrame(name)) {
        continue;
      }
      key += " < " + name;
      used++;
    }
    Group& group = groups[key];
    group.bytes += entry.size;
    group.resident += residents[index];
    group.count++;
  }

  std::vector<std::pair<std::string, Group>> sorted(groups.begin(), groups.end());
  std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
    return a.second.resident > b.second.resident;
  });

  std::string temporary = std::string(aPath) + ".tmp";
  FILE* out = fopen(temporary.c_str(), "w");
  if (!out) {
    printf("alloc trace: cannot write %s\n", aPath);
    return;
  }
  fprintf(out, "tracked_mb %.1f resident_mb %.1f entries %zu dropped %zu\n",
          tracked / 1048576.0, trackedResident / 1048576.0, entries.size(),
          dropped);
  for (const auto& [key, group] : sorted) {
    fprintf(out, "resident %.1f committed %.1f MB x%zu %s\n",
            group.resident / 1048576.0, group.bytes / 1048576.0, group.count,
            key.c_str());
  }

  fprintf(out, "untracked_private_resident_mb %.1f\n",
          untrackedTotal / 1048576.0);
  for (auto it = untracked.rbegin(); it != untracked.rend(); ++it) {
    fprintf(out, "untracked region_mb %zu total_mb %.1f regions %zu\n", it->first,
            it->second.first / 1048576.0, it->second.second);
  }
  std::sort(unknown.begin(), unknown.end(), [](const Unknown& a, const Unknown& b) {
    return a.resident > b.resident;
  });
  for (const Unknown& item : unknown) {
    auto* bytes = static_cast<const unsigned char*>(item.info.BaseAddress);
    size_t zero = 0;
    size_t sampled = 0;
    bool readable = item.info.Protect & PAGE_READWRITE;
    if (readable) {
      for (size_t offset = 0; offset < item.info.RegionSize; offset += 1048576) {
        for (size_t i = 0; i < 4096; i++) {
          zero += bytes[offset + i] == 0;
        }
        sampled += 4096;
      }
    }
    fprintf(out, "unknown base %p alloc %p size_mb %.1f resident_mb %.1f protect %lx zero_pct %zu head",
            item.info.BaseAddress, item.info.AllocationBase,
            item.info.RegionSize / 1048576.0, item.resident / 1048576.0,
            item.info.Protect, sampled ? zero * 100 / sampled : 0);
    for (size_t i = 0; readable && i < 16; i++) {
      fprintf(out, " %02x", bytes[i]);
    }
    fprintf(out, " owner %s", item.owner ? item.owner->label : "none");
    if (item.owner) {
      for (uint32_t i = 0; i < item.owner->frameCount && i < 10; i++) {
        fprintf(out, " < %s", Symbol(item.owner->frames[i]).c_str());
      }
    }
    fprintf(out, "\n");
  }
  fclose(out);
  MoveFileExA(temporary.c_str(), aPath, MOVEFILE_REPLACE_EXISTING);
}

}

void InstallAllocTrace() {
  if (!PR_GetEnv("VGE_ALLOC_TRACE")) {
    return;
  }
  sEntries = static_cast<Entry*>(VirtualAlloc(
      nullptr, sizeof(Entry) * kSlots, MEM_RESERVE | MEM_COMMIT,
      PAGE_READWRITE));
  sNtdll.Init("ntdll.dll");
  sAllocateStub.Set(sNtdll, "NtAllocateVirtualMemory", &AllocateHook);
  sAllocateExStub.Set(sNtdll, "NtAllocateVirtualMemoryEx", &AllocateExHook);
  sFreeStub.Set(sNtdll, "NtFreeVirtualMemory", &FreeHook);
  LoadLibraryW(L"win32u.dll");
  sWin32u.Init("win32u.dll");
  sLock2Stub.Set(sWin32u, "NtGdiDdDDILock2", &Lock2Hook);
  sLockStub.Set(sWin32u, "NtGdiDdDDILock", &LockHook);
  InstallDdiHooks(DdiExports(), std::make_index_sequence<256>());
}

void ScheduleAllocTraceReport() {
  const char* path = PR_GetEnv("VGE_ALLOC_TRACE");
  if (!path) {
    return;
  }
  const char* delay = PR_GetEnv("VGE_ALLOC_TRACE_DELAY");
  NS_DelayedDispatchToCurrentThread(
      NS_NewRunnableFunction("VgeAllocTraceReport",
                             [path] { WriteReport(path); }),
      (delay ? atoi(delay) : 30) * 1000);
}

}
