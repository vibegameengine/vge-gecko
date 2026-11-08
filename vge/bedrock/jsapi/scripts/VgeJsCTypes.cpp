#include "VgeJsScripts.h"

#include <windows.h>

#include <cstring>

#include "js/experimental/CTypes.h"

namespace vge::jsapi {

namespace {

enum class CKind : int {
  Void,
  Bool,
  Int8,
  UInt8,
  Int16,
  UInt16,
  Int32,
  UInt32,
  Int64,
  UInt64,
  Float32,
  Float64,
  Pointer,
  CharPtr,
  Char16Ptr
};

constexpr size_t kMaxArgs = 12;

struct CTypeInfo {
  const char* name;
  CKind kind;
  int size;
};

const CTypeInfo kTypes[] = {
    {"void_t", CKind::Void, 0},          {"bool", CKind::Bool, 1},
    {"int8_t", CKind::Int8, 1},          {"uint8_t", CKind::UInt8, 1},
    {"int16_t", CKind::Int16, 2},        {"uint16_t", CKind::UInt16, 2},
    {"int32_t", CKind::Int32, 4},        {"uint32_t", CKind::UInt32, 4},
    {"int64_t", CKind::Int64, 8},        {"uint64_t", CKind::UInt64, 8},
    {"short", CKind::Int16, 2},          {"unsigned_short", CKind::UInt16, 2},
    {"int", CKind::Int32, 4},            {"unsigned_int", CKind::UInt32, 4},
    {"long", CKind::Int32, 4},           {"unsigned_long", CKind::UInt32, 4},
    {"long_long", CKind::Int64, 8},      {"unsigned_long_long", CKind::UInt64, 8},
    {"size_t", CKind::UInt64, 8},        {"ssize_t", CKind::Int64, 8},
    {"intptr_t", CKind::Int64, 8},       {"uintptr_t", CKind::UInt64, 8},
    {"off_t", CKind::Int64, 8},          {"float32_t", CKind::Float32, 4},
    {"float", CKind::Float32, 4},        {"float64_t", CKind::Float64, 8},
    {"double", CKind::Float64, 8},       {"char", CKind::Int8, 1},
    {"signed_char", CKind::Int8, 1},     {"unsigned_char", CKind::UInt8, 1},
    {"char16_t", CKind::UInt16, 2},      {"jschar", CKind::UInt16, 2},
    {"voidptr_t", CKind::Pointer, 8},
};

struct CTypesState {
  JS::CTypesActivityCallback activity = nullptr;
};

thread_local std::unordered_map<JSContext*, CTypesState> gCTypesStates;
std::unordered_map<JSObject*, const JS::CTypesCallbacks*> gCTypesCallbacks;

v8::Local<v8::Private> TypeKey(v8::Isolate* aIsolate) {
  return v8::Private::ForApi(
      aIsolate, v8::String::NewFromUtf8Literal(aIsolate, "vge.ctypes.type"));
}

v8::Local<v8::Private> HandleKey(v8::Isolate* aIsolate) {
  return v8::Private::ForApi(
      aIsolate, v8::String::NewFromUtf8Literal(aIsolate, "vge.ctypes.library"));
}

void Throw(v8::Isolate* aIsolate, const char* aMessage) {
  aIsolate->ThrowException(
      v8::Exception::TypeError(V8String(aIsolate, aMessage)));
}

bool KindOf(v8::Local<v8::Context> aContext, v8::Local<v8::Value> aType,
            CKind* aKind) {
  if (!aType->IsObject()) {
    return false;
  }
  v8::Local<v8::Value> tag;
  if (!aType.As<v8::Object>()
           ->GetPrivate(aContext, TypeKey(aContext->GetIsolate()))
           .ToLocal(&tag) ||
      !tag->IsInt32()) {
    return false;
  }
  *aKind = CKind(tag.As<v8::Int32>()->Value());
  return true;
}

v8::Local<v8::Object> MakeType(v8::Local<v8::Context> aContext,
                               const char* aName, CKind aKind, int aSize);

void DefineType(v8::Local<v8::Context> aContext, v8::Local<v8::Object> aType,
                const char* aName, CKind aKind, int aSize) {
  v8::Isolate* isolate = aContext->GetIsolate();
  aType->SetPrivate(aContext, TypeKey(isolate), v8::Integer::New(isolate, int(aKind)))
      .Check();
  aType->Set(aContext, V8String(isolate, "name"), V8String(isolate, aName))
      .Check();
  aType->Set(aContext, V8String(isolate, "size"), v8::Integer::New(isolate, aSize))
      .Check();
}

v8::Local<v8::Object> MakeType(v8::Local<v8::Context> aContext,
                               const char* aName, CKind aKind, int aSize) {
  v8::Isolate* isolate = aContext->GetIsolate();
  v8::Local<v8::Object> type = v8::Object::New(isolate);
  DefineType(aContext, type, aName, aKind, aSize);
  if (aKind != CKind::Void && aKind != CKind::Pointer) {
    CKind pointerKind = CKind::Pointer;
    std::string name = std::string(aName) + "*";
    if (aKind == CKind::Int8 || aKind == CKind::UInt8) {
      pointerKind = CKind::CharPtr;
    } else if (aKind == CKind::UInt16 &&
               (strcmp(aName, "char16_t") == 0 || strcmp(aName, "jschar") == 0)) {
      pointerKind = CKind::Char16Ptr;
    }
    v8::Local<v8::Object> pointer = v8::Object::New(isolate);
    DefineType(aContext, pointer, name.c_str(), pointerKind, 8);
    type->Set(aContext, V8String(isolate, "ptr"), pointer).Check();
  }
  return type;
}

template <typename T>
T Pick(uint64_t aBits) {
  if constexpr (std::is_same_v<T, double>) {
    double d;
    memcpy(&d, &aBits, sizeof(d));
    return d;
  } else if constexpr (std::is_same_v<T, float>) {
    float f;
    uint32_t low = uint32_t(aBits);
    memcpy(&f, &low, sizeof(f));
    return f;
  } else {
    return aBits;
  }
}

template <typename R, typename A0, typename A1, typename A2, typename A3>
R Invoke(void* aFunction, const uint64_t* a) {
  using Fn = R (*)(A0, A1, A2, A3, uint64_t, uint64_t, uint64_t, uint64_t,
                   uint64_t, uint64_t, uint64_t, uint64_t);
  return reinterpret_cast<Fn>(aFunction)(
      Pick<A0>(a[0]), Pick<A1>(a[1]), Pick<A2>(a[2]), Pick<A3>(a[3]), a[4],
      a[5], a[6], a[7], a[8], a[9], a[10], a[11]);
}

int ClassOf(CKind aKind) {
  if (aKind == CKind::Float64) {
    return 1;
  }
  if (aKind == CKind::Float32) {
    return 2;
  }
  return 0;
}

template <typename R, typename... Chosen>
R Dispatch(void* aFunction, const uint64_t* a, const int* aClasses) {
  if constexpr (sizeof...(Chosen) == 4) {
    return Invoke<R, Chosen...>(aFunction, a);
  } else {
    switch (aClasses[sizeof...(Chosen)]) {
      case 1:
        return Dispatch<R, Chosen..., double>(aFunction, a, aClasses);
      case 2:
        return Dispatch<R, Chosen..., float>(aFunction, a, aClasses);
      default:
        return Dispatch<R, Chosen..., uint64_t>(aFunction, a, aClasses);
    }
  }
}

struct Declared {
  void* function;
  CKind returnKind;
  std::vector<CKind> argKinds;
  JSObject* ctypes;
};

bool ToArg(JSContext* aCx, v8::Local<v8::Context> aContext, CKind aKind,
           v8::Local<v8::Value> aValue, uint64_t* aOut,
           std::vector<std::string>* aUtf8Keep,
           std::vector<std::u16string>* aUtf16Keep) {
  v8::Isolate* isolate = aContext->GetIsolate();
  switch (aKind) {
    case CKind::Float32:
    case CKind::Float64: {
      double d = aValue->NumberValue(aContext).FromMaybe(0);
      if (aKind == CKind::Float32) {
        float f = float(d);
        uint32_t bits;
        memcpy(&bits, &f, sizeof(bits));
        *aOut = bits;
      } else {
        memcpy(aOut, &d, sizeof(d));
      }
      return true;
    }
    case CKind::CharPtr:
      if (aValue->IsString()) {
        aUtf8Keep->push_back(Utf8Of(isolate, aValue));
        *aOut = uint64_t(uintptr_t(aUtf8Keep->back().c_str()));
        return true;
      }
      break;
    case CKind::Char16Ptr:
      if (aValue->IsString()) {
        aUtf16Keep->push_back(Utf16Of(isolate, aValue));
        *aOut = uint64_t(uintptr_t(aUtf16Keep->back().c_str()));
        return true;
      }
      break;
    default:
      break;
  }
  if (aValue->IsNullOrUndefined()) {
    *aOut = 0;
    return true;
  }
  if (aValue->IsBigInt()) {
    *aOut = aValue.As<v8::BigInt>()->Uint64Value();
    return true;
  }
  if (aValue->IsBoolean()) {
    *aOut = aValue->IsTrue() ? 1 : 0;
    return true;
  }
  if (aValue->IsNumber()) {
    double d = aValue.As<v8::Number>()->Value();
    *aOut = d < 0 ? uint64_t(int64_t(d)) : uint64_t(d);
    return true;
  }
  return false;
}

v8::Local<v8::Value> FromResult(v8::Isolate* aIsolate, CKind aKind,
                                uint64_t aBits, double aDouble) {
  switch (aKind) {
    case CKind::Void:
      return v8::Undefined(aIsolate);
    case CKind::Bool:
      return v8::Boolean::New(aIsolate, uint8_t(aBits) != 0);
    case CKind::Int8:
      return v8::Integer::New(aIsolate, int8_t(aBits));
    case CKind::UInt8:
      return v8::Integer::New(aIsolate, uint8_t(aBits));
    case CKind::Int16:
      return v8::Integer::New(aIsolate, int16_t(aBits));
    case CKind::UInt16:
      return v8::Integer::New(aIsolate, uint16_t(aBits));
    case CKind::Int32:
      return v8::Integer::New(aIsolate, int32_t(aBits));
    case CKind::UInt32:
      return v8::Integer::NewFromUnsigned(aIsolate, uint32_t(aBits));
    case CKind::Int64:
      return v8::BigInt::New(aIsolate, int64_t(aBits));
    case CKind::Float32:
    case CKind::Float64:
      return v8::Number::New(aIsolate, aDouble);
    default:
      return v8::BigInt::NewFromUnsigned(aIsolate, aBits);
  }
}

void CallDeclared(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  JSContext* cx = CurrentContext();
  v8::Isolate* isolate = aInfo.GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  Declared* declared =
      static_cast<Declared*>(aInfo.Data().As<v8::External>()->Value());
  if (size_t(aInfo.Length()) != declared->argKinds.size()) {
    Throw(isolate, "ctypes: wrong number of arguments");
    return;
  }
  uint64_t args[kMaxArgs] = {};
  int classes[4] = {};
  std::vector<std::string> utf8Keep;
  std::vector<std::u16string> utf16Keep;
  utf8Keep.reserve(kMaxArgs);
  utf16Keep.reserve(kMaxArgs);
  for (size_t i = 0; i < declared->argKinds.size(); i++) {
    if (!ToArg(cx, context, declared->argKinds[i], aInfo[int(i)], &args[i],
               &utf8Keep, &utf16Keep)) {
      Throw(isolate, "ctypes: can't convert argument");
      return;
    }
    if (i < 4) {
      classes[i] = ClassOf(declared->argKinds[i]);
    }
  }
  JS::CTypesActivityCallback activity = gCTypesStates[cx].activity;
  if (activity) {
    activity(cx, JS::CTypesActivityType::BeginCall);
  }
  uint64_t bits = 0;
  double real = 0;
  switch (ClassOf(declared->returnKind)) {
    case 1:
      real = Dispatch<double>(declared->function, args, classes);
      break;
    case 2:
      real = Dispatch<float>(declared->function, args, classes);
      break;
    default:
      bits = Dispatch<uint64_t>(declared->function, args, classes);
      break;
  }
  if (activity) {
    activity(cx, JS::CTypesActivityType::EndCall);
  }
  aInfo.GetReturnValue().Set(FromResult(isolate, declared->returnKind, bits, real));
}

HMODULE LibraryOf(v8::Local<v8::Context> aContext, v8::Local<v8::Value> aThis) {
  if (!aThis->IsObject()) {
    return nullptr;
  }
  v8::Local<v8::Value> handle;
  if (!aThis.As<v8::Object>()
           ->GetPrivate(aContext, HandleKey(aContext->GetIsolate()))
           .ToLocal(&handle) ||
      !handle->IsExternal()) {
    return nullptr;
  }
  return static_cast<HMODULE>(handle.As<v8::External>()->Value());
}

void LibraryClose(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  v8::Isolate* isolate = aInfo.GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  if (HMODULE module = LibraryOf(context, aInfo.This())) {
    FreeLibrary(module);
    aInfo.This()
        ->SetPrivate(context, HandleKey(isolate), v8::Undefined(isolate))
        .Check();
  }
}

void LibraryDeclare(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  v8::Isolate* isolate = aInfo.GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  HMODULE module = LibraryOf(context, aInfo.This());
  if (!module) {
    Throw(isolate, "ctypes: library is closed");
    return;
  }
  if (aInfo.Length() < 3 || !aInfo[0]->IsString()) {
    Throw(isolate, "ctypes: declare requires name, abi and return type");
    return;
  }
  std::string name = Utf8Of(isolate, aInfo[0]);
  FARPROC function = GetProcAddress(module, name.c_str());
  if (!function) {
    Throw(isolate, "ctypes: couldn't find function symbol in library");
    return;
  }
  Declared* declared = new Declared();
  declared->function = reinterpret_cast<void*>(function);
  if (!KindOf(context, aInfo[2], &declared->returnKind)) {
    delete declared;
    Throw(isolate, "ctypes: invalid return type");
    return;
  }
  for (int i = 3; i < aInfo.Length(); i++) {
    CKind kind;
    if (!KindOf(context, aInfo[i], &kind) || kind == CKind::Void) {
      delete declared;
      Throw(isolate, "ctypes: invalid argument type");
      return;
    }
    declared->argKinds.push_back(kind);
  }
  if (declared->argKinds.size() > kMaxArgs) {
    delete declared;
    Throw(isolate, "ctypes: too many arguments");
    return;
  }
  v8::Local<v8::Function> callable;
  if (!v8::Function::New(context, CallDeclared,
                         v8::External::New(isolate, declared),
                         int(declared->argKinds.size()),
                         v8::ConstructorBehavior::kThrow)
           .ToLocal(&callable)) {
    delete declared;
    return;
  }
  callable->SetName(V8String(isolate, name.c_str()));
  aInfo.GetReturnValue().Set(callable);
}

void CTypesOpen(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  v8::Isolate* isolate = aInfo.GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  if (aInfo.Length() < 1 || !aInfo[0]->IsString()) {
    Throw(isolate, "ctypes.open requires a library path");
    return;
  }
  std::u16string path = Utf16Of(isolate, aInfo[0]);
  HMODULE module = LoadLibraryW(reinterpret_cast<const wchar_t*>(path.c_str()));
  if (!module) {
    isolate->ThrowException(v8::Exception::Error(
        V8String(isolate, "couldn't open library")));
    return;
  }
  v8::Local<v8::Object> library = v8::Object::New(isolate);
  library->SetPrivate(context, HandleKey(isolate), v8::External::New(isolate, module))
      .Check();
  v8::Local<v8::Function> close;
  v8::Local<v8::Function> declare;
  if (v8::Function::New(context, LibraryClose).ToLocal(&close)) {
    library->Set(context, V8String(isolate, "close"), close).Check();
  }
  if (v8::Function::New(context, LibraryDeclare).ToLocal(&declare)) {
    library->Set(context, V8String(isolate, "declare"), declare).Check();
  }
  aInfo.GetReturnValue().Set(library);
}

void CTypesLibraryName(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  v8::Isolate* isolate = aInfo.GetIsolate();
  if (aInfo.Length() < 1 || !aInfo[0]->IsString()) {
    Throw(isolate, "ctypes.libraryName requires a string");
    return;
  }
  std::u16string name = Utf16Of(isolate, aInfo[0]) + u".dll";
  aInfo.GetReturnValue().Set(V8String(isolate, name.data(), name.size()));
}

}

}

using namespace vge::jsapi;

bool JS::InitCTypesClass(JSContext* cx, Handle<JSObject*> global) {
  AutoV8 scope(cx);
  v8::Isolate* isolate = cx->isolate;
  v8::Local<v8::Context> context = scope.context();
  v8::Local<v8::Object> ctypes = v8::Object::New(isolate);
  for (const CTypeInfo& info : kTypes) {
    ctypes
        ->Set(context, V8String(isolate, info.name),
              MakeType(context, info.name, info.kind, info.size))
        .Check();
  }
  for (const char* abi : {"default_abi", "stdcall_abi", "thiscall_abi",
                          "winapi_abi"}) {
    v8::Local<v8::Object> marker = v8::Object::New(isolate);
    marker->Set(context, V8String(isolate, "name"), V8String(isolate, abi))
        .Check();
    ctypes->Set(context, V8String(isolate, abi), marker).Check();
  }
  v8::Local<v8::Function> open;
  v8::Local<v8::Function> libraryName;
  if (!v8::Function::New(context, CTypesOpen).ToLocal(&open) ||
      !v8::Function::New(context, CTypesLibraryName).ToLocal(&libraryName)) {
    return false;
  }
  ctypes->Set(context, V8String(isolate, "open"), open).Check();
  ctypes->Set(context, V8String(isolate, "libraryName"), libraryName).Check();
  v8::TryCatch tryCatch(isolate);
  if (ToV8(cx, global.get())
          ->DefineOwnProperty(context, V8String(isolate, "ctypes"), ctypes,
                              v8::DontEnum)
          .IsNothing()) {
    return ReportPending(cx, tryCatch);
  }
  return true;
}

void JS::SetCTypesActivityCallback(JSContext* cx,
                                   JS::CTypesActivityCallback cb) {
  gCTypesStates[cx].activity = cb;
}

void JS::SetCTypesCallbacks(JSObject* ctypesObj,
                            const JS::CTypesCallbacks* callbacks) {
  gCTypesCallbacks[ctypesObj] = callbacks;
}
