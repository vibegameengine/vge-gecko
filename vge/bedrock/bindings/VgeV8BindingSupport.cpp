#include "vge/bedrock/VgeV8BindingSupport.h"

#include <stdio.h>

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ErrorList.h"
#include "mozilla/BasePrincipal.h"
#include "mozilla/Preferences.h"
#include "mozilla/dom/BrowsingContext.h"
#include "nsGlobalWindowInner.h"
#include "nsPIDOMWindow.h"
#include "nsPrintfCString.h"

namespace vge::bedrock::v8b {

namespace {

struct IsolateData {
  std::vector<v8::Global<v8::FunctionTemplate>> mTemplates;
};

IsolateData& DataFor(v8::Isolate* aIsolate) {
  auto* data = static_cast<IsolateData*>(aIsolate->GetData(0));
  if (!data) {
    data = new IsolateData();
    data->mTemplates.resize(gProtoCount);
    aIsolate->SetData(0, data);
  }
  return *data;
}

struct WrapperHolder {
  v8::Global<v8::Object> mHandle;
  void* mNative;
  int mProto;
  nsWrapperCache* mCache;
};

std::unordered_set<WrapperHolder*>& LiveHolders() {
  static std::unordered_set<WrapperHolder*> sHolders;
  return sHolders;
}

void ReleaseSecondPass(const v8::WeakCallbackInfo<WrapperHolder>& aData) {
  WrapperHolder* holder = aData.GetParameter();
  LiveHolders().erase(holder);
  gProtoInfos[holder->mProto].mRelease(holder->mNative);
  delete holder;
}

void ReleaseFirstPass(const v8::WeakCallbackInfo<WrapperHolder>& aData) {
  WrapperHolder* holder = aData.GetParameter();
  holder->mHandle.Reset();
  if (holder->mCache && holder->mCache->GetVgeV8Wrapper() == holder) {
    holder->mCache->SetVgeV8Wrapper(nullptr);
  }
  aData.SetSecondPassCallback(ReleaseSecondPass);
}

struct WindowEntry {
  v8::Global<v8::Context> mContext;
  WrapperHolder* mHolder;
};

std::unordered_map<nsGlobalWindowInner*, WindowEntry>& Windows() {
  static std::unordered_map<nsGlobalWindowInner*, WindowEntry> sWindows;
  return sWindows;
}

bool ReadProto(v8::Local<v8::Object> aObject, int* aProto, void** aNative) {
  if (aObject->InternalFieldCount() < 2) {
    return false;
  }
  v8::Local<v8::Data> protoField = aObject->GetInternalField(1);
  if (!protoField->IsValue() || !protoField.As<v8::Value>()->IsInt32()) {
    return false;
  }
  *aProto = protoField.As<v8::Value>().As<v8::Int32>()->Value();
  *aNative = aObject->GetAlignedPointerFromInternalField(0);
  return *aNative && *aProto >= 0 && *aProto < gProtoCount;
}

void ConsolePrint(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  v8::Isolate* isolate = aInfo.GetIsolate();
  nsAutoCString line;
  for (int i = 0; i < aInfo.Length(); ++i) {
    v8::String::Utf8Value text(isolate, aInfo[i]);
    if (i) {
      line.Append(' ');
    }
    line.Append(*text ? *text : "<unprintable>");
  }
  v8::String::Utf8Value level(isolate, aInfo.Data());
  printf("console.%s: %s\n", *level, line.get());
}

void InstallConsole(v8::Isolate* aIsolate, v8::Local<v8::Context> aContext) {
  v8::Local<v8::Object> console = v8::Object::New(aIsolate);
  for (const char* level : {"log", "info", "warn", "error", "debug"}) {
    v8::Local<v8::String> name =
        v8::String::NewFromUtf8(aIsolate, level).ToLocalChecked();
    v8::Local<v8::Function> fn =
        v8::Function::New(aContext, ConsolePrint, name).ToLocalChecked();
    console->Set(aContext, name, fn).Check();
  }
  aContext->Global()
      ->Set(aContext, v8::String::NewFromUtf8Literal(aIsolate, "console"),
            console)
      .Check();
}

}

v8::Local<v8::FunctionTemplate> CachedTemplate(v8::Isolate* aIsolate,
                                               int aProto) {
  return DataFor(aIsolate).mTemplates[aProto].Get(aIsolate);
}

void StoreTemplate(v8::Isolate* aIsolate, int aProto,
                   v8::Local<v8::FunctionTemplate> aTemplate) {
  DataFor(aIsolate).mTemplates[aProto].Reset(aIsolate, aTemplate);
}

void* Unwrap(v8::Isolate* aIsolate, v8::Local<v8::Value> aValue,
             int aTargetProto) {
  if (aValue.IsEmpty() || !aValue->IsObject()) {
    return nullptr;
  }
  v8::Local<v8::Object> object = aValue.As<v8::Object>();
  int proto;
  void* native;
  if (!ReadProto(object, &proto, &native)) {
    v8::Local<v8::Value> prototype = object->GetPrototype();
    if (!prototype->IsObject() ||
        !ReadProto(prototype.As<v8::Object>(), &proto, &native)) {
      return nullptr;
    }
  }
  return gProtoInfos[proto].mCast(aTargetProto, native);
}

v8::Local<v8::Value> WrapProto(v8::Isolate* aIsolate, void* aNative,
                               int aProto, nsWrapperCache* aCache) {
  if (aCache) {
    if (auto* holder = static_cast<WrapperHolder*>(aCache->GetVgeV8Wrapper())) {
      return holder->mHandle.Get(aIsolate);
    }
  }
  v8::Local<v8::Context> context = aIsolate->GetCurrentContext();
  v8::Local<v8::FunctionTemplate> tmpl =
      gProtoInfos[aProto].mTemplate(aIsolate);
  v8::Local<v8::Object> object;
  if (!tmpl->InstanceTemplate()->NewInstance(context).ToLocal(&object)) {
    return v8::Null(aIsolate);
  }
  object->SetAlignedPointerInInternalField(0, aNative);
  object->SetInternalField(1, v8::Integer::New(aIsolate, aProto));
  gProtoInfos[aProto].mAddRef(aNative);

  auto* holder = new WrapperHolder{v8::Global<v8::Object>(aIsolate, object),
                                   aNative, aProto, aCache};
  holder->mHandle.SetWeak(holder, ReleaseFirstPass,
                          v8::WeakCallbackType::kParameter);
  if (aCache) {
    aCache->SetVgeV8Wrapper(holder);
  }
  LiveHolders().insert(holder);
  return object;
}

bool ToString(v8::Isolate* aIsolate, v8::Local<v8::Value> aValue,
              nsAString& aResult, StringNull aNullMode) {
  if (aValue->IsNull() || (aNullMode == StringNull::Null &&
                           aValue->IsUndefined())) {
    if (aNullMode == StringNull::Null) {
      aResult.SetIsVoid(true);
      return true;
    }
    if (aNullMode == StringNull::EmptyString) {
      aResult.Truncate();
      return true;
    }
  }
  v8::Local<v8::String> string;
  if (!aValue->ToString(aIsolate->GetCurrentContext()).ToLocal(&string)) {
    return false;
  }
  int length = string->Length();
  if (!aResult.SetLength(length, mozilla::fallible)) {
    ThrowTypeError(aIsolate, "out of memory converting a string");
    return false;
  }
  string->Write(aIsolate, reinterpret_cast<uint16_t*>(aResult.BeginWriting()),
                0, length, v8::String::NO_NULL_TERMINATION);
  return true;
}

bool ToString(v8::Isolate* aIsolate, v8::Local<v8::Value> aValue,
              nsACString& aResult, StringNull aNullMode) {
  nsAutoString wide;
  if (!ToString(aIsolate, aValue, wide, aNullMode)) {
    return false;
  }
  if (wide.IsVoid()) {
    aResult.SetIsVoid(true);
    return true;
  }
  CopyUTF16toUTF8(wide, aResult);
  return true;
}

v8::Local<v8::Value> FromString(v8::Isolate* aIsolate,
                                const nsAString& aString) {
  if (aString.IsVoid()) {
    return v8::Null(aIsolate);
  }
  v8::Local<v8::String> string;
  if (!v8::String::NewFromTwoByte(
           aIsolate, reinterpret_cast<const uint16_t*>(aString.BeginReading()),
           v8::NewStringType::kNormal, aString.Length())
           .ToLocal(&string)) {
    return v8::String::Empty(aIsolate);
  }
  return string;
}

v8::Local<v8::Value> FromUTF8String(v8::Isolate* aIsolate,
                                    const nsACString& aString) {
  if (aString.IsVoid()) {
    return v8::Null(aIsolate);
  }
  v8::Local<v8::String> string;
  if (!v8::String::NewFromUtf8(aIsolate, aString.BeginReading(),
                               v8::NewStringType::kNormal, aString.Length())
           .ToLocal(&string)) {
    return v8::String::Empty(aIsolate);
  }
  return string;
}

bool ToNumber(v8::Isolate* aIsolate, v8::Local<v8::Value> aValue,
              double* aResult) {
  v8::Local<v8::Number> number;
  if (!aValue->ToNumber(aIsolate->GetCurrentContext()).ToLocal(&number)) {
    return false;
  }
  *aResult = number->Value();
  return true;
}

bool ToBoolean(v8::Isolate* aIsolate, v8::Local<v8::Value> aValue) {
  return aValue->BooleanValue(aIsolate);
}

bool ToRestrictedDouble(v8::Isolate* aIsolate, v8::Local<v8::Value> aValue,
                        double* aResult, const char* aWhere) {
  if (!ToNumber(aIsolate, aValue, aResult)) {
    return false;
  }
  if (!std::isfinite(*aResult)) {
    nsPrintfCString message("%s: value is not a finite floating-point value",
                            aWhere);
    ThrowTypeError(aIsolate, message.get());
    return false;
  }
  return true;
}

void ThrowTypeError(v8::Isolate* aIsolate, const char* aMessage) {
  aIsolate->ThrowException(v8::Exception::TypeError(
      v8::String::NewFromUtf8(aIsolate, aMessage).ToLocalChecked()));
}

void ThrowErrorResult(v8::Isolate* aIsolate,
                      mozilla::dom::binding_detail::FastErrorResult& aRv,
                      const char* aWhere) {
  nsresult rv = aRv.StealNSResult();
  nsAutoCString name;
  mozilla::GetErrorName(rv, name);
  nsPrintfCString message("%s: %s", aWhere, name.get());
  aIsolate->ThrowException(v8::Exception::Error(
      v8::String::NewFromUtf8(aIsolate, message.get()).ToLocalChecked()));
}

void IllegalConstructor(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  ThrowTypeError(aInfo.GetIsolate(), "Illegal constructor");
}

bool InstallMember(const char* aPref) {
  return !aPref || mozilla::Preferences::GetBool(aPref, false);
}

nsIPrincipal* SubjectPrincipal(v8::Isolate* aIsolate) {
  v8::Local<v8::Context> context = aIsolate->GetCurrentContext();
  if (context.IsEmpty()) {
    return nullptr;
  }
  auto* window = static_cast<nsGlobalWindowInner*>(
      Unwrap(aIsolate, context->Global(), WindowProto()));
  return window ? window->GetPrincipal() : nullptr;
}

nsIPrincipal* NonSystemSubjectPrincipal(v8::Isolate* aIsolate) {
  nsIPrincipal* principal = SubjectPrincipal(aIsolate);
  if (principal && principal->IsSystemPrincipal()) {
    return nullptr;
  }
  return principal;
}

v8::Local<v8::Context> WindowContext(v8::Isolate* aIsolate,
                                     nsGlobalWindowInner* aWindow) {
  auto& windows = Windows();
  auto found = windows.find(aWindow);
  if (found != windows.end()) {
    return found->second.mContext.Get(aIsolate);
  }

  int proto = WindowProto();
  v8::Local<v8::FunctionTemplate> windowTemplate =
      gProtoInfos[proto].mTemplate(aIsolate);
  v8::Local<v8::Context> context =
      v8::Context::New(aIsolate, nullptr, windowTemplate->InstanceTemplate());
  v8::Context::Scope scope(context);

  v8::Local<v8::Object> globalProxy = context->Global();
  v8::Local<v8::Object> global =
      globalProxy->GetPrototype().As<v8::Object>();
  void* native = gProtoInfos[proto].mCast(proto, aWindow);
  global->SetAlignedPointerInInternalField(0, native);
  global->SetInternalField(1, v8::Integer::New(aIsolate, proto));
  gProtoInfos[proto].mAddRef(native);

  auto* holder = new WrapperHolder{
      v8::Global<v8::Object>(aIsolate, globalProxy), native, proto, aWindow};
  aWindow->SetVgeV8Wrapper(holder);
  InstallConsole(aIsolate, context);
  for (int i = 0; i < gProtoCount; ++i) {
    v8::Local<v8::Function> interfaceObject;
    if (gProtoInfos[i].mTemplate(aIsolate)->GetFunction(context).ToLocal(
            &interfaceObject)) {
      global
          ->DefineOwnProperty(
              context,
              v8::String::NewFromUtf8(aIsolate, gProtoInfos[i].mName)
                  .ToLocalChecked(),
              interfaceObject, v8::DontEnum)
          .Check();
    }
  }

  WindowEntry& entry = windows[aWindow];
  entry.mContext.Reset(aIsolate, context);
  entry.mHolder = holder;
  return context;
}

v8::Local<v8::Value> WrapWindowProxy(
    v8::Isolate* aIsolate, const mozilla::dom::WindowProxyHolder& aProxy) {
  mozilla::dom::BrowsingContext* browsingContext = aProxy.get();
  nsPIDOMWindowOuter* outer =
      browsingContext ? browsingContext->GetDOMWindow() : nullptr;
  nsPIDOMWindowInner* inner = outer ? outer->GetCurrentInnerWindow() : nullptr;
  if (!inner) {
    return v8::Null(aIsolate);
  }
  return WindowContext(aIsolate, nsGlobalWindowInner::Cast(inner))->Global();
}

v8::Local<v8::Value> WrapWindowProxy(
    v8::Isolate* aIsolate,
    const mozilla::dom::Nullable<mozilla::dom::WindowProxyHolder>& aProxy) {
  if (aProxy.IsNull()) {
    return v8::Null(aIsolate);
  }
  return WrapWindowProxy(aIsolate, aProxy.Value());
}

v8::Local<v8::Value> WrapEventTarget(v8::Isolate* aIsolate,
                                     mozilla::dom::EventTarget* aTarget) {
  if (!aTarget) {
    return v8::Null(aIsolate);
  }
  if (aTarget->IsNode()) {
    return Wrap(aIsolate, static_cast<nsINode*>(aTarget));
  }
  if (aTarget->IsInnerWindow()) {
    return WindowContext(aIsolate,
                         nsGlobalWindowInner::Cast(aTarget->GetAsInnerWindow()))
        ->Global();
  }
  static int sProto = -1;
  if (sProto < 0) {
    for (int i = 0; i < gProtoCount; ++i) {
      if (!strcmp(gProtoInfos[i].mName, "EventTarget")) {
        sProto = i;
      }
    }
  }
  return WrapProto(aIsolate, aTarget, sProto, aTarget);
}

void ForgetWindowContext(nsGlobalWindowInner* aWindow) {
  auto& windows = Windows();
  auto found = windows.find(aWindow);
  if (found == windows.end()) {
    return;
  }
  WrapperHolder* holder = found->second.mHolder;
  aWindow->SetVgeV8Wrapper(nullptr);
  gProtoInfos[holder->mProto].mRelease(holder->mNative);
  holder->mHandle.Reset();
  delete holder;
  found->second.mContext.Reset();
  windows.erase(found);
}

void ReleaseAllWrappers() {
  std::vector<nsGlobalWindowInner*> windows;
  for (auto& entry : Windows()) {
    windows.push_back(entry.first);
  }
  for (nsGlobalWindowInner* window : windows) {
    ForgetWindowContext(window);
  }
  for (WrapperHolder* holder : LiveHolders()) {
    holder->mHandle.Reset();
    if (holder->mCache && holder->mCache->GetVgeV8Wrapper() == holder) {
      holder->mCache->SetVgeV8Wrapper(nullptr);
    }
    gProtoInfos[holder->mProto].mRelease(holder->mNative);
    delete holder;
  }
  LiveHolders().clear();
}

}
