#include "VgeJsGcInternal.h"

#include <algorithm>
#include <thread>

#include "js/MemoryMetrics.h"

namespace vge::jsapi::gc {

namespace {

std::vector<JS::Compartment*> Compartments(JSRuntime* aRuntime) {
  std::vector<JS::Compartment*> out;
  for (JS::Realm* realm : aRuntime->realms) {
    JS::Compartment* comp = JS::GetCompartmentForRealm(realm);
    if (std::find(out.begin(), out.end(), comp) == out.end()) {
      out.push_back(comp);
    }
  }
  return out;
}

class SnapshotIndex {
 public:
  explicit SnapshotIndex(const v8::HeapSnapshot* aSnapshot)
      : mSnapshot(aSnapshot) {
    int count = aSnapshot->GetNodesCount();
    mNodes.reserve(count);
    for (int i = 0; i < count; i++) {
      const v8::HeapGraphNode* node = aSnapshot->GetNode(i);
      mIndex.emplace(node, uint32_t(i));
      mNodes.push_back(node);
    }
  }
  ~SnapshotIndex() { const_cast<v8::HeapSnapshot*>(mSnapshot)->Delete(); }

  int32_t Find(const v8::HeapGraphNode* aNode) const {
    auto it = mIndex.find(aNode);
    return it == mIndex.end() ? -1 : int32_t(it->second);
  }

  const v8::HeapSnapshot* mSnapshot;
  std::vector<const v8::HeapGraphNode*> mNodes;
  std::unordered_map<const v8::HeapGraphNode*, uint32_t> mIndex;
};

void Attribute(JSContext* aCx, SnapshotIndex& aIndex,
               const std::vector<JS::Realm*>& aRealms,
               std::vector<int32_t>& aOwner) {
  v8::HeapProfiler* profiler = aCx->isolate->GetHeapProfiler();
  aOwner.assign(aIndex.mNodes.size(), -1);
  std::vector<uint32_t> frontier;
  for (size_t r = 0; r < aRealms.size(); r++) {
    JSObject* global = aRealms[r]->global;
    if (!global) {
      continue;
    }
    v8::HandleScope scope(aCx->isolate);
    v8::Local<v8::Value> handle = HandleOf(aCx->isolate, JS::GCCellPtr(global));
    if (handle.IsEmpty()) {
      continue;
    }
    const v8::HeapGraphNode* node =
        aIndex.mSnapshot->GetNodeById(profiler->GetObjectId(handle));
    int32_t idx = node ? aIndex.Find(node) : -1;
    if (idx >= 0 && aOwner[idx] < 0) {
      aOwner[idx] = int32_t(r);
      frontier.push_back(uint32_t(idx));
    }
  }
  while (!frontier.empty()) {
    std::vector<uint32_t> next;
    for (uint32_t n : frontier) {
      const v8::HeapGraphNode* node = aIndex.mNodes[n];
      int children = node->GetChildrenCount();
      for (int i = 0; i < children; i++) {
        const v8::HeapGraphEdge* edge = node->GetChild(i);
        if (edge->GetType() == v8::HeapGraphEdge::kWeak) {
          continue;
        }
        int32_t m = aIndex.Find(edge->GetToNode());
        if (m >= 0 && aOwner[m] < 0) {
          aOwner[m] = aOwner[n];
          next.push_back(uint32_t(m));
        }
      }
    }
    frontier = std::move(next);
  }
}

size_t Measure(JSContext* aCx, JS::ObjectPrivateVisitor* aOpv,
               const std::vector<JS::Realm*>& aRealms, JS::ZoneStats& aZone,
               JS::RealmStats* aRealmStats) {
  v8::Isolate* isolate = aCx->isolate;
  v8::HandleScope scope(isolate);
  v8::HeapProfiler* profiler = isolate->GetHeapProfiler();
  const v8::HeapSnapshot* snapshot =
      TakeColoredSnapshot(aCx, v8::HeapProfiler::HeapSnapshotOptions());
  if (!snapshot) {
    return 0;
  }
  SnapshotIndex index(snapshot);
  std::vector<int32_t> owner;
  Attribute(aCx, index, aRealms, owner);
  size_t gcThings = 0;
  for (size_t n = 0; n < index.mNodes.size(); n++) {
    const v8::HeapGraphNode* node = index.mNodes[n];
    size_t size = node->GetShallowSize();
    int32_t realmIndex = owner[n] >= 0 ? owner[n] : (aRealms.empty() ? -1 : 0);
    JS::RealmStats* realm = realmIndex >= 0 ? &aRealmStats[realmIndex] : nullptr;
    v8::HeapGraphNode::Type type = node->GetType();
    if (type == v8::HeapGraphNode::kNative && (node->GetId() & 1)) {
      type = v8::HeapGraphNode::kObject;
    }
    switch (type) {
      case v8::HeapGraphNode::kString:
      case v8::HeapGraphNode::kConsString:
      case v8::HeapGraphNode::kSlicedString: {
        v8::Local<v8::String> name = node->GetName();
        if (name->IsOneByte()) {
          aZone.stringInfo.gcHeapLatin1 += size;
        } else {
          aZone.stringInfo.gcHeapTwoByte += size;
        }
        gcThings += size;
        break;
      }
      case v8::HeapGraphNode::kSymbol:
        aZone.symbolsGCHeap += size;
        gcThings += size;
        break;
      case v8::HeapGraphNode::kBigInt:
        aZone.bigIntsGCHeap += size;
        gcThings += size;
        break;
      case v8::HeapGraphNode::kCode:
        aZone.jitCodesGCHeap += size;
        gcThings += size;
        break;
      case v8::HeapGraphNode::kObjectShape:
        aZone.shapeInfo.shapesGCHeapShared += size;
        gcThings += size;
        break;
      case v8::HeapGraphNode::kNative:
        if (realm) {
          realm->classInfo.objectsMallocHeapMisc += size;
        } else {
          aZone.zoneObject += size;
        }
        break;
      case v8::HeapGraphNode::kSynthetic:
        break;
      default: {
        if (!realm) {
          aZone.compartmentObjects += size;
          break;
        }
        realm->classInfo.objectsGCHeap += size;
        gcThings += size;
        if (aOpv && type == v8::HeapGraphNode::kObject) {
          v8::HandleScope inner(isolate);
          JS::GCCellPtr cell =
              CellForV8Value(aCx, profiler->FindObjectById(node->GetId()));
          nsISupports* iface = nullptr;
          if (cell && aOpv->getISupports_(&cell.as<JSObject>(), &iface) &&
              iface) {
            realm->objectsPrivate += aOpv->sizeOfIncludingThis(iface);
          }
        }
        break;
      }
    }
  }
  return gcThings;
}

}

}

using namespace vge::jsapi;
using namespace vge::jsapi::gc;

void JS::ZoneStats::initStrings() {
  isTotals = false;
  allStrings.emplace();
}

void JS::RealmStats::initClasses() {
  isTotals = false;
  allClasses.emplace();
}

JS_PUBLIC_API bool JS::CollectGlobalStats(JS::GlobalStats* gStats) {
  unsigned threads = std::max(1u, std::thread::hardware_concurrency());
  gStats->helperThread.idleThreadCount = threads > 1 ? threads - 1 : 1;
  gStats->helperThread.activeThreadCount = 0;
  return true;
}

JS_PUBLIC_API bool JS::CollectRuntimeStats(JSContext* cx,
                                           JS::RuntimeStats* rtStats,
                                           JS::ObjectPrivateVisitor* opv,
                                           bool anonymize) {
  JSRuntime* rt = cx->runtime;
  std::vector<JS::Realm*> realms = rt->realms;
  if (!rtStats->realmStatsVector.reserve(realms.size()) ||
      !rtStats->zoneStatsVector.reserve(1)) {
    return false;
  }
  for (size_t i = 0; i < realms.size(); i++) {
    rtStats->realmStatsVector.infallibleAppend(JS::RealmStats());
  }
  rtStats->zoneStatsVector.infallibleAppend(JS::ZoneStats());
  JS::ZoneStats& zone = rtStats->zoneStatsVector.back();
  size_t gcThings = Measure(cx, opv, realms, zone,
                            rtStats->realmStatsVector.begin());

  JS::AutoCheckCannotGC nogc;
  zone.initStrings();
  zone.allStrings.reset();
  rtStats->currZoneStats = &zone;
  rtStats->initExtraZoneStats(rt->zone, &zone, nogc);
  for (size_t i = 0; i < realms.size(); i++) {
    JS::RealmStats& stats = rtStats->realmStatsVector[i];
    stats.initClasses();
    stats.allClasses.reset();
    stats.realmObject += rtStats->mallocSizeOf_(realms[i]);
    rtStats->initExtraRealmStats(realms[i], &stats, nogc);
  }

  v8::HeapStatistics heap;
  cx->isolate->GetHeapStatistics(&heap);
  rtStats->runtime.object = rtStats->mallocSizeOf_(rt);
  rtStats->runtime.contexts = rtStats->mallocSizeOf_(cx);
  rtStats->runtime.temporary = heap.malloced_memory();

  rtStats->zTotals.addSizes(zone);
  for (size_t i = 0; i < realms.size(); i++) {
    rtStats->realmTotals.addSizes(rtStats->realmStatsVector[i]);
  }
  rtStats->gcHeapGCThings = rtStats->zTotals.sizeOfLiveGCThings() +
                            rtStats->realmTotals.sizeOfLiveGCThings();
  size_t total = std::max<size_t>(heap.total_heap_size(), gcThings);
  total = std::max(total, rtStats->gcHeapGCThings);
  rtStats->gcHeapChunkTotal =
      (total + js::gc::ChunkSize - 1) / js::gc::ChunkSize * js::gc::ChunkSize;
  rtStats->gcHeapDecommittedPages = 0;
  rtStats->gcHeapUnusedChunks = 0;
  rtStats->gcHeapChunkAdmin = 0;
  rtStats->gcHeapUnusedArenas =
      rtStats->gcHeapChunkTotal - rtStats->gcHeapGCThings;
  return true;
}

JS_PUBLIC_API bool JS::AddSizeOfTab(JSContext* cx, JS::Zone* zone,
                                    mozilla::MallocSizeOf mallocSizeOf,
                                    JS::ObjectPrivateVisitor* opv,
                                    JS::TabSizes* sizes,
                                    const JS::AutoRequireNoGC& nogc) {
  v8::Isolate* isolate = cx->isolate;
  size_t count = isolate->NumberOfHeapSpaces();
  for (size_t i = 0; i < count; i++) {
    v8::HeapSpaceStatistics stats;
    if (!isolate->GetHeapSpaceStatistics(&stats, i)) {
      continue;
    }
    const char* name = stats.space_name();
    if (strstr(name, "code")) {
      sizes->add(JS::TabSizes::Other, stats.space_used_size());
    } else {
      sizes->add(JS::TabSizes::Objects, stats.space_used_size());
    }
  }
  v8::HeapStatistics heap;
  isolate->GetHeapStatistics(&heap);
  sizes->add(JS::TabSizes::Other, heap.malloced_memory());
  sizes->add(JS::TabSizes::Private, heap.external_memory());
  return true;
}

JS_PUBLIC_API size_t JS::PeakSizeOfTemporary(const JSContext* cx) {
  v8::HeapStatistics heap;
  cx->isolate->GetHeapStatistics(&heap);
  return heap.peak_malloced_memory();
}

JS_PUBLIC_API size_t JS::SystemCompartmentCount(JSContext* cx) {
  size_t n = 0;
  for (JS::Compartment* comp : Compartments(cx->runtime)) {
    if (comp->systemCompartment) {
      n++;
    }
  }
  return n;
}

JS_PUBLIC_API size_t JS::UserCompartmentCount(JSContext* cx) {
  size_t n = 0;
  for (JS::Compartment* comp : Compartments(cx->runtime)) {
    if (!comp->systemCompartment) {
      n++;
    }
  }
  return n;
}

JS_PUBLIC_API size_t JS::SystemRealmCount(JSContext* cx) {
  size_t n = 0;
  for (JS::Realm* realm : cx->runtime->realms) {
    if (realm->isSystem) {
      n++;
    }
  }
  return n;
}

JS_PUBLIC_API size_t JS::UserRealmCount(JSContext* cx) {
  size_t n = 0;
  for (JS::Realm* realm : cx->runtime->realms) {
    if (!realm->isSystem) {
      n++;
    }
  }
  return n;
}

JS_PUBLIC_API size_t js::MemoryReportingSundriesThreshold() { return 8 * 1024; }
