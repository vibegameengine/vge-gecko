#include "VgeJsBuffers.h"

#include <unordered_map>

#include "js/ErrorReport.h"
#include "js/Exception.h"
#include "js/Object.h"
#include "js/Proxy.h"
#include "js/Wrapper.h"
#include "mozilla/Maybe.h"
#include "v8-promise.h"

namespace vge::jsapi::buffers {

namespace {

enum JobSlot : uint32_t {
  kJobGlobal = 0,
  kJobIncumbent = 1,
  kJobHostData = 2,
  kJobFlowId = 3,
  kJobSlotCount = 4,
};

const JSClass kJobClass = {"VgeMicroTaskJob",
                           JSCLASS_HAS_RESERVED_SLOTS(kJobSlotCount)};

std::mutex gStatesLock;
std::unordered_map<JSRuntime*, ContextState*> gStates;
thread_local ContextState* tState = nullptr;
std::atomic<uint64_t> gNextPromiseId{1};

constexpr const char* kResolvedKey = "vge.buffers.alreadyResolved";
constexpr const char* kPromiseIdKey = "vge.buffers.promiseId";
constexpr const char* kUserInputKey = "vge.buffers.userInput";

void TraceQueue(JSTracer* aTracer, std::deque<JS::MicroTask>& aQueue) {
  for (JS::MicroTask& task : aQueue) {
    task.trace(aTracer, "microtask-queue-entry");
  }
}

void TraceQueues(JSTracer* aTracer, void* aData) {
  ContextState* state = static_cast<ContextState*>(aData);
  TraceQueue(aTracer, state->regular);
  TraceQueue(aTracer, state->debug);
  for (SavedQueues* saved : state->saved) {
    TraceQueue(aTracer, saved->regular);
    TraceQueue(aTracer, saved->debug);
  }
}

bool TracerRegistered(JSRuntime* aRuntime, ContextState* aState) {
  for (auto& entry : aRuntime->blackRootTracers) {
    if (entry.first == TraceQueues && entry.second == aState) {
      return true;
    }
  }
  return false;
}

v8::Local<v8::Context> ContextOf(JSContext* aCx) {
  v8::Local<v8::Context> ctx = aCx->v8Context();
  if (ctx.IsEmpty()) {
    for (JS::Realm* realm : aCx->runtime->realms) {
      if (!realm->context.IsEmpty()) {
        return realm->context.Get(aCx->isolate);
      }
    }
  }
  return ctx;
}

bool IsJSTask(const JS::MicroTask& aTask) { return aTask.isJS(); }

void RefreshPending(ContextState* aState) {
  aState->v8JobPending = false;
  for (const JS::MicroTask& task : aState->regular) {
    aState->v8JobPending |= IsJSTask(task);
  }
  for (const JS::MicroTask& task : aState->debug) {
    aState->v8JobPending |= IsJSTask(task);
  }
}

mozilla::Maybe<JS::MicroTask> PopFront(ContextState* aState,
                                       std::deque<JS::MicroTask>& aQueue) {
  if (aQueue.empty()) {
    return mozilla::Nothing();
  }
  JS::MicroTask task = aQueue.front();
  aQueue.pop_front();
  if (IsJSTask(task)) {
    aState->v8JobPending = false;
  }
  return mozilla::Some(task);
}

JSObject* GlobalForJob(JSContext* aCx, v8::Local<v8::Context> aJobContext) {
  JS::Realm* realm = nullptr;
  if (!aJobContext.IsEmpty()) {
    realm = RealmOfV8Context(aCx, aJobContext);
  }
  if ((!realm || !realm->global) && aCx->realm()) {
    realm = aCx->realm();
  }
  if (realm && realm->global) {
    return realm->global;
  }
  for (JS::Realm* r : aCx->runtime->realms) {
    if (r->global) {
      return r->global;
    }
  }
  return nullptr;
}

bool AlreadyResolved(v8::Local<v8::Context> aCtx,
                     v8::Local<v8::Object> aState) {
  v8::Local<v8::Value> flag;
  return aState->GetPrivate(aCtx, PrivateKey(aCtx->GetIsolate(), kResolvedKey))
             .ToLocal(&flag) &&
         flag->IsTrue();
}

void MarkResolved(v8::Local<v8::Context> aCtx, v8::Local<v8::Object> aState) {
  aState
      ->SetPrivate(aCtx, PrivateKey(aCtx->GetIsolate(), kResolvedKey),
                   v8::True(aCtx->GetIsolate()))
      .Check();
}

void ResolvingFunction(const v8::FunctionCallbackInfo<v8::Value>& aInfo,
                       bool aReject) {
  v8::Isolate* isolate = aInfo.GetIsolate();
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  v8::Local<v8::Array> data = aInfo.Data().As<v8::Array>();
  v8::Local<v8::Value> promiseValue;
  v8::Local<v8::Value> stateValue;
  if (!data->Get(ctx, 0).ToLocal(&promiseValue) ||
      !data->Get(ctx, 1).ToLocal(&stateValue)) {
    return;
  }
  v8::Local<v8::Object> state = stateValue.As<v8::Object>();
  if (AlreadyResolved(ctx, state)) {
    return;
  }
  MarkResolved(ctx, state);
  v8::Local<v8::Promise::Resolver> resolver =
      promiseValue.As<v8::Promise::Resolver>();
  v8::Local<v8::Value> arg = aInfo[0];
  if (aReject) {
    (void)resolver->Reject(ctx, arg);
  } else {
    (void)resolver->Resolve(ctx, arg);
  }
}

void ResolveCallback(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  ResolvingFunction(aInfo, false);
}

void RejectCallback(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  ResolvingFunction(aInfo, true);
}

bool MakeResolvingFunctions(v8::Local<v8::Context> aCtx,
                            v8::Local<v8::Promise> aPromise,
                            v8::Local<v8::Object> aState,
                            v8::Local<v8::Function>* aResolve,
                            v8::Local<v8::Function>* aReject) {
  v8::Isolate* isolate = aCtx->GetIsolate();
  v8::Local<v8::Value> elements[] = {aPromise, aState};
  v8::Local<v8::Array> data = v8::Array::New(isolate, elements, 2);
  return v8::Function::New(aCtx, ResolveCallback, data, 1).ToLocal(aResolve) &&
         v8::Function::New(aCtx, RejectCallback, data, 1).ToLocal(aReject);
}

void IdentityCallback(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  aInfo.GetReturnValue().Set(aInfo[0]);
}

void ThrowerCallback(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  aInfo.GetIsolate()->ThrowException(aInfo[0]);
}

bool RequiresDeferral(v8::Local<v8::Context> aCtx,
                      v8::Local<v8::Value> aValue) {
  if (!aValue->IsObject()) {
    return false;
  }
  v8::Isolate* isolate = aCtx->GetIsolate();
  v8::Local<v8::String> then = v8::String::NewFromUtf8Literal(isolate, "then");
  v8::Local<v8::Value> current = aValue;
  while (current->IsObject()) {
    v8::Local<v8::Object> obj = current.As<v8::Object>();
    if (obj->IsProxy() || obj->HasNamedLookupInterceptor()) {
      return true;
    }
    v8::Local<v8::Value> desc;
    if (!obj->GetOwnPropertyDescriptor(aCtx, then).ToLocal(&desc)) {
      return true;
    }
    if (desc->IsObject()) {
      v8::Local<v8::Object> d = desc.As<v8::Object>();
      v8::Local<v8::Value> getter;
      if (d->Get(aCtx, v8::String::NewFromUtf8Literal(isolate, "get"))
              .ToLocal(&getter) &&
          !getter->IsUndefined()) {
        return true;
      }
      v8::Local<v8::Value> value;
      if (d->Has(aCtx, v8::String::NewFromUtf8Literal(isolate, "value"))
              .FromMaybe(false) &&
          d->Get(aCtx, v8::String::NewFromUtf8Literal(isolate, "value"))
              .ToLocal(&value)) {
        return value->IsFunction();
      }
      return false;
    }
    current = obj->GetPrototype();
  }
  return false;
}

void DeferredResolveCallback(
    const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  v8::Isolate* isolate = aInfo.GetIsolate();
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  v8::Local<v8::Array> data = aInfo.Data().As<v8::Array>();
  v8::Local<v8::Value> promiseValue;
  v8::Local<v8::Value> resolution;
  if (!data->Get(ctx, 0).ToLocal(&promiseValue) ||
      !data->Get(ctx, 1).ToLocal(&resolution)) {
    return;
  }
  v8::Local<v8::Promise> promise = promiseValue.As<v8::Promise>();
  v8::Local<v8::Promise::Resolver> resolver =
      promiseValue.As<v8::Promise::Resolver>();
  if (promise->State() != v8::Promise::kPending) {
    return;
  }
  if (!resolution->IsObject()) {
    (void)resolver->Resolve(ctx, resolution);
    return;
  }
  if (resolution->StrictEquals(promise)) {
    (void)resolver->Reject(
        ctx, v8::Exception::TypeError(v8::String::NewFromUtf8Literal(
                 isolate, "A promise cannot be resolved with itself.")));
    return;
  }
  v8::TryCatch tryCatch(isolate);
  v8::Local<v8::Value> then;
  if (!resolution.As<v8::Object>()
           ->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "then"))
           .ToLocal(&then)) {
    v8::Local<v8::Value> exception = tryCatch.Exception();
    tryCatch.Reset();
    (void)resolver->Reject(ctx, exception);
    return;
  }
  if (!then->IsFunction()) {
    (void)resolver->Resolve(ctx, resolution);
    return;
  }
  v8::Local<v8::Object> state = v8::Object::New(isolate);
  v8::Local<v8::Function> resolveFn;
  v8::Local<v8::Function> rejectFn;
  if (!MakeResolvingFunctions(ctx, promise, state, &resolveFn, &rejectFn)) {
    return;
  }
  v8::Local<v8::Value> argv[] = {resolveFn, rejectFn};
  if (then.As<v8::Function>()->Call(ctx, resolution, 2, argv).IsEmpty()) {
    v8::Local<v8::Value> exception = tryCatch.Exception();
    tryCatch.Reset();
    if (!AlreadyResolved(ctx, state)) {
      MarkResolved(ctx, state);
      (void)resolver->Reject(ctx, exception);
    }
  }
}

void PropagateUserInput(v8::Local<v8::Context> aCtx,
                        v8::Local<v8::Promise> aPromise,
                        v8::Local<v8::Value> aParent) {
  if (aParent.IsEmpty() || !aParent->IsPromise()) {
    return;
  }
  v8::Local<v8::Private> key = PrivateKey(aCtx->GetIsolate(), kUserInputKey);
  v8::Local<v8::Value> state;
  if (aParent.As<v8::Object>()->GetPrivate(aCtx, key).ToLocal(&state) &&
      state->IsInt32() && state.As<v8::Int32>()->Value() != 0) {
    aPromise->SetPrivate(aCtx, key, state).Check();
  }
}

void PromiseHookCallback(v8::PromiseHookType aType,
                         v8::Local<v8::Promise> aPromise,
                         v8::Local<v8::Value> aParent) {
  JSContext* cx = CurrentContext();
  if (!cx) {
    return;
  }
  if (aType != v8::PromiseHookType::kInit &&
      aType != v8::PromiseHookType::kResolve) {
    return;
  }
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::Context> creation;
  if (!aPromise->GetCreationContext().ToLocal(&creation)) {
    creation = ContextOf(cx);
  }
  if (aType == v8::PromiseHookType::kInit) {
    if (aParent.IsEmpty() || aParent->IsUndefined()) {
      return;
    }
    PropagateUserInput(creation, aPromise, aParent);
  }
  NoteV8Job(cx, creation);
}

void PromiseRejectCallback(v8::PromiseRejectMessage aMessage) {
  JSContext* cx = CurrentContext();
  if (!cx) {
    return;
  }
  ContextState* state = StateFor(cx);
  if (!state->rejectionTracker) {
    return;
  }
  JS::PromiseRejectionHandlingState handling;
  switch (aMessage.GetEvent()) {
    case v8::kPromiseRejectWithNoHandler:
      handling = JS::PromiseRejectionHandlingState::Unhandled;
      break;
    case v8::kPromiseHandlerAddedAfterReject:
      handling = JS::PromiseRejectionHandlingState::Handled;
      break;
    default:
      return;
  }
  v8::HandleScope scope(cx->isolate);
  JS::RootedObject promise(cx, ObjectFromV8(cx, aMessage.GetPromise()));
  state->rejectionTracker(cx, false, promise, handling,
                          state->rejectionTrackerData);
}

void CallTracker(JSContext* aCx, JS::HandleObject aPromise,
                 JS::PromiseRejectionHandlingState aState) {
  ContextState* state = StateFor(aCx);
  if (state->rejectionTracker) {
    state->rejectionTracker(aCx, false, aPromise, aState,
                            state->rejectionTrackerData);
  }
}

JSObject* UnwrapPromiseOrReport(JSContext* aCx, JS::HandleObject aPromise) {
  JSObject* unwrapped = Unwrapped(aPromise);
  if (!unwrapped) {
    js::ReportAccessDenied(aCx);
    return nullptr;
  }
  return unwrapped;
}

v8::Local<v8::Promise> PromiseOf(JSContext* aCx, JSObject* aObj) {
  if (!aObj) {
    return v8::Local<v8::Promise>();
  }
  v8::Local<v8::Object> obj = V8Of(aCx, aObj);
  if (!obj->IsPromise()) {
    return v8::Local<v8::Promise>();
  }
  return obj.As<v8::Promise>();
}

bool ResolveOrReject(JSContext* aCx, JS::HandleObject aPromiseObj,
                     JS::HandleValue aValue, bool aReject) {
  JS::RootedObject unwrapped(aCx, UnwrapPromiseOrReport(aCx, aPromiseObj));
  if (!unwrapped) {
    return false;
  }
  JS::RootedValue value(aCx, aValue);
  mozilla::Maybe<JSAutoRealm> ar;
  if (unwrapped != aPromiseObj) {
    ar.emplace(aCx, unwrapped);
    if (!JS_WrapValue(aCx, &value)) {
      return false;
    }
  }
  AutoV8 v8(aCx);
  v8::Local<v8::Context> ctx = ContextOf(aCx);
  v8::Local<v8::Promise> promise = PromiseOf(aCx, unwrapped);
  if (promise.IsEmpty()) {
    JS_ReportErrorNumberASCII(aCx, js::GetErrorMessage, nullptr,
                              JSMSG_NOT_EXPECTED_TYPE, "ResolvePromise",
                              "Promise", "object");
    return false;
  }
  if (AlreadyResolved(ctx, promise)) {
    return true;
  }
  MarkResolved(ctx, promise);
  v8::TryCatch tryCatch(aCx->isolate);
  v8::Local<v8::Promise::Resolver> resolver =
      promise.As<v8::Promise::Resolver>();
  v8::Local<v8::Value> v = ToV8(aCx, value.get());
  v8::Maybe<bool> ok =
      aReject ? resolver->Reject(ctx, v) : resolver->Resolve(ctx, v);
  if (ok.IsNothing()) {
    return v8.Check(tryCatch);
  }
  return true;
}

void WaitForAllElement(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  v8::Isolate* isolate = aInfo.GetIsolate();
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  v8::Local<v8::Array> data = aInfo.Data().As<v8::Array>();
  v8::Local<v8::Value> holderValue;
  v8::Local<v8::Value> indexValue;
  v8::Local<v8::Value> called;
  if (!data->Get(ctx, 0).ToLocal(&holderValue) ||
      !data->Get(ctx, 1).ToLocal(&indexValue) ||
      !data->Get(ctx, 2).ToLocal(&called)) {
    return;
  }
  if (called->IsTrue()) {
    return;
  }
  (void)data->Set(ctx, 2, v8::True(isolate));
  v8::Local<v8::Array> holder = holderValue.As<v8::Array>();
  v8::Local<v8::Value> resolverValue;
  v8::Local<v8::Value> valuesValue;
  v8::Local<v8::Value> remainingValue;
  if (!holder->Get(ctx, 0).ToLocal(&resolverValue) ||
      !holder->Get(ctx, 1).ToLocal(&valuesValue) ||
      !holder->Get(ctx, 2).ToLocal(&remainingValue)) {
    return;
  }
  uint32_t index = indexValue.As<v8::Uint32>()->Value();
  (void)valuesValue.As<v8::Array>()->Set(ctx, index, aInfo[0]);
  int32_t remaining = remainingValue.As<v8::Int32>()->Value() - 1;
  (void)holder->Set(ctx, 2, v8::Integer::New(isolate, remaining));
  if (remaining == 0) {
    (void)resolverValue.As<v8::Promise::Resolver>()->Resolve(ctx, valuesValue);
  }
}

void WaitForAllReject(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  v8::Isolate* isolate = aInfo.GetIsolate();
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  (void)aInfo.Data().As<v8::Promise::Resolver>()->Reject(ctx, aInfo[0]);
}

}

ContextState* StateForRuntime(JSRuntime* aRuntime) {
  std::lock_guard<std::mutex> lock(gStatesLock);
  ContextState*& state = gStates[aRuntime];
  if (!state) {
    state = new ContextState();
  }
  if (state->runtime != aRuntime || state->cx != aRuntime->mainContext ||
      !TracerRegistered(aRuntime, state)) {
    state->regular.clear();
    state->debug.clear();
    state->saved.clear();
    state->v8JobPending = false;
    state->draining = false;
    state->hooksInstalled = false;
    state->rejectionTracker = nullptr;
    state->rejectionTrackerData = nullptr;
    state->dispatchCallback = nullptr;
    state->delayedDispatchCallback = nullptr;
    state->asyncTaskStarted = nullptr;
    state->asyncTaskFinished = nullptr;
    state->asyncTaskClosure = nullptr;
    state->liveTasks.clear();
    state->shuttingDown = false;
    state->beforeWait = nullptr;
    state->afterWait = nullptr;
    state->runtime = aRuntime;
    state->cx = aRuntime->mainContext;
    aRuntime->blackRootTracers.emplace_back(TraceQueues, state);
  }
  return state;
}

ContextState* StateFor(JSContext* aCx) {
  ContextState* cached = tState;
  if (cached && cached->cx == aCx && cached->runtime == aCx->runtime &&
      TracerRegistered(aCx->runtime, cached)) {
    return cached;
  }
  tState = StateForRuntime(aCx->runtime);
  return tState;
}

void InstallJobHooks(JSContext* aCx) {
  ContextState* state = StateFor(aCx);
  if (state->hooksInstalled) {
    return;
  }
  state->hooksInstalled = true;
  aCx->isolate->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);
  aCx->isolate->SetPromiseHook(PromiseHookCallback);
  aCx->isolate->SetPromiseRejectCallback(PromiseRejectCallback);
}

void NoteV8Job(JSContext* aCx, v8::Local<v8::Context> aJobContext) {
  ContextState* state = StateFor(aCx);
  if (state->draining || state->v8JobPending) {
    return;
  }
  JS::RootedObject global(aCx, GlobalForJob(aCx, aJobContext));
  if (!global) {
    return;
  }
  state->v8JobPending = true;
  JSAutoRealm ar(aCx, global);
  JS::RootedObject job(aCx, NewClassObject(aCx, &kJobClass, nullptr));
  if (!job) {
    state->v8JobPending = false;
    JS_ClearPendingException(aCx);
    return;
  }
  JS::RootedObject incumbent(aCx);
  JS::RootedObject hostData(aCx);
  if (aCx->jobQueue &&
      !aCx->jobQueue->getHostDefinedData(aCx, &incumbent, &hostData)) {
    JS_ClearPendingException(aCx);
    incumbent = nullptr;
    hostData = nullptr;
  }
  JS::Value* slots = ObjectCell::from(job)->fixedSlots();
  slots[kJobGlobal] = JS::ObjectValue(*global);
  slots[kJobIncumbent] = JS::ObjectOrNullValue(incumbent);
  slots[kJobHostData] = JS::ObjectOrNullValue(hostData);
  slots[kJobFlowId] = JS::DoubleValue(double(state->nextFlowId++));
  JS::MicroTask task = js::MicroTaskQueueElement::MakeJS(JS::ObjectValue(*job));
  if (aCx->jobQueue && aCx->jobQueue->useDebugQueue(global)) {
    state->debug.push_back(task);
  } else {
    state->regular.push_back(task);
  }
  state->canSkipEnqueuingJobs = false;
}

bool DispatchToEventLoop(JSContext* aCx,
                         js::UniquePtr<JS::Dispatchable>&& aTask) {
  ContextState* state = StateFor(aCx);
  if (state->shuttingDown || !state->dispatchCallback) {
    JS::Dispatchable::ReleaseFailedTask(std::move(aTask));
    return false;
  }
  JS::Dispatchable* raw = aTask.get();
  state->liveTasks.push_back(raw);
  if (state->asyncTaskStarted) {
    state->asyncTaskStarted(state->asyncTaskClosure, raw);
  }
  if (!state->dispatchCallback(state->asyncTaskClosure, std::move(aTask))) {
    auto it = std::find(state->liveTasks.begin(), state->liveTasks.end(), raw);
    if (it != state->liveTasks.end()) {
      state->liveTasks.erase(it);
      if (state->asyncTaskFinished) {
        state->asyncTaskFinished(state->asyncTaskClosure, raw);
      }
    }
    return false;
  }
  return true;
}

}

using namespace vge::jsapi;
using namespace vge::jsapi::buffers;

void JS::SetJobQueue(JSContext* aCx, JS::JobQueue* aQueue) {
  aCx->jobQueue = aQueue;
  InstallJobHooks(aCx);
}

void JS::SetPromiseRejectionTrackerCallback(
    JSContext* aCx, JS::PromiseRejectionTrackerCallback aCallback,
    void* aData) {
  ContextState* state = StateFor(aCx);
  state->rejectionTracker = aCallback;
  state->rejectionTrackerData = aData;
  InstallJobHooks(aCx);
}

void JS::JobQueueIsEmpty(JSContext* aCx) {
  StateFor(aCx)->canSkipEnqueuingJobs = true;
}

void JS::JobQueueMayNotBeEmpty(JSContext* aCx) {
  StateFor(aCx)->canSkipEnqueuingJobs = false;
}

void JS::MicroTask::trace(JSTracer* aTracer, const char* aName) {
  if (value_.isGCThing()) {
    JS::TraceRoot(aTracer, &value_, aName);
    return;
  }
  JSRuntime* rt = aTracer->runtime();
  JSContext* cx = rt ? rt->mainContext : nullptr;
  if (cx && cx->jobQueue) {
    cx->jobQueue->traceNonGCThingMicroTask(aTracer, &value_);
  }
}

bool JS::EnqueueMicroTask(JSContext* aCx, const JS::MicroTask& aEntry) {
  ContextState* state = StateFor(aCx);
  state->canSkipEnqueuingJobs = false;
  state->regular.push_back(aEntry);
  return true;
}

bool JS::EnqueueDebugMicroTask(JSContext* aCx, const JS::MicroTask& aEntry) {
  StateFor(aCx)->debug.push_back(aEntry);
  return true;
}

bool JS::PrependMicroTask(JSContext* aCx, const JS::MicroTask& aEntry) {
  ContextState* state = StateFor(aCx);
  state->canSkipEnqueuingJobs = false;
  state->regular.push_front(aEntry);
  if (aEntry.isJS()) {
    state->v8JobPending = true;
  }
  return true;
}

mozilla::Maybe<JS::MicroTask> JS::DequeueNextMicroTask(JSContext* aCx) {
  ContextState* state = StateFor(aCx);
  if (!state->debug.empty()) {
    return PopFront(state, state->debug);
  }
  return PopFront(state, state->regular);
}

mozilla::Maybe<JS::MicroTask> JS::DequeueNextDebuggerMicroTask(
    JSContext* aCx) {
  ContextState* state = StateFor(aCx);
  return PopFront(state, state->debug);
}

mozilla::Maybe<JS::MicroTask> JS::DequeueNextRegularMicroTask(JSContext* aCx) {
  ContextState* state = StateFor(aCx);
  return PopFront(state, state->regular);
}

mozilla::Maybe<JS::MicroTask> JS::PeekNextMicroTask(JSContext* aCx) {
  ContextState* state = StateFor(aCx);
  if (!state->debug.empty()) {
    return mozilla::Some(state->debug.front());
  }
  if (!state->regular.empty()) {
    return mozilla::Some(state->regular.front());
  }
  return mozilla::Nothing();
}

bool JS::HasAnyMicroTasks(JSContext* aCx) {
  ContextState* state = StateFor(aCx);
  return !state->debug.empty() || !state->regular.empty();
}

bool JS::HasDebuggerMicroTasks(JSContext* aCx) {
  return !StateFor(aCx)->debug.empty();
}

bool JS::HasRegularMicroTasks(JSContext* aCx) {
  return !StateFor(aCx)->regular.empty();
}

size_t JS::GetRegularMicroTaskCount(JSContext* aCx) {
  return StateFor(aCx)->regular.size();
}

namespace {

struct SavedMicroTaskQueueImpl : public JS::SavedMicroTaskQueue {
  SavedMicroTaskQueueImpl(ContextState* aState) : state(aState) {
    std::swap(saved.regular, aState->regular);
    std::swap(saved.debug, aState->debug);
    aState->saved.push_back(&saved);
    RefreshPending(aState);
  }
  ~SavedMicroTaskQueueImpl() override {
    auto it = std::find(state->saved.begin(), state->saved.end(), &saved);
    if (it != state->saved.end()) {
      state->saved.erase(it);
    }
  }
  ContextState* state;
  SavedQueues saved;
};

}

js::UniquePtr<JS::SavedMicroTaskQueue> JS::SaveMicroTaskQueue(JSContext* aCx) {
  auto saved = js::MakeUnique<SavedMicroTaskQueueImpl>(StateFor(aCx));
  if (!saved) {
    JS_ReportOutOfMemory(aCx);
    return nullptr;
  }
  return saved;
}

void JS::RestoreMicroTaskQueue(
    JSContext* aCx, js::UniquePtr<JS::SavedMicroTaskQueue> aSavedQueue) {
  ContextState* state = StateFor(aCx);
  SavedMicroTaskQueueImpl* impl =
      static_cast<SavedMicroTaskQueueImpl*>(aSavedQueue.get());
  std::swap(impl->saved.regular, state->regular);
  std::swap(impl->saved.debug, state->debug);
  RefreshPending(state);
}

JSObject* JS::JSMicroTaskRef::executionGlobal() && {
  return JS::GetReservedSlot(maybeWrappedObject(), kJobGlobal)
      .toObjectOrNull();
}

bool JS::JSMicroTaskRef::maybeGetHostDefinedData(
    JS::MutableHandleObject aIncumbentGlobal,
    JS::MutableHandleObject aOptionalHostDefinedData) && {
  JSObject* job = maybeWrappedObject();
  aIncumbentGlobal.set(JS::GetReservedSlot(job, kJobIncumbent).toObjectOrNull());
  aOptionalHostDefinedData.set(
      JS::GetReservedSlot(job, kJobHostData).toObjectOrNull());
  return true;
}

bool JS::JSMicroTaskRef::maybeGetAllocationSite(JS::MutableHandleObject aOut) && {
  aOut.set(nullptr);
  return true;
}

JSObject* JS::JSMicroTaskRef::maybeGetPromise() && { return nullptr; }

bool JS::JSMicroTaskRef::getFlowId(uint64_t* aUid) && {
  *aUid = uint64_t(
      JS::GetReservedSlot(maybeWrappedObject(), kJobFlowId).toNumber());
  return true;
}

bool JS::JSMicroTaskRef::run(JSContext* aCx) && {
  ContextState* state = StateFor(aCx);
  if (state->draining) {
    return true;
  }
  state->draining = true;
  {
    v8::HandleScope scope(aCx->isolate);
    aCx->isolate->PerformMicrotaskCheckpoint();
  }
  state->draining = false;
  return !aCx->isolate->IsExecutionTerminating();
}

bool JS::RunJSMicroTask(JSContext* aCx, JS::Handle<JS::MicroTask> aTask) {
  return JS::JSMicroTaskRef(aTask.get()).run(aCx);
}

bool JS::RunJSMicroTask(JSContext* aCx,
                        JS::Handle<mozilla::Maybe<JS::MicroTask>> aTask) {
  if (aTask.get().isNothing()) {
    return false;
  }
  return JS::JSMicroTaskRef(aTask.get().ref()).run(aCx);
}

JSObject* JS::NewPromiseObject(JSContext* aCx, JS::HandleObject aExecutor) {
  AutoV8 v8(aCx);
  v8::TryCatch tryCatch(aCx->isolate);
  v8::Local<v8::Context> ctx = ContextOf(aCx);
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(ctx).ToLocal(&resolver)) {
    v8.Check(tryCatch);
    return nullptr;
  }
  v8::Local<v8::Promise> promise = resolver->GetPromise();
  if (aExecutor) {
    v8::Local<v8::Function> resolveFn;
    v8::Local<v8::Function> rejectFn;
    if (!MakeResolvingFunctions(ctx, promise, promise, &resolveFn,
                                &rejectFn)) {
      v8.Check(tryCatch);
      return nullptr;
    }
    v8::Local<v8::Object> executor = V8Of(aCx, aExecutor);
    v8::Local<v8::Value> argv[] = {resolveFn, rejectFn};
    if (executor->CallAsFunction(ctx, v8::Undefined(aCx->isolate), 2, argv)
            .IsEmpty()) {
      if (tryCatch.HasTerminated()) {
        return nullptr;
      }
      v8::Local<v8::Value> exception = tryCatch.Exception();
      tryCatch.Reset();
      if (!AlreadyResolved(ctx, promise)) {
        MarkResolved(ctx, promise);
        (void)resolver->Reject(ctx, exception);
      }
    }
  }
  return ObjectFromV8(aCx, promise);
}

bool JS::IsPromiseObject(JS::HandleObject aObj) {
  JSContext* cx = CurrentContext();
  v8::HandleScope scope(cx->isolate);
  return V8Of(cx, aObj)->IsPromise();
}

JS::PromiseState JS::GetPromiseState(JS::HandleObject aPromise) {
  JSContext* cx = CurrentContext();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::Promise> promise = PromiseOf(cx, Unwrapped(aPromise));
  if (promise.IsEmpty()) {
    return JS::PromiseState::Pending;
  }
  switch (promise->State()) {
    case v8::Promise::kFulfilled:
      return JS::PromiseState::Fulfilled;
    case v8::Promise::kRejected:
      return JS::PromiseState::Rejected;
    default:
      return JS::PromiseState::Pending;
  }
}

uint64_t JS::GetPromiseID(JS::HandleObject aPromise) {
  JSContext* cx = CurrentContext();
  AutoV8 v8(cx);
  v8::Local<v8::Context> ctx = ContextOf(cx);
  v8::Local<v8::Object> promise = V8Of(cx, aPromise);
  v8::Local<v8::Private> key = PrivateKey(cx->isolate, kPromiseIdKey);
  v8::Local<v8::Value> id;
  if (promise->GetPrivate(ctx, key).ToLocal(&id) && id->IsNumber()) {
    return uint64_t(id.As<v8::Number>()->Value());
  }
  uint64_t fresh = gNextPromiseId++;
  promise->SetPrivate(ctx, key, v8::Number::New(cx->isolate, double(fresh)))
      .Check();
  return fresh;
}

JS::Value JS::GetPromiseResult(JS::HandleObject aPromise) {
  JSContext* cx = CurrentContext();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::Promise> promise = PromiseOf(cx, aPromise);
  if (promise.IsEmpty() || promise->State() == v8::Promise::kPending) {
    return JS::UndefinedValue();
  }
  return ValueFromV8(cx, promise->Result());
}

bool JS::GetPromiseIsHandled(JS::HandleObject aPromise) {
  JSContext* cx = CurrentContext();
  v8::HandleScope scope(cx->isolate);
  return PromiseOf(cx, aPromise)->HasHandler();
}

bool JS::SetSettledPromiseIsHandled(JSContext* aCx, JS::HandleObject aPromise) {
  JS::RootedObject unwrapped(aCx, UnwrapPromiseOrReport(aCx, aPromise));
  if (!unwrapped) {
    return false;
  }
  mozilla::Maybe<JSAutoRealm> ar;
  if (unwrapped != aPromise) {
    ar.emplace(aCx, unwrapped);
  }
  {
    v8::HandleScope scope(aCx->isolate);
    PromiseOf(aCx, unwrapped)->MarkAsHandled();
  }
  CallTracker(aCx, unwrapped, JS::PromiseRejectionHandlingState::Handled);
  return true;
}

bool JS::SetAnyPromiseIsHandled(JSContext* aCx, JS::HandleObject aPromise) {
  JS::RootedObject unwrapped(aCx, UnwrapPromiseOrReport(aCx, aPromise));
  if (!unwrapped) {
    return false;
  }
  mozilla::Maybe<JSAutoRealm> ar;
  if (unwrapped != aPromise) {
    ar.emplace(aCx, unwrapped);
  }
  bool settled;
  {
    v8::HandleScope scope(aCx->isolate);
    v8::Local<v8::Promise> promise = PromiseOf(aCx, unwrapped);
    settled = promise->State() != v8::Promise::kPending;
    promise->MarkAsHandled();
  }
  if (settled) {
    CallTracker(aCx, unwrapped, JS::PromiseRejectionHandlingState::Handled);
  }
  return true;
}

JSObject* JS::GetPromiseAllocationSite(JS::HandleObject aPromise) {
  return nullptr;
}

JSObject* JS::GetPromiseResolutionSite(JS::HandleObject aPromise) {
  return nullptr;
}

JSObject* JS::CallOriginalPromiseResolve(JSContext* aCx,
                                         JS::HandleValue aValue) {
  AutoV8 v8(aCx);
  v8::TryCatch tryCatch(aCx->isolate);
  v8::Local<v8::Context> ctx = ContextOf(aCx);
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(ctx).ToLocal(&resolver)) {
    v8.Check(tryCatch);
    return nullptr;
  }
  v8::Local<v8::Value> value = ToV8(aCx, aValue.get());
  if (value->IsPromise()) {
    v8::Local<v8::String> ctorKey =
        v8::String::NewFromUtf8Literal(aCx->isolate, "constructor");
    v8::Local<v8::Value> original;
    v8::Local<v8::Value> ctor;
    v8::Local<v8::Value> proto = resolver->GetPromise()->GetPrototype();
    if (!proto->IsObject() ||
        !proto.As<v8::Object>()->Get(ctx, ctorKey).ToLocal(&original) ||
        !value.As<v8::Object>()->Get(ctx, ctorKey).ToLocal(&ctor)) {
      v8.Check(tryCatch);
      return nullptr;
    }
    if (ctor->StrictEquals(original)) {
      return ObjectFromV8(aCx, value.As<v8::Object>());
    }
  }
  if (resolver->Resolve(ctx, value).IsNothing()) {
    v8.Check(tryCatch);
    return nullptr;
  }
  MarkResolved(ctx, resolver->GetPromise());
  return ObjectFromV8(aCx, resolver->GetPromise());
}

JSObject* JS::CallOriginalPromiseReject(JSContext* aCx,
                                        JS::HandleValue aValue) {
  AutoV8 v8(aCx);
  v8::TryCatch tryCatch(aCx->isolate);
  v8::Local<v8::Context> ctx = ContextOf(aCx);
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(ctx).ToLocal(&resolver) ||
      resolver->Reject(ctx, ToV8(aCx, aValue.get())).IsNothing()) {
    v8.Check(tryCatch);
    return nullptr;
  }
  MarkResolved(ctx, resolver->GetPromise());
  return ObjectFromV8(aCx, resolver->GetPromise());
}

bool JS::ResolvePromise(JSContext* aCx, JS::HandleObject aPromiseObj,
                        JS::HandleValue aValue) {
  return ResolveOrReject(aCx, aPromiseObj, aValue, false);
}

bool JS::RejectPromise(JSContext* aCx, JS::HandleObject aPromiseObj,
                       JS::HandleValue aValue) {
  return ResolveOrReject(aCx, aPromiseObj, aValue, true);
}

bool JS::SafeResolve(JSContext* aCx, JS::HandleObject aPromiseObj,
                     JS::HandleValue aValue, bool* aDeferred) {
  if (aDeferred) {
    *aDeferred = false;
  }
  JS::RootedObject unwrapped(aCx, UnwrapPromiseOrReport(aCx, aPromiseObj));
  if (!unwrapped) {
    return false;
  }
  JS::RootedValue value(aCx, aValue);
  mozilla::Maybe<JSAutoRealm> ar;
  if (unwrapped != aPromiseObj) {
    ar.emplace(aCx, unwrapped);
    if (!JS_WrapValue(aCx, &value)) {
      return false;
    }
  }
  bool needsDeferral;
  {
    AutoV8 v8(aCx);
    v8::Local<v8::Context> ctx = ContextOf(aCx);
    v8::Local<v8::Promise> promise = PromiseOf(aCx, unwrapped);
    if (promise->State() != v8::Promise::kPending) {
      return true;
    }
    v8::Local<v8::Value> v = ToV8(aCx, value.get());
    needsDeferral = RequiresDeferral(ctx, v);
    if (needsDeferral) {
      if (AlreadyResolved(ctx, promise)) {
        return true;
      }
      MarkResolved(ctx, promise);
      v8::Local<v8::Value> elements[] = {promise, v};
      v8::Local<v8::Array> data = v8::Array::New(aCx->isolate, elements, 2);
      v8::Local<v8::Function> job;
      if (!v8::Function::New(ctx, DeferredResolveCallback, data, 0)
               .ToLocal(&job)) {
        return false;
      }
      aCx->isolate->EnqueueMicrotask(job);
      v8::Local<v8::Context> creation;
      if (!promise->GetCreationContext().ToLocal(&creation)) {
        creation = ctx;
      }
      NoteV8Job(aCx, creation);
      if (aDeferred) {
        *aDeferred = true;
      }
      return true;
    }
  }
  return ResolveOrReject(aCx, unwrapped, value, false);
}

bool JS::AddPromiseReactions(JSContext* aCx, JS::HandleObject aPromiseObj,
                             JS::HandleObject aOnFulfilled,
                             JS::HandleObject aOnRejected) {
  JS::RootedObject unwrapped(aCx, UnwrapPromiseOrReport(aCx, aPromiseObj));
  if (!unwrapped) {
    return false;
  }
  AutoV8 v8(aCx);
  v8::TryCatch tryCatch(aCx->isolate);
  v8::Local<v8::Context> ctx = ContextOf(aCx);
  v8::Local<v8::Promise> promise = PromiseOf(aCx, unwrapped);
  if (promise.IsEmpty()) {
    JS_ReportErrorNumberASCII(aCx, js::GetErrorMessage, nullptr,
                              JSMSG_INCOMPATIBLE_PROTO, "Promise", "then",
                              JS::GetClass(unwrapped)->name);
    return false;
  }
  v8::Local<v8::Function> onFulfilled;
  v8::Local<v8::Function> onRejected;
  if (aOnFulfilled) {
    onFulfilled = V8Of(aCx, aOnFulfilled).As<v8::Function>();
  } else if (!v8::Function::New(ctx, IdentityCallback).ToLocal(&onFulfilled)) {
    return v8.Check(tryCatch);
  }
  if (aOnRejected) {
    onRejected = V8Of(aCx, aOnRejected).As<v8::Function>();
  } else if (!v8::Function::New(ctx, ThrowerCallback).ToLocal(&onRejected)) {
    return v8.Check(tryCatch);
  }
  if (promise->Then(ctx, onFulfilled, onRejected).IsEmpty()) {
    return v8.Check(tryCatch);
  }
  return true;
}

JS::PromiseUserInputEventHandlingState
JS::GetPromiseUserInputEventHandlingState(JSObject* aPromise) {
  JSContext* cx = CurrentContext();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::Promise> promise = PromiseOf(cx, Unwrapped(aPromise));
  if (promise.IsEmpty()) {
    return JS::PromiseUserInputEventHandlingState::DontCare;
  }
  v8::Local<v8::Value> state;
  if (!promise
           ->GetPrivate(ContextOf(cx), PrivateKey(cx->isolate, kUserInputKey))
           .ToLocal(&state) ||
      !state->IsInt32()) {
    return JS::PromiseUserInputEventHandlingState::DontCare;
  }
  return JS::PromiseUserInputEventHandlingState(
      state.As<v8::Int32>()->Value());
}

bool JS::SetPromiseUserInputEventHandlingState(
    JS::HandleObject aPromise, JS::PromiseUserInputEventHandlingState aState) {
  JSContext* cx = CurrentContext();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::Promise> promise = PromiseOf(cx, Unwrapped(aPromise));
  if (promise.IsEmpty()) {
    return false;
  }
  switch (aState) {
    case JS::PromiseUserInputEventHandlingState::DontCare:
    case JS::PromiseUserInputEventHandlingState::HadUserInteractionAtCreation:
    case JS::PromiseUserInputEventHandlingState::
        DidntHaveUserInteractionAtCreation:
      break;
    default:
      return false;
  }
  promise
      ->SetPrivate(ContextOf(cx), PrivateKey(cx->isolate, kUserInputKey),
                   v8::Integer::New(cx->isolate, int32_t(aState)))
      .Check();
  return true;
}

JSObject* JS::GetWaitForAllPromise(JSContext* aCx,
                                   JS::HandleObjectVector aPromises) {
  AutoV8 v8(aCx);
  v8::TryCatch tryCatch(aCx->isolate);
  v8::Local<v8::Context> ctx = ContextOf(aCx);
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(ctx).ToLocal(&resolver)) {
    v8.Check(tryCatch);
    return nullptr;
  }
  uint32_t count = uint32_t(aPromises.length());
  v8::Local<v8::Array> values = v8::Array::New(aCx->isolate, int(count));
  for (uint32_t i = 0; i < count; i++) {
    (void)values->Set(ctx, i, v8::Undefined(aCx->isolate));
  }
  v8::Local<v8::Value> holderElements[] = {
      resolver, values, v8::Integer::New(aCx->isolate, int32_t(count) + 1)};
  v8::Local<v8::Array> holder = v8::Array::New(aCx->isolate, holderElements, 3);
  v8::Local<v8::Function> rejectFn;
  if (!v8::Function::New(ctx, WaitForAllReject, resolver, 1)
           .ToLocal(&rejectFn)) {
    v8.Check(tryCatch);
    return nullptr;
  }
  for (uint32_t i = 0; i < count; i++) {
    JSObject* unwrapped = Unwrapped(aPromises[i]);
    v8::Local<v8::Promise> promise = PromiseOf(aCx, unwrapped);
    if (promise.IsEmpty()) {
      JS_ReportErrorNumberASCII(aCx, js::GetErrorMessage, nullptr,
                                JSMSG_DEAD_OBJECT);
      return nullptr;
    }
    v8::Local<v8::Value> elementData[] = {
        holder, v8::Integer::NewFromUnsigned(aCx->isolate, i),
        v8::False(aCx->isolate)};
    v8::Local<v8::Function> elementFn;
    if (!v8::Function::New(ctx, WaitForAllElement,
                           v8::Array::New(aCx->isolate, elementData, 3), 1)
             .ToLocal(&elementFn) ||
        promise->Then(ctx, elementFn, rejectFn).IsEmpty()) {
      v8.Check(tryCatch);
      return nullptr;
    }
  }
  v8::Local<v8::Value> remainingValue;
  if (!holder->Get(ctx, 2).ToLocal(&remainingValue)) {
    v8.Check(tryCatch);
    return nullptr;
  }
  int32_t remaining = remainingValue.As<v8::Int32>()->Value() - 1;
  (void)holder->Set(ctx, 2, v8::Integer::New(aCx->isolate, remaining));
  if (remaining == 0 && resolver->Resolve(ctx, values).IsNothing()) {
    v8.Check(tryCatch);
    return nullptr;
  }
  return ObjectFromV8(aCx, resolver->GetPromise());
}

void JS::Dispatchable::Run(JSContext* aCx,
                           js::UniquePtr<JS::Dispatchable>&& aTask,
                           MaybeShuttingDown aMaybeShuttingDown) {
  JS::Dispatchable* raw = aTask.release();
  ContextState* state = StateFor(aCx);
  auto it = std::find(state->liveTasks.begin(), state->liveTasks.end(), raw);
  bool live = it != state->liveTasks.end();
  if (live) {
    state->liveTasks.erase(it);
    if (state->asyncTaskFinished) {
      state->asyncTaskFinished(state->asyncTaskClosure, raw);
    }
  }
  raw->run(aCx, live && !state->shuttingDown ? aMaybeShuttingDown
                                             : JS::Dispatchable::ShuttingDown);
}

void JS::Dispatchable::ReleaseFailedTask(
    js::UniquePtr<JS::Dispatchable>&& aTask) {
  aTask.release()->transferToRuntime();
}

void JS::InitAsyncTaskCallbacks(
    JSContext* aCx, JS::DispatchToEventLoopCallback aDispatchCallback,
    JS::DelayedDispatchToEventLoopCallback aDelayedDispatchCallback,
    JS::AsyncTaskStartedCallback aAsyncTaskStartedCallback,
    JS::AsyncTaskFinishedCallback aAsyncTaskFinishedCallback, void* aClosure) {
  ContextState* state = StateFor(aCx);
  state->dispatchCallback = aDispatchCallback;
  state->delayedDispatchCallback = aDelayedDispatchCallback;
  state->asyncTaskStarted = aAsyncTaskStartedCallback;
  state->asyncTaskFinished = aAsyncTaskFinishedCallback;
  state->asyncTaskClosure = aClosure;
  state->shuttingDown = false;
}

void JS::CancelAsyncTasks(JSContext* aCx) {
  ContextState* state = StateFor(aCx);
  std::vector<JS::Dispatchable*> tasks;
  std::swap(tasks, state->liveTasks);
  for (JS::Dispatchable* task : tasks) {
    if (state->asyncTaskFinished) {
      state->asyncTaskFinished(state->asyncTaskClosure, task);
    }
  }
}

void JS::ShutdownAsyncTasks(JSContext* aCx) {
  JS::CancelAsyncTasks(aCx);
  StateFor(aCx)->shuttingDown = true;
}
