#include "VgeJsGcInternal.h"

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
    mBlack.assign(count, 0);
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

  void MarkBlackFrom(uint32_t aStart) {
    if (mBlack[aStart]) {
      return;
    }
    std::vector<uint32_t> stack;
    mBlack[aStart] = 1;
    stack.push_back(aStart);
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
        if (it == mIndex.end() || mBlack[it->second]) {
          continue;
        }
        mBlack[it->second] = 1;
        stack.push_back(it->second);
      }
    }
  }

  void MarkBlackFromRoot() {
    const v8::HeapGraphNode* root = mSnapshot->GetRoot();
    auto it = mIndex.find(root);
    if (it == mIndex.end()) {
      return;
    }
    mBlack[it->second] = 1;
    int children = root->GetChildrenCount();
    for (int i = 0; i < children; i++) {
      const v8::HeapGraphEdge* edge = root->GetChild(i);
      const v8::HeapGraphNode* to = edge->GetToNode();
      if (edge->GetType() == v8::HeapGraphEdge::kWeak ||
          to->GetType() != v8::HeapGraphNode::kSynthetic) {
        continue;
      }
      auto child = mIndex.find(to);
      if (child != mIndex.end()) {
        MarkBlackFrom(child->second);
      }
    }
  }

  bool IsBlackNode(uint32_t aIndex) const { return mBlack[aIndex]; }

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
        if (mStamp[m] == mGeneration || mBlack[m]) {
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

  std::vector<uint32_t> NodesNamed(const char* aName) {
    std::vector<uint32_t> out;
    v8::HandleScope scope(mCx->isolate);
    for (uint32_t i = 0; i < mNodes.size(); i++) {
      const v8::HeapGraphNode* node = mNodes[i];
      if (node->GetType() != v8::HeapGraphNode::kObject) {
        continue;
      }
      v8::String::Utf8Value name(mCx->isolate, node->GetName());
      if (*name && strcmp(*name, aName) == 0) {
        out.push_back(i);
      }
    }
    return out;
  }

  v8::Local<v8::Value> ValueAt(uint32_t aIndex) {
    return mProfiler->FindObjectById(mNodes[aIndex]->GetId());
  }

 private:
  JSContext* mCx;
  v8::HeapProfiler* mProfiler;
  const v8::HeapSnapshot* mSnapshot;
  std::vector<const v8::HeapGraphNode*> mNodes;
  std::unordered_map<const v8::HeapGraphNode*, uint32_t> mIndex;
  std::vector<uint8_t> mBlack;
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

void CollectWeakMaps(JSContext* aCx, GcState& aState, HeapGraph& aGraph,
                     std::vector<uint32_t>& aWeakMapNodes,
                     std::vector<uint32_t>& aWeakRefNodes) {
  v8::Isolate* isolate = aCx->isolate;
  for (uint32_t index : aWeakMapNodes) {
    v8::HandleScope scope(isolate);
    v8::Local<v8::Value> value = aGraph.ValueAt(index);
    if (value.IsEmpty() || !value->IsWeakMap()) {
      continue;
    }
    JS::GCCellPtr map = CellForV8Value(aCx, value);
    JSObject* mapObj =
        map ? &map.as<JSObject>() : ObjectFromV8(aCx, value.As<v8::Object>());
    bool keyValue = false;
    v8::Local<v8::Array> entries;
    if (!value.As<v8::Object>()->PreviewEntries(&keyValue).ToLocal(&entries) ||
        !keyValue) {
      continue;
    }
    v8::Local<v8::Context> ctx = aCx->v8Context();
    if (ctx.IsEmpty()) {
      ctx = value.As<v8::Object>()->GetCreationContextChecked();
    }
    uint32_t length = entries->Length();
    for (uint32_t i = 0; i + 1 < length; i += 2) {
      v8::Local<v8::Value> key;
      v8::Local<v8::Value> val;
      if (!entries->Get(ctx, i).ToLocal(&key) ||
          !entries->Get(ctx, i + 1).ToLocal(&val)) {
        continue;
      }
      JS::GCCellPtr keyCell;
      if (key->IsObject()) {
        keyCell = JS::GCCellPtr(ObjectFromV8(aCx, key.As<v8::Object>()));
      } else if (key->IsSymbol()) {
        keyCell = JS::GCCellPtr(SymbolFromV8(aCx, key.As<v8::Symbol>()));
      }
      JS::GCCellPtr valueCell;
      if (val->IsObject()) {
        valueCell = JS::GCCellPtr(ObjectFromV8(aCx, val.As<v8::Object>()));
      } else if (val->IsSymbol()) {
        valueCell = JS::GCCellPtr(SymbolFromV8(aCx, val.As<v8::Symbol>()));
      }
      if (keyCell) {
        aState.weakMapEntries.push_back({mapObj, keyCell, valueCell});
      }
    }
  }
  for (uint32_t index : aWeakRefNodes) {
    v8::HandleScope scope(isolate);
    JS::GCCellPtr ref = aGraph.CellAt(index);
    v8::Local<v8::Value> value = aGraph.ValueAt(index);
    if (value.IsEmpty() || !value->IsWeakRef() || !ref) {
      continue;
    }
    std::vector<uint32_t> targets;
    aGraph.CellChildren(index, targets);
    for (uint32_t t : targets) {
      JS::GCCellPtr target = aGraph.CellAt(t);
      if (target && target.is<JSObject>()) {
        aState.weakRefTargets.emplace_back(&ref.as<JSObject>(),
                                           &target.as<JSObject>());
      }
    }
  }
}

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
      TakeColoredSnapshot(aCx, v8::HeapProfiler::HeapSnapshotOptions());
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

void ForgetDeadColoredCells(GcState& aState) {
  if (aState.coloredCells.empty()) {
    return;
  }
  std::unordered_set<js::gc::Cell*> dead;
  std::vector<JS::GCCellPtr> kept;
  for (JS::GCCellPtr thing : aState.coloredCells) {
    CellTail* tail = TailOf(thing.asCell(), thing.kind());
    if (tail && tail->handle.IsEmpty() && thing.is<JSObject>()) {
      ClearColor(thing);
      dead.insert(thing.asCell());
      continue;
    }
    kept.push_back(thing);
  }
  if (dead.empty()) {
    return;
  }
  aState.coloredCells = std::move(kept);
  std::vector<JS::GCCellPtr> gray;
  for (JS::GCCellPtr thing : aState.grayCells) {
    if (!dead.count(thing.asCell())) {
      gray.push_back(thing);
    }
  }
  aState.grayCells = std::move(gray);
  for (js::gc::Cell* cell : dead) {
    aState.jsEdges.erase(cell);
  }
  for (auto& [from, edges] : aState.jsEdges) {
    std::vector<JS::GCCellPtr> keptEdges;
    for (JS::GCCellPtr e : edges) {
      if (!dead.count(e.asCell())) {
        keptEdges.push_back(e);
      }
    }
    edges = std::move(keptEdges);
  }
  std::vector<WeakMapEntry> entries;
  for (WeakMapEntry& e : aState.weakMapEntries) {
    if (!dead.count(reinterpret_cast<js::gc::Cell*>(e.map)) &&
        !dead.count(e.key.asCell()) &&
        (!e.value || !dead.count(e.value.asCell()))) {
      entries.push_back(e);
    }
  }
  aState.weakMapEntries = std::move(entries);
}

namespace {

bool ColorFromSnapshot(JSContext* aCx, const v8::HeapSnapshot* aSnapshot) {
  GcState& state = StateOf(aCx->runtime);
  JSRuntime* rt = aCx->runtime;
  v8::HandleScope outer(aCx->isolate);
  InvalidateGrayBits(state);
  uint64_t startCount = GcCount(state);
  HeapGraph graph(aCx, aSnapshot);
  graph.MarkBlackFromRoot();

  std::vector<JS::GCCellPtr> blackRoots;
  {
    CellCollector collector(rt, blackRoots);
    TraceBlackRoots(rt, &collector);
  }
  for (JS::GCCellPtr thing : blackRoots) {
    int32_t index = graph.IndexOfCell(thing);
    if (index >= 0) {
      graph.MarkBlackFrom(uint32_t(index));
    }
  }
  std::vector<JS::GCCellPtr> grayRoots;
  if (rt->grayRootTracer) {
    CellCollector collector(rt, grayRoots);
    JS::SliceBudget budget = JS::SliceBudget::unlimited();
    rt->grayRootTracer(&collector, budget, rt->grayRootTracerData);
  }

  std::unordered_set<js::gc::Cell*> colored;
  for (JS::GCCellPtr thing : blackRoots) {
    if (colored.insert(thing.asCell()).second) {
      Color(state, thing, false);
    }
  }

  std::vector<uint32_t> work;
  std::vector<uint8_t> grayNode(graph.size(), 0);
  for (JS::GCCellPtr thing : grayRoots) {
    int32_t index = graph.IndexOfCell(thing);
    if (index < 0) {
      continue;
    }
    if (graph.IsBlackNode(uint32_t(index))) {
      if (colored.insert(thing.asCell()).second) {
        Color(state, thing, false);
      }
      continue;
    }
    if (!grayNode[index]) {
      grayNode[index] = 1;
      work.push_back(uint32_t(index));
    }
  }

  std::vector<uint32_t> children;
  while (!work.empty()) {
    uint32_t n = work.back();
    work.pop_back();
    JS::GCCellPtr from = graph.CellAt(n);
    if (!from || !LiveTail(from)) {
      continue;
    }
    if (colored.insert(from.asCell()).second) {
      Color(state, from, true);
    }
    children.clear();
    graph.CellChildren(n, children);
    std::vector<JS::GCCellPtr>& edges = state.jsEdges[from.asCell()];
    for (uint32_t m : children) {
      JS::GCCellPtr to = graph.CellAt(m);
      edges.push_back(to);
      if (!grayNode[m]) {
        grayNode[m] = 1;
        work.push_back(m);
      }
    }
  }

  std::vector<uint32_t> weakMapNodes = graph.NodesNamed("WeakMap");
  std::vector<uint32_t> weakRefNodes = graph.NodesNamed("WeakRef");

  if (GcCount(state) != startCount) {
    InvalidateGrayBits(state);
    return false;
  }
  state.grayBitsValid = true;
  CollectWeakMaps(aCx, state, graph, weakMapNodes, weakRefNodes);
  if (GcCount(state) != startCount) {
    InvalidateGrayBits(state);
    return false;
  }
  return true;
}

}

const v8::HeapSnapshot* TakeColoredSnapshot(
    JSContext* aCx, const v8::HeapProfiler::HeapSnapshotOptions& aOptions) {
  EnsureHooks(aCx);
  const v8::HeapSnapshot* snapshot =
      aCx->isolate->GetHeapProfiler()->TakeHeapSnapshot(aOptions);
  if (snapshot) {
    ColorFromSnapshot(aCx, snapshot);
  }
  return snapshot;
}

bool ComputeGrayBits(JSContext* aCx) {
  EnsureHooks(aCx);
  GcState& state = StateOf(aCx->runtime);
  v8::HandleScope outer(aCx->isolate);
  bool wasDriving = state.drivingGc;
  state.drivingGc = true;
  v8::HeapProfiler::HeapSnapshotOptions options;
  options.numerics_mode = v8::HeapProfiler::NumericsMode::kHideNumericValues;
  const v8::HeapSnapshot* snapshot = TakeColoredSnapshot(aCx, options);
  state.drivingGc = wasDriving;
  if (!snapshot) {
    return false;
  }
  const_cast<v8::HeapSnapshot*>(snapshot)->Delete();
  return state.grayBitsValid;
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
