#include "VgeJsGcInternal.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <windows.h>

#include <algorithm>
#include <string>
#include <thread>

#include "js/CharacterEncoding.h"
#include "js/PropertyAndElement.h"
#include "js/SliceBudget.h"
#include "js/Utility.h"
#include "js/friend/PerformanceHint.h"
#include "mozilla/EnumeratedArray.h"
#include "mozilla/Vector.h"

namespace js::gcstats {

struct Statistics {
  uint64_t gcNumber;
  const char* reason;
  const char* nonincrementalReason;
  mozilla::TimeStamp start;
  mozilla::TimeStamp end;
};

}

namespace vge::jsapi::gc {

namespace phases {
#include "StatsPhasesGeneratedEnum.inc"

struct PhaseKindInfo {
  Phase firstPhase;
  const char* name;
};

struct PhaseInfo {
  Phase parent;
  Phase firstChild;
  Phase nextSibling;
  Phase nextWithPhaseKind;
  PhaseKind phaseKind;
  uint8_t depth;
  const char* name;
  const char* path;
};

using PhaseTable =
    mozilla::EnumeratedArray<Phase, PhaseInfo, size_t(Phase::LIMIT)>;
using PhaseKindTable =
    mozilla::EnumeratedArray<PhaseKind, PhaseKindInfo, size_t(PhaseKind::LIMIT)>;

#include "StatsPhasesGenerated.inc"
}

namespace {

#define VGE_GC_ABORT_REASONS(D)  \
  D(None, 0)                     \
  D(NonIncrementalRequested, 1)  \
  D(AbortRequested, 2)           \
  D(Unused1, 3)                  \
  D(IncrementalDisabled, 4)      \
  D(ModeChange, 5)               \
  D(MallocBytesTrigger, 6)       \
  D(GCBytesTrigger, 7)           \
  D(ZoneChange, 8)               \
  D(CompartmentRevived, 9)       \
  D(GrayRootBufferingFailed, 10) \
  D(JitCodeBytesTrigger, 11)

uint32_t DefaultParam(JSGCParamKey aKey) {
  switch (aKey) {
    case JSGC_MAX_BYTES:
      return 0xffffffff;
    case JSGC_MIN_NURSERY_BYTES:
      return 256 * 1024;
    case JSGC_MAX_NURSERY_BYTES:
      return JS::DefaultNurseryMaxBytes;
    case JSGC_ALLOCATION_THRESHOLD:
      return 27;
    case JSGC_SMALL_HEAP_SIZE_MAX:
      return 100;
    case JSGC_LARGE_HEAP_SIZE_MIN:
      return 500;
    case JSGC_SMALL_HEAP_INCREMENTAL_LIMIT:
      return 170;
    case JSGC_LARGE_HEAP_INCREMENTAL_LIMIT:
      return 110;
    case JSGC_HIGH_FREQUENCY_TIME_LIMIT:
      return 1000;
    case JSGC_LOW_FREQUENCY_HEAP_GROWTH:
      return 150;
    case JSGC_HIGH_FREQUENCY_SMALL_HEAP_GROWTH:
      return 300;
    case JSGC_HIGH_FREQUENCY_LARGE_HEAP_GROWTH:
      return 150;
    case JSGC_MALLOC_THRESHOLD_BASE:
      return 38;
    case JSGC_ZONE_ALLOC_DELAY_KB:
      return 1024;
    case JSGC_URGENT_THRESHOLD_MB:
      return 16;
    case JSGC_NURSERY_EAGER_COLLECTION_THRESHOLD_KB:
      return uint32_t(js::gc::ChunkSize / 4 / 1024);
    case JSGC_NURSERY_EAGER_COLLECTION_THRESHOLD_PERCENT:
      return 25;
    case JSGC_NURSERY_EAGER_COLLECTION_TIMEOUT_MS:
      return 5000;
    case JSGC_BALANCED_HEAP_LIMITS_ENABLED:
      return 0;
    case JSGC_HEAP_GROWTH_FACTOR:
      return 50;
    case JSGC_MIN_LAST_DITCH_GC_PERIOD:
      return 60;
    case JSGC_PARALLEL_MARKING_THRESHOLD_MB:
      return 4;
    case JSGC_GENERATE_MISSING_ALLOC_SITES:
      return 0;
    case JSGC_NURSERY_MAX_TIME_GOAL_MS:
      return 4;
    case JSGC_STORE_BUFFER_ENTRIES:
      return 16384;
    case JSGC_STORE_BUFFER_SCALING:
      return 25;
    case JSGC_INCREMENTAL_GC_ENABLED:
      return 0;
    case JSGC_PER_ZONE_GC_ENABLED:
      return 0;
    case JSGC_SLICE_TIME_BUDGET_MS:
      return 0;
    case JSGC_MIN_EMPTY_CHUNK_COUNT:
      return 1;
    case JSGC_COMPACTING_ENABLED:
      return 1;
    case JSGC_NURSERY_ENABLED:
      return 1;
    case JSGC_PARALLEL_MARKING_ENABLED:
      return 0;
    case JSGC_CONCURRENT_MARKING_ENABLED:
      return 0;
    case JSGC_INCREMENTAL_WEAKMAP_ENABLED:
      return 1;
    case JSGC_SEMISPACE_NURSERY_ENABLED:
      return 0;
    case JSGC_HELPER_THREAD_RATIO:
      return 50;
    case JSGC_MAX_HELPER_THREADS:
      return 8;
    case JSGC_MAX_MARKING_THREADS:
      return 2;
    default:
      return 0;
  }
}

uint32_t StoredParam(GcState& aState, JSGCParamKey aKey) {
  size_t k = size_t(aKey);
  if (k < kGCParamCount && aState.paramSet[k]) {
    return aState.params[k];
  }
  return DefaultParam(aKey);
}

size_t SpaceSize(v8::Isolate* aIsolate, const char* aName, bool aUsed) {
  size_t count = aIsolate->NumberOfHeapSpaces();
  for (size_t i = 0; i < count; i++) {
    v8::HeapSpaceStatistics stats;
    if (!aIsolate->GetHeapSpaceStatistics(&stats, i)) {
      continue;
    }
    if (strcmp(stats.space_name(), aName) == 0) {
      return aUsed ? stats.space_used_size() : stats.space_size();
    }
  }
  return 0;
}

uint32_t HelperThreadCount(GcState& aState) {
  unsigned cpus = std::max(1u, std::thread::hardware_concurrency());
  double ratio = StoredParam(aState, JSGC_HELPER_THREAD_RATIO) / 100.0;
  uint32_t count = uint32_t(std::max(1.0, cpus * ratio));
  return std::min(count, StoredParam(aState, JSGC_MAX_HELPER_THREADS));
}

bool HighFrequency(GcState& aState) {
  if (aState.cycleStart.IsNull() || aState.cycleEnd.IsNull()) {
    return false;
  }
  mozilla::TimeDuration since = mozilla::TimeStamp::Now() - aState.cycleEnd;
  return since.ToMilliseconds() <
         StoredParam(aState, JSGC_HIGH_FREQUENCY_TIME_LIMIT);
}

char16_t* Inflate(const std::string& aText) {
  char16_t* out = js_pod_malloc<char16_t>(aText.size() + 1);
  if (!out) {
    return nullptr;
  }
  for (size_t i = 0; i < aText.size(); i++) {
    out[i] = char16_t(uint8_t(aText[i]));
  }
  out[aText.size()] = 0;
  return out;
}

JS::UniqueChars Dup(const std::string& aText) {
  char* out = js_pod_malloc<char>(aText.size() + 1);
  if (!out) {
    return nullptr;
  }
  memcpy(out, aText.c_str(), aText.size() + 1);
  return JS::UniqueChars(out);
}

double Millis(mozilla::TimeStamp aStart, mozilla::TimeStamp aEnd) {
  if (aStart.IsNull() || aEnd.IsNull() || aEnd < aStart) {
    return 0.0;
  }
  return (aEnd - aStart).ToMilliseconds();
}

std::string Format(const char* aFormat, ...) {
  char buffer[1024];
  va_list args;
  va_start(args, aFormat);
  vsnprintf(buffer, sizeof(buffer), aFormat, args);
  va_end(args);
  return buffer;
}

class LatinExternal final : public v8::String::ExternalOneByteStringResource {
 public:
  LatinExternal(const JS::Latin1Char* aChars, size_t aLength)
      : mChars(reinterpret_cast<const char*>(aChars)), mLength(aLength) {}
  const char* data() const override { return mChars; }
  size_t length() const override { return mLength; }

 private:
  const char* mChars;
  size_t mLength;
};

class TwoByteExternal final : public v8::String::ExternalStringResource {
 public:
  TwoByteExternal(const char16_t* aChars, size_t aLength)
      : mChars(reinterpret_cast<const uint16_t*>(aChars)), mLength(aLength) {}
  const uint16_t* data() const override { return mChars; }
  size_t length() const override { return mLength; }

 private:
  const uint16_t* mChars;
  size_t mLength;
};

JSString* NewExternalCell(JSContext* aCx, const void* aChars, size_t aLength,
                          bool aLatin1,
                          const JSExternalStringCallbacks* aCallbacks) {
  v8::HandleScope scope(aCx->isolate);
  v8::Local<v8::String> str;
  if (aLatin1) {
    auto* resource =
        new LatinExternal(static_cast<const JS::Latin1Char*>(aChars), aLength);
    if (!v8::String::NewExternalOneByte(aCx->isolate, resource)
             .ToLocal(&str)) {
      delete resource;
      return nullptr;
    }
  } else {
    auto* resource =
        new TwoByteExternal(static_cast<const char16_t*>(aChars), aLength);
    if (!v8::String::NewExternalTwoByte(aCx->isolate, resource)
             .ToLocal(&str)) {
      delete resource;
      return nullptr;
    }
  }
  StringCell* cell = static_cast<StringCell*>(AllocCell(sizeof(StringCell)));
  uintptr_t flags = JS::shadow::String::EXTERNAL_FLAGS;
  if (aLatin1) {
    flags |= JS::shadow::String::LATIN1_CHARS_BIT;
  }
  cell->flags = flags | (uintptr_t(aLength) << 32);
  cell->chars = aChars;
  cell->external = aCallbacks;
  cell->tail.kind = CellKind::String;
  BindCellHandle(aCx, &cell->tail, str, false);
  return cell->asString();
}

JSString* NewCopiedString(JSContext* aCx, const char16_t* aChars,
                          size_t aLength) {
  v8::HandleScope scope(aCx->isolate);
  v8::Local<v8::String> str;
  if (!v8::String::NewFromTwoByte(aCx->isolate,
                                  reinterpret_cast<const uint16_t*>(aChars),
                                  v8::NewStringType::kNormal, int(aLength))
           .ToLocal(&str)) {
    return nullptr;
  }
  return StringFromV8(aCx, str);
}

JSString* NewCopiedLatin1(JSContext* aCx, const JS::Latin1Char* aChars,
                          size_t aLength) {
  v8::HandleScope scope(aCx->isolate);
  v8::Local<v8::String> str;
  if (!v8::String::NewFromOneByte(aCx->isolate, aChars,
                                  v8::NewStringType::kNormal, int(aLength))
           .ToLocal(&str)) {
    return nullptr;
  }
  return StringFromV8(aCx, str);
}

constexpr size_t kMaxInlineCopy = 24;

bool SetNumber(JSContext* aCx, JS::HandleObject aObj, const char* aName,
               double aValue) {
  JS::RootedValue v(aCx, JS::NumberValue(aValue));
  return JS_DefineProperty(aCx, aObj, aName, v, JSPROP_ENUMERATE);
}

bool SetBool(JSContext* aCx, JS::HandleObject aObj, const char* aName,
             bool aValue) {
  JS::RootedValue v(aCx, JS::BooleanValue(aValue));
  return JS_DefineProperty(aCx, aObj, aName, v, JSPROP_ENUMERATE);
}

}

}

using namespace vge::jsapi;
using namespace vge::jsapi::gc;

void JS::TimeBudget::setDeadlineFromNow() {
  deadline = mozilla::TimeStamp::Now() + budget;
}

JS::SliceBudget::SliceBudget(TimeBudget time, InterruptRequestFlag* interrupt)
    : counter(StepsPerExpensiveCheck),
      interruptRequested(interrupt),
      budget(TimeBudget(time)) {
  budget.as<TimeBudget>().setDeadlineFromNow();
}

JS::SliceBudget::SliceBudget(WorkBudget work)
    : counter(work.budget), interruptRequested(nullptr), budget(work) {}

int JS::SliceBudget::describe(char* buffer, size_t maxlen) const {
  if (isUnlimited()) {
    return snprintf(buffer, maxlen, "unlimited");
  }
  const char* nonstop = keepGoing ? "nonstop " : "";
  if (isWorkBudget()) {
    return snprintf(buffer, maxlen, "%swork(%" PRId64 ")", nonstop,
                    workBudget());
  }
  const char* interruptStr = "";
  if (interruptRequested) {
    interruptStr = interrupted ? "INTERRUPTED " : "interruptible ";
  }
  const char* extra = "";
  if (idle) {
    extra = extended ? " (started idle but extended)" : " (idle)";
  }
  double millis = timeBudget().ToMilliseconds();
  return snprintf(buffer, maxlen, "%s%s%3.1fms%s", nonstop, interruptStr,
                  millis, extra);
}

bool JS::SliceBudget::checkOverBudget() {
  if (isWorkBudget()) {
    return true;
  }
  if (interruptRequested && *interruptRequested) {
    interrupted = true;
  }
  if (interrupted) {
    return true;
  }
  if (isTimeBudget() &&
      mozilla::TimeStamp::Now() >= budget.as<TimeBudget>().deadline) {
    return true;
  }
  counter = StepsPerExpensiveCheck;
  return false;
}

JS_PUBLIC_API const char* JS::ExplainGCReason(JS::GCReason reason) {
  switch (reason) {
#define SWITCH_REASON(name, _) \
  case JS::GCReason::name:     \
    return #name;
    GCREASONS(SWITCH_REASON)
#undef SWITCH_REASON
    case JS::GCReason::NO_REASON:
      return "NO_REASON";
    default:
      MOZ_CRASH("bad GC reason");
  }
}

JS_PUBLIC_API bool JS::InternalGCReason(JS::GCReason reason) {
  return reason < JS::GCReason::FIRST_FIREFOX_REASON;
}

JS_PUBLIC_API const char* JS::ExplainGCAbortReason(uint32_t reason) {
  switch (reason) {
#define SWITCH_REASON(name, num) \
  case num:                      \
    return #name;
    VGE_GC_ABORT_REASONS(SWITCH_REASON)
#undef SWITCH_REASON
    default:
      MOZ_CRASH("bad GC abort reason");
  }
}

JS_PUBLIC_API const char* JS::GetGCPhaseName(uint32_t phase) {
  phases::PhaseKind kind = static_cast<phases::PhaseKind>(phase);
  MOZ_RELEASE_ASSERT(kind < phases::PhaseKind::LIMIT);
  return phases::phaseKinds[kind].name;
}

JS_PUBLIC_API void JS::PrepareZoneForGC(JSContext* cx, JS::Zone* zone) {
  EnsureHooks(cx);
}

JS_PUBLIC_API void JS::PrepareForFullGC(JSContext* cx) { EnsureHooks(cx); }

JS_PUBLIC_API void JS::PrepareForIncrementalGC(JSContext* cx) {
  EnsureHooks(cx);
}

JS_PUBLIC_API void JS::NonIncrementalGC(JSContext* cx, JS::GCOptions options,
                                        JS::GCReason reason) {
  RunFullGC(cx, options, reason);
}

JS_PUBLIC_API void JS::StartIncrementalGC(JSContext* cx, JS::GCOptions options,
                                          JS::GCReason reason,
                                          const JS::SliceBudget& budget) {
  RunFullGC(cx, options, reason);
}

JS_PUBLIC_API void JS::IncrementalGCSlice(JSContext* cx, JS::GCReason reason,
                                          const JS::SliceBudget& budget) {
  GcState& state = StateOf(cx->runtime);
  if (state.v8Marking) {
    RunFullGC(cx, state.currentOptions, reason);
  }
}

JS_PUBLIC_API bool JS::IncrementalGCHasForegroundWork(JSContext* cx) {
  GcState* state = MaybeStateOf(cx->runtime);
  return state && state->v8Marking;
}

JS_PUBLIC_API void JS::FinishIncrementalGC(JSContext* cx, JS::GCReason reason) {
  GcState& state = StateOf(cx->runtime);
  RunFullGC(cx, state.currentOptions, reason);
}

JS_PUBLIC_API void JS::AbortIncrementalGC(JSContext* cx) {
  GcState& state = StateOf(cx->runtime);
  if (state.v8Marking) {
    RunFullGC(cx, state.currentOptions, JS::GCReason::ABORT_GC);
  }
}

JS_PUBLIC_API bool JS::IsIncrementalGCInProgress(JSContext* cx) {
  GcState* state = MaybeStateOf(cx->runtime);
  return state && state->v8Marking;
}

JS_PUBLIC_API bool JS::IsIncrementalGCInProgress(JSRuntime* rt) {
  GcState* state = MaybeStateOf(rt);
  return state && state->v8Marking;
}

JS_PUBLIC_API bool JS::WasIncrementalGC(JSRuntime* rt) {
  GcState* state = MaybeStateOf(rt);
  return state && state->lastWasIncremental;
}

JS_PUBLIC_API JS::GCSliceCallback JS::SetGCSliceCallback(
    JSContext* cx, JS::GCSliceCallback callback) {
  EnsureHooks(cx);
  GcState& state = StateOf(cx->runtime);
  JS::GCSliceCallback old = state.sliceCallback;
  state.sliceCallback = callback;
  return old;
}

JS_PUBLIC_API bool JS::AddGCNurseryCollectionCallback(
    JSContext* cx, JS::GCNurseryCollectionCallback callback, void* data) {
  EnsureHooks(cx);
  StateOf(cx->runtime).nurseryCallbacks.emplace_back(callback, data);
  return true;
}

JS_PUBLIC_API void JS::RemoveGCNurseryCollectionCallback(
    JSContext* cx, JS::GCNurseryCollectionCallback callback, void* data) {
  auto& list = StateOf(cx->runtime).nurseryCallbacks;
  for (auto it = list.begin(); it != list.end(); ++it) {
    if (it->first == callback && it->second == data) {
      list.erase(it);
      return;
    }
  }
}

JS_PUBLIC_API void JS::SetCreateGCSliceBudgetCallback(
    JSContext* cx, JS::CreateSliceBudgetCallback cb) {
  StateOf(cx->runtime).sliceBudgetCallback = cb;
}

JS_PUBLIC_API void JS::SetLowMemoryState(JSContext* cx, bool newState) {
  GcState& state = StateOf(cx->runtime);
  state.lowMemory = newState;
  cx->isolate->MemoryPressureNotification(
      newState ? v8::MemoryPressureLevel::kModerate
               : v8::MemoryPressureLevel::kNone);
}

JS_PUBLIC_API bool JS_AddExtraGCRootsTracer(JSContext* cx,
                                            JSTraceDataOp traceOp,
                                            void* data) {
  EnsureHooks(cx);
  cx->runtime->blackRootTracers.emplace_back(traceOp, data);
  return true;
}

JS_PUBLIC_API void JS_RemoveExtraGCRootsTracer(JSContext* cx,
                                               JSTraceDataOp traceOp,
                                               void* data) {
  auto& list = cx->runtime->blackRootTracers;
  for (auto it = list.begin(); it != list.end(); ++it) {
    if (it->first == traceOp && it->second == data) {
      list.erase(it);
      return;
    }
  }
}

JS_PUBLIC_API void JS_GC(JSContext* cx, JS::GCReason reason) {
  RunFullGC(cx, JS::GCOptions::Normal, reason);
}

JS_PUBLIC_API void JS_MaybeGC(JSContext* cx) {
  EnsureHooks(cx);
  GcState& state = StateOf(cx->runtime);
  if (state.v8Marking) {
    RunFullGC(cx, state.currentOptions, JS::GCReason::EAGER_ALLOC_TRIGGER);
    return;
  }
  size_t used = V8HeapUsed(cx->isolate);
  size_t base = size_t(StoredParam(state, JSGC_ALLOCATION_THRESHOLD)) << 20;
  double growth = StoredParam(state, HighFrequency(state)
                                         ? JSGC_HIGH_FREQUENCY_SMALL_HEAP_GROWTH
                                         : JSGC_LOW_FREQUENCY_HEAP_GROWTH) /
                  100.0;
  size_t trigger =
      std::max(base, size_t(double(state.heapAfterCycle) * growth));
  if (used >= trigger) {
    RunFullGC(cx, JS::GCOptions::Normal, JS::GCReason::EAGER_ALLOC_TRIGGER);
  }
}

JS_PUBLIC_API void JS_SetGCCallback(JSContext* cx, JSGCCallback cb,
                                    void* data) {
  EnsureHooks(cx);
  GcState& state = StateOf(cx->runtime);
  state.embedderGcCallback = cb;
  state.embedderGcData = data;
}

JS_PUBLIC_API bool JS_AddFinalizeCallback(JSContext* cx, JSFinalizeCallback cb,
                                          void* data) {
  EnsureHooks(cx);
  cx->runtime->finalizeCallbacks.emplace_back(cb, data);
  return true;
}

JS_PUBLIC_API void JS_RemoveFinalizeCallback(JSContext* cx,
                                             JSFinalizeCallback cb) {
  auto& list = cx->runtime->finalizeCallbacks;
  for (auto it = list.begin(); it != list.end(); ++it) {
    if (it->first == cb) {
      list.erase(it);
      return;
    }
  }
}

JS_PUBLIC_API bool JS_AddWeakPointerZonesCallback(
    JSContext* cx, JSWeakPointerZonesCallback cb, void* data) {
  EnsureHooks(cx);
  StateOf(cx->runtime).weakZoneCallbacks.emplace_back(cb, data);
  return true;
}

JS_PUBLIC_API void JS_RemoveWeakPointerZonesCallback(
    JSContext* cx, JSWeakPointerZonesCallback cb) {
  auto& list = StateOf(cx->runtime).weakZoneCallbacks;
  for (auto it = list.begin(); it != list.end(); ++it) {
    if (it->first == cb) {
      list.erase(it);
      return;
    }
  }
}

JS_PUBLIC_API bool JS_AddWeakPointerCompartmentCallback(
    JSContext* cx, JSWeakPointerCompartmentCallback cb, void* data) {
  EnsureHooks(cx);
  StateOf(cx->runtime).weakCompartmentCallbacks.emplace_back(cb, data);
  return true;
}

JS_PUBLIC_API void JS_RemoveWeakPointerCompartmentCallback(
    JSContext* cx, JSWeakPointerCompartmentCallback cb) {
  auto& list = StateOf(cx->runtime).weakCompartmentCallbacks;
  for (auto it = list.begin(); it != list.end(); ++it) {
    if (it->first == cb) {
      list.erase(it);
      return;
    }
  }
}

JS_PUBLIC_API void JS_SetGCParameter(JSContext* cx, JSGCParamKey key,
                                     uint32_t value) {
  GcState& state = StateOf(cx->runtime);
  size_t k = size_t(key);
  if (k >= kGCParamCount) {
    return;
  }
  state.params[k] = value;
  state.paramSet[k] = true;
}

JS_PUBLIC_API void JS_ResetGCParameter(JSContext* cx, JSGCParamKey key) {
  GcState& state = StateOf(cx->runtime);
  size_t k = size_t(key);
  if (k < kGCParamCount) {
    state.paramSet[k] = false;
    state.params[k] = 0;
  }
}

JS_PUBLIC_API uint32_t JS_GetGCParameter(JSContext* cx, JSGCParamKey key) {
  GcState& state = StateOf(cx->runtime);
  v8::Isolate* isolate = cx->isolate;
  switch (key) {
    case JSGC_BYTES:
      return uint32_t(std::min<size_t>(V8HeapUsed(isolate), UINT32_MAX));
    case JSGC_NURSERY_BYTES:
      return uint32_t(SpaceSize(isolate, "new_space", false));
    case JSGC_NUMBER:
      return uint32_t(state.majorNumber + state.minorNumber);
    case JSGC_MAJOR_GC_NUMBER:
      return uint32_t(state.majorNumber);
    case JSGC_MINOR_GC_NUMBER:
      return uint32_t(state.minorNumber);
    case JSGC_SLICE_NUMBER:
      return uint32_t(state.sliceNumber);
    case JSGC_UNUSED_CHUNKS: {
      v8::HeapStatistics stats;
      isolate->GetHeapStatistics(&stats);
      size_t unused = stats.total_heap_size() - stats.used_heap_size();
      return uint32_t(unused / js::gc::ChunkSize);
    }
    case JSGC_TOTAL_CHUNKS: {
      v8::HeapStatistics stats;
      isolate->GetHeapStatistics(&stats);
      return uint32_t((stats.total_heap_size() + js::gc::ChunkSize - 1) /
                      js::gc::ChunkSize);
    }
    case JSGC_CHUNK_BYTES:
      return uint32_t(js::gc::ChunkSize);
    case JSGC_HELPER_THREAD_COUNT:
      return HelperThreadCount(state);
    case JSGC_MARKING_THREAD_COUNT:
      return std::min(HelperThreadCount(state),
                      StoredParam(state, JSGC_MAX_MARKING_THREADS));
    case JSGC_SYSTEM_PAGE_SIZE_KB: {
      SYSTEM_INFO info;
      GetSystemInfo(&info);
      return uint32_t(info.dwPageSize / 1024);
    }
    case JSGC_HIGH_FREQUENCY_MODE:
      return HighFrequency(state);
    default:
      return StoredParam(state, key);
  }
}

JS_PUBLIC_API JSString* JS_NewMaybeExternalStringLatin1(
    JSContext* cx, const JS::Latin1Char* chars, size_t length,
    const JSExternalStringCallbacks* callbacks, bool* allocatedExternal) {
  if (length <= kMaxInlineCopy) {
    *allocatedExternal = false;
    return NewCopiedLatin1(cx, chars, length);
  }
  *allocatedExternal = true;
  return NewExternalCell(cx, chars, length, true, callbacks);
}

JS_PUBLIC_API JSString* JS_NewMaybeExternalUCString(
    JSContext* cx, const char16_t* chars, size_t length,
    const JSExternalStringCallbacks* callbacks, bool* allocatedExternal) {
  if (length <= kMaxInlineCopy) {
    *allocatedExternal = false;
    return NewCopiedString(cx, chars, length);
  }
  *allocatedExternal = true;
  return NewExternalCell(cx, chars, length, false, callbacks);
}

JS_PUBLIC_API JSString* JS_NewMaybeExternalStringUTF8(
    JSContext* cx, const JS::UTF8Chars& utf8,
    const JSExternalStringCallbacks* callbacks, bool* allocatedExternal) {
  const unsigned char* begin = utf8.begin().get();
  size_t length = utf8.length();
  bool ascii = true;
  for (size_t i = 0; i < length && ascii; i++) {
    ascii = begin[i] < 0x80;
  }
  if (ascii) {
    return JS_NewMaybeExternalStringLatin1(cx, begin, length, callbacks,
                                           allocatedExternal);
  }
  *allocatedExternal = false;
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::String> str;
  if (!v8::String::NewFromUtf8(cx->isolate,
                               reinterpret_cast<const char*>(begin),
                               v8::NewStringType::kNormal, int(length))
           .ToLocal(&str)) {
    return nullptr;
  }
  return StringFromV8(cx, str);
}

JS_PUBLIC_API JS::GCReason JS::WantEagerMinorGC(JSRuntime* rt) {
  return JS::GCReason::NO_REASON;
}

JS_PUBLIC_API void JS::MaybeRunNurseryCollection(JSRuntime* rt,
                                                 JS::GCReason reason) {
  if (JS::WantEagerMinorGC(rt) != JS::GCReason::NO_REASON) {
    JS::RunNurseryCollection(rt, reason, mozilla::TimeDuration());
  }
}

JS_PUBLIC_API void JS::RunNurseryCollection(
    JSRuntime* rt, JS::GCReason reason,
    mozilla::TimeDuration aSinceLastMinorGC) {
  GcState& state = StateOf(rt);
  if (!state.lastMinorEnd.IsNull() &&
      mozilla::TimeStamp::Now() - state.lastMinorEnd < aSinceLastMinorGC) {
    return;
  }
  if (reason == JS::GCReason::PREPARE_FOR_PAGELOAD) {
    rt->isolate->SetRAILMode(v8::PERFORMANCE_LOAD);
  }
}

JS_PUBLIC_API void JS::SetHostCleanupFinalizationRegistryCallback(
    JSContext* cx, JSHostCleanupFinalizationRegistryCallback cb, void* data) {
  GcState& state = StateOf(cx->runtime);
  state.cleanupCallback = cb;
  state.cleanupData = data;
}

JS_PUBLIC_API void JS::ClearKeptObjects(JSContext* cx) {
  cx->isolate->ClearKeptObjects();
}

JS_PUBLIC_API bool JS::AtomsZoneIsCollecting(JSRuntime* runtime) {
  return false;
}

JS_PUBLIC_API bool JS::IsAtomsZone(JS::Zone* zone) {
  return JS::shadow::Zone::from(zone)->isAtomsZone();
}

JS_PUBLIC_API JSObject* js::gc::NewMemoryInfoObject(JSContext* cx) {
  GcState& state = StateOf(cx->runtime);
  v8::HeapStatistics stats;
  cx->isolate->GetHeapStatistics(&stats);
  JS::RootedObject obj(cx, JS_NewPlainObject(cx));
  if (!obj) {
    return nullptr;
  }
  if (!SetNumber(cx, obj, "gcBytes", double(stats.used_heap_size())) ||
      !SetNumber(cx, obj, "gcMaxBytes", double(stats.heap_size_limit())) ||
      !SetNumber(cx, obj, "mallocBytes", double(stats.malloced_memory())) ||
      !SetBool(cx, obj, "gcIsHighFrequencyMode", HighFrequency(state)) ||
      !SetNumber(cx, obj, "gcNumber",
                 double(state.majorNumber + state.minorNumber)) ||
      !SetNumber(cx, obj, "majorGCCount", double(state.majorNumber)) ||
      !SetNumber(cx, obj, "minorGCCount", double(state.minorNumber)) ||
      !SetNumber(cx, obj, "sliceCount", double(state.sliceNumber))) {
    return nullptr;
  }
  JS::RootedObject zone(cx, JS_NewPlainObject(cx));
  if (!zone) {
    return nullptr;
  }
  size_t base = size_t(StoredParam(state, JSGC_ALLOCATION_THRESHOLD)) << 20;
  if (!SetNumber(cx, zone, "gcBytes", double(stats.used_heap_size())) ||
      !SetNumber(cx, zone, "gcTriggerBytes",
                 double(std::max(base, state.heapAfterCycle))) ||
      !SetNumber(cx, zone, "mallocBytes", double(stats.malloced_memory())) ||
      !SetNumber(cx, zone, "gcNumber", double(state.majorNumber))) {
    return nullptr;
  }
  JS::RootedValue zoneValue(cx, JS::ObjectValue(*zone));
  if (!JS_DefineProperty(cx, obj, "zone", zoneValue, JSPROP_ENUMERATE)) {
    return nullptr;
  }
  return obj;
}

JS_PUBLIC_API JS::UniqueChars JS::MinorGcToJSON(JSContext* cx) {
  GcState& state = StateOf(cx->runtime);
  std::string json = Format(
      "{\"status\":\"%s\",\"reason\":\"%s\",\"bytes_tenured\":%zu,"
      "\"cur_capacity\":%zu,\"total_time\":%.3f,\"minor_gc_number\":%" PRIu64
      ",\"major_gc_number\":%" PRIu64 "}",
      state.minorNumber ? "complete" : "nursery empty",
      JS::ExplainGCReason(state.lastMinorReason),
      state.minorAfter > state.minorBefore
          ? size_t(0)
          : state.minorBefore - state.minorAfter,
      SpaceSize(cx->isolate, "new_space", false),
      Millis(state.lastMinorStart, state.lastMinorEnd) * 1000.0,
      state.minorNumber, state.majorNumber);
  return Dup(json);
}

char16_t* JS::GCDescription::formatSliceMessage(JSContext* cx) const {
  GcState& state = StateOf(cx->runtime);
  std::string text = Format(
      "GC Slice %" PRIu64 " - Pause: %.3fms of unlimited budget (@ %.3fms); "
      "Reason: %s; Reset: no; Times: Major GC %" PRIu64,
      state.sliceNumber, Millis(state.sliceStart, state.sliceEnd),
      Millis(state.cycleStart, state.sliceStart), JS::ExplainGCReason(reason_),
      state.majorNumber);
  return Inflate(text);
}

char16_t* JS::GCDescription::formatSummaryMessage(JSContext* cx) const {
  GcState& state = StateOf(cx->runtime);
  std::string text = Format(
      "GC %s Total Time: %.3fms; Max Pause: %.3fms; Heap Size: "
      "%zu -> %zu KB; Reason: %s; Incremental: %s",
      isZone_ ? "Zonal" : "Full", Millis(state.cycleStart, state.cycleEnd),
      Millis(state.sliceStart, state.sliceEnd), state.heapBeforeCycle / 1024,
      state.heapAfterCycle / 1024, JS::ExplainGCReason(reason_),
      state.lastWasIncremental ? "yes" : "no");
  return Inflate(text);
}

mozilla::TimeStamp JS::GCDescription::startTime(JSContext* cx) const {
  return StateOf(cx->runtime).cycleStart;
}

mozilla::TimeStamp JS::GCDescription::endTime(JSContext* cx) const {
  return StateOf(cx->runtime).cycleEnd;
}

mozilla::TimeStamp JS::GCDescription::lastSliceStart(JSContext* cx) const {
  return StateOf(cx->runtime).sliceStart;
}

mozilla::TimeStamp JS::GCDescription::lastSliceEnd(JSContext* cx) const {
  return StateOf(cx->runtime).sliceEnd;
}

JS::UniqueChars JS::GCDescription::sliceToJSONProfiler(JSContext* cx) const {
  GcState& state = StateOf(cx->runtime);
  std::string json = Format(
      "{\"slice\":%" PRIu64 ",\"pause\":%.3f,\"reason\":\"%s\","
      "\"budget\":\"unlimited\",\"major_gc_number\":%" PRIu64
      ",\"trigger_amount\":0,\"trigger_threshold\":0}",
      state.sliceNumber, Millis(state.sliceStart, state.sliceEnd),
      JS::ExplainGCReason(reason_), state.majorNumber);
  return Dup(json);
}

JS::UniqueChars JS::GCDescription::formatJSONProfiler(JSContext* cx) const {
  GcState& state = StateOf(cx->runtime);
  std::string json = Format(
      "{\"status\":\"completed\",\"timestamp\":0,\"max_pause\":%.3f,"
      "\"total_time\":%.3f,\"reason\":\"%s\",\"zones_collected\":1,"
      "\"total_zones\":1,\"total_compartments\":%zu,\"minor_gcs\":%" PRIu64
      ",\"slices\":1,\"allocated_bytes\":%zu,\"post_heap_size\":%zu,"
      "\"major_gc_number\":%" PRIu64 ",\"nonincremental_reason\":\"%s\"}",
      Millis(state.sliceStart, state.sliceEnd),
      Millis(state.cycleStart, state.cycleEnd), JS::ExplainGCReason(reason_),
      cx->runtime->realms.size(), state.minorNumber, state.heapBeforeCycle,
      state.heapAfterCycle, state.majorNumber,
      state.lastWasIncremental ? "None" : "NonIncrementalRequested");
  return Dup(json);
}

JS::dbg::GarbageCollectionEvent::Ptr JS::dbg::GarbageCollectionEvent::Create(
    JSRuntime* rt, js::gcstats::Statistics& stats, uint64_t majorGCNumber) {
  auto data = js::MakeUnique<GarbageCollectionEvent>(majorGCNumber);
  if (!data) {
    return nullptr;
  }
  data->nonincrementalReason = stats.nonincrementalReason;
  data->reason = stats.reason;
  if (!data->collections.append(Collection{stats.start, stats.end})) {
    return nullptr;
  }
  return data;
}

JS::dbg::GarbageCollectionEvent::Ptr JS::GCDescription::toGCEvent(
    JSContext* cx) const {
  GcState& state = StateOf(cx->runtime);
  js::gcstats::Statistics stats{
      state.majorNumber, JS::ExplainGCReason(reason_),
      state.lastWasIncremental ? nullptr : "NonIncrementalRequested",
      state.cycleStart, state.cycleEnd};
  return JS::dbg::GarbageCollectionEvent::Create(cx->runtime, stats,
                                                 state.majorNumber);
}

JS::AutoAssertNoGC::AutoAssertNoGC(JSContext* maybecx)
    : cx_(maybecx ? maybecx : CurrentContext()) {
  if (cx_) {
    StateOf(cx_->runtime).noGcDepth++;
  }
}

JS::AutoAssertNoGC::~AutoAssertNoGC() { reset(); }

void JS::AutoAssertNoGC::reset() {
  if (cx_) {
    StateOf(cx_->runtime).noGcDepth--;
    cx_ = nullptr;
  }
}

JS_PUBLIC_API js::gc::ProfilerMemoryCounts js::gc::GetProfilerMemoryCounts() {
  js::gc::ProfilerMemoryCounts counts;
  JSContext* cx = CurrentContext();
  if (!cx) {
    return counts;
  }
  v8::HeapStatistics stats;
  cx->isolate->GetHeapStatistics(&stats);
  counts.bytes = stats.total_physical_size();
  GcState* state = MaybeStateOf(cx->runtime);
  counts.operations = state ? state->majorNumber + state->minorNumber : 0;
  return counts;
}

JS_PUBLIC_API void js::gc::SetPerformanceHint(JSContext* cx,
                                              js::gc::PerformanceHint hint) {
  cx->isolate->SetRAILMode(hint == js::gc::PerformanceHint::InPageLoad
                               ? v8::PERFORMANCE_LOAD
                               : v8::PERFORMANCE_ANIMATION);
}

JS_PUBLIC_API void js::SetRealmIsDyingHint(JSObject* global) {
  JSContext* cx = CurrentContext();
  if (cx) {
    cx->isolate->ContextDisposedNotification(true);
  }
}
