#ifndef vge_bedrock_VgeV8BindingSupport_h
#define vge_bedrock_VgeV8BindingSupport_h

#include <cmath>
#include <type_traits>

#include "mozilla/ErrorResult.h"
#include "mozilla/RefPtr.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/dom/BindingUtils.h"
#include "mozilla/dom/Nullable.h"
#include "mozilla/dom/WindowProxyHolder.h"
#include "mozilla/dom/Event.h"
#include "mozilla/dom/EventTarget.h"
#include "nsCOMPtr.h"
#include "nsINode.h"
#include "nsString.h"
#include "nsWrapperCache.h"
#include "v8.h"

class nsGlobalWindowInner;

namespace mozilla::dom {
class Document;
}

namespace vge::bedrock::v8b {

using CastFn = void* (*)(int aTargetProto, void* aSelf);
using RefFn = void (*)(void* aSelf);
using TemplateFn = v8::Local<v8::FunctionTemplate> (*)(v8::Isolate* aIsolate);

struct ProtoInfo {
  const char* mName;
  CastFn mCast;
  RefFn mAddRef;
  RefFn mRelease;
  TemplateFn mTemplate;
};

extern const ProtoInfo gProtoInfos[];
extern const int gProtoCount;

template <class T>
struct ProtoIdOf;

int DynamicNodeProto(nsINode* aNode);
void* NodeAsProto(nsINode* aNode, int aProto);
int DynamicEventProto(mozilla::dom::Event* aEvent);
void* EventAsProto(mozilla::dom::Event* aEvent, int aProto);
int WindowProto();

enum class StringNull { Stringify, EmptyString, Null };

v8::Local<v8::FunctionTemplate> CachedTemplate(v8::Isolate* aIsolate,
                                               int aProto);
void StoreTemplate(v8::Isolate* aIsolate, int aProto,
                   v8::Local<v8::FunctionTemplate> aTemplate);

void* Unwrap(v8::Isolate* aIsolate, v8::Local<v8::Value> aValue,
             int aTargetProto);
v8::Local<v8::Value> WrapProto(v8::Isolate* aIsolate, void* aNative,
                               int aProto, nsWrapperCache* aCache);

template <class T>
T* GetRaw(T* aPtr) {
  return aPtr;
}
template <class T>
T* GetRaw(const RefPtr<T>& aPtr) {
  return aPtr.get();
}
template <class T>
T* GetRaw(const nsCOMPtr<T>& aPtr) {
  return aPtr.get();
}
template <class T>
T* GetRaw(mozilla::UniquePtr<T>& aPtr) {
  return aPtr.release();
}

template <class T>
void AddRefNative(void* aPtr) {
  if constexpr (mozilla::dom::IsRefcounted<T>::value) {
    NS_ADDREF(static_cast<T*>(aPtr));
  }
}

template <class T>
void ReleaseNative(void* aPtr) {
  if constexpr (mozilla::dom::IsRefcounted<T>::value) {
    T* native = static_cast<T*>(aPtr);
    NS_RELEASE(native);
  } else {
    delete static_cast<T*>(aPtr);
  }
}

v8::Local<v8::Value> WrapEventTarget(v8::Isolate* aIsolate,
                                     mozilla::dom::EventTarget* aTarget);

template <class T>
v8::Local<v8::Value> Wrap(v8::Isolate* aIsolate, T* aNative) {
  if (!aNative) {
    return v8::Null(aIsolate);
  }
  if constexpr (std::is_same_v<T, mozilla::dom::EventTarget>) {
    return WrapEventTarget(aIsolate, aNative);
  } else if constexpr (std::is_base_of_v<nsINode, T>) {
    nsINode* node = aNative;
    int proto = DynamicNodeProto(node);
    return WrapProto(aIsolate, NodeAsProto(node, proto), proto, node);
  } else if constexpr (std::is_base_of_v<mozilla::dom::Event, T>) {
    mozilla::dom::Event* event = aNative;
    int proto = DynamicEventProto(event);
    return WrapProto(aIsolate, EventAsProto(event, proto), proto, event);
  } else if constexpr (std::is_base_of_v<nsWrapperCache, T>) {
    return WrapProto(aIsolate, aNative, ProtoIdOf<T>::kId,
                     static_cast<nsWrapperCache*>(aNative));
  } else {
    return WrapProto(aIsolate, aNative, ProtoIdOf<T>::kId, nullptr);
  }
}

bool ToString(v8::Isolate* aIsolate, v8::Local<v8::Value> aValue,
              nsAString& aResult, StringNull aNullMode);
bool ToString(v8::Isolate* aIsolate, v8::Local<v8::Value> aValue,
              nsACString& aResult, StringNull aNullMode);
v8::Local<v8::Value> FromString(v8::Isolate* aIsolate,
                                const nsAString& aString);
v8::Local<v8::Value> FromUTF8String(v8::Isolate* aIsolate,
                                    const nsACString& aString);

bool ToNumber(v8::Isolate* aIsolate, v8::Local<v8::Value> aValue,
              double* aResult);
bool ToBoolean(v8::Isolate* aIsolate, v8::Local<v8::Value> aValue);

template <class T>
bool ToInteger(v8::Isolate* aIsolate, v8::Local<v8::Value> aValue,
               T* aResult) {
  double number;
  if (!ToNumber(aIsolate, aValue, &number)) {
    return false;
  }
  if (number != number || number == HUGE_VAL || number == -HUGE_VAL) {
    *aResult = 0;
    return true;
  }
  double truncated = number < 0 ? -floor(-number) : floor(number);
  constexpr double kModulo = 18446744073709551616.0;
  double wrapped = fmod(truncated, kModulo);
  if (wrapped < 0) {
    wrapped += kModulo;
  }
  uint64_t bits = wrapped >= 9223372036854775808.0
                      ? static_cast<uint64_t>(wrapped - 9223372036854775808.0) +
                            0x8000000000000000ull
                      : static_cast<uint64_t>(wrapped);
  *aResult = static_cast<T>(bits);
  return true;
}

bool ToRestrictedDouble(v8::Isolate* aIsolate, v8::Local<v8::Value> aValue,
                        double* aResult, const char* aWhere);

void ThrowTypeError(v8::Isolate* aIsolate, const char* aMessage);
void ThrowErrorResult(v8::Isolate* aIsolate,
                      mozilla::dom::binding_detail::FastErrorResult& aRv,
                      const char* aWhere);

void IllegalConstructor(const v8::FunctionCallbackInfo<v8::Value>& aInfo);

bool InstallMember(const char* aPref);

nsIPrincipal* SubjectPrincipal(v8::Isolate* aIsolate);
nsIPrincipal* NonSystemSubjectPrincipal(v8::Isolate* aIsolate);

v8::Local<v8::Context> WindowContext(v8::Isolate* aIsolate,
                                     nsGlobalWindowInner* aWindow);
v8::Local<v8::Value> WrapWindowProxy(
    v8::Isolate* aIsolate, const mozilla::dom::WindowProxyHolder& aProxy);
v8::Local<v8::Value> WrapWindowProxy(
    v8::Isolate* aIsolate,
    const mozilla::dom::Nullable<mozilla::dom::WindowProxyHolder>& aProxy);
void ForgetWindowContext(nsGlobalWindowInner* aWindow);
void ReleaseAllWrappers();

}

#endif
