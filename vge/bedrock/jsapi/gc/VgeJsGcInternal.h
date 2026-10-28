#ifndef vge_bedrock_jsapi_gc_VgeJsGcInternal_h
#define vge_bedrock_jsapi_gc_VgeJsGcInternal_h

#include <stdint.h>

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "VgeJsInternal.h"
#include "js/AllocationRecording.h"
#include "js/Context.h"
#include "js/GCAPI.h"
#include "js/HeapAPI.h"
#include "js/Interrupt.h"
#include "js/MemoryCallbacks.h"
#include "js/ProfilingStack.h"
#include "js/TelemetryTimers.h"
#include "js/TracingAPI.h"
#include "js/UbiNode.h"
#include "js/friend/CycleCollector.h"
#include "js/friend/UsageStatistics.h"
#include "mozilla/TimeStamp.h"
#include "v8-profiler.h"
#include "v8-unwinder.h"

namespace vge::jsapi::gc {

constexpr size_t kGCParamCount = 64;
constexpr size_t kSampleFrames = 255;

struct GrayEdge {
  JS::GCCellPtr from;
  JS::GCCellPtr to;
};

struct WeakMapEntry {
  JSObject* map;
  JS::GCCellPtr key;
  JS::GCCellPtr value;
};

struct CodeRange {
  uintptr_t start;
  size_t length;
  const char* name;
  uint64_t realmID;
  int scriptId;
};

struct GcState {
  explicit GcState(JSRuntime* aRuntime) : runtime(aRuntime) {}

  JSRuntime* runtime;
  bool hooksInstalled = false;

  JSGCCallback embedderGcCallback = nullptr;
  void* embedderGcData = nullptr;
  std::vector<std::pair<JS::GCNurseryCollectionCallback, void*>>
      nurseryCallbacks;
  JS::GCSliceCallback sliceCallback = nullptr;
  JS::CreateSliceBudgetCallback sliceBudgetCallback = nullptr;
  std::vector<std::pair<JSWeakPointerZonesCallback, void*>> weakZoneCallbacks;
  std::vector<std::pair<JSWeakPointerCompartmentCallback, void*>>
      weakCompartmentCallbacks;
  JSObjectsTenuredCallback tenuredCallback = nullptr;
  void* tenuredData = nullptr;
  JS::DoCycleCollectionCallback cycleCollectionCallback = nullptr;
  JSHostCleanupFinalizationRegistryCallback cleanupCallback = nullptr;
  void* cleanupData = nullptr;
  JSGrayRootsTracer embedderGrayTracer = nullptr;
  void* embedderGrayData = nullptr;

  uint32_t params[kGCParamCount] = {};
  bool paramSet[kGCParamCount] = {};

  v8::GCType currentType = v8::kGCTypeAll;
  bool inV8Gc = false;
  bool v8Marking = false;
  bool lastWasIncremental = false;
  bool cycleOpen = false;
  bool lowMemory = false;
  bool drivingGc = false;
  uint32_t noGcDepth = 0;
  JS::GCReason currentReason = JS::GCReason::NO_REASON;
  JS::GCOptions currentOptions = JS::GCOptions::Normal;
  uint64_t majorNumber = 0;
  uint64_t minorNumber = 0;
  uint64_t sliceNumber = 0;
  mozilla::TimeStamp cycleStart;
  mozilla::TimeStamp cycleEnd;
  mozilla::TimeStamp sliceStart;
  mozilla::TimeStamp sliceEnd;
  mozilla::TimeStamp lastMinorStart;
  mozilla::TimeStamp lastMinorEnd;
  size_t heapBeforeCycle = 0;
  size_t heapAfterCycle = 0;
  size_t minorBefore = 0;
  size_t minorAfter = 0;
  JS::GCReason lastMinorReason = JS::GCReason::NO_REASON;

  bool grayBitsValid = false;
  std::vector<JS::GCCellPtr> coloredCells;
  std::vector<JS::GCCellPtr> grayCells;
  std::unordered_map<js::gc::Cell*, std::vector<JS::GCCellPtr>> jsEdges;
  std::vector<WeakMapEntry> weakMapEntries;
  bool ubiValid = false;
  std::unordered_map<js::gc::Cell*, std::vector<JS::GCCellPtr>> ubiEdges;
  std::unordered_map<js::gc::Cell*, size_t> ubiSizes;
  std::vector<JS::GCCellPtr> ubiRoots;
  std::vector<std::pair<JSObject*, JSObject*>> weakRefTargets;

  std::atomic<bool> interruptRequested{false};
  bool interruptCallbackDisabled = false;

  JS::OutOfMemoryCallback oomCallback = nullptr;
  void* oomData = nullptr;
  JSAccumulateTelemetryDataCallback telemetryCallback = nullptr;
  JSSetUseCounterCallback useCounterCallback = nullptr;

  bool brittleMode = false;
  bool measuringExecutionTime = false;
  JS::JSTimers timers;
  mozilla::TimeStamp executionStart;

  JS::RecordAllocationsCallback recordAllocations = nullptr;
  double recordProbability = 0.0;

  void (*constructUbiNodeForDOMObject)(void*, JSObject*) = nullptr;

  bool profilingEnabled = false;
  uint64_t sampleBufferRangeStart = 0;
  void* samplerFrames[kSampleFrames] = {};
  std::mutex codeLock;
  std::vector<CodeRange> codeRanges;
  std::unordered_set<std::string> namePool;
  std::unordered_set<int> profiledScriptIds;
  v8::RegisterState sampleRegisters;
  void (*eventMarker)(mozilla::MarkerCategory, const char*,
                      const char*) = nullptr;
  void (*intervalMarker)(mozilla::MarkerCategory, const char*,
                         mozilla::TimeStamp, const char*) = nullptr;
  void (*flowMarker)(mozilla::MarkerCategory, const char*,
                     uint64_t) = nullptr;
  void (*terminatingFlowMarker)(mozilla::MarkerCategory, const char*,
                                uint64_t) = nullptr;

  bool pcCountProfiling = false;
  std::vector<int> pcCountScripts;

  size_t associatedMemory = 0;
  size_t peakTemporary = 0;
};

GcState& StateOf(JSRuntime* aRuntime);
GcState* MaybeStateOf(JSRuntime* aRuntime);
void EnsureHooks(JSContext* aCx);

void RunFullGC(JSContext* aCx, JS::GCOptions aOptions, JS::GCReason aReason);
void InvalidateGrayBits(GcState& aState);
void InvalidateUbiGraph(GcState& aState);
void ForgetDeadColoredCells(GcState& aState);
bool ComputeGrayBits(JSContext* aCx);
const v8::HeapSnapshot* TakeColoredSnapshot(
    JSContext* aCx, const v8::HeapProfiler::HeapSnapshotOptions& aOptions);
bool BuildFullCellGraph(JSContext* aCx);

js::gc::Cell* CellOf(JS::GCCellPtr aThing);
bool IsVgeCell(const void* aPtr);
CellTail* LiveTail(JS::GCCellPtr aThing);
v8::Local<v8::Value> HandleOf(v8::Isolate* aIsolate, JS::GCCellPtr aThing);
JS::GCCellPtr CellForV8Value(JSContext* aCx, v8::Local<v8::Value> aValue);
JS::TraceKind KindOfCell(const js::gc::Cell* aCell);

void SetColorBit(js::gc::Cell* aCell, js::gc::ColorBit aBit, bool aValue);
bool IsBlack(js::gc::Cell* aCell);
bool IsGray(js::gc::Cell* aCell);

void TraceObjectCellChildren(JSTracer* aTracer, ObjectCell* aCell);
void TraceCellChildren(JSTracer* aTracer, JS::GCCellPtr aThing);
void TraceBlackRoots(JSRuntime* aRuntime, JSTracer* aTracer);

bool DispatchEdge(JSTracer* aTracer, JS::GCCellPtr* aThing, const char* aName);

void MarkingBarrier(JS::GCCellPtr aThing);

const char16_t* UbiTypeNameFor(JS::TraceKind aKind);

size_t V8HeapUsed(v8::Isolate* aIsolate);

}

#endif
