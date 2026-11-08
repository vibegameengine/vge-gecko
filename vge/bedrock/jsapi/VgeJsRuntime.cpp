#include "VgeJsInternal.h"
#include "platform/VgeJsPlatform.h"

#include <windows.h>

#include "js/Context.h"
#include "js/GCAPI.h"
#include "js/Initialization.h"
#include "js/RootingAPI.h"
#include "js/TracingAPI.h"
#include "cppgc/heap-state.h"
#include "libplatform/libplatform.h"
#include "v8-gray-marking.h"
#include "mozilla/Maybe.h"
#include "mozilla/ThreadLocal.h"

JS::detail::InitState JS::detail::libraryInitState =
    JS::detail::InitState::Uninitialized;

namespace vge::jsapi {

namespace {

std::unique_ptr<v8::Platform> sPlatform;
MOZ_THREAD_LOCAL(JSContext*) tlsContext;

class RootMarker final : public JS::CallbackTracer {
 public:
  explicit RootMarker(JSRuntime* aRuntime)
      : JS::CallbackTracer(aRuntime, JS::TracerKind::Callback,
                           JS::WeakMapTraceAction::TraceKeysAndValues) {}

  bool onChild(JS::GCCellPtr aThing, const char* aName) override {
    CellTail* tail = TailOf(aThing.asCell(), aThing.kind());
    if (!tail || tail->rootedThisGC || tail->handle.IsEmpty()) {
      return true;
    }
    tail->rootedThisGC = true;
    tail->handle.ClearWeak();
    runtime()->strongThisGC.push_back(tail);
    return true;
  }
};

class SlotTracer final : public JS::CallbackTracer {
 public:
  SlotTracer(JSRuntime* aRuntime, cppgc::Visitor* aVisitor)
      : JS::CallbackTracer(aRuntime, JS::TracerKind::Callback,
                           JS::WeakMapTraceAction::TraceKeysAndValues),
        mVisitor(aVisitor),
        mMarking(cppgc::subtle::HeapState::IsMarking(
            aRuntime->cppHeap->GetHeapHandle())) {}

  bool onChild(JS::GCCellPtr aThing, const char* aName) override {
    CellTail* tail = TailOf(aThing.asCell(), aThing.kind());
    if (!tail) {
      return true;
    }
    if (!tail->traced.IsEmpty()) {
      static_cast<v8::JSVisitor*>(mVisitor)->Trace(tail->traced);
      if (mMarking) {
        tail->tracedEpoch = runtime()->tracedEpoch;
      }
    }
    return true;
  }

 private:
  cppgc::Visitor* mVisitor;
  bool mMarking;
};

void RefreshTracedNodes(JSRuntime* aRuntime) {
  uint8_t epoch = aRuntime->tracedEpoch;
  v8::HandleScope scope(aRuntime->isolate);
  std::vector<CellTail*> gone;
  for (CellTail* tail : aRuntime->tracedTails) {
    if (tail->tracedEpoch == epoch) {
      continue;
    }
    new (&tail->traced) v8::TracedReference<v8::Value>();
    tail->tracedEpoch = 0;
    if (tail->handle.IsEmpty()) {
      gone.push_back(tail);
      continue;
    }
    tail->traced.Reset(aRuntime->isolate, tail->handle.Get(aRuntime->isolate));
  }
  for (CellTail* tail : gone) {
    aRuntime->tracedTails.erase(tail);
  }
  aRuntime->tracedEpoch = epoch == UINT8_MAX ? 1 : epoch + 1;
}

void WeakCellCallback(const v8::WeakCallbackInfo<CellTail>& aInfo) {
  CellTail* tail = aInfo.GetParameter();
  tail->handle.Reset();
  if (tail->anchor) {
    NoteDeadAnchoredCell(tail);
    return;
  }
  switch (tail->kind) {
    case CellKind::Object:
      FreeObjectCell(reinterpret_cast<ObjectCell*>(
          reinterpret_cast<uint8_t*>(tail) - offsetof(ObjectCell, tail)));
      break;
    case CellKind::String: {
      StringCell* cell = reinterpret_cast<StringCell*>(
          reinterpret_cast<uint8_t*>(tail) - offsetof(StringCell, tail));
      FreeStringChars(cell);
      FreeCell(cell, sizeof(StringCell));
      break;
    }
    default:
      FreeCell(reinterpret_cast<uint8_t*>(tail) - offsetof(StringCell, tail),
               sizeof(StringCell));
      break;
  }
}

void MakeWeak(CellTail* aTail) {
  aTail->handle.SetWeak(aTail, WeakCellCallback,
                        v8::WeakCallbackType::kParameter);
}

void TraceStackRoots(JSContext* aCx, JSTracer* aTracer) {
  JS::RootingContext* rc = JS::RootingContext::get(aCx);
  rc->traceStackRoots(aTracer);
  rc->traceAllGCRooters(aTracer);
}

void TracePersistentRoot(JSTracer* aTracer, JS::RootKind aKind,
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

void TraceAllRoots(JSRuntime* aRuntime, JSTracer* aTracer) {
  if (aRuntime->mainContext) {
    TraceStackRoots(aRuntime->mainContext, aTracer);
    JS::TraceRoot(aTracer, &aRuntime->mainContext->pendingException,
                  "pending exception");
  }
  for (size_t k = 0; k < size_t(JS::RootKind::Limit); k++) {
    for (js::PersistentRootedBase* r : aRuntime->heapRoots[k]) {
      TracePersistentRoot(aTracer, JS::RootKind(k), r);
    }
  }
  for (auto& [op, data] : aRuntime->blackRootTracers) {
    op(aTracer, data);
  }
  TraceEnteredRealmGlobals(aRuntime, aTracer);
}

bool IsMinorGC(v8::GCType aType) {
  return aType & (v8::kGCTypeScavenge | v8::kGCTypeMinorMarkSweep);
}

void HoldYoungCells(JSRuntime* aRuntime) {
  for (CellTail* tail : aRuntime->youngTails) {
    if (!tail->handle.IsEmpty()) {
      tail->handle.ClearWeak();
    }
  }
}

void ReleaseYoungCells(JSRuntime* aRuntime) {
  for (auto it = aRuntime->youngTails.begin();
       it != aRuntime->youngTails.end();) {
    CellTail* tail = *it;
    if (tail->handle.IsEmpty()) {
      it = aRuntime->youngTails.erase(it);
      continue;
    }
    MakeWeak(tail);
    if (v8::IsGlobalInYoungGeneration(tail->handle)) {
      ++it;
    } else {
      it = aRuntime->youngTails.erase(it);
    }
  }
}

void RealmContextDied(const v8::WeakCallbackInfo<JS::Realm>& aInfo) {
  JS::Realm* realm = aInfo.GetParameter();
  realm->context.Reset();
  realm->contextDead = true;
  realm->global = nullptr;
  realm->runtime->dyingRealms.push_back(realm);
}

void RunClassFinalizer(JSRuntime* aRuntime, ObjectCell* aCell) {
  if (aCell->tail.classFinalized) {
    return;
  }
  aCell->tail.classFinalized = true;
  const JSClass* clasp = aCell->shape->base.clasp;
  if (clasp->hasFinalize()) {
    clasp->doFinalize(reinterpret_cast<JS::GCContext*>(aRuntime),
                      aCell->asObject());
  }
}

void FinalizeDeadAnchoredCells(JSRuntime* aRuntime) {
  std::vector<ObjectCell*> dead;
  dead.swap(aRuntime->deadAnchored);
  for (ObjectCell* cell : dead) {
    RunClassFinalizer(aRuntime, cell);
  }
}

void FinalizeAnchoredCell(JSRuntime* aRuntime, ObjectCell* aCell) {
  RunClassFinalizer(aRuntime, aCell);
  std::erase(aRuntime->deadAnchored, aCell);
  aCell->tail.anchor = nullptr;
  aCell->tail.handle.Reset();
  FreeObjectCell(aCell);
}

void FinalizePendingCells(JSRuntime* aRuntime) {
  while (!aRuntime->pendingFinalize.empty()) {
    std::vector<ObjectCell*> pending;
    pending.swap(aRuntime->pendingFinalize);
    for (ObjectCell* cell : pending) {
      FinalizeAnchoredCell(aRuntime, cell);
    }
  }
}

void DeleteRealmShapes(JSRuntime* aRuntime, JS::Realm* aRealm) {
  for (auto& [clasp, list] : aRuntime->shapes) {
    std::vector<Shape*> kept;
    for (Shape* shape : list) {
      if (shape->base.realm == aRealm) {
        delete shape;
      } else {
        kept.push_back(shape);
      }
    }
    list = std::move(kept);
  }
}

void DestroyRealm(JSRuntime* aRuntime, JS::Realm* aRealm,
                  bool aDestroyingRuntime) {
  JS::GCContext* gcx = reinterpret_cast<JS::GCContext*>(aRuntime);
  if (aRuntime->destroyRealmCallback) {
    aRuntime->destroyRealmCallback(gcx, aRealm);
  }
  if (aRealm->principals) {
    JS_DropPrincipals(aRuntime->mainContext, aRealm->principals);
    aRealm->principals = nullptr;
  }
  ForgetRealmClasses(aRealm);
  ForgetRealmProxies(aRealm);
  ForgetRealmScripts(aRealm);
  std::erase(aRuntime->realms, aRealm);
  std::erase(aRuntime->dyingRealms, aRealm);
  JS::Compartment* comp = aRealm->compartment();
  std::erase(comp->realms, aRealm);
  if (!aDestroyingRuntime) {
    DeleteRealmShapes(aRuntime, aRealm);
  }
  aRealm->context.Reset();
  delete aRealm;
  if (!comp->realms.empty()) {
    return;
  }
  if (aRuntime->destroyCompartmentCallback) {
    aRuntime->destroyCompartmentCallback(gcx, comp);
  }
  ForgetCompartmentClasses(comp);
  ForgetCompartmentProxies(comp);
  delete comp;
}

void SweepDyingRealms(JSRuntime* aRuntime) {
  std::vector<JS::Realm*> dead;
  for (JS::Realm* realm : aRuntime->dyingRealms) {
    if (realm->liveCells == 0 && realm->enterDepth == 0) {
      dead.push_back(realm);
    }
  }
  for (JS::Realm* realm : dead) {
    DestroyRealm(aRuntime, realm, false);
  }
}

void GCPrologue(v8::Isolate* aIsolate, v8::GCType aType,
                v8::GCCallbackFlags aFlags, void* aData) {
  JSRuntime* rt = static_cast<JSRuntime*>(aData);
  if (IsMinorGC(aType)) {
    rt->gcNumber++;
    HoldYoungCells(rt);
    return;
  }
  if (!(aType & v8::kGCTypeMarkSweepCompact) || rt->inGC) {
    return;
  }
  rt->inGC = true;
  rt->gcNumber++;
  if (rt->gcCallback && rt->mainContext) {
    rt->gcCallback(rt->mainContext, JSGC_BEGIN, JS::GCReason::API,
                   rt->gcCallbackData);
  }
  RootMarker marker(rt);
  TraceAllRoots(rt, &marker);
}

void GCEpilogue(v8::Isolate* aIsolate, v8::GCType aType,
                v8::GCCallbackFlags aFlags, void* aData) {
  JSRuntime* rt = static_cast<JSRuntime*>(aData);
  if (IsMinorGC(aType)) {
    ReleaseYoungCells(rt);
    return;
  }
  if (!(aType & v8::kGCTypeMarkSweepCompact)) {
    return;
  }
  RefreshTracedNodes(rt);
  ReleaseYoungCells(rt);
  if (!rt->inGC) {
    return;
  }
  for (CellTail* tail : rt->strongThisGC) {
    tail->rootedThisGC = false;
    if (!tail->handle.IsEmpty()) {
      MakeWeak(tail);
    }
  }
  rt->strongThisGC.clear();
  rt->inGC = false;
  if (rt->mainContext) {
    JS::GCContext* gcx = reinterpret_cast<JS::GCContext*>(rt);
    for (auto& [cb, data] : rt->finalizeCallbacks) {
      cb(gcx, JSFINALIZE_GROUP_PREPARE, data);
    }
    if (rt->weakPointerSweep) {
      rt->sweepingWeakPointers = true;
      rt->weakPointerSweep(rt);
      rt->sweepingWeakPointers = false;
    }
    for (auto& [cb, data] : rt->finalizeCallbacks) {
      cb(gcx, JSFINALIZE_GROUP_START, data);
    }
    FinalizeDeadAnchoredCells(rt);
    FinalizePendingCells(rt);
    for (auto& [cb, data] : rt->finalizeCallbacks) {
      cb(gcx, JSFINALIZE_GROUP_END, data);
      cb(gcx, JSFINALIZE_COLLECTION_END, data);
    }
    if (rt->gcCallback) {
      rt->gcCallback(rt->mainContext, JSGC_END, JS::GCReason::API,
                     rt->gcCallbackData);
    }
    SweepDyingRealms(rt);
  }
}

}

void TraceEnteredRealmGlobals(JSRuntime* aRuntime, JSTracer* aTracer) {
  for (JS::Realm* realm : aRuntime->realms) {
    if (realm->enterDepth > 0 && realm->global) {
      JS::TraceRoot(aTracer, &realm->global, "on-stack realm global");
    }
  }
}

void SetTraced(v8::Isolate* aIsolate, CellTail* aTail,
               v8::Local<v8::Value> aValue) {
  aTail->traced.Reset(aIsolate, aValue);
  aTail->tracedEpoch = 0;
  Runtime()->tracedTails.insert(aTail);
}

void NoteDeadAnchoredCell(CellTail* aTail) {
  Runtime()->deadAnchored.push_back(reinterpret_cast<ObjectCell*>(
      reinterpret_cast<uint8_t*>(aTail) - offsetof(ObjectCell, tail)));
}

void ForgetTraced(CellTail* aTail) {
  new (&aTail->traced) v8::TracedReference<v8::Value>();
  aTail->tracedEpoch = 0;
  if (JSRuntime* rt = Runtime()) {
    rt->tracedTails.erase(aTail);
    rt->youngTails.erase(aTail);
  }
}

void WatchRealmContext(JS::Realm* aRealm) {
  aRealm->context.SetWeak(aRealm, RealmContextDied,
                          v8::WeakCallbackType::kParameter);
}

Anchor::~Anchor() {
  ObjectCell* cell = mCell;
  JSRuntime* rt = Runtime();
  if (rt && rt->inGC) {
    cell->tail.anchor = nullptr;
    cell->tail.handle.Reset();
    rt->pendingFinalize.push_back(cell);
    return;
  }
  FinalizeAnchoredCell(rt, cell);
}

void Anchor::Trace(cppgc::Visitor* aVisitor) const {
  ObjectCell* cell = mCell;
  JSRuntime* rt = Runtime();
  SlotTracer tracer(rt, aVisitor);
  Shape* shape = cell->shape;
  JS::Value* slots = shape->isProxy
                         ? &cell->proxyData()->values()->privateSlot
                         : cell->fixedSlots();
  size_t count = shape->isProxy ? shape->reservedSlots + 1
                                : std::min<size_t>(shape->reservedSlots,
                                                   kFixedSlots);
  JS::Value* dynamic = DynamicSlots(cell);
  size_t dynamicCount =
      dynamic ? shape->reservedSlots - kFixedSlots : 0;
  for (size_t i = 0; i < count + dynamicCount; i++) {
    JS::Value& slot = i < count ? slots[i] : dynamic[i - count];
    if (slot.isGCThing()) {
      JS::TraceEdge(&tracer, reinterpret_cast<JS::Heap<JS::Value>*>(&slot),
                    "slot");
    }
  }
  if (shape->isProxy && cell->proxyData()->expando) {
    JS::TraceRoot(&tracer, &cell->proxyData()->expando, "expando");
  }
  const JSClass* clasp = shape->base.clasp;
  if (clasp->hasTrace()) {
    clasp->doTrace(&tracer, cell->asObject());
  }
  if (shape->isProxy) {
    cell->proxyData()->handler->trace(&tracer, cell->asObject());
  }
  static_cast<v8::JSVisitor*>(aVisitor)->Trace(mWrapper);
}

void Anchor::SetWrapper(v8::Isolate* aIsolate,
                        v8::Local<v8::Object> aWrapper) {
  mWrapper.Reset(aIsolate, aWrapper);
  mWrapper.SetWrapperClassId(kAnchorWrapperClassId);
}

JSRuntime* Runtime() {
  JSContext* cx = tlsContext.get();
  return cx ? cx->runtime : nullptr;
}

JSContext* CurrentContext() { return tlsContext.get(); }

AutoV8::AutoV8(JSContext* aCx)
    : mCx(aCx), mIsolate(aCx->isolate), mScope(aCx->isolate) {
  mContext = aCx->v8Context();
  if (!mContext.IsEmpty()) {
    mContextScope.emplace(mContext);
  }
}

AutoV8::~AutoV8() = default;

bool AutoV8::Check(const v8::TryCatch& aTry) {
  if (!aTry.HasCaught()) {
    return true;
  }
  if (aTry.HasTerminated()) {
    return false;
  }
  mCx->setPending(aTry.Exception());
  return false;
}

}

using namespace vge::jsapi;

JSRuntime::JSRuntime() = default;

JSContext::JSContext(JSRuntime* aRuntime)
    : JS::RootingContext(nullptr),
      runtime(aRuntime),
      isolate(aRuntime->isolate) {}

void JSContext::enterRealm(JS::Realm* aRealm) {
  realmStack.push_back(realm_);
  realm_ = aRealm;
  zone_ = aRealm ? runtime->zone : zone_;
  if (aRealm) {
    aRealm->enterDepth++;
    if (!aRealm->context.IsEmpty()) {
      AutoV8 v8(this);
      v8::Local<v8::Context> ctx = aRealm->context.Get(isolate);
      ctx->Enter();
    }
  }
}

void JSContext::leaveRealm(JS::Realm* aOld) {
  if (realm_) {
    MOZ_ASSERT(realm_->enterDepth > 0);
    realm_->enterDepth--;
    if (!realm_->context.IsEmpty()) {
      v8::HandleScope scope(isolate);
      realm_->context.Get(isolate)->Exit();
    }
  }
  realm_ = aOld;
  if (!realmStack.empty()) {
    realmStack.pop_back();
  }
}

v8::Local<v8::Context> JSContext::v8Context() {
  if (realm_ && !realm_->context.IsEmpty()) {
    return realm_->context.Get(isolate);
  }
  return isolate->GetCurrentContext();
}

void JSContext::setPending(v8::Local<v8::Value> aException) {
  pendingException = ValueFromV8(this, aException);
  exceptionPending = true;
}

JS::RootingContext::RootingContext(js::Nursery* aNursery)
    : nursery_(aNursery), zone_(nullptr), realm_(nullptr) {
  for (auto& head : stackRoots_) {
    head = nullptr;
  }
  for (auto& head : autoGCRooters_) {
    head = nullptr;
  }
  for (auto& limit : nativeStackLimit) {
    limit = JS::NativeStackLimitMin;
  }
}

js::GeckoProfilerThread::GeckoProfilerThread()
    : profilingStack_(nullptr), profilingStackIfEnabled_(nullptr) {}

const char* JS::detail::InitWithFailureDiagnostic(
    bool aIsDebugBuild, JS::detail::FrontendOnly aFrontendOnly) {
  if (!tlsContext.init()) {
    return "vge jsapi: thread-local context";
  }
  static const char kUnfrozenFlags[] =
      "--no-freeze-flags-after-init --no-cppheap-concurrent-marking "
      "--js-promise-withresolvers "
      "--harmony-array-from-async --harmony-iterator-helpers "
      "--harmony-set-methods --harmony-array-grouping --harmony-rab-gsab "
      "--harmony-rab-gsab-transfer --harmony-change-array-by-copy "
      "--harmony-string-is-well-formed --harmony-import-attributes "
      "--harmony-json-parse-with-source --harmony-regexp-unicode-sets";
  v8::V8::SetFlagsFromString(kUnfrozenFlags, sizeof(kUnfrozenFlags) - 1);
  sPlatform = NewEventLoopPlatform();
  HMODULE thisModule = nullptr;
  char modulePath[MAX_PATH];
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(&JS_NewContext),
                          &thisModule) ||
      !GetModuleFileNameA(thisModule, modulePath, MAX_PATH) ||
      !v8::V8::InitializeICUDefaultLocation(modulePath)) {
    return "vge jsapi: icudtl.dat was not found next to the engine module";
  }
  v8::V8::InitializePlatform(sPlatform.get());
  cppgc::InitializeProcess(sPlatform->GetPageAllocator());
  if (!v8::V8::Initialize()) {
    return "vge jsapi: v8::V8::Initialize failed";
  }
  libraryInitState = InitState::Running;
  return nullptr;
}

void JS_ShutDown() {
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  cppgc::ShutdownProcess();
  sPlatform.reset();
  JS::detail::libraryInitState = JS::detail::InitState::ShutDown;
}

JSContext* JS_NewContext(uint32_t aMaxBytes, JSRuntime* aParentRuntime) {
  JSRuntime* rt = new JSRuntime();
  v8::Isolate::CreateParams params;
  params.array_buffer_allocator =
      v8::ArrayBuffer::Allocator::NewDefaultAllocator();
  rt->isolate = v8::Isolate::Allocate();
  AttachIsolateToCurrentThread(sPlatform.get(), rt->isolate);
  v8::Isolate::Initialize(rt->isolate, params);
  rt->isolate->Enter();
  rt->cppHeap = v8::CppHeap::Create(
      sPlatform.get(),
      v8::CppHeapCreateParams({}, v8::WrapperDescriptor(0, 1, kWrapperEmbedderId)));
  rt->isolate->AttachCppHeap(rt->cppHeap.get());
  rt->zone = new JS::Zone(rt);
  JSContext* cx = new JSContext(rt);
  rt->mainContext = cx;
  tlsContext.set(cx);
  ::new (cx->taskScope) v8::HandleScope(rt->isolate);
  {
    v8::HandleScope scope(rt->isolate);
    rt->cellKey.Reset(rt->isolate,
                      v8::Private::New(rt->isolate,
                                       v8::String::NewFromUtf8Literal(
                                           rt->isolate, "vge.jsapi.cell")));
    rt->slotsKey.Reset(rt->isolate,
                       v8::Private::New(rt->isolate,
                                        v8::String::NewFromUtf8Literal(
                                            rt->isolate, "vge.jsapi.slots")));
  }
  rt->isolate->AddGCPrologueCallback(GCPrologue, rt);
  rt->isolate->AddGCEpilogueCallback(GCEpilogue, rt);
  return cx;
}

void JS_DestroyContext(JSContext* aCx) {
  JSRuntime* rt = aCx->runtime;
  rt->isolate->RemoveGCPrologueCallback(GCPrologue, rt);
  rt->isolate->RemoveGCEpilogueCallback(GCEpilogue, rt);
  rt->cellKey.Reset();
  rt->slotsKey.Reset();
  rt->isolate->DetachCppHeap();
  rt->cppHeap.reset();
  std::vector<JS::Realm*> realms = rt->realms;
  for (JS::Realm* realm : realms) {
    DestroyRealm(rt, realm, true);
  }
  reinterpret_cast<v8::HandleScope*>(aCx->taskScope)->~HandleScope();
  DetachIsolate(sPlatform.get(), rt->isolate);
  rt->isolate->Exit();
  rt->isolate->Dispose();
  ReleaseCellChunks(rt);
  tlsContext.set(nullptr);
  delete aCx;
}

void js::ReleaseTaskHandles(JSContext* aCx) {
  if (aCx->isolate->InContext()) {
    return;
  }
  reinterpret_cast<v8::HandleScope*>(aCx->taskScope)->~HandleScope();
  ::new (aCx->taskScope) v8::HandleScope(aCx->isolate);
}

void* JS_GetContextPrivate(JSContext* aCx) { return aCx->data; }

void JS_SetContextPrivate(JSContext* aCx, void* aData) { aCx->data = aData; }

JSRuntime* JS_GetParentRuntime(JSContext* aCx) { return aCx->runtime; }

JSRuntime* JS_GetRuntime(JSContext* aCx) { return aCx->runtime; }

void JS_SetFutexCanWait(JSContext* aCx) {}

bool JS::InitSelfHostedCode(JSContext* aCx, JS::SelfHostedCache aCache,
                            JS::SelfHostedWriter aWriter) {
  return true;
}

void js::AssertHeapIsIdle() {}

JSAutoRealm::JSAutoRealm(JSContext* aCx, JSObject* aTarget)
    : cx_(aCx), oldRealm_(aCx->realm()) {
  aCx->enterRealm(JS::GetObjectRealmOrNull(aTarget));
}

JSAutoRealm::JSAutoRealm(JSContext* aCx, JSScript* aTarget)
    : cx_(aCx), oldRealm_(aCx->realm()) {
  aCx->enterRealm(RealmOfScript(aTarget));
}

JSAutoRealm::~JSAutoRealm() { cx_->leaveRealm(oldRealm_); }

JSAutoNullableRealm::JSAutoNullableRealm(JSContext* aCx,
                                         JSObject* aTargetOrNull)
    : cx_(aCx), oldRealm_(aCx->realm()) {
  aCx->enterRealm(aTargetOrNull ? JS::GetObjectRealmOrNull(aTargetOrNull)
                                : nullptr);
}

JSAutoNullableRealm::~JSAutoNullableRealm() { cx_->leaveRealm(oldRealm_); }

JS::Realm* JS::EnterRealm(JSContext* aCx, JSObject* aTarget) {
  JS::Realm* old = aCx->realm();
  aCx->enterRealm(JS::GetObjectRealmOrNull(aTarget));
  return old;
}

void JS::LeaveRealm(JSContext* aCx, JS::Realm* aOldRealm) {
  aCx->leaveRealm(aOldRealm);
}

v8::Platform* vge::jsapi::CurrentPlatform() { return sPlatform.get(); }

v8::Isolate* vge::jsapi::MainIsolate() {
  JSContext* cx = CurrentContext();
  return cx ? cx->isolate : nullptr;
}

void JS::RootingContext::traceStackRoots(JSTracer* aTracer) {
  for (size_t k = 0; k < size_t(JS::RootKind::Limit); k++) {
    JS::RootKind kind = JS::RootKind(k);
    for (js::StackRootedBase* r = stackRoots_[kind]; r; r = r->previous()) {
      void* address = reinterpret_cast<uint8_t*>(r) +
                      sizeof(js::StackRootedBase*) * 2;
      switch (kind) {
        case JS::RootKind::Object:
          if (*static_cast<JSObject**>(address)) {
            JS::TraceRoot(aTracer, static_cast<JSObject**>(address), "rooted");
          }
          break;
        case JS::RootKind::String:
          if (*static_cast<JSString**>(address)) {
            JS::TraceRoot(aTracer, static_cast<JSString**>(address), "rooted");
          }
          break;
        case JS::RootKind::Symbol:
          if (*static_cast<JS::Symbol**>(address)) {
            JS::TraceRoot(aTracer, static_cast<JS::Symbol**>(address),
                          "rooted");
          }
          break;
        case JS::RootKind::BigInt:
          if (*static_cast<JS::BigInt**>(address)) {
            JS::TraceRoot(aTracer, static_cast<JS::BigInt**>(address),
                          "rooted");
          }
          break;
        case JS::RootKind::Value:
          JS::TraceRoot(aTracer, static_cast<JS::Value*>(address), "rooted");
          break;
        case JS::RootKind::Id:
          JS::TraceRoot(aTracer, static_cast<jsid*>(address), "rooted");
          break;
        case JS::RootKind::Traceable:
          static_cast<js::StackRootedTraceableBase*>(r)->trace(aTracer,
                                                               "rooted");
          break;
        default:
          break;
      }
    }
  }
}

void JS::RootingContext::traceAllGCRooters(JSTracer* aTracer) {
  for (size_t k = 0; k < size_t(JS::AutoGCRooterKind::Limit); k++) {
    traceGCRooterList(aTracer, autoGCRooters_[JS::AutoGCRooterKind(k)]);
  }
}

void JS::RootingContext::traceGCRooterList(JSTracer* aTracer,
                                           JS::AutoGCRooter* aHead) {
  for (JS::AutoGCRooter* r = aHead; r; r = r->down) {
    r->trace(aTracer);
  }
}
