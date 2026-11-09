#include "VgeJsGcInternal.h"

#include <algorithm>

#include "mozilla/ScopeExit.h"
#include "js/Proxy.h"
#include "js/Wrapper.h"
#include "js/friend/CycleCollector.h"

namespace vge::jsapi::gc {

namespace {

class CellCollector final : public JS::CallbackTracer {
 public:
  CellCollector(JSRuntime* aRuntime, std::vector<JS::GCCellPtr>& aOut)
      : JS::CallbackTracer(aRuntime, JS::TracerKind::Callback,
                           JS::WeakMapTraceAction::TraceKeysAndValues),
        mOut(aOut) {}

  bool onChild(JS::GCCellPtr aThing, const char* aName) override {
    if (aThing && IsVgeCell(aThing.asCell())) {
      mOut.push_back(aThing);
    }
    return true;
  }

 private:
  std::vector<JS::GCCellPtr>& mOut;
};

class HeapGraph {
 public:
  HeapGraph(JSContext* aCx, const v8::HeapSnapshot* aSnapshot)
      : mCx(aCx),
        mProfiler(aCx->isolate->GetHeapProfiler()),
        mSnapshot(aSnapshot) {
    int count = mSnapshot->GetNodesCount();
    mNodes.reserve(count);
    mIndex.reserve(count);
    for (int i = 0; i < count; i++) {
      const v8::HeapGraphNode* node = mSnapshot->GetNode(i);
      mIndex.emplace(node, uint32_t(i));
      mNodes.push_back(node);
    }
    mCellState.assign(count, 0);
    mCells.assign(count, JS::GCCellPtr());
    mStamp.assign(count, 0);
    IndexCells();
  }

  void IndexCells() {
    v8::Isolate* isolate = mCx->isolate;
    v8::HandleScope scope(isolate);
    for (ObjectCell* cell : mCx->runtime->objectCells) {
      if (cell->tail.handle.IsEmpty()) {
        continue;
      }
      v8::Local<v8::Value> value = cell->tail.handle.Get(isolate);
      JS::GCCellPtr thing(cell->asObject());
      v8::SnapshotObjectId id = mProfiler->GetObjectId(value);
      if (id != v8::HeapProfiler::kUnknownObjectId) {
        mCellById.emplace(id, thing);
      }
      const JSClass* clasp = cell->shape ? cell->shape->base.clasp : nullptr;
      if (clasp && (clasp->flags & JSCLASS_IS_GLOBAL) && value->IsObject()) {
        v8::Local<v8::Value> inner = value.As<v8::Object>()->GetPrototype();
        v8::SnapshotObjectId innerId = mProfiler->GetObjectId(inner);
        if (innerId != v8::HeapProfiler::kUnknownObjectId) {
          mCellById.emplace(innerId, thing);
        }
      }
    }
  }

  size_t size() const { return mNodes.size(); }

  int32_t IndexOfValue(v8::Local<v8::Value> aValue) {
    if (aValue.IsEmpty()) {
      return -1;
    }
    v8::SnapshotObjectId id = mProfiler->GetObjectId(aValue);
    if (id == v8::HeapProfiler::kUnknownObjectId) {
      return -1;
    }
    const v8::HeapGraphNode* node = mSnapshot->GetNodeById(id);
    if (!node) {
      return -1;
    }
    auto it = mIndex.find(node);
    return it == mIndex.end() ? -1 : int32_t(it->second);
  }

  int32_t IndexOfCell(JS::GCCellPtr aThing) {
    v8::HandleScope scope(mCx->isolate);
    return IndexOfValue(HandleOf(mCx->isolate, aThing));
  }

  JS::GCCellPtr CellAt(uint32_t aIndex) {
    if (mCellState[aIndex]) {
      return mCells[aIndex];
    }
    mCellState[aIndex] = 1;
    const v8::HeapGraphNode* node = mNodes[aIndex];
    v8::HeapGraphNode::Type type = node->GetType();
    if (type != v8::HeapGraphNode::kObject &&
        type != v8::HeapGraphNode::kClosure &&
        type != v8::HeapGraphNode::kRegExp &&
        type != v8::HeapGraphNode::kNative) {
      return JS::GCCellPtr();
    }
    auto found = mCellById.find(node->GetId());
    if (found != mCellById.end()) {
      mCells[aIndex] = found->second;
    }
    return mCells[aIndex];
  }

  int32_t RootIndex() const {
    auto it = mIndex.find(mSnapshot->GetRoot());
    return it == mIndex.end() ? -1 : int32_t(it->second);
  }

  size_t ShallowSize(uint32_t aIndex) const {
    return mNodes[aIndex]->GetShallowSize();
  }

  void CellChildren(uint32_t aStart, std::vector<uint32_t>& aOut) {
    mGeneration++;
    std::vector<uint32_t> stack;
    stack.push_back(aStart);
    mStamp[aStart] = mGeneration;
    while (!stack.empty()) {
      uint32_t n = stack.back();
      stack.pop_back();
      const v8::HeapGraphNode* node = mNodes[n];
      int children = node->GetChildrenCount();
      for (int i = 0; i < children; i++) {
        const v8::HeapGraphEdge* edge = node->GetChild(i);
        if (edge->GetType() == v8::HeapGraphEdge::kWeak) {
          continue;
        }
        auto it = mIndex.find(edge->GetToNode());
        if (it == mIndex.end()) {
          continue;
        }
        uint32_t m = it->second;
        if (mStamp[m] == mGeneration) {
          continue;
        }
        mStamp[m] = mGeneration;
        if (CellAt(m)) {
          aOut.push_back(m);
          continue;
        }
        stack.push_back(m);
      }
    }
  }

 private:
  JSContext* mCx;
  v8::HeapProfiler* mProfiler;
  const v8::HeapSnapshot* mSnapshot;
  std::vector<const v8::HeapGraphNode*> mNodes;
  std::unordered_map<const v8::HeapGraphNode*, uint32_t> mIndex;
  std::vector<uint8_t> mCellState;
  std::vector<JS::GCCellPtr> mCells;
  std::vector<uint32_t> mStamp;
  std::unordered_map<v8::SnapshotObjectId, JS::GCCellPtr> mCellById;
  uint32_t mGeneration = 0;
};

void Color(GcState& aState, JS::GCCellPtr aThing, bool aGray) {
  js::gc::Cell* cell = aThing.asCell();
  if (aGray) {
    SetColorBit(cell, js::gc::ColorBit::BlackBit, false);
    SetColorBit(cell, js::gc::ColorBit::GrayOrBlackBit, true);
    aState.grayCells.push_back(aThing);
  } else {
    SetColorBit(cell, js::gc::ColorBit::BlackBit, true);
  }
  aState.coloredCells.push_back(aThing);
}

void ClearColor(JS::GCCellPtr aThing) {
  SetColorBit(aThing.asCell(), js::gc::ColorBit::BlackBit, false);
  SetColorBit(aThing.asCell(), js::gc::ColorBit::GrayOrBlackBit, false);
}

uint64_t GcCount(GcState& aState) {
  return aState.majorNumber + aState.minorNumber;
}

class GrayRootTracer final : public JS::CallbackTracer {
 public:
  GrayRootTracer(JSRuntime* aRuntime, v8::GrayRootMarker& aMarker)
      : JS::CallbackTracer(aRuntime, JS::TracerKind::Callback,
                           JS::WeakMapTraceAction::TraceKeysAndValues),
        mMarker(aMarker) {}

  bool onChild(JS::GCCellPtr aThing, const char* aName) override {
    CellTail* tail = LiveTail(aThing);
    if (tail) {
      mMarker.Mark(tail->handle);
    }
    return true;
  }

 private:
  v8::GrayRootMarker& mMarker;
};

using CellsByAddress = std::unordered_map<uintptr_t, JS::GCCellPtr>;

CellsByAddress ColorMarkedCells(GcState& aState,
                                const v8::GrayMarkingResult& aResult,
                                bool aKeepBlackCells,
                                std::vector<JS::GCCellPtr>& aCellAt) {
  CellsByAddress cells;
  if (aKeepBlackCells) {
    cells.reserve(aState.runtime->objectCells.size());
  }
  auto setCellAt = [&](uintptr_t aAddress, JS::GCCellPtr aThing) {
    uint32_t index = aResult.GrayIndexOf(aAddress);
    if (index != v8::kNotGrayIndex && !aCellAt[index]) {
      aCellAt[index] = aThing;
    }
  };
  for (ObjectCell* cell : aState.runtime->objectCells) {
    if (cell->tail.handle.IsEmpty()) {
      continue;
    }
    uintptr_t address = aResult.AddressOf(cell->tail.handle);
    uint32_t index = aResult.GrayIndexOf(address);
    bool gray = index != v8::kNotGrayIndex;
    if (!gray && !(aKeepBlackCells && aResult.IsMarked(address))) {
      continue;
    }
    JS::GCCellPtr thing(cell->asObject());
    if (gray) {
      Color(aState, thing, true);
      setCellAt(address, thing);
    }
    if (aKeepBlackCells) {
      cells.emplace(address, thing);
    }
    const JSClass* clasp = cell->shape ? cell->shape->base.clasp : nullptr;
    if (clasp && (clasp->flags & JSCLASS_IS_GLOBAL)) {
      uintptr_t inner = aResult.InnerGlobalOfProxy(address);
      if (inner) {
        setCellAt(inner, thing);
        if (aKeepBlackCells) {
          cells.emplace(inner, thing);
        }
      }
    }
  }
  return cells;
}

void CollectWeakEntries(GcState& aState, const v8::GrayMarkingResult& aResult,
                        const CellsByAddress& aCells,
                        std::vector<v8::GrayMarkingEdge>& aEdges) {
  size_t count = 0;
  const v8::GrayMarkingEphemeron* ephemerons = aResult.Ephemerons(&count);
  for (size_t i = 0; i < count; i++) {
    const v8::GrayMarkingEphemeron& entry = ephemerons[i];
    auto key = aCells.find(entry.key);
    auto value = aCells.find(entry.value);
    auto map = entry.owner ? aCells.find(entry.owner) : aCells.end();
    if (key != aCells.end() && value != aCells.end() &&
        (!entry.owner || map != aCells.end())) {
      JSObject* mapObj = entry.owner ? &map->second.as<JSObject>() : nullptr;
      aState.weakMapEntries.push_back({mapObj, key->second, value->second});
      continue;
    }
    if (aResult.IsGray(entry.key)) {
      aEdges.push_back({entry.key, entry.value});
    }
    if (aResult.IsGray(entry.table)) {
      aEdges.push_back({entry.table, entry.value});
    }
  }
  const v8::GrayMarkingWeakRef* refs = aResult.WeakRefs(&count);
  for (size_t i = 0; i < count; i++) {
    auto ref = aCells.find(refs[i].ref);
    auto target = aCells.find(refs[i].target);
    if (ref != aCells.end() && target != aCells.end()) {
      aState.weakRefTargets.emplace_back(&ref->second.as<JSObject>(),
                                         &target->second.as<JSObject>());
    }
  }
}

void CollectCellEdges(GcState& aState, const v8::GrayMarkingResult& aResult,
                      const std::vector<JS::GCCellPtr>& aCellAt,
                      std::vector<v8::GrayMarkingEdge>& aEdges) {
  size_t nodeCount = aCellAt.size();
  auto indexOf = [&](uintptr_t aAddress) {
    return aResult.GrayIndexOf(aAddress);
  };
  std::vector<uint32_t> offsets(nodeCount + 1, 0);
  for (const v8::GrayMarkingEdge& edge : aEdges) {
    offsets[indexOf(edge.from) + 1]++;
  }
  for (size_t i = 0; i < nodeCount; i++) {
    offsets[i + 1] += offsets[i];
  }
  std::vector<uint32_t> targets(aEdges.size());
  std::vector<uint32_t> fill(offsets.begin(), offsets.end() - 1);
  for (const v8::GrayMarkingEdge& edge : aEdges) {
    targets[fill[indexOf(edge.from)]++] = indexOf(edge.to);
  }
  const std::vector<JS::GCCellPtr>& cellAt = aCellAt;
  std::vector<uint32_t> stamp(nodeCount, 0);
  std::vector<uint32_t> stack;
  uint32_t generation = 0;
  for (uint32_t start = 0; start < nodeCount; start++) {
    JS::GCCellPtr from = cellAt[start];
    if (!from) {
      continue;
    }
    generation++;
    stamp[start] = generation;
    stack.push_back(start);
    std::vector<JS::GCCellPtr> edges;
    while (!stack.empty()) {
      uint32_t n = stack.back();
      stack.pop_back();
      for (uint32_t e = offsets[n]; e < offsets[n + 1]; e++) {
        uint32_t m = targets[e];
        if (stamp[m] == generation) {
          continue;
        }
        stamp[m] = generation;
        if (!cellAt[m]) {
          stack.push_back(m);
        } else if (cellAt[m] != from) {
          edges.push_back(cellAt[m]);
        }
      }
    }
    if (!edges.empty()) {
      aState.jsEdges[from.asCell()] = std::move(edges);
    }
  }
}

class CellGrayMarkingClient final : public v8::GrayMarkingClient {
 public:
  explicit CellGrayMarkingClient(GcState& aState) : mState(aState) {}

  void MarkGrayRoots(v8::GrayRootMarker& aMarker) override {
    JSRuntime* rt = mState.runtime;
    if (!rt->grayRootTracer) {
      return;
    }
    GrayRootTracer tracer(rt, aMarker);
    JS::SliceBudget budget = JS::SliceBudget::unlimited();
    rt->grayRootTracer(&tracer, budget, rt->grayRootTracerData);
  }

  void GrayMarkingFinished(const v8::GrayMarkingResult& aResult) override {
    size_t ephemeronCount = 0;
    size_t weakRefCount = 0;
    aResult.Ephemerons(&ephemeronCount);
    aResult.WeakRefs(&weakRefCount);
    size_t nodeCount = 0;
    aResult.GrayObjects(&nodeCount);
    std::vector<JS::GCCellPtr> cellAt(nodeCount);
    CellsByAddress cells = ColorMarkedCells(
        mState, aResult, ephemeronCount || weakRefCount, cellAt);
    size_t edgeCount = 0;
    const v8::GrayMarkingEdge* edges = aResult.Edges(&edgeCount);
    std::vector<v8::GrayMarkingEdge> allEdges(edges, edges + edgeCount);
    CollectWeakEntries(mState, aResult, cells, allEdges);
    CollectCellEdges(mState, aResult, cellAt, allEdges);
    mState.grayBitsValid = true;
  }

 private:
  GcState& mState;
};

}

void InstallGrayMarkingClient(GcState& aState) {
  aState.grayMarkingClient = std::make_unique<CellGrayMarkingClient>(aState);
  v8::SetGrayMarkingClient(aState.runtime->isolate,
                           aState.grayMarkingClient.get());
}

void InvalidateGrayBits(GcState& aState) {
  for (JS::GCCellPtr thing : aState.coloredCells) {
    if (IsVgeCell(thing.asCell())) {
      ClearColor(thing);
    }
  }
  aState.coloredCells.clear();
  aState.grayCells.clear();
  aState.jsEdges.clear();
  aState.weakMapEntries.clear();
  aState.weakRefTargets.clear();
  aState.grayBitsValid = false;
}

void InvalidateUbiGraph(GcState& aState) {
  aState.ubiValid = false;
  aState.ubiEdges.clear();
  aState.ubiSizes.clear();
  aState.ubiRoots.clear();
}

bool BuildFullCellGraph(JSContext* aCx) {
  EnsureHooks(aCx);
  GcState& state = StateOf(aCx->runtime);
  JSRuntime* rt = aCx->runtime;
  v8::Isolate* isolate = aCx->isolate;
  v8::HandleScope outer(isolate);
  const v8::HeapSnapshot* snapshot =
      TakeReportSnapshot(aCx, v8::HeapProfiler::HeapSnapshotOptions());
  if (!snapshot) {
    return false;
  }
  InvalidateUbiGraph(state);
  uint64_t startCount = GcCount(state);
  HeapGraph graph(aCx, snapshot);
  auto deleteSnapshot = mozilla::MakeScopeExit(
      [&] { const_cast<v8::HeapSnapshot*>(snapshot)->Delete(); });

  std::vector<JS::GCCellPtr> embedderRoots;
  {
    CellCollector collector(rt, embedderRoots);
    TraceBlackRoots(rt, &collector);
    if (rt->grayRootTracer) {
      JS::SliceBudget budget = JS::SliceBudget::unlimited();
      rt->grayRootTracer(&collector, budget, rt->grayRootTracerData);
    }
  }

  std::vector<uint8_t> seen(graph.size(), 0);
  std::vector<uint32_t> work;
  std::vector<uint32_t> children;
  for (JS::GCCellPtr thing : embedderRoots) {
    state.ubiRoots.push_back(thing);
    int32_t index = graph.IndexOfCell(thing);
    if (index >= 0 && !seen[index]) {
      seen[index] = 1;
      work.push_back(uint32_t(index));
    }
  }
  int32_t snapshotRoot = graph.RootIndex();
  if (snapshotRoot >= 0) {
    children.clear();
    graph.CellChildren(uint32_t(snapshotRoot), children);
    for (uint32_t m : children) {
      JS::GCCellPtr cell = graph.CellAt(m);
      state.ubiRoots.push_back(cell);
      if (!seen[m]) {
        seen[m] = 1;
        work.push_back(m);
      }
    }
  }
  while (!work.empty()) {
    uint32_t n = work.back();
    work.pop_back();
    JS::GCCellPtr from = graph.CellAt(n);
    if (!from) {
      continue;
    }
    state.ubiSizes[from.asCell()] = graph.ShallowSize(n);
    children.clear();
    graph.CellChildren(n, children);
    std::vector<JS::GCCellPtr>& edges = state.ubiEdges[from.asCell()];
    for (uint32_t m : children) {
      edges.push_back(graph.CellAt(m));
      if (!seen[m]) {
        seen[m] = 1;
        work.push_back(m);
      }
    }
  }
  if (GcCount(state) != startCount) {
    InvalidateUbiGraph(state);
    return false;
  }
  state.ubiValid = true;
  return true;
}

const v8::HeapSnapshot* TakeReportSnapshot(
    JSContext* aCx, const v8::HeapProfiler::HeapSnapshotOptions& aOptions) {
  EnsureHooks(aCx);
  return aCx->isolate->GetHeapProfiler()->TakeHeapSnapshot(aOptions);
}

}

using namespace vge::jsapi;
using namespace vge::jsapi::gc;

namespace {

class UnmarkGrayTracer final : public JS::CallbackTracer {
 public:
  explicit UnmarkGrayTracer(JSRuntime* aRuntime)
      : JS::CallbackTracer(aRuntime, JS::TracerKind::UnmarkGray,
                           JS::WeakMapTraceAction::TraceKeysAndValues) {}

  bool onChild(JS::GCCellPtr aThing, const char* aName) override {
    if (aThing && IsVgeCell(aThing.asCell()) && IsGray(aThing.asCell())) {
      SetColorBit(aThing.asCell(), js::gc::ColorBit::BlackBit, true);
      mUnmarked = true;
      mStack.push_back(aThing);
    }
    return true;
  }

  bool Run(JS::GCCellPtr aThing) {
    onChild(aThing, "root");
    while (!mStack.empty()) {
      JS::GCCellPtr next = mStack.back();
      mStack.pop_back();
      TraceCellChildren(this, next);
    }
    return mUnmarked;
  }

 private:
  std::vector<JS::GCCellPtr> mStack;
  bool mUnmarked = false;
};

}

JS_PUBLIC_API bool JS::UnmarkGrayGCThingRecursively(JS::GCCellPtr thing) {
  if (!thing || !IsVgeCell(thing.asCell())) {
    return false;
  }
  JSRuntime* rt = vge::jsapi::Runtime();
  if (!rt) {
    return false;
  }
  UnmarkGrayTracer tracer(rt);
  return tracer.Run(thing);
}

JS_PUBLIC_API bool js::gc::detail::CanCheckGrayBits(
    const js::gc::TenuredCell* cell) {
  JSRuntime* rt = vge::jsapi::Runtime();
  GcState* state = rt ? MaybeStateOf(rt) : nullptr;
  return state && state->grayBitsValid && !state->v8Marking;
}

JS_PUBLIC_API bool js::gc::detail::CellIsMarkedGrayIfKnown(
    const js::gc::TenuredCell* cell) {
  js::gc::Cell* c =
      reinterpret_cast<js::gc::Cell*>(const_cast<js::gc::TenuredCell*>(cell));
  if (!IsVgeCell(c) || !IsGray(c)) {
    return false;
  }
  return CanCheckGrayBits(cell);
}

JS_PUBLIC_API void JS_SetGrayGCRootsTracer(JSContext* cx,
                                           JSGrayRootsTracer traceOp,
                                           void* data) {
  EnsureHooks(cx);
  GcState& state = StateOf(cx->runtime);
  state.embedderGrayTracer = traceOp;
  state.embedderGrayData = data;
  cx->runtime->grayRootTracer = traceOp;
  cx->runtime->grayRootTracerData = data;
}

JS_PUBLIC_API void JS_SetObjectsTenuredCallback(JSContext* cx,
                                                JSObjectsTenuredCallback cb,
                                                void* data) {
  EnsureHooks(cx);
  GcState& state = StateOf(cx->runtime);
  state.tenuredCallback = cb;
  state.tenuredData = data;
}

JS_PUBLIC_API JS::DoCycleCollectionCallback JS::SetDoCycleCollectionCallback(
    JSContext* cx, JS::DoCycleCollectionCallback callback) {
  GcState& state = StateOf(cx->runtime);
  JS::DoCycleCollectionCallback prior = state.cycleCollectionCallback;
  state.cycleCollectionCallback = callback;
  return prior;
}

JS_PUBLIC_API void JS::MaybeClearWeakRefTargets(
    JSRuntime* runtime, JS::ShouldClearWeakRefTargetCallback callback,
    void* data) {
  GcState& state = StateOf(runtime);
  std::vector<std::pair<JSObject*, JSObject*>> kept;
  for (auto& [ref, target] : state.weakRefTargets) {
    if (!callback(JS::GCCellPtr(target), data)) {
      kept.emplace_back(ref, target);
    }
  }
  state.weakRefTargets = std::move(kept);
}

JS_PUBLIC_API void js::TraceWeakMaps(js::WeakMapTracer* trc) {
  GcState& state = StateOf(trc->runtime);
  for (const WeakMapEntry& entry : state.weakMapEntries) {
    trc->trace(entry.map, entry.key, entry.value);
  }
}

JS_PUBLIC_API bool js::AreGCGrayBitsValid(JSRuntime* rt) {
  GcState* state = MaybeStateOf(rt);
  return state && state->grayBitsValid;
}

JS_PUBLIC_API bool js::ZoneGlobalsAreAllGray(JS::Zone* zone) {
  JSRuntime* rt = JS::shadow::Zone::from(zone)->runtimeFromAnyThread();
  for (JS::Realm* realm : rt->realms) {
    if (realm->contextDead) {
      continue;
    }
    JSObject* global = realm->global;
    if (!global || !IsGray(reinterpret_cast<js::gc::Cell*>(global))) {
      return false;
    }
  }
  return true;
}

JS_PUBLIC_API void js::TraceGrayWrapperTargets(JSTracer* trc, JS::Zone* zone) {
  JSRuntime* rt = JS::shadow::Zone::from(zone)->runtimeFromAnyThread();
  GcState& state = StateOf(rt);
  std::vector<JS::GCCellPtr> gray = state.grayCells;
  for (JS::GCCellPtr thing : gray) {
    if (!thing.is<JSObject>()) {
      continue;
    }
    JSObject* wrapper = &thing.as<JSObject>();
    if (!js::IsCrossCompartmentWrapper(wrapper)) {
      continue;
    }
    JSObject* target = js::GetProxyTargetObject(wrapper);
    if (target && IsVgeCell(target) &&
        IsGray(reinterpret_cast<js::gc::Cell*>(target))) {
      js::UnsafeTraceManuallyBarrieredEdge(trc, &target, "gray CCW target");
    }
  }
}

JS_PUBLIC_API void js::IterateGrayObjects(JS::Zone* zone,
                                          js::IterateGCThingCallback cellCallback,
                                          void* data) {
  JSRuntime* rt = JS::shadow::Zone::from(zone)->runtimeFromAnyThread();
  GcState& state = StateOf(rt);
  JS::AutoCheckCannotGC nogc;
  std::vector<JS::GCCellPtr> gray = state.grayCells;
  for (JS::GCCellPtr thing : gray) {
    if (thing.is<JSObject>() && IsGray(thing.asCell()) && LiveTail(thing)) {
      cellCallback(data, thing, nogc);
    }
  }
}
