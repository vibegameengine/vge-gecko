#include "VgeJsGcInternal.h"

#include <atomic>

#include "js/GCPolicyAPI.h"
#include "js/Id.h"
#include "js/RootingAPI.h"
#include "js/Symbol.h"
#include "js/TraceKind.h"
#include "js/Value.h"
#include "js/friend/CycleCollector.h"
#include "mozilla/HashFunctions.h"

namespace vge::jsapi::gc {

namespace {

std::atomic<uint32_t> sNextSerial{1};

void TracePersistent(JSTracer* aTracer, JS::RootKind aKind,
                     js::PersistentRootedBase* aRoot) {
  void* address = reinterpret_cast<uint8_t*>(aRoot) +
                  sizeof(mozilla::LinkedListElement<js::PersistentRootedBase>);
  switch (aKind) {
    case JS::RootKind::Object:
      if (*static_cast<JSObject**>(address)) {
        JS::TraceRoot(aTracer, static_cast<JSObject**>(address), "persistent");
      }
      break;
    case JS::RootKind::String:
      if (*static_cast<JSString**>(address)) {
        JS::TraceRoot(aTracer, static_cast<JSString**>(address), "persistent");
      }
      break;
    case JS::RootKind::Symbol:
      if (*static_cast<JS::Symbol**>(address)) {
        JS::TraceRoot(aTracer, static_cast<JS::Symbol**>(address),
                      "persistent");
      }
      break;
    case JS::RootKind::BigInt:
      if (*static_cast<JS::BigInt**>(address)) {
        JS::TraceRoot(aTracer, static_cast<JS::BigInt**>(address),
                      "persistent");
      }
      break;
    case JS::RootKind::Script:
      if (*static_cast<JSScript**>(address)) {
        JS::TraceRoot(aTracer, static_cast<JSScript**>(address), "persistent");
      }
      break;
    case JS::RootKind::Value:
      JS::TraceRoot(aTracer, static_cast<JS::Value*>(address), "persistent");
      break;
    case JS::RootKind::Id:
      JS::TraceRoot(aTracer, static_cast<jsid*>(address), "persistent");
      break;
    case JS::RootKind::Traceable:
      static_cast<js::PersistentRootedTraceableBase*>(aRoot)->trace(
          aTracer, "persistent");
      break;
    default:
      break;
  }
}

template <typename T>
bool TraceTyped(JSTracer* aTracer, T** aThingp, const char* aName);

template <>
bool TraceTyped<JSObject>(JSTracer* aTracer, JSObject** aThingp,
                          const char* aName) {
  return aTracer->onObjectEdge(aThingp, aName);
}

template <>
bool TraceTyped<JSString>(JSTracer* aTracer, JSString** aThingp,
                          const char* aName) {
  return aTracer->onStringEdge(aThingp, aName);
}

template <>
bool TraceTyped<JS::Symbol>(JSTracer* aTracer, JS::Symbol** aThingp,
                            const char* aName) {
  return aTracer->onSymbolEdge(aThingp, aName);
}

template <>
bool TraceTyped<JS::BigInt>(JSTracer* aTracer, JS::BigInt** aThingp,
                            const char* aName) {
  return aTracer->onBigIntEdge(aThingp, aName);
}

bool TraceScriptEdge(JSTracer* aTracer, JSScript** aThingp,
                     const char* aName) {
  return aTracer->onScriptEdge(reinterpret_cast<js::BaseScript**>(aThingp),
                               aName);
}

bool TraceValueEdge(JSTracer* aTracer, JS::Value* aValue, const char* aName) {
  if (!aValue->isGCThing()) {
    return true;
  }
  JS::GCCellPtr thing(*aValue);
  JS::GCCellPtr before = thing;
  bool ok = DispatchEdge(aTracer, &thing, aName);
  if (!ok) {
    aValue->setUndefined();
    return false;
  }
  if (thing == before) {
    return true;
  }
  switch (thing.kind()) {
    case JS::TraceKind::Object:
      aValue->setObject(thing.as<JSObject>());
      break;
    case JS::TraceKind::String:
      aValue->setString(&thing.as<JSString>());
      break;
    case JS::TraceKind::Symbol:
      aValue->setSymbol(&thing.as<JS::Symbol>());
      break;
    case JS::TraceKind::BigInt:
      aValue->setBigInt(&thing.as<JS::BigInt>());
      break;
    default:
      aValue->setPrivateGCThing(thing.asCell());
      break;
  }
  return true;
}

bool TraceIdEdge(JSTracer* aTracer, jsid* aId, const char* aName) {
  if (aId->isString()) {
    JSString* str = aId->toString();
    bool ok = aTracer->onStringEdge(&str, aName);
    if (!ok) {
      *aId = JS::PropertyKey::Void();
      return false;
    }
    if (str != aId->toString()) {
      *aId = JS::PropertyKey::NonIntAtom(str);
    }
    return true;
  }
  if (aId->isSymbol()) {
    JS::Symbol* sym = aId->toSymbol();
    bool ok = aTracer->onSymbolEdge(&sym, aName);
    if (!ok) {
      *aId = JS::PropertyKey::Void();
      return false;
    }
    if (sym != aId->toSymbol()) {
      *aId = JS::PropertyKey::Symbol(sym);
    }
  }
  return true;
}

}

bool DispatchEdge(JSTracer* aTracer, JS::GCCellPtr* aThing,
                  const char* aName) {
  if (!*aThing) {
    return true;
  }
  bool ok = true;
  switch (aThing->kind()) {
#define VGE_DISPATCH(name, type, _1, _2)        \
  case JS::TraceKind::name: {                   \
    type* t = &aThing->as<type>();              \
    type* before = t;                           \
    ok = aTracer->on##name##Edge(&t, aName);    \
    if (ok && t != before) {                    \
      *aThing = JS::GCCellPtr(t);               \
    }                                           \
    break;                                      \
  }
    JS_FOR_EACH_TRACEKIND(VGE_DISPATCH)
#undef VGE_DISPATCH
    default:
      break;
  }
  return ok;
}

bool IsVgeCell(const void* aPtr) { return aPtr && IsCellAddress(aPtr); }

js::gc::Cell* CellOf(JS::GCCellPtr aThing) { return aThing.asCell(); }

CellTail* LiveTail(JS::GCCellPtr aThing) {
  if (!aThing || !IsVgeCell(aThing.asCell())) {
    return nullptr;
  }
  CellTail* tail = TailOf(aThing.asCell(), aThing.kind());
  if (!tail || tail->handle.IsEmpty()) {
    return nullptr;
  }
  return tail;
}

v8::Local<v8::Value> HandleOf(v8::Isolate* aIsolate, JS::GCCellPtr aThing) {
  CellTail* tail = LiveTail(aThing);
  if (!tail) {
    return v8::Local<v8::Value>();
  }
  return tail->handle.Get(aIsolate);
}

JS::GCCellPtr CellForV8Value(JSContext* aCx, v8::Local<v8::Value> aValue) {
  if (aValue.IsEmpty() || !aValue->IsObject()) {
    return JS::GCCellPtr();
  }
  v8::Local<v8::Object> obj = aValue.As<v8::Object>();
  v8::Local<v8::Value> existing;
  v8::Local<v8::Context> ctx = aCx->v8Context();
  if (ctx.IsEmpty()) {
    for (JS::Realm* realm : aCx->runtime->realms) {
      if (!realm->context.IsEmpty()) {
        ctx = realm->context.Get(aCx->isolate);
        break;
      }
    }
  }
  if (ctx.IsEmpty()) {
    return JS::GCCellPtr();
  }
  if (!obj->GetPrivate(ctx, aCx->runtime->cellKey.Get(aCx->isolate))
           .ToLocal(&existing) ||
      !existing->IsExternal()) {
    return JS::GCCellPtr();
  }
  auto* cell =
      static_cast<ObjectCell*>(existing.As<v8::External>()->Value());
  return JS::GCCellPtr(cell->asObject());
}

void SetColorBit(js::gc::Cell* aCell, js::gc::ColorBit aBit, bool aValue) {
  auto* chunk = reinterpret_cast<js::gc::ArenaChunkBase*>(
      uintptr_t(aCell) & ~js::gc::ChunkMask);
  js::gc::AtomicBitmapWord* word;
  uintptr_t mask;
  chunk->markBits.getMarkWordAndMask(aCell, aBit, &word, &mask);
  if (aValue) {
    *word |= mask;
  } else {
    *word &= ~mask;
  }
}

bool IsBlack(js::gc::Cell* aCell) {
  auto* chunk = reinterpret_cast<js::gc::ArenaChunkBase*>(
      uintptr_t(aCell) & ~js::gc::ChunkMask);
  return chunk->markBits.isMarkedBlack(aCell);
}

bool IsGray(js::gc::Cell* aCell) {
  auto* chunk = reinterpret_cast<js::gc::ArenaChunkBase*>(
      uintptr_t(aCell) & ~js::gc::ChunkMask);
  return chunk->markBits.isMarkedGray(aCell);
}

void TraceObjectCellChildren(JSTracer* aTracer, ObjectCell* aCell) {
  Shape* shape = aCell->shape;
  if (!shape) {
    return;
  }
  JS::Value* slots = shape->isProxy
                         ? &aCell->proxyData()->values()->privateSlot
                         : aCell->fixedSlots();
  size_t count = shape->isProxy
                     ? shape->reservedSlots + 1
                     : std::min<size_t>(shape->reservedSlots, kFixedSlots);
  for (size_t i = 0; i < count; i++) {
    if (slots[i].isGCThing()) {
      js::gc::TraceExternalEdge(aTracer, &slots[i], "slot");
    }
  }
  if (shape->isProxy && aCell->proxyData()->expando) {
    js::gc::TraceExternalEdge(aTracer, &aCell->proxyData()->expando,
                              "expando");
  }
  const JSClass* clasp = shape->base.clasp;
  if (clasp && clasp->hasTrace()) {
    clasp->doTrace(aTracer, aCell->asObject());
  }
  if (shape->isProxy && aCell->proxyData()->handler) {
    aCell->proxyData()->handler->trace(aTracer, aCell->asObject());
  }
}

void TraceCellChildren(JSTracer* aTracer, JS::GCCellPtr aThing) {
  if (!aThing || !IsVgeCell(aThing.asCell())) {
    return;
  }
  if (aThing.is<JSObject>()) {
    TraceObjectCellChildren(aTracer,
                            ObjectCell::from(&aThing.as<JSObject>()));
  }
  GcState* state = aTracer->runtime() ? MaybeStateOf(aTracer->runtime())
                                      : nullptr;
  if (!state) {
    return;
  }
  std::vector<JS::GCCellPtr> edges;
  if (state->ubiValid) {
    auto it = state->ubiEdges.find(aThing.asCell());
    if (it != state->ubiEdges.end()) {
      edges = it->second;
    }
  } else if (state->grayBitsValid) {
    auto it = state->jsEdges.find(aThing.asCell());
    if (it != state->jsEdges.end()) {
      edges = it->second;
    }
  }
  for (JS::GCCellPtr& edge : edges) {
    DispatchEdge(aTracer, &edge, "js edge");
  }
}

void TraceBlackRoots(JSRuntime* aRuntime, JSTracer* aTracer) {
  if (aRuntime->mainContext) {
    JS::RootingContext* rc = JS::RootingContext::get(aRuntime->mainContext);
    rc->traceStackRoots(aTracer);
    rc->traceAllGCRooters(aTracer);
  }
  for (size_t k = 0; k < size_t(JS::RootKind::Limit); k++) {
    for (js::PersistentRootedBase* r : aRuntime->heapRoots[k]) {
      TracePersistent(aTracer, JS::RootKind(k), r);
    }
  }
  for (auto& [op, data] : aRuntime->blackRootTracers) {
    op(aTracer, data);
  }
  TraceEnteredRealmGlobals(aRuntime, aTracer);
}

JS::TraceKind KindOfCell(const js::gc::Cell* aCell) {
  JSRuntime* rt = Runtime();
  if (rt) {
    void* first = *reinterpret_cast<void* const*>(aCell);
    for (auto& [clasp, list] : rt->shapes) {
      for (Shape* s : list) {
        if (first == s) {
          return JS::TraceKind::Object;
        }
      }
    }
  }
  const StringCell* asString = reinterpret_cast<const StringCell*>(aCell);
  switch (asString->tail.kind) {
    case CellKind::String:
      return JS::TraceKind::String;
    case CellKind::Symbol:
      return JS::TraceKind::Symbol;
    case CellKind::BigInt:
      return JS::TraceKind::BigInt;
    default:
      return JS::TraceKind::Script;
  }
}

}

using namespace vge::jsapi;
using namespace vge::jsapi::gc;

#define VGE_TRACE_OBJECTLIKE(type, call)                                   \
  JS_PUBLIC_API void js::gc::TraceExternalEdge(JSTracer* trc, type* thingp, \
                                               const char* name) {          \
    if (*thingp) {                                                          \
      call;                                                                 \
    }                                                                       \
  }                                                                         \
  JS_PUBLIC_API void JS::TraceRoot(JSTracer* trc, type* thingp,             \
                                   const char* name) {                      \
    if (*thingp) {                                                          \
      call;                                                                 \
    }                                                                       \
  }

VGE_TRACE_OBJECTLIKE(JSObject*, trc->onObjectEdge(thingp, name))
VGE_TRACE_OBJECTLIKE(JSFunction*,
                     trc->onObjectEdge(reinterpret_cast<JSObject**>(thingp),
                                       name))
VGE_TRACE_OBJECTLIKE(JSString*, trc->onStringEdge(thingp, name))
VGE_TRACE_OBJECTLIKE(JSLinearString*,
                     trc->onStringEdge(reinterpret_cast<JSString**>(thingp),
                                       name))
VGE_TRACE_OBJECTLIKE(JSAtom*,
                     trc->onStringEdge(reinterpret_cast<JSString**>(thingp),
                                       name))
VGE_TRACE_OBJECTLIKE(JS::Symbol*, trc->onSymbolEdge(thingp, name))
VGE_TRACE_OBJECTLIKE(JS::BigInt*, trc->onBigIntEdge(thingp, name))
VGE_TRACE_OBJECTLIKE(JSScript*, TraceScriptEdge(trc, thingp, name))

#undef VGE_TRACE_OBJECTLIKE

JS_PUBLIC_API void js::gc::TraceExternalEdge(JSTracer* trc, JS::Value* thingp,
                                             const char* name) {
  TraceValueEdge(trc, thingp, name);
}

JS_PUBLIC_API void JS::TraceRoot(JSTracer* trc, JS::Value* thingp,
                                 const char* name) {
  TraceValueEdge(trc, thingp, name);
}

JS_PUBLIC_API void js::gc::TraceExternalEdge(JSTracer* trc, jsid* thingp,
                                             const char* name) {
  TraceIdEdge(trc, thingp, name);
}

JS_PUBLIC_API void JS::TraceRoot(JSTracer* trc, jsid* thingp,
                                 const char* name) {
  TraceIdEdge(trc, thingp, name);
}

JS_PUBLIC_API void js::UnsafeTraceManuallyBarrieredEdge(JSTracer* trc,
                                                        JSObject** thingp,
                                                        const char* name) {
  if (*thingp) {
    trc->onObjectEdge(thingp, name);
  }
}

JS_PUBLIC_API void JS::TraceChildren(JSTracer* trc, JS::GCCellPtr thing) {
  TraceCellChildren(trc, thing);
}

namespace js::gc {

template <typename T>
JS_PUBLIC_API bool TraceWeakEdge(JSTracer* trc, JS::Heap<T>* thingp) {
  T thing = thingp->unbarrieredGet();
  if (!thing) {
    return true;
  }
  if (!LiveTail(JS::GCCellPtr(thing))) {
    thingp->unbarrieredSet(nullptr);
    return false;
  }
  JS::GCCellPtr cell(thing);
  bool ok = DispatchEdge(trc, &cell, "JS::Heap edge");
  if (!ok) {
    thingp->unbarrieredSet(nullptr);
  }
  return ok;
}

template JS_PUBLIC_API bool TraceWeakEdge<JSObject*>(JSTracer*,
                                                     JS::Heap<JSObject*>*);

template <typename T>
JS_PUBLIC_API bool EdgeNeedsSweepUnbarrieredSlow(T* thingp) {
  return *thingp && !LiveTail(JS::GCCellPtr(*thingp));
}

template JS_PUBLIC_API bool EdgeNeedsSweepUnbarrieredSlow<JSObject*>(
    JSObject**);

}

JS_PUBLIC_API bool JS_UpdateWeakPointerAfterGC(JSTracer* trc,
                                               JS::Heap<JSObject*>* objp) {
  JSObject* obj = objp->unbarrieredGet();
  if (!obj) {
    return true;
  }
  if (!LiveTail(JS::GCCellPtr(obj))) {
    objp->unbarrieredSet(nullptr);
    return false;
  }
  return true;
}

JS_PUBLIC_API bool JS_UpdateWeakPointerAfterGCUnbarriered(JSTracer* trc,
                                                          JSObject** objp) {
  if (!*objp) {
    return true;
  }
  if (!LiveTail(JS::GCCellPtr(*objp))) {
    *objp = nullptr;
    return false;
  }
  return true;
}

JS_PUBLIC_API void JS::HeapObjectPostWriteBarrier(JSObject** objp,
                                                  JSObject* prev,
                                                  JSObject* next) {
  if (next) {
    MarkingBarrier(JS::GCCellPtr(next));
  }
}

JS_PUBLIC_API void JS::HeapObjectWriteBarriers(JSObject** objp, JSObject* prev,
                                               JSObject* next) {
  if (next) {
    MarkingBarrier(JS::GCCellPtr(next));
  }
}

JS_PUBLIC_API void JS::HeapStringWriteBarriers(JSString** objp, JSString* prev,
                                               JSString* next) {
  if (next) {
    MarkingBarrier(JS::GCCellPtr(next));
  }
}

JS_PUBLIC_API void JS::HeapBigIntWriteBarriers(JS::BigInt** bip,
                                               JS::BigInt* prev,
                                               JS::BigInt* next) {
  if (next) {
    MarkingBarrier(JS::GCCellPtr(next));
  }
}

JS_PUBLIC_API void JS::HeapScriptWriteBarriers(JSScript** objp, JSScript* prev,
                                               JSScript* next) {}

JS_PUBLIC_API void JS::IncrementalPreWriteBarrier(JSObject* obj) {
  if (obj) {
    MarkingBarrier(JS::GCCellPtr(obj));
  }
}

JS_PUBLIC_API void JS::IncrementalPreWriteBarrier(JS::GCCellPtr thing) {
  MarkingBarrier(thing);
}

JS_PUBLIC_API void js::gc::PerformIncrementalReadBarrier(JS::GCCellPtr thing) {
  MarkingBarrier(thing);
}

JS_PUBLIC_API bool JS::IsIncrementalBarrierNeeded(JSContext* cx) {
  GcState* state = MaybeStateOf(cx->runtime);
  return state && state->v8Marking;
}

JS_PUBLIC_API void JS::AddPersistentRoot(JS::RootingContext* cx,
                                         JS::RootKind kind,
                                         js::PersistentRootedBase* root) {
  JSContext* jcx = static_cast<JSContext*>(cx);
  jcx->runtime->heapRoots[size_t(kind)].insertBack(root);
}

JS_PUBLIC_API void JS::AddPersistentRoot(JSRuntime* rt, JS::RootKind kind,
                                         js::PersistentRootedBase* root) {
  rt->heapRoots[size_t(kind)].insertBack(root);
}

JS::TraceKind JS::GCCellPtr::outOfLineKind() const {
  return KindOfCell(asCell());
}

JS_PUBLIC_API JS::TraceKind JS::GCThingTraceKind(void* thing) {
  return KindOfCell(static_cast<js::gc::Cell*>(thing));
}

JS_PUBLIC_API JS::Zone* JS::GetTenuredGCThingZone(JS::GCCellPtr thing) {
  return js::gc::detail::GetTenuredGCThingZone(thing.asCell());
}

JS_PUBLIC_API JS::Zone* JS::GetNurseryCellZone(js::gc::Cell* cell) {
  return js::gc::detail::GetTenuredGCThingZone(cell);
}

JS_PUBLIC_API JS::Zone* JS::GetObjectZone(JSObject* obj) {
  return js::gc::detail::GetTenuredGCThingZone(obj);
}

JS_PUBLIC_API const char* JS::GCTraceKindToAscii(JS::TraceKind kind) {
  switch (kind) {
#define MAP_NAME(name, _0, _1, _2) \
  case JS::TraceKind::name:        \
    return "JS " #name;
    JS_FOR_EACH_TRACEKIND(MAP_NAME);
#undef MAP_NAME
    default:
      return "Invalid";
  }
}

JS_PUBLIC_API size_t JS::GCTraceKindSize(JS::TraceKind kind) {
  switch (kind) {
    case JS::TraceKind::Object:
      return sizeof(ObjectCell);
    case JS::TraceKind::String:
    case JS::TraceKind::Symbol:
    case JS::TraceKind::BigInt:
    case JS::TraceKind::Script:
      return sizeof(StringCell);
    default:
      return 0;
  }
}

void JS::TracingContext::getEdgeName(const char* name, char* buffer,
                                     size_t bufferSize) {
  MOZ_ASSERT(bufferSize > 0);
  if (functor_) {
    (*functor_)(this, name, buffer, bufferSize);
    return;
  }
  if (index_ != InvalidIndex) {
    snprintf(buffer, bufferSize, "%s[%zu]", name, index_);
    return;
  }
  snprintf(buffer, bufferSize, "%s", name);
}

JS::CallbackTracer::CallbackTracer(JSContext* cx, JS::TracerKind kind,
                                   JS::TraceOptions options)
    : GenericTracerImpl(cx->runtime, kind, options) {}

JS_PUBLIC_API void JS_TraceShapeCycleCollectorChildren(JSTracer* trc,
                                                       js::Shape* shape) {
  auto* vgeShape = reinterpret_cast<Shape*>(shape);
  JS::Realm* realm = vgeShape->base.realm;
  if (realm && realm->global) {
    JSObject* global = realm->global;
    JS::TraceRoot(trc, &global, "baseshape_global");
  }
}

template <>
JS_PUBLIC_API bool js::StableCellHasher<JSObject*>::maybeGetHash(
    JSObject* const& l, mozilla::HashNumber* hashOut) {
  if (!l) {
    *hashOut = 0;
    return true;
  }
  CellTail* tail = &ObjectCell::from(l)->tail;
  if (!tail->serial) {
    return false;
  }
  *hashOut = mozilla::HashGeneric(tail->serial);
  return true;
}

template <>
JS_PUBLIC_API bool js::StableCellHasher<JSObject*>::ensureHash(
    JSObject* const& l, mozilla::HashNumber* hashOut) {
  if (!l) {
    *hashOut = 0;
    return true;
  }
  CellTail* tail = &ObjectCell::from(l)->tail;
  if (!tail->serial) {
    uint32_t serial = sNextSerial++;
    if (!serial) {
      serial = sNextSerial++;
    }
    tail->serial = serial;
  }
  *hashOut = mozilla::HashGeneric(tail->serial);
  return true;
}

template <>
JS_PUBLIC_API mozilla::HashNumber js::StableCellHasher<JSObject*>::hash(
    JSObject* const& l) {
  mozilla::HashNumber hash;
  ensureHash(l, &hash);
  return hash;
}

template <>
JS_PUBLIC_API bool js::StableCellHasher<JSObject*>::match(JSObject* const& k,
                                                          JSObject* const& l) {
  if (k == l) {
    return true;
  }
  if (!k || !l) {
    return false;
  }
  uint32_t ks = ObjectCell::from(k)->tail.serial;
  uint32_t ls = ObjectCell::from(l)->tail.serial;
  return ks && ks == ls;
}

bool JS::PropertyKey::isWellKnownSymbol(JS::SymbolCode code) const {
  if (!isSymbol()) {
    return false;
  }
  return reinterpret_cast<const JS::shadow::Symbol*>(toSymbol())->code_ ==
         uint32_t(code);
}

JS::PropertyKey JS::PropertyKey::fromPinnedString(JSString* str) {
  JSContext* cx = CurrentContext();
  JSString* atom = AtomizeCell(cx, str);
  StringCell* cell = StringCell::from(atom);
  uint32_t length = cell->length();
  bool isIndex = length > 0 && length <= 10;
  uint64_t index = 0;
  for (uint32_t i = 0; isIndex && i < length; i++) {
    char16_t c = cell->latin1()
                     ? static_cast<const uint8_t*>(cell->chars)[i]
                     : static_cast<const char16_t*>(cell->chars)[i];
    if (c < '0' || c > '9' || (i == 0 && c == '0' && length > 1)) {
      isIndex = false;
      break;
    }
    index = index * 10 + (c - '0');
  }
  if (isIndex && index <= uint64_t(JS::PropertyKey::IntMax)) {
    return JS::PropertyKey::Int(int32_t(index));
  }
  return JS::PropertyKey::NonIntAtom(atom);
}
