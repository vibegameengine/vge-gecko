#include "VgeJsClassesState.h"

#include <algorithm>

#include "js/RealmIterators.h"
#include "js/WrapperCallbacks.h"
#include "jsfriendapi.h"

namespace vge::jsapi {

namespace {

const char kIntrinsicsSource[] =
    "(() => {"
    " const gp = Object.getPrototypeOf;"
    " const agf = gp(async function*(){});"
    " return [gp(gp([][Symbol.iterator]())), gp(agf.prototype),"
    " gp(new Map().entries()), gp(new Set().values()),"
    " gp(function*(){}).constructor, gp(async function(){}).constructor,"
    " agf.constructor, gp(Int8Array), eval, Function, Object.isExtensible,"
    " Reflect.set];"
    "})()";

v8::Local<v8::Object> AsObject(v8::Local<v8::Value> aValue) {
  if (aValue.IsEmpty() || !aValue->IsObject()) {
    return v8::Local<v8::Object>();
  }
  return aValue.As<v8::Object>();
}

void Store(v8::Isolate* aIsolate, v8::Global<v8::Object>& aSlot,
           v8::Local<v8::Value> aValue) {
  v8::Local<v8::Object> object = AsObject(aValue);
  if (!object.IsEmpty()) {
    aSlot.Reset(aIsolate, object);
  }
}

void StoreFunction(v8::Isolate* aIsolate, v8::Global<v8::Function>& aSlot,
                   v8::Local<v8::Value> aValue) {
  if (!aValue.IsEmpty() && aValue->IsFunction()) {
    aSlot.Reset(aIsolate, aValue.As<v8::Function>());
  }
}

v8::Local<v8::Value> RealGet(v8::Local<v8::Context> aContext,
                             v8::Local<v8::Object> aObject, const char* aName) {
  v8::Isolate* isolate = aContext->GetIsolate();
  v8::Local<v8::Value> value;
  if (aObject.IsEmpty() ||
      !aObject
           ->GetRealNamedProperty(
               aContext, v8::String::NewFromUtf8(isolate, aName).ToLocalChecked())
           .ToLocal(&value)) {
    return v8::Local<v8::Value>();
  }
  return value;
}

void DeleteGlobal(v8::Local<v8::Context> aContext, v8::Local<v8::Object> aGlobal,
                  const char* aName) {
  v8::Isolate* isolate = aContext->GetIsolate();
  aGlobal->Delete(aContext, v8::String::NewFromUtf8(isolate, aName).ToLocalChecked())
      .FromMaybe(false);
}

JSObject* CellOrNull(JSContext* aCx, const v8::Global<v8::Object>& aSlot) {
  if (aSlot.IsEmpty()) {
    return nullptr;
  }
  return ObjectFromV8(aCx, aSlot.Get(aCx->isolate));
}

JS::Compartment* CurrentCompartment(JSContext* aCx) {
  JS::Realm* realm = aCx->realm();
  return realm ? realm->compartment() : nullptr;
}

JS::Compartment* CompartmentOf(JSObject* aObj) {
  JS::Realm* realm = RealmOfCell(aObj);
  return realm ? realm->compartment() : nullptr;
}

class DeadObjectHandler : public js::NurseryAllocableProxyHandler {
 public:
  constexpr DeadObjectHandler() : js::NurseryAllocableProxyHandler(&family) {}

  bool getOwnPropertyDescriptor(
      JSContext* cx, JS::HandleObject wrapper, JS::HandleId id,
      JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc)
      const override {
    return ReportDead(cx);
  }
  bool defineProperty(JSContext* cx, JS::HandleObject wrapper, JS::HandleId id,
                      JS::Handle<JS::PropertyDescriptor> desc,
                      JS::ObjectOpResult& result) const override {
    return ReportDead(cx);
  }
  bool ownPropertyKeys(JSContext* cx, JS::HandleObject wrapper,
                       JS::MutableHandleIdVector props) const override {
    return ReportDead(cx);
  }
  bool delete_(JSContext* cx, JS::HandleObject wrapper, JS::HandleId id,
               JS::ObjectOpResult& result) const override {
    return ReportDead(cx);
  }
  bool getPrototype(JSContext* cx, JS::HandleObject proxy,
                    JS::MutableHandleObject protop) const override {
    protop.set(nullptr);
    return true;
  }
  bool getPrototypeIfOrdinary(JSContext* cx, JS::HandleObject proxy,
                              bool* isOrdinary,
                              JS::MutableHandleObject protop) const override {
    *isOrdinary = false;
    return true;
  }
  bool preventExtensions(JSContext* cx, JS::HandleObject proxy,
                         JS::ObjectOpResult& result) const override {
    return ReportDead(cx);
  }
  bool isExtensible(JSContext* cx, JS::HandleObject proxy,
                    bool* extensible) const override {
    *extensible = true;
    return true;
  }
  bool call(JSContext* cx, JS::HandleObject wrapper,
            const JS::CallArgs& args) const override {
    return ReportDead(cx);
  }
  bool construct(JSContext* cx, JS::HandleObject wrapper,
                 const JS::CallArgs& args) const override {
    return ReportDead(cx);
  }
  bool nativeCall(JSContext* cx, JS::IsAcceptableThis test,
                  JS::NativeImpl impl, const JS::CallArgs& args) const override {
    return ReportDead(cx);
  }
  bool getBuiltinClass(JSContext* cx, JS::HandleObject proxy,
                       js::ESClass* cls) const override {
    return ReportDead(cx);
  }
  bool isArray(JSContext* cx, JS::HandleObject proxy,
               JS::IsArrayAnswer* answer) const override {
    return ReportDead(cx);
  }
  const char* className(JSContext* cx, JS::HandleObject proxy) const override {
    return "DeadObject";
  }
  JSString* fun_toString(JSContext* cx, JS::HandleObject proxy,
                         bool isToSource) const override {
    ReportDead(cx);
    return nullptr;
  }
  js::RegExpShared* regexp_toShared(JSContext* cx,
                                    JS::HandleObject proxy) const override {
    ReportDead(cx);
    return nullptr;
  }
  bool useProxyExpandoObjectForPrivateFields() const override { return false; }
  bool isCallable(JSObject* obj) const override {
    return Flags(obj) & kCallable;
  }
  bool isConstructor(JSObject* obj) const override {
    return Flags(obj) & kConstructor;
  }
  bool finalizeInBackground(const JS::Value& priv) const override {
    return priv.toInt32() & kBackground;
  }
  bool mayBeSwapped() const override { return true; }

  static constexpr int32_t kCallable = 1 << 0;
  static constexpr int32_t kConstructor = 1 << 1;
  static constexpr int32_t kBackground = 1 << 2;
  static const char family;

 private:
  static bool ReportDead(JSContext* cx) {
    JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                              JSMSG_DEAD_OBJECT);
    return false;
  }
  static int32_t Flags(JSObject* obj) {
    return js::GetProxyPrivate(obj).toInt32();
  }
};

const char DeadObjectHandler::family = 0;
const DeadObjectHandler sDeadObjectHandler;

int32_t DeadFlagsFor(JSObject* aObj) {
  int32_t flags = 0;
  if (!aObj) {
    return DeadObjectHandler::kBackground;
  }
  if (JS::IsCallable(aObj)) {
    flags |= DeadObjectHandler::kCallable;
  }
  if (JS::IsConstructor(aObj)) {
    flags |= DeadObjectHandler::kConstructor;
  }
  const JSClass* clasp = JS::GetClass(aObj);
  if (!clasp->hasFinalize() || (clasp->flags & JSCLASS_BACKGROUND_FINALIZE)) {
    flags |= DeadObjectHandler::kBackground;
  }
  return flags;
}

void NukeProxyCell(JSObject* aProxy) {
  int32_t flags = DeadFlagsFor(aProxy);
  ObjectCell* cell = ObjectCell::from(aProxy);
  cell->proxyData()->handler = &sDeadObjectHandler;
  cell->proxyData()->expando = nullptr;
  js::SetProxyPrivate(aProxy, JS::Int32Value(flags));
  uint32_t reserved = JSCLASS_RESERVED_SLOTS(JS::GetClass(aProxy));
  for (uint32_t i = 0; i < reserved; i++) {
    js::SetProxyReservedSlot(aProxy, i, JS::UndefinedValue());
  }
}

void SwapProxyCells(JSObject* aA, JSObject* aB) {
  ObjectCell* a = ObjectCell::from(aA);
  ObjectCell* b = ObjectCell::from(aB);
  std::swap(a->proxyData()->handler, b->proxyData()->handler);
  std::swap(a->proxyData()->expando, b->proxyData()->expando);
  JS::Value privA = js::GetProxyPrivate(aA);
  JS::Value privB = js::GetProxyPrivate(aB);
  js::SetProxyPrivate(aA, privB);
  js::SetProxyPrivate(aB, privA);
  uint32_t reserved = std::min(JSCLASS_RESERVED_SLOTS(JS::GetClass(aA)),
                               JSCLASS_RESERVED_SLOTS(JS::GetClass(aB)));
  for (uint32_t i = 0; i < reserved; i++) {
    JS::Value va = js::GetProxyReservedSlot(aA, i);
    JS::Value vb = js::GetProxyReservedSlot(aB, i);
    js::SetProxyReservedSlot(aA, i, vb);
    js::SetProxyReservedSlot(aB, i, va);
  }
}

bool WrapObjectImpl(JSContext* aCx, JS::MutableHandleObject aObj) {
  if (!aObj) {
    return true;
  }
  JS::Compartment* comp = CurrentCompartment(aCx);
  if (!comp) {
    return true;
  }
  JS::RootedObject objectPassedToWrap(aCx, aObj);
  if (CompartmentOf(aObj) == comp) {
    aObj.set(js::ToWindowProxyIfWindow(aObj));
    return true;
  }
  aObj.set(js::UncheckedUnwrap(aObj, true));
  if (CompartmentOf(aObj) == comp) {
    return true;
  }
  if (js::IsWindow(aObj)) {
    aObj.set(js::ToWindowProxyIfWindow(aObj));
    aObj.set(js::UncheckedUnwrap(aObj));
    if (JS_IsDeadWrapper(aObj)) {
      aObj.set(JS_NewDeadWrapper(aCx, aObj));
      return !!aObj;
    }
  }
  if (JS_IsDeadWrapper(aObj)) {
    aObj.set(JS_NewDeadWrapper(aCx, aObj));
    return !!aObj;
  }
  const JSWrapObjectCallbacks* callbacks = aCx->runtime->wrapObjectCallbacks;
  if (callbacks && callbacks->preWrap) {
    JS::RootedObject global(aCx, aCx->realm()->global);
    JS::RootedObject obj(aCx, aObj);
    callbacks->preWrap(aCx, global, nullptr, obj, objectPassedToWrap, aObj);
    if (!aObj) {
      return false;
    }
  }
  if (CompartmentOf(aObj) == comp) {
    return true;
  }
  CompartmentExtra& extra = CompartmentData(comp);
  v8::HandleScope scope(aCx->isolate);
  auto found = extra.wrappers.find(aObj.get());
  if (found != extra.wrappers.end() && !found->second.IsEmpty()) {
    aObj.set(ObjectFromV8(aCx, found->second.Get(aCx->isolate)));
    return true;
  }
  if (!callbacks || !callbacks->wrap) {
    JS_ReportErrorASCII(aCx, "no wrap callback for cross-compartment object");
    return false;
  }
  JS::RootedObject target(aCx, aObj);
  JS::RootedObject wrapper(aCx, callbacks->wrap(aCx, nullptr, target));
  if (!wrapper) {
    return false;
  }
  v8::Global<v8::Object>& entry = extra.wrappers[target.get()];
  entry.Reset(aCx->isolate, ToV8(aCx, wrapper));
  entry.SetWeak();
  aObj.set(wrapper);
  return true;
}

bool WrapDescriptor(JSContext* aCx, JS::MutableHandle<JS::PropertyDescriptor> aDesc) {
  if (aDesc.hasGetter()) {
    JS::RootedObject getter(aCx, aDesc.getter());
    if (!WrapObjectImpl(aCx, &getter)) {
      return false;
    }
    aDesc.setGetter(getter);
  }
  if (aDesc.hasSetter()) {
    JS::RootedObject setter(aCx, aDesc.setter());
    if (!WrapObjectImpl(aCx, &setter)) {
      return false;
    }
    aDesc.setSetter(setter);
  }
  if (aDesc.hasValue()) {
    JS::RootedValue value(aCx, aDesc.value());
    if (!JS_WrapValue(aCx, &value)) {
      return false;
    }
    aDesc.setValue(value);
  }
  return true;
}

void IterateRealmsWhere(JSContext* aCx, void* aData,
                        JS::IterateRealmCallback aCallback,
                        JS::Compartment* aCompartment,
                        JSPrincipals* aPrincipals, bool aFilterPrincipals) {
  std::vector<JS::Realm*> realms = aCx->runtime->realms;
  JS::AutoCheckCannotGC nogc;
  for (JS::Realm* realm : realms) {
    if (aCompartment && realm->compartment() != aCompartment) {
      continue;
    }
    if (aFilterPrincipals && realm->principals != aPrincipals) {
      continue;
    }
    aCallback(aCx, aData, realm, nogc);
  }
}

}

bool IsDeselectedConstructor(JS::Realm* aRealm, JSProtoKey aKey) {
  const JS::RealmCreationOptions& options = aRealm->creationOptions;
  switch (aKey) {
    case JSProto_Atomics:
      return !options.getSharedMemoryAndAtomicsEnabled();
    case JSProto_SharedArrayBuffer:
      return !options.getSharedMemoryAndAtomicsEnabled() ||
             !options.defineSharedArrayBufferConstructor();
    default:
      return false;
  }
}

void CaptureRealmIntrinsics(JSContext* aCx, JS::Realm* aRealm,
                            v8::Local<v8::Context> aContext) {
  v8::Isolate* isolate = aCx->isolate;
  v8::HandleScope scope(isolate);
  v8::TryCatch tryCatch(isolate);
  RealmExtra& extra = RealmData(aRealm);
  v8::Local<v8::Object> global = extra.innerGlobal.Get(isolate);
  for (size_t k = 1; k < JSProto_LIMIT; k++) {
    const char* name = StandardClassName(JSProtoKey(k));
    if (!name) {
      continue;
    }
    v8::Local<v8::Object> ctor = AsObject(RealGet(aContext, global, name));
    if (ctor.IsEmpty()) {
      continue;
    }
    extra.ctors[k].Reset(isolate, ctor);
    if (ctor->IsFunction()) {
      Store(isolate, extra.protos[k], RealGet(aContext, ctor, "prototype"));
    }
  }
  v8::Local<v8::Script> script;
  v8::Local<v8::Value> result;
  if (v8::Script::Compile(aContext, v8::String::NewFromUtf8Literal(
                                        isolate, kIntrinsicsSource))
          .ToLocal(&script) &&
      script->Run(aContext).ToLocal(&result) && result->IsArray()) {
    v8::Local<v8::Array> values = result.As<v8::Array>();
    auto at = [&](uint32_t i) {
      v8::Local<v8::Value> v;
      if (!values->Get(aContext, i).ToLocal(&v)) {
        return v8::Undefined(isolate).As<v8::Value>();
      }
      return v;
    };
    Store(isolate, extra.iteratorProto, at(0));
    Store(isolate, extra.asyncIteratorProto, at(1));
    Store(isolate, extra.mapIteratorProto, at(2));
    Store(isolate, extra.setIteratorProto, at(3));
    const JSProtoKey fnKeys[] = {JSProto_GeneratorFunction,
                                 JSProto_AsyncFunction,
                                 JSProto_AsyncGeneratorFunction,
                                 JSProto_TypedArray};
    for (uint32_t i = 0; i < 4; i++) {
      v8::Local<v8::Object> ctor = AsObject(at(4 + i));
      if (ctor.IsEmpty()) {
        continue;
      }
      extra.ctors[fnKeys[i]].Reset(isolate, ctor);
      Store(isolate, extra.protos[fnKeys[i]], RealGet(aContext, ctor, "prototype"));
    }
    StoreFunction(isolate, extra.eval, at(8));
    StoreFunction(isolate, extra.functionCtor, at(9));
    StoreFunction(isolate, extra.isExtensible, at(10));
    StoreFunction(isolate, extra.reflectSet, at(11));
  }
  tryCatch.Reset();
  if (extra.protos[JSProto_Iterator].IsEmpty() && !extra.iteratorProto.IsEmpty()) {
    extra.protos[JSProto_Iterator].Reset(isolate, extra.iteratorProto.Get(isolate));
  }
  if (!extra.protos[JSProto_Object].IsEmpty()) {
    extra.objectProto =
        ObjectFromV8(aCx, extra.protos[JSProto_Object].Get(isolate));
  }
  if (!extra.protos[JSProto_Function].IsEmpty()) {
    extra.functionProto =
        ObjectFromV8(aCx, extra.protos[JSProto_Function].Get(isolate));
  }
  struct SelfHosted {
    const char* name;
    JSProtoKey key;
    const char* property;
  };
  const SelfHosted selfHosted[] = {
      {"$ArrayValues", JSProto_Array, "values"},
      {"ArrayValues", JSProto_Array, "values"},
      {"ArrayKeys", JSProto_Array, "keys"},
      {"ArrayEntries", JSProto_Array, "entries"},
      {"ArrayForEach", JSProto_Array, "forEach"},
      {"Object_valueOf", JSProto_Object, "valueOf"},
  };
  for (const SelfHosted& entry : selfHosted) {
    if (extra.protos[entry.key].IsEmpty()) {
      continue;
    }
    v8::Local<v8::Value> fn =
        RealGet(aContext, extra.protos[entry.key].Get(isolate), entry.property);
    StoreFunction(isolate, extra.selfHosted[entry.name], fn);
  }
  KeepRealmIntrinsics(aCx, aRealm, aContext);
}

void KeepRealmIntrinsics(JSContext* aCx, JS::Realm* aRealm,
                         v8::Local<v8::Context> aContext) {
  v8::Isolate* isolate = aCx->isolate;
  v8::HandleScope scope(isolate);
  RealmExtra& extra = RealmData(aRealm);
  std::vector<v8::Local<v8::Value>> kept;
  auto keep = [&](auto& aSlot) {
    if (!aSlot.IsEmpty()) {
      kept.push_back(aSlot.Get(isolate));
      aSlot.SetWeak();
    }
  };
  for (size_t k = 0; k < JSProto_LIMIT; k++) {
    keep(extra.ctors[k]);
    keep(extra.protos[k]);
  }
  keep(extra.eval);
  keep(extra.functionCtor);
  keep(extra.isExtensible);
  keep(extra.reflectSet);
  keep(extra.iteratorProto);
  keep(extra.asyncIteratorProto);
  keep(extra.mapIteratorProto);
  keep(extra.setIteratorProto);
  for (auto& [name, fn] : extra.selfHosted) {
    keep(fn);
  }
  v8::Local<v8::Object> inner = extra.innerGlobal.Get(isolate);
  inner
      ->SetPrivate(aContext, RealmPrivateKey(isolate, "vge.jsapi.realm.intrinsics"),
                   v8::Array::New(isolate, kept.data(), kept.size()))
      .Check();
  extra.innerGlobal.SetWeak();
}

v8::Local<v8::Private> RealmPrivateKey(v8::Isolate* aIsolate, const char* aName) {
  return v8::Private::ForApi(
      aIsolate, v8::String::NewFromUtf8(aIsolate, aName).ToLocalChecked());
}

void ForgetRealmClasses(JS::Realm* aRealm) {
  ClassesState& state = State(aRealm->runtime->mainContext);
  auto found = state.realms.find(aRealm);
  if (found == state.realms.end()) {
    return;
  }
  delete found->second;
  state.realms.erase(found);
}

void ForgetCompartmentClasses(JS::Compartment* aCompartment) {
  JSContext* cx = CurrentContext();
  if (!cx) {
    return;
  }
  ClassesState& state = State(cx);
  auto found = state.compartments.find(aCompartment);
  if (found == state.compartments.end()) {
    return;
  }
  delete found->second;
  state.compartments.erase(found);
}

JSProtoKey StandardKeyOf(JSContext* aCx, JSObject* aObj, bool* aIsPrototype) {
  *aIsPrototype = false;
  JS::Realm* realm = RealmOfCell(aObj);
  const JSClass* clasp = JS::GetClass(aObj);
  v8::Isolate* isolate = aCx->isolate;
  v8::HandleScope scope(isolate);
  v8::Local<v8::Object> object = ToV8(aCx, aObj);
  if (realm) {
    RealmExtra& extra = RealmData(realm);
    for (size_t k = 1; k < JSProto_LIMIT; k++) {
      if (!extra.protos[k].IsEmpty() && extra.protos[k].Get(isolate) == object) {
        *aIsPrototype = true;
        return JSProtoKey(k);
      }
    }
    const JSProtoKey namespaces[] = {JSProto_Math, JSProto_JSON, JSProto_Reflect,
                                     JSProto_Atomics, JSProto_WebAssembly
#ifdef JS_HAS_INTL_API
                                     ,
                                     JSProto_Intl
#endif
    };
    for (JSProtoKey key : namespaces) {
      if (!extra.ctors[key].IsEmpty() && extra.ctors[key].Get(isolate) == object) {
        return key;
      }
    }
  }
  if (clasp != js::ObjectClassPtr && clasp != js::FunctionClassPtr &&
      clasp != &ArrayClass && clasp != &FunctionExtendedClass) {
    if (js::IsProxy(aObj)) {
      return JSProto_Null;
    }
    return JSCLASS_CACHED_PROTO_KEY(clasp);
  }
  if (object->IsArray()) return JSProto_Array;
  if (object->IsFunction()) return JSProto_Function;
  if (object->IsDate()) return JSProto_Date;
  if (object->IsRegExp()) return JSProto_RegExp;
  if (object->IsBooleanObject()) return JSProto_Boolean;
  if (object->IsNumberObject()) return JSProto_Number;
  if (object->IsStringObject()) return JSProto_String;
  if (object->IsSymbolObject()) return JSProto_Symbol;
  if (object->IsBigIntObject()) return JSProto_BigInt;
  if (object->IsMap()) return JSProto_Map;
  if (object->IsSet()) return JSProto_Set;
  if (object->IsWeakMap()) return JSProto_WeakMap;
  if (object->IsWeakSet()) return JSProto_WeakSet;
  if (object->IsPromise()) return JSProto_Promise;
  if (object->IsArrayBuffer()) return JSProto_ArrayBuffer;
  if (object->IsSharedArrayBuffer()) return JSProto_SharedArrayBuffer;
  if (object->IsDataView()) return JSProto_DataView;
  if (object->IsInt8Array()) return JSProto_Int8Array;
  if (object->IsUint8Array()) return JSProto_Uint8Array;
  if (object->IsUint8ClampedArray()) return JSProto_Uint8ClampedArray;
  if (object->IsInt16Array()) return JSProto_Int16Array;
  if (object->IsUint16Array()) return JSProto_Uint16Array;
  if (object->IsInt32Array()) return JSProto_Int32Array;
  if (object->IsUint32Array()) return JSProto_Uint32Array;
  if (object->IsFloat32Array()) return JSProto_Float32Array;
  if (object->IsFloat64Array()) return JSProto_Float64Array;
  if (object->IsBigInt64Array()) return JSProto_BigInt64Array;
  if (object->IsBigUint64Array()) return JSProto_BigUint64Array;
  if (object->IsProxy()) return JSProto_Null;
  if (object->IsNativeError()) {
    if (realm) {
      RealmExtra& extra = RealmData(realm);
      v8::Local<v8::Value> proto = object->GetPrototype();
      const JSProtoKey errors[] = {
          JSProto_Error,          JSProto_InternalError, JSProto_AggregateError,
          JSProto_EvalError,      JSProto_RangeError,    JSProto_ReferenceError,
          JSProto_SuppressedError, JSProto_SyntaxError,  JSProto_TypeError,
          JSProto_URIError,       JSProto_CompileError,  JSProto_LinkError,
          JSProto_RuntimeError};
      for (JSProtoKey key : errors) {
        if (!extra.protos[key].IsEmpty() && extra.protos[key].Get(isolate) == proto) {
          return key;
        }
      }
    }
    return JSProto_Error;
  }
  return JSProto_Object;
}

JS::Realm* RealmOfV8Context(JSContext* aCx, v8::Local<v8::Context> aContext) {
  if (aContext.IsEmpty() ||
      aContext->GetNumberOfEmbedderDataFields() <= uint32_t(kRealmEmbedderIndex)) {
    return nullptr;
  }
  return static_cast<JS::Realm*>(
      aContext->GetAlignedPointerFromEmbedderData(kRealmEmbedderIndex));
}

}

using namespace vge::jsapi;

const char js::Wrapper::family = 0;
JSObject* const js::Wrapper::defaultProto = reinterpret_cast<JSObject*>(0x1);

JSObject* JS_NewGlobalObject(JSContext* aCx, const JSClass* aClass,
                             JSPrincipals* aPrincipals,
                             JS::OnNewGlobalHookOption aHookOption,
                             const JS::RealmOptions& aOptions) {
  JSRuntime* rt = aCx->runtime;
  v8::Isolate* isolate = aCx->isolate;
  const JS::RealmCreationOptions& creation = aOptions.creationOptions();
  JS::Compartment* comp;
  if (creation.compartmentSpecifier() ==
      JS::CompartmentSpecifier::ExistingCompartment) {
    comp = creation.compartment();
  } else {
    comp = new JS::Compartment(rt->zone);
  }
  JS::Realm* realm = new JS::Realm(comp, rt);
  realm->creationOptions = creation;
  realm->behaviors = aOptions.behaviors();
  bool isSystem = aPrincipals && aPrincipals == rt->trustedPrincipals;
  realm->isSystem = isSystem;
  if (comp->realms.empty()) {
    comp->systemCompartment = isSystem;
  }
  if (aPrincipals) {
    JS_HoldPrincipals(aPrincipals);
    realm->principals = aPrincipals;
  }

  ClassesState& state = State(aCx);
  v8::HandleScope scope(isolate);
  if (state.securityToken.IsEmpty()) {
    state.securityToken.Reset(
        isolate, v8::Symbol::New(isolate, v8::String::NewFromUtf8Literal(
                                              isolate, "vge.jsapi.security")));
  }
  v8::Local<v8::ObjectTemplate> globalTemplate =
      ClassTemplate(aCx, aClass)->InstanceTemplate();
  v8::Local<v8::Context> ctx =
      v8::Context::New(isolate, nullptr, globalTemplate);
  if (ctx.IsEmpty()) {
    delete realm;
    JS_ReportOutOfMemory(aCx);
    return nullptr;
  }
  comp->realms.push_back(realm);
  rt->realms.push_back(realm);
  ctx->SetSecurityToken(state.securityToken.Get(isolate));
  ctx->SetAlignedPointerInEmbedderData(kRealmEmbedderIndex, realm);
  realm->context.Reset(isolate, ctx);
  WatchRealmContext(realm);

  JS::Realm* old = aCx->realm();
  aCx->enterRealm(realm);
  v8::Local<v8::Object> proxy = ctx->Global();
  v8::Local<v8::Object> inner = proxy->GetPrototype().As<v8::Object>();
  ObjectCell* cell = NewObjectCell(aCx, ShapeFor(aClass, realm), proxy, true);
  proxy->SetAlignedPointerInInternalField(
      0, const_cast<WrapperTypeTag*>(&kWrapperTypeTag));
  inner->SetPrivate(ctx, rt->cellKey.Get(isolate), v8::External::New(isolate, cell))
      .Check();
  AnchorCellOn(aCx, cell, inner);
  realm->global = cell->asObject();
  realm->globalClass = aClass;
  RealmExtra& extra = RealmData(realm);
  extra.innerGlobal.Reset(isolate, inner);
  CaptureRealmIntrinsics(aCx, realm, ctx);
  DeleteGlobal(ctx, inner, "console");
  if (IsDeselectedConstructor(realm, JSProto_SharedArrayBuffer)) {
    DeleteGlobal(ctx, inner, "SharedArrayBuffer");
  }
  if (IsDeselectedConstructor(realm, JSProto_Atomics)) {
    DeleteGlobal(ctx, inner, "Atomics");
  }
  aCx->leaveRealm(old);

  JS::RootedObject global(aCx, realm->global);
  if (aHookOption == JS::FireOnNewGlobalHook) {
    JS_FireOnNewGlobalObject(aCx, global);
  }
  return global;
}

void JS_FireOnNewGlobalObject(JSContext* aCx, JS::HandleObject aGlobal) {
  JS::Realm* realm = RealmOfCell(aGlobal);
  if (realm) {
    RealmData(realm).firedOnNewGlobal = true;
  }
}

void JS_GlobalObjectTraceHook(JSTracer* aTrc, JSObject* aGlobal) {
  JS::Realm* realm = RealmOfCell(aGlobal);
  if (!realm || realm->global != aGlobal) {
    return;
  }
  if (JSTraceOp trace = realm->creationOptions.getTrace()) {
    trace(aTrc, aGlobal);
  }
}

const JSClassOps JS::DefaultGlobalClassOps = {
    nullptr,
    nullptr,
    nullptr,
    JS_NewEnumerateStandardClasses,
    JS_ResolveStandardClass,
    JS_MayResolveStandardClass,
    nullptr,
    nullptr,
    nullptr,
    JS_GlobalObjectTraceHook,
};

bool JS_IsGlobalObject(JSObject* aObj) { return IsGlobalCell(aObj); }

JSObject* JS::CurrentGlobalOrNull(JSContext* aCx) {
  JS::Realm* realm = aCx->realm();
  return realm ? realm->global : nullptr;
}

JSObject* JS::GetNonCCWObjectGlobal(JSObject* aObj) {
  JS::Realm* realm = RealmOfCell(aObj);
  return realm ? realm->global : nullptr;
}

JSObject* JS_GlobalLexicalEnvironment(JSObject* aObj) { return aObj; }

JS::Realm* JS::GetCurrentRealmOrNull(JSContext* aCx) { return aCx->realm(); }

JS::Realm* JS::GetObjectRealmOrNull(JSObject* aObj) {
  return js::IsCrossCompartmentWrapper(aObj) ? nullptr : RealmOfCell(aObj);
}

void* JS::GetRealmPrivate(JS::Realm* aRealm) { return aRealm->data; }

void JS::SetRealmPrivate(JS::Realm* aRealm, void* aData) {
  aRealm->data = aData;
}

bool JS::HasRealmInitializedGlobal(JS::Realm* aRealm) {
  return aRealm->global != nullptr;
}

JSObject* JS::GetRealmGlobalOrNull(JS::Realm* aRealm) { return aRealm->global; }

void JS::SetDestroyRealmCallback(JSContext* aCx,
                                 JS::DestroyRealmCallback aCallback) {
  aCx->runtime->destroyRealmCallback = aCallback;
}

void JS::SetRealmNameCallback(JSContext* aCx, JS::RealmNameCallback aCallback) {
  aCx->runtime->realmNameCallback = aCallback;
}

void js::gc::TraceRealmRoot(JSTracer* aTrc, JS::Realm* aRealm,
                            const char* aName) {
  if (aRealm->global) {
    JS::TraceRoot(aTrc, &aRealm->global, aName);
  }
}

static JSObject* RealmObject(JSContext* aCx, JSProtoKey aKey) {
  JS::Realm* realm = aCx->realm();
  if (!realm) {
    return nullptr;
  }
  return CellOrNull(aCx, RealmData(realm).protos[aKey]);
}

JSObject* JS::GetRealmObjectPrototype(JSContext* aCx) {
  return RealmData(aCx->realm()).objectProto;
}

JS::Handle<JSObject*> JS::GetRealmObjectPrototypeHandle(JSContext* aCx) {
  return JS::Handle<JSObject*>::fromMarkedLocation(
      &RealmData(aCx->realm()).objectProto);
}

JSObject* JS::GetRealmFunctionPrototype(JSContext* aCx) {
  return RealmData(aCx->realm()).functionProto;
}

JS::Handle<JSObject*> JS::GetRealmFunctionPrototypeHandle(JSContext* aCx) {
  return JS::Handle<JSObject*>::fromMarkedLocation(
      &RealmData(aCx->realm()).functionProto);
}

JSObject* JS::GetRealmErrorPrototype(JSContext* aCx) {
  return RealmObject(aCx, JSProto_Error);
}

JSObject* JS::GetRealmIteratorPrototype(JSContext* aCx) {
  return CellOrNull(aCx, RealmData(aCx->realm()).iteratorProto);
}

JSObject* JS::GetRealmAsyncIteratorPrototype(JSContext* aCx) {
  return CellOrNull(aCx, RealmData(aCx->realm()).asyncIteratorProto);
}

JSObject* JS::GetRealmMapIteratorPrototype(JSContext* aCx) {
  return CellOrNull(aCx, RealmData(aCx->realm()).mapIteratorProto);
}

JSObject* JS::GetRealmSetIteratorPrototype(JSContext* aCx) {
  return CellOrNull(aCx, RealmData(aCx->realm()).setIteratorProto);
}

JSObject* JS::GetRealmKeyObject(JSContext* aCx) {
  RealmExtra& extra = RealmData(aCx->realm());
  AutoV8 v8(aCx);
  if (extra.keyObject.IsEmpty()) {
    v8::Local<v8::Object> key = v8::Object::New(aCx->isolate);
    extra.innerGlobal.Get(aCx->isolate)
        ->SetPrivate(v8.context(),
                     RealmPrivateKey(aCx->isolate, "vge.jsapi.realm.keyObject"),
                     key)
        .Check();
    extra.keyObject.Reset(aCx->isolate, key);
    extra.keyObject.SetWeak();
  }
  return ObjectFromV8(aCx, extra.keyObject.Get(aCx->isolate));
}

JS::Realm* JS::GetFunctionRealm(JSContext* aCx, JS::HandleObject aObjArg) {
  JS::RootedObject obj(aCx, aObjArg);
  while (true) {
    obj = js::CheckedUnwrapStatic(obj);
    if (!obj) {
      JS_ReportErrorNumberASCII(aCx, js::GetErrorMessage, nullptr,
                                JSMSG_OBJECT_ACCESS_DENIED);
      return nullptr;
    }
    if (js::IsScriptedProxy(obj)) {
      JSObject* target = js::GetProxyTargetObject(obj);
      if (!target) {
        JS_ReportErrorNumberASCII(aCx, js::GetErrorMessage, nullptr,
                                  JSMSG_PROXY_REVOKED);
        return nullptr;
      }
      obj = target;
      continue;
    }
    if (js::IsProxy(obj)) {
      return aCx->realm();
    }
    v8::HandleScope scope(aCx->isolate);
    v8::Local<v8::Object> object = ToV8(aCx, obj);
    if (object->IsFunction()) {
      v8::Local<v8::Value> bound = object.As<v8::Function>()->GetBoundFunction();
      if (bound->IsFunction()) {
        obj = ObjectFromV8(aCx, bound.As<v8::Object>());
        continue;
      }
      return RealmOfCell(obj);
    }
    return aCx->realm();
  }
}

JS::RealmCreationOptions&
JS::RealmCreationOptions::setNewCompartmentInSystemZone() {
  compSpec_ = CompartmentSpecifier::NewCompartmentInSystemZone;
  comp_ = nullptr;
  return *this;
}

JS::RealmCreationOptions&
JS::RealmCreationOptions::setNewCompartmentInExistingZone(JSObject* aObj) {
  compSpec_ = CompartmentSpecifier::NewCompartmentInExistingZone;
  zone_ = js::GetObjectZoneFromAnyThread(aObj);
  return *this;
}

JS::RealmCreationOptions& JS::RealmCreationOptions::setExistingCompartment(
    JSObject* aObj) {
  compSpec_ = CompartmentSpecifier::ExistingCompartment;
  comp_ = CompartmentOf(aObj);
  return *this;
}

JS::RealmCreationOptions& JS::RealmCreationOptions::setExistingCompartment(
    JS::Compartment* aCompartment) {
  compSpec_ = CompartmentSpecifier::ExistingCompartment;
  comp_ = aCompartment;
  return *this;
}

JS::RealmCreationOptions& JS::RealmCreationOptions::setNewCompartmentAndZone() {
  compSpec_ = CompartmentSpecifier::NewCompartmentAndZone;
  comp_ = nullptr;
  return *this;
}

bool JS::RealmCreationOptions::getSharedMemoryAndAtomicsEnabled() const {
  return sharedMemoryAndAtomics_;
}

JS::RealmCreationOptions&
JS::RealmCreationOptions::setSharedMemoryAndAtomicsEnabled(bool aFlag) {
  sharedMemoryAndAtomics_ = aFlag;
  return *this;
}

JS::RealmCreationOptions& JS::RealmCreationOptions::setCoopAndCoepEnabled(
    bool aFlag) {
  coopAndCoep_ = aFlag;
  return *this;
}

void JS::SetRealmLocaleOverride(JS::Realm* aRealm, const char* aLocale) {
  aRealm->behaviors.setLocaleOverride(aLocale);
}

void JS::SetRealmTimezoneOverride(JS::Realm* aRealm, const char* aTimezone) {
  aRealm->behaviors.setTimeZoneOverride(aTimezone);
}

void JS::SetRealmNonLive(JS::Realm* aRealm) { aRealm->behaviors.setNonLive(); }

bool JS::GetIsSecureContext(JS::Realm* aRealm) {
  return aRealm->creationOptions.secureContext();
}

JSPrincipals* JS::GetRealmPrincipals(JS::Realm* aRealm) {
  return aRealm->principals;
}

bool JS::GetDebuggerObservesWasm(JS::Realm* aRealm) { return false; }

void JS::SetRealmPrincipals(JS::Realm* aRealm, JSPrincipals* aPrincipals) {
  if (aPrincipals == aRealm->principals) {
    return;
  }
  bool isSystem =
      aPrincipals && aPrincipals == aRealm->runtime->trustedPrincipals;
  MOZ_RELEASE_ASSERT(aRealm->isSystem == isSystem);
  if (aRealm->principals) {
    JS_DropPrincipals(CurrentContext(), aRealm->principals);
    aRealm->principals = nullptr;
  }
  if (aPrincipals) {
    JS_HoldPrincipals(aPrincipals);
    aRealm->principals = aPrincipals;
  }
}

void JS_HoldPrincipals(JSPrincipals* aPrincipals) { ++aPrincipals->refcount; }

void JS_DropPrincipals(JSContext* aCx, JSPrincipals* aPrincipals) {
  int rc = --aPrincipals->refcount;
  if (rc == 0) {
    ClassesState& state = State(aCx);
    if (state.destroyPrincipals) {
      state.destroyPrincipals(aPrincipals);
    }
  }
}

void JS_SetSecurityCallbacks(JSContext* aCx, const JSSecurityCallbacks* aScb) {
  aCx->runtime->securityCallbacks = aScb;
}

const JSSecurityCallbacks* JS_GetSecurityCallbacks(JSContext* aCx) {
  return aCx->runtime->securityCallbacks;
}

void JS_SetTrustedPrincipals(JSContext* aCx, JSPrincipals* aPrin) {
  aCx->runtime->trustedPrincipals = aPrin;
}

void JS_InitDestroyPrincipalsCallback(JSContext* aCx,
                                      JSDestroyPrincipalsOp aDestroy) {
  State(aCx).destroyPrincipals = aDestroy;
}

void JS_InitReadPrincipalsCallback(JSContext* aCx, JSReadPrincipalsOp aRead) {
  State(aCx).readPrincipals = aRead;
}

void JS::IterateRealms(JSContext* aCx, void* aData,
                       JS::IterateRealmCallback aCallback) {
  IterateRealmsWhere(aCx, aData, aCallback, nullptr, nullptr, false);
}

void JS::IterateRealmsWithPrincipals(JSContext* aCx, JSPrincipals* aPrincipals,
                                     void* aData,
                                     JS::IterateRealmCallback aCallback) {
  IterateRealmsWhere(aCx, aData, aCallback, nullptr, aPrincipals, true);
}

void JS::IterateRealmsInCompartment(JSContext* aCx,
                                    JS::Compartment* aCompartment, void* aData,
                                    JS::IterateRealmCallback aCallback) {
  IterateRealmsWhere(aCx, aData, aCallback, aCompartment, nullptr, false);
}

void JS_IterateCompartmentsInZone(JSContext* aCx, JS::Zone* aZone, void* aData,
                                  JSIterateCompartmentCallback aCallback) {
  std::vector<JS::Compartment*> seen;
  std::vector<JS::Realm*> realms = aCx->runtime->realms;
  for (JS::Realm* realm : realms) {
    JS::Compartment* comp = realm->compartment();
    if (comp->zone != aZone ||
        std::find(seen.begin(), seen.end(), comp) != seen.end()) {
      continue;
    }
    seen.push_back(comp);
    if (aCallback(aCx, aData, comp) == JS::CompartmentIterResult::Stop) {
      return;
    }
  }
}

JS::Realm* js::GetAnyRealmInZone(JS::Zone* aZone) {
  JSContext* cx = CurrentContext();
  for (JS::Realm* realm : cx->runtime->realms) {
    if (realm->compartment()->zone == aZone) {
      return realm;
    }
  }
  return nullptr;
}

JS::Zone* js::GetRealmZone(JS::Realm* aRealm) {
  return aRealm->compartment()->zone;
}

JS::Zone* js::GetObjectZoneFromAnyThread(const JSObject* aObj) {
  JS::Realm* realm = RealmOfCell(const_cast<JSObject*>(aObj));
  if (realm) {
    return realm->compartment()->zone;
  }
  return CurrentContext()->runtime->zone;
}

bool js::IsSystemCompartment(JS::Compartment* aComp) {
  if (!aComp->realms.empty()) {
    return aComp->realms[0]->isSystem;
  }
  return aComp->systemCompartment;
}

bool js::IsSystemRealm(JS::Realm* aRealm) { return aRealm->isSystem; }

bool js::IsSystemZone(JS::Zone* aZone) { return aZone->isSystemZone(); }

bool js::IsCompartmentZoneSweepingOrCompacting(JS::Compartment* aComp) {
  JSContext* cx = CurrentContext();
  return cx && (cx->runtime->inGC || cx->runtime->sweepingWeakPointers);
}

JSObject* js::GetFirstGlobalInCompartment(JS::Compartment* aComp) {
  for (JS::Realm* realm : aComp->realms) {
    if (realm->global) {
      return realm->global;
    }
  }
  return nullptr;
}

bool js::CompartmentHasLiveGlobal(JS::Compartment* aComp) {
  return js::GetFirstGlobalInCompartment(aComp) != nullptr;
}

bool js::IsSharableCompartment(JS::Compartment* aComp) {
  return js::CompartmentHasLiveGlobal(aComp);
}

bool js::IsObjectInContextCompartment(JSObject* aObj, const JSContext* aCx) {
  JS::Realm* realm = const_cast<JSContext*>(aCx)->realm();
  return realm && CompartmentOf(aObj) == realm->compartment();
}

void js::AssertSameCompartment(JSContext* aCx, JSObject* aObj) {
  MOZ_ASSERT(!aObj || !aCx->realm() ||
             CompartmentOf(aObj) == aCx->realm()->compartment());
}

void js::AssertSameCompartment(JSContext* aCx, JS::HandleValue aValue) {
  MOZ_ASSERT(!aValue.isObject() || !aCx->realm() ||
             CompartmentOf(&aValue.toObject()) == aCx->realm()->compartment());
}

void js::NotifyAnimationActivity(JSObject* aObj) {}

void JS_SetWrapObjectCallbacks(JSContext* aCx,
                               const JSWrapObjectCallbacks* aCallbacks) {
  aCx->runtime->wrapObjectCallbacks = aCallbacks;
}

bool JS_WrapObject(JSContext* aCx, JS::MutableHandleObject aObjp) {
  return WrapObjectImpl(aCx, aObjp);
}

bool JS_WrapValue(JSContext* aCx, JS::MutableHandleValue aVp) {
  if (!aVp.isObject()) {
    return true;
  }
  JS::RootedObject obj(aCx, &aVp.toObject());
  if (!WrapObjectImpl(aCx, &obj)) {
    return false;
  }
  aVp.setObject(*obj);
  return true;
}

bool JS_WrapId(JSContext* aCx, JS::MutableHandleId aId) { return true; }

bool JS_WrapPropertyDescriptor(
    JSContext* aCx, JS::MutableHandle<JS::PropertyDescriptor> aDesc) {
  return WrapDescriptor(aCx, aDesc);
}

bool JS_WrapPropertyDescriptor(
    JSContext* aCx,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> aDesc) {
  if (aDesc.isNothing()) {
    return true;
  }
  JS::Rooted<JS::PropertyDescriptor> desc(aCx, *aDesc);
  if (!WrapDescriptor(aCx, &desc)) {
    return false;
  }
  aDesc.set(mozilla::Some(desc.get()));
  return true;
}

void js::RemapRemoteWindowProxies(JSContext* aCx,
                                  js::CompartmentTransplantCallback* aCallback,
                                  JS::MutableHandleObject aTarget) {
  MOZ_RELEASE_ASSERT(js::IsProxy(aTarget));
  std::vector<JS::Compartment*> comps;
  for (JS::Realm* realm : aCx->runtime->realms) {
    if (std::find(comps.begin(), comps.end(), realm->compartment()) ==
        comps.end()) {
      comps.push_back(realm->compartment());
    }
  }
  JS::RootedObject targetCompartmentProxy(aCx);
  JS::RootedVector<JSObject*> otherProxies(aCx);
  for (JS::Compartment* comp : comps) {
    JSObject* remote = aCallback->getObjectToTransplant(comp);
    if (!remote) {
      continue;
    }
    MOZ_RELEASE_ASSERT(js::IsProxy(remote));
    NukeProxyCell(remote);
    if (comp == CompartmentOf(aTarget)) {
      targetCompartmentProxy = remote;
    } else if (!otherProxies.append(remote)) {
      MOZ_CRASH("js::RemapRemoteWindowProxies");
    }
  }
  if (targetCompartmentProxy) {
    SwapProxyCells(targetCompartmentProxy, aTarget);
    aTarget.set(targetCompartmentProxy);
  }
  for (size_t i = 0; i < otherProxies.length(); i++) {
    JS::RootedObject dead(aCx, otherProxies[i]);
    JSAutoRealm ar(aCx, dead);
    JS::RootedObject wrapper(aCx, aTarget);
    if (!JS_WrapObject(aCx, &wrapper)) {
      MOZ_CRASH("js::RemapRemoteWindowProxies");
    }
    SwapProxyCells(dead, wrapper);
    v8::HandleScope scope(aCx->isolate);
    v8::Global<v8::Object>& entry =
        CompartmentData(CompartmentOf(dead)).wrappers[aTarget.get()];
    entry.Reset(aCx->isolate, ToV8(aCx, dead));
    entry.SetWeak();
  }
}

JSObject* js::Wrapper::New(JSContext* aCx, JSObject* aObj,
                           const js::Wrapper* aHandler,
                           const js::WrapperOptions& aOptions) {
  mozilla::Maybe<JSAutoRealm> ar;
  if (aHandler->isCrossCompartmentWrapper()) {
    JS::Compartment* comp = CurrentCompartment(aCx);
    JSObject* first = comp ? js::GetFirstGlobalInCompartment(comp) : nullptr;
    if (first) {
      ar.emplace(aCx, first);
    }
  }
  JS::RootedValue priv(aCx, JS::ObjectValue(*aObj));
  return js::NewProxyObject(aCx, aHandler, priv, aOptions.proto(), aOptions);
}

JSObject* js::Wrapper::Renew(JSObject* aExisting, JSObject* aObj,
                             const js::Wrapper* aHandler) {
  ObjectCell* cell = ObjectCell::from(aExisting);
  cell->proxyData()->handler = aHandler;
  js::SetProxyPrivate(aExisting, JS::ObjectValue(*aObj));
  uint32_t reserved = JSCLASS_RESERVED_SLOTS(JS::GetClass(aExisting));
  for (uint32_t i = 0; i < reserved; i++) {
    js::SetProxyReservedSlot(aExisting, i, JS::UndefinedValue());
  }
  return aExisting;
}

JSObject* js::Wrapper::wrappedObject(JSObject* aWrapper) {
  JSObject* target = js::GetProxyTargetObject(aWrapper);
  if (target) {
    JS::ExposeObjectToActiveJS(target);
  }
  return target;
}

bool js::Wrapper::finalizeInBackground(const JS::Value& aPriv) const {
  if (!aPriv.isObject()) {
    return true;
  }
  const JSClass* clasp = JS::GetClass(&aPriv.toObject());
  return !clasp->hasFinalize() || (clasp->flags & JSCLASS_BACKGROUND_FINALIZE);
}

bool JS_IsDeadWrapper(JSObject* aObj) {
  return js::IsProxy(aObj) &&
         js::GetProxyHandler(aObj)->family() == &DeadObjectHandler::family;
}

namespace vge::jsapi::proxy {

void NukeProxy(JSContext* aCx, JS::HandleObject aProxy) {
  NukeProxyCell(aProxy);
}

}

JSObject* JS_NewDeadWrapper(JSContext* aCx, JSObject* aOrigObj) {
  JS::RootedValue priv(aCx, JS::Int32Value(DeadFlagsFor(aOrigObj)));
  return js::NewProxyObject(aCx, &sDeadObjectHandler, priv, nullptr);
}

JSObject* JS_FindCompilationScope(JSContext* aCx, JS::HandleObject aObjArg) {
  JS::RootedObject obj(aCx, aObjArg);
  if (js::IsWrapper(obj)) {
    obj = js::UncheckedUnwrap(obj);
  }
  return js::ToWindowIfWindowProxy(obj);
}

void js::SetScriptEnvironmentPreparer(JSContext* aCx,
                                      js::ScriptEnvironmentPreparer* aPreparer) {
  State(aCx).environmentPreparer = aPreparer;
}

void js::SetDOMCallbacks(JSContext* aCx, const js::DOMCallbacks* aCallbacks) {
  State(aCx).domCallbacks = aCallbacks;
}

void js::SetPreserveWrapperCallbacks(
    JSContext* aCx, js::PreserveWrapperCallback aPreserveWrapper,
    js::HasReleasedWrapperCallback aHasReleasedWrapper) {
  ClassesState& state = State(aCx);
  state.preserveWrapper = aPreserveWrapper;
  state.hasReleasedWrapper = aHasReleasedWrapper;
}

void js::CommitPendingWrapperPreservations(JSContext* aCx) {
  ClassesState& state = State(aCx);
  std::vector<JSObject*> pending;
  pending.swap(state.pendingPreservations);
  if (!state.preserveWrapper) {
    return;
  }
  for (JSObject* obj : pending) {
    JS::RootedObject rooted(aCx, obj);
    state.preserveWrapper(aCx, rooted);
  }
}

js::AutoAssertNoContentJS::AutoAssertNoContentJS(JSContext* aCx)
    : context_(aCx), prevAllowContentJS_(State(aCx).allowContentJS) {
  State(aCx).allowContentJS = false;
}

js::AutoAssertNoContentJS::~AutoAssertNoContentJS() {
  State(context_).allowContentJS = prevAllowContentJS_;
}
