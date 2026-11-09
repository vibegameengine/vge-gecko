#include "VgeAllocTrace.h"

#include <windows.h>
#include <winternl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "mozilla/StackWalk.h"
#include "nsThreadUtils.h"
#include "nsWindowsDllInterceptor.h"
#include "prenv.h"

namespace vge::bedrock {

namespace {

constexpr size_t kMinimumSize = 64 * 1024;
constexpr size_t kMaxEntries = 1 << 16;
constexpr size_t kFrames = 32;
const HANDLE kCurrentProcess = HANDLE(-1);

enum class Kind : uint32_t { Commit, Lock };

struct Entry {
  uintptr_t base;
  uintptr_t allocationBase;
  size_t size;
  Kind kind;
  uint32_t frameCount;
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

void Record(uintptr_t aBase, size_t aSize, Kind aKind) {
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
  entry.frameCount = RtlCaptureStackBackTrace(2, kFrames, entry.frames, nullptr);
  AcquireSRWLockExclusive(&sLock);
  for (size_t i = 0; i < sCount; i++) {
    if (sEntries[i].base == aBase) {
      sEntries[i] = entry;
      ReleaseSRWLockExclusive(&sLock);
      return;
    }
  }
  if (sCount < kMaxEntries) {
    sEntries[sCount++] = entry;
  } else {
    sDropped++;
  }
  ReleaseSRWLockExclusive(&sLock);
}

template <typename Predicate>
void RemoveIf(Predicate aPredicate) {
  AcquireSRWLockExclusive(&sLock);
  for (size_t i = 0; i < sCount;) {
    if (aPredicate(sEntries[i])) {
      sEntries[i] = sEntries[--sCount];
    } else {
      i++;
    }
  }
  ReleaseSRWLockExclusive(&sLock);
}

NTSTATUS NTAPI AllocateHook(HANDLE aProcess, PVOID* aBase, ULONG_PTR aZeroBits,
                            PSIZE_T aSize, ULONG aType, ULONG aProtect) {
  NTSTATUS status =
      sAllocateStub(aProcess, aBase, aZeroBits, aSize, aType, aProtect);
  if (status >= 0 && aProcess == kCurrentProcess && (aType & MEM_COMMIT) &&
      *aSize >= kMinimumSize) {
    Record(uintptr_t(*aBase), *aSize, Kind::Commit);
  }
  return status;
}

NTSTATUS NTAPI AllocateExHook(HANDLE aProcess, PVOID* aBase, PSIZE_T aSize,
                              ULONG aType, ULONG aProtect, void* aParameters,
                              ULONG aCount) {
  NTSTATUS status =
      sAllocateExStub(aProcess, aBase, aSize, aType, aProtect, aParameters, aCount);
  if (status >= 0 && aProcess == kCurrentProcess && (aType & MEM_COMMIT) &&
      *aSize >= kMinimumSize) {
    Record(uintptr_t(*aBase), *aSize, Kind::Commit);
  }
  return status;
}

NTSTATUS NTAPI FreeHook(HANDLE aProcess, PVOID* aBase, PSIZE_T aSize,
                        ULONG aType) {
  uintptr_t base = aBase ? uintptr_t(*aBase) : 0;
  size_t size = aSize ? *aSize : 0;
  NTSTATUS status = sFreeStub(aProcess, aBase, aSize, aType);
  if (status >= 0 && aProcess == kCurrentProcess) {
    if (aType & MEM_RELEASE) {
      RemoveIf([&](const Entry& e) { return e.allocationBase == base; });
    } else if (aType & MEM_DECOMMIT) {
      RemoveIf([&](const Entry& e) {
        return e.base >= base && e.base + e.size <= base + size;
      });
    }
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

std::string Symbol(void* aAddress) {
  MozCodeAddressDetails details;
  MozDescribeCodeAddress(aAddress, &details);
  if (details.function[0]) {
    return details.function;
  }
  char buffer[128];
  snprintf(buffer, sizeof(buffer), "%s+0x%llx", details.library,
           static_cast<unsigned long long>(details.loffset));
  return buffer;
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
  AcquireSRWLockShared(&sLock);
  entries.assign(sEntries, sEntries + sCount);
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
  uintptr_t cursor = 0;
  MEMORY_BASIC_INFORMATION info;
  while (VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info))) {
    uintptr_t base = uintptr_t(info.BaseAddress);
    uintptr_t end = base + info.RegionSize;
    if (info.State == MEM_COMMIT && info.Type == MEM_PRIVATE &&
        info.RegionSize >= kMinimumSize) {
      uintptr_t position = base;
      size_t resident = 0;
      for (const Entry& entry : entries) {
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

  FILE* out = fopen(aPath, "w");
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
  fclose(out);
}

}

void InstallAllocTrace() {
  if (!PR_GetEnv("VGE_ALLOC_TRACE")) {
    return;
  }
  sEntries = static_cast<Entry*>(VirtualAlloc(
      nullptr, sizeof(Entry) * kMaxEntries, MEM_RESERVE | MEM_COMMIT,
      PAGE_READWRITE));
  sNtdll.Init("ntdll.dll");
  sAllocateStub.Set(sNtdll, "NtAllocateVirtualMemory", &AllocateHook);
  sAllocateExStub.Set(sNtdll, "NtAllocateVirtualMemoryEx", &AllocateExHook);
  sFreeStub.Set(sNtdll, "NtFreeVirtualMemory", &FreeHook);
  LoadLibraryW(L"win32u.dll");
  sWin32u.Init("win32u.dll");
  sLock2Stub.Set(sWin32u, "NtGdiDdDDILock2", &Lock2Hook);
  sLockStub.Set(sWin32u, "NtGdiDdDDILock", &LockHook);
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
