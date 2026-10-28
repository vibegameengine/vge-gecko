#include "VgeJsClassesState.h"

#include <chrono>
#include <string>

#include "js/CharacterEncoding.h"
#include "js/ColumnNumber.h"
#include "js/Date.h"
#include "js/DOMEventDispatch.h"
#include "js/RootingAPI.h"
#include "js/ScriptPrivate.h"
#include "js/String.h"
#include "js/friend/XrayJitInfo.h"
#include "js/experimental/TypedData.h"
#include "jsfriendapi.h"
#include "mozilla/TextUtils.h"
#include "scripts/VgeJsScripts.h"
#include "unicode/uchar.h"

namespace vge::jsapi {

JSObject* NewModuleObjectFor(JSContext* aCx, v8::Local<v8::Module> aModule,
                             ScriptData* aScript);

namespace {

JS::DispatchDOMEventCallback sDispatchDOMEventCallback = nullptr;
JS::XrayJitInfo* sXrayJitInfo = nullptr;

int CurrentFrameCount(v8::Isolate* aIsolate) {
  v8::HandleScope scope(aIsolate);
  return v8::StackTrace::CurrentStackTrace(aIsolate, 1 << 16,
                                           v8::StackTrace::kScriptId)
      ->GetFrameCount();
}

bool CallerIsHidden(JSContext* aCx) {
  std::vector<int>& hidden = State(aCx).hiddenCallerDepths;
  return !hidden.empty() && hidden.back() == CurrentFrameCount(aCx->isolate);
}

v8::Local<v8::StackFrame> TopFrame(JSContext* aCx) {
  v8::Local<v8::StackTrace> trace = v8::StackTrace::CurrentStackTrace(
      aCx->isolate, 1,
      v8::StackTrace::StackTraceOptions(v8::StackTrace::kScriptName |
                                        v8::StackTrace::kColumnOffset |
                                        v8::StackTrace::kScriptId));
  if (trace->GetFrameCount() == 0) {
    return v8::Local<v8::StackFrame>();
  }
  return trace->GetFrame(aCx->isolate, 0);
}

void RawRootsTracer(JSTracer* aTrc, void* aData) {
  ClassesState* state = static_cast<ClassesState*>(aData);
  for (auto& [vp, name] : state->rawRoots) {
    JS::TraceRoot(aTrc, vp, name);
  }
}

bool IsExtensible(JSContext* aCx, JS::HandleObject aObj, bool* aResult) {
  AutoV8 v8(aCx);
  v8::TryCatch tryCatch(aCx->isolate);
  RealmExtra& extra = RealmData(aCx->realm());
  v8::Local<v8::Value> arg = ToV8(aCx, aObj);
  v8::Local<v8::Value> result;
  if (extra.isExtensible.IsEmpty() ||
      !extra.isExtensible.Get(aCx->isolate)
           ->Call(v8.context(), v8::Undefined(aCx->isolate), 1, &arg)
           .ToLocal(&result)) {
    return v8.Check(tryCatch);
  }
  *aResult = result->IsTrue();
  return true;
}

bool ProxyPrototype(JSContext* aCx, JS::HandleObject aObj,
                    JS::MutableHandleObject aResult, bool* aDone) {
  *aDone = false;
  if (!js::IsProxy(aObj)) {
    return true;
  }
  const js::BaseProxyHandler* handler = js::GetProxyHandler(aObj);
  bool ordinary = true;
  if (!handler->getPrototypeIfOrdinary(aCx, aObj, &ordinary, aResult)) {
    return false;
  }
  if (!ordinary) {
    *aDone = true;
    return handler->getPrototype(aCx, aObj, aResult);
  }
  *aDone = true;
  return true;
}

void AppendAscii(std::u16string& aOut, const char* aText) {
  while (*aText) {
    aOut.push_back(char16_t(static_cast<unsigned char>(*aText++)));
  }
}

void AppendV8String(v8::Isolate* aIsolate, std::u16string& aOut,
                    v8::Local<v8::String> aString) {
  size_t start = aOut.size();
  aOut.resize(start + aString->Length());
  aString->Write(aIsolate, reinterpret_cast<uint16_t*>(&aOut[start]), 0,
                 aString->Length(), v8::String::NO_NULL_TERMINATION);
}

void AppendQuoted(v8::Isolate* aIsolate, std::u16string& aOut,
                  v8::Local<v8::String> aString) {
  std::u16string raw;
  AppendV8String(aIsolate, raw, aString);
  aOut.push_back(u'"');
  for (char16_t c : raw) {
    switch (c) {
      case u'"':
        AppendAscii(aOut, "\\\"");
        break;
      case u'\\':
        AppendAscii(aOut, "\\\\");
        break;
      case u'\n':
        AppendAscii(aOut, "\\n");
        break;
      case u'\r':
        AppendAscii(aOut, "\\r");
        break;
      case u'\t':
        AppendAscii(aOut, "\\t");
        break;
      case u'\b':
        AppendAscii(aOut, "\\b");
        break;
      case u'\f':
        AppendAscii(aOut, "\\f");
        break;
      case u'\v':
        AppendAscii(aOut, "\\v");
        break;
      default:
        if (c < 0x20 || (c >= 0x7f && c < 0xa0)) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\x%02X", unsigned(c));
          AppendAscii(aOut, buf);
        } else {
          aOut.push_back(c);
        }
    }
  }
  aOut.push_back(u'"');
}

bool AppendSource(JSContext* aCx, v8::Local<v8::Context> aContext,
                  v8::Local<v8::Value> aValue, std::u16string& aOut,
                  std::vector<v8::Local<v8::Object>>& aStack);

bool AppendSymbolSource(JSContext* aCx, v8::Local<v8::Symbol> aSymbol,
                        std::u16string& aOut) {
  v8::Isolate* isolate = aCx->isolate;
  for (uint32_t code = 0; code < uint32_t(JS::SymbolCode::Limit); code++) {
    JS::Symbol* wellKnown = WellKnownSymbol(aCx, JS::SymbolCode(code));
    if (ToV8(aCx, wellKnown) == aSymbol) {
      v8::Local<v8::Value> description = aSymbol->Description(isolate);
      if (description->IsString()) {
        AppendV8String(isolate, aOut, description.As<v8::String>());
        return true;
      }
    }
  }
  v8::Local<v8::Value> description = aSymbol->Description(isolate);
  if (!description->IsString()) {
    AppendAscii(aOut, "Symbol()");
    return true;
  }
  v8::Local<v8::String> desc = description.As<v8::String>();
  bool registered = v8::Symbol::For(isolate, desc) == aSymbol;
  AppendAscii(aOut, registered ? "Symbol.for(" : "Symbol(");
  AppendQuoted(isolate, aOut, desc);
  aOut.push_back(u')');
  return true;
}

bool AppendObjectSource(JSContext* aCx, v8::Local<v8::Context> aContext,
                        v8::Local<v8::Object> aObject, std::u16string& aOut,
                        std::vector<v8::Local<v8::Object>>& aStack) {
  v8::Isolate* isolate = aCx->isolate;
  for (v8::Local<v8::Object>& seen : aStack) {
    if (seen == aObject) {
      AppendAscii(aOut, aObject->IsArray() ? "[]" : "{}");
      return true;
    }
  }
  if (aStack.size() > 1000) {
    JS_ReportErrorNumberASCII(aCx, js::GetErrorMessage, nullptr,
                              JSMSG_OVER_RECURSED);
    return false;
  }
  v8::Local<v8::Value> toSource;
  if (aObject
          ->Get(aContext, v8::String::NewFromUtf8Literal(isolate, "toSource"))
          .ToLocal(&toSource) &&
      toSource->IsFunction()) {
    v8::Local<v8::Value> result;
    if (!toSource.As<v8::Function>()->Call(aContext, aObject, 0, nullptr).ToLocal(
            &result)) {
      return false;
    }
    v8::Local<v8::String> text;
    if (!result->ToString(aContext).ToLocal(&text)) {
      return false;
    }
    AppendV8String(isolate, aOut, text);
    return true;
  }
  if (aObject->IsFunction()) {
    v8::Local<v8::String> text;
    if (!aObject.As<v8::Function>()->FunctionProtoToString(aContext).ToLocal(
            &text)) {
      return false;
    }
    AppendV8String(isolate, aOut, text);
    return true;
  }
  aStack.push_back(aObject);
  bool ok = true;
  if (aObject->IsArray()) {
    v8::Local<v8::Array> array = aObject.As<v8::Array>();
    aOut.push_back(u'[');
    for (uint32_t i = 0; ok && i < array->Length(); i++) {
      if (i) {
        AppendAscii(aOut, ", ");
      }
      if (!array->HasRealIndexedProperty(aContext, i).FromMaybe(false)) {
        if (i + 1 == array->Length()) {
          aOut.push_back(u',');
        }
        continue;
      }
      v8::Local<v8::Value> element;
      ok = array->Get(aContext, i).ToLocal(&element) &&
           AppendSource(aCx, aContext, element, aOut, aStack);
    }
    aOut.push_back(u']');
  } else if (aObject->IsNativeError()) {
    v8::Local<v8::Value> name, message;
    if (!aObject->Get(aContext, v8::String::NewFromUtf8Literal(isolate, "name"))
             .ToLocal(&name) ||
        !aObject
             ->Get(aContext, v8::String::NewFromUtf8Literal(isolate, "message"))
             .ToLocal(&message)) {
      aStack.pop_back();
      return false;
    }
    v8::Local<v8::Message> info = v8::Exception::CreateMessage(isolate, aObject);
    AppendAscii(aOut, "(new ");
    v8::Local<v8::String> nameText;
    if (!name.IsEmpty() && name->ToString(aContext).ToLocal(&nameText)) {
      AppendV8String(isolate, aOut, nameText);
    } else {
      AppendAscii(aOut, "Error");
    }
    aOut.push_back(u'(');
    v8::Local<v8::String> messageText = v8::String::Empty(isolate);
    if (!message->IsUndefined() &&
        !message->ToString(aContext).ToLocal(&messageText)) {
      aStack.pop_back();
      return false;
    }
    AppendQuoted(isolate, aOut, messageText);
    v8::Local<v8::Value> resource = info->GetScriptResourceName();
    if (resource->IsString()) {
      AppendAscii(aOut, ", ");
      AppendQuoted(isolate, aOut, resource.As<v8::String>());
      int line = info->GetLineNumber(aContext).FromMaybe(0);
      if (line) {
        char buf[16];
        snprintf(buf, sizeof(buf), ", %d", line);
        AppendAscii(aOut, buf);
      }
    }
    AppendAscii(aOut, "))");
  } else if (aObject->IsDate()) {
    char buf[64];
    snprintf(buf, sizeof(buf), "(new Date(%.17g))",
             aObject.As<v8::Date>()->ValueOf());
    AppendAscii(aOut, buf);
  } else if (aObject->IsRegExp() || aObject->IsNumberObject() ||
             aObject->IsBooleanObject() || aObject->IsStringObject()) {
    if (aObject->IsRegExp()) {
      v8::Local<v8::String> text;
      ok = aObject->ToString(aContext).ToLocal(&text);
      if (ok) {
        AppendV8String(isolate, aOut, text);
      }
    } else {
      AppendAscii(aOut, aObject->IsNumberObject()    ? "(new Number("
                        : aObject->IsBooleanObject() ? "(new Boolean("
                                                     : "(new String(");
      v8::Local<v8::Value> primitive;
      if (aObject->IsNumberObject()) {
        primitive = v8::Number::New(isolate,
                                    aObject.As<v8::NumberObject>()->ValueOf());
      } else if (aObject->IsBooleanObject()) {
        primitive = v8::Boolean::New(
            isolate, aObject.As<v8::BooleanObject>()->ValueOf());
      } else {
        primitive = aObject.As<v8::StringObject>()->ValueOf();
      }
      ok = AppendSource(aCx, aContext, primitive, aOut, aStack);
      AppendAscii(aOut, "))");
    }
  } else {
    v8::Local<v8::Array> names;
    ok = aObject
             ->GetPropertyNames(aContext, v8::KeyCollectionMode::kOwnOnly,
                                static_cast<v8::PropertyFilter>(
                                    v8::ONLY_ENUMERABLE | v8::SKIP_SYMBOLS),
                                v8::IndexFilter::kIncludeIndices,
                                v8::KeyConversionMode::kConvertToString)
             .ToLocal(&names);
    AppendAscii(aOut, "({");
    for (uint32_t i = 0; ok && i < names->Length(); i++) {
      v8::Local<v8::Value> key;
      v8::Local<v8::Value> value;
      if (!names->Get(aContext, i).ToLocal(&key) ||
          !aObject->Get(aContext, key).ToLocal(&value)) {
        ok = false;
        break;
      }
      if (i) {
        AppendAscii(aOut, ", ");
      }
      v8::Local<v8::String> keyText = key.As<v8::String>();
      JS::RootedId id(aCx);
      bool identifier = false;
      if (IdFromV8(aCx, keyText, &id) && id.isString()) {
        std::u16string chars;
        AppendV8String(isolate, chars, keyText);
        identifier = JS_IsIdentifier(chars.data(), chars.size());
      }
      if (identifier || id.isInt()) {
        AppendV8String(isolate, aOut, keyText);
      } else {
        AppendQuoted(isolate, aOut, keyText);
      }
      aOut.push_back(u':');
      ok = AppendSource(aCx, aContext, value, aOut, aStack);
    }
    AppendAscii(aOut, "})");
  }
  aStack.pop_back();
  return ok;
}

bool AppendSource(JSContext* aCx, v8::Local<v8::Context> aContext,
                  v8::Local<v8::Value> aValue, std::u16string& aOut,
                  std::vector<v8::Local<v8::Object>>& aStack) {
  v8::Isolate* isolate = aCx->isolate;
  if (aValue->IsUndefined()) {
    AppendAscii(aOut, "(void 0)");
    return true;
  }
  if (aValue->IsNull()) {
    AppendAscii(aOut, "null");
    return true;
  }
  if (aValue->IsString()) {
    AppendQuoted(isolate, aOut, aValue.As<v8::String>());
    return true;
  }
  if (aValue->IsSymbol()) {
    return AppendSymbolSource(aCx, aValue.As<v8::Symbol>(), aOut);
  }
  if (aValue->IsNumber()) {
    double d = aValue.As<v8::Number>()->Value();
    if (d == 0 && std::signbit(d)) {
      AppendAscii(aOut, "-0");
      return true;
    }
  }
  if (aValue->IsBigInt()) {
    v8::Local<v8::String> text;
    if (!aValue->ToString(aContext).ToLocal(&text)) {
      return false;
    }
    AppendV8String(isolate, aOut, text);
    aOut.push_back(u'n');
    return true;
  }
  if (aValue->IsObject()) {
    return AppendObjectSource(aCx, aContext, aValue.As<v8::Object>(), aOut,
                              aStack);
  }
  v8::Local<v8::String> text;
  if (!aValue->ToString(aContext).ToLocal(&text)) {
    return false;
  }
  AppendV8String(isolate, aOut, text);
  return true;
}

bool EnumerateStandard(JSContext* aCx, JS::HandleObject aObj,
                       JS::MutableHandleIdVector aProperties,
                       bool aEnumerableOnly, bool aIncludeResolved) {
  if (aEnumerableOnly) {
    return true;
  }
  v8::HandleScope scope(aCx->isolate);
  v8::Local<v8::Context> ctx = aCx->v8Context();
  v8::Local<v8::Object> global = ScriptTarget(aCx, aObj);
  JS::Realm* realm = RealmOfCell(aObj);
  auto append = [&](const char* aName) {
    v8::Local<v8::String> name =
        v8::String::NewFromUtf8(aCx->isolate, aName).ToLocalChecked();
    JS::RootedId id(aCx);
    return IdFromV8(aCx, name, &id) && aProperties.append(id);
  };
  if (!append("undefined")) {
    return false;
  }
  if (!aIncludeResolved) {
    return true;
  }
  if (!append("globalThis")) {
    return false;
  }
  for (size_t k = 1; k < JSProto_LIMIT; k++) {
    const char* name = StandardClassName(JSProtoKey(k));
    if (!name || (realm && IsDeselectedConstructor(realm, JSProtoKey(k)))) {
      continue;
    }
    if (!global
             ->HasRealNamedProperty(
                 ctx, v8::String::NewFromUtf8(aCx->isolate, name).ToLocalChecked())
             .FromMaybe(false)) {
      continue;
    }
    if (!append(name)) {
      return false;
    }
  }
  const char* const builtins[] = {"eval",        "NaN",        "Infinity",
                                  "isNaN",       "isFinite",   "parseFloat",
                                  "parseInt",    "escape",     "unescape",
                                  "decodeURI",   "encodeURI",  "decodeURIComponent",
                                  "encodeURIComponent"};
  for (const char* name : builtins) {
    if (!append(name)) {
      return false;
    }
  }
  return true;
}

bool IsStandardName(jsid aId) {
  if (!aId.isString()) {
    return false;
  }
  if (IdIsAsciiName(aId, "undefined") || IdIsAsciiName(aId, "globalThis")) {
    return true;
  }
  for (size_t k = 1; k < JSProto_LIMIT; k++) {
    const char* name = StandardClassName(JSProtoKey(k));
    if (name && IdIsAsciiName(aId, name)) {
      return true;
    }
  }
  const char* const builtins[] = {"eval",       "NaN",       "Infinity",
                                  "isNaN",      "isFinite",  "parseFloat",
                                  "parseInt",   "escape",    "unescape",
                                  "decodeURI",  "encodeURI", "decodeURIComponent",
                                  "encodeURIComponent", "uneval"};
  for (const char* name : builtins) {
    if (IdIsAsciiName(aId, name)) {
      return true;
    }
  }
  return false;
}

std::pair<uint8_t*, size_t> ViewBytes(JSObject* aObj, bool* aShared) {
  JSContext* cx = CurrentContext();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::Object> object = ToV8(cx, aObj);
  if (!object->IsArrayBufferView()) {
    *aShared = false;
    return {nullptr, 0};
  }
  v8::Local<v8::ArrayBufferView> view = object.As<v8::ArrayBufferView>();
  v8::Local<v8::Object> buffer = view->Buffer();
  uint8_t* base;
  if (buffer->IsSharedArrayBuffer()) {
    *aShared = true;
    base = static_cast<uint8_t*>(buffer.As<v8::SharedArrayBuffer>()->Data());
  } else {
    *aShared = false;
    base = static_cast<uint8_t*>(buffer.As<v8::ArrayBuffer>()->Data());
  }
  return {base ? base + view->ByteOffset() : nullptr, view->ByteLength()};
}

bool ProfilingReturnTrue(JSContext* aCx, unsigned aArgc, JS::Value* aVp) {
  JS::CallArgs args = JS::CallArgsFromVp(aArgc, aVp);
  args.rval().setBoolean(true);
  return true;
}

bool ProfilingReturnUndefined(JSContext* aCx, unsigned aArgc, JS::Value* aVp) {
  JS::CallArgs args = JS::CallArgsFromVp(aArgc, aVp);
  args.rval().setUndefined();
  return true;
}

bool MaxGCPause(JSContext* aCx, unsigned aArgc, JS::Value* aVp) {
  JS::CallArgs args = JS::CallArgsFromVp(aArgc, aVp);
  args.rval().setNumber(0.0);
  return true;
}

const JSFunctionSpec kProfilingFunctions[] = {
    JS_FN("startProfiling", ProfilingReturnTrue, 1, 0),
    JS_FN("stopProfiling", ProfilingReturnTrue, 1, 0),
    JS_FN("pauseProfilers", ProfilingReturnTrue, 1, 0),
    JS_FN("resumeProfilers", ProfilingReturnTrue, 1, 0),
    JS_FN("dumpProfile", ProfilingReturnTrue, 2, 0),
    JS_FN("getMaxGCPauseSinceClear", MaxGCPause, 0, 0),
    JS_FN("clearMaxGCPauseAccumulator", ProfilingReturnUndefined, 0, 0),
    JS_FS_END};

bool TestingGC(JSContext* aCx, unsigned aArgc, JS::Value* aVp) {
  JS::CallArgs args = JS::CallArgsFromVp(aArgc, aVp);
  aCx->isolate->LowMemoryNotification();
  args.rval().setUndefined();
  return true;
}

bool TestingMinorGC(JSContext* aCx, unsigned aArgc, JS::Value* aVp) {
  JS::CallArgs args = JS::CallArgsFromVp(aArgc, aVp);
  aCx->isolate->MemoryPressureNotification(v8::MemoryPressureLevel::kModerate);
  args.rval().setUndefined();
  return true;
}

const JSFunctionSpec kTestingFunctions[] = {
    JS_FN("gc", TestingGC, 0, 0), JS_FN("minorgc", TestingMinorGC, 0, 0),
    JS_FS_END};

}

}

using namespace vge::jsapi;

int64_t JS_Now() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

const char* JS_GetImplementationVersion(void) {
  return "JavaScript-C" MOZILLA_VERSION;
}

void JS::AutoFilename::reset() {
  if (filename_.is<const char*>()) {
    filename_.as<const char*>() = nullptr;
  } else {
    filename_.as<JS::UniqueChars>().reset();
  }
}

void JS::AutoFilename::setUnowned(const char* aFilename) {
  filename_.as<const char*>() = aFilename ? aFilename : "";
}

void JS::AutoFilename::setOwned(JS::UniqueChars&& aFilename) {
  filename_ = mozilla::AsVariant(std::move(aFilename));
}

const char* JS::AutoFilename::get() const {
  if (filename_.is<const char*>()) {
    return filename_.as<const char*>();
  }
  return filename_.as<JS::UniqueChars>().get();
}

bool JS::DescribeScriptedCaller(JS::AutoFilename* aFilename, JSContext* aCx,
                                uint32_t* aLineno,
                                JS::ColumnNumberOneOrigin* aColumn) {
  if (aFilename) {
    aFilename->reset();
  }
  if (aLineno) {
    *aLineno = 0;
  }
  if (aColumn) {
    *aColumn = JS::ColumnNumberOneOrigin();
  }
  if (!aCx->realm()) {
    return false;
  }
  v8::HandleScope scope(aCx->isolate);
  v8::Local<v8::StackFrame> frame = TopFrame(aCx);
  if (frame.IsEmpty() || CallerIsHidden(aCx)) {
    return false;
  }
  if (aFilename) {
    std::string name;
    if (ScriptData* data = ScriptDataByV8Id(frame->GetScriptId())) {
      name = data->filename;
    } else {
      v8::Local<v8::String> scriptName = frame->GetScriptName();
      if (!scriptName.IsEmpty()) {
        name = Utf8Of(aCx->isolate, scriptName);
      }
    }
    JS::UniqueChars copy(js_pod_malloc<char>(name.size() + 1));
    if (!copy) {
      aFilename->setUnowned("out of memory");
    } else {
      memcpy(copy.get(), name.c_str(), name.size() + 1);
      aFilename->setOwned(std::move(copy));
    }
  }
  if (aLineno) {
    *aLineno = uint32_t(frame->GetLineNumber());
  }
  if (aColumn) {
    *aColumn = JS::ColumnNumberOneOrigin(uint32_t(frame->GetColumn()));
  }
  return true;
}

JSObject* JS::GetScriptedCallerGlobal(JSContext* aCx) {
  v8::HandleScope scope(aCx->isolate);
  v8::Local<v8::StackFrame> frame = TopFrame(aCx);
  if (frame.IsEmpty() || CallerIsHidden(aCx)) {
    return nullptr;
  }
  if (ScriptData* data = ScriptDataByV8Id(frame->GetScriptId())) {
    if (data->realm && data->realm->global) {
      return data->realm->global;
    }
  }
  JS::Realm* realm =
      RealmOfV8Context(aCx, aCx->isolate->GetEnteredOrMicrotaskContext());
  return realm ? realm->global : nullptr;
}

void JS::HideScriptedCaller(JSContext* aCx) {
  State(aCx).hiddenCallerDepths.push_back(CurrentFrameCount(aCx->isolate));
}

void JS::UnhideScriptedCaller(JSContext* aCx) {
  std::vector<int>& hidden = State(aCx).hiddenCallerDepths;
  if (!hidden.empty()) {
    hidden.pop_back();
  }
}

bool JS::GetFirstArgumentAsTypeHint(JSContext* aCx, const JS::CallArgs& aArgs,
                                    JSType* aResult) {
  if (aArgs.get(0).isString()) {
    jsid id = JS::PropertyKey::Void();
    JS::RootedId rooted(aCx);
    {
      v8::HandleScope scope(aCx->isolate);
      if (!IdFromV8(aCx, ToV8(aCx, aArgs.get(0).toString()), &rooted)) {
        return false;
      }
    }
    id = rooted;
    if (IdIsAsciiName(id, "default")) {
      *aResult = JSTYPE_UNDEFINED;
      return true;
    }
    if (IdIsAsciiName(id, "string")) {
      *aResult = JSTYPE_STRING;
      return true;
    }
    if (IdIsAsciiName(id, "number")) {
      *aResult = JSTYPE_NUMBER;
      return true;
    }
  }
  JS::RootedString source(aCx, JS_ValueToSource(aCx, aArgs.get(0)));
  if (!source) {
    return false;
  }
  JS::UniqueChars bytes = JS_EncodeStringToUTF8(aCx, source);
  if (!bytes) {
    return false;
  }
  return ReportJSError(aCx, JSMSG_NOT_EXPECTED_TYPE, "Symbol.toPrimitive",
                       "\"string\", \"number\", or \"default\"", bytes.get());
}

bool JS::IsUnwrappedSavedFrame(JSObject* aObj) {
  return aObj && IsSavedFrameObject(aObj);
}

bool JS::IsMaybeWrappedSavedFrame(JSObject* aObj) {
  if (!aObj) {
    return false;
  }
  JSObject* unwrapped = js::UncheckedUnwrap(aObj, true);
  return unwrapped && IsSavedFrameObject(unwrapped);
}

bool js::IsSavedFrame(JSObject* aObj) { return IsSavedFrameObject(aObj); }

bool JS_CharsToId(JSContext* aCx, JS::TwoByteChars aChars,
                  JS::MutableHandleId aIdp) {
  v8::HandleScope scope(aCx->isolate);
  v8::Local<v8::String> name;
  if (!v8::String::NewFromTwoByte(
           aCx->isolate, reinterpret_cast<const uint16_t*>(aChars.begin().get()),
           v8::NewStringType::kInternalized, int(aChars.length()))
           .ToLocal(&name)) {
    JS_ReportOutOfMemory(aCx);
    return false;
  }
  aIdp.set(JS::PropertyKey::NonIntAtom(AtomFromV8(aCx, name)));
  return true;
}

bool JS_IndexToId(JSContext* aCx, uint32_t aIndex, JS::MutableHandleId aId) {
  if (aIndex <= uint32_t(JS::PropertyKey::IntMax)) {
    aId.set(JS::PropertyKey::Int(int32_t(aIndex)));
    return true;
  }
  v8::HandleScope scope(aCx->isolate);
  return IdFromV8(aCx, v8::Integer::NewFromUnsigned(aCx->isolate, aIndex)
                           ->ToString(aCx->v8Context())
                           .ToLocalChecked(),
                  aId);
}

bool JS_IsIdentifier(const char16_t* aChars, size_t aLength) {
  if (aLength == 0) {
    return false;
  }
  size_t i = 0;
  bool first = true;
  while (i < aLength) {
    char32_t c = aChars[i++];
    if (c >= 0xD800 && c <= 0xDBFF && i < aLength && aChars[i] >= 0xDC00 &&
        aChars[i] <= 0xDFFF) {
      c = 0x10000 + ((c - 0xD800) << 10) + (aChars[i++] - 0xDC00);
    }
    bool ok;
    if (first) {
      ok = c == '$' || c == '_' || u_hasBinaryProperty(UChar32(c), UCHAR_ID_START);
    } else {
      ok = c == '$' || c == 0x200C || c == 0x200D ||
           u_hasBinaryProperty(UChar32(c), UCHAR_ID_CONTINUE);
    }
    if (!ok) {
      return false;
    }
    first = false;
  }
  return true;
}

bool JS_FreezeObject(JSContext* aCx, JS::HandleObject aObj) {
  AutoV8 v8(aCx);
  v8::TryCatch tryCatch(aCx->isolate);
  v8::Maybe<bool> ok = ScriptTarget(aCx, aObj)->SetIntegrityLevel(
      v8.context(), v8::IntegrityLevel::kFrozen);
  if (ok.IsNothing()) {
    return v8.Check(tryCatch);
  }
  return true;
}

bool JS_DeepFreezeObject(JSContext* aCx, JS::HandleObject aObj) {
  bool extensible = false;
  if (!IsExtensible(aCx, aObj, &extensible)) {
    return false;
  }
  if (!extensible) {
    return true;
  }
  if (!JS_FreezeObject(aCx, aObj)) {
    return false;
  }
  JS::RootedValueVector values(aCx);
  {
    AutoV8 v8(aCx);
    v8::Local<v8::Context> ctx = v8.context();
    v8::TryCatch tryCatch(aCx->isolate);
    v8::Local<v8::Object> object = ScriptTarget(aCx, aObj);
    v8::Local<v8::Array> names;
    if (!object
             ->GetPropertyNames(ctx, v8::KeyCollectionMode::kOwnOnly,
                                v8::ALL_PROPERTIES,
                                v8::IndexFilter::kIncludeIndices,
                                v8::KeyConversionMode::kKeepNumbers)
             .ToLocal(&names)) {
      return v8.Check(tryCatch);
    }
    v8::Local<v8::String> valueKey =
        v8::String::NewFromUtf8Literal(aCx->isolate, "value");
    for (uint32_t i = 0; i < names->Length(); i++) {
      v8::Local<v8::Value> key, descriptor, value;
      if (!names->Get(ctx, i).ToLocal(&key)) {
        return v8.Check(tryCatch);
      }
      v8::Local<v8::Name> name;
      if (key->IsName()) {
        name = key.As<v8::Name>();
      } else {
        v8::Local<v8::String> text;
        if (!key->ToString(ctx).ToLocal(&text)) {
          return v8.Check(tryCatch);
        }
        name = text;
      }
      if (!object->GetOwnPropertyDescriptor(ctx, name).ToLocal(&descriptor) ||
          !descriptor->IsObject()) {
        continue;
      }
      if (!descriptor.As<v8::Object>()->Get(ctx, valueKey).ToLocal(&value) ||
          !value->IsObject()) {
        continue;
      }
      if (!values.append(ValueFromV8(aCx, value))) {
        return false;
      }
    }
  }
  for (size_t i = 0; i < values.length(); i++) {
    JS::RootedObject child(aCx, &values[i].toObject());
    if (!JS_DeepFreezeObject(aCx, child)) {
      return false;
    }
  }
  return true;
}

bool JS_DefineProfilingFunctions(JSContext* aCx, JS::HandleObject aObj) {
  return JS_DefineFunctions(aCx, aObj, kProfilingFunctions);
}

JSObject* js::GetTestingFunctions(JSContext* aCx) {
  JS::RootedObject obj(aCx, JS_NewPlainObject(aCx));
  if (!obj || !JS_DefineFunctions(aCx, obj, kTestingFunctions)) {
    return nullptr;
  }
  return obj;
}

bool JS_InitReflectParse(JSContext* aCx, JS::HandleObject aGlobal) {
  return true;
}

bool JS_GetClassObject(JSContext* aCx, JSProtoKey aKey,
                       JS::MutableHandleObject aObjp) {
  JS::Realm* realm = aCx->realm();
  if (realm && aKey > JSProto_Null && aKey < JSProto_LIMIT) {
    RealmExtra& extra = RealmData(realm);
    if (!extra.ctors[aKey].IsEmpty()) {
      v8::HandleScope scope(aCx->isolate);
      aObjp.set(ObjectFromV8(aCx, extra.ctors[aKey].Get(aCx->isolate)));
      return true;
    }
  }
  const char* name = StandardClassName(aKey);
  return ReportJSError(aCx, JSMSG_CONSTRUCTOR_DISABLED,
                       name ? name : "constructor");
}

bool JS_GetClassPrototype(JSContext* aCx, JSProtoKey aKey,
                          JS::MutableHandleObject aObjp) {
  if (aKey == JSProto_BoundFunction) {
    aKey = JSProto_Function;
  }
  JS::Realm* realm = aCx->realm();
  if (realm && aKey > JSProto_Null && aKey < JSProto_LIMIT) {
    RealmExtra& extra = RealmData(realm);
    if (!extra.protos[aKey].IsEmpty()) {
      v8::HandleScope scope(aCx->isolate);
      aObjp.set(ObjectFromV8(aCx, extra.protos[aKey].Get(aCx->isolate)));
      return true;
    }
  }
  const char* name = StandardClassName(aKey);
  return ReportJSError(aCx, JSMSG_CONSTRUCTOR_DISABLED,
                       name ? name : "constructor");
}

JSProtoKey JS_IdToProtoKey(JSContext* aCx, JS::HandleId aId) {
  if (!aId.isString()) {
    return JSProto_Null;
  }
  if (aCx->realm() && !aCx->realm()->creationOptions.getToSourceEnabled() &&
      IdIsAsciiName(aId, "uneval")) {
    return JSProto_Null;
  }
  for (size_t k = 1; k < JSProto_LIMIT; k++) {
    const char* name = StandardClassName(JSProtoKey(k));
    if (name && IdIsAsciiName(aId, name)) {
      if (aCx->realm() && IsDeselectedConstructor(aCx->realm(), JSProtoKey(k))) {
        return JSProto_Null;
      }
      return JSProtoKey(k);
    }
  }
  return JSProto_Null;
}

bool JS_ResolveStandardClass(JSContext* aCx, JS::HandleObject aObj,
                             JS::HandleId aId, bool* aResolved) {
  *aResolved = false;
  return true;
}

bool JS_MayResolveStandardClass(const JSAtomState& aNames, jsid aId,
                                JSObject* aMaybeObj) {
  if (!aMaybeObj) {
    return true;
  }
  return IsStandardName(aId);
}

bool JS_NewEnumerateStandardClasses(JSContext* aCx, JS::HandleObject aObj,
                                    JS::MutableHandleIdVector aProperties,
                                    bool aEnumerableOnly) {
  return EnumerateStandard(aCx, aObj, aProperties, aEnumerableOnly, false);
}

bool JS_NewEnumerateStandardClassesIncludingResolved(
    JSContext* aCx, JS::HandleObject aObj, JS::MutableHandleIdVector aProperties,
    bool aEnumerableOnly) {
  return EnumerateStandard(aCx, aObj, aProperties, aEnumerableOnly, true);
}

bool JS_GetPrototype(JSContext* aCx, JS::HandleObject aObj,
                     JS::MutableHandleObject aResult) {
  bool done = false;
  if (!ProxyPrototype(aCx, aObj, aResult, &done)) {
    return false;
  }
  if (done && !js::IsProxy(aObj)) {
    return true;
  }
  if (done) {
    bool ordinary = true;
    JS::RootedObject probe(aCx);
    js::GetProxyHandler(aObj)->getPrototypeIfOrdinary(aCx, aObj, &ordinary,
                                                     &probe);
    if (!ordinary) {
      return true;
    }
  }
  v8::HandleScope scope(aCx->isolate);
  v8::Local<v8::Value> proto = ScriptTarget(aCx, aObj)->GetPrototype();
  aResult.set(proto->IsObject() ? ObjectFromV8(aCx, proto.As<v8::Object>())
                                : nullptr);
  return true;
}

bool js::GetObjectProto(JSContext* aCx, JS::Handle<JSObject*> aObj,
                        JS::MutableHandle<JSObject*> aProto) {
  return JS_GetPrototype(aCx, aObj, aProto);
}

JSObject* js::GetStaticPrototype(JSObject* aObj) {
  JSContext* cx = CurrentContext();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::Value> proto = ScriptTarget(cx, aObj)->GetPrototype();
  return proto->IsObject() ? ObjectFromV8(cx, proto.As<v8::Object>()) : nullptr;
}

bool JS_SetPrototype(JSContext* aCx, JS::HandleObject aObj,
                     JS::HandleObject aProto) {
  if (js::IsProxy(aObj)) {
    bool ordinary = true;
    JS::RootedObject current(aCx);
    const js::BaseProxyHandler* handler = js::GetProxyHandler(aObj);
    if (!handler->getPrototypeIfOrdinary(aCx, aObj, &ordinary, &current)) {
      return false;
    }
    if (!ordinary) {
      JS::ObjectOpResult result;
      return handler->setPrototype(aCx, aObj, aProto, result) &&
             result.checkStrict(aCx, aObj);
    }
  }
  AutoV8 v8(aCx);
  v8::Isolate* isolate = aCx->isolate;
  v8::Local<v8::Context> ctx = v8.context();
  v8::Local<v8::Object> target = ScriptTarget(aCx, aObj);
  v8::Local<v8::Value> proto = aProto ? ToV8(aCx, aProto).As<v8::Value>()
                                      : v8::Null(isolate).As<v8::Value>();
  if (target->GetPrototype() == proto) {
    return true;
  }
  ClassesState& state = State(aCx);
  if (!state.immutableProtoKey.IsEmpty() &&
      target->HasPrivate(ctx, state.immutableProtoKey.Get(isolate))
          .FromMaybe(false)) {
    JS::ObjectOpResult result;
    result.failCantSetProto();
    return result.checkStrict(aCx, aObj);
  }
  v8::TryCatch tryCatch(isolate);
  if (target->SetPrototype(ctx, proto).IsNothing()) {
    return v8.Check(tryCatch);
  }
  return true;
}

bool JS_SetImmutablePrototype(JSContext* aCx, JS::HandleObject aObj,
                              bool* aSucceeded) {
  if (js::IsProxy(aObj)) {
    return js::GetProxyHandler(aObj)->setImmutablePrototype(aCx, aObj,
                                                           aSucceeded);
  }
  AutoV8 v8(aCx);
  v8::Isolate* isolate = aCx->isolate;
  ClassesState& state = State(aCx);
  if (state.immutableProtoKey.IsEmpty()) {
    state.immutableProtoKey.Reset(
        isolate, v8::Private::New(isolate, v8::String::NewFromUtf8Literal(
                                               isolate, "vge.jsapi.immutableProto")));
  }
  ScriptTarget(aCx, aObj)
      ->SetPrivate(v8.context(), state.immutableProtoKey.Get(isolate),
                   v8::True(isolate))
      .Check();
  *aSucceeded = true;
  return true;
}

bool JS_HasInstance(JSContext* aCx, JS::HandleObject aObj,
                    JS::HandleValue aValue, bool* aBp) {
  AutoV8 v8(aCx);
  v8::TryCatch tryCatch(aCx->isolate);
  v8::Maybe<bool> result =
      ToV8(aCx, aValue)->InstanceOf(v8.context(), ToV8(aCx, aObj));
  if (result.IsNothing()) {
    return v8.Check(tryCatch);
  }
  *aBp = result.FromJust();
  return true;
}

bool JS_LinkConstructorAndPrototype(JSContext* aCx, JS::HandleObject aCtor,
                                    JS::HandleObject aProto) {
  JS::RootedValue protoVal(aCx, JS::ObjectValue(*aProto));
  JS::RootedValue ctorVal(aCx, JS::ObjectValue(*aCtor));
  return JS_DefineProperty(aCx, aCtor, "prototype", protoVal,
                           JSPROP_PERMANENT | JSPROP_READONLY) &&
         JS_DefineProperty(aCx, aProto, "constructor", ctorVal, 0);
}

bool JS_ValueToObject(JSContext* aCx, JS::HandleValue aValue,
                      JS::MutableHandleObject aObjp) {
  if (aValue.isNullOrUndefined()) {
    aObjp.set(nullptr);
    return true;
  }
  if (aValue.isObject()) {
    aObjp.set(&aValue.toObject());
    return true;
  }
  AutoV8 v8(aCx);
  v8::TryCatch tryCatch(aCx->isolate);
  v8::Local<v8::Object> object;
  if (!ToV8(aCx, aValue)->ToObject(v8.context()).ToLocal(&object)) {
    return v8.Check(tryCatch);
  }
  aObjp.set(ObjectFromV8(aCx, object));
  return true;
}

JSString* JS_ValueToSource(JSContext* aCx, JS::HandleValue aValue) {
  AutoV8 v8(aCx);
  v8::TryCatch tryCatch(aCx->isolate);
  std::u16string out;
  std::vector<v8::Local<v8::Object>> stack;
  if (!AppendSource(aCx, v8.context(), ToV8(aCx, aValue), out, stack)) {
    v8.Check(tryCatch);
    return nullptr;
  }
  v8::Local<v8::String> text;
  if (!v8::String::NewFromTwoByte(aCx->isolate,
                                  reinterpret_cast<const uint16_t*>(out.data()),
                                  v8::NewStringType::kNormal, int(out.size()))
           .ToLocal(&text)) {
    JS_ReportOutOfMemory(aCx);
    return nullptr;
  }
  return StringFromV8(aCx, text);
}

JSType JS_TypeOfValue(JSContext* aCx, JS::HandleValue aValue) {
  if (aValue.isUndefined()) return JSTYPE_UNDEFINED;
  if (aValue.isNull()) return JSTYPE_OBJECT;
  if (aValue.isBoolean()) return JSTYPE_BOOLEAN;
  if (aValue.isNumber()) return JSTYPE_NUMBER;
  if (aValue.isString()) return JSTYPE_STRING;
  if (aValue.isSymbol()) return JSTYPE_SYMBOL;
  if (aValue.isBigInt()) return JSTYPE_BIGINT;
  JSObject* obj = &aValue.toObject();
  if (JS::GetClass(obj)->emulatesUndefined()) {
    return JSTYPE_UNDEFINED;
  }
  v8::HandleScope scope(aCx->isolate);
  v8::Local<v8::Object> object = ToV8(aCx, obj);
  if (object->IsUndetectable()) {
    return JSTYPE_UNDEFINED;
  }
  return object->IsCallable() ? JSTYPE_FUNCTION : JSTYPE_OBJECT;
}

mozilla::Maybe<JSExnType> JS_GetErrorType(const JS::Value& aVal) {
  if (!aVal.isObject()) {
    return mozilla::Nothing();
  }
  JSContext* cx = CurrentContext();
  JSObject* obj = &aVal.toObject();
  v8::HandleScope scope(cx->isolate);
  if (!ToV8(cx, obj)->IsNativeError()) {
    return mozilla::Nothing();
  }
  bool isPrototype = false;
  switch (StandardKeyOf(cx, obj, &isPrototype)) {
    case JSProto_InternalError:
      return mozilla::Some(JSEXN_INTERNALERR);
    case JSProto_AggregateError:
      return mozilla::Some(JSEXN_AGGREGATEERR);
    case JSProto_EvalError:
      return mozilla::Some(JSEXN_EVALERR);
    case JSProto_RangeError:
      return mozilla::Some(JSEXN_RANGEERR);
    case JSProto_ReferenceError:
      return mozilla::Some(JSEXN_REFERENCEERR);
    case JSProto_SuppressedError:
      return mozilla::Some(JSEXN_SUPPRESSEDERR);
    case JSProto_SyntaxError:
      return mozilla::Some(JSEXN_SYNTAXERR);
    case JSProto_TypeError:
      return mozilla::Some(JSEXN_TYPEERR);
    case JSProto_URIError:
      return mozilla::Some(JSEXN_URIERR);
    case JSProto_CompileError:
      return mozilla::Some(JSEXN_WASMCOMPILEERROR);
    case JSProto_LinkError:
      return mozilla::Some(JSEXN_WASMLINKERROR);
    case JSProto_RuntimeError:
      return mozilla::Some(JSEXN_WASMRUNTIMEERROR);
    default:
      return mozilla::Some(JSEXN_ERR);
  }
}

JSLinearString* js::GetErrorTypeName(JSContext* aCx, int16_t aExnType) {
  static const JSProtoKey kKeys[] = {
      JSProto_Error,          JSProto_InternalError,  JSProto_AggregateError,
      JSProto_EvalError,      JSProto_RangeError,     JSProto_ReferenceError,
      JSProto_SuppressedError, JSProto_SyntaxError,   JSProto_TypeError,
      JSProto_URIError,       JSProto_DebuggeeWouldRun, JSProto_CompileError,
      JSProto_LinkError,      JSProto_RuntimeError};
  if (aExnType < 0 || aExnType >= JSEXN_ERROR_LIMIT ||
      aExnType == JSEXN_INTERNALERR ||
      size_t(aExnType) >= sizeof(kKeys) / sizeof(kKeys[0])) {
    return nullptr;
  }
  JSString* atom = JS_AtomizeString(aCx, StandardClassName(kKeys[aExnType]));
  return reinterpret_cast<JSLinearString*>(atom);
}

const char* JS_GetScriptFilename(JSScript* aScript) {
  return ScriptDataOf(aScript)->filename.c_str();
}

unsigned JS_GetScriptBaseLineNumber(JSContext* aCx, JSScript* aScript) {
  return ScriptDataOf(aScript)->lineno;
}

JSPrincipals* JS_GetScriptPrincipals(JSScript* aScript) {
  ScriptData* data = ScriptDataOf(aScript);
  return data->realm ? data->realm->principals : nullptr;
}

unsigned JS_PCToLineNumber(JSScript* aScript, jsbytecode* aPc,
                           JS::LimitedColumnNumberOneOrigin* aColumnp) {
  ScriptData* data = ScriptDataOf(aScript);
  if (aColumnp) {
    *aColumnp = JS::LimitedColumnNumberOneOrigin::fromUnlimited(data->column);
  }
  return data->lineno;
}

JSString* JS_DecompileScript(JSContext* aCx, JS::HandleScript aScript) {
  ScriptData* data = ScriptDataOf(aScript);
  AutoV8 v8(aCx);
  v8::TryCatch tryCatch(aCx->isolate);
  if (!data->function.IsEmpty()) {
    v8::Local<v8::String> text;
    if (!data->function.Get(aCx->isolate)
             ->FunctionProtoToString(v8.context())
             .ToLocal(&text)) {
      v8.Check(tryCatch);
      return nullptr;
    }
    return StringFromV8(aCx, text);
  }
  v8::Local<v8::String> text;
  if (!v8::String::NewFromTwoByte(
           aCx->isolate, reinterpret_cast<const uint16_t*>(data->source.data()),
           v8::NewStringType::kNormal, int(data->source.size()))
           .ToLocal(&text)) {
    JS_ReportOutOfMemory(aCx);
    return nullptr;
  }
  return StringFromV8(aCx, text);
}

JS::Value JS::GetScriptPrivate(JSScript* aScript) {
  return ScriptDataOf(aScript)->privateValue;
}

JS::Value JS::GetScriptedCallerPrivate(JSContext* aCx) {
  v8::HandleScope scope(aCx->isolate);
  v8::Local<v8::StackFrame> frame = TopFrame(aCx);
  if (frame.IsEmpty()) {
    return JS::UndefinedValue();
  }
  ScriptData* data = ScriptDataByV8Id(frame->GetScriptId());
  return data ? data->privateValue : JS::UndefinedValue();
}

void JS::SetScriptPrivateReferenceHooks(JSRuntime* aRt,
                                        JS::ScriptPrivateReferenceHook aAddRef,
                                        JS::ScriptPrivateReferenceHook aRelease) {
  ClassesState& state = State(CurrentContext());
  state.scriptPrivateAddRef = aAddRef;
  state.scriptPrivateRelease = aRelease;
}

const char* js::MaybeGetModuleFilename(JSObject* aObj) {
  if (JS::GetClass(aObj) != &ModuleObjectClass) {
    return nullptr;
  }
  JSContext* cx = CurrentContext();
  JS::RootedObject module(cx, aObj);
  JSScript* script = JS::GetModuleScript(module);
  return script ? ScriptDataOf(script)->filename.c_str() : nullptr;
}

bool js::ExecuteInFrameScriptEnvironment(JSContext* aCx,
                                         JS::HandleObject aObj,
                                         JS::HandleScript aScript,
                                         JS::MutableHandleObject aEnv) {
  JS::RootedValue rval(aCx);
  if (!ExecuteScriptCell(aCx, aScript, {aObj.get()}, nullptr, &rval)) {
    return false;
  }
  aEnv.set(aObj);
  return true;
}

void JS::DisableSpectreMitigationsAfterInit() {
  MOZ_RELEASE_ASSERT(CurrentContext());
}

void JS_AbortIfWrongThread(JSContext* aCx) {
  if (CurrentContext() != aCx) {
    MOZ_CRASH();
  }
}

void JS_SetGlobalJitCompilerOption(JSContext* aCx, JSJitCompilerOption aOpt,
                                   uint32_t aValue) {
  const char* flag = nullptr;
  switch (aOpt) {
    case JSJITCOMPILER_BASELINE_INTERPRETER_ENABLE:
      flag = aValue ? "--sparkplug" : "--no-sparkplug";
      break;
    case JSJITCOMPILER_BASELINE_ENABLE:
      flag = aValue ? "--maglev" : "--no-maglev";
      break;
    case JSJITCOMPILER_ION_ENABLE:
      flag = aValue ? "--turbofan" : "--no-turbofan";
      break;
    case JSJITCOMPILER_NATIVE_REGEXP_ENABLE:
      flag = aValue ? "--regexp-tier-up" : "--regexp-interpret-all";
      break;
    case JSJITCOMPILER_OFFTHREAD_COMPILATION_ENABLE:
      flag = aValue ? "--concurrent-recompilation"
                    : "--no-concurrent-recompilation";
      break;
    case JSJITCOMPILER_WASM_JIT_BASELINE:
      flag = aValue ? "--liftoff" : "--no-liftoff";
      break;
    case JSJITCOMPILER_WASM_JIT_OPTIMIZING:
      flag = aValue ? "--wasm-tier-up" : "--no-wasm-tier-up";
      break;
    default:
      break;
  }
  if (flag) {
    v8::V8::SetFlagsFromString(flag);
  }
}

void JS_SetOffthreadBaselineCompilationEnabled(JSContext* aCx, bool aEnabled) {
  v8::V8::SetFlagsFromString(aEnabled ? "--concurrent-sparkplug"
                                      : "--no-concurrent-sparkplug");
}

void JS_SetOffthreadIonCompilationEnabled(JSContext* aCx, bool aEnabled) {
  v8::V8::SetFlagsFromString(aEnabled ? "--concurrent-recompilation"
                                      : "--no-concurrent-recompilation");
}

bool JS::IsGCPoisoning() { return false; }

bool JS::dbg::ShouldAvoidSideEffects(JSContext* aCx) { return false; }

bool JS_InitializePropertiesFromCompatibleNativeObject(JSContext* aCx,
                                                       JS::HandleObject aDst,
                                                       JS::HandleObject aSrc) {
  AutoV8 v8(aCx);
  v8::Isolate* isolate = aCx->isolate;
  v8::Local<v8::Context> ctx = v8.context();
  v8::TryCatch tryCatch(isolate);
  v8::Local<v8::Object> src = ScriptTarget(aCx, aSrc);
  v8::Local<v8::Object> dst = ScriptTarget(aCx, aDst);
  if (dst->SetPrototype(ctx, src->GetPrototype()).IsNothing()) {
    return v8.Check(tryCatch);
  }
  v8::Local<v8::Array> names;
  if (!src->GetPropertyNames(ctx, v8::KeyCollectionMode::kOwnOnly,
                             v8::ALL_PROPERTIES, v8::IndexFilter::kIncludeIndices,
                             v8::KeyConversionMode::kConvertToString)
           .ToLocal(&names)) {
    return v8.Check(tryCatch);
  }
  for (uint32_t i = 0; i < names->Length(); i++) {
    v8::Local<v8::Value> key, descriptor;
    if (!names->Get(ctx, i).ToLocal(&key) || !key->IsName()) {
      return v8.Check(tryCatch);
    }
    if (!src->GetOwnPropertyDescriptor(ctx, key.As<v8::Name>())
             .ToLocal(&descriptor)) {
      return v8.Check(tryCatch);
    }
    v8::Local<v8::Object> d = descriptor.As<v8::Object>();
    auto field = [&](const char* aName) {
      v8::Local<v8::Value> v;
      if (!d->Get(ctx, v8::String::NewFromUtf8(isolate, aName).ToLocalChecked())
               .ToLocal(&v)) {
        v = v8::Undefined(isolate);
      }
      return v;
    };
    v8::Local<v8::Value> getter = field("get");
    v8::Local<v8::Value> setter = field("set");
    bool accessor = d->HasOwnProperty(ctx, v8::String::NewFromUtf8Literal(
                                               isolate, "get"))
                        .FromMaybe(false);
    std::unique_ptr<v8::PropertyDescriptor> desc;
    if (accessor) {
      desc = std::make_unique<v8::PropertyDescriptor>(getter, setter);
    } else {
      desc = std::make_unique<v8::PropertyDescriptor>(
          field("value"), field("writable")->IsTrue());
    }
    desc->set_enumerable(field("enumerable")->IsTrue());
    desc->set_configurable(field("configurable")->IsTrue());
    if (dst->DefineProperty(ctx, key.As<v8::Name>(), *desc).IsNothing()) {
      return v8.Check(tryCatch);
    }
  }
  for (uint32_t i = 0; i < JSCLASS_RESERVED_SLOTS(JS::GetClass(aSrc)) &&
                       i < kFixedSlots;
       i++) {
    JS_SetReservedSlot(aDst, i, JS::GetReservedSlot(aSrc, i));
  }
  return true;
}

static bool CollectEntries(JSContext* aCx, JS::HandleObject aObj, bool aKeysOnly,
                           JS::MutableHandle<JS::GCVector<JS::Value>> aOut) {
  AutoV8 v8(aCx);
  v8::TryCatch tryCatch(aCx->isolate);
  v8::Local<v8::Object> object = ToV8(aCx, aObj);
  v8::Local<v8::Array> entries;
  if (object->IsMap()) {
    entries = object.As<v8::Map>()->AsArray();
  } else if (object->IsSet()) {
    entries = object.As<v8::Set>()->AsArray();
  } else {
    bool keyValue = false;
    if (!object->PreviewEntries(&keyValue).ToLocal(&entries)) {
      return v8.Check(tryCatch);
    }
  }
  for (uint32_t i = 0; i < entries->Length(); i++) {
    v8::Local<v8::Value> value;
    if (!entries->Get(v8.context(), i).ToLocal(&value)) {
      return v8.Check(tryCatch);
    }
    if (!aOut.append(ValueFromV8(aCx, value))) {
      JS_ReportOutOfMemory(aCx);
      return false;
    }
  }
  return true;
}

bool js::GetMapObjectKeysAndValuesInterleaved(
    JSContext* aCx, JS::HandleObject aObj,
    JS::MutableHandle<JS::GCVector<JS::Value>> aEntries) {
  return CollectEntries(aCx, aObj, false, aEntries);
}

bool js::GetSetObjectKeys(JSContext* aCx, JS::HandleObject aObj,
                          JS::MutableHandle<JS::GCVector<JS::Value>> aKeys) {
  return CollectEntries(aCx, aObj, true, aKeys);
}

static bool WeakKeys(JSContext* aCx, JS::HandleObject aObjArg, bool aMap,
                     JS::MutableHandleObject aRet) {
  if (!aObjArg) {
    aRet.set(nullptr);
    return true;
  }
  JS::RootedObject obj(aCx, js::UncheckedUnwrap(aObjArg));
  AutoV8 v8(aCx);
  v8::Local<v8::Object> object = ToV8(aCx, obj);
  if (aMap ? !object->IsWeakMap() : !object->IsWeakSet()) {
    aRet.set(nullptr);
    return true;
  }
  v8::TryCatch tryCatch(aCx->isolate);
  bool keyValue = false;
  v8::Local<v8::Array> entries;
  if (!object->PreviewEntries(&keyValue).ToLocal(&entries)) {
    return v8.Check(tryCatch);
  }
  uint32_t step = keyValue ? 2 : 1;
  std::vector<v8::Local<v8::Value>> keys;
  for (uint32_t i = 0; i < entries->Length(); i += step) {
    v8::Local<v8::Value> key;
    if (!entries->Get(v8.context(), i).ToLocal(&key)) {
      return v8.Check(tryCatch);
    }
    keys.push_back(key);
  }
  v8::Local<v8::Array> array =
      v8::Array::New(aCx->isolate, keys.data(), keys.size());
  JS::RootedObject result(aCx, ObjectFromV8(aCx, array));
  if (!JS_WrapObject(aCx, &result)) {
    return false;
  }
  aRet.set(result);
  return true;
}

bool JS_NondeterministicGetWeakMapKeys(JSContext* aCx, JS::HandleObject aObj,
                                       JS::MutableHandleObject aRet) {
  return WeakKeys(aCx, aObj, true, aRet);
}

bool JS_NondeterministicGetWeakSetKeys(JSContext* aCx, JS::HandleObject aObj,
                                       JS::MutableHandleObject aRet) {
  return WeakKeys(aCx, aObj, false, aRet);
}

bool js::AddRawValueRoot(JSContext* aCx, JS::Value* aVp, const char* aName) {
  ClassesState& state = State(aCx);
  if (!state.rawRootTracerInstalled) {
    aCx->runtime->blackRootTracers.emplace_back(RawRootsTracer, &state);
    state.rawRootTracerInstalled = true;
  }
  state.rawRoots.emplace_back(aVp, aName);
  return true;
}

void js::RemoveRawValueRoot(JSContext* aCx, JS::Value* aVp) {
  std::vector<std::pair<JS::Value*, const char*>>& roots = State(aCx).rawRoots;
  for (size_t i = 0; i < roots.size(); i++) {
    if (roots[i].first == aVp) {
      roots.erase(roots.begin() + i);
      return;
    }
  }
}

bool js::AppendUnique(JSContext* aCx, JS::MutableHandleIdVector aBase,
                      JS::HandleIdVector aOthers) {
  JS::RootedIdVector unique(aCx);
  if (!unique.reserve(aOthers.length())) {
    return false;
  }
  for (size_t i = 0; i < aOthers.length(); ++i) {
    bool isUnique = true;
    for (size_t j = 0; j < aBase.length(); ++j) {
      if (aOthers[i].get() == aBase[j]) {
        isUnique = false;
        break;
      }
    }
    if (isUnique && !unique.append(aOthers[i])) {
      return false;
    }
  }
  return aBase.appendAll(std::move(unique));
}

static bool DateValue(JSContext* aCx, JS::HandleObject aObj, double* aValue,
                      bool* aIsDate) {
  JSObject* unwrapped = js::CheckedUnwrapStatic(aObj);
  *aIsDate = false;
  if (!unwrapped) {
    return true;
  }
  v8::HandleScope scope(aCx->isolate);
  v8::Local<v8::Object> object = ToV8(aCx, unwrapped);
  if (!object->IsDate()) {
    return true;
  }
  *aIsDate = true;
  *aValue = object.As<v8::Date>()->ValueOf();
  return true;
}

bool js::DateIsValid(JSContext* aCx, JS::HandleObject aObj, bool* aIsValid) {
  double value = 0;
  bool isDate = false;
  if (!DateValue(aCx, aObj, &value, &isDate)) {
    return false;
  }
  *aIsValid = isDate && !std::isnan(value);
  return true;
}

bool js::DateGetMsecSinceEpoch(JSContext* aCx, JS::HandleObject aObj,
                               double* aMsec) {
  bool isDate = false;
  if (!DateValue(aCx, aObj, aMsec, &isDate)) {
    return false;
  }
  if (!isDate) {
    *aMsec = 0;
  }
  return true;
}

bool js::GetElementsWithAdder(JSContext* aCx, JS::HandleObject aObj,
                              JS::HandleObject aReceiver, uint32_t aBegin,
                              uint32_t aEnd, js::ElementAdder* aAdder) {
  JS::RootedValue val(aCx);
  for (uint32_t i = aBegin; i < aEnd; i++) {
    {
      AutoV8 v8(aCx);
      v8::TryCatch tryCatch(aCx->isolate);
      v8::Local<v8::Object> object = ScriptTarget(aCx, aObj);
      if (aAdder->getBehavior() ==
          js::ElementAdder::CheckHasElemPreserveHoles) {
        v8::Maybe<bool> found = object->Has(v8.context(), i);
        if (found.IsNothing()) {
          return v8.Check(tryCatch);
        }
        if (!found.FromJust()) {
          aAdder->appendHole();
          continue;
        }
      }
      v8::Local<v8::Value> element;
      if (!object->Get(v8.context(), i).ToLocal(&element)) {
        return v8.Check(tryCatch);
      }
      val = ValueFromV8(aCx, element);
    }
    if (!aAdder->append(aCx, val)) {
      return false;
    }
  }
  return true;
}

bool js::GetPropertyKeys(JSContext* aCx, JS::HandleObject aObj,
                         unsigned aFlags, JS::MutableHandleIdVector aProps) {
  AutoV8 v8(aCx);
  v8::TryCatch tryCatch(aCx->isolate);
  int filter = v8::ALL_PROPERTIES;
  if (!(aFlags & JSITER_HIDDEN)) {
    filter |= v8::ONLY_ENUMERABLE;
  }
  if (aFlags & JSITER_SYMBOLSONLY) {
    filter |= v8::SKIP_STRINGS;
  } else if (!(aFlags & JSITER_SYMBOLS)) {
    filter |= v8::SKIP_SYMBOLS;
  }
  v8::Local<v8::Array> names;
  if (!ScriptTarget(aCx, aObj)
           ->GetPropertyNames(v8.context(),
                              (aFlags & JSITER_OWNONLY)
                                  ? v8::KeyCollectionMode::kOwnOnly
                                  : v8::KeyCollectionMode::kIncludePrototypes,
                              static_cast<v8::PropertyFilter>(filter),
                              v8::IndexFilter::kIncludeIndices,
                              v8::KeyConversionMode::kKeepNumbers)
           .ToLocal(&names)) {
    return v8.Check(tryCatch);
  }
  for (uint32_t i = 0; i < names->Length(); i++) {
    v8::Local<v8::Value> key;
    JS::RootedId id(aCx);
    if (!names->Get(v8.context(), i).ToLocal(&key)) {
      return v8.Check(tryCatch);
    }
    if (!IdFromV8(aCx, key, &id) || !aProps.append(id)) {
      return false;
    }
  }
  return true;
}

bool JS_Enumerate(JSContext* aCx, JS::HandleObject aObj,
                  JS::MutableHandle<JS::IdVector> aProps) {
  JS::RootedIdVector ids(aCx);
  if (!js::GetPropertyKeys(aCx, aObj, JSITER_OWNONLY, &ids)) {
    return false;
  }
  return aProps.append(ids.begin(), ids.end());
}

bool js::GetRealmOriginalEval(JSContext* aCx, JS::MutableHandleObject aEval) {
  RealmExtra& extra = RealmData(aCx->realm());
  v8::HandleScope scope(aCx->isolate);
  aEval.set(extra.eval.IsEmpty()
                ? nullptr
                : ObjectFromV8(aCx, extra.eval.Get(aCx->isolate)));
  return true;
}

bool js::IsArgumentsObject(JS::HandleObject aObj) {
  JSContext* cx = CurrentContext();
  v8::HandleScope scope(cx->isolate);
  return ToV8(cx, aObj)->IsArgumentsObject();
}

bool js::SetPropertyIgnoringNamedGetter(
    JSContext* aCx, JS::HandleObject aObj, JS::HandleId aId,
    JS::HandleValue aV, JS::HandleValue aReceiver,
    JS::Handle<mozilla::Maybe<JS::PropertyDescriptor>> aOwnDesc,
    JS::ObjectOpResult& aResult) {
  JS::Rooted<JS::PropertyDescriptor> ownDesc(aCx);
  if (aOwnDesc.isNothing()) {
    JS::RootedObject proto(aCx);
    if (!JS_GetPrototype(aCx, aObj, &proto)) {
      return false;
    }
    if (proto) {
      AutoV8 v8(aCx);
      v8::TryCatch tryCatch(aCx->isolate);
      RealmExtra& extra = RealmData(aCx->realm());
      v8::Local<v8::Value> args[] = {ToV8(aCx, proto), ToV8(aCx, aId),
                                     ToV8(aCx, aV), ToV8(aCx, aReceiver)};
      v8::Local<v8::Value> ok;
      if (!extra.reflectSet.Get(aCx->isolate)
               ->Call(v8.context(), v8::Undefined(aCx->isolate), 4, args)
               .ToLocal(&ok)) {
        return v8.Check(tryCatch);
      }
      return ok->IsTrue() ? aResult.succeed() : aResult.failReadOnly();
    }
    ownDesc.set(JS::PropertyDescriptor::Data(
        JS::UndefinedValue(),
        {JS::PropertyAttribute::Configurable, JS::PropertyAttribute::Enumerable,
         JS::PropertyAttribute::Writable}));
  } else {
    ownDesc.set(*aOwnDesc);
  }
  if (ownDesc.isDataDescriptor()) {
    if (!ownDesc.writable()) {
      return aResult.failReadOnly();
    }
    if (!aReceiver.isObject()) {
      return aResult.fail(JSMSG_SET_NON_OBJECT_RECEIVER);
    }
    JS::RootedObject receiverObj(aCx, &aReceiver.toObject());
    JS::Rooted<mozilla::Maybe<JS::PropertyDescriptor>> existing(aCx);
    if (!JS_GetOwnPropertyDescriptorById(aCx, receiverObj, aId, &existing)) {
      return false;
    }
    JS::Rooted<JS::PropertyDescriptor> desc(aCx);
    if (existing.isSome()) {
      if (existing->isAccessorDescriptor()) {
        return aResult.fail(JSMSG_OVERWRITING_ACCESSOR);
      }
      if (!existing->writable()) {
        return aResult.failReadOnly();
      }
      desc = JS::PropertyDescriptor::Empty();
      desc.setValue(aV);
    } else {
      desc = JS::PropertyDescriptor::Data(
          aV, {JS::PropertyAttribute::Configurable,
               JS::PropertyAttribute::Enumerable,
               JS::PropertyAttribute::Writable});
    }
    return JS_DefinePropertyById(aCx, receiverObj, aId, desc, aResult);
  }
  JS::RootedObject setter(aCx);
  if (ownDesc.hasSetter()) {
    setter = ownDesc.setter();
  }
  if (!setter) {
    return aResult.fail(JSMSG_GETTER_ONLY);
  }
  JS::RootedValue setterValue(aCx, JS::ObjectValue(*setter));
  JS::RootedValue ignored(aCx);
  if (!JS::Call(aCx, aReceiver, setterValue, JS::HandleValueArray(aV),
                &ignored)) {
    return false;
  }
  return aResult.succeed();
}

bool js::StringIsArrayIndex(const char16_t* aStr, uint32_t aLength,
                            uint32_t* aIndexp) {
  if (aLength == 0 || aLength > 10) {
    return false;
  }
  if (!mozilla::IsAsciiDigit(aStr[0])) {
    return false;
  }
  if (aStr[0] == '0' && aLength > 1) {
    return false;
  }
  uint64_t index = 0;
  for (uint32_t i = 0; i < aLength; i++) {
    if (!mozilla::IsAsciiDigit(aStr[i])) {
      return false;
    }
    index = index * 10 + (aStr[i] - '0');
  }
  if (index > uint64_t(UINT32_MAX) - 1) {
    return false;
  }
  *aIndexp = uint32_t(index);
  return true;
}

bool js::StringIsArrayIndex(const JSLinearString* aStr, uint32_t* aIndexp) {
  StringCell* cell =
      StringCell::from(reinterpret_cast<const JSString*>(aStr));
  uint32_t length = cell->length();
  if (cell->latin1()) {
    std::u16string chars(length, u'\0');
    const uint8_t* src = static_cast<const uint8_t*>(cell->chars);
    for (uint32_t i = 0; i < length; i++) {
      chars[i] = src[i];
    }
    return js::StringIsArrayIndex(chars.data(), length, aIndexp);
  }
  return js::StringIsArrayIndex(static_cast<const char16_t*>(cell->chars),
                                length, aIndexp);
}

uint64_t js::GetGCHeapUsage(JSContext* aCx) {
  v8::HeapStatistics stats;
  aCx->isolate->GetHeapStatistics(&stats);
  return stats.used_heap_size();
}

void JS::SetDispatchDOMEventCallback(JSContext* aCx,
                                     JS::DispatchDOMEventCallback aCallback) {
  sDispatchDOMEventCallback = aCallback;
}

void JS::SetXrayJitInfo(JS::XrayJitInfo* aInfo) { sXrayJitInfo = aInfo; }

#define VGE_TYPED_ARRAY_GETDATA(T)                                         \
  template <>                                                              \
  mozilla::Span<JS::TypedArray<JS::Scalar::T>::DataType>                   \
  JS::TypedArray<JS::Scalar::T>::getData(bool* aIsSharedMemory,            \
                                         const JS::AutoRequireNoGC&) {     \
    auto bytes = ViewBytes(obj, aIsSharedMemory);                          \
    return {reinterpret_cast<DataType*>(bytes.first),                      \
            bytes.second / sizeof(DataType)};                              \
  }

VGE_TYPED_ARRAY_GETDATA(Int8)
VGE_TYPED_ARRAY_GETDATA(Uint8)
VGE_TYPED_ARRAY_GETDATA(Int16)
VGE_TYPED_ARRAY_GETDATA(Uint16)
VGE_TYPED_ARRAY_GETDATA(Int32)
VGE_TYPED_ARRAY_GETDATA(Uint32)
VGE_TYPED_ARRAY_GETDATA(Float32)
VGE_TYPED_ARRAY_GETDATA(Float64)
VGE_TYPED_ARRAY_GETDATA(Uint8Clamped)
VGE_TYPED_ARRAY_GETDATA(BigInt64)
VGE_TYPED_ARRAY_GETDATA(BigUint64)
