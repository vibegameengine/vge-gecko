#ifndef vge_bedrock_jsapi_VgeV8Direct_h
#define vge_bedrock_jsapi_VgeV8Direct_h

#include <stdint.h>

#include "VgeJsInternal.h"
#include "mozilla/dom/BindingUtils.h"
#include "mozilla/dom/DOMJSClass.h"
#include "mozilla/dom/DOMString.h"
#include "mozilla/FloatingPoint.h"
#include "nsString.h"

namespace vge::jsapi {

struct WrapperTypeTag;
extern const WrapperTypeTag kWrapperTypeTag;

using DirectInfo = v8::FunctionCallbackInfo<v8::Value>;

void DirectSlowPath(const DirectInfo& aInfo);
void DirectThrowPending(JSContext* aCx);

inline ObjectCell* DirectCell(v8::Local<v8::Value> aValue) {
  if (!aValue->IsObject()) {
    return nullptr;
  }
  v8::Local<v8::Object> object = aValue.As<v8::Object>();
  if (object->InternalFieldCount() != 2 ||
      object->GetAlignedPointerFromInternalField(0) != &kWrapperTypeTag) {
    return nullptr;
  }
  Anchor* anchor =
      static_cast<Anchor*>(object->GetAlignedPointerFromInternalField(1));
  return anchor ? anchor->Cell() : nullptr;
}

inline void* DirectUnwrap(v8::Local<v8::Value> aValue, uint32_t aProtoID,
                          uint32_t aDepth) {
  ObjectCell* cell = DirectCell(aValue);
  if (!cell || cell->shape->isProxy) {
    return nullptr;
  }
  const JSClass* clasp = cell->shape->base.clasp;
  if (!clasp->isDOMClass()) {
    return nullptr;
  }
  const mozilla::dom::DOMJSClass* domClass =
      mozilla::dom::DOMJSClass::FromJSClass(clasp);
  if (uint32_t(domClass->mInterfaceChain[aDepth]) != aProtoID) {
    return nullptr;
  }
  const JS::Value& native = cell->fixedSlots()[DOM_OBJECT_SLOT];
  return native.isUndefined() ? nullptr : native.toPrivate();
}

class DirectFrame {
 public:
  explicit DirectFrame(const DirectInfo& aInfo)
      : mCx(CurrentContext()), mOld(mCx->realm()) {
    Anchor* anchor = static_cast<Anchor*>(
        aInfo.Data().As<v8::Object>()->GetAlignedPointerFromInternalField(1));
    JS::Realm* realm = anchor->Cell()->shape->base.realm;
    mEntered = realm != mOld;
    if (mEntered) {
      mCx->enterRealm(realm);
    }
  }
  ~DirectFrame() {
    if (mEntered) {
      mCx->leaveRealm(mOld);
    }
  }
  JSContext* cx() const { return mCx; }

 private:
  JSContext* mCx;
  JS::Realm* mOld;
  bool mEntered = false;
};

inline void DirectAssignString(v8::Isolate* aIsolate,
                               v8::Local<v8::Value> aValue, nsAString& aOut) {
  v8::Local<v8::String> string = aValue.As<v8::String>();
  int length = string->Length();
  aOut.SetLength(length);
  string->Write(aIsolate, reinterpret_cast<uint16_t*>(aOut.BeginWriting()), 0,
                length, v8::String::NO_NULL_TERMINATION);
}

inline bool DirectReturnString(JSContext* aCx, const DirectInfo& aInfo,
                               const nsAString& aValue, bool aNullable) {
  if (aNullable && aValue.IsVoid()) {
    aInfo.GetReturnValue().SetNull();
    return true;
  }
  v8::Local<v8::String> string;
  if (!v8::String::NewFromTwoByte(
           aInfo.GetIsolate(),
           reinterpret_cast<const uint16_t*>(aValue.BeginReading()),
           v8::NewStringType::kNormal, int(aValue.Length()))
           .ToLocal(&string)) {
    JS_ReportOutOfMemory(aCx);
    return false;
  }
  aInfo.GetReturnValue().Set(string);
  return true;
}

inline bool DirectFitsInt32(v8::Local<v8::Value> aValue, int32_t aMin,
                            int32_t aMax) {
  if (!aValue->IsInt32()) {
    return false;
  }
  int32_t value = aValue.As<v8::Int32>()->Value();
  return value >= aMin && value <= aMax;
}

inline bool DirectIsFiniteDouble(v8::Local<v8::Value> aValue) {
  return aValue->IsNumber() &&
         std::isfinite(aValue.As<v8::Number>()->Value());
}

inline bool DirectIsFiniteFloat(v8::Local<v8::Value> aValue) {
  return aValue->IsNumber() &&
         std::isfinite(float(aValue.As<v8::Number>()->Value()));
}

}

#endif
