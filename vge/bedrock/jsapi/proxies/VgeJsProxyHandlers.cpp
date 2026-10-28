#include "VgeJsProxyInternal.h"

#include "js/Object.h"
#include "js/PropertyDescriptor.h"

namespace vp = vge::jsapi::proxy;
using vp::AutoEnterRealmOf;

using JS::CallArgs;
using JS::HandleId;
using JS::HandleObject;
using JS::HandleValue;
using JS::MutableHandle;
using JS::MutableHandleIdVector;
using JS::MutableHandleObject;
using JS::MutableHandleValue;
using JS::ObjectOpResult;
using JS::PropertyDescriptor;

namespace {

JSObject* TargetOf(JSObject* aProxy) { return js::GetProxyTargetObject(aProxy); }

bool WrapIds(JSContext* aCx, MutableHandleIdVector aIds) {
  for (size_t i = 0; i < aIds.length(); i++) {
    if (!JS_WrapId(aCx, aIds[i])) {
      return false;
    }
  }
  return true;
}

bool WrapReceiver(JSContext* aCx, HandleObject aWrapper,
                  MutableHandleValue aReceiver) {
  if (JS::ObjectValue(*aWrapper) == aReceiver) {
    JSObject* wrapped = js::Wrapper::wrappedObject(aWrapper);
    if (!js::IsWrapper(wrapped)) {
      aReceiver.setObject(*wrapped);
      return true;
    }
  }
  return JS_WrapValue(aCx, aReceiver);
}

js::RegExpShared* ReportNoRegExpShared(JSContext* aCx) {
  vp::ReportErrorNumber(aCx, JSMSG_UNEXPECTED_TYPE, {"object", "not a RegExp"});
  return nullptr;
}

}

bool js::BaseProxyHandler::enter(JSContext* cx, HandleObject wrapper,
                                 HandleId id, Action act, bool mayThrow,
                                 bool* bp) const {
  *bp = true;
  return true;
}

bool js::BaseProxyHandler::has(JSContext* cx, HandleObject proxy, HandleId id,
                               bool* bp) const {
  if (!hasOwn(cx, proxy, id, bp)) {
    return false;
  }
  if (*bp) {
    return true;
  }
  JS::RootedObject proto(cx);
  if (!vp::GetPrototype(cx,proxy, &proto)) {
    return false;
  }
  if (proto) {
    return vp::HasProperty(cx, proto, id, bp);
  }
  *bp = false;
  return true;
}

bool js::BaseProxyHandler::hasOwn(JSContext* cx, HandleObject proxy,
                                  HandleId id, bool* bp) const {
  JS::Rooted<mozilla::Maybe<PropertyDescriptor>> desc(cx);
  if (!getOwnPropertyDescriptor(cx, proxy, id, &desc)) {
    return false;
  }
  *bp = desc.isSome();
  return true;
}

bool js::BaseProxyHandler::get(JSContext* cx, HandleObject proxy,
                               HandleValue receiver, HandleId id,
                               MutableHandleValue vp) const {
  JS::Rooted<mozilla::Maybe<PropertyDescriptor>> desc(cx);
  if (!getOwnPropertyDescriptor(cx, proxy, id, &desc)) {
    return false;
  }
  if (desc.isNothing()) {
    JS::RootedObject proto(cx);
    if (!vp::GetPrototype(cx,proxy, &proto)) {
      return false;
    }
    if (!proto) {
      vp.setUndefined();
      return true;
    }
    return vp::GetProperty(cx, proto, receiver, id, vp);
  }
  if (desc->isDataDescriptor()) {
    vp.set(desc->value());
    return true;
  }
  JS::RootedObject getter(cx, desc->getter());
  if (!getter) {
    vp.setUndefined();
    return true;
  }
  return vp::CallGetter(cx, receiver, getter, vp);
}

bool js::BaseProxyHandler::set(JSContext* cx, HandleObject proxy, HandleId id,
                               HandleValue v, HandleValue receiver,
                               ObjectOpResult& result) const {
  JS::Rooted<mozilla::Maybe<PropertyDescriptor>> ownDesc(cx);
  if (!getOwnPropertyDescriptor(cx, proxy, id, &ownDesc)) {
    return false;
  }
  return vp::SetPropertyIgnoringNamedGetter(cx, proxy, id, v, receiver, ownDesc,
                                        result);
}

bool js::BaseProxyHandler::getOwnEnumerablePropertyKeys(
    JSContext* cx, HandleObject proxy, MutableHandleIdVector props) const {
  if (!ownPropertyKeys(cx, proxy, props)) {
    return false;
  }
  JS::RootedId id(cx);
  JS::Rooted<mozilla::Maybe<PropertyDescriptor>> desc(cx);
  size_t i = 0;
  for (size_t j = 0, len = props.length(); j < len; j++) {
    id = props[j];
    if (id.isSymbol()) {
      continue;
    }
    if (!getOwnPropertyDescriptor(cx, proxy, id, &desc)) {
      desc.reset();
      return false;
    }
    if (desc.isSome() && desc->enumerable()) {
      props[i++].set(id);
    }
  }
  return props.resize(i);
}

bool js::BaseProxyHandler::enumerate(JSContext* cx, HandleObject proxy,
                                     MutableHandleIdVector props) const {
  return vp::ForInKeys(cx, proxy, props);
}

bool js::BaseProxyHandler::call(JSContext* cx, HandleObject proxy,
                                const CallArgs& args) const {
  MOZ_CRASH("callable proxies should implement call trap");
}

bool js::BaseProxyHandler::construct(JSContext* cx, HandleObject proxy,
                                     const CallArgs& args) const {
  MOZ_CRASH("callable proxies should implement construct trap");
}

const char* js::BaseProxyHandler::className(JSContext* cx,
                                            HandleObject proxy) const {
  return isCallable(proxy) ? "Function" : "Object";
}

JSString* js::BaseProxyHandler::fun_toString(JSContext* cx, HandleObject proxy,
                                             bool isToSource) const {
  if (isCallable(proxy)) {
    return JS_NewStringCopyZ(cx, "function () {\n    [native code]\n}");
  }
  vp::ReportErrorNumber(cx, JSMSG_INCOMPATIBLE_PROTO,
                    {"Function", "toString", "object"});
  return nullptr;
}

js::RegExpShared* js::BaseProxyHandler::regexp_toShared(
    JSContext* cx, HandleObject proxy) const {
  MOZ_CRASH("This should have been a wrapped regexp");
}

bool js::BaseProxyHandler::boxedValue_unbox(JSContext* cx, HandleObject proxy,
                                            MutableHandleValue vp) const {
  vp.setUndefined();
  return true;
}

bool js::BaseProxyHandler::nativeCall(JSContext* cx, JS::IsAcceptableThis test,
                                      JS::NativeImpl impl,
                                      const CallArgs& args) const {
  return vp::ReportIncompatible(cx, args);
}

bool js::BaseProxyHandler::getBuiltinClass(JSContext* cx, HandleObject proxy,
                                           ESClass* cls) const {
  *cls = ESClass::Other;
  return true;
}

bool js::BaseProxyHandler::isArray(JSContext* cx, HandleObject proxy,
                                   JS::IsArrayAnswer* answer) const {
  *answer = JS::IsArrayAnswer::NotArray;
  return true;
}

void js::BaseProxyHandler::trace(JSTracer* trc, JSObject* proxy) const {}

void js::BaseProxyHandler::finalize(JS::GCContext* gcx, JSObject* proxy) const {
}

size_t js::BaseProxyHandler::objectMoved(JSObject* proxy, JSObject* old) const {
  return 0;
}

bool js::BaseProxyHandler::getPrototype(JSContext* cx, HandleObject proxy,
                                        MutableHandleObject protop) const {
  MOZ_CRASH("must override getPrototype with dynamic prototype");
}

bool js::BaseProxyHandler::setPrototype(JSContext* cx, HandleObject proxy,
                                        HandleObject proto,
                                        ObjectOpResult& result) const {
  vp::ReportErrorNumber(cx, JSMSG_CANT_SET_PROTO_OF, {"incompatible Proxy"});
  return false;
}

bool js::BaseProxyHandler::setImmutablePrototype(JSContext* cx,
                                                 HandleObject proxy,
                                                 bool* succeeded) const {
  *succeeded = false;
  return true;
}

bool js::BaseProxyHandler::getElements(JSContext* cx, HandleObject proxy,
                                       uint32_t begin, uint32_t end,
                                       ElementAdder* adder) const {
  return js::GetElementsWithAdder(cx, proxy, proxy, begin, end, adder);
}

bool js::BaseProxyHandler::isCallable(JSObject* obj) const { return false; }

bool js::BaseProxyHandler::isConstructor(JSObject* obj) const { return false; }

bool js::ForwardingProxyHandler::getOwnPropertyDescriptor(
    JSContext* cx, HandleObject proxy, HandleId id,
    MutableHandle<mozilla::Maybe<PropertyDescriptor>> desc) const {
  JS::RootedObject target(cx, TargetOf(proxy));
  return vp::GetOwnPropertyDescriptor(cx, target, id, desc);
}

bool js::ForwardingProxyHandler::defineProperty(
    JSContext* cx, HandleObject proxy, HandleId id,
    JS::Handle<PropertyDescriptor> desc, ObjectOpResult& result) const {
  JS::RootedObject target(cx, TargetOf(proxy));
  return vp::DefineProperty(cx, target, id, desc, result);
}

bool js::ForwardingProxyHandler::ownPropertyKeys(
    JSContext* cx, HandleObject proxy, MutableHandleIdVector props) const {
  JS::RootedObject target(cx, TargetOf(proxy));
  return vp::OwnPropertyKeys(cx, target, props);
}

bool js::ForwardingProxyHandler::delete_(JSContext* cx, HandleObject proxy,
                                         HandleId id,
                                         ObjectOpResult& result) const {
  JS::RootedObject target(cx, TargetOf(proxy));
  return vp::DeleteProperty(cx, target, id, result);
}

bool js::ForwardingProxyHandler::enumerate(JSContext* cx, HandleObject proxy,
                                           MutableHandleIdVector props) const {
  JS::RootedObject target(cx, TargetOf(proxy));
  return vp::ForInKeys(cx, target, props);
}

bool js::ForwardingProxyHandler::getPrototype(
    JSContext* cx, HandleObject proxy, MutableHandleObject protop) const {
  JS::RootedObject target(cx, TargetOf(proxy));
  return vp::GetPrototype(cx,target, protop);
}

bool js::ForwardingProxyHandler::setPrototype(JSContext* cx,
                                              HandleObject proxy,
                                              HandleObject proto,
                                              ObjectOpResult& result) const {
  JS::RootedObject target(cx, TargetOf(proxy));
  return vp::SetPrototype(cx, target, proto, result);
}

bool js::ForwardingProxyHandler::getPrototypeIfOrdinary(
    JSContext* cx, HandleObject proxy, bool* isOrdinary,
    MutableHandleObject protop) const {
  JS::RootedObject target(cx, TargetOf(proxy));
  return vp::GetPrototypeIfOrdinary(cx,target, isOrdinary, protop);
}

bool js::ForwardingProxyHandler::setImmutablePrototype(JSContext* cx,
                                                       HandleObject proxy,
                                                       bool* succeeded) const {
  JS::RootedObject target(cx, TargetOf(proxy));
  return vp::SetImmutablePrototype(cx, target, succeeded);
}

bool js::ForwardingProxyHandler::preventExtensions(
    JSContext* cx, HandleObject proxy, ObjectOpResult& result) const {
  JS::RootedObject target(cx, TargetOf(proxy));
  return vp::PreventExtensions(cx, target, result);
}

bool js::ForwardingProxyHandler::isExtensible(JSContext* cx, HandleObject proxy,
                                              bool* extensible) const {
  JS::RootedObject target(cx, TargetOf(proxy));
  return vp::IsExtensible(cx, target, extensible);
}

bool js::ForwardingProxyHandler::has(JSContext* cx, HandleObject proxy,
                                     HandleId id, bool* bp) const {
  JS::RootedObject target(cx, TargetOf(proxy));
  return vp::HasProperty(cx, target, id, bp);
}

bool js::ForwardingProxyHandler::get(JSContext* cx, HandleObject proxy,
                                     HandleValue receiver, HandleId id,
                                     MutableHandleValue vp) const {
  JS::RootedObject target(cx, TargetOf(proxy));
  return vp::GetProperty(cx, target, receiver, id, vp);
}

bool js::ForwardingProxyHandler::set(JSContext* cx, HandleObject proxy,
                                     HandleId id, HandleValue v,
                                     HandleValue receiver,
                                     ObjectOpResult& result) const {
  JS::RootedObject target(cx, TargetOf(proxy));
  return vp::SetProperty(cx, target, id, v, receiver, result);
}

bool js::ForwardingProxyHandler::call(JSContext* cx, HandleObject proxy,
                                      const CallArgs& args) const {
  JS::RootedValue target(cx, js::GetProxyPrivate(proxy));
  return vp::Call(cx, target, args.thisv(), args, args.rval());
}

bool js::ForwardingProxyHandler::construct(JSContext* cx, HandleObject proxy,
                                           const CallArgs& args) const {
  JS::RootedValue target(cx, js::GetProxyPrivate(proxy));
  if (!target.isObject() || !vp::IsConstructorObject(&target.toObject())) {
    vp::ReportNotFunction(cx, target, JSMSG_NOT_CONSTRUCTOR);
    return false;
  }
  JS::RootedObject obj(cx);
  if (!vp::Construct(cx, target, args, args.newTarget(), &obj)) {
    return false;
  }
  args.rval().setObject(*obj);
  return true;
}

bool js::ForwardingProxyHandler::hasOwn(JSContext* cx, HandleObject proxy,
                                        HandleId id, bool* bp) const {
  JS::RootedObject target(cx, TargetOf(proxy));
  return vp::HasOwnProperty(cx, target, id, bp);
}

bool js::ForwardingProxyHandler::getOwnEnumerablePropertyKeys(
    JSContext* cx, HandleObject proxy, MutableHandleIdVector props) const {
  JS::RootedObject target(cx, TargetOf(proxy));
  return vp::OwnEnumerableStringKeys(cx, target, props);
}

bool js::ForwardingProxyHandler::nativeCall(JSContext* cx,
                                            JS::IsAcceptableThis test,
                                            JS::NativeImpl impl,
                                            const CallArgs& args) const {
  args.setThis(JS::ObjectValue(*TargetOf(&args.thisv().toObject())));
  if (!test(args.thisv())) {
    return vp::ReportIncompatible(cx, args);
  }
  return impl(cx, args);
}

bool js::ForwardingProxyHandler::getBuiltinClass(JSContext* cx,
                                                 HandleObject proxy,
                                                 ESClass* cls) const {
  JS::RootedObject target(cx, TargetOf(proxy));
  return vp::GetBuiltinClass(cx, target, cls);
}

bool js::ForwardingProxyHandler::isArray(JSContext* cx, HandleObject proxy,
                                         JS::IsArrayAnswer* answer) const {
  JS::RootedObject target(cx, TargetOf(proxy));
  return vp::IsArray(cx, target, answer);
}

const char* js::ForwardingProxyHandler::className(JSContext* cx,
                                                  HandleObject proxy) const {
  JS::RootedObject target(cx, TargetOf(proxy));
  return vp::ClassName(cx, target);
}

JSString* js::ForwardingProxyHandler::fun_toString(JSContext* cx,
                                                   HandleObject proxy,
                                                   bool isToSource) const {
  JS::RootedObject target(cx, TargetOf(proxy));
  return vp::FunToString(cx, target, isToSource);
}

js::RegExpShared* js::ForwardingProxyHandler::regexp_toShared(
    JSContext* cx, HandleObject proxy) const {
  JS::RootedObject target(cx, TargetOf(proxy));
  if (js::IsProxy(target)) {
    return js::GetProxyHandler(target)->regexp_toShared(cx, target);
  }
  return ReportNoRegExpShared(cx);
}

bool js::ForwardingProxyHandler::boxedValue_unbox(JSContext* cx,
                                                  HandleObject proxy,
                                                  MutableHandleValue vp) const {
  JS::RootedObject target(cx, TargetOf(proxy));
  return vp::Unbox(cx, target, vp);
}

bool js::ForwardingProxyHandler::isCallable(JSObject* obj) const {
  return vp::IsCallableObject(TargetOf(obj));
}

bool js::ForwardingProxyHandler::isConstructor(JSObject* obj) const {
  return vp::IsConstructorObject(TargetOf(obj));
}

bool js::CrossCompartmentWrapper::getOwnPropertyDescriptor(
    JSContext* cx, HandleObject wrapper, HandleId id,
    MutableHandle<mozilla::Maybe<PropertyDescriptor>> desc) const {
  JS::RootedId idCopy(cx, id);
  bool ok;
  {
    AutoEnterRealmOf call(cx, wrappedObject(wrapper));
    ok = JS_WrapId(cx, &idCopy) &&
         Wrapper::getOwnPropertyDescriptor(cx, wrapper, idCopy, desc);
  }
  return ok && JS_WrapPropertyDescriptor(cx, desc);
}

bool js::CrossCompartmentWrapper::defineProperty(
    JSContext* cx, HandleObject wrapper, HandleId id,
    JS::Handle<PropertyDescriptor> desc, ObjectOpResult& result) const {
  JS::RootedId idCopy(cx, id);
  JS::Rooted<PropertyDescriptor> desc2(cx, desc);
  AutoEnterRealmOf call(cx, wrappedObject(wrapper));
  return JS_WrapId(cx, &idCopy) && JS_WrapPropertyDescriptor(cx, &desc2) &&
         Wrapper::defineProperty(cx, wrapper, idCopy, desc2, result);
}

bool js::CrossCompartmentWrapper::ownPropertyKeys(
    JSContext* cx, HandleObject wrapper, MutableHandleIdVector props) const {
  bool ok;
  {
    AutoEnterRealmOf call(cx, wrappedObject(wrapper));
    ok = Wrapper::ownPropertyKeys(cx, wrapper, props);
  }
  return ok && WrapIds(cx, props);
}

bool js::CrossCompartmentWrapper::delete_(JSContext* cx, HandleObject wrapper,
                                          HandleId id,
                                          ObjectOpResult& result) const {
  JS::RootedId idCopy(cx, id);
  AutoEnterRealmOf call(cx, wrappedObject(wrapper));
  return JS_WrapId(cx, &idCopy) &&
         Wrapper::delete_(cx, wrapper, idCopy, result);
}

bool js::CrossCompartmentWrapper::getPrototype(
    JSContext* cx, HandleObject wrapper, MutableHandleObject protop) const {
  {
    JS::RootedObject wrapped(cx, wrappedObject(wrapper));
    AutoEnterRealmOf call(cx, wrapped.get());
    if (!vp::GetPrototype(cx,wrapped, protop)) {
      return false;
    }
  }
  return JS_WrapObject(cx, protop);
}

bool js::CrossCompartmentWrapper::setPrototype(JSContext* cx,
                                               HandleObject wrapper,
                                               HandleObject proto,
                                               ObjectOpResult& result) const {
  JS::RootedObject protoCopy(cx, proto);
  AutoEnterRealmOf call(cx, wrappedObject(wrapper));
  return JS_WrapObject(cx, &protoCopy) &&
         Wrapper::setPrototype(cx, wrapper, protoCopy, result);
}

bool js::CrossCompartmentWrapper::getPrototypeIfOrdinary(
    JSContext* cx, HandleObject wrapper, bool* isOrdinary,
    MutableHandleObject protop) const {
  {
    JS::RootedObject wrapped(cx, wrappedObject(wrapper));
    AutoEnterRealmOf call(cx, wrapped.get());
    if (!vp::GetPrototypeIfOrdinary(cx,wrapped, isOrdinary, protop)) {
      return false;
    }
    if (!*isOrdinary) {
      return true;
    }
  }
  return JS_WrapObject(cx, protop);
}

bool js::CrossCompartmentWrapper::setImmutablePrototype(JSContext* cx,
                                                        HandleObject wrapper,
                                                        bool* succeeded) const {
  AutoEnterRealmOf call(cx, wrappedObject(wrapper));
  return Wrapper::setImmutablePrototype(cx, wrapper, succeeded);
}

bool js::CrossCompartmentWrapper::preventExtensions(
    JSContext* cx, HandleObject wrapper, ObjectOpResult& result) const {
  AutoEnterRealmOf call(cx, wrappedObject(wrapper));
  return Wrapper::preventExtensions(cx, wrapper, result);
}

bool js::CrossCompartmentWrapper::isExtensible(JSContext* cx,
                                               HandleObject wrapper,
                                               bool* extensible) const {
  AutoEnterRealmOf call(cx, wrappedObject(wrapper));
  return Wrapper::isExtensible(cx, wrapper, extensible);
}

bool js::CrossCompartmentWrapper::has(JSContext* cx, HandleObject wrapper,
                                      HandleId id, bool* bp) const {
  JS::RootedId idCopy(cx, id);
  AutoEnterRealmOf call(cx, wrappedObject(wrapper));
  return JS_WrapId(cx, &idCopy) && Wrapper::has(cx, wrapper, idCopy, bp);
}

bool js::CrossCompartmentWrapper::hasOwn(JSContext* cx, HandleObject wrapper,
                                         HandleId id, bool* bp) const {
  JS::RootedId idCopy(cx, id);
  AutoEnterRealmOf call(cx, wrappedObject(wrapper));
  return JS_WrapId(cx, &idCopy) && Wrapper::hasOwn(cx, wrapper, idCopy, bp);
}

bool js::CrossCompartmentWrapper::get(JSContext* cx, HandleObject wrapper,
                                      HandleValue receiver, HandleId id,
                                      MutableHandleValue vp) const {
  JS::RootedValue receiverCopy(cx, receiver);
  JS::RootedId idCopy(cx, id);
  {
    AutoEnterRealmOf call(cx, wrappedObject(wrapper));
    if (!JS_WrapId(cx, &idCopy) || !WrapReceiver(cx, wrapper, &receiverCopy)) {
      return false;
    }
    if (!Wrapper::get(cx, wrapper, receiverCopy, idCopy, vp)) {
      return false;
    }
  }
  return JS_WrapValue(cx, vp);
}

bool js::CrossCompartmentWrapper::set(JSContext* cx, HandleObject wrapper,
                                      HandleId id, HandleValue v,
                                      HandleValue receiver,
                                      ObjectOpResult& result) const {
  JS::RootedId idCopy(cx, id);
  JS::RootedValue valCopy(cx, v);
  JS::RootedValue receiverCopy(cx, receiver);
  AutoEnterRealmOf call(cx, wrappedObject(wrapper));
  return JS_WrapId(cx, &idCopy) && JS_WrapValue(cx, &valCopy) &&
         WrapReceiver(cx, wrapper, &receiverCopy) &&
         Wrapper::set(cx, wrapper, idCopy, valCopy, receiverCopy, result);
}

bool js::CrossCompartmentWrapper::getOwnEnumerablePropertyKeys(
    JSContext* cx, HandleObject wrapper, MutableHandleIdVector props) const {
  bool ok;
  {
    AutoEnterRealmOf call(cx, wrappedObject(wrapper));
    ok = Wrapper::getOwnEnumerablePropertyKeys(cx, wrapper, props);
  }
  return ok && WrapIds(cx, props);
}

bool js::CrossCompartmentWrapper::enumerate(JSContext* cx,
                                            HandleObject wrapper,
                                            MutableHandleIdVector props) const {
  bool ok;
  {
    AutoEnterRealmOf call(cx, wrappedObject(wrapper));
    ok = Wrapper::enumerate(cx, wrapper, props);
  }
  return ok && WrapIds(cx, props);
}

bool js::CrossCompartmentWrapper::call(JSContext* cx, HandleObject wrapper,
                                       const CallArgs& args) const {
  JS::RootedObject wrapped(cx, wrappedObject(wrapper));
  {
    AutoEnterRealmOf call(cx, wrapped.get());
    args.setCallee(JS::ObjectValue(*wrapped));
    if (!JS_WrapValue(cx, args.mutableThisv())) {
      return false;
    }
    for (size_t n = 0; n < args.length(); ++n) {
      if (!JS_WrapValue(cx, args[n])) {
        return false;
      }
    }
    if (!Wrapper::call(cx, wrapper, args)) {
      return false;
    }
  }
  return JS_WrapValue(cx, args.rval());
}

bool js::CrossCompartmentWrapper::construct(JSContext* cx,
                                            HandleObject wrapper,
                                            const CallArgs& args) const {
  JS::RootedObject wrapped(cx, wrappedObject(wrapper));
  {
    AutoEnterRealmOf call(cx, wrapped.get());
    for (size_t n = 0; n < args.length(); ++n) {
      if (!JS_WrapValue(cx, args[n])) {
        return false;
      }
    }
    if (!JS_WrapValue(cx, args.newTarget())) {
      return false;
    }
    if (!Wrapper::construct(cx, wrapper, args)) {
      return false;
    }
  }
  return JS_WrapValue(cx, args.rval());
}

bool js::CrossCompartmentWrapper::nativeCall(JSContext* cx,
                                             JS::IsAcceptableThis test,
                                             JS::NativeImpl impl,
                                             const CallArgs& srcArgs) const {
  JS::RootedObject wrapper(cx, &srcArgs.thisv().toObject());
  JS::RootedObject wrapped(cx, wrappedObject(wrapper));
  {
    AutoEnterRealmOf call(cx, wrapped.get());
    unsigned argc = srcArgs.length();
    JS::RootedValueVector dst(cx);
    if (!dst.resize(argc + 3)) {
      return false;
    }
    const JS::Value* src = srcArgs.base();
    JS::RootedValue source(cx);
    for (unsigned i = 0; i < argc + 2; i++) {
      source = src[i];
      if (!JS_WrapValue(cx, &source)) {
        return false;
      }
      dst[i].set(source);
      if (i == 1 && source.isObject()) {
        JSObject* thisObj = &source.toObject();
        if (js::IsWrapper(thisObj) &&
            Wrapper::wrapperHandler(thisObj)->hasSecurityPolicy()) {
          dst[i].setObject(*Wrapper::wrappedObject(thisObj));
        }
      }
    }
    dst[argc + 2].setUndefined();
    JS::CallArgs dstArgs = JS::CallArgsFromVp(argc, dst.begin());
    if (!JS::CallNonGenericMethod(cx, test, impl, dstArgs)) {
      return false;
    }
    srcArgs.rval().set(dstArgs.rval());
  }
  return JS_WrapValue(cx, srcArgs.rval());
}

const char* js::CrossCompartmentWrapper::className(JSContext* cx,
                                                   HandleObject wrapper) const {
  AutoEnterRealmOf call(cx, wrappedObject(wrapper));
  return Wrapper::className(cx, wrapper);
}

JSString* js::CrossCompartmentWrapper::fun_toString(JSContext* cx,
                                                    HandleObject wrapper,
                                                    bool isToSource) const {
  JS::RootedString str(cx);
  {
    AutoEnterRealmOf call(cx, wrappedObject(wrapper));
    str = Wrapper::fun_toString(cx, wrapper, isToSource);
    if (!str) {
      return nullptr;
    }
  }
  JS::RootedValue v(cx, JS::StringValue(str));
  if (!JS_WrapValue(cx, &v)) {
    return nullptr;
  }
  return v.toString();
}

js::RegExpShared* js::CrossCompartmentWrapper::regexp_toShared(
    JSContext* cx, HandleObject wrapper) const {
  AutoEnterRealmOf call(cx, wrappedObject(wrapper));
  return Wrapper::regexp_toShared(cx, wrapper);
}

bool js::CrossCompartmentWrapper::boxedValue_unbox(JSContext* cx,
                                                   HandleObject wrapper,
                                                   MutableHandleValue vp) const {
  bool ok;
  {
    AutoEnterRealmOf call(cx, wrappedObject(wrapper));
    ok = Wrapper::boxedValue_unbox(cx, wrapper, vp);
  }
  return ok && JS_WrapValue(cx, vp);
}

const js::CrossCompartmentWrapper js::CrossCompartmentWrapper::singleton(0u);

bool js::OpaqueCrossCompartmentWrapper::getOwnPropertyDescriptor(
    JSContext* cx, HandleObject wrapper, HandleId id,
    MutableHandle<mozilla::Maybe<PropertyDescriptor>> desc) const {
  desc.reset();
  return true;
}

bool js::OpaqueCrossCompartmentWrapper::defineProperty(
    JSContext* cx, HandleObject wrapper, HandleId id,
    JS::Handle<PropertyDescriptor> desc, ObjectOpResult& result) const {
  return result.succeed();
}

bool js::OpaqueCrossCompartmentWrapper::ownPropertyKeys(
    JSContext* cx, HandleObject wrapper, MutableHandleIdVector props) const {
  return true;
}

bool js::OpaqueCrossCompartmentWrapper::delete_(JSContext* cx,
                                                HandleObject wrapper,
                                                HandleId id,
                                                ObjectOpResult& result) const {
  return result.succeed();
}

bool js::OpaqueCrossCompartmentWrapper::enumerate(
    JSContext* cx, HandleObject proxy, MutableHandleIdVector props) const {
  return BaseProxyHandler::enumerate(cx, proxy, props);
}

bool js::OpaqueCrossCompartmentWrapper::getPrototype(
    JSContext* cx, HandleObject proxy, MutableHandleObject protop) const {
  protop.set(nullptr);
  return true;
}

bool js::OpaqueCrossCompartmentWrapper::setPrototype(
    JSContext* cx, HandleObject proxy, HandleObject proto,
    ObjectOpResult& result) const {
  return result.succeed();
}

bool js::OpaqueCrossCompartmentWrapper::getPrototypeIfOrdinary(
    JSContext* cx, HandleObject proxy, bool* isOrdinary,
    MutableHandleObject protop) const {
  *isOrdinary = false;
  return true;
}

bool js::OpaqueCrossCompartmentWrapper::setImmutablePrototype(
    JSContext* cx, HandleObject proxy, bool* succeeded) const {
  *succeeded = false;
  return true;
}

bool js::OpaqueCrossCompartmentWrapper::preventExtensions(
    JSContext* cx, HandleObject wrapper, ObjectOpResult& result) const {
  return result.fail(JSMSG_CANT_CHANGE_EXTENSIBILITY);
}

bool js::OpaqueCrossCompartmentWrapper::isExtensible(JSContext* cx,
                                                     HandleObject wrapper,
                                                     bool* extensible) const {
  *extensible = true;
  return true;
}

bool js::OpaqueCrossCompartmentWrapper::has(JSContext* cx,
                                            HandleObject wrapper, HandleId id,
                                            bool* bp) const {
  return BaseProxyHandler::has(cx, wrapper, id, bp);
}

bool js::OpaqueCrossCompartmentWrapper::get(JSContext* cx,
                                            HandleObject wrapper,
                                            HandleValue receiver, HandleId id,
                                            MutableHandleValue vp) const {
  return BaseProxyHandler::get(cx, wrapper, receiver, id, vp);
}

bool js::OpaqueCrossCompartmentWrapper::set(JSContext* cx,
                                            HandleObject wrapper, HandleId id,
                                            HandleValue v,
                                            HandleValue receiver,
                                            ObjectOpResult& result) const {
  return BaseProxyHandler::set(cx, wrapper, id, v, receiver, result);
}

bool js::OpaqueCrossCompartmentWrapper::call(JSContext* cx,
                                             HandleObject wrapper,
                                             const CallArgs& args) const {
  JS::RootedValue v(cx, JS::ObjectValue(*wrapper));
  vp::ReportNotFunction(cx, v, JSMSG_NOT_FUNCTION);
  return false;
}

bool js::OpaqueCrossCompartmentWrapper::construct(JSContext* cx,
                                                  HandleObject wrapper,
                                                  const CallArgs& args) const {
  JS::RootedValue v(cx, JS::ObjectValue(*wrapper));
  vp::ReportNotFunction(cx, v, JSMSG_NOT_FUNCTION);
  return false;
}

bool js::OpaqueCrossCompartmentWrapper::hasOwn(JSContext* cx,
                                               HandleObject wrapper,
                                               HandleId id, bool* bp) const {
  return BaseProxyHandler::hasOwn(cx, wrapper, id, bp);
}

bool js::OpaqueCrossCompartmentWrapper::getOwnEnumerablePropertyKeys(
    JSContext* cx, HandleObject wrapper, MutableHandleIdVector props) const {
  return BaseProxyHandler::getOwnEnumerablePropertyKeys(cx, wrapper, props);
}

bool js::OpaqueCrossCompartmentWrapper::getBuiltinClass(JSContext* cx,
                                                        HandleObject wrapper,
                                                        ESClass* cls) const {
  *cls = ESClass::Other;
  return true;
}

bool js::OpaqueCrossCompartmentWrapper::isArray(
    JSContext* cx, HandleObject obj, JS::IsArrayAnswer* answer) const {
  *answer = JS::IsArrayAnswer::NotArray;
  return true;
}

const char* js::OpaqueCrossCompartmentWrapper::className(
    JSContext* cx, HandleObject proxy) const {
  return "Opaque";
}

JSString* js::OpaqueCrossCompartmentWrapper::fun_toString(
    JSContext* cx, HandleObject proxy, bool isToSource) const {
  vp::ReportErrorNumber(cx, JSMSG_INCOMPATIBLE_PROTO,
                    {"Function", "toString", "object"});
  return nullptr;
}

const js::OpaqueCrossCompartmentWrapper
    js::OpaqueCrossCompartmentWrapper::singleton;

template <class Base>
bool js::SecurityWrapper<Base>::enter(JSContext* cx, HandleObject wrapper,
                                      HandleId id, Wrapper::Action act,
                                      bool mayThrow, bool* bp) const {
  if (mayThrow) {
    js::ReportAccessDenied(cx);
  }
  *bp = false;
  return false;
}

template <class Base>
bool js::SecurityWrapper<Base>::nativeCall(JSContext* cx,
                                           JS::IsAcceptableThis test,
                                           JS::NativeImpl impl,
                                           const CallArgs& args) const {
  js::ReportAccessDenied(cx);
  return false;
}

template <class Base>
bool js::SecurityWrapper<Base>::setPrototype(JSContext* cx,
                                             HandleObject wrapper,
                                             HandleObject proto,
                                             ObjectOpResult& result) const {
  js::ReportAccessDenied(cx);
  return false;
}

template <class Base>
bool js::SecurityWrapper<Base>::setImmutablePrototype(JSContext* cx,
                                                      HandleObject wrapper,
                                                      bool* succeeded) const {
  js::ReportAccessDenied(cx);
  return false;
}

template <class Base>
bool js::SecurityWrapper<Base>::preventExtensions(
    JSContext* cx, HandleObject wrapper, ObjectOpResult& result) const {
  return result.fail(JSMSG_CANT_CHANGE_EXTENSIBILITY);
}

template <class Base>
bool js::SecurityWrapper<Base>::isExtensible(JSContext* cx,
                                             HandleObject wrapper,
                                             bool* extensible) const {
  *extensible = true;
  return true;
}

template <class Base>
bool js::SecurityWrapper<Base>::getBuiltinClass(JSContext* cx,
                                                HandleObject wrapper,
                                                ESClass* cls) const {
  *cls = ESClass::Other;
  return true;
}

template <class Base>
bool js::SecurityWrapper<Base>::isArray(JSContext* cx, HandleObject obj,
                                        JS::IsArrayAnswer* answer) const {
  *answer = JS::IsArrayAnswer::NotArray;
  return true;
}

template <class Base>
js::RegExpShared* js::SecurityWrapper<Base>::regexp_toShared(
    JSContext* cx, HandleObject obj) const {
  return Base::regexp_toShared(cx, obj);
}

template <class Base>
bool js::SecurityWrapper<Base>::boxedValue_unbox(JSContext* cx,
                                                 HandleObject obj,
                                                 MutableHandleValue vp) const {
  vp.setUndefined();
  return true;
}

template <class Base>
bool js::SecurityWrapper<Base>::defineProperty(
    JSContext* cx, HandleObject wrapper, HandleId id,
    JS::Handle<PropertyDescriptor> desc, ObjectOpResult& result) const {
  if (desc.isAccessorDescriptor()) {
    std::string name = vp::IdToUTF8(cx, id);
    vp::ReportErrorNumber(cx, JSMSG_ACCESSOR_DEF_DENIED, {name.c_str()});
    return false;
  }
  return Base::defineProperty(cx, wrapper, id, desc, result);
}

template class js::SecurityWrapper<js::Wrapper>;
template class js::SecurityWrapper<js::CrossCompartmentWrapper>;
