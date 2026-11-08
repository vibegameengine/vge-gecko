#ifndef vge_bedrock_jsapi_proxies_VgeJsProxyInternal_h
#define vge_bedrock_jsapi_proxies_VgeJsProxyInternal_h

#include <initializer_list>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "VgeJsInternal.h"
#include "js/CallArgs.h"
#include "js/Wrapper.h"
#include "js/friend/DOMProxy.h"
#include "js/friend/ErrorMessages.h"
#include "js/friend/WindowProxy.h"
#include "js/friend/Wrapper.h"

namespace vge::jsapi::proxy {

enum ReflectOp : int {
  kReflectGet,
  kReflectSet,
  kReflectHas,
  kReflectDefineProperty,
  kReflectDeleteProperty,
  kReflectGetOwnPropertyDescriptor,
  kReflectOwnKeys,
  kReflectGetPrototypeOf,
  kReflectSetPrototypeOf,
  kReflectIsExtensible,
  kReflectPreventExtensions,
  kReflectApply,
  kReflectConstruct,
  kReflectCount
};

struct ProxyRecord {
  bool lazyProto = false;
  bool callable = false;
};

struct RuntimeState {
  JSRuntime* runtime = nullptr;
  v8::Global<v8::ObjectTemplate> plainTemplate;
  v8::Global<v8::ObjectTemplate> callableTemplate;
  v8::Global<v8::ObjectTemplate> undetectableTemplate;
  v8::Global<v8::Context> intrinsics;
  v8::Global<v8::Function> reflect[kReflectCount];
  std::unordered_map<ObjectCell*, ProxyRecord> proxies;
  std::unordered_map<const JS::Value*, ObjectCell*> lazyPrivateSlots;
  std::unordered_set<JS::Realm*> nukedIncoming;
  std::unordered_set<JS::Compartment*> nukedOutgoing;
  std::unordered_set<ObjectCell*> syncingPrototype;
  const JSClass* windowProxyClass = nullptr;
};

RuntimeState& State();

JSObject* const kLazyProto = reinterpret_cast<JSObject*>(0x1);

JS::Realm* ObjectRealm(JSObject* aObj);
JS::Compartment* ObjectCompartment(JSObject* aObj);
bool IsLazyProto(JSObject* aProxy);

class AutoEnterRealmOf {
 public:
  AutoEnterRealmOf(JSContext* aCx, JS::Realm* aRealm);
  AutoEnterRealmOf(JSContext* aCx, JSObject* aObj);
  ~AutoEnterRealmOf();

 private:
  JSContext* mCx;
  JS::Realm* mOld;
};

void ThrowPendingToV8(JSContext* aCx);
bool CallReflect(JSContext* aCx, ReflectOp aOp,
                 std::initializer_list<v8::Local<v8::Value>> aArgs,
                 v8::Local<v8::Value>* aResult);

std::string IdToUTF8(JSContext* aCx, JS::HandleId aId);
std::string ValueToPrintable(JSContext* aCx, JS::HandleValue aValue);
void ReportErrorNumber(JSContext* aCx, unsigned aCode,
                       std::initializer_list<const char*> aArgs);
bool ReportOpFailure(JSContext* aCx, JS::HandleObject aObj, JS::HandleId aId,
                     const JS::ObjectOpResult& aResult);
bool ReportIncompatible(JSContext* aCx, const JS::CallArgs& aArgs);
void ReportNotFunction(JSContext* aCx, JS::HandleValue aValue,
                       unsigned aCode);

v8::Local<v8::Object> DescriptorToV8(JSContext* aCx,
                                     JS::Handle<JS::PropertyDescriptor> aDesc);
bool DescriptorFromV8(JSContext* aCx, v8::Local<v8::Value> aObject,
                      JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>>
                          aDesc);

bool GetOwnPropertyDescriptor(
    JSContext* aCx, JS::HandleObject aObj, JS::HandleId aId,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> aDesc);
bool DefineProperty(JSContext* aCx, JS::HandleObject aObj, JS::HandleId aId,
                    JS::Handle<JS::PropertyDescriptor> aDesc,
                    JS::ObjectOpResult& aResult);
bool OwnPropertyKeys(JSContext* aCx, JS::HandleObject aObj,
                     JS::MutableHandleIdVector aProps);
bool OwnEnumerableStringKeys(JSContext* aCx, JS::HandleObject aObj,
                             JS::MutableHandleIdVector aProps);
bool ForInKeys(JSContext* aCx, JS::HandleObject aObj,
               JS::MutableHandleIdVector aProps);
bool DeleteProperty(JSContext* aCx, JS::HandleObject aObj, JS::HandleId aId,
                    JS::ObjectOpResult& aResult);
bool HasProperty(JSContext* aCx, JS::HandleObject aObj, JS::HandleId aId,
                 bool* aFound);
bool HasOwnProperty(JSContext* aCx, JS::HandleObject aObj, JS::HandleId aId,
                    bool* aFound);
bool GetProperty(JSContext* aCx, JS::HandleObject aObj,
                 JS::HandleValue aReceiver, JS::HandleId aId,
                 JS::MutableHandleValue aVp);
bool SetProperty(JSContext* aCx, JS::HandleObject aObj, JS::HandleId aId,
                 JS::HandleValue aValue, JS::HandleValue aReceiver,
                 JS::ObjectOpResult& aResult);
bool SetPropertyIgnoringNamedGetter(
    JSContext* aCx, JS::HandleObject aObj, JS::HandleId aId,
    JS::HandleValue aValue, JS::HandleValue aReceiver,
    JS::Handle<mozilla::Maybe<JS::PropertyDescriptor>> aOwnDesc,
    JS::ObjectOpResult& aResult);
bool GetPrototype(JSContext* aCx, JS::HandleObject aObj,
                  JS::MutableHandleObject aProto);
bool SetPrototype(JSContext* aCx, JS::HandleObject aObj, JS::HandleObject aProto,
                  JS::ObjectOpResult& aResult);
bool GetPrototypeIfOrdinary(JSContext* aCx, JS::HandleObject aObj,
                            bool* aIsOrdinary, JS::MutableHandleObject aProto);
bool SetImmutablePrototype(JSContext* aCx, JS::HandleObject aObj,
                           bool* aSucceeded);
bool PreventExtensions(JSContext* aCx, JS::HandleObject aObj,
                       JS::ObjectOpResult& aResult);
bool IsExtensible(JSContext* aCx, JS::HandleObject aObj, bool* aExtensible);
bool Call(JSContext* aCx, JS::HandleValue aCallee, JS::HandleValue aThis,
          const JS::CallArgs& aArgs, JS::MutableHandleValue aRval);
bool Construct(JSContext* aCx, JS::HandleValue aCallee,
               const JS::CallArgs& aArgs, JS::HandleValue aNewTarget,
               JS::MutableHandleObject aResult);
bool IsCallableObject(JSObject* aObj);
bool IsConstructorObject(JSObject* aObj);
bool IsArray(JSContext* aCx, JS::HandleObject aObj, JS::IsArrayAnswer* aAnswer);
bool GetBuiltinClass(JSContext* aCx, JS::HandleObject aObj, js::ESClass* aCls);
const char* ClassName(JSContext* aCx, JS::HandleObject aObj);
JSString* FunToString(JSContext* aCx, JS::HandleObject aObj, bool aIsToSource);
bool Unbox(JSContext* aCx, JS::HandleObject aObj, JS::MutableHandleValue aVp);
bool CallGetter(JSContext* aCx, JS::HandleValue aReceiver,
                JS::HandleObject aGetter, JS::MutableHandleValue aVp);
bool CallSetter(JSContext* aCx, JS::HandleValue aReceiver,
                JS::HandleObject aSetter, JS::HandleValue aValue);

bool ProxyGetOwnPropertyDescriptor(
    JSContext* aCx, JS::HandleObject aProxy, JS::HandleId aId,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> aDesc);
bool ProxyDefineProperty(JSContext* aCx, JS::HandleObject aProxy,
                         JS::HandleId aId,
                         JS::Handle<JS::PropertyDescriptor> aDesc,
                         JS::ObjectOpResult& aResult);
bool ProxyOwnPropertyKeys(JSContext* aCx, JS::HandleObject aProxy,
                          JS::MutableHandleIdVector aProps);
bool ProxyDelete(JSContext* aCx, JS::HandleObject aProxy, JS::HandleId aId,
                 JS::ObjectOpResult& aResult);
bool ProxyHas(JSContext* aCx, JS::HandleObject aProxy, JS::HandleId aId,
              bool* aFound);
bool ProxyHasOwn(JSContext* aCx, JS::HandleObject aProxy, JS::HandleId aId,
                 bool* aFound);
bool ProxyGet(JSContext* aCx, JS::HandleObject aProxy,
              JS::HandleValue aReceiver, JS::HandleId aId,
              JS::MutableHandleValue aVp);
bool ProxySet(JSContext* aCx, JS::HandleObject aProxy, JS::HandleId aId,
              JS::HandleValue aValue, JS::HandleValue aReceiver,
              JS::ObjectOpResult& aResult);
bool ProxyCall(JSContext* aCx, JS::HandleObject aProxy,
               const JS::CallArgs& aArgs);
bool ProxyConstruct(JSContext* aCx, JS::HandleObject aProxy,
                    const JS::CallArgs& aArgs);
bool ProxyGetPrototype(JSContext* aCx, JS::HandleObject aProxy,
                       JS::MutableHandleObject aProto);
bool ProxySetPrototype(JSContext* aCx, JS::HandleObject aProxy,
                       JS::HandleObject aProto, JS::ObjectOpResult& aResult);
bool ProxyGetPrototypeIfOrdinary(JSContext* aCx, JS::HandleObject aProxy,
                                 bool* aIsOrdinary,
                                 JS::MutableHandleObject aProto);
bool ProxySetImmutablePrototype(JSContext* aCx, JS::HandleObject aProxy,
                                bool* aSucceeded);
bool ProxyPreventExtensions(JSContext* aCx, JS::HandleObject aProxy,
                            JS::ObjectOpResult& aResult);
bool ProxyIsExtensible(JSContext* aCx, JS::HandleObject aProxy,
                       bool* aExtensible);
bool ProxyGetOwnEnumerablePropertyKeys(JSContext* aCx, JS::HandleObject aProxy,
                                       JS::MutableHandleIdVector aProps);
bool ProxyEnumerate(JSContext* aCx, JS::HandleObject aProxy,
                    JS::MutableHandleIdVector aProps);
const char* ProxyClassName(JSContext* aCx, JS::HandleObject aProxy);
JSString* ProxyFunToString(JSContext* aCx, JS::HandleObject aProxy,
                           bool aIsToSource);
bool ProxyGetElements(JSContext* aCx, JS::HandleObject aProxy, uint32_t aBegin,
                      uint32_t aEnd, js::ElementAdder* aAdder);
bool ProxyGetBuiltinClass(JSContext* aCx, JS::HandleObject aProxy,
                          js::ESClass* aCls);
bool ProxyIsArray(JSContext* aCx, JS::HandleObject aProxy,
                  JS::IsArrayAnswer* aAnswer);
bool ProxyBoxedValueUnbox(JSContext* aCx, JS::HandleObject aProxy,
                          JS::MutableHandleValue aVp);

void SyncLazyPrototype(JSContext* aCx, JSObject* aProxy);
void SwapProxies(JSContext* aCx, JS::HandleObject aA, JS::HandleObject aB);
void NukeProxy(JSContext* aCx, JS::HandleObject aProxy);
JSObject* FindCrossCompartmentWrapper(JS::Compartment* aCompartment,
                                      JSObject* aTarget);
void RemapWrapper(JSContext* aCx, JS::HandleObject aWrapper,
                  JS::HandleObject aNewTarget);

}

#endif
