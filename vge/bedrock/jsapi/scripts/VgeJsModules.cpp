#include "VgeJsScripts.h"

#include "js/Object.h"
#include "js/Promise.h"
#include "js/TracingAPI.h"
#include "js/WasmModule.h"
#include "js/friend/ErrorMessages.h"

namespace vge::jsapi {

namespace {

enum class LoadStatus { New, Unlinked };

struct RequestEntry {
  JS::Heap<JSObject*> request;
  JS::Heap<JSObject*> loaded;
  std::u16string specifier;
  JS::ModuleType type = JS::ModuleType::JavaScript;
  uint32_t line = 0;
  uint32_t column = 1;
};

struct ModuleData {
  v8::Global<v8::Module> module;
  ScriptData* script = nullptr;
  bool synthetic = false;
  bool sourcePhase = false;
  bool cleared = false;
  LoadStatus loadStatus = LoadStatus::New;
  JSObject* self = nullptr;
  int hash = 0;
  std::vector<RequestEntry> requests;
  JS::Heap<JS::Value> privateValue;
  JS::Heap<JS::Value> syntheticDefault;
  JS::Heap<JSObject*> wasmSource;
};

thread_local std::unordered_multimap<int, ModuleData*> gModulesByHash;

enum RequestSlot : uint32_t { kSpecifier, kType, kSourcePhase, kRequestSlots };

ModuleData* DataOf(JSObject* aModule) {
  JS::Value slot = JS::GetReservedSlot(aModule, 0);
  return slot.isUndefined() ? nullptr
                            : static_cast<ModuleData*>(slot.toPrivate());
}

void ForgetModule(ModuleData* aData) {
  auto range = gModulesByHash.equal_range(aData->hash);
  for (auto it = range.first; it != range.second; ++it) {
    if (it->second == aData) {
      gModulesByHash.erase(it);
      return;
    }
  }
}

void FinalizeModule(JS::GCContext* aGcx, JSObject* aObj) {
  ModuleData* data = DataOf(aObj);
  if (!data) {
    return;
  }
  data->self = nullptr;
  Bury([data] {
    ForgetModule(data);
    if (data->script) {
      data->script->module = nullptr;
    }
    delete data;
  });
}

void TraceModule(JSTracer* aTrc, JSObject* aObj) {
  ModuleData* data = DataOf(aObj);
  if (!data) {
    return;
  }
  for (RequestEntry& entry : data->requests) {
    JS::TraceEdge(aTrc, &entry.request, "module request");
    JS::TraceEdge(aTrc, &entry.loaded, "loaded module");
  }
  JS::TraceEdge(aTrc, &data->privateValue, "module private");
  JS::TraceEdge(aTrc, &data->syntheticDefault, "synthetic default");
  JS::TraceEdge(aTrc, &data->wasmSource, "wasm source");
}

const JSClassOps kModuleOps = {
    nullptr, nullptr, nullptr,        nullptr, nullptr,
    nullptr, FinalizeModule, nullptr, nullptr, TraceModule,
};

struct LoadState {
  bool isLoading = true;
  uint32_t pending = 1;
  std::vector<JS::Heap<JSObject*>> visited;
  JS::Heap<JS::Value> hostDefined;
  JS::LoadModuleResolvedCallback resolved = nullptr;
  JS::LoadModuleRejectedCallback rejected = nullptr;
  JS::Heap<JSObject*> promise;
  JS::Heap<JSObject*> dynamicPromise;
  JS::Heap<JSObject*> dynamicModule;
};

LoadState* LoadStateOf(JSObject* aObj) {
  JS::Value slot = JS::GetReservedSlot(aObj, 0);
  return slot.isUndefined() ? nullptr
                            : static_cast<LoadState*>(slot.toPrivate());
}

void FinalizeLoadState(JS::GCContext* aGcx, JSObject* aObj) {
  if (LoadState* state = LoadStateOf(aObj)) {
    Bury([state] { delete state; });
  }
}

void TraceLoadState(JSTracer* aTrc, JSObject* aObj) {
  LoadState* state = LoadStateOf(aObj);
  if (!state) {
    return;
  }
  for (auto& module : state->visited) {
    JS::TraceEdge(aTrc, &module, "visited module");
  }
  JS::TraceEdge(aTrc, &state->hostDefined, "host defined");
  JS::TraceEdge(aTrc, &state->promise, "load promise");
  JS::TraceEdge(aTrc, &state->dynamicPromise, "dynamic promise");
  JS::TraceEdge(aTrc, &state->dynamicModule, "dynamic module");
}

const JSClassOps kLoadStateOps = {
    nullptr, nullptr, nullptr,           nullptr, nullptr,
    nullptr, FinalizeLoadState, nullptr, nullptr, TraceLoadState,
};

const JSClass GraphLoadingStateClass = {
    "GraphLoadingStateRecord",
    JSCLASS_HAS_RESERVED_SLOTS(1) | JSCLASS_FOREGROUND_FINALIZE,
    &kLoadStateOps,
};

JS::ModuleType TypeFromAttributes(JSContext* aCx,
                                  v8::Local<v8::FixedArray> aAttributes,
                                  int aStride) {
  if (aAttributes.IsEmpty()) {
    return JS::ModuleType::JavaScript;
  }
  v8::Local<v8::Context> context = aCx->v8Context();
  for (int i = 0; i + 1 < aAttributes->Length(); i += aStride) {
    v8::Local<v8::Value> key = aAttributes->Get(context, i).As<v8::Value>();
    v8::Local<v8::Value> value =
        aAttributes->Get(context, i + 1).As<v8::Value>();
    if (Utf8Of(aCx->isolate, key) != "type") {
      return JS::ModuleType::Unknown;
    }
    std::string type = Utf8Of(aCx->isolate, value);
    if (type == "json") {
      return JS::ModuleType::JSON;
    }
    if (type == "css") {
      return JS::ModuleType::CSS;
    }
    if (type == "bytes") {
      return JS::ModuleType::Bytes;
    }
    if (type == "text") {
      return JS::ModuleType::Text;
    }
    return JS::ModuleType::Unknown;
  }
  return JS::ModuleType::JavaScript;
}

JSObject* NewRequestObject(JSContext* aCx, const std::u16string& aSpecifier,
                           JS::ModuleType aType, bool aSourcePhase) {
  JS::Rooted<JSObject*> request(
      aCx, NewClassObject(aCx, &ModuleRequestClass, nullptr));
  if (!request) {
    return nullptr;
  }
  v8::HandleScope scope(aCx->isolate);
  JSString* specifier = AtomFromV8(
      aCx, V8String(aCx->isolate, aSpecifier.data(), aSpecifier.size()));
  JS::SetReservedSlot(request, kSpecifier, JS::StringValue(specifier));
  JS::SetReservedSlot(request, kType, JS::Int32Value(int32_t(aType)));
  JS::SetReservedSlot(request, kSourcePhase, JS::BooleanValue(aSourcePhase));
  return request;
}

v8::Local<v8::Private> ModuleHolderKey(v8::Isolate* aIsolate) {
  return v8::Private::ForApi(
      aIsolate, v8::String::NewFromUtf8Literal(aIsolate, "vge.moduleHolder"));
}

bool HoldModuleFromRecord(JSContext* aCx, v8::Local<v8::Object> aRecord,
                          v8::Local<v8::Module> aModule) {
  v8::Isolate* isolate = aCx->isolate;
  v8::Local<v8::Context> context = aRecord->GetCreationContextChecked();
  v8::Local<v8::ObjectTemplate> holderTemplate =
      v8::ObjectTemplate::New(isolate);
  holderTemplate->SetInternalFieldCount(1);
  v8::Local<v8::Object> holder;
  if (!holderTemplate->NewInstance(context).ToLocal(&holder)) {
    return false;
  }
  holder->SetInternalField(0, reinterpret_cast<v8::Local<v8::Value>&>(aModule));
  return aRecord->SetPrivate(context, ModuleHolderKey(isolate), holder)
      .FromMaybe(false);
}

void ReleaseModuleFromRecord(JSContext* aCx, v8::Local<v8::Object> aRecord) {
  v8::Isolate* isolate = aCx->isolate;
  aRecord
      ->DeletePrivate(aRecord->GetCreationContextChecked(),
                      ModuleHolderKey(isolate))
      .Check();
}

JSObject* NewModuleRecord(JSContext* aCx, ModuleData* aData) {
  JS::Rooted<JSObject*> object(
      aCx, NewClassObject(aCx, &ModuleObjectClass, nullptr));
  if (!object) {
    delete aData;
    return nullptr;
  }
  JS::SetReservedSlot(object, 0, JS::PrivateValue(aData));
  aData->self = object;
  if (!aData->module.IsEmpty()) {
    v8::HandleScope scope(aCx->isolate);
    v8::Local<v8::Module> module = aData->module.Get(aCx->isolate);
    if (!HoldModuleFromRecord(aCx, ToV8(aCx, object.get()), module)) {
      return nullptr;
    }
    aData->module.SetWeak();
    aData->hash = module->GetIdentityHash();
    gModulesByHash.emplace(aData->hash, aData);
  }
  return object;
}

bool RequestMatches(const RequestEntry& aEntry, JSObject* aRequest) {
  if (aEntry.request.unbarrieredGet() == aRequest) {
    return true;
  }
  JSContext* cx = CurrentContext();
  v8::HandleScope scope(cx->isolate);
  std::u16string specifier = Utf16Of(
      cx->isolate, ToV8(cx, JS::GetReservedSlot(aRequest, kSpecifier)));
  JS::ModuleType type =
      JS::ModuleType(JS::GetReservedSlot(aRequest, kType).toInt32());
  return aEntry.specifier == specifier && aEntry.type == type;
}

bool CallResolved(JSContext* aCx, JS::Handle<JSObject*> aStateObject) {
  LoadState* state = LoadStateOf(aStateObject);
  JS::Rooted<JS::Value> hostDefined(aCx, state->hostDefined);
  if (state->resolved) {
    return state->resolved(aCx, hostDefined);
  }
  if (state->promise) {
    JS::Rooted<JSObject*> promise(aCx, state->promise);
    return JS::ResolvePromise(aCx, promise, JS::UndefinedHandleValue);
  }
  return true;
}

bool CallRejected(JSContext* aCx, JS::Handle<JSObject*> aStateObject,
                  JS::HandleValue aError) {
  LoadState* state = LoadStateOf(aStateObject);
  JS::Rooted<JS::Value> hostDefined(aCx, state->hostDefined);
  if (state->rejected) {
    return state->rejected(aCx, hostDefined, aError);
  }
  if (state->promise) {
    JS::Rooted<JSObject*> promise(aCx, state->promise);
    return JS::RejectPromise(aCx, promise, aError);
  }
  return true;
}

bool HostLoadImportedModule(JSContext* aCx, JS::HandleValue aReferrer,
                            JS::HandleObject aRequest,
                            JS::HandleValue aHostDefined,
                            JS::HandleValue aPayload, uint32_t aLine,
                            uint32_t aColumn);
bool DynamicLoadResolved(JSContext* aCx, JS::Handle<JSObject*> aStateObject);
bool DynamicLoadRejected(JSContext* aCx, JS::Handle<JSObject*> aStateObject,
                         JS::HandleValue aError);

bool StateResolved(JSContext* aCx, JS::Handle<JSObject*> aStateObject) {
  if (LoadStateOf(aStateObject)->dynamicPromise) {
    return DynamicLoadResolved(aCx, aStateObject);
  }
  return CallResolved(aCx, aStateObject);
}

bool StateRejected(JSContext* aCx, JS::Handle<JSObject*> aStateObject,
                   JS::HandleValue aError) {
  if (LoadStateOf(aStateObject)->dynamicPromise) {
    return DynamicLoadRejected(aCx, aStateObject, aError);
  }
  return CallRejected(aCx, aStateObject, aError);
}

bool InnerModuleLoading(JSContext* aCx, JS::Handle<JSObject*> aStateObject,
                        JS::Handle<JSObject*> aModule) {
  LoadState* state = LoadStateOf(aStateObject);
  ModuleData* data = DataOf(aModule);
  bool visited = false;
  for (auto& entry : state->visited) {
    if (entry.unbarrieredGet() == aModule.get()) {
      visited = true;
      break;
    }
  }
  if (data && !data->synthetic && !data->sourcePhase &&
      data->loadStatus == LoadStatus::New && !visited) {
    state->visited.emplace_back(aModule.get());
    state->pending += uint32_t(data->requests.size());
    for (size_t i = 0; i < data->requests.size(); i++) {
      JS::Rooted<JSObject*> request(aCx, data->requests[i].request);
      JS::Rooted<JSObject*> loaded(aCx, data->requests[i].loaded);
      if (data->requests[i].type == JS::ModuleType::Unknown) {
        ThrowNumberedError(
            aCx, js::GetErrorMessage, nullptr,
            JSMSG_IMPORT_ATTRIBUTES_STATIC_IMPORT_UNSUPPORTED_ATTRIBUTE,
            "type");
        JS::Rooted<JS::Value> error(aCx);
        JS_GetPendingException(aCx, &error);
        JS_ClearPendingException(aCx);
        state->isLoading = false;
        return StateRejected(aCx, aStateObject, error);
      }
      if (loaded) {
        if (!InnerModuleLoading(aCx, aStateObject, loaded)) {
          return false;
        }
      } else {
        JS::Rooted<JS::Value> referrer(aCx, JS::ObjectValue(*aModule));
        JS::Rooted<JS::Value> hostDefined(aCx, state->hostDefined);
        JS::Rooted<JS::Value> payload(aCx, JS::ObjectValue(*aStateObject));
        if (!HostLoadImportedModule(aCx, referrer, request, hostDefined,
                                    payload, data->requests[i].line,
                                    data->requests[i].column)) {
          return false;
        }
      }
      state = LoadStateOf(aStateObject);
      if (!state->isLoading) {
        return true;
      }
    }
  }
  state->pending--;
  if (state->pending == 0) {
    state->isLoading = false;
    for (auto& entry : state->visited) {
      if (ModuleData* loadedData = DataOf(entry.unbarrieredGet())) {
        if (loadedData->loadStatus == LoadStatus::New) {
          loadedData->loadStatus = LoadStatus::Unlinked;
        }
      }
    }
    return StateResolved(aCx, aStateObject);
  }
  return true;
}

bool ContinueModuleLoading(JSContext* aCx, JS::Handle<JSObject*> aStateObject,
                           JS::Handle<JSObject*> aModule,
                           JS::HandleValue aError) {
  LoadState* state = LoadStateOf(aStateObject);
  if (!state->isLoading) {
    return true;
  }
  if (aModule) {
    return InnerModuleLoading(aCx, aStateObject, aModule);
  }
  state->isLoading = false;
  return StateRejected(aCx, aStateObject, aError);
}

JSObject* NewLoadState(JSContext* aCx, JS::HandleValue aHostDefined) {
  JS::Rooted<JSObject*> object(
      aCx, NewClassObject(aCx, &GraphLoadingStateClass, nullptr));
  if (!object) {
    return nullptr;
  }
  LoadState* state = new LoadState();
  state->hostDefined = aHostDefined;
  JS::SetReservedSlot(object, 0, JS::PrivateValue(state));
  return object;
}

v8::Local<v8::Private> ResolverKey(v8::Isolate* aIsolate) {
  return v8::Private::ForApi(
      aIsolate,
      v8::String::NewFromUtf8Literal(aIsolate, "vge.scripts.resolver"));
}

void SettleDynamic(JSContext* aCx, JS::HandleObject aPromise, bool aResolve,
                   v8::Local<v8::Value> aValue) {
  v8::Local<v8::Context> context = aCx->v8Context();
  v8::Local<v8::Object> promise = ToV8(aCx, aPromise.get());
  v8::Local<v8::Value> stored;
  if (!promise->GetPrivate(context, ResolverKey(aCx->isolate))
           .ToLocal(&stored) ||
      !stored->IsObject()) {
    JS::Rooted<JS::Value> value(aCx, ValueFromV8(aCx, aValue));
    if (aResolve) {
      JS::ResolvePromise(aCx, aPromise, value);
    } else {
      JS::RejectPromise(aCx, aPromise, value);
    }
    return;
  }
  v8::Local<v8::Promise::Resolver> resolver =
      stored.As<v8::Promise::Resolver>();
  if (aResolve) {
    resolver->Resolve(context, aValue).Check();
  } else {
    resolver->Reject(context, aValue).Check();
  }
}

void RejectDynamicWithPending(JSContext* aCx, JS::HandleObject aPromise) {
  JS::Rooted<JS::Value> error(aCx);
  if (!JS_GetPendingException(aCx, &error)) {
    error.setUndefined();
  }
  JS_ClearPendingException(aCx);
  v8::HandleScope scope(aCx->isolate);
  SettleDynamic(aCx, aPromise, false, ToV8(aCx, error));
}

void DynamicFulfilled(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  JSContext* cx = CurrentContext();
  v8::Local<v8::Context> ctx = cx->v8Context();
  v8::Local<v8::Array> data = aInfo.Data().As<v8::Array>();
  JS::Rooted<JSObject*> promise(
      cx,
      ObjectFromV8(cx, data->Get(ctx, 0).ToLocalChecked().As<v8::Object>()));
  JS::Rooted<JSObject*> module(
      cx,
      ObjectFromV8(cx, data->Get(ctx, 1).ToLocalChecked().As<v8::Object>()));
  JSObject* ns = JS::GetModuleNamespace(cx, module);
  if (!ns) {
    RejectDynamicWithPending(cx, promise);
    return;
  }
  SettleDynamic(cx, promise, true, ToV8(cx, ns));
}

void DynamicRejected(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  JSContext* cx = CurrentContext();
  v8::Local<v8::Context> ctx = cx->v8Context();
  v8::Local<v8::Array> data = aInfo.Data().As<v8::Array>();
  JS::Rooted<JSObject*> promise(
      cx,
      ObjectFromV8(cx, data->Get(ctx, 0).ToLocalChecked().As<v8::Object>()));
  SettleDynamic(cx, promise, false, aInfo[0]);
}

bool DynamicLoadResolved(JSContext* aCx, JS::Handle<JSObject*> aStateObject) {
  LoadState* state = LoadStateOf(aStateObject);
  JS::Rooted<JSObject*> promise(aCx, state->dynamicPromise);
  JS::Rooted<JSObject*> module(aCx, state->dynamicModule);
  if (!JS::ModuleLink(aCx, module)) {
    RejectDynamicWithPending(aCx, promise);
    return true;
  }
  JS::Rooted<JS::Value> evaluation(aCx);
  if (!JS::ModuleEvaluate(aCx, module, &evaluation) ||
      !evaluation.isObject()) {
    RejectDynamicWithPending(aCx, promise);
    return true;
  }
  AutoV8 scope(aCx);
  v8::Local<v8::Context> context = scope.context();
  v8::Local<v8::Object> evaluationPromise = ToV8(aCx, &evaluation.toObject());
  if (!evaluationPromise->IsPromise()) {
    return true;
  }
  v8::Local<v8::Array> holder = v8::Array::New(aCx->isolate, 2);
  holder->Set(context, 0, ToV8(aCx, promise.get())).Check();
  holder->Set(context, 1, ToV8(aCx, module.get())).Check();
  v8::Local<v8::Function> onFulfilled;
  v8::Local<v8::Function> onRejected;
  if (!v8::Function::New(context, DynamicFulfilled, holder)
           .ToLocal(&onFulfilled) ||
      !v8::Function::New(context, DynamicRejected, holder)
           .ToLocal(&onRejected)) {
    return true;
  }
  v8::Local<v8::Promise> chained;
  if (evaluationPromise.As<v8::Promise>()
          ->Then(context, onFulfilled, onRejected)
          .ToLocal(&chained)) {
    chained->MarkAsHandled();
  }
  return true;
}

bool DynamicLoadRejected(JSContext* aCx, JS::Handle<JSObject*> aStateObject,
                         JS::HandleValue aError) {
  LoadState* state = LoadStateOf(aStateObject);
  JS::Rooted<JSObject*> promise(aCx, state->dynamicPromise);
  v8::HandleScope scope(aCx->isolate);
  SettleDynamic(aCx, promise, false, ToV8(aCx, aError));
  return true;
}

bool ContinueDynamicImport(JSContext* aCx, JS::HandleObject aPromise,
                           JS::HandleObject aModule) {
  JS::Rooted<JS::Value> undefined(aCx);
  JS::Rooted<JSObject*> stateObject(aCx, NewLoadState(aCx, undefined));
  if (!stateObject) {
    RejectDynamicWithPending(aCx, aPromise);
    return true;
  }
  LoadState* state = LoadStateOf(stateObject);
  state->dynamicPromise = aPromise;
  state->dynamicModule = aModule;
  ModuleData* data = DataOf(aModule);
  if (data && (data->synthetic || data->sourcePhase)) {
    return DynamicLoadResolved(aCx, stateObject);
  }
  return InnerModuleLoading(aCx, stateObject, aModule);
}

bool HostLoadImportedModule(JSContext* aCx, JS::HandleValue aReferrer,
                            JS::HandleObject aRequest,
                            JS::HandleValue aHostDefined,
                            JS::HandleValue aPayload, uint32_t aLine,
                            uint32_t aColumn) {
  JS::ModuleLoadHook hook = RuntimeState(aCx->runtime).moduleLoadHook;
  if (!hook) {
    JS_ReportErrorASCII(aCx, "Module load hook not set");
    return false;
  }
  if (!hook(aCx, aReferrer, aRequest, aHostDefined, aPayload, aLine,
            JS::ColumnNumberOneOrigin(aColumn))) {
    if (JS_IsExceptionPending(aCx)) {
      return JS::FinishLoadingImportedModuleFailedWithPendingException(
          aCx, aPayload);
    }
    return JS::FinishLoadingImportedModuleFailed(aCx, aPayload,
                                                 JS::UndefinedHandleValue);
  }
  return true;
}

v8::MaybeLocal<v8::Module> ResolveModule(v8::Local<v8::Context> aContext,
                                         v8::Local<v8::String> aSpecifier,
                                         v8::Local<v8::FixedArray> aAttributes,
                                         v8::Local<v8::Module> aReferrer) {
  JSContext* cx = CurrentContext();
  JSObject* referrer = ModuleObjectForV8(cx, aReferrer);
  ModuleData* data = referrer ? DataOf(referrer) : nullptr;
  std::u16string specifier = Utf16Of(cx->isolate, aSpecifier);
  JS::ModuleType type = TypeFromAttributes(cx, aAttributes, 3);
  if (data) {
    for (RequestEntry& entry : data->requests) {
      if (entry.specifier != specifier || entry.type != type) {
        continue;
      }
      JSObject* loaded = entry.loaded;
      ModuleData* loadedData = loaded ? DataOf(loaded) : nullptr;
      if (loadedData && !loadedData->module.IsEmpty()) {
        return loadedData->module.Get(cx->isolate);
      }
    }
  }
  std::string text = "Module not loaded: " +
                     Utf8FromUtf16(specifier.data(), specifier.size());
  cx->isolate->ThrowException(
      v8::Exception::SyntaxError(V8String(cx->isolate, text.c_str())));
  return v8::MaybeLocal<v8::Module>();
}

v8::MaybeLocal<v8::Value> SyntheticSteps(v8::Local<v8::Context> aContext,
                                         v8::Local<v8::Module> aModule) {
  JSContext* cx = CurrentContext();
  v8::Isolate* isolate = cx->isolate;
  JSObject* object = ModuleObjectForV8(cx, aModule);
  ModuleData* data = object ? DataOf(object) : nullptr;
  v8::Local<v8::Value> value =
      data ? ToV8(cx, data->syntheticDefault.get())
           : v8::Local<v8::Value>(v8::Undefined(isolate));
  if (aModule
          ->SetSyntheticModuleExport(isolate, V8String(isolate, "default"),
                                     value)
          .IsNothing()) {
    return v8::MaybeLocal<v8::Value>();
  }
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(aContext).ToLocal(&resolver)) {
    return v8::MaybeLocal<v8::Value>();
  }
  resolver->Resolve(aContext, v8::Undefined(isolate)).Check();
  return resolver->GetPromise();
}

v8::MaybeLocal<v8::Promise> ImportDynamically(
    v8::Local<v8::Context> aContext, v8::Local<v8::Data> aHostDefined,
    v8::Local<v8::Value> aResourceName, v8::Local<v8::String> aSpecifier,
    v8::Local<v8::FixedArray> aAttributes) {
  JSContext* cx = CurrentContext();
  v8::Isolate* isolate = cx->isolate;
  v8::EscapableHandleScope scope(isolate);
  JS::Realm* old = cx->realm();
  JS::Realm* realm = RealmOfV8Context(cx, aContext);
  cx->enterRealm(realm ? realm : old);
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(aContext).ToLocal(&resolver)) {
    cx->leaveRealm(old);
    return v8::MaybeLocal<v8::Promise>();
  }
  v8::Local<v8::Promise> promise = resolver->GetPromise();
  promise->SetPrivate(aContext, ResolverKey(isolate), resolver).Check();
  JS::Rooted<JS::Value> referrer(cx);
  if (!aHostDefined.IsEmpty() && aHostDefined->IsFixedArray()) {
    v8::Local<v8::PrimitiveArray> options =
        aHostDefined.As<v8::PrimitiveArray>();
    if (options->Length() == 2) {
      uint32_t id =
          options->Get(isolate, 1)->Uint32Value(aContext).FromMaybe(0);
      if (ScriptData* data = ScriptDataByReferrerId(id)) {
        referrer = data->module ? JS::ObjectValue(*data->module)
                                : data->privateValue;
      }
    }
  }
  JS::ModuleType type = TypeFromAttributes(cx, aAttributes, 2);
  JS::Rooted<JSObject*> request(
      cx, NewRequestObject(cx, Utf16Of(isolate, aSpecifier), type, false));
  JS::Rooted<JSObject*> promiseObject(cx, ObjectFromV8(cx, promise));
  if (type == JS::ModuleType::Unknown) {
    ThrowNumberedError(
        cx, js::GetErrorMessage, nullptr,
        JSMSG_IMPORT_ATTRIBUTES_DYNAMIC_IMPORT_UNSUPPORTED_ATTRIBUTE, "type");
    RejectDynamicWithPending(cx, promiseObject);
  } else if (request) {
    JS::Rooted<JS::Value> payload(cx, JS::ObjectValue(*promiseObject));
    HostLoadImportedModule(cx, referrer, request, JS::UndefinedHandleValue,
                           payload, 0, 1);
  } else {
    RejectDynamicWithPending(cx, promiseObject);
  }
  cx->leaveRealm(old);
  return scope.Escape(promise);
}

void InitializeImportMeta(v8::Local<v8::Context> aContext,
                          v8::Local<v8::Module> aModule,
                          v8::Local<v8::Object> aMeta) {
  JSContext* cx = CurrentContext();
  JS::ModuleMetadataHook hook = RuntimeState(cx->runtime).moduleMetadataHook;
  JS::Rooted<JSObject*> module(cx, ModuleObjectForV8(cx, aModule));
  if (!hook || !module) {
    return;
  }
  JS::Rooted<JSObject*> meta(cx, ObjectFromV8(cx, aMeta));
  if (!hook(cx, module, meta) && JS_IsExceptionPending(cx)) {
    JS::Rooted<JS::Value> error(cx);
    JS_GetPendingException(cx, &error);
    JS_ClearPendingException(cx);
    cx->isolate->ThrowException(ToV8(cx, error));
  }
}

bool LinkModule(JSContext* aCx, JS::HandleObject aModule) {
  ModuleData* data = DataOf(aModule);
  if (!data || data->module.IsEmpty()) {
    return true;
  }
  AutoV8 scope(aCx);
  v8::Local<v8::Module> module = data->module.Get(aCx->isolate);
  if (module->GetStatus() != v8::Module::kUninstantiated) {
    return true;
  }
  v8::TryCatch tryCatch(aCx->isolate);
  bool ok = false;
  if (!module->InstantiateModule(scope.context(), ResolveModule).To(&ok) ||
      !ok) {
    return ReportPending(aCx, tryCatch);
  }
  return true;
}

void RethrowRejection(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  aInfo.GetIsolate()->ThrowException(aInfo[0]);
}

}

const JSClass ModuleObjectClass = {
    "Module",
    JSCLASS_HAS_RESERVED_SLOTS(1) | JSCLASS_FOREGROUND_FINALIZE,
    &kModuleOps,
};

const JSClass ModuleRequestClass = {
    "ModuleRequest",
    JSCLASS_HAS_RESERVED_SLOTS(kRequestSlots),
};

JSObject* ModuleObjectForV8(JSContext* aCx, v8::Local<v8::Module> aModule) {
  auto range = gModulesByHash.equal_range(aModule->GetIdentityHash());
  for (auto it = range.first; it != range.second; ++it) {
    ModuleData* data = it->second;
    if (data->self && !data->module.IsEmpty() &&
        data->module.Get(aCx->isolate) == aModule) {
      return data->self;
    }
  }
  return nullptr;
}

JSObject* NewModuleObjectFor(JSContext* aCx, v8::Local<v8::Module> aModule,
                             ScriptData* aScript) {
  ModuleData* data = new ModuleData();
  data->module.Reset(aCx->isolate, aModule);
  data->script = aScript;
  data->synthetic = aModule->IsSyntheticModule();
  if (aScript) {
    aScript->isModule = true;
  }
  v8::Local<v8::Context> context = aCx->v8Context();
  v8::Local<v8::FixedArray> requests = aModule->GetModuleRequests();
  JS::Rooted<JSObject*> object(aCx, NewModuleRecord(aCx, data));
  if (!object) {
    return nullptr;
  }
  for (int i = 0; i < requests->Length(); i++) {
    v8::Local<v8::ModuleRequest> request =
        requests->Get(context, i).As<v8::ModuleRequest>();
    RequestEntry entry;
    entry.specifier = Utf16Of(aCx->isolate, request->GetSpecifier());
    entry.type = TypeFromAttributes(aCx, request->GetImportAssertions(), 3);
    v8::Location location =
        aModule->SourceOffsetToLocation(request->GetSourceOffset());
    entry.line = uint32_t(location.GetLineNumber() + 1);
    entry.column = uint32_t(location.GetColumnNumber() + 1);
    JSObject* requestObject =
        NewRequestObject(aCx, entry.specifier, entry.type, false);
    if (!requestObject) {
      return nullptr;
    }
    data->requests.push_back(std::move(entry));
    data->requests.back().request = requestObject;
  }
  if (aScript) {
    aScript->module = object;
  }
  return object;
}

void InstallModuleCallbacks(v8::Isolate* aIsolate) {
  aIsolate->SetHostImportModuleDynamicallyCallback(ImportDynamically);
  aIsolate->SetHostInitializeImportMetaObjectCallback(InitializeImportMeta);
}

JSObject* NewWasmSourcePhaseModule(JSContext* aCx, JS::HandleObject aWasm) {
  ModuleData* data = new ModuleData();
  data->sourcePhase = true;
  data->wasmSource = aWasm;
  data->loadStatus = LoadStatus::Unlinked;
  return NewModuleRecord(aCx, data);
}

}

using namespace vge::jsapi;

void JS::SetModuleLoadHook(JSRuntime* rt, JS::ModuleLoadHook func) {
  RuntimeState(rt).moduleLoadHook = func;
}

void JS::SetModuleMetadataHook(JSRuntime* rt, JS::ModuleMetadataHook func) {
  RuntimeState(rt).moduleMetadataHook = func;
}

JS::Value JS::GetReferrerPrivate(Handle<Value> referrer) {
  if (referrer.isObject() &&
      JS::GetClass(&referrer.toObject()) == &ModuleObjectClass) {
    return JS::GetModulePrivate(&referrer.toObject());
  }
  return referrer;
}

bool JS::FinishLoadingImportedModule(JSContext* cx, Handle<Value> referrer,
                                     Handle<JSObject*> moduleRequest,
                                     Handle<Value> payload,
                                     Handle<JSObject*> result,
                                     bool usePromise) {
  if (referrer.isObject() &&
      JS::GetClass(&referrer.toObject()) == &ModuleObjectClass) {
    ModuleData* data = DataOf(&referrer.toObject());
    for (RequestEntry& entry : data->requests) {
      if (RequestMatches(entry, moduleRequest)) {
        if (!entry.loaded) {
          entry.loaded = result;
        }
        break;
      }
    }
  }
  JS::Rooted<JSObject*> object(cx, &payload.toObject());
  if (JS::GetClass(object) == &GraphLoadingStateClass) {
    return ContinueModuleLoading(cx, object, result, JS::UndefinedHandleValue);
  }
  return ContinueDynamicImport(cx, object, result);
}

bool JS::FinishLoadingImportedModuleFailed(JSContext* cx,
                                           Handle<Value> payloadArg,
                                           Handle<Value> error) {
  JS::Rooted<JSObject*> payload(cx, &payloadArg.toObject());
  if (JS::GetClass(payload) == &GraphLoadingStateClass) {
    return ContinueModuleLoading(cx, payload, nullptr, error);
  }
  v8::HandleScope scope(cx->isolate);
  SettleDynamic(cx, payload, false, ToV8(cx, error));
  return true;
}

bool JS::FinishLoadingImportedModuleFailedWithPendingException(
    JSContext* cx, Handle<Value> payload) {
  JS::Rooted<JS::Value> error(cx);
  JS_GetPendingException(cx, &error);
  JS_ClearPendingException(cx);
  return FinishLoadingImportedModuleFailed(cx, payload, error);
}

bool JS::LoadRequestedModules(JSContext* cx, Handle<JSObject*> module,
                              Handle<Value> hostDefined,
                              LoadModuleResolvedCallback resolved,
                              LoadModuleRejectedCallback rejected) {
  ModuleData* data = DataOf(module);
  if (data && (data->synthetic || data->sourcePhase)) {
    return resolved(cx, hostDefined);
  }
  JS::Rooted<JSObject*> stateObject(cx, NewLoadState(cx, hostDefined));
  if (!stateObject) {
    return false;
  }
  LoadState* state = LoadStateOf(stateObject);
  state->resolved = resolved;
  state->rejected = rejected;
  if (!InnerModuleLoading(cx, stateObject, module)) {
    LoadStateOf(stateObject)->isLoading = false;
    return false;
  }
  return true;
}

bool JS::LoadRequestedModules(JSContext* cx, Handle<JSObject*> module,
                              Handle<Value> hostDefined,
                              MutableHandle<JSObject*> promiseOut) {
  JS::Rooted<JSObject*> promise(cx, JS::NewPromiseObject(cx, nullptr));
  if (!promise) {
    return false;
  }
  ModuleData* data = DataOf(module);
  if (data && (data->synthetic || data->sourcePhase)) {
    promiseOut.set(promise);
    return JS::ResolvePromise(cx, promise, JS::UndefinedHandleValue);
  }
  JS::Rooted<JSObject*> stateObject(cx, NewLoadState(cx, hostDefined));
  if (!stateObject) {
    return false;
  }
  LoadStateOf(stateObject)->promise = promise;
  if (!InnerModuleLoading(cx, stateObject, module)) {
    LoadStateOf(stateObject)->isLoading = false;
    return false;
  }
  promiseOut.set(promise);
  return true;
}

JSObject* JS::CompileJsonModule(JSContext* cx,
                                const ReadOnlyCompileOptions& options,
                                SourceText<char16_t>& srcBuf) {
  AutoV8 scope(cx);
  v8::TryCatch tryCatch(cx->isolate);
  v8::Local<v8::Value> parsed;
  if (!v8::JSON::Parse(scope.context(),
                       V8String(cx->isolate, srcBuf.get(), srcBuf.length()))
           .ToLocal(&parsed)) {
    ReportPending(cx, tryCatch);
    return nullptr;
  }
  JS::Rooted<JS::Value> value(cx, ValueFromV8(cx, parsed));
  return JS::CreateDefaultExportSyntheticModule(cx, value);
}

JSObject* JS::CompileJsonModule(JSContext* cx,
                                const ReadOnlyCompileOptions& options,
                                SourceText<mozilla::Utf8Unit>& srcBuf) {
  std::u16string text = Utf16FromUtf8(srcBuf.get(), srcBuf.length());
  JS::SourceText<char16_t> source;
  if (!source.init(cx, text.data(), text.size(),
                   JS::SourceOwnership::Borrowed)) {
    return nullptr;
  }
  return CompileJsonModule(cx, options, source);
}

JSObject* JS::CreateDefaultExportSyntheticModule(JSContext* cx,
                                                 Handle<Value> defaultExport) {
  EnsureIsolateSetup(cx);
  AutoV8 scope(cx);
  v8::Isolate* isolate = cx->isolate;
  std::vector<v8::Local<v8::String>> names = {V8String(isolate, "default")};
  v8::Local<v8::Module> module = v8::Module::CreateSyntheticModule(
      isolate, v8::String::Empty(isolate), names, SyntheticSteps);
  JS::Rooted<JSObject*> object(cx, NewModuleObjectFor(cx, module, nullptr));
  if (!object) {
    return nullptr;
  }
  ModuleData* data = DataOf(object);
  data->syntheticDefault = defaultExport;
  data->loadStatus = LoadStatus::Unlinked;
  return object;
}

JSObject* JS::CompileWasmModule(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    js::Vector<uint8_t, 0, js::MallocAllocPolicy>& srcBuf) {
  ThrowNumberedError(cx, js::GetErrorMessage, nullptr,
                           JSMSG_WASM_COMPILE_ERROR,
                           "Compilation of wasm modules not implemented.");
  return nullptr;
}

JSObject* JS::CompileWasmModuleAsSource(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    js::Vector<uint8_t, 0, js::MallocAllocPolicy>& srcBuf) {
  JS::SharedWasmCompileArgs compileArgs =
      JS::BuildCompileArgsForESM(cx, options);
  if (!compileArgs) {
    return nullptr;
  }
  JS::ESMCompileResult compileResult =
      JS::CompileForESM(*compileArgs, srcBuf.begin(), srcBuf.length());
  JS::Rooted<JSObject*> wasmModuleObject(cx);
  if (!JS::FinishCompileForESM(cx, *compileArgs, compileResult,
                               &wasmModuleObject)) {
    return nullptr;
  }
  return JS::CreateWasmSourcePhaseModule(cx, wasmModuleObject);
}

JSObject* JS::CreateWasmSourcePhaseModule(JSContext* cx,
                                          Handle<JSObject*> wasmModuleObject) {
  return NewWasmSourcePhaseModule(cx, wasmModuleObject);
}

void JS::SetModulePrivate(JSObject* module, const Value& value) {
  ModuleData* data = DataOf(module);
  data->privateValue = value;
  if (data->script) {
    data->script->privateValue = value;
  }
}

JS::Value JS::GetModulePrivate(JSObject* module) {
  return DataOf(module)->privateValue;
}

bool JS::IsCyclicModule(JSObject* module) {
  ModuleData* data = DataOf(module);
  return data && !data->synthetic && !data->sourcePhase;
}

void JS::ResetPreloadedModule(JSObject* module) {
  if (!JS::IsCyclicModule(module)) {
    return;
  }
  ModuleData* data = DataOf(module);
  data->loadStatus = LoadStatus::New;
  for (RequestEntry& entry : data->requests) {
    entry.loaded = nullptr;
  }
}

bool JS::ModuleLink(JSContext* cx, Handle<JSObject*> moduleArg) {
  ModuleData* data = DataOf(moduleArg);
  if (data && data->loadStatus == LoadStatus::New && !data->synthetic &&
      !data->sourcePhase) {
    ThrowNumberedError(cx, js::GetErrorMessage, nullptr,
                       JSMSG_BAD_MODULE_STATUS, "new");
    return false;
  }
  return LinkModule(cx, moduleArg);
}

bool JS::ModuleEvaluate(JSContext* cx, Handle<JSObject*> moduleRecord,
                        MutableHandle<JS::Value> rval) {
  ModuleData* data = DataOf(moduleRecord);
  AutoV8 scope(cx);
  v8::Local<v8::Context> context = scope.context();
  if (!data || data->module.IsEmpty()) {
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
      return false;
    }
    resolver->Resolve(context, v8::Undefined(cx->isolate)).Check();
    rval.set(ValueFromV8(cx, resolver->GetPromise()));
    return true;
  }
  v8::Local<v8::Module> module = data->module.Get(cx->isolate);
  if (data->synthetic && module->GetStatus() == v8::Module::kUninstantiated &&
      !LinkModule(cx, moduleRecord)) {
    return false;
  }
  if (module->GetStatus() == v8::Module::kUninstantiated ||
      module->GetStatus() == v8::Module::kInstantiating) {
    ThrowNumberedError(cx, js::GetErrorMessage, nullptr,
                       JSMSG_BAD_MODULE_STATUS, "unlinked");
    return false;
  }
  v8::TryCatch tryCatch(cx->isolate);
  v8::Local<v8::Value> result;
  if (!module->Evaluate(context).ToLocal(&result)) {
    return ReportPending(cx, tryCatch);
  }
  rval.set(ValueFromV8(cx, result));
  return true;
}

bool JS::ThrowOnModuleEvaluationFailure(JSContext* cx,
                                        Handle<JSObject*> evaluationPromise,
                                        ModuleErrorBehaviour errorBehaviour) {
  if (!evaluationPromise) {
    return false;
  }
  AutoV8 scope(cx);
  v8::Local<v8::Context> context = scope.context();
  v8::Local<v8::Object> object = ToV8(cx, evaluationPromise.get());
  if (!object->IsPromise()) {
    return true;
  }
  v8::Local<v8::Promise> promise = object.As<v8::Promise>();
  if (errorBehaviour == JS::ThrowModuleErrorsSync) {
    promise->MarkAsHandled();
    if (promise->State() != v8::Promise::kRejected) {
      return true;
    }
    JS::Rooted<JS::Value> error(cx, ValueFromV8(cx, promise->Result()));
    JS_SetPendingException(cx, error);
    return false;
  }
  v8::Local<v8::Function> onRejected;
  if (!v8::Function::New(context, RethrowRejection).ToLocal(&onRejected)) {
    return false;
  }
  v8::Local<v8::Promise> chained;
  return promise->Catch(context, onRejected).ToLocal(&chained);
}

JSScript* JS::GetModuleScript(JS::HandleObject moduleRecord) {
  ModuleData* data = DataOf(moduleRecord);
  if (!data || data->synthetic || data->sourcePhase || !data->script) {
    return nullptr;
  }
  return data->script->cell;
}

JSObject* JS::GetModuleNamespace(JSContext* cx, HandleObject moduleRecord) {
  ModuleData* data = DataOf(moduleRecord);
  if (data->sourcePhase) {
    return data->wasmSource;
  }
  AutoV8 scope(cx);
  v8::Local<v8::Module> module = data->module.Get(cx->isolate);
  if (module->GetStatus() == v8::Module::kUninstantiated) {
    ThrowNumberedError(cx, js::GetErrorMessage, nullptr,
                       JSMSG_BAD_MODULE_STATUS, "unlinked");
    return nullptr;
  }
  return ObjectFromV8(cx, module->GetModuleNamespace().As<v8::Object>());
}

JSString* JS::GetModuleRequestSpecifier(JSContext* cx,
                                        Handle<JSObject*> moduleRequestArg) {
  return JS::GetReservedSlot(moduleRequestArg, kSpecifier).toString();
}

JS::ModuleType JS::GetModuleRequestType(JSContext* cx,
                                        Handle<JSObject*> moduleRequestArg) {
  return JS::ModuleType(
      JS::GetReservedSlot(moduleRequestArg, kType).toInt32());
}

bool JS::ModuleRequestIsSourcePhase(JSContext* cx,
                                    Handle<JSObject*> moduleRequestArg) {
  return JS::GetReservedSlot(moduleRequestArg, kSourcePhase).toBoolean();
}

void JS::ClearModuleEnvironment(JSObject* moduleObj) {
  ModuleData* data = DataOf(moduleObj);
  if (!data || data->cleared) {
    return;
  }
  data->cleared = true;
  ForgetModule(data);
  if (!data->module.IsEmpty()) {
    JSContext* cx = CurrentContext();
    v8::HandleScope scope(cx->isolate);
    ReleaseModuleFromRecord(cx, ToV8(cx, moduleObj));
  }
  data->module.Reset();
  data->syntheticDefault = JS::UndefinedValue();
}

bool JS::ModuleIsLinked(JSObject* moduleObj) {
  ModuleData* data = DataOf(moduleObj);
  if (!data || data->sourcePhase) {
    return true;
  }
  if (data->module.IsEmpty()) {
    return data->cleared;
  }
  JSContext* cx = CurrentContext();
  v8::HandleScope scope(cx->isolate);
  v8::Module::Status status = data->module.Get(cx->isolate)->GetStatus();
  return status != v8::Module::kUninstantiated &&
         status != v8::Module::kInstantiating;
}
