#include "VgeJsBuffers.h"

#include <stdio.h>
#include <string.h>

#include "js/ErrorReport.h"
#include "js/Proxy.h"
#include "js/Utility.h"
#include "js/Wrapper.h"
#include "mozilla/CheckedInt.h"
#include "v8-typed-array.h"

#include <windows.h>

namespace vge::jsapi::buffers {

namespace {

template <typename Tag, typename Tag::type Member>
struct ClassPtrAccess {
  friend typename Tag::type Get(Tag) { return Member; }
};

struct DataViewFixedTag {
  using type = const JSClass* const*;
  friend type Get(DataViewFixedTag);
};
struct DataViewResizableTag {
  using type = const JSClass* const*;
  friend type Get(DataViewResizableTag);
};

struct DataViewImmutableTag {
  using type = const JSClass* const*;
  friend type Get(DataViewImmutableTag);
};

template struct ClassPtrAccess<DataViewFixedTag,
                               &JS::DataView::FixedLengthClassPtr>;
template struct ClassPtrAccess<DataViewImmutableTag,
                               &JS::DataView::ImmutableClassPtr>;
template struct ClassPtrAccess<DataViewResizableTag,
                               &JS::DataView::ResizableClassPtr>;

const JSClass* DataViewClass(bool aResizable) {
  return aResizable ? *Get(DataViewResizableTag()) : *Get(DataViewFixedTag());
}

const JSClass* TypedArrayClass(JS::Scalar::Type aType, bool aResizable) {
  return aResizable ? &TypedArrayResizableClasses[aType]
                    : &TypedArrayFixedClasses[aType];
}

bool TypedArrayClassType(const JSClass* aClass, JS::Scalar::Type* aType) {
  for (size_t i = 0; i < JS::Scalar::MaxTypedArrayViewType; i++) {
    if (aClass == &TypedArrayFixedClasses[i] ||
        aClass == &TypedArrayResizableClasses[i] ||
        aClass == &TypedArrayImmutableClasses[i]) {
      *aType = JS::Scalar::Type(i);
      return true;
    }
  }
  return false;
}

JSContext* Cx() { return CurrentContext(); }

v8::Local<v8::Context> AnyContext(JSContext* aCx) {
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

void ReportError(JSContext* aCx, unsigned aNumber) {
  JS_ReportErrorNumberASCII(aCx, js::GetErrorMessage, nullptr, aNumber);
}

v8::Local<v8::Object> UnwrappedV8(JSContext* aCx, JSObject* aObj) {
  JSObject* unwrapped = Unwrapped(aObj);
  if (!unwrapped) {
    return v8::Local<v8::Object>();
  }
  return V8Of(aCx, unwrapped);
}

v8::Local<v8::ArrayBufferView> ViewOf(JSContext* aCx, JSObject* aObj) {
  v8::Local<v8::Object> obj = UnwrappedV8(aCx, aObj);
  if (obj.IsEmpty() || !obj->IsArrayBufferView()) {
    return v8::Local<v8::ArrayBufferView>();
  }
  return obj.As<v8::ArrayBufferView>();
}

v8::Local<v8::Object> BufferOf(JSContext* aCx, JSObject* aObj,
                               bool aAllowShared) {
  v8::Local<v8::Object> obj = UnwrappedV8(aCx, aObj);
  if (obj.IsEmpty()) {
    return obj;
  }
  if (obj->IsArrayBuffer() || (aAllowShared && obj->IsSharedArrayBuffer())) {
    return obj;
  }
  return v8::Local<v8::Object>();
}

bool StoreResizable(v8::Local<v8::Object> aBuffer) {
  if (aBuffer->IsArrayBuffer()) {
    return aBuffer.As<v8::ArrayBuffer>()
        ->GetBackingStore()
        ->IsResizableByUserJavaScript();
  }
  if (aBuffer->IsSharedArrayBuffer()) {
    return aBuffer.As<v8::SharedArrayBuffer>()
        ->GetBackingStore()
        ->IsResizableByUserJavaScript();
  }
  return false;
}

bool ViewResizable(v8::Local<v8::ArrayBufferView> aView) {
  if (!aView->HasBuffer()) {
    return false;
  }
  return aView->Buffer()->GetBackingStore()->IsResizableByUserJavaScript();
}

bool ViewIsShared(v8::Local<v8::ArrayBufferView> aView) {
  if (!aView->HasBuffer()) {
    return false;
  }
  return aView->Buffer()->GetBackingStore()->IsShared();
}

v8::Local<v8::TypedArray> MakeView(JS::Scalar::Type aType,
                                   v8::Local<v8::Object> aBuffer,
                                   size_t aOffset, size_t aLength) {
  bool shared = aBuffer->IsSharedArrayBuffer();
#define VGE_MAKE(V8Type)                                                    \
  if (shared) {                                                             \
    return v8::V8Type::New(aBuffer.As<v8::SharedArrayBuffer>(), aOffset,    \
                           aLength);                                        \
  }                                                                         \
  return v8::V8Type::New(aBuffer.As<v8::ArrayBuffer>(), aOffset, aLength);
  switch (aType) {
    case JS::Scalar::Int8:
      VGE_MAKE(Int8Array)
    case JS::Scalar::Uint8:
      VGE_MAKE(Uint8Array)
    case JS::Scalar::Int16:
      VGE_MAKE(Int16Array)
    case JS::Scalar::Uint16:
    case JS::Scalar::Float16:
      VGE_MAKE(Uint16Array)
    case JS::Scalar::Int32:
      VGE_MAKE(Int32Array)
    case JS::Scalar::Uint32:
      VGE_MAKE(Uint32Array)
    case JS::Scalar::Float32:
      VGE_MAKE(Float32Array)
    case JS::Scalar::Float64:
      VGE_MAKE(Float64Array)
    case JS::Scalar::Uint8Clamped:
      VGE_MAKE(Uint8ClampedArray)
    case JS::Scalar::BigInt64:
      VGE_MAKE(BigInt64Array)
    case JS::Scalar::BigUint64:
      VGE_MAKE(BigUint64Array)
    default:
      MOZ_CRASH("vge buffers: not a typed array type");
  }
#undef VGE_MAKE
}

JS::Realm* RealmFor(JSContext* aCx, v8::Local<v8::Object> aObject) {
  v8::Local<v8::Context> creation;
  if (aObject->GetCreationContext().ToLocal(&creation)) {
    for (JS::Realm* r : aCx->runtime->realms) {
      if (!r->context.IsEmpty() && r->context.Get(aCx->isolate) == creation) {
        return r;
      }
    }
  }
  return aCx->realm();
}

JSObject* NewCell(JSContext* aCx, v8::Local<v8::Object> aObject,
                  const JSClass* aClass) {
  Shape* shape = ShapeFor(aClass, RealmFor(aCx, aObject));
  return NewObjectCell(aCx, shape, aObject, false)->asObject();
}

void ReportTooLarge(JSContext* aCx, JS::Scalar::Type aType) {
  JS_ReportErrorNumberASCII(aCx, js::GetErrorMessage, nullptr,
                            JSMSG_TYPED_ARRAY_CONSTRUCT_TOO_LARGE,
                            ScalarName(aType));
}

JSObject* NewTypedArray(JSContext* aCx, JS::Scalar::Type aType,
                        size_t aNelements) {
  mozilla::CheckedInt<size_t> bytes =
      mozilla::CheckedInt<size_t>(aNelements) * ElementSize(aType);
  if (!bytes.isValid() || aNelements > v8::TypedArray::kMaxLength) {
    ReportTooLarge(aCx, aType);
    return nullptr;
  }
  AutoV8 v8(aCx);
  v8::Local<v8::ArrayBuffer> buffer =
      v8::ArrayBuffer::New(aCx->isolate, bytes.value());
  v8::Local<v8::TypedArray> view = MakeView(aType, buffer, 0, aNelements);
  return NewCell(aCx, view, TypedArrayClass(aType, false));
}

JSObject* NewTypedArrayFromArray(JSContext* aCx, JS::Scalar::Type aType,
                                 JS::HandleObject aOther) {
  AutoV8 v8(aCx);
  v8::TryCatch tryCatch(aCx->isolate);
  v8::Local<v8::Context> ctx = AnyContext(aCx);
  v8::Local<v8::Object> source = V8Of(aCx, aOther);
  uint64_t length = 0;
  if (source->IsTypedArray()) {
    length = source.As<v8::TypedArray>()->Length();
  } else {
    v8::Local<v8::Value> lengthValue;
    v8::Local<v8::Integer> lengthInt;
    if (!source
             ->Get(ctx, v8::String::NewFromUtf8Literal(aCx->isolate, "length"))
             .ToLocal(&lengthValue) ||
        !lengthValue->ToInteger(ctx).ToLocal(&lengthInt)) {
      v8.Check(tryCatch);
      return nullptr;
    }
    int64_t raw = lengthInt->Value();
    length = raw < 0 ? 0 : uint64_t(raw);
  }
  if (length > v8::TypedArray::kMaxLength) {
    ReportTooLarge(aCx, aType);
    return nullptr;
  }
  v8::Local<v8::ArrayBuffer> buffer = v8::ArrayBuffer::New(
      aCx->isolate, size_t(length) * ElementSize(aType));
  v8::Local<v8::TypedArray> view = MakeView(aType, buffer, 0, size_t(length));
  for (uint32_t i = 0; i < length; i++) {
    v8::Local<v8::Value> element;
    if (!source->Get(ctx, i).ToLocal(&element) ||
        view->Set(ctx, i, element).IsNothing()) {
      v8.Check(tryCatch);
      return nullptr;
    }
  }
  return NewCell(aCx, view, TypedArrayClass(aType, false));
}

template <typename T>
T* TypedArrayDataOf(JSObject* aObj, JS::Scalar::Type aType,
                    bool* aIsSharedMemory) {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  JSObject* unwrapped = Unwrapped(aObj);
  if (!unwrapped) {
    return nullptr;
  }
  v8::Local<v8::Object> obj = V8Of(cx, unwrapped);
  if (!obj->IsTypedArray()) {
    return nullptr;
  }
  JS::Scalar::Type type;
  if (!TypedArrayClassType(JS::GetClass(unwrapped), &type)) {
    type = ScalarOf(obj);
  }
  if (type != aType) {
    return nullptr;
  }
  v8::Local<v8::TypedArray> view = obj.As<v8::TypedArray>();
  *aIsSharedMemory = ViewIsShared(view);
  return reinterpret_cast<T*>(ViewData(view));
}

v8::Local<v8::Object> UnwrapOrReportArrayBuffer(JSContext* aCx,
                                                JS::HandleObject aObj) {
  JSObject* unwrapped = js::CheckedUnwrapStatic(aObj);
  if (!unwrapped) {
    js::ReportAccessDenied(aCx);
    return v8::Local<v8::Object>();
  }
  v8::Local<v8::Object> obj = V8Of(aCx, unwrapped);
  if (!obj->IsArrayBuffer()) {
    ReportError(aCx, JSMSG_ARRAYBUFFER_REQUIRED);
    return v8::Local<v8::Object>();
  }
  return obj;
}

bool BufferPinned(JSContext* aCx, v8::Local<v8::Object> aBuffer) {
  v8::Local<v8::Value> pinKey;
  return aBuffer
             ->GetPrivate(AnyContext(aCx),
                          PrivateKey(aCx->isolate, "vge.buffers.pinned"))
             .ToLocal(&pinKey) &&
         pinKey->IsSymbol();
}

void FreeJsContents(void* aData, size_t aLength, void* aDeleterData) {
  js_free(aData);
}

void FreeMappedContents(void* aData, size_t aLength, void* aDeleterData) {
  JS::ReleaseMappedArrayBufferContents(aData, aLength);
}

struct ExternalContents {
  JS::BufferContentsFreeFunc freeFunc;
  void* userData;
};

void FreeExternalContents(void* aData, size_t aLength, void* aDeleterData) {
  ExternalContents* external = static_cast<ExternalContents*>(aDeleterData);
  external->freeFunc(aData, external->userData);
  delete external;
}

JSObject* NewBufferForContents(JSContext* aCx, size_t aNbytes, void* aData,
                               v8::BackingStore::DeleterCallback aDeleter,
                               void* aDeleterData) {
  if (aNbytes > v8::TypedArray::kMaxLength) {
    ReportError(aCx, JSMSG_BAD_ARRAY_LENGTH);
    return nullptr;
  }
  std::unique_ptr<v8::BackingStore> store = v8::ArrayBuffer::NewBackingStore(
      aData, aNbytes, aDeleter, aDeleterData);
  return NewArrayBufferFromStore(aCx, std::move(store));
}

}

const JSClass ArrayBufferClasses[5] = {
    {"ArrayBuffer", 0},       {"ArrayBuffer", 0},
    {"ArrayBuffer", 0},       {"SharedArrayBuffer", 0},
    {"SharedArrayBuffer", 0},
};

#define VGE_TA_CLASS(ExternalType, NativeType, Name) {#Name "Array", 0},
const JSClass TypedArrayFixedClasses[JS::Scalar::MaxTypedArrayViewType] = {
    JS_FOR_EACH_TYPED_ARRAY(VGE_TA_CLASS)};
const JSClass TypedArrayResizableClasses[JS::Scalar::MaxTypedArrayViewType] = {
    JS_FOR_EACH_TYPED_ARRAY(VGE_TA_CLASS)};
const JSClass TypedArrayImmutableClasses[JS::Scalar::MaxTypedArrayViewType] = {
    JS_FOR_EACH_TYPED_ARRAY(VGE_TA_CLASS)};
#undef VGE_TA_CLASS

v8::Local<v8::Private> PrivateKey(v8::Isolate* aIsolate, const char* aName) {
  return v8::Private::ForApi(
      aIsolate, v8::String::NewFromUtf8(aIsolate, aName).ToLocalChecked());
}

JSObject* Unwrapped(JSObject* aObj) {
  if (!aObj) {
    return nullptr;
  }
  if (js::IsProxy(aObj)) {
    return js::CheckedUnwrapStatic(aObj);
  }
  return aObj;
}

v8::Local<v8::Object> V8Of(JSContext* aCx, JSObject* aObj) {
  return ToV8(aCx, aObj);
}

JS::Scalar::Type ScalarOf(v8::Local<v8::Value> aValue) {
  if (aValue->IsInt8Array()) {
    return JS::Scalar::Int8;
  }
  if (aValue->IsUint8Array()) {
    return JS::Scalar::Uint8;
  }
  if (aValue->IsInt16Array()) {
    return JS::Scalar::Int16;
  }
  if (aValue->IsUint16Array()) {
    return JS::Scalar::Uint16;
  }
  if (aValue->IsInt32Array()) {
    return JS::Scalar::Int32;
  }
  if (aValue->IsUint32Array()) {
    return JS::Scalar::Uint32;
  }
  if (aValue->IsFloat32Array()) {
    return JS::Scalar::Float32;
  }
  if (aValue->IsFloat64Array()) {
    return JS::Scalar::Float64;
  }
  if (aValue->IsUint8ClampedArray()) {
    return JS::Scalar::Uint8Clamped;
  }
  if (aValue->IsBigInt64Array()) {
    return JS::Scalar::BigInt64;
  }
  if (aValue->IsBigUint64Array()) {
    return JS::Scalar::BigUint64;
  }
  return JS::Scalar::MaxTypedArrayViewType;
}

size_t ElementSize(JS::Scalar::Type aType) {
  switch (aType) {
    case JS::Scalar::Int16:
    case JS::Scalar::Uint16:
    case JS::Scalar::Float16:
      return 2;
    case JS::Scalar::Int32:
    case JS::Scalar::Uint32:
    case JS::Scalar::Float32:
      return 4;
    case JS::Scalar::Float64:
    case JS::Scalar::BigInt64:
    case JS::Scalar::BigUint64:
    case JS::Scalar::Int64:
      return 8;
    case JS::Scalar::Simd128:
      return 16;
    default:
      return 1;
  }
}

const char* ScalarName(JS::Scalar::Type aType) {
  switch (aType) {
#define VGE_NAME(ExternalType, NativeType, Name) \
  case JS::Scalar::Name:                         \
    return #Name;
    JS_FOR_EACH_TYPED_ARRAY(VGE_NAME)
#undef VGE_NAME
    default:
      return "";
  }
}

const JSClass* ClassForV8(v8::Local<v8::Object> aObject) {
  if (aObject->IsArrayBuffer()) {
    return StoreResizable(aObject) ? &ArrayBufferClasses[kResizableUnshared]
                                   : &ArrayBufferClasses[kFixedUnshared];
  }
  if (aObject->IsSharedArrayBuffer()) {
    return StoreResizable(aObject) ? &ArrayBufferClasses[kGrowableShared]
                                   : &ArrayBufferClasses[kFixedShared];
  }
  if (aObject->IsTypedArray()) {
    return TypedArrayClass(ScalarOf(aObject),
                           ViewResizable(aObject.As<v8::ArrayBufferView>()));
  }
  if (aObject->IsDataView()) {
    return DataViewClass(ViewResizable(aObject.As<v8::ArrayBufferView>()));
  }
  return nullptr;
}

bool IsBufferClass(const JSClass* aClass) {
  JS::Scalar::Type type;
  if (TypedArrayClassType(aClass, &type)) {
    return true;
  }
  for (const JSClass& c : ArrayBufferClasses) {
    if (aClass == &c) {
      return true;
    }
  }
  return aClass == DataViewClass(false) || aClass == DataViewClass(true) ||
         aClass == *Get(DataViewImmutableTag());
}

JSObject* BufferObjectFromV8(JSContext* aCx, v8::Local<v8::Object> aObject) {
  const JSClass* clasp = ClassForV8(aObject);
  v8::Local<v8::Value> existing;
  if (aObject
          ->GetPrivate(AnyContext(aCx),
                       aCx->runtime->cellKey.Get(aCx->isolate))
          .ToLocal(&existing) &&
      existing->IsExternal()) {
    ObjectCell* cell =
        static_cast<ObjectCell*>(existing.As<v8::External>()->Value());
    if (clasp && !cell->shape->isProxy &&
        cell->shape->base.clasp == js::ObjectClassPtr) {
      cell->shape = ShapeFor(clasp, cell->shape->base.realm);
    }
    return cell->asObject();
  }
  if (!clasp) {
    return ObjectFromV8(aCx, aObject);
  }
  return NewCell(aCx, aObject, clasp);
}

uint8_t* ViewData(v8::Local<v8::ArrayBufferView> aView) {
  uint8_t* base = static_cast<uint8_t*>(aView->Buffer()->Data());
  if (!base) {
    return nullptr;
  }
  return base + aView->ByteOffset();
}

uint8_t* BufferData(v8::Local<v8::Object> aBuffer) {
  if (aBuffer->IsArrayBuffer()) {
    return static_cast<uint8_t*>(aBuffer.As<v8::ArrayBuffer>()->Data());
  }
  if (aBuffer->IsSharedArrayBuffer()) {
    return static_cast<uint8_t*>(aBuffer.As<v8::SharedArrayBuffer>()->Data());
  }
  return nullptr;
}

size_t BufferLength(v8::Local<v8::Object> aBuffer) {
  if (aBuffer->IsArrayBuffer()) {
    return aBuffer.As<v8::ArrayBuffer>()->ByteLength();
  }
  if (aBuffer->IsSharedArrayBuffer()) {
    return aBuffer.As<v8::SharedArrayBuffer>()->ByteLength();
  }
  return 0;
}

bool BufferDetached(v8::Local<v8::Object> aBuffer) {
  return aBuffer->IsArrayBuffer() &&
         aBuffer.As<v8::ArrayBuffer>()->WasDetached();
}

JSObject* NewArrayBufferFromStore(JSContext* aCx,
                                  std::shared_ptr<v8::BackingStore> aStore) {
  AutoV8 v8(aCx);
  v8::Local<v8::ArrayBuffer> buffer =
      v8::ArrayBuffer::New(aCx->isolate, std::move(aStore));
  return BufferObjectFromV8(aCx, buffer);
}

JSObject* NewTypedArrayWithBuffer(JSContext* aCx, JS::Scalar::Type aType,
                                  JS::HandleObject aBuffer, size_t aByteOffset,
                                  int64_t aLength) {
  AutoV8 v8(aCx);
  JSObject* unwrapped = Unwrapped(aBuffer);
  if (!unwrapped) {
    js::ReportAccessDenied(aCx);
    return nullptr;
  }
  v8::Local<v8::Object> buffer = V8Of(aCx, unwrapped);
  if (!buffer->IsArrayBuffer() && !buffer->IsSharedArrayBuffer()) {
    ReportError(aCx, JSMSG_TYPED_ARRAY_BAD_ARGS);
    return nullptr;
  }
  size_t elemSize = ElementSize(aType);
  const char* name = ScalarName(aType);
  char size[8];
  snprintf(size, sizeof(size), "%u", unsigned(elemSize));
  if (aByteOffset % elemSize != 0) {
    JS_ReportErrorNumberASCII(aCx, js::GetErrorMessage, nullptr,
                              JSMSG_TYPED_ARRAY_CONSTRUCT_OFFSET_BOUNDS, name,
                              size);
    return nullptr;
  }
  if (BufferDetached(buffer)) {
    ReportError(aCx, JSMSG_TYPED_ARRAY_DETACHED);
    return nullptr;
  }
  size_t bufferLength = BufferLength(buffer);
  size_t length;
  if (aLength < 0) {
    if (!StoreResizable(buffer) && bufferLength % elemSize != 0) {
      JS_ReportErrorNumberASCII(aCx, js::GetErrorMessage, nullptr,
                                JSMSG_TYPED_ARRAY_CONSTRUCT_OFFSET_MISALIGNED,
                                name, size);
      return nullptr;
    }
    if (aByteOffset > bufferLength) {
      JS_ReportErrorNumberASCII(
          aCx, js::GetErrorMessage, nullptr,
          JSMSG_TYPED_ARRAY_CONSTRUCT_OFFSET_LENGTH_BOUNDS, name);
      return nullptr;
    }
    length = (bufferLength - aByteOffset) / elemSize;
  } else {
    mozilla::CheckedInt<size_t> end =
        mozilla::CheckedInt<size_t>(size_t(aLength)) * elemSize + aByteOffset;
    if (!end.isValid() || end.value() > bufferLength) {
      JS_ReportErrorNumberASCII(
          aCx, js::GetErrorMessage, nullptr,
          JSMSG_TYPED_ARRAY_CONSTRUCT_ARRAY_LENGTH_BOUNDS, name);
      return nullptr;
    }
    length = size_t(aLength);
  }
  if (length > v8::TypedArray::kMaxLength) {
    ReportTooLarge(aCx, aType);
    return nullptr;
  }
  v8::Local<v8::TypedArray> view = MakeView(aType, buffer, aByteOffset, length);
  return NewCell(aCx, view, TypedArrayClass(aType, StoreResizable(buffer)));
}

JSObject* NewDataViewObject(JSContext* aCx, JS::HandleObject aBuffer,
                            size_t aByteOffset,
                            mozilla::Maybe<size_t> aLength) {
  AutoV8 v8(aCx);
  JSObject* unwrapped = Unwrapped(aBuffer);
  if (!unwrapped) {
    js::ReportAccessDenied(aCx);
    return nullptr;
  }
  v8::Local<v8::Object> buffer = V8Of(aCx, unwrapped);
  if (!buffer->IsArrayBuffer() && !buffer->IsSharedArrayBuffer()) {
    JS_ReportErrorNumberASCII(aCx, js::GetErrorMessage, nullptr,
                              JSMSG_NOT_EXPECTED_TYPE, "DataView",
                              "ArrayBuffer", "object");
    return nullptr;
  }
  if (BufferDetached(buffer)) {
    ReportError(aCx, JSMSG_TYPED_ARRAY_DETACHED);
    return nullptr;
  }
  size_t bufferLength = BufferLength(buffer);
  if (aByteOffset > bufferLength) {
    ReportError(aCx, JSMSG_OFFSET_OUT_OF_BUFFER);
    return nullptr;
  }
  size_t length = bufferLength - aByteOffset;
  if (aLength) {
    if (*aLength > length) {
      ReportError(aCx, JSMSG_INVALID_DATA_VIEW_LENGTH);
      return nullptr;
    }
    length = *aLength;
  }
  v8::Local<v8::DataView> view =
      buffer->IsSharedArrayBuffer()
          ? v8::DataView::New(buffer.As<v8::SharedArrayBuffer>(), aByteOffset,
                              length)
          : v8::DataView::New(buffer.As<v8::ArrayBuffer>(), aByteOffset,
                              length);
  return NewCell(aCx, view, DataViewClass(StoreResizable(buffer)));
}

}

using namespace vge::jsapi;
using namespace vge::jsapi::buffers;

const JSClass* const JS::ArrayBuffer::FixedLengthUnsharedClass =
    &ArrayBufferClasses[kFixedUnshared];
const JSClass* const JS::ArrayBuffer::ResizableUnsharedClass =
    &ArrayBufferClasses[kResizableUnshared];
const JSClass* const JS::ArrayBuffer::ImmutableUnsharedClass =
    &ArrayBufferClasses[kImmutableUnshared];
const JSClass* const JS::ArrayBuffer::FixedLengthSharedClass =
    &ArrayBufferClasses[kFixedShared];
const JSClass* const JS::ArrayBuffer::GrowableSharedClass =
    &ArrayBufferClasses[kGrowableShared];

const JSClass* const JS::TypedArray_base::fixedLengthClasses =
    TypedArrayFixedClasses;
const JSClass* const JS::TypedArray_base::resizableClasses =
    TypedArrayResizableClasses;
const JSClass* const JS::TypedArray_base::immutableClasses =
    TypedArrayImmutableClasses;

#define VGE_TYPED_ARRAY_NEW(Name)                                    \
  JSObject* JS_New##Name##Array(JSContext* aCx, size_t aNelements) { \
    return NewTypedArray(aCx, JS::Scalar::Name, aNelements);         \
  }

VGE_TYPED_ARRAY_NEW(Int8)
VGE_TYPED_ARRAY_NEW(Uint8)
VGE_TYPED_ARRAY_NEW(Int16)
VGE_TYPED_ARRAY_NEW(Uint16)
VGE_TYPED_ARRAY_NEW(Int32)
VGE_TYPED_ARRAY_NEW(Uint32)
VGE_TYPED_ARRAY_NEW(Float32)
VGE_TYPED_ARRAY_NEW(Float64)
VGE_TYPED_ARRAY_NEW(Uint8Clamped)
VGE_TYPED_ARRAY_NEW(BigInt64)
VGE_TYPED_ARRAY_NEW(BigUint64)
#undef VGE_TYPED_ARRAY_NEW

#define VGE_TYPED_ARRAY_WITH_BUFFER(Name)                                     \
  JSObject* JS_New##Name##ArrayWithBuffer(JSContext* aCx,                    \
                                          JS::HandleObject aBuffer,          \
                                          size_t aByteOffset,                \
                                          int64_t aLength) {                 \
    return NewTypedArrayWithBuffer(aCx, JS::Scalar::Name, aBuffer,           \
                                   aByteOffset, aLength);                    \
  }

VGE_TYPED_ARRAY_WITH_BUFFER(Int8)
VGE_TYPED_ARRAY_WITH_BUFFER(Uint8)
VGE_TYPED_ARRAY_WITH_BUFFER(Int16)
VGE_TYPED_ARRAY_WITH_BUFFER(Uint16)
VGE_TYPED_ARRAY_WITH_BUFFER(Int32)
VGE_TYPED_ARRAY_WITH_BUFFER(Uint32)
VGE_TYPED_ARRAY_WITH_BUFFER(Float32)
VGE_TYPED_ARRAY_WITH_BUFFER(Float64)
VGE_TYPED_ARRAY_WITH_BUFFER(Uint8Clamped)
VGE_TYPED_ARRAY_WITH_BUFFER(BigInt64)
VGE_TYPED_ARRAY_WITH_BUFFER(BigUint64)
VGE_TYPED_ARRAY_WITH_BUFFER(Float16)
#undef VGE_TYPED_ARRAY_WITH_BUFFER

JSObject* JS_NewUint8ArrayFromArray(JSContext* aCx, JS::HandleObject aOther) {
  return NewTypedArrayFromArray(aCx, JS::Scalar::Uint8, aOther);
}

float* JS_GetFloat32ArrayData(JSObject* aObj, bool* aIsSharedMemory,
                              const JS::AutoRequireNoGC&) {
  return TypedArrayDataOf<float>(aObj, JS::Scalar::Float32, aIsSharedMemory);
}

uint8_t* JS_GetUint8ClampedArrayData(JSObject* aObj, bool* aIsSharedMemory,
                                     const JS::AutoRequireNoGC&) {
  return TypedArrayDataOf<uint8_t>(aObj, JS::Scalar::Uint8Clamped,
                                   aIsSharedMemory);
}

bool JS_IsTypedArrayObject(JSObject* aObj) {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  JSObject* unwrapped = Unwrapped(aObj);
  if (!unwrapped) {
    return false;
  }
  v8::Local<v8::Object> obj = V8Of(cx, unwrapped);
  if (!obj->IsTypedArray()) {
    return false;
  }
  BufferObjectFromV8(cx, obj);
  return true;
}

bool JS_IsArrayBufferViewObject(JSObject* aObj) {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  JSObject* unwrapped = Unwrapped(aObj);
  if (!unwrapped) {
    return false;
  }
  v8::Local<v8::Object> obj = V8Of(cx, unwrapped);
  if (!obj->IsArrayBufferView()) {
    return false;
  }
  BufferObjectFromV8(cx, obj);
  return true;
}

bool JS_GetTypedArraySharedness(JSObject* aObj) {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::ArrayBufferView> view = ViewOf(cx, aObj);
  if (view.IsEmpty() || !view->IsTypedArray()) {
    return false;
  }
  return ViewIsShared(view);
}

JS::Scalar::Type JS_GetArrayBufferViewType(JSObject* aObj) {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  JSObject* unwrapped = Unwrapped(aObj);
  if (!unwrapped) {
    return JS::Scalar::MaxTypedArrayViewType;
  }
  v8::Local<v8::Object> obj = V8Of(cx, unwrapped);
  if (!obj->IsTypedArray()) {
    return JS::Scalar::MaxTypedArrayViewType;
  }
  JS::Scalar::Type type;
  if (TypedArrayClassType(JS::GetClass(unwrapped), &type)) {
    return type;
  }
  return ScalarOf(obj);
}

size_t JS_GetTypedArrayLength(JSObject* aObj) {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::ArrayBufferView> view = ViewOf(cx, aObj);
  if (view.IsEmpty() || !view->IsTypedArray()) {
    return 0;
  }
  return view.As<v8::TypedArray>()->Length();
}

size_t JS_GetTypedArrayByteOffset(JSObject* aObj) {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::ArrayBufferView> view = ViewOf(cx, aObj);
  if (view.IsEmpty() || !view->IsTypedArray()) {
    return 0;
  }
  return view->ByteOffset();
}

size_t JS_GetTypedArrayByteLength(JSObject* aObj) {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::ArrayBufferView> view = ViewOf(cx, aObj);
  if (view.IsEmpty() || !view->IsTypedArray()) {
    return 0;
  }
  return view->ByteLength();
}

size_t JS_GetArrayBufferViewByteLength(JSObject* aObj) {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::ArrayBufferView> view = ViewOf(cx, aObj);
  return view.IsEmpty() ? 0 : view->ByteLength();
}

size_t JS_GetArrayBufferViewByteOffset(JSObject* aObj) {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::ArrayBufferView> view = ViewOf(cx, aObj);
  return view.IsEmpty() ? 0 : view->ByteOffset();
}

void* JS_GetArrayBufferViewData(JSObject* aObj, bool* aIsSharedMemory,
                                const JS::AutoRequireNoGC&) {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::ArrayBufferView> view = ViewOf(cx, aObj);
  if (view.IsEmpty()) {
    return nullptr;
  }
  *aIsSharedMemory = ViewIsShared(view);
  return ViewData(view);
}

JSObject* JS_GetArrayBufferViewBuffer(JSContext* aCx, JS::HandleObject aObj,
                                      bool* aIsSharedMemory) {
  JS::RootedObject result(aCx);
  {
    AutoV8 v8(aCx);
    v8::Local<v8::ArrayBufferView> view = ViewOf(aCx, aObj);
    if (view.IsEmpty()) {
      js::ReportAccessDenied(aCx);
      return nullptr;
    }
    v8::Local<v8::ArrayBuffer> buffer = view->Buffer();
    *aIsSharedMemory = buffer->GetBackingStore()->IsShared();
    result = BufferObjectFromV8(aCx, buffer);
  }
  if (!JS_WrapObject(aCx, &result)) {
    return nullptr;
  }
  return result;
}

JSObject* JS_NewDataView(JSContext* aCx, JS::HandleObject aBuffer,
                         size_t aByteOffset, size_t aByteLength) {
  return NewDataViewObject(aCx, aBuffer, aByteOffset,
                           mozilla::Some(aByteLength));
}

bool JS::IsLargeArrayBufferView(JSObject* aObj) {
  return JS_GetArrayBufferViewByteLength(aObj) > size_t(INT32_MAX);
}

bool JS::IsResizableArrayBufferView(JSObject* aObj) {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::ArrayBufferView> view = ViewOf(cx, aObj);
  return !view.IsEmpty() && ViewResizable(view);
}

bool JS::IsImmutableArrayBufferView(JSObject* aObj) { return false; }

bool JS::PinArrayBufferOrViewLength(JSObject* aObj, bool aPin) {
  JSContext* cx = Cx();
  AutoV8 v8(cx);
  v8::Local<v8::Object> obj = UnwrappedV8(cx, aObj);
  if (obj.IsEmpty() || obj->IsSharedArrayBuffer()) {
    return false;
  }
  if (obj->IsArrayBufferView()) {
    v8::Local<v8::ArrayBufferView> view = obj.As<v8::ArrayBufferView>();
    if (ViewIsShared(view)) {
      return false;
    }
    obj = view->Buffer();
  }
  if (!obj->IsArrayBuffer()) {
    return false;
  }
  v8::Local<v8::Context> ctx = AnyContext(cx);
  v8::Local<v8::Private> key = PrivateKey(cx->isolate, "vge.buffers.pinned");
  if (BufferPinned(cx, obj) == aPin) {
    return false;
  }
  v8::Local<v8::ArrayBuffer> buffer = obj.As<v8::ArrayBuffer>();
  if (aPin) {
    v8::Local<v8::Symbol> detachKey = v8::Symbol::New(cx->isolate);
    buffer->SetDetachKey(detachKey);
    obj->SetPrivate(ctx, key, detachKey).Check();
  } else {
    buffer->SetDetachKey(v8::Undefined(cx->isolate));
    obj->SetPrivate(ctx, key, v8::Undefined(cx->isolate)).Check();
  }
  return true;
}

bool JS::EnsureNonInlineArrayBufferOrView(JSContext* aCx, JSObject* aObj) {
  AutoV8 v8(aCx);
  v8::Local<v8::Object> obj = UnwrappedV8(aCx, aObj);
  if (!obj.IsEmpty()) {
    if (obj->IsArrayBuffer() || obj->IsSharedArrayBuffer()) {
      return true;
    }
    if (obj->IsArrayBufferView()) {
      obj.As<v8::ArrayBufferView>()->Buffer();
      return true;
    }
  }
  JS_ReportErrorASCII(aCx, "unhandled type");
  return false;
}

bool JS::ArrayBufferOrView::isDetached() const {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::Object> o = V8Of(cx, obj);
  if (o->IsArrayBufferView()) {
    v8::Local<v8::ArrayBufferView> view = o.As<v8::ArrayBufferView>();
    return view->HasBuffer() && view->Buffer()->WasDetached();
  }
  return BufferDetached(o);
}

bool JS::ArrayBufferOrView::isResizable() const {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::Object> o = V8Of(cx, obj);
  if (o->IsArrayBufferView()) {
    return ViewResizable(o.As<v8::ArrayBufferView>());
  }
  return StoreResizable(o);
}

bool JS::ArrayBufferOrView::isImmutable() const { return false; }

bool JS::ArrayBufferView::isDetached() const {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::ArrayBufferView> view = V8Of(cx, obj).As<v8::ArrayBufferView>();
  return view->HasBuffer() && view->Buffer()->WasDetached();
}

bool JS::ArrayBufferView::isResizable() const {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  return ViewResizable(V8Of(cx, obj).As<v8::ArrayBufferView>());
}

bool JS::ArrayBufferView::isImmutable() const { return false; }

mozilla::Span<uint8_t> JS::ArrayBufferView::getData(
    bool* aIsSharedMemory, const JS::AutoRequireNoGC&) {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::ArrayBufferView> view = V8Of(cx, obj).As<v8::ArrayBufferView>();
  *aIsSharedMemory = ViewIsShared(view);
  return {ViewData(view), view->ByteLength()};
}

JS::TypedArray_base JS::TypedArray_base::fromObject(JSObject* aUnwrapped) {
  if (!aUnwrapped) {
    return TypedArray_base(nullptr);
  }
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::Object> obj = V8Of(cx, aUnwrapped);
  if (!obj->IsTypedArray()) {
    return TypedArray_base(nullptr);
  }
  BufferObjectFromV8(cx, obj);
  return TypedArray_base(aUnwrapped);
}

bool JS::IsArrayBufferViewShared(JSObject* aObj) {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::ArrayBufferView> view = ViewOf(cx, aObj);
  return !view.IsEmpty() && ViewIsShared(view);
}

bool JS::IsSharedArrayBufferObject(JSObject* aObj) {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  JSObject* unwrapped = Unwrapped(aObj);
  if (!unwrapped) {
    return false;
  }
  v8::Local<v8::Object> obj = V8Of(cx, unwrapped);
  if (!obj->IsSharedArrayBuffer()) {
    return false;
  }
  BufferObjectFromV8(cx, obj);
  return true;
}

bool JS::IsArrayBufferObject(JSObject* aObj) {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  JSObject* unwrapped = Unwrapped(aObj);
  if (!unwrapped) {
    return false;
  }
  v8::Local<v8::Object> obj = V8Of(cx, unwrapped);
  if (!obj->IsArrayBuffer()) {
    return false;
  }
  BufferObjectFromV8(cx, obj);
  return true;
}

bool JS::IsDetachedArrayBufferObject(JSObject* aObj) {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::Object> buffer = BufferOf(cx, aObj, false);
  return !buffer.IsEmpty() && BufferDetached(buffer);
}

bool JS::ArrayBufferHasData(JSObject* aObj) {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::Object> buffer = V8Of(cx, Unwrapped(aObj));
  return !BufferDetached(buffer);
}

JSObject* JS::UnwrapArrayBuffer(JSObject* aObj) {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  JSObject* unwrapped = Unwrapped(aObj);
  if (!unwrapped) {
    return nullptr;
  }
  v8::Local<v8::Object> obj = V8Of(cx, unwrapped);
  if (!obj->IsArrayBuffer()) {
    return nullptr;
  }
  return BufferObjectFromV8(cx, obj);
}

JS::ArrayBuffer JS::ArrayBuffer::unwrap(JSObject* aMaybeWrapped) {
  if (!aMaybeWrapped) {
    return JS::ArrayBuffer(nullptr);
  }
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  JSObject* unwrapped = Unwrapped(aMaybeWrapped);
  if (!unwrapped) {
    return JS::ArrayBuffer(nullptr);
  }
  v8::Local<v8::Object> obj = V8Of(cx, unwrapped);
  if (!obj->IsArrayBuffer() && !obj->IsSharedArrayBuffer()) {
    return JS::ArrayBuffer(nullptr);
  }
  return fromObject(BufferObjectFromV8(cx, obj));
}

JS::ArrayBuffer JS::ArrayBuffer::create(JSContext* aCx, size_t aNbytes) {
  return JS::ArrayBuffer(JS::NewArrayBuffer(aCx, aNbytes));
}

mozilla::Span<uint8_t> JS::ArrayBuffer::getData(bool* aIsSharedMemory,
                                                const JS::AutoRequireNoGC&) {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::Object> buffer = BufferOf(cx, obj, true);
  if (buffer.IsEmpty()) {
    return nullptr;
  }
  *aIsSharedMemory = buffer->IsSharedArrayBuffer();
  return {BufferData(buffer), BufferLength(buffer)};
}

size_t JS::GetArrayBufferByteLength(JSObject* aObj) {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::Object> buffer = BufferOf(cx, aObj, false);
  return buffer.IsEmpty() ? 0 : BufferLength(buffer);
}

uint8_t* JS::GetArrayBufferData(JSObject* aObj, bool* aIsSharedMemory,
                                const JS::AutoRequireNoGC&) {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::Object> buffer = BufferOf(cx, aObj, false);
  if (buffer.IsEmpty()) {
    return nullptr;
  }
  *aIsSharedMemory = false;
  return BufferData(buffer);
}

void JS::GetArrayBufferLengthAndData(JSObject* aObj, size_t* aLength,
                                     bool* aIsSharedMemory, uint8_t** aData) {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::Object> buffer = V8Of(cx, aObj);
  *aLength = BufferLength(buffer);
  *aData = BufferData(buffer);
  *aIsSharedMemory = false;
}

void JS::GetArrayBufferMaybeSharedLengthAndData(JSObject* aObj,
                                                size_t* aLength,
                                                bool* aIsSharedMemory,
                                                uint8_t** aData) {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::Object> buffer = V8Of(cx, aObj);
  *aLength = BufferLength(buffer);
  *aData = BufferData(buffer);
  *aIsSharedMemory = buffer->IsSharedArrayBuffer();
}

bool JS::IsLargeArrayBufferMaybeShared(JSObject* aObj) {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::Object> buffer = BufferOf(cx, aObj, true);
  return !buffer.IsEmpty() && BufferLength(buffer) > size_t(INT32_MAX);
}

bool JS::IsResizableArrayBufferMaybeShared(JSObject* aObj) {
  JSContext* cx = Cx();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::Object> buffer = BufferOf(cx, aObj, true);
  return !buffer.IsEmpty() && StoreResizable(buffer);
}

bool JS::IsImmutableArrayBufferMaybeShared(JSObject* aObj) { return false; }

bool JS::DetachArrayBuffer(JSContext* aCx, JS::HandleObject aObj) {
  AutoV8 v8(aCx);
  v8::Local<v8::Object> obj = UnwrapOrReportArrayBuffer(aCx, aObj);
  if (obj.IsEmpty()) {
    return false;
  }
  v8::Local<v8::ArrayBuffer> buffer = obj.As<v8::ArrayBuffer>();
  if (!buffer->IsDetachable()) {
    ReportError(aCx, JSMSG_WASM_NO_TRANSFER);
    return false;
  }
  if (BufferPinned(aCx, obj)) {
    ReportError(aCx, JSMSG_ARRAYBUFFER_LENGTH_PINNED);
    return false;
  }
  v8::TryCatch tryCatch(aCx->isolate);
  if (buffer->Detach(v8::Local<v8::Value>()).IsNothing()) {
    return v8.Check(tryCatch);
  }
  return true;
}

bool JS::HasDefinedArrayBufferDetachKey(JSContext* aCx, JS::HandleObject aObj,
                                        bool* aIsDefined) {
  AutoV8 v8(aCx);
  v8::Local<v8::Object> obj = UnwrapOrReportArrayBuffer(aCx, aObj);
  if (obj.IsEmpty()) {
    return false;
  }
  *aIsDefined = !obj.As<v8::ArrayBuffer>()->IsDetachable();
  return true;
}

JSObject* JS::NewArrayBuffer(JSContext* aCx, size_t aNbytes) {
  if (aNbytes > v8::TypedArray::kMaxLength) {
    ReportError(aCx, JSMSG_BAD_ARRAY_LENGTH);
    return nullptr;
  }
  AutoV8 v8(aCx);
  return BufferObjectFromV8(aCx, v8::ArrayBuffer::New(aCx->isolate, aNbytes));
}

JSObject* JS::NewArrayBufferWithContents(
    JSContext* aCx, size_t aNbytes,
    mozilla::UniquePtr<void, JS::FreePolicy> aContents) {
  JSObject* result = JS::NewArrayBufferWithContents(
      aCx, aNbytes, aContents.get(),
      JS::NewArrayBufferOutOfMemory::CallerMustFreeMemory);
  if (result) {
    (void)aContents.release();
  }
  return result;
}

JSObject* JS::NewArrayBufferWithContents(JSContext* aCx, size_t aNbytes,
                                         void* aData,
                                         JS::NewArrayBufferOutOfMemory) {
  if (!aData) {
    return JS::NewArrayBuffer(aCx, 0);
  }
  return NewBufferForContents(aCx, aNbytes, aData, FreeJsContents, nullptr);
}

JSObject* JS::NewExternalArrayBuffer(
    JSContext* aCx, size_t aNbytes,
    mozilla::UniquePtr<void, JS::BufferContentsDeleter> aContents) {
  ExternalContents* external =
      new ExternalContents{aContents.get_deleter().freeFunc(),
                           aContents.get_deleter().userData()};
  JSObject* result = NewBufferForContents(aCx, aNbytes, aContents.get(),
                                          FreeExternalContents, external);
  if (result) {
    (void)aContents.release();
  } else {
    delete external;
  }
  return result;
}

JSObject* JS::NewMappedArrayBufferWithContents(JSContext* aCx, size_t aNbytes,
                                               void* aData) {
  return NewBufferForContents(aCx, aNbytes, aData, FreeMappedContents,
                              nullptr);
}

JSObject* JS::CopyArrayBuffer(JSContext* aCx, JS::Handle<JSObject*> aSource) {
  AutoV8 v8(aCx);
  v8::Local<v8::Object> source = UnwrapOrReportArrayBuffer(aCx, aSource);
  if (source.IsEmpty()) {
    return nullptr;
  }
  if (BufferDetached(source)) {
    ReportError(aCx, JSMSG_TYPED_ARRAY_DETACHED);
    return nullptr;
  }
  size_t length = BufferLength(source);
  v8::Local<v8::ArrayBuffer> copy = v8::ArrayBuffer::New(aCx->isolate, length);
  if (length) {
    memcpy(copy->Data(), BufferData(source), length);
  }
  return BufferObjectFromV8(aCx, copy);
}

void* JS::StealArrayBufferContents(JSContext* aCx, JS::HandleObject aObj) {
  AutoV8 v8(aCx);
  v8::Local<v8::Object> obj = UnwrapOrReportArrayBuffer(aCx, aObj);
  if (obj.IsEmpty()) {
    return nullptr;
  }
  v8::Local<v8::ArrayBuffer> buffer = obj.As<v8::ArrayBuffer>();
  if (buffer->WasDetached()) {
    ReportError(aCx, JSMSG_TYPED_ARRAY_DETACHED);
    return nullptr;
  }
  if (BufferPinned(aCx, obj)) {
    ReportError(aCx, JSMSG_ARRAYBUFFER_LENGTH_PINNED);
    return nullptr;
  }
  if (!buffer->IsDetachable()) {
    ReportError(aCx, JSMSG_WASM_NO_TRANSFER);
    return nullptr;
  }
  size_t length = buffer->ByteLength();
  void* contents = js_pod_arena_malloc<uint8_t>(js::ArrayBufferContentsArena,
                                                length ? length : 1);
  if (!contents) {
    JS_ReportOutOfMemory(aCx);
    return nullptr;
  }
  if (length) {
    memcpy(contents, buffer->Data(), length);
  }
  v8::TryCatch tryCatch(aCx->isolate);
  if (buffer->Detach(v8::Local<v8::Value>()).IsNothing()) {
    js_free(contents);
    v8.Check(tryCatch);
    return nullptr;
  }
  return contents;
}

bool JS::ArrayBufferCopyData(JSContext* aCx, JS::Handle<JSObject*> aTo,
                             size_t aToIndex, JS::Handle<JSObject*> aFrom,
                             size_t aFromIndex, size_t aCount) {
  AutoV8 v8(aCx);
  v8::Local<v8::Object> to = BufferOf(aCx, aTo, true);
  if (to.IsEmpty()) {
    js::ReportAccessDenied(aCx);
    return false;
  }
  v8::Local<v8::Object> from = BufferOf(aCx, aFrom, true);
  if (from.IsEmpty()) {
    js::ReportAccessDenied(aCx);
    return false;
  }
  if (aToIndex + aCount < aToIndex || aFromIndex + aCount < aFromIndex ||
      aToIndex + aCount > BufferLength(to) ||
      aFromIndex + aCount > BufferLength(from)) {
    ReportError(aCx, JSMSG_ARRAYBUFFER_COPY_RANGE);
    return false;
  }
  if (aCount) {
    memmove(BufferData(to) + aToIndex, BufferData(from) + aFromIndex, aCount);
  }
  return true;
}

JSObject* JS::ArrayBufferClone(JSContext* aCx, JS::Handle<JSObject*> aSource,
                               size_t aSourceOffset, size_t aSourceLength) {
  if (JS::IsDetachedArrayBufferObject(aSource)) {
    ReportError(aCx, JSMSG_TYPED_ARRAY_DETACHED);
    return nullptr;
  }
  JS::RootedObject target(aCx, JS::NewArrayBuffer(aCx, aSourceLength));
  if (!target) {
    return nullptr;
  }
  if (!JS::ArrayBufferCopyData(aCx, target, 0, aSource, aSourceOffset,
                               aSourceLength)) {
    return nullptr;
  }
  return target;
}

void* JS::CreateMappedArrayBufferContents(int aFd, size_t aOffset,
                                          size_t aLength) {
  constexpr size_t kArrayBufferAlignment = 8;
  if (aLength == 0 || aOffset % kArrayBufferAlignment != 0) {
    return nullptr;
  }
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  size_t granularity = info.dwAllocationGranularity;
  size_t alignedOffset = aOffset - (aOffset % granularity);
  size_t alignedLength = aLength + (aOffset % granularity);
  HANDLE file = reinterpret_cast<HANDLE>(intptr_t(aFd));
  HANDLE mapping =
      CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (!mapping) {
    return nullptr;
  }
  uint8_t* map = static_cast<uint8_t*>(MapViewOfFile(
      mapping, FILE_MAP_COPY, DWORD(uint64_t(alignedOffset) >> 32),
      DWORD(alignedOffset), alignedLength));
  CloseHandle(mapping);
  if (!map) {
    return nullptr;
  }
  return map + (aOffset - alignedOffset);
}

void JS::ReleaseMappedArrayBufferContents(void* aContents, size_t aLength) {
  if (!aContents) {
    return;
  }
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  uintptr_t region = uintptr_t(aContents);
  UnmapViewOfFile(reinterpret_cast<void*>(
      region - (region % info.dwAllocationGranularity)));
}

void JS_free(JSContext* aCx, void* aPtr) { js_free(aPtr); }

bool JS::Call(JSContext* aCx, JS::Handle<JS::Value> aThisv,
              JS::Handle<JS::Value> aFval, const JS::HandleValueArray& aArgs,
              JS::MutableHandle<JS::Value> aRval) {
  AutoV8 v8(aCx);
  v8::TryCatch tryCatch(aCx->isolate);
  v8::Local<v8::Value> callee = ToV8(aCx, aFval.get());
  if (!callee->IsFunction()) {
    JS_ReportErrorNumberASCII(aCx, js::GetErrorMessage, nullptr,
                              JSMSG_NOT_FUNCTION,
                              aFval.isObject() ? "object" : "value");
    return false;
  }
  std::vector<v8::Local<v8::Value>> argv(aArgs.length());
  for (size_t i = 0; i < aArgs.length(); i++) {
    argv[i] = ToV8(aCx, aArgs[i].get());
  }
  v8::Local<v8::Value> result;
  if (!callee.As<v8::Function>()
           ->Call(AnyContext(aCx), ToV8(aCx, aThisv.get()), int(argv.size()),
                  argv.data())
           .ToLocal(&result)) {
    return v8.Check(tryCatch);
  }
  aRval.set(ValueFromV8(aCx, result));
  return true;
}
