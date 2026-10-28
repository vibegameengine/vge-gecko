#include "VgeJsProxyInternal.h"

#include <vector>

#include "js/Object.h"
#include "js/WrapperCallbacks.h"

namespace vp = vge::jsapi::proxy;
using vge::jsapi::CurrentContext;
using vge::jsapi::ObjectCell;
using vge::jsapi::ObjectFromV8;
using vge::jsapi::Runtime;
using vge::jsapi::ToV8;

namespace {

const void* gDOMProxyHandlerFamily = nullptr;
JS::DOMProxyShadowsCheck gDOMProxyShadowsCheck = nullptr;
const void* gDOMRemoteProxyHandlerFamily = nullptr;

v8::Local<v8::Private> WindowProxyKey(v8::Isolate* aIsolate) {
  return v8::Private::ForApi(
      aIsolate, v8::String::NewFromUtf8Literal(aIsolate, "vge.jsapi.windowProxy"));
}

v8::Local<v8::Context> ContextOf(JSContext* aCx, v8::Local<v8::Object> aObj) {
  v8::Local<v8::Context> ctx;
  if (aObj->GetCreationContext().ToLocal(&ctx)) {
    return ctx;
  }
  return aCx->v8Context();
}

JSObject* MaybeWindowProxy(JSObject* aGlobal) {
  JSContext* cx = CurrentContext();
  if (!cx) {
    return nullptr;
  }
  v8::Isolate* isolate = cx->isolate;
  v8::HandleScope scope(isolate);
  v8::Local<v8::Object> global = ToV8(cx, aGlobal);
  if (global.IsEmpty()) {
    return nullptr;
  }
  v8::Local<v8::Context> ctx = ContextOf(cx, global);
  if (ctx.IsEmpty()) {
    return nullptr;
  }
  v8::Local<v8::Value> value;
  if (!global->GetPrivate(ctx, WindowProxyKey(isolate)).ToLocal(&value) ||
      !value->IsObject()) {
    return nullptr;
  }
  return ObjectFromV8(cx, value.As<v8::Object>());
}

std::vector<JS::Compartment*> ListCompartments(JSRuntime* aRt) {
  std::vector<JS::Compartment*> out;
  for (JS::Realm* realm : aRt->realms) {
    JS::Compartment* c = JS::GetCompartmentForRealm(realm);
    bool seen = false;
    for (JS::Compartment* known : out) {
      if (known == c) {
        seen = true;
        break;
      }
    }
    if (!seen) {
      out.push_back(c);
    }
  }
  return out;
}

bool NukedAllRealms(JSRuntime* aRt, JS::Compartment* aComp) {
  vp::RuntimeState& st = vp::State();
  for (JS::Realm* realm : aRt->realms) {
    if (JS::GetCompartmentForRealm(realm) == aComp &&
        !st.nukedIncoming.count(realm)) {
      return false;
    }
  }
  return true;
}

std::vector<JSObject*> CrossCompartmentWrappers() {
  std::vector<JSObject*> out;
  for (auto& entry : vp::State().proxies) {
    JSObject* obj = entry.first->asObject();
    if (js::IsCrossCompartmentWrapper(obj)) {
      out.push_back(obj);
    }
  }
  return out;
}

JS::Compartment* CurrentCompartment(JSContext* aCx) {
  return aCx->realm() ? JS::GetCompartmentForRealm(aCx->realm()) : nullptr;
}

bool NewDead(JSContext* aCx, JS::MutableHandleObject aObj) {
  aObj.set(JS_NewDeadWrapper(aCx, aObj));
  return !!aObj;
}

bool NonWrapperObjectForCurrentCompartment(JSContext* aCx,
                                           JS::HandleObject aOrigObj,
                                           JS::MutableHandleObject aObj) {
  JS::Compartment* comp = CurrentCompartment(aCx);
  if (vp::ObjectCompartment(aObj) == comp) {
    aObj.set(js::ToWindowProxyIfWindow(aObj));
    return true;
  }
  JS::RootedObject passed(aCx, aObj);
  aObj.set(js::UncheckedUnwrap(aObj, true));
  if (vp::ObjectCompartment(aObj) == comp) {
    return true;
  }
  if (!js::AllowNewWrapper(comp, aObj)) {
    return NewDead(aCx, aObj);
  }
  if (js::IsWindow(aObj)) {
    aObj.set(js::ToWindowProxyIfWindow(aObj));
    aObj.set(js::UncheckedUnwrap(aObj));
    if (JS_IsDeadWrapper(aObj)) {
      return NewDead(aCx, aObj);
    }
    if (vp::ObjectCompartment(aObj) != comp &&
        !js::AllowNewWrapper(comp, aObj)) {
      return NewDead(aCx, aObj);
    }
  }
  if (JS_IsDeadWrapper(aObj)) {
    return NewDead(aCx, aObj);
  }
  const JSWrapObjectCallbacks* callbacks = aCx->runtime->wrapObjectCallbacks;
  if (callbacks && callbacks->preWrap) {
    JS::RootedObject global(aCx, aCx->realm()->global);
    callbacks->preWrap(aCx, global, aOrigObj, aObj, passed, aObj);
    if (!aObj) {
      return false;
    }
  }
  return true;
}

bool Rewrap(JSContext* aCx, JS::MutableHandleObject aObj,
            JS::HandleObject aExisting) {
  if (!NonWrapperObjectForCurrentCompartment(aCx, aExisting, aObj)) {
    return false;
  }
  if (vp::ObjectCompartment(aObj) == CurrentCompartment(aCx)) {
    return true;
  }
  const JSWrapObjectCallbacks* callbacks = aCx->runtime->wrapObjectCallbacks;
  JSObject* wrapper =
      callbacks && callbacks->wrap
          ? callbacks->wrap(aCx, aExisting, aObj)
          : js::Wrapper::New(aCx, aObj, &js::CrossCompartmentWrapper::singleton);
  if (!wrapper) {
    return false;
  }
  aObj.set(wrapper);
  return true;
}

void RemapDeadWrapper(JSContext* aCx, JS::HandleObject aWrapper,
                      JS::HandleObject aNewTarget) {
  vp::AutoEnterRealmOf realm(aCx, vp::ObjectRealm(aWrapper));
  JS::RootedObject tobj(aCx, aNewTarget);
  if (!Rewrap(aCx, &tobj, aWrapper)) {
    MOZ_CRASH("js::RemapWrapper");
  }
  if (tobj != aWrapper) {
    vp::SwapProxies(aCx, aWrapper, tobj);
  }
}

}

namespace vge::jsapi::proxy {

JSObject* FindCrossCompartmentWrapper(JS::Compartment* aCompartment,
                                      JSObject* aTarget) {
  for (auto& entry : State().proxies) {
    JSObject* obj = entry.first->asObject();
    if (js::IsCrossCompartmentWrapper(obj) &&
        ObjectCompartment(obj) == aCompartment &&
        js::GetProxyTargetObject(obj) == aTarget) {
      return obj;
    }
  }
  return nullptr;
}

void RemapWrapper(JSContext* aCx, JS::HandleObject aWrapper,
                  JS::HandleObject aNewTarget) {
  JS::RootedObject origTarget(aCx, js::GetProxyTargetObject(aWrapper));
  NukeProxy(aCx, aWrapper);
  if (JS_IsDeadWrapper(origTarget)) {
    return;
  }
  RemapDeadWrapper(aCx, aWrapper, aNewTarget);
}

}

JS_PUBLIC_API JSObject* js::UncheckedUnwrapWithoutExpose(JSObject* wrapped) {
  while (wrapped && js::IsWrapper(wrapped) &&
         MOZ_LIKELY(!js::IsWindowProxy(wrapped))) {
    wrapped = js::GetProxyTargetObject(wrapped);
  }
  return wrapped;
}

JS_PUBLIC_API JSObject* js::UncheckedUnwrap(JSObject* wrapped,
                                            bool stopAtWindowProxy,
                                            unsigned* flagsp) {
  unsigned flags = 0;
  while (true) {
    if (!js::IsWrapper(wrapped) ||
        MOZ_UNLIKELY(stopAtWindowProxy && js::IsWindowProxy(wrapped))) {
      break;
    }
    flags |= Wrapper::wrapperHandler(wrapped)->flags();
    wrapped = Wrapper::wrappedObject(wrapped);
  }
  if (flagsp) {
    *flagsp = flags;
  }
  return wrapped;
}

JS_PUBLIC_API JSObject* js::UnwrapOneCheckedStatic(JSObject* obj) {
  if (!js::IsWrapper(obj) || MOZ_UNLIKELY(js::IsWindowProxy(obj))) {
    return obj;
  }
  const Wrapper* handler = Wrapper::wrapperHandler(obj);
  return handler->hasSecurityPolicy() ? nullptr : Wrapper::wrappedObject(obj);
}

JS_PUBLIC_API JSObject* js::CheckedUnwrapStatic(JSObject* obj) {
  while (true) {
    JSObject* wrapper = obj;
    obj = UnwrapOneCheckedStatic(obj);
    if (!obj || obj == wrapper) {
      return obj;
    }
  }
}

JS_PUBLIC_API JSObject* js::UnwrapOneCheckedDynamic(JS::HandleObject obj,
                                                    JSContext* cx,
                                                    bool stopAtWindowProxy) {
  if (!js::IsWrapper(obj) ||
      MOZ_UNLIKELY(stopAtWindowProxy && js::IsWindowProxy(obj))) {
    return obj;
  }
  const Wrapper* handler = Wrapper::wrapperHandler(obj);
  if (!handler->hasSecurityPolicy() ||
      handler->dynamicCheckedUnwrapAllowed(obj, cx)) {
    return Wrapper::wrappedObject(obj);
  }
  return nullptr;
}

JS_PUBLIC_API JSObject* js::CheckedUnwrapDynamic(JSObject* obj, JSContext* cx,
                                                 bool stopAtWindowProxy) {
  JS::RootedObject wrapper(cx, obj);
  while (true) {
    JSObject* unwrapped =
        UnwrapOneCheckedDynamic(wrapper, cx, stopAtWindowProxy);
    if (!unwrapped || unwrapped == wrapper) {
      return unwrapped;
    }
    wrapper = unwrapped;
  }
}

JS_PUBLIC_API void js::ReportAccessDenied(JSContext* cx) {
  vp::ReportErrorNumber(cx, JSMSG_OBJECT_ACCESS_DENIED, {});
}

JS_PUBLIC_API bool js::AllowNewWrapper(JS::Compartment* target, JSObject* obj) {
  vp::RuntimeState& st = vp::State();
  return !st.nukedOutgoing.count(target) &&
         !st.nukedIncoming.count(vp::ObjectRealm(obj));
}

JS_PUBLIC_API bool js::NukedObjectRealm(JSObject* obj) {
  return vp::State().nukedIncoming.count(vp::ObjectRealm(obj)) != 0;
}

JS_PUBLIC_API void js::NukeCrossCompartmentWrapperIfExists(
    JSContext* cx, JS::Compartment* source, JSObject* target) {
  JS::RootedObject wrapper(cx, vp::FindCrossCompartmentWrapper(source, target));
  if (wrapper) {
    vp::NukeProxy(cx, wrapper);
  }
}

JS_PUBLIC_API bool js::NukeCrossCompartmentWrappers(
    JSContext* cx, const CompartmentFilter& sourceFilter, JS::Realm* target,
    js::NukeReferencesToWindow nukeReferencesToWindow,
    js::NukeReferencesFromTarget nukeReferencesFromTarget) {
  JSRuntime* rt = cx->runtime;
  vp::RuntimeState& st = vp::State();
  if (nukeReferencesFromTarget == NukeAllReferences) {
    st.nukedIncoming.insert(target);
  }
  JS::Compartment* targetComp = JS::GetCompartmentForRealm(target);
  JS::RootedObjectVector toNuke(cx);
  for (JS::Compartment* c : ListCompartments(rt)) {
    if (!sourceFilter.match(c)) {
      continue;
    }
    bool nukeAll = nukeReferencesFromTarget == NukeAllReferences &&
                   targetComp == c && NukedAllRealms(rt, c);
    if (nukeAll) {
      st.nukedOutgoing.insert(c);
    }
    for (JSObject* wrapper : CrossCompartmentWrappers()) {
      if (vp::ObjectCompartment(wrapper) != c) {
        continue;
      }
      JSObject* key = js::GetProxyTargetObject(wrapper);
      if (!key) {
        continue;
      }
      if (!nukeAll && vp::ObjectCompartment(key) != targetComp) {
        continue;
      }
      JSObject* wrapped = js::UncheckedUnwrap(key);
      if (!nukeAll && vp::ObjectRealm(wrapped) != target) {
        continue;
      }
      if (nukeReferencesToWindow == DontNukeWindowReferences && !nukeAll &&
          js::IsWindowProxy(wrapped)) {
        continue;
      }
      if (!toNuke.append(wrapper)) {
        return false;
      }
    }
  }
  JS::RootedObject wrapper(cx);
  for (size_t i = 0; i < toNuke.length(); i++) {
    wrapper = toNuke[i];
    vp::NukeProxy(cx, wrapper);
  }
  return true;
}

JS_PUBLIC_API bool js::RemapAllWrappersForObject(JSContext* cx,
                                                 JS::HandleObject oldTarget,
                                                 JS::HandleObject newTarget) {
  JS::RootedObjectVector toTransplant(cx);
  for (JSObject* wrapper : CrossCompartmentWrappers()) {
    if (js::GetProxyTargetObject(wrapper) == oldTarget &&
        !toTransplant.append(wrapper)) {
      return false;
    }
  }
  JS::RootedObject wrapper(cx);
  for (size_t i = 0; i < toTransplant.length(); i++) {
    wrapper = toTransplant[i];
    vp::RemapWrapper(cx, wrapper, newTarget);
  }
  return true;
}

JS_PUBLIC_API bool js::RecomputeWrappers(JSContext* cx,
                                         const CompartmentFilter& sourceFilter,
                                         const CompartmentFilter& targetFilter) {
  JS::RootedObjectVector toRecompute(cx);
  for (JSObject* wrapper : CrossCompartmentWrappers()) {
    JSObject* target = js::GetProxyTargetObject(wrapper);
    if (!target || !sourceFilter.match(vp::ObjectCompartment(wrapper)) ||
        !targetFilter.match(vp::ObjectCompartment(target))) {
      continue;
    }
    if (!toRecompute.append(wrapper)) {
      return false;
    }
  }
  JS::RootedObject wrapper(cx);
  JS::RootedObject wrapped(cx);
  for (size_t i = 0; i < toRecompute.length(); i++) {
    wrapper = toRecompute[i];
    wrapped = js::GetProxyTargetObject(wrapper);
    vp::RemapWrapper(cx, wrapper, wrapped);
  }
  return true;
}

JS_PUBLIC_API JSObject* JS_TransplantObject(JSContext* cx,
                                            JS::HandleObject origobj,
                                            JS::HandleObject target) {
  MOZ_RELEASE_ASSERT(origobj != target);
  MOZ_RELEASE_ASSERT(js::IsProxy(origobj) &&
                     !js::IsCrossCompartmentWrapper(origobj));
  MOZ_RELEASE_ASSERT(js::IsProxy(target) &&
                     !js::IsCrossCompartmentWrapper(target));
  JS::RootedObject newIdentity(cx);
  JS::Compartment* destination = vp::ObjectCompartment(target);
  if (vp::ObjectCompartment(origobj) == destination) {
    vp::AutoEnterRealmOf realm(cx, vp::ObjectRealm(origobj));
    vp::SwapProxies(cx, origobj, target);
    newIdentity = origobj;
  } else if (JSObject* existing =
                 vp::FindCrossCompartmentWrapper(destination, origobj)) {
    newIdentity = existing;
    vp::NukeProxy(cx, newIdentity);
    vp::AutoEnterRealmOf realm(cx, vp::ObjectRealm(newIdentity));
    vp::SwapProxies(cx, newIdentity, target);
  } else {
    newIdentity = target;
  }
  if (!js::RemapAllWrappersForObject(cx, origobj, newIdentity)) {
    MOZ_CRASH("JS_TransplantObject");
  }
  if (vp::ObjectCompartment(origobj) != destination) {
    JS::RootedObject newIdentityWrapper(cx, newIdentity);
    vp::AutoEnterRealmOf realm(cx, vp::ObjectRealm(origobj));
    if (!JS_WrapObject(cx, &newIdentityWrapper)) {
      MOZ_CRASH("JS_TransplantObject");
    }
    MOZ_RELEASE_ASSERT(js::IsWrapper(newIdentityWrapper));
    vp::SwapProxies(cx, origobj, newIdentityWrapper);
  }
  return newIdentity;
}

JS_PUBLIC_API void js::SetWindowProxyClass(JSContext* cx,
                                           const JSClass* clasp) {
  vp::State().windowProxyClass = clasp;
}

JS_PUBLIC_API void js::SetWindowProxy(JSContext* cx,
                                      JS::Handle<JSObject*> global,
                                      JS::Handle<JSObject*> windowProxy) {
  v8::Isolate* isolate = cx->isolate;
  v8::HandleScope scope(isolate);
  v8::Local<v8::Object> g = ToV8(cx, global.get());
  v8::Local<v8::Context> ctx = ContextOf(cx, g);
  v8::Maybe<bool> set =
      g->SetPrivate(ctx, WindowProxyKey(isolate), ToV8(cx, windowProxy.get()));
  MOZ_RELEASE_ASSERT(set.FromMaybe(false));
}

JS_PUBLIC_API bool js::IsWindowProxy(JSObject* obj) {
  if (!Runtime()) {
    return false;
  }
  const JSClass* clasp = vp::State().windowProxyClass;
  return clasp && JS::GetClass(obj) == clasp;
}

JS_PUBLIC_API bool js::detail::IsWindowSlow(JSObject* obj) {
  return MaybeWindowProxy(obj) != nullptr;
}

JS_PUBLIC_API JSObject* js::detail::ToWindowProxyIfWindowSlow(JSObject* obj) {
  if (JSObject* windowProxy = MaybeWindowProxy(obj)) {
    return windowProxy;
  }
  return obj;
}

JS_PUBLIC_API JSObject* js::ToWindowIfWindowProxy(JSObject* obj) {
  if (js::IsWindowProxy(obj)) {
    return vp::ObjectRealm(obj)->global;
  }
  return obj;
}

JS_PUBLIC_API void JS::SetDOMProxyInformation(
    const void* domProxyHandlerFamily,
    JS::DOMProxyShadowsCheck domProxyShadowsCheck,
    const void* domRemoteProxyHandlerFamily) {
  gDOMProxyHandlerFamily = domProxyHandlerFamily;
  gDOMProxyShadowsCheck = domProxyShadowsCheck;
  gDOMRemoteProxyHandlerFamily = domRemoteProxyHandlerFamily;
}
