#include "VgeJsProxyInternal.h"

#include <vector>

#include "js/Object.h"
#include "js/PropertyDescriptor.h"
#include "js/TracingAPI.h"
#include "vm/PropertyResult.h"

namespace vge::jsapi::proxy {

RuntimeState& State() {
  static thread_local RuntimeState* sState = nullptr;
  JSRuntime* rt = Runtime();
  if (!sState || sState->runtime != rt) {
    sState = new RuntimeState();
    sState->runtime = rt;
  }
  return *sState;
}

bool IsLazyProto(JSObject* aProxy) {
  RuntimeState& st = State();
  auto it = st.proxies.find(ObjectCell::from(aProxy));
  return it != st.proxies.end() && it->second.lazyProto;
}

namespace {

struct Policy {
  bool allowed;
  bool rv;
};

Policy EnterPolicy(JSContext* aCx, const js::BaseProxyHandler* aHandler,
                   JS::HandleObject aProxy, JS::HandleId aId,
                   js::BaseProxyHandler::Action aAct, bool aMayThrow) {
  Policy policy{true, false};
  if (!aHandler->hasSecurityPolicy()) {
    return policy;
  }
  bool rv = false;
  policy.allowed = aHandler->enter(aCx, aProxy, aId, aAct, aMayThrow, &rv);
  policy.rv = rv;
  if (!policy.allowed && !rv && aMayThrow && !aCx->exceptionPending) {
    if (aId.isVoid()) {
      js::ReportAccessDenied(aCx);
    } else {
      std::string name = IdToUTF8(aCx, aId);
      ReportErrorNumber(aCx, JSMSG_PROPERTY_ACCESS_DENIED, {name.c_str()});
    }
  }
  return policy;
}

const js::BaseProxyHandler* HandlerOf(JSObject* aProxy) {
  return js::GetProxyHandler(aProxy);
}

JS::Value WindowProxyIfWindow(const JS::Value& aValue, JSObject* aProxy) {
  if (aValue.isObject() && aValue != JS::ObjectValue(*aProxy)) {
    return JS::ObjectValue(*js::ToWindowProxyIfWindow(&aValue.toObject()));
  }
  return aValue;
}

bool IsIndexId(jsid aId, uint32_t* aIndex) {
  if (aId.isInt()) {
    *aIndex = uint32_t(aId.toInt());
    return true;
  }
  if (!aId.isString()) {
    return false;
  }
  StringCell* str = StringCell::from(aId.toString());
  uint32_t length = str->length();
  if (length == 0 || length > 10) {
    return false;
  }
  uint64_t value = 0;
  for (uint32_t i = 0; i < length; i++) {
    char16_t c = str->latin1()
                     ? char16_t(static_cast<const uint8_t*>(str->chars)[i])
                     : static_cast<const char16_t*>(str->chars)[i];
    if (c < '0' || c > '9' || (i == 0 && c == '0' && length > 1)) {
      return false;
    }
    value = value * 10 + uint64_t(c - '0');
  }
  if (value > 4294967294ULL) {
    return false;
  }
  *aIndex = uint32_t(value);
  return true;
}

bool IndexToId(JSContext* aCx, uint32_t aIndex, JS::MutableHandleId aId) {
  if (aIndex <= uint32_t(JS::PropertyKey::IntMax)) {
    aId.set(JS::PropertyKey::Int(int32_t(aIndex)));
    return true;
  }
  return IdFromV8(aCx, v8::Integer::NewFromUnsigned(aCx->isolate, aIndex), aId);
}

class InterceptorScope {
 public:
  InterceptorScope(JSContext* aCx, v8::Local<v8::Object> aHolder)
      : proxy(aCx, ObjectFromV8(aCx, aHolder)), mRealm(aCx, proxy.get()) {}

  JS::RootedObject proxy;

 private:
  AutoEnterRealmOf mRealm;
};

void GetCommon(JSContext* aCx, JS::HandleObject aProxy, JS::HandleId aId,
               const v8::PropertyCallbackInfo<v8::Value>& aInfo) {
  JS::RootedValue receiver(aCx, ValueFromV8(aCx, aInfo.This()));
  JS::RootedValue vp(aCx);
  if (!ProxyGet(aCx, aProxy, receiver, aId, &vp)) {
    ThrowPendingToV8(aCx);
    return;
  }
  aInfo.GetReturnValue().Set(ToV8(aCx, vp.get()));
}

void SetCommon(JSContext* aCx, JS::HandleObject aProxy, JS::HandleId aId,
               v8::Local<v8::Value> aValue,
               const v8::PropertyCallbackInfo<v8::Value>& aInfo) {
  JS::RootedValue value(aCx, ValueFromV8(aCx, aValue));
  JS::RootedValue receiver(aCx, ValueFromV8(aCx, aInfo.This()));
  JS::ObjectOpResult result;
  if (!ProxySet(aCx, aProxy, aId, value, receiver, result)) {
    ThrowPendingToV8(aCx);
    return;
  }
  if (!result.ok() && aInfo.ShouldThrowOnError()) {
    ReportOpFailure(aCx, aProxy, aId, result);
    ThrowPendingToV8(aCx);
    return;
  }
  aInfo.GetReturnValue().Set(aValue);
}

void QueryCommon(JSContext* aCx, JS::HandleObject aProxy, JS::HandleId aId,
                 const v8::PropertyCallbackInfo<v8::Integer>& aInfo) {
  JS::Rooted<mozilla::Maybe<JS::PropertyDescriptor>> desc(aCx);
  if (!ProxyGetOwnPropertyDescriptor(aCx, aProxy, aId, &desc)) {
    ThrowPendingToV8(aCx);
    return;
  }
  if (desc.isNothing()) {
    return;
  }
  int attributes = v8::None;
  if (desc->hasEnumerable() && !desc->enumerable()) {
    attributes |= v8::DontEnum;
  }
  if (desc->hasConfigurable() && !desc->configurable()) {
    attributes |= v8::DontDelete;
  }
  if (desc->hasWritable() && !desc->writable()) {
    attributes |= v8::ReadOnly;
  }
  aInfo.GetReturnValue().Set(int32_t(attributes));
}

void DeleteCommon(JSContext* aCx, JS::HandleObject aProxy, JS::HandleId aId,
                  const v8::PropertyCallbackInfo<v8::Boolean>& aInfo) {
  JS::ObjectOpResult result;
  if (!ProxyDelete(aCx, aProxy, aId, result)) {
    ThrowPendingToV8(aCx);
    return;
  }
  if (!result.ok() && aInfo.ShouldThrowOnError()) {
    ReportOpFailure(aCx, aProxy, aId, result);
    ThrowPendingToV8(aCx);
    return;
  }
  aInfo.GetReturnValue().Set(result.ok());
}

void EnumerateCommon(JSContext* aCx, JS::HandleObject aProxy, bool aIndexed,
                     const v8::PropertyCallbackInfo<v8::Array>& aInfo) {
  JS::RootedIdVector keys(aCx);
  if (!ProxyOwnPropertyKeys(aCx, aProxy, &keys)) {
    ThrowPendingToV8(aCx);
    return;
  }
  v8::Isolate* isolate = aCx->isolate;
  std::vector<v8::Local<v8::Value>> out;
  out.reserve(keys.length());
  for (size_t i = 0; i < keys.length(); i++) {
    uint32_t index = 0;
    bool isIndex = IsIndexId(keys[i], &index);
    if (aIndexed && isIndex) {
      out.push_back(v8::Integer::NewFromUnsigned(isolate, index));
    } else if (!aIndexed && !isIndex) {
      out.push_back(ToV8(aCx, keys[i].get()));
    }
  }
  aInfo.GetReturnValue().Set(v8::Array::New(isolate, out.data(), out.size()));
}

void DefineCommon(JSContext* aCx, JS::HandleObject aProxy, JS::HandleId aId,
                  const v8::PropertyDescriptor& aDesc,
                  const v8::PropertyCallbackInfo<v8::Value>& aInfo) {
  JS::Rooted<JS::PropertyDescriptor> desc(aCx, JS::PropertyDescriptor::Empty());
  if (aDesc.has_value()) {
    JS::RootedValue value(aCx, ValueFromV8(aCx, aDesc.value()));
    desc.setValue(value);
  }
  if (aDesc.has_writable()) {
    desc.setWritable(aDesc.writable());
  }
  if (aDesc.has_get()) {
    v8::Local<v8::Value> getter = aDesc.get();
    desc.setGetter(getter->IsObject()
                       ? ObjectFromV8(aCx, getter.As<v8::Object>())
                       : nullptr);
  }
  if (aDesc.has_set()) {
    v8::Local<v8::Value> setter = aDesc.set();
    desc.setSetter(setter->IsObject()
                       ? ObjectFromV8(aCx, setter.As<v8::Object>())
                       : nullptr);
  }
  if (aDesc.has_enumerable()) {
    desc.setEnumerable(aDesc.enumerable());
  }
  if (aDesc.has_configurable()) {
    desc.setConfigurable(aDesc.configurable());
  }
  JS::ObjectOpResult result;
  if (!ProxyDefineProperty(aCx, aProxy, aId, desc, result)) {
    ThrowPendingToV8(aCx);
    return;
  }
  if (!result.ok() && aInfo.ShouldThrowOnError()) {
    ReportOpFailure(aCx, aProxy, aId, result);
    ThrowPendingToV8(aCx);
    return;
  }
  aInfo.GetReturnValue().Set(true);
}

void DescriptorCommon(JSContext* aCx, JS::HandleObject aProxy,
                      JS::HandleId aId,
                      const v8::PropertyCallbackInfo<v8::Value>& aInfo) {
  JS::Rooted<mozilla::Maybe<JS::PropertyDescriptor>> desc(aCx);
  if (!ProxyGetOwnPropertyDescriptor(aCx, aProxy, aId, &desc)) {
    ThrowPendingToV8(aCx);
    return;
  }
  if (desc.isNothing()) {
    return;
  }
  JS::Rooted<JS::PropertyDescriptor> complete(aCx, *desc);
  aInfo.GetReturnValue().Set(DescriptorToV8(aCx, complete));
}

void NamedGetter(v8::Local<v8::Name> aName,
                 const v8::PropertyCallbackInfo<v8::Value>& aInfo) {
  JSContext* cx = CurrentContext();
  InterceptorScope scope(cx, aInfo.Holder());
  JS::RootedId id(cx);
  if (!IdFromV8(cx, aName, &id)) {
    ThrowPendingToV8(cx);
    return;
  }
  GetCommon(cx, scope.proxy, id, aInfo);
}

void NamedSetter(v8::Local<v8::Name> aName, v8::Local<v8::Value> aValue,
                 const v8::PropertyCallbackInfo<v8::Value>& aInfo) {
  JSContext* cx = CurrentContext();
  InterceptorScope scope(cx, aInfo.Holder());
  JS::RootedId id(cx);
  if (!IdFromV8(cx, aName, &id)) {
    ThrowPendingToV8(cx);
    return;
  }
  SetCommon(cx, scope.proxy, id, aValue, aInfo);
}

void NamedQuery(v8::Local<v8::Name> aName,
                const v8::PropertyCallbackInfo<v8::Integer>& aInfo) {
  JSContext* cx = CurrentContext();
  InterceptorScope scope(cx, aInfo.Holder());
  JS::RootedId id(cx);
  if (!IdFromV8(cx, aName, &id)) {
    ThrowPendingToV8(cx);
    return;
  }
  QueryCommon(cx, scope.proxy, id, aInfo);
}

void NamedDeleter(v8::Local<v8::Name> aName,
                  const v8::PropertyCallbackInfo<v8::Boolean>& aInfo) {
  JSContext* cx = CurrentContext();
  InterceptorScope scope(cx, aInfo.Holder());
  JS::RootedId id(cx);
  if (!IdFromV8(cx, aName, &id)) {
    ThrowPendingToV8(cx);
    return;
  }
  DeleteCommon(cx, scope.proxy, id, aInfo);
}

void NamedEnumerator(const v8::PropertyCallbackInfo<v8::Array>& aInfo) {
  JSContext* cx = CurrentContext();
  InterceptorScope scope(cx, aInfo.Holder());
  EnumerateCommon(cx, scope.proxy, false, aInfo);
}

void NamedDefiner(v8::Local<v8::Name> aName, const v8::PropertyDescriptor& aDesc,
                  const v8::PropertyCallbackInfo<v8::Value>& aInfo) {
  JSContext* cx = CurrentContext();
  InterceptorScope scope(cx, aInfo.Holder());
  JS::RootedId id(cx);
  if (!IdFromV8(cx, aName, &id)) {
    ThrowPendingToV8(cx);
    return;
  }
  DefineCommon(cx, scope.proxy, id, aDesc, aInfo);
}

void NamedDescriptor(v8::Local<v8::Name> aName,
                     const v8::PropertyCallbackInfo<v8::Value>& aInfo) {
  JSContext* cx = CurrentContext();
  InterceptorScope scope(cx, aInfo.Holder());
  JS::RootedId id(cx);
  if (!IdFromV8(cx, aName, &id)) {
    ThrowPendingToV8(cx);
    return;
  }
  DescriptorCommon(cx, scope.proxy, id, aInfo);
}

void IndexedGetter(uint32_t aIndex,
                   const v8::PropertyCallbackInfo<v8::Value>& aInfo) {
  JSContext* cx = CurrentContext();
  InterceptorScope scope(cx, aInfo.Holder());
  JS::RootedId id(cx);
  if (!IndexToId(cx, aIndex, &id)) {
    ThrowPendingToV8(cx);
    return;
  }
  GetCommon(cx, scope.proxy, id, aInfo);
}

void IndexedSetter(uint32_t aIndex, v8::Local<v8::Value> aValue,
                   const v8::PropertyCallbackInfo<v8::Value>& aInfo) {
  JSContext* cx = CurrentContext();
  InterceptorScope scope(cx, aInfo.Holder());
  JS::RootedId id(cx);
  if (!IndexToId(cx, aIndex, &id)) {
    ThrowPendingToV8(cx);
    return;
  }
  SetCommon(cx, scope.proxy, id, aValue, aInfo);
}

void IndexedQuery(uint32_t aIndex,
                  const v8::PropertyCallbackInfo<v8::Integer>& aInfo) {
  JSContext* cx = CurrentContext();
  InterceptorScope scope(cx, aInfo.Holder());
  JS::RootedId id(cx);
  if (!IndexToId(cx, aIndex, &id)) {
    ThrowPendingToV8(cx);
    return;
  }
  QueryCommon(cx, scope.proxy, id, aInfo);
}

void IndexedDeleter(uint32_t aIndex,
                    const v8::PropertyCallbackInfo<v8::Boolean>& aInfo) {
  JSContext* cx = CurrentContext();
  InterceptorScope scope(cx, aInfo.Holder());
  JS::RootedId id(cx);
  if (!IndexToId(cx, aIndex, &id)) {
    ThrowPendingToV8(cx);
    return;
  }
  DeleteCommon(cx, scope.proxy, id, aInfo);
}

void IndexedEnumerator(const v8::PropertyCallbackInfo<v8::Array>& aInfo) {
  JSContext* cx = CurrentContext();
  InterceptorScope scope(cx, aInfo.Holder());
  EnumerateCommon(cx, scope.proxy, true, aInfo);
}

void IndexedDefiner(uint32_t aIndex, const v8::PropertyDescriptor& aDesc,
                    const v8::PropertyCallbackInfo<v8::Value>& aInfo) {
  JSContext* cx = CurrentContext();
  InterceptorScope scope(cx, aInfo.Holder());
  JS::RootedId id(cx);
  if (!IndexToId(cx, aIndex, &id)) {
    ThrowPendingToV8(cx);
    return;
  }
  DefineCommon(cx, scope.proxy, id, aDesc, aInfo);
}

void IndexedDescriptor(uint32_t aIndex,
                       const v8::PropertyCallbackInfo<v8::Value>& aInfo) {
  JSContext* cx = CurrentContext();
  InterceptorScope scope(cx, aInfo.Holder());
  JS::RootedId id(cx);
  if (!IndexToId(cx, aIndex, &id)) {
    ThrowPendingToV8(cx);
    return;
  }
  DescriptorCommon(cx, scope.proxy, id, aInfo);
}

void CallAsFunction(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  JSContext* cx = CurrentContext();
  InterceptorScope scope(cx, aInfo.Holder());
  JS::HandleObject proxy = scope.proxy;
  unsigned argc = unsigned(aInfo.Length());
  bool constructing = aInfo.IsConstructCall();
  JS::RootedValueVector vp(cx);
  if (!vp.resize(argc + 3)) {
    ThrowPendingToV8(cx);
    return;
  }
  vp[0].setObject(*proxy);
  if (constructing) {
    vp[1].set(JS::MagicValue(JS_IS_CONSTRUCTING));
  } else {
    vp[1].setUndefined();
  }
  for (unsigned i = 0; i < argc; i++) {
    vp[2 + i].set(ValueFromV8(cx, aInfo[int(i)]));
  }
  if (constructing) {
    v8::Local<v8::Value> newTarget = aInfo.NewTarget();
    if (newTarget->IsObject()) {
      vp[2 + argc].set(ValueFromV8(cx, newTarget));
    } else {
      vp[2 + argc].setObject(*proxy);
    }
  }
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp.begin());
  JS::RootedValue callee(cx, JS::ObjectValue(*proxy));
  const js::BaseProxyHandler* handler = HandlerOf(proxy);
  if (constructing) {
    if (!handler->isConstructor(proxy)) {
      ReportNotFunction(cx, callee, JSMSG_NOT_CONSTRUCTOR);
      ThrowPendingToV8(cx);
      return;
    }
    if (!ProxyConstruct(cx, proxy, args)) {
      ThrowPendingToV8(cx);
      return;
    }
  } else {
    if (!handler->isCallable(proxy)) {
      ReportNotFunction(cx, callee, JSMSG_NOT_FUNCTION);
      ThrowPendingToV8(cx);
      return;
    }
    if (!ProxyCall(cx, proxy, args)) {
      ThrowPendingToV8(cx);
      return;
    }
  }
  aInfo.GetReturnValue().Set(ToV8(cx, args.rval().get()));
}

v8::Local<v8::ObjectTemplate> ProxyTemplate(JSContext* aCx, bool aCallable) {
  RuntimeState& st = State();
  v8::Global<v8::ObjectTemplate>& slot =
      aCallable ? st.callableTemplate : st.plainTemplate;
  if (slot.IsEmpty()) {
    v8::Local<v8::ObjectTemplate> tpl = v8::ObjectTemplate::New(aCx->isolate);
    tpl->SetInternalFieldCount(2);
    tpl->SetHandler(v8::NamedPropertyHandlerConfiguration(
        NamedGetter, NamedSetter, NamedQuery, NamedDeleter, NamedEnumerator,
        NamedDefiner, NamedDescriptor));
    tpl->SetHandler(v8::IndexedPropertyHandlerConfiguration(
        IndexedGetter, IndexedSetter, IndexedQuery, IndexedDeleter,
        IndexedEnumerator, IndexedDefiner, IndexedDescriptor));
    if (aCallable) {
      tpl->SetCallAsFunctionHandler(CallAsFunction);
    }
    slot.Reset(aCx->isolate, tpl);
  }
  return slot.Get(aCx->isolate);
}

void ProxyFinalize(JS::GCContext* aGcx, JSObject* aObj) {
  const js::BaseProxyHandler* handler = js::GetProxyHandler(aObj);
  if (handler) {
    handler->finalize(aGcx, aObj);
  }
  if (!Runtime()) {
    return;
  }
  RuntimeState& st = State();
  ObjectCell* cell = ObjectCell::from(aObj);
  st.proxies.erase(cell);
  st.lazyPrivateSlots.erase(&cell->proxyData()->values()->privateSlot);
  st.syncingPrototype.erase(cell);
}

void ProxyTrace(JSTracer* aTrc, JSObject* aObj) {
  js::detail::ProxyDataLayout* data = js::detail::GetProxyDataLayout(aObj);
  if (data->expando) {
    JS::TraceRoot(aTrc, &data->expando, "expando");
  }
  JS::Value* slots = &data->values()->privateSlot;
  size_t count = JSCLASS_RESERVED_SLOTS(JS::GetClass(aObj)) + 1;
  for (size_t i = 0; i < count; i++) {
    if (slots[i].isGCThing()) {
      JS::TraceEdge(aTrc, reinterpret_cast<JS::Heap<JS::Value>*>(&slots[i]),
                    "proxy slot");
    }
  }
  if (data->handler) {
    data->handler->trace(aTrc, aObj);
  }
}

size_t ProxyObjectMoved(JSObject* aObj, JSObject* aOld) {
  return js::GetProxyHandler(aObj)->objectMoved(aObj, aOld);
}

bool ProxyLookupProperty(JSContext* aCx, JS::HandleObject aObj,
                         JS::HandleId aId, JS::MutableHandleObject aObjp,
                         js::PropertyResult* aPropp) {
  bool found = false;
  if (!ProxyHas(aCx, aObj, aId, &found)) {
    return false;
  }
  if (found) {
    aPropp->setProxyProperty();
    aObjp.set(aObj);
  } else {
    aPropp->setNotFound();
    aObjp.set(nullptr);
  }
  return true;
}

}

bool ProxyGetOwnPropertyDescriptor(
    JSContext* aCx, JS::HandleObject aProxy, JS::HandleId aId,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> aDesc) {
  const js::BaseProxyHandler* handler = HandlerOf(aProxy);
  aDesc.reset();
  Policy policy = EnterPolicy(aCx, handler, aProxy, aId,
                              js::BaseProxyHandler::GET_PROPERTY_DESCRIPTOR,
                              true);
  if (!policy.allowed) {
    return policy.rv;
  }
  return handler->getOwnPropertyDescriptor(aCx, aProxy, aId, aDesc);
}

bool ProxyDefineProperty(JSContext* aCx, JS::HandleObject aProxy,
                         JS::HandleId aId,
                         JS::Handle<JS::PropertyDescriptor> aDesc,
                         JS::ObjectOpResult& aResult) {
  const js::BaseProxyHandler* handler = HandlerOf(aProxy);
  Policy policy =
      EnterPolicy(aCx, handler, aProxy, aId, js::BaseProxyHandler::SET, true);
  if (!policy.allowed) {
    if (!policy.rv) {
      return false;
    }
    return aResult.succeed();
  }
  return handler->defineProperty(aCx, aProxy, aId, aDesc, aResult);
}

bool ProxyOwnPropertyKeys(JSContext* aCx, JS::HandleObject aProxy,
                          JS::MutableHandleIdVector aProps) {
  const js::BaseProxyHandler* handler = HandlerOf(aProxy);
  JS::RootedId voidId(aCx, JS::PropertyKey::Void());
  Policy policy = EnterPolicy(aCx, handler, aProxy, voidId,
                              js::BaseProxyHandler::ENUMERATE, true);
  if (!policy.allowed) {
    return policy.rv;
  }
  return handler->ownPropertyKeys(aCx, aProxy, aProps);
}

bool ProxyDelete(JSContext* aCx, JS::HandleObject aProxy, JS::HandleId aId,
                 JS::ObjectOpResult& aResult) {
  const js::BaseProxyHandler* handler = HandlerOf(aProxy);
  Policy policy =
      EnterPolicy(aCx, handler, aProxy, aId, js::BaseProxyHandler::SET, true);
  if (!policy.allowed) {
    if (policy.rv) {
      aResult.succeed();
    }
    return policy.rv;
  }
  return handler->delete_(aCx, aProxy, aId, aResult);
}

bool ProxyHas(JSContext* aCx, JS::HandleObject aProxy, JS::HandleId aId,
              bool* aFound) {
  const js::BaseProxyHandler* handler = HandlerOf(aProxy);
  *aFound = false;
  Policy policy =
      EnterPolicy(aCx, handler, aProxy, aId, js::BaseProxyHandler::GET, true);
  if (!policy.allowed) {
    return policy.rv;
  }
  if (handler->hasPrototype()) {
    if (!handler->hasOwn(aCx, aProxy, aId, aFound)) {
      return false;
    }
    if (*aFound) {
      return true;
    }
    JS::RootedObject proto(aCx);
    if (!GetPrototype(aCx, aProxy, &proto)) {
      return false;
    }
    if (!proto) {
      return true;
    }
    return HasProperty(aCx, proto, aId, aFound);
  }
  return handler->has(aCx, aProxy, aId, aFound);
}

bool ProxyHasOwn(JSContext* aCx, JS::HandleObject aProxy, JS::HandleId aId,
                 bool* aFound) {
  const js::BaseProxyHandler* handler = HandlerOf(aProxy);
  *aFound = false;
  Policy policy =
      EnterPolicy(aCx, handler, aProxy, aId, js::BaseProxyHandler::GET, true);
  if (!policy.allowed) {
    return policy.rv;
  }
  return handler->hasOwn(aCx, aProxy, aId, aFound);
}

bool ProxyGet(JSContext* aCx, JS::HandleObject aProxy,
              JS::HandleValue aReceiver, JS::HandleId aId,
              JS::MutableHandleValue aVp) {
  JS::RootedValue receiver(aCx, WindowProxyIfWindow(aReceiver, aProxy));
  const js::BaseProxyHandler* handler = HandlerOf(aProxy);
  aVp.setUndefined();
  Policy policy =
      EnterPolicy(aCx, handler, aProxy, aId, js::BaseProxyHandler::GET, true);
  if (!policy.allowed) {
    return policy.rv;
  }
  if (handler->hasPrototype()) {
    bool own = false;
    if (!handler->hasOwn(aCx, aProxy, aId, &own)) {
      return false;
    }
    if (!own) {
      JS::RootedObject proto(aCx);
      if (!GetPrototype(aCx, aProxy, &proto)) {
        return false;
      }
      if (!proto) {
        return true;
      }
      return GetProperty(aCx, proto, receiver, aId, aVp);
    }
  }
  return handler->get(aCx, aProxy, receiver, aId, aVp);
}

bool ProxySet(JSContext* aCx, JS::HandleObject aProxy, JS::HandleId aId,
              JS::HandleValue aValue, JS::HandleValue aReceiver,
              JS::ObjectOpResult& aResult) {
  JS::RootedValue receiver(aCx, WindowProxyIfWindow(aReceiver, aProxy));
  const js::BaseProxyHandler* handler = HandlerOf(aProxy);
  Policy policy =
      EnterPolicy(aCx, handler, aProxy, aId, js::BaseProxyHandler::SET, true);
  if (!policy.allowed) {
    if (!policy.rv) {
      return false;
    }
    return aResult.succeed();
  }
  if (handler->hasPrototype()) {
    return handler->BaseProxyHandler::set(aCx, aProxy, aId, aValue, receiver,
                                          aResult);
  }
  return handler->set(aCx, aProxy, aId, aValue, receiver, aResult);
}

bool ProxyCall(JSContext* aCx, JS::HandleObject aProxy,
               const JS::CallArgs& aArgs) {
  const js::BaseProxyHandler* handler = HandlerOf(aProxy);
  JS::RootedId voidId(aCx, JS::PropertyKey::Void());
  Policy policy = EnterPolicy(aCx, handler, aProxy, voidId,
                              js::BaseProxyHandler::CALL, true);
  if (!policy.allowed) {
    aArgs.rval().setUndefined();
    return policy.rv;
  }
  return handler->call(aCx, aProxy, aArgs);
}

bool ProxyConstruct(JSContext* aCx, JS::HandleObject aProxy,
                    const JS::CallArgs& aArgs) {
  const js::BaseProxyHandler* handler = HandlerOf(aProxy);
  JS::RootedId voidId(aCx, JS::PropertyKey::Void());
  Policy policy = EnterPolicy(aCx, handler, aProxy, voidId,
                              js::BaseProxyHandler::CALL, true);
  if (!policy.allowed) {
    aArgs.rval().setUndefined();
    return policy.rv;
  }
  return handler->construct(aCx, aProxy, aArgs);
}

bool ProxyGetPrototype(JSContext* aCx, JS::HandleObject aProxy,
                       JS::MutableHandleObject aProto) {
  return HandlerOf(aProxy)->getPrototype(aCx, aProxy, aProto);
}

bool ProxySetPrototype(JSContext* aCx, JS::HandleObject aProxy,
                       JS::HandleObject aProto, JS::ObjectOpResult& aResult) {
  return HandlerOf(aProxy)->setPrototype(aCx, aProxy, aProto, aResult);
}

bool ProxyGetPrototypeIfOrdinary(JSContext* aCx, JS::HandleObject aProxy,
                                 bool* aIsOrdinary,
                                 JS::MutableHandleObject aProto) {
  return HandlerOf(aProxy)->getPrototypeIfOrdinary(aCx, aProxy, aIsOrdinary,
                                                   aProto);
}

bool ProxySetImmutablePrototype(JSContext* aCx, JS::HandleObject aProxy,
                                bool* aSucceeded) {
  return HandlerOf(aProxy)->setImmutablePrototype(aCx, aProxy, aSucceeded);
}

bool ProxyPreventExtensions(JSContext* aCx, JS::HandleObject aProxy,
                            JS::ObjectOpResult& aResult) {
  return HandlerOf(aProxy)->preventExtensions(aCx, aProxy, aResult);
}

bool ProxyIsExtensible(JSContext* aCx, JS::HandleObject aProxy,
                       bool* aExtensible) {
  return HandlerOf(aProxy)->isExtensible(aCx, aProxy, aExtensible);
}

bool ProxyGetOwnEnumerablePropertyKeys(JSContext* aCx, JS::HandleObject aProxy,
                                       JS::MutableHandleIdVector aProps) {
  const js::BaseProxyHandler* handler = HandlerOf(aProxy);
  JS::RootedId voidId(aCx, JS::PropertyKey::Void());
  Policy policy = EnterPolicy(aCx, handler, aProxy, voidId,
                              js::BaseProxyHandler::ENUMERATE, true);
  if (!policy.allowed) {
    return policy.rv;
  }
  return handler->getOwnEnumerablePropertyKeys(aCx, aProxy, aProps);
}

bool ProxyEnumerate(JSContext* aCx, JS::HandleObject aProxy,
                    JS::MutableHandleIdVector aProps) {
  const js::BaseProxyHandler* handler = HandlerOf(aProxy);
  if (handler->hasPrototype()) {
    if (!ProxyGetOwnEnumerablePropertyKeys(aCx, aProxy, aProps)) {
      return false;
    }
    JS::RootedObject proto(aCx);
    if (!GetPrototype(aCx, aProxy, &proto)) {
      return false;
    }
    if (!proto) {
      return true;
    }
    return ForInKeys(aCx, proto, aProps);
  }
  JS::RootedId voidId(aCx, JS::PropertyKey::Void());
  Policy policy = EnterPolicy(aCx, handler, aProxy, voidId,
                              js::BaseProxyHandler::ENUMERATE, true);
  if (!policy.allowed) {
    return policy.rv;
  }
  return handler->enumerate(aCx, aProxy, aProps);
}

const char* ProxyClassName(JSContext* aCx, JS::HandleObject aProxy) {
  const js::BaseProxyHandler* handler = HandlerOf(aProxy);
  JS::RootedId voidId(aCx, JS::PropertyKey::Void());
  Policy policy = EnterPolicy(aCx, handler, aProxy, voidId,
                              js::BaseProxyHandler::GET, false);
  if (!policy.allowed) {
    return handler->BaseProxyHandler::className(aCx, aProxy);
  }
  return handler->className(aCx, aProxy);
}

JSString* ProxyFunToString(JSContext* aCx, JS::HandleObject aProxy,
                           bool aIsToSource) {
  const js::BaseProxyHandler* handler = HandlerOf(aProxy);
  JS::RootedId voidId(aCx, JS::PropertyKey::Void());
  Policy policy = EnterPolicy(aCx, handler, aProxy, voidId,
                              js::BaseProxyHandler::GET, false);
  if (!policy.allowed) {
    return handler->BaseProxyHandler::fun_toString(aCx, aProxy, aIsToSource);
  }
  return handler->fun_toString(aCx, aProxy, aIsToSource);
}

bool ProxyGetElements(JSContext* aCx, JS::HandleObject aProxy, uint32_t aBegin,
                      uint32_t aEnd, js::ElementAdder* aAdder) {
  const js::BaseProxyHandler* handler = HandlerOf(aProxy);
  JS::RootedId voidId(aCx, JS::PropertyKey::Void());
  Policy policy = EnterPolicy(aCx, handler, aProxy, voidId,
                              js::BaseProxyHandler::GET, true);
  if (!policy.allowed) {
    if (policy.rv) {
      return js::GetElementsWithAdder(aCx, aProxy, aProxy, aBegin, aEnd,
                                      aAdder);
    }
    return false;
  }
  return handler->getElements(aCx, aProxy, aBegin, aEnd, aAdder);
}

bool ProxyGetBuiltinClass(JSContext* aCx, JS::HandleObject aProxy,
                          js::ESClass* aCls) {
  return HandlerOf(aProxy)->getBuiltinClass(aCx, aProxy, aCls);
}

bool ProxyIsArray(JSContext* aCx, JS::HandleObject aProxy,
                  JS::IsArrayAnswer* aAnswer) {
  return HandlerOf(aProxy)->isArray(aCx, aProxy, aAnswer);
}

bool ProxyBoxedValueUnbox(JSContext* aCx, JS::HandleObject aProxy,
                          JS::MutableHandleValue aVp) {
  return HandlerOf(aProxy)->boxedValue_unbox(aCx, aProxy, aVp);
}

void SyncLazyPrototype(JSContext* aCx, JSObject* aProxy) {
  RuntimeState& st = State();
  if (!aCx || (st.runtime && st.runtime->inGC)) {
    return;
  }
  ObjectCell* cell = ObjectCell::from(aProxy);
  if (!st.syncingPrototype.insert(cell).second) {
    return;
  }
  JS::RootedObject proxy(aCx, aProxy);
  {
    AutoEnterRealmOf realm(aCx, proxy.get());
    bool wasPending = aCx->exceptionPending;
    JS::RootedValue savedException(aCx, aCx->pendingException);
    JS::RootedObject proto(aCx);
    if (ProxyGetPrototype(aCx, proxy, &proto)) {
      AutoV8 v8(aCx);
      v8::TryCatch tryCatch(aCx->isolate);
      v8::Local<v8::Value> protoV8 =
          proto ? v8::Local<v8::Value>(ToV8(aCx, proto.get()))
                : v8::Local<v8::Value>(v8::Null(aCx->isolate));
      v8::Maybe<bool> set =
          ToV8(aCx, proxy.get())->SetPrototype(v8.context(), protoV8);
      (void)set.IsJust();
    }
    aCx->exceptionPending = wasPending;
    aCx->pendingException = savedException;
  }
  st.syncingPrototype.erase(cell);
}

void SwapProxies(JSContext* aCx, JS::HandleObject aA, JS::HandleObject aB) {
  RuntimeState& st = State();
  ObjectCell* a = ObjectCell::from(aA);
  ObjectCell* b = ObjectCell::from(aB);
  AutoV8 v8(aCx);
  v8::Local<v8::Object> va = ToV8(aCx, aA.get());
  v8::Local<v8::Object> vb = ToV8(aCx, aB.get());
  v8::Local<v8::Value> protoA = va->GetPrototype();
  v8::Local<v8::Value> protoB = vb->GetPrototype();
  std::swap(a->shape, b->shape);
  std::swap(a->words, b->words);
  ProxyRecord& ra = st.proxies[a];
  ProxyRecord& rb = st.proxies[b];
  std::swap(ra.lazyProto, rb.lazyProto);
  const JS::Value* slotA = &a->proxyData()->values()->privateSlot;
  const JS::Value* slotB = &b->proxyData()->values()->privateSlot;
  st.lazyPrivateSlots.erase(slotA);
  st.lazyPrivateSlots.erase(slotB);
  if (ra.lazyProto) {
    st.lazyPrivateSlots[slotA] = a;
  }
  if (rb.lazyProto) {
    st.lazyPrivateSlots[slotB] = b;
  }
  {
    v8::TryCatch tryCatch(aCx->isolate);
    if (!ra.lazyProto) {
      v8::Maybe<bool> set = va->SetPrototype(v8.context(), protoB);
      (void)set.IsJust();
    }
    if (!rb.lazyProto) {
      v8::Maybe<bool> set = vb->SetPrototype(v8.context(), protoA);
      (void)set.IsJust();
    }
  }
  if (ra.lazyProto) {
    SyncLazyPrototype(aCx, aA);
  }
  if (rb.lazyProto) {
    SyncLazyPrototype(aCx, aB);
  }
}

}

using namespace vge::jsapi;
using namespace vge::jsapi::proxy;

JSObject* vge::jsapi::NewProxyCell(JSContext* aCx,
                                   const js::BaseProxyHandler* aHandler,
                                   JS::HandleValue aPrivate,
                                   JS::HandleObject aProto,
                                   const JSClass* aClass) {
  Shape* shape = ShapeFor(aClass, aCx->realm());
  bool lazy = aProto.get() == kLazyProto;
  bool callable = false;
  {
    ObjectCell probe{};
    probe.shape = shape;
    probe.proxyData()->init(aHandler, shape->reservedSlots);
    probe.proxyData()->values()->privateSlot = aPrivate.get();
    callable = aHandler->isCallable(probe.asObject());
  }
  AutoV8 v8(aCx);
  v8::TryCatch tryCatch(aCx->isolate);
  v8::Local<v8::Object> obj;
  if (!ProxyTemplate(aCx, callable)->NewInstance(v8.context()).ToLocal(&obj)) {
    v8.Check(tryCatch);
    return nullptr;
  }
  ObjectCell* cell = NewObjectCell(aCx, shape, obj, true);
  cell->proxyData()->handler = aHandler;
  cell->proxyData()->expando = nullptr;
  cell->proxyData()->values()->privateSlot = aPrivate.get();
  RuntimeState& st = State();
  ProxyRecord record;
  record.lazyProto = lazy;
  record.callable = callable;
  st.proxies[cell] = record;
  if (lazy) {
    st.lazyPrivateSlots[&cell->proxyData()->values()->privateSlot] = cell;
  }
  v8::Local<v8::Value> protoV8 =
      (!lazy && aProto) ? v8::Local<v8::Value>(ToV8(aCx, aProto.get()))
                        : v8::Local<v8::Value>(v8::Null(aCx->isolate));
  v8::Maybe<bool> set = obj->SetPrototype(v8.context(), protoV8);
  if (set.IsNothing()) {
    v8.Check(tryCatch);
    return nullptr;
  }
  JSObject* result = cell->asObject();
  if (lazy) {
    SyncLazyPrototype(aCx, result);
  }
  return result;
}

JS_PUBLIC_API JSObject* js::NewProxyObject(JSContext* cx,
                                           const BaseProxyHandler* handler,
                                           JS::HandleValue priv,
                                           JSObject* proto_,
                                           const ProxyOptions& options) {
  if (options.lazyProto() || proto_ == kLazyProto) {
    JSObject* marker = kLazyProto;
    return NewProxyCell(cx, handler, priv,
                        JS::Handle<JSObject*>::fromMarkedLocation(&marker),
                        options.clasp());
  }
  JS::RootedObject proto(cx, proto_);
  return NewProxyCell(cx, handler, priv, proto, options.clasp());
}

JS_PUBLIC_API void js::detail::SetValueInProxy(JS::Value* slot,
                                               const JS::Value& value) {
  bool changed = *slot != value;
  *slot = value;
  if (!changed || !Runtime()) {
    return;
  }
  RuntimeState& st = State();
  auto it = st.lazyPrivateSlots.find(slot);
  if (it == st.lazyPrivateSlots.end()) {
    return;
  }
  SyncLazyPrototype(CurrentContext(), it->second->asObject());
}

JS_PUBLIC_API void js::NukeNonCCWProxy(JSContext* cx, JS::HandleObject proxy) {
  js::GetProxyHandler(proxy)->finalize(
      reinterpret_cast<JS::GCContext*>(cx->runtime), proxy);
  NukeProxy(cx, proxy);
}

const JSClassOps js::ProxyClassOps = {
    .finalize = ProxyFinalize,
    .trace = ProxyTrace,
};

const js::ClassExtension js::ProxyClassExtension = {
    ProxyObjectMoved,
};

const js::ObjectOps js::ProxyObjectOps = {
    ProxyLookupProperty,
    vge::jsapi::proxy::ProxyDefineProperty,
    vge::jsapi::proxy::ProxyHas,
    vge::jsapi::proxy::ProxyGet,
    vge::jsapi::proxy::ProxySet,
    vge::jsapi::proxy::ProxyGetOwnPropertyDescriptor,
    vge::jsapi::proxy::ProxyDelete,
    vge::jsapi::proxy::ProxyGetElements,
    vge::jsapi::proxy::ProxyFunToString,
};

const JSClass js::ProxyClass = PROXY_CLASS_DEF_WITH_CLASS_SPEC(
    "Proxy",
    JSCLASS_HAS_CACHED_PROTO(JSProto_Proxy) |
        JSCLASS_HAS_RESERVED_SLOTS(js::SwappableProxyReservedSlots),
    JS_NULL_CLASS_SPEC);

void vge::jsapi::ForgetRealmProxies(JS::Realm* aRealm) {
  State().nukedIncoming.erase(aRealm);
}

void vge::jsapi::ForgetCompartmentProxies(JS::Compartment* aCompartment) {
  State().nukedOutgoing.erase(aCompartment);
}
