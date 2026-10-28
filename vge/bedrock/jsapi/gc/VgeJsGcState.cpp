#include "VgeJsGcInternal.h"

#include "js/shadow/Zone.h"

namespace vge::jsapi::gc {

namespace {

std::mutex sStatesLock;
std::unordered_map<JSRuntime*, GcState*>& States() {
  static std::unordered_map<JSRuntime*, GcState*> states;
  return states;
}

struct ZoneBits : public JS::shadow::Zone {
  static void SetMarking(JS::Zone* aZone, bool aOn) {
    ZoneBits* zone = static_cast<ZoneBits*>(JS::shadow::Zone::from(aZone));
    zone->needsMarkingBarrier_ = aOn ? uint32_t(Incremental) : 0;
    zone->gcState_ = aOn ? MarkBlackOnly : NoGC;
  }
};

class WeakSweepTracer final : public JS::CallbackTracer {
 public:
  explicit WeakSweepTracer(JSRuntime* aRuntime)
      : JS::CallbackTracer(aRuntime, JS::TracerKind::Callback,
                           JS::WeakEdgeTraceAction::Trace) {}
  bool onChild(JS::GCCellPtr aThing, const char* aName) override {
    CellTail* tail = LiveTail(aThing);
    return tail != nullptr;
  }
};

bool IsMajor(v8::GCType aType) {
  return aType & v8::kGCTypeMarkSweepCompact;
}

bool IsMinor(v8::GCType aType) {
  return aType & (v8::kGCTypeScavenge | v8::kGCTypeMinorMarkSweep);
}

JS::GCReason ReasonFor(GcState& aState, v8::GCCallbackFlags aFlags) {
  if (aState.drivingGc) {
    return aState.currentReason;
  }
  if (aFlags & v8::kGCCallbackFlagForced) {
    return JS::GCReason::API;
  }
  if (aFlags & (v8::kGCCallbackFlagCollectAllAvailableGarbage |
                v8::kGCCallbackFlagCollectAllExternalMemory)) {
    return JS::GCReason::MEM_PRESSURE;
  }
  return JS::GCReason::ALLOC_TRIGGER;
}

void FireSlice(GcState& aState, JS::GCProgress aProgress) {
  JSContext* cx = aState.runtime->mainContext;
  if (!aState.sliceCallback || !cx) {
    return;
  }
  JS::GCDescription desc(false, aProgress == JS::GC_CYCLE_END,
                         aState.currentOptions, aState.currentReason);
  aState.sliceCallback(cx, aProgress, desc);
}

void OpenCycle(GcState& aState, v8::Isolate* aIsolate) {
  if (aState.cycleOpen) {
    return;
  }
  aState.cycleOpen = true;
  aState.cycleStart = mozilla::TimeStamp::Now();
  aState.heapBeforeCycle = V8HeapUsed(aIsolate);
  FireSlice(aState, JS::GC_CYCLE_BEGIN);
}

void BeginSlice(GcState& aState) {
  aState.sliceNumber++;
  aState.sliceStart = mozilla::TimeStamp::Now();
  FireSlice(aState, JS::GC_SLICE_BEGIN);
}

void EndSlice(GcState& aState) {
  aState.sliceEnd = mozilla::TimeStamp::Now();
  FireSlice(aState, JS::GC_SLICE_END);
}

void RunWeakPointerCallbacks(GcState& aState) {
  JSRuntime* rt = aState.runtime;
  WeakSweepTracer tracer(rt);
  for (auto& [cb, data] : aState.weakZoneCallbacks) {
    cb(&tracer, data);
  }
  if (aState.weakCompartmentCallbacks.empty()) {
    return;
  }
  std::vector<JS::Compartment*> compartments;
  for (JS::Realm* realm : rt->realms) {
    JS::Compartment* comp = JS::GetCompartmentForRealm(realm);
    bool seen = false;
    for (JS::Compartment* c : compartments) {
      seen = seen || c == comp;
    }
    if (!seen) {
      compartments.push_back(comp);
    }
  }
  for (JS::Compartment* comp : compartments) {
    for (auto& [cb, data] : aState.weakCompartmentCallbacks) {
      cb(&tracer, comp, data);
    }
  }
}

void SweepWeakPointers(JSRuntime* aRuntime) {
  GcState* state = MaybeStateOf(aRuntime);
  if (!state ||
      (!IsMajor(state->currentType) && !IsMinor(state->currentType))) {
    return;
  }
  RunWeakPointerCallbacks(*state);
}

void Trampoline(JSContext* aCx, JSGCStatus aStatus, JS::GCReason aReason,
                void* aData) {
  GcState& state = *static_cast<GcState*>(aData);
  if (aStatus != JSGC_END) {
    return;
  }
  if (!IsMajor(state.currentType) && !IsMinor(state.currentType)) {
    return;
  }
  if (IsMajor(state.currentType) && state.embedderGcCallback) {
    state.embedderGcCallback(aCx, JSGC_END, state.currentReason,
                             state.embedderGcData);
  }
}

void Prologue(v8::Isolate* aIsolate, v8::GCType aType,
              v8::GCCallbackFlags aFlags, void* aData) {
  GcState& state = *static_cast<GcState*>(aData);
  JSContext* cx = state.runtime->mainContext;
  MOZ_ASSERT(state.noGcDepth == 0, "V8 GC inside JS::AutoAssertNoGC");
  state.currentType = aType;
  state.inV8Gc = true;
  InvalidateUbiGraph(state);
  if (aType & v8::kGCTypeIncrementalMarking) {
    state.currentReason = ReasonFor(state, aFlags);
    state.v8Marking = true;
    ZoneBits::SetMarking(state.runtime->zone, true);
    OpenCycle(state, aIsolate);
    BeginSlice(state);
    return;
  }
  if (IsMajor(aType)) {
    InvalidateGrayBits(state);
    state.lastWasIncremental = state.cycleOpen;
    state.currentReason = ReasonFor(state, aFlags);
    OpenCycle(state, aIsolate);
    BeginSlice(state);
    if (state.embedderGcCallback && cx) {
      state.embedderGcCallback(cx, JSGC_BEGIN, state.currentReason,
                               state.embedderGcData);
    }
    return;
  }
  if (IsMinor(aType)) {
    state.lastMinorReason = state.drivingGc ? state.currentReason
                                            : JS::GCReason::OUT_OF_NURSERY;
    state.lastMinorStart = mozilla::TimeStamp::Now();
    state.minorBefore = V8HeapUsed(aIsolate);
    for (auto& [cb, data] : state.nurseryCallbacks) {
      if (cx) {
        cb(cx, JS::GCNurseryProgress::GC_NURSERY_COLLECTION_START,
           state.lastMinorReason, data);
      }
    }
  }
}

void Epilogue(v8::Isolate* aIsolate, v8::GCType aType,
              v8::GCCallbackFlags aFlags, void* aData) {
  GcState& state = *static_cast<GcState*>(aData);
  JSContext* cx = state.runtime->mainContext;
  state.inV8Gc = false;
  if (aType & v8::kGCTypeIncrementalMarking) {
    EndSlice(state);
    return;
  }
  if (IsMajor(aType)) {
    state.majorNumber++;
    state.v8Marking = false;
    ZoneBits::SetMarking(state.runtime->zone, false);
    EndSlice(state);
    state.cycleEnd = mozilla::TimeStamp::Now();
    state.heapAfterCycle = V8HeapUsed(aIsolate);
    FireSlice(state, JS::GC_CYCLE_END);
    state.cycleOpen = false;
    if (state.telemetryCallback) {
      state.telemetryCallback(
          JSMetric::GC_MS,
          JSTelemetryData(mozilla::TimeDuration(state.cycleEnd -
                                                state.cycleStart)));
      state.telemetryCallback(JSMetric::GC_REASON_2,
                              JSTelemetryData(size_t(state.currentReason)));
      state.telemetryCallback(JSMetric::GC_NON_INCREMENTAL,
                              JSTelemetryData(!state.lastWasIncremental));
    }
    return;
  }
  if (IsMinor(aType)) {
    state.minorNumber++;
    state.lastMinorEnd = mozilla::TimeStamp::Now();
    state.minorAfter = V8HeapUsed(aIsolate);
    ForgetDeadColoredCells(state);
    for (auto& [cb, data] : state.nurseryCallbacks) {
      if (cx) {
        cb(cx, JS::GCNurseryProgress::GC_NURSERY_COLLECTION_END,
           state.lastMinorReason, data);
      }
    }
    if (state.tenuredCallback) {
      state.tenuredCallback(reinterpret_cast<JS::GCContext*>(state.runtime),
                            state.tenuredData);
    }
    if (state.telemetryCallback) {
      state.telemetryCallback(
          JSMetric::GC_MINOR_US,
          JSTelemetryData(mozilla::TimeDuration(state.lastMinorEnd -
                                                state.lastMinorStart)));
      state.telemetryCallback(JSMetric::GC_MINOR_REASON,
                              JSTelemetryData(size_t(state.lastMinorReason)));
    }
  }
}

}

GcState* MaybeStateOf(JSRuntime* aRuntime) {
  std::lock_guard<std::mutex> lock(sStatesLock);
  auto it = States().find(aRuntime);
  return it == States().end() ? nullptr : it->second;
}

GcState& StateOf(JSRuntime* aRuntime) {
  std::lock_guard<std::mutex> lock(sStatesLock);
  GcState*& slot = States()[aRuntime];
  if (!slot) {
    slot = new GcState(aRuntime);
  }
  return *slot;
}

void EnsureHooks(JSContext* aCx) {
  JSRuntime* rt = aCx->runtime;
  GcState& state = StateOf(rt);
  if (state.hooksInstalled) {
    return;
  }
  state.hooksInstalled = true;
  rt->isolate->AddGCPrologueCallback(Prologue, &state);
  rt->isolate->AddGCEpilogueCallback(Epilogue, &state);
  rt->gcCallback = Trampoline;
  rt->gcCallbackData = &state;
  rt->weakPointerSweep = SweepWeakPointers;
}

size_t V8HeapUsed(v8::Isolate* aIsolate) {
  v8::HeapStatistics stats;
  aIsolate->GetHeapStatistics(&stats);
  return stats.used_heap_size();
}

void RunFullGC(JSContext* aCx, JS::GCOptions aOptions, JS::GCReason aReason) {
  EnsureHooks(aCx);
  GcState& state = StateOf(aCx->runtime);
  bool wasDriving = state.drivingGc;
  state.drivingGc = true;
  state.currentReason = aReason;
  state.currentOptions = aOptions;
  v8::Isolate* isolate = aCx->isolate;
  if (aOptions != JS::GCOptions::Normal) {
    isolate->LowMemoryNotification();
  } else {
    isolate->MemoryPressureNotification(v8::MemoryPressureLevel::kCritical);
    isolate->MemoryPressureNotification(
        state.lowMemory ? v8::MemoryPressureLevel::kModerate
                        : v8::MemoryPressureLevel::kNone);
  }
  ComputeGrayBits(aCx);
  state.drivingGc = wasDriving;
}

void MarkingBarrier(JS::GCCellPtr aThing) {
  if (!aThing) {
    return;
  }
  CellTail* tail = LiveTail(aThing);
  if (!tail) {
    return;
  }
  JSContext* cx = CurrentContext();
  if (!cx) {
    return;
  }
  GcState* state = MaybeStateOf(cx->runtime);
  if (!state || !state->v8Marking) {
    return;
  }
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::Value> value = tail->handle.Get(cx->isolate);
  SetTraced(cx->isolate, tail, value);
}

}
