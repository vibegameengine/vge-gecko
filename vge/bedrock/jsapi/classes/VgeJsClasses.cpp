#include "VgeJsClassesState.h"

#include <iterator>
#include <mutex>

#include "js/Prefs.h"
#include "js/ScalarType.h"
#include "js/String.h"
#include "js/TracingAPI.h"
#include "js/experimental/TypedData.h"

namespace vge::jsapi::buffers {
extern const JSClass ArrayBufferClasses[5];
extern const JSClass TypedArrayFixedClasses[JS::Scalar::MaxTypedArrayViewType];
}

namespace vge::jsapi {

extern const JSClass SavedFrameClass;

const WrapperTypeTag kWrapperTypeTag = {kWrapperEmbedderId};

const JSClass PlainObjectClass = {"Object",
                                  JSCLASS_HAS_CACHED_PROTO(JSProto_Object)};
const JSClass FunctionClass = {
    "Function",
    JSCLASS_HAS_RESERVED_SLOTS(4) | JSCLASS_HAS_CACHED_PROTO(JSProto_Function)};
const JSClass FunctionExtendedClass = {
    "Function",
    JSCLASS_HAS_RESERVED_SLOTS(7) | JSCLASS_HAS_CACHED_PROTO(JSProto_Function)};
const JSClass DataViewClasses[3] = {
    {"DataView", JSCLASS_HAS_CACHED_PROTO(JSProto_DataView)},
    {"DataView", JSCLASS_HAS_CACHED_PROTO(JSProto_DataView)},
    {"DataView", JSCLASS_HAS_CACHED_PROTO(JSProto_DataView)},
};

namespace {

#define VGE_STD_CLASS(name, clasp) \
  {#name, JSCLASS_HAS_CACHED_PROTO(JSProto_##name)},
#define VGE_STD_NOCLASS(name, clasp) {nullptr, 0},
const JSClass kStandardClasses[JSProto_LIMIT] = {
    JS_FOR_PROTOTYPES(VGE_STD_CLASS, VGE_STD_NOCLASS)};
#undef VGE_STD_CLASS
#undef VGE_STD_NOCLASS

#define VGE_STD_NAME(name, clasp) #name,
#define VGE_STD_NONAME(name, clasp) nullptr,
const char* const kStandardNames[JSProto_LIMIT] = {
    JS_FOR_PROTOTYPES(VGE_STD_NAME, VGE_STD_NONAME)};
#undef VGE_STD_NAME
#undef VGE_STD_NONAME

std::mutex gStatesLock;
std::unordered_map<JSRuntime*, ClassesState*>& AllStates() {
  static std::unordered_map<JSRuntime*, ClassesState*> states;
  return states;
}
thread_local ClassesState* tCachedState = nullptr;

ClassesState& StateFor(JSRuntime* aRuntime) {
  if (tCachedState && tCachedState->runtime == aRuntime) {
    return *tCachedState;
  }
  std::lock_guard<std::mutex> lock(gStatesLock);
  ClassesState*& state = AllStates()[aRuntime];
  if (!state) {
    state = new ClassesState();
    state->runtime = aRuntime;
  }
  tCachedState = state;
  return *state;
}

const JSAtomState& DummyAtomState() {
  static const uintptr_t sStorage[1] = {0};
  return *reinterpret_cast<const JSAtomState*>(sStorage);
}

class AutoCellRealm {
 public:
  AutoCellRealm(JSContext* aCx, JS::Realm* aRealm)
      : mCx(aCx), mOld(aCx->realm()), mEntered(aRealm && aRealm != mOld) {
    if (mEntered) {
      mCx->enterRealm(aRealm);
    }
  }
  ~AutoCellRealm() {
    if (mEntered) {
      mCx->leaveRealm(mOld);
    }
  }

 private:
  JSContext* mCx;
  JS::Realm* mOld;
  bool mEntered;
};

bool HasPropertyHooks(const JSClass* aClass) {
  return aClass->getResolve() || aClass->getAddProperty() ||
         aClass->getDelProperty() || aClass->getEnumerate() ||
         aClass->getNewEnumerate();
}

const JSClass* ClassOfData(v8::Local<v8::Value> aData) {
  return static_cast<const JSClass*>(aData.As<v8::External>()->Value());
}

bool ResolveOwn(JSContext* aCx, JS::HandleObject aObj, const JSClass* aClass,
                JS::HandleId aId) {
  JSResolveOp resolve = aClass->getResolve();
  if (!resolve) {
    return true;
  }
  if (JSMayResolveOp mayResolve = aClass->getMayResolve()) {
    if (!mayResolve(DummyAtomState(), aId, aObj)) {
      return true;
    }
  }
  std::vector<std::pair<JSObject*, jsid>>& resolving = State(aCx).resolving;
  for (const auto& entry : resolving) {
    if (entry.first == aObj.get() && entry.second == aId.get()) {
      return true;
    }
  }
  resolving.emplace_back(aObj.get(), aId.get());
  bool resolved = false;
  bool ok = resolve(aCx, aObj, aId, &resolved);
  resolving.pop_back();
  return ok;
}

bool HasOwnReal(v8::Local<v8::Context> aContext, v8::Local<v8::Object> aHolder,
                v8::Local<v8::Value> aKey) {
  if (aKey->IsName()) {
    return aHolder->HasRealNamedProperty(aContext, aKey.As<v8::Name>())
        .FromMaybe(false);
  }
  return aHolder
      ->HasRealIndexedProperty(aContext, aKey.As<v8::Uint32>()->Value())
      .FromMaybe(false);
}

bool ProtoChainHasReal(v8::Local<v8::Context> aContext,
                       v8::Local<v8::Object> aHolder,
                       v8::Local<v8::Value> aKey) {
  v8::Local<v8::Value> proto = aHolder->GetPrototype();
  while (proto->IsObject()) {
    v8::Local<v8::Object> p = proto.As<v8::Object>();
    if (p->IsProxy()) {
      return true;
    }
    if (HasOwnReal(aContext, p, aKey)) {
      return true;
    }
    proto = p->GetPrototype();
  }
  return false;
}

struct HookCall {
  JSContext* cx;
  JS::Rooted<JSObject*> obj;
  JS::Rooted<jsid> id;
  const JSClass* clasp;
  mozilla::Maybe<AutoCellRealm> realm;

  HookCall(JSContext* aCx, v8::Local<v8::Object> aHolder,
           v8::Local<v8::Value> aKey, v8::Local<v8::Value> aData)
      : cx(aCx),
        obj(aCx, ObjectFromV8(aCx, aHolder)),
        id(aCx),
        clasp(ClassOfData(aData)) {
    realm.emplace(aCx, RealmOfCell(obj));
    if (!aKey.IsEmpty()) {
      IdFromV8(aCx, aKey, &id);
    }
  }
};

v8::Local<v8::Value> IndexKey(v8::Isolate* aIsolate, uint32_t aIndex) {
  return v8::Integer::NewFromUnsigned(aIsolate, aIndex);
}

v8::Local<v8::Name> IndexName(v8::Isolate* aIsolate, uint32_t aIndex) {
  char buf[16];
  int n = snprintf(buf, sizeof(buf), "%u", aIndex);
  return v8::String::NewFromUtf8(aIsolate, buf, v8::NewStringType::kNormal, n)
      .ToLocalChecked();
}

void LookupHook(v8::Local<v8::Object> aHolder, v8::Local<v8::Value> aKey,
                v8::Local<v8::Value> aData) {
  JSContext* cx = CurrentContext();
  v8::Local<v8::Context> ctx = cx->isolate->GetCurrentContext();
  if (HasOwnReal(ctx, aHolder, aKey)) {
    return;
  }
  HookCall call(cx, aHolder, aKey, aData);
  if (!ResolveOwn(cx, call.obj, call.clasp, call.id)) {
    ThrowPendingToV8(cx);
  }
}

void QueryHook(v8::Local<v8::Object> aHolder, v8::Local<v8::Value> aKey,
               v8::Local<v8::Name> aName, v8::Local<v8::Value> aData,
               const v8::PropertyCallbackInfo<v8::Integer>& aInfo) {
  JSContext* cx = CurrentContext();
  v8::Local<v8::Context> ctx = cx->isolate->GetCurrentContext();
  if (HasOwnReal(ctx, aHolder, aKey)) {
    return;
  }
  {
    HookCall call(cx, aHolder, aKey, aData);
    if (!ResolveOwn(cx, call.obj, call.clasp, call.id)) {
      ThrowPendingToV8(cx);
      return;
    }
  }
  if (!HasOwnReal(ctx, aHolder, aKey)) {
    return;
  }
  v8::Maybe<v8::PropertyAttribute> attrs =
      aHolder->GetRealNamedPropertyAttributes(ctx, aName);
  if (attrs.IsJust()) {
    aInfo.GetReturnValue().Set(int32_t(attrs.FromJust()));
  }
}

void AddHook(v8::Local<v8::Object> aHolder, v8::Local<v8::Value> aKey,
             v8::Local<v8::Value> aValue, v8::Local<v8::Value> aData,
             bool aCheckProtoChain) {
  JSContext* cx = CurrentContext();
  v8::Local<v8::Context> ctx = cx->isolate->GetCurrentContext();
  if (HasOwnReal(ctx, aHolder, aKey)) {
    return;
  }
  HookCall call(cx, aHolder, aKey, aData);
  if (!ResolveOwn(cx, call.obj, call.clasp, call.id)) {
    ThrowPendingToV8(cx);
    return;
  }
  JSAddPropertyOp add = call.clasp->getAddProperty();
  if (!add || HasOwnReal(ctx, aHolder, aKey)) {
    return;
  }
  if (aCheckProtoChain && ProtoChainHasReal(ctx, aHolder, aKey)) {
    return;
  }
  JS::Rooted<JS::Value> v(cx, ValueFromV8(cx, aValue));
  if (!add(cx, call.obj, call.id, v)) {
    ThrowPendingToV8(cx);
  }
}

void DeleteHook(v8::Local<v8::Object> aHolder, v8::Local<v8::Value> aKey,
                v8::Local<v8::Name> aName, v8::Local<v8::Value> aData,
                const v8::PropertyCallbackInfo<v8::Boolean>& aInfo) {
  JSContext* cx = CurrentContext();
  v8::Local<v8::Context> ctx = cx->isolate->GetCurrentContext();
  HookCall call(cx, aHolder, aKey, aData);
  if (!HasOwnReal(ctx, aHolder, aKey)) {
    if (!ResolveOwn(cx, call.obj, call.clasp, call.id)) {
      ThrowPendingToV8(cx);
      return;
    }
    if (!HasOwnReal(ctx, aHolder, aKey)) {
      return;
    }
  }
  JSDeletePropertyOp del = call.clasp->getDelProperty();
  if (!del) {
    return;
  }
  v8::Maybe<v8::PropertyAttribute> attrs =
      aHolder->GetRealNamedPropertyAttributes(ctx, aName);
  if (attrs.IsJust() && (attrs.FromJust() & v8::DontDelete)) {
    return;
  }
  JS::ObjectOpResult result;
  if (!del(cx, call.obj, call.id, result)) {
    ThrowPendingToV8(cx);
    return;
  }
  if (!result.ok()) {
    aInfo.GetReturnValue().Set(false);
  }
}

void EnumerateHook(v8::Local<v8::Object> aHolder, v8::Local<v8::Value> aData,
                   bool aIndices,
                   const v8::PropertyCallbackInfo<v8::Array>& aInfo) {
  JSContext* cx = CurrentContext();
  v8::Isolate* isolate = cx->isolate;
  HookCall call(cx, aHolder, v8::Local<v8::Value>(), aData);
  if (JSEnumerateOp enumerate = call.clasp->getEnumerate()) {
    if (!enumerate(cx, call.obj)) {
      ThrowPendingToV8(cx);
      return;
    }
  }
  JSNewEnumerateOp newEnumerate = call.clasp->getNewEnumerate();
  if (!newEnumerate) {
    return;
  }
  JS::RootedIdVector ids(cx);
  if (!newEnumerate(cx, call.obj, &ids, false)) {
    ThrowPendingToV8(cx);
    return;
  }
  std::vector<v8::Local<v8::Value>> keys;
  for (size_t i = 0; i < ids.length(); i++) {
    jsid id = ids[i];
    if (id.isInt() != aIndices) {
      continue;
    }
    if (id.isInt()) {
      keys.push_back(IndexKey(isolate, uint32_t(id.toInt())));
    } else {
      keys.push_back(ToV8(cx, id));
    }
  }
  aInfo.GetReturnValue().Set(
      v8::Array::New(isolate, keys.data(), keys.size()));
}

v8::Local<v8::Value> DescriptorValue(const v8::PropertyDescriptor& aDesc,
                                     v8::Isolate* aIsolate) {
  if (aDesc.has_value()) {
    return aDesc.value();
  }
  return v8::Undefined(aIsolate);
}

void NamedGetter(v8::Local<v8::Name> aName,
                 const v8::PropertyCallbackInfo<v8::Value>& aInfo) {
  LookupHook(aInfo.Holder(), aName, aInfo.Data());
}

void NamedSetter(v8::Local<v8::Name> aName, v8::Local<v8::Value> aValue,
                 const v8::PropertyCallbackInfo<v8::Value>& aInfo) {
  AddHook(aInfo.Holder(), aName, aValue, aInfo.Data(), true);
}

void NamedQuery(v8::Local<v8::Name> aName,
                const v8::PropertyCallbackInfo<v8::Integer>& aInfo) {
  QueryHook(aInfo.Holder(), aName, aName, aInfo.Data(), aInfo);
}

void NamedDeleter(v8::Local<v8::Name> aName,
                  const v8::PropertyCallbackInfo<v8::Boolean>& aInfo) {
  DeleteHook(aInfo.Holder(), aName, aName, aInfo.Data(), aInfo);
}

void NamedEnumerator(const v8::PropertyCallbackInfo<v8::Array>& aInfo) {
  EnumerateHook(aInfo.Holder(), aInfo.Data(), false, aInfo);
}

void NamedDefiner(v8::Local<v8::Name> aName,
                  const v8::PropertyDescriptor& aDesc,
                  const v8::PropertyCallbackInfo<v8::Value>& aInfo) {
  AddHook(aInfo.Holder(), aName, DescriptorValue(aDesc, aInfo.GetIsolate()),
          aInfo.Data(), false);
}

void IndexedGetter(uint32_t aIndex,
                   const v8::PropertyCallbackInfo<v8::Value>& aInfo) {
  LookupHook(aInfo.Holder(), IndexKey(aInfo.GetIsolate(), aIndex),
             aInfo.Data());
}

void IndexedSetter(uint32_t aIndex, v8::Local<v8::Value> aValue,
                   const v8::PropertyCallbackInfo<v8::Value>& aInfo) {
  AddHook(aInfo.Holder(), IndexKey(aInfo.GetIsolate(), aIndex), aValue,
          aInfo.Data(), true);
}

void IndexedQuery(uint32_t aIndex,
                  const v8::PropertyCallbackInfo<v8::Integer>& aInfo) {
  QueryHook(aInfo.Holder(), IndexKey(aInfo.GetIsolate(), aIndex),
            IndexName(aInfo.GetIsolate(), aIndex), aInfo.Data(), aInfo);
}

void IndexedDeleter(uint32_t aIndex,
                    const v8::PropertyCallbackInfo<v8::Boolean>& aInfo) {
  DeleteHook(aInfo.Holder(), IndexKey(aInfo.GetIsolate(), aIndex),
             IndexName(aInfo.GetIsolate(), aIndex), aInfo.Data(), aInfo);
}

void IndexedEnumerator(const v8::PropertyCallbackInfo<v8::Array>& aInfo) {
  EnumerateHook(aInfo.Holder(), aInfo.Data(), true, aInfo);
}

void IndexedDefiner(uint32_t aIndex, const v8::PropertyDescriptor& aDesc,
                    const v8::PropertyCallbackInfo<v8::Value>& aInfo) {
  AddHook(aInfo.Holder(), IndexKey(aInfo.GetIsolate(), aIndex),
          DescriptorValue(aDesc, aInfo.GetIsolate()), aInfo.Data(), false);
}

void CallHook(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  JSContext* cx = CurrentContext();
  const JSClass* clasp = ClassOfData(aInfo.Data());
  bool constructing = aInfo.IsConstructCall();
  JSNative native = constructing ? clasp->getConstruct() : clasp->getCall();
  v8::Local<v8::Object> callee = aInfo.Holder();
  JSObject* obj = ObjectFromV8(cx, callee);
  if (!native) {
    ReportJSError(cx, constructing ? JSMSG_NOT_CONSTRUCTOR : JSMSG_NOT_FUNCTION,
                  clasp->name);
    ThrowPendingToV8(cx);
    return;
  }
  InvokeNative(cx, native, RealmOfCell(obj), callee, aInfo);
}

}

void SlotWriteBarrier(JSContext* aCx, const JS::Value& aValue) {
  if (!aValue.isGCThing() || !aCx) {
    return;
  }
  CellTail* tail = TailOf(reinterpret_cast<js::gc::Cell*>(aValue.toGCThing()),
                          aValue.traceKind());
  if (!tail || tail->handle.IsEmpty()) {
    return;
  }
  v8::HandleScope scope(aCx->isolate);
  SetTraced(aCx->isolate, tail, tail->handle.Get(aCx->isolate));
}

JS::Value* SlotAddress(JSObject* aObj, uint32_t aIndex) {
  ObjectCell* cell = ObjectCell::from(aObj);
  if (cell->shape->isProxy) {
    return &cell->proxyData()->values()->reservedSlots[aIndex];
  }
  if (aIndex < kFixedSlots) {
    return &cell->fixedSlots()[aIndex];
  }
  JS::Value*& dynamic = *reinterpret_cast<JS::Value**>(&cell->words[0]);
  if (!dynamic) {
    uint32_t count = JSCLASS_RESERVED_SLOTS(cell->shape->base.clasp);
    size_t extra = count > kFixedSlots ? count - kFixedSlots : 1;
    dynamic = static_cast<JS::Value*>(malloc(extra * sizeof(JS::Value)));
    for (size_t i = 0; i < extra; i++) {
      dynamic[i] = JS::UndefinedValue();
    }
  }
  return &dynamic[aIndex - kFixedSlots];
}

bool IdIsName(jsid aId, const char* aName) {
  if (!aId.isString()) {
    return false;
  }
  StringCell* cell = StringCell::from(aId.toString());
  size_t length = strlen(aName);
  if (cell->length() != length) {
    return false;
  }
  if (cell->latin1()) {
    return memcmp(cell->chars, aName, length) == 0;
  }
  const char16_t* chars = static_cast<const char16_t*>(cell->chars);
  for (size_t i = 0; i < length; i++) {
    if (chars[i] != char16_t(static_cast<unsigned char>(aName[i]))) {
      return false;
    }
  }
  return true;
}

JSObject* NewPlainObjectWithProto(JSContext* aCx, JS::HandleObject aProto) {
  AutoV8 v8(aCx);
  v8::Isolate* isolate = aCx->isolate;
  v8::TryCatch tryCatch(isolate);
  v8::Local<v8::Object> object = v8::Object::New(isolate);
  v8::Local<v8::Value> proto = aProto ? ToV8(aCx, aProto).As<v8::Value>()
                                      : v8::Null(isolate).As<v8::Value>();
  if (object->SetPrototype(v8.context(), proto).IsNothing()) {
    v8.Check(tryCatch);
    return nullptr;
  }
  return ObjectFromV8(aCx, object);
}

bool IdIsAsciiName(jsid aId, const char* aName) { return IdIsName(aId, aName); }

ClassesState& State(JSContext* aCx) { return StateFor(aCx->runtime); }

void PushNoResolve(JSContext* aCx, JSObject* aObj, jsid aId) {
  State(aCx).resolving.emplace_back(aObj, aId);
}

void PopNoResolve(JSContext* aCx) { State(aCx).resolving.pop_back(); }

RealmExtra& RealmData(JS::Realm* aRealm) {
  ClassesState& state = StateFor(aRealm->runtime);
  RealmExtra*& extra = state.realms[aRealm];
  if (!extra) {
    extra = new RealmExtra();
  }
  return *extra;
}

CompartmentExtra& CompartmentData(JS::Compartment* aCompartment) {
  ClassesState& state = State(CurrentContext());
  CompartmentExtra*& extra = state.compartments[aCompartment];
  if (!extra) {
    extra = new CompartmentExtra();
  }
  return *extra;
}

JS::Realm* RealmOfCell(JSObject* aObj) {
  if (!aObj) {
    return nullptr;
  }
  return ObjectCell::from(aObj)->shape->base.realm;
}

bool IsGlobalCell(JSObject* aObj) {
  return aObj && (JS::GetClass(aObj)->flags & JSCLASS_IS_GLOBAL);
}

v8::Local<v8::Object> ScriptTarget(JSContext* aCx, JSObject* aObj) {
  v8::Local<v8::Object> object = ToV8(aCx, aObj);
  if (IsGlobalCell(aObj)) {
    JS::Realm* realm = RealmOfCell(aObj);
    if (realm && realm->global == aObj) {
      RealmExtra& extra = RealmData(realm);
      if (!extra.innerGlobal.IsEmpty()) {
        return extra.innerGlobal.Get(aCx->isolate);
      }
    }
  }
  return object;
}

void ThrowPendingToV8(JSContext* aCx) {
  if (!JS_IsExceptionPending(aCx)) {
    aCx->isolate->TerminateExecution();
    return;
  }
  JS::Rooted<JS::Value> exception(aCx);
  if (!JS_GetPendingException(aCx, &exception)) {
    aCx->isolate->TerminateExecution();
    return;
  }
  JS_ClearPendingException(aCx);
  aCx->isolate->ThrowException(ToV8(aCx, exception));
}

bool ReportJSError(JSContext* aCx, unsigned aErrorNumber, const char* aArg0,
                   const char* aArg1, const char* aArg2) {
  const char* args[4] = {aArg0 ? aArg0 : "", aArg1 ? aArg1 : "",
                         aArg2 ? aArg2 : "", nullptr};
  JS_ReportErrorNumberUTF8Array(aCx, js::GetErrorMessage, nullptr,
                                aErrorNumber, args);
  return false;
}

v8::Local<v8::ObjectTemplate> HolderTemplate(JSContext* aCx) {
  ClassesState& state = State(aCx);
  if (state.holderTemplate.IsEmpty()) {
    v8::HandleScope scope(aCx->isolate);
    v8::Local<v8::ObjectTemplate> templ =
        v8::ObjectTemplate::New(aCx->isolate);
    templ->SetInternalFieldCount(2);
    state.holderTemplate.Reset(aCx->isolate, templ);
  }
  return state.holderTemplate.Get(aCx->isolate);
}

void AnchorCellOn(JSContext* aCx, ObjectCell* aCell,
                  v8::Local<v8::Object> aHolder) {
  if (!aCell->tail.anchor) {
    aCell->tail.anchor = cppgc::MakeGarbageCollected<Anchor>(
        aCx->isolate->GetCppHeap()->GetAllocationHandle(), aCell);
  }
  aHolder->SetAlignedPointerInInternalField(
      0, const_cast<WrapperTypeTag*>(&kWrapperTypeTag));
  aHolder->SetAlignedPointerInInternalField(1, aCell->tail.anchor);
  aCell->tail.anchor->SetWrapper(aCx->isolate, aHolder);
}

void InvokeNative(JSContext* aCx, JSNative aNative, JS::Realm* aRealm,
                  v8::Local<v8::Object> aCallee,
                  const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  bool constructing = aInfo.IsConstructCall();
  unsigned argc = unsigned(aInfo.Length());
  JS::RootedValueVector vp(aCx);
  if (!vp.resize(2 + argc + (constructing ? 1 : 0))) {
    aCx->isolate->ThrowError("out of memory");
    return;
  }
  vp[0].setObject(*ObjectFromV8(aCx, aCallee));
  if (constructing) {
    vp[1].setMagic(JS_IS_CONSTRUCTING);
  } else {
    vp[1].set(ValueFromV8(aCx, aInfo.This()));
  }
  for (unsigned i = 0; i < argc; i++) {
    vp[2 + i].set(ValueFromV8(aCx, aInfo[int(i)]));
  }
  if (constructing) {
    vp[2 + argc].set(ValueFromV8(aCx, aInfo.NewTarget()));
  }
  bool ok;
  {
    AutoCellRealm realm(aCx, aRealm);
    ok = aNative(aCx, argc, vp.begin());
  }
  if (!ok) {
    ThrowPendingToV8(aCx);
    return;
  }
  aInfo.GetReturnValue().Set(ToV8(aCx, vp[0]));
}

v8::Local<v8::FunctionTemplate> ClassTemplate(JSContext* aCx,
                                              const JSClass* aClass) {
  ClassesState& state = State(aCx);
  v8::Isolate* isolate = aCx->isolate;
  auto found = state.templates.find(aClass);
  if (found != state.templates.end()) {
    return found->second.Get(isolate);
  }
  v8::EscapableHandleScope scope(isolate);
  v8::Local<v8::FunctionTemplate> ft = v8::FunctionTemplate::New(isolate);
  ft->SetClassName(v8::String::NewFromUtf8(
                       isolate, aClass->name ? aClass->name : "Object")
                       .ToLocalChecked());
  v8::Local<v8::ObjectTemplate> inst = ft->InstanceTemplate();
  inst->SetInternalFieldCount(2);
  v8::Local<v8::Value> data =
      v8::External::New(isolate, const_cast<JSClass*>(aClass));
  if (HasPropertyHooks(aClass)) {
    inst->SetHandler(v8::NamedPropertyHandlerConfiguration(
        NamedGetter, NamedSetter, NamedQuery, NamedDeleter, NamedEnumerator,
        NamedDefiner,
        static_cast<v8::GenericNamedPropertyDescriptorCallback>(nullptr),
        data));
    inst->SetHandler(v8::IndexedPropertyHandlerConfiguration(
        IndexedGetter, IndexedSetter, IndexedQuery, IndexedDeleter,
        IndexedEnumerator, IndexedDefiner,
        static_cast<v8::IndexedPropertyDescriptorCallback>(nullptr), data));
  }
  if (aClass->getCall() || aClass->getConstruct()) {
    inst->SetCallAsFunctionHandler(CallHook, data);
  }
  if (aClass->flags & JSCLASS_EMULATES_UNDEFINED) {
    inst->MarkAsUndetectable();
  }
  state.templates[aClass].Reset(isolate, ft);
  return scope.Escape(ft);
}

JSObject* NewClassObject(JSContext* aCx, const JSClass* aClass,
                         JS::HandleObject aProto) {
  AutoV8 v8(aCx);
  v8::Isolate* isolate = aCx->isolate;
  v8::Local<v8::Context> ctx = v8.context();
  v8::TryCatch tryCatch(isolate);
  v8::Local<v8::FunctionTemplate> ft = ClassTemplate(aCx, aClass);
  v8::Local<v8::Object> object;
  if (!ft->InstanceTemplate()->NewInstance(ctx).ToLocal(&object)) {
    v8.Check(tryCatch);
    return nullptr;
  }
  v8::Local<v8::Value> proto = aProto ? ToV8(aCx, aProto).As<v8::Value>()
                                      : v8::Null(isolate).As<v8::Value>();
  if (object->SetPrototype(ctx, proto).IsNothing()) {
    v8.Check(tryCatch);
    return nullptr;
  }
  Shape* shape = ShapeFor(aClass, aCx->realm());
  ObjectCell* cell = NewObjectCell(aCx, shape, object, true);
  object->SetAlignedPointerInInternalField(
      0, const_cast<WrapperTypeTag*>(&kWrapperTypeTag));
  return cell->asObject();
}

const char* StandardClassName(JSProtoKey aKey) {
  if (aKey <= JSProto_Null || aKey >= JSProto_LIMIT) {
    return nullptr;
  }
  return kStandardNames[aKey];
}

}

using namespace vge::jsapi;

JS_PUBLIC_DATA const JSClass* const js::FunctionClassPtr = &FunctionClass;
JS_PUBLIC_DATA const JSClass* const js::FunctionExtendedClassPtr =
    &FunctionExtendedClass;
JS_PUBLIC_DATA const JSClass* const js::ObjectClassPtr = &PlainObjectClass;

const JSClass* const JS::DataView::FixedLengthClassPtr = &DataViewClasses[0];
const JSClass* const JS::DataView::ResizableClassPtr = &DataViewClasses[1];
const JSClass* const JS::DataView::ImmutableClassPtr = &DataViewClasses[2];

JS_PUBLIC_API const JSClass* js::ProtoKeyToClass(JSProtoKey aKey) {
  switch (aKey) {
    case JSProto_Object:
      return &PlainObjectClass;
    case JSProto_Function:
      return &FunctionClass;
    case JSProto_Array:
      return &ArrayClass;
    case JSProto_ArrayBuffer:
      return &buffers::ArrayBufferClasses[0];
    case JSProto_SharedArrayBuffer:
      return &buffers::ArrayBufferClasses[3];
    case JSProto_DataView:
      return &DataViewClasses[0];
    case JSProto_SavedFrame:
      return &SavedFrameClass;
    case JSProto_Int8Array:
      return &buffers::TypedArrayFixedClasses[JS::Scalar::Int8];
    case JSProto_Uint8Array:
      return &buffers::TypedArrayFixedClasses[JS::Scalar::Uint8];
    case JSProto_Int16Array:
      return &buffers::TypedArrayFixedClasses[JS::Scalar::Int16];
    case JSProto_Uint16Array:
      return &buffers::TypedArrayFixedClasses[JS::Scalar::Uint16];
    case JSProto_Int32Array:
      return &buffers::TypedArrayFixedClasses[JS::Scalar::Int32];
    case JSProto_Uint32Array:
      return &buffers::TypedArrayFixedClasses[JS::Scalar::Uint32];
    case JSProto_Float32Array:
      return &buffers::TypedArrayFixedClasses[JS::Scalar::Float32];
    case JSProto_Float64Array:
      return &buffers::TypedArrayFixedClasses[JS::Scalar::Float64];
    case JSProto_Uint8ClampedArray:
      return &buffers::TypedArrayFixedClasses[JS::Scalar::Uint8Clamped];
    case JSProto_BigInt64Array:
      return &buffers::TypedArrayFixedClasses[JS::Scalar::BigInt64];
    case JSProto_BigUint64Array:
      return &buffers::TypedArrayFixedClasses[JS::Scalar::BigUint64];
    case JSProto_Float16Array:
      return &buffers::TypedArrayFixedClasses[JS::Scalar::Float16];
    default:
      break;
  }
  if (aKey <= JSProto_Null || aKey >= JSProto_LIMIT ||
      !kStandardClasses[aKey].name) {
    return nullptr;
  }
  return &kStandardClasses[aKey];
}

JSObject* JS_NewPlainObject(JSContext* aCx) {
  AutoV8 v8(aCx);
  return ObjectFromV8(aCx, v8::Object::New(aCx->isolate));
}

JSObject* JS_NewObject(JSContext* aCx, const JSClass* aClass) {
  if (!aClass) {
    return JS_NewPlainObject(aCx);
  }
  JSProtoKey key = JSCLASS_CACHED_PROTO_KEY(aClass);
  if (key == JSProto_Null) {
    key = JSProto_Object;
  }
  JS::RootedObject proto(aCx);
  if (!JS_GetClassPrototype(aCx, key, &proto)) {
    return nullptr;
  }
  return NewClassObject(aCx, aClass, proto);
}

JSObject* JS_NewObjectWithGivenProto(JSContext* aCx, const JSClass* aClass,
                                     JS::HandleObject aProto) {
  if (!aClass) {
    return NewPlainObjectWithProto(aCx, aProto);
  }
  return NewClassObject(aCx, aClass, aProto);
}

JSObject* JS_NewObjectWithGivenProtoAndUseAllocSite(JSContext* aCx,
                                                    const JSClass* aClass,
                                                    JS::HandleObject aProto) {
  return JS_NewObjectWithGivenProto(aCx, aClass, aProto);
}

JSObject* JS_NewObjectWithoutMetadata(JSContext* aCx, const JSClass* aClass,
                                      JS::Handle<JSObject*> aProto) {
  return JS_NewObjectWithGivenProto(aCx, aClass, aProto);
}

void JS_SetReservedSlot(JSObject* aObj, uint32_t aIndex,
                        const JS::Value& aValue) {
  *SlotAddress(aObj, aIndex) = aValue;
  SlotWriteBarrier(CurrentContext(), aValue);
}

void JS_SetAllNonReservedSlotsToUndefined(JS::HandleObject aObj) {
  ObjectCell* cell = ObjectCell::from(aObj);
  if (cell->shape->isProxy) {
    return;
  }
  uint32_t reserved = JSCLASS_RESERVED_SLOTS(cell->shape->base.clasp);
  for (uint32_t i = reserved; i < kFixedSlots; i++) {
    cell->fixedSlots()[i] = JS::UndefinedValue();
  }
  JSContext* cx = CurrentContext();
  JSAutoRealm realm(cx, aObj);
  AutoV8 v8(cx);
  v8::Isolate* isolate = cx->isolate;
  v8::Local<v8::Context> ctx = v8.context();
  v8::TryCatch tryCatch(isolate);
  v8::Local<v8::Object> object = ScriptTarget(cx, aObj);
  v8::Local<v8::Array> names;
  if (!object
           ->GetPropertyNames(ctx, v8::KeyCollectionMode::kOwnOnly,
                              v8::ALL_PROPERTIES,
                              v8::IndexFilter::kIncludeIndices,
                              v8::KeyConversionMode::kConvertToString)
           .ToLocal(&names)) {
    return;
  }
  v8::Local<v8::Value> undefined = v8::Undefined(isolate);
  v8::Local<v8::String> valueKey =
      v8::String::NewFromUtf8Literal(isolate, "value");
  for (uint32_t i = 0; i < names->Length(); i++) {
    v8::Local<v8::Value> key;
    if (!names->Get(ctx, i).ToLocal(&key) || !key->IsName()) {
      continue;
    }
    v8::Local<v8::Name> name = key.As<v8::Name>();
    if (!object->HasRealNamedProperty(ctx, name).FromMaybe(false)) {
      continue;
    }
    v8::Local<v8::Value> descriptor;
    if (!object->GetOwnPropertyDescriptor(ctx, name).ToLocal(&descriptor) ||
        !descriptor->IsObject()) {
      continue;
    }
    if (!descriptor.As<v8::Object>()
             ->HasOwnProperty(ctx, valueKey)
             .FromMaybe(false)) {
      continue;
    }
    v8::Maybe<v8::PropertyAttribute> attrs =
        object->GetRealNamedPropertyAttributes(ctx, name);
    object
        ->DefineOwnProperty(ctx, name, undefined,
                            attrs.FromMaybe(v8::PropertyAttribute::None))
        .FromMaybe(false);
  }
}

bool JS::ObjectOpResult::failReadOnly() { return fail(JSMSG_READ_ONLY); }
bool JS::ObjectOpResult::failCantDelete() { return fail(JSMSG_CANT_DELETE); }
bool JS::ObjectOpResult::failCantDefineWindowElement() {
  return fail(JSMSG_CANT_DEFINE_WINDOW_ELEMENT);
}
bool JS::ObjectOpResult::failCantDeleteWindowElement() {
  return fail(JSMSG_CANT_DELETE_WINDOW_ELEMENT);
}
bool JS::ObjectOpResult::failCantDefineWindowNamedProperty() {
  return fail(JSMSG_CANT_DEFINE_WINDOW_NAMED_PROPERTY);
}
bool JS::ObjectOpResult::failCantDeleteWindowNamedProperty() {
  return fail(JSMSG_CANT_DELETE_WINDOW_NAMED_PROPERTY);
}
bool JS::ObjectOpResult::failCantPreventExtensions() {
  return fail(JSMSG_CANT_PREVENT_EXTENSIONS);
}
bool JS::ObjectOpResult::failCantSetProto() {
  return fail(JSMSG_CANT_SET_PROTO);
}
bool JS::ObjectOpResult::failNoNamedSetter() {
  return fail(JSMSG_NO_NAMED_SETTER);
}
bool JS::ObjectOpResult::failNoIndexedSetter() {
  return fail(JSMSG_NO_INDEXED_SETTER);
}
bool JS::ObjectOpResult::failNotDataDescriptor() {
  return fail(JSMSG_NOT_DATA_DESCRIPTOR);
}
bool JS::ObjectOpResult::failInvalidDescriptor() {
  return fail(JSMSG_INVALID_DESCRIPTOR);
}
bool JS::ObjectOpResult::failBadArrayLength() {
  return fail(JSMSG_BAD_ARRAY_LENGTH);
}
bool JS::ObjectOpResult::failBadIndex() { return fail(JSMSG_BAD_INDEX); }

bool JS::ObjectOpResult::reportError(JSContext* aCx, JS::HandleObject aObj) {
  JS_ReportErrorNumberASCII(aCx, js::GetErrorMessage, nullptr,
                            unsigned(code_));
  return false;
}

void JS::PropertyDescriptor::trace(JSTracer* aTrc) {
  if (getter_) {
    JS::TraceRoot(aTrc, &getter_, "Descriptor::getter");
  }
  if (setter_) {
    JS::TraceRoot(aTrc, &setter_, "Descriptor::setter");
  }
  JS::TraceRoot(aTrc, &value_, "Descriptor::value");
}

bool JSPropertySpec::getValue(JSContext* aCx,
                              JS::MutableHandleValue aVp) const {
  switch (u.value.type) {
    case ValueWrapper::Type::String: {
      JSString* atom = JS_AtomizeString(aCx, u.value.string);
      if (!atom) {
        return false;
      }
      aVp.setString(atom);
      return true;
    }
    case ValueWrapper::Type::Int32:
      aVp.setInt32(u.value.int32);
      return true;
    case ValueWrapper::Type::Double:
      aVp.setDouble(u.value.double_);
      return true;
  }
  return false;
}

bool js::ElementAdder::append(JSContext* aCx, JS::HandleValue aValue) {
  if (resObj_) {
    if (!JS_DefineElement(aCx, resObj_, index_, aValue, JSPROP_ENUMERATE)) {
      return false;
    }
  } else {
    vp_[index_] = aValue;
  }
  index_++;
  return true;
}

void js::ElementAdder::appendHole() {
  if (!resObj_) {
    vp_[index_].setMagic(JS_ELEMENTS_HOLE);
  }
  index_++;
}

JSProtoKey JS::IdentifyStandardInstance(JSObject* aObj) {
  bool isPrototype = false;
  JSProtoKey key = StandardKeyOf(CurrentContext(), aObj, &isPrototype);
  return key != JSProto_Null && !isPrototype ? key : JSProto_Null;
}

JSProtoKey JS::IdentifyStandardPrototype(JSObject* aObj) {
  bool isPrototype = false;
  JSProtoKey key = StandardKeyOf(CurrentContext(), aObj, &isPrototype);
  return key != JSProto_Null && isPrototype ? key : JSProto_Null;
}

JSProtoKey JS::IdentifyStandardInstanceOrPrototype(JSObject* aObj) {
  bool isPrototype = false;
  return StandardKeyOf(CurrentContext(), aObj, &isPrototype);
}

JSProtoKey JS::IdentifyStandardConstructor(JSObject* aObj) {
  JSContext* cx = CurrentContext();
  JS::Realm* realm = RealmOfCell(aObj);
  if (!realm || !JS::GetClass(aObj)->isJSFunction()) {
    return JSProto_Null;
  }
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::Object> object = ToV8(cx, aObj);
  RealmExtra& extra = RealmData(realm);
  for (size_t k = 1; k < JSProto_LIMIT; k++) {
    if (!extra.ctors[k].IsEmpty() && extra.ctors[k].Get(cx->isolate) == object &&
        object->IsFunction()) {
      return JSProtoKey(k);
    }
  }
  return JSProto_Null;
}

bool js::ShouldIgnorePropertyDefinition(JSContext* aCx, JSProtoKey aKey,
                                        jsid aId) {
  if (aCx->realm() && !aCx->realm()->creationOptions.getToSourceEnabled() &&
      (IdIsName(aId, "toSource") || IdIsName(aId, "uneval"))) {
    return true;
  }
  if (aKey == JSProto_FinalizationRegistry &&
      !JS::Prefs::experimental_weakrefs_expose_cleanupSome() &&
      IdIsName(aId, "cleanupSome")) {
    return true;
  }
  if (aKey == JSProto_Function) {
    if (!JS::Prefs::experimental_error_iserror() && IdIsName(aId, "isError")) {
      return true;
    }
    if (!JS::Prefs::experimental_iterator_sequencing() &&
        IdIsName(aId, "concat")) {
      return true;
    }
    if (!JS::Prefs::experimental_joint_iteration() &&
        (IdIsName(aId, "zip") || IdIsName(aId, "zipKeyed"))) {
      return true;
    }
    if (!JS::Prefs::experimental_promise_allkeyed() &&
        (IdIsName(aId, "allKeyed") || IdIsName(aId, "allSettledKeyed"))) {
      return true;
    }
  }
#ifdef JS_HAS_INTL_API
  if (aKey == JSProto_Locale && !JS::Prefs::experimental_intl_locale_info()) {
    if (IdIsName(aId, "firstDayOfWeek") || IdIsName(aId, "getTextInfo") ||
        IdIsName(aId, "getNumberingSystems") ||
        IdIsName(aId, "getCollations") || IdIsName(aId, "getCalendars") ||
        IdIsName(aId, "getHourCycles") || IdIsName(aId, "getWeekInfo") ||
        IdIsName(aId, "getTimeZones")) {
      return true;
    }
  }
#endif
#ifdef NIGHTLY_BUILD
  if (aKey == JSProto_Function && !JS::Prefs::experimental_iterator_range() &&
      IdIsName(aId, "range")) {
    return true;
  }
  if (aKey == JSProto_ArrayBuffer &&
      !JS::Prefs::experimental_arraybuffer_immutable()) {
    if (IdIsName(aId, "immutable") || IdIsName(aId, "sliceToImmutable") ||
        IdIsName(aId, "transferToImmutable")) {
      return true;
    }
  }
#endif
  if (aKey == JSProto_Iterator) {
    if (!JS::Prefs::experimental_iterator_chunking() &&
        (IdIsName(aId, "chunks") || IdIsName(aId, "windows"))) {
      return true;
    }
    if (!JS::Prefs::experimental_iterator_includes() &&
        IdIsName(aId, "includes")) {
      return true;
    }
    if (!JS::Prefs::experimental_iterator_join() && IdIsName(aId, "join")) {
      return true;
    }
  }
  if (aKey == JSProto_Function &&
      !JS::Prefs::experimental_error_capture_stack_trace() &&
      IdIsName(aId, "captureStackTrace")) {
    return true;
  }
  if (aKey == JSProto_Function &&
      !JS::Prefs::experimental_error_stack_trace_limit() &&
      IdIsName(aId, "stackTraceLimit")) {
    return true;
  }
  if (aKey == JSProto_Atomics && !JS::Prefs::experimental_atomics_pause() &&
      IdIsName(aId, "pause")) {
    return true;
  }
  if (aKey == JSProto_Atomics && !JS::Prefs::atomics_wait_async() &&
      IdIsName(aId, "waitAsync")) {
    return true;
  }
  return false;
}
