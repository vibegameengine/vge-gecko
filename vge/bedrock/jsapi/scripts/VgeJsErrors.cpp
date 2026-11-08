#include "VgeJsScripts.h"

#include <stdarg.h>
#include <stdio.h>

#include "js/Printf.h"
#include "js/Warnings.h"
#include "js/Wrapper.h"
#include "js/friend/ErrorMessages.h"
#include "mozilla/TextUtils.h"

namespace vge::jsapi {

namespace {

const JSErrorFormatString gErrorFormatStrings[JSErr_Limit] = {
#define MSG_DEF(name, count, exception, format) \
  {#name, format, count, exception},
#include "js/friend/ErrorNumbers.msg"
#undef MSG_DEF
};

struct ErrorInfo {
  JSErrorReport report;
  JSExnType exnType = JSEXN_ERR;
  bool exnTypeKnown = false;
  std::string filename;
  std::string message;
  std::u16string linebuf;
  v8::Global<v8::Object> weak;
  JSObject* stack = nullptr;
  bool stackKnown = false;
  v8::Global<v8::Value> stackKeep;
};

thread_local bool gInterceptorRunning = false;

v8::Local<v8::Private> ErrorInfoKey(v8::Isolate* aIsolate) {
  return v8::Private::ForApi(
      aIsolate, v8::String::NewFromUtf8Literal(aIsolate, "vge.scripts.error"));
}

void ErrorInfoDied(const v8::WeakCallbackInfo<ErrorInfo>& aInfo) {
  ErrorInfo* info = aInfo.GetParameter();
  info->weak.Reset();
  Bury([info] { delete info; });
}

ErrorInfo* FindErrorInfo(JSContext* aCx, v8::Local<v8::Object> aError) {
  v8::Local<v8::Value> stored;
  if (!aError->GetPrivate(aCx->v8Context(), ErrorInfoKey(aCx->isolate))
           .ToLocal(&stored) ||
      !stored->IsExternal()) {
    return nullptr;
  }
  return static_cast<ErrorInfo*>(stored.As<v8::External>()->Value());
}

void SetReportStrings(ErrorInfo* aInfo) {
  aInfo->report.filename = JS::ConstUTF8CharsZ(aInfo->filename.c_str());
  if (!aInfo->message.empty() || !aInfo->report.message()) {
    aInfo->report.initBorrowedMessage(aInfo->message.c_str());
  }
  if (!aInfo->linebuf.empty()) {
    size_t token = std::min(aInfo->report.tokenOffset(), aInfo->linebuf.size());
    aInfo->report.initBorrowedLinebuf(aInfo->linebuf.c_str(),
                                      aInfo->linebuf.size(), token);
  }
}

ErrorInfo* AttachErrorInfo(JSContext* aCx, v8::Local<v8::Object> aError,
                           ErrorInfo* aInfo) {
  aInfo->weak.Reset(aCx->isolate, aError);
  aInfo->weak.SetWeak(aInfo, ErrorInfoDied, v8::WeakCallbackType::kParameter);
  aError
      ->SetPrivate(aCx->v8Context(), ErrorInfoKey(aCx->isolate),
                   v8::External::New(aCx->isolate, aInfo))
      .Check();
  return aInfo;
}

bool GetOwnOrInheritedData(JSContext* aCx, v8::Local<v8::Object> aObj,
                           const char* aName, bool aSideEffects,
                           v8::Local<v8::Value>* aOut) {
  v8::Local<v8::Context> context = aCx->v8Context();
  v8::Local<v8::String> key = V8String(aCx->isolate, aName);
  v8::TryCatch tryCatch(aCx->isolate);
  v8::Local<v8::Value> current = aObj;
  for (int depth = 0; depth < 64 && current->IsObject(); depth++) {
    v8::Local<v8::Object> object = current.As<v8::Object>();
    if (object->IsProxy()) {
      break;
    }
    v8::Local<v8::Value> descriptor;
    if (!object->GetOwnPropertyDescriptor(context, key).ToLocal(&descriptor)) {
      return false;
    }
    if (descriptor->IsObject()) {
      v8::Local<v8::Object> desc = descriptor.As<v8::Object>();
      v8::Local<v8::String> valueKey = v8::String::NewFromUtf8Literal(aCx->isolate, "value");
      bool hasValue = false;
      if (desc->HasOwnProperty(context, valueKey).To(&hasValue) && hasValue) {
        return desc->Get(context, valueKey).ToLocal(aOut);
      }
      break;
    }
    current = object->GetPrototype();
  }
  if (!aSideEffects) {
    return false;
  }
  if (!aObj->Get(context, key).ToLocal(aOut)) {
    return false;
  }
  return true;
}

}

JSExnType ExnTypeFromName(const std::string& aName) {
  static const struct {
    const char* name;
    JSExnType type;
  } kNames[] = {{"InternalError", JSEXN_INTERNALERR},
                {"AggregateError", JSEXN_AGGREGATEERR},
                {"EvalError", JSEXN_EVALERR},
                {"RangeError", JSEXN_RANGEERR},
                {"ReferenceError", JSEXN_REFERENCEERR},
                {"SuppressedError", JSEXN_SUPPRESSEDERR},
                {"SyntaxError", JSEXN_SYNTAXERR},
                {"TypeError", JSEXN_TYPEERR},
                {"URIError", JSEXN_URIERR},
                {"DebuggeeWouldRun", JSEXN_DEBUGGEEWOULDRUN},
                {"CompileError", JSEXN_WASMCOMPILEERROR},
                {"LinkError", JSEXN_WASMLINKERROR},
                {"RuntimeError", JSEXN_WASMRUNTIMEERROR}};
  for (auto& entry : kNames) {
    if (aName == entry.name) {
      return entry.type;
    }
  }
  return JSEXN_ERR;
}

namespace {

const char* NameOfExnType(JSExnType aType) {
  switch (aType) {
    case JSEXN_INTERNALERR:
      return "InternalError";
    case JSEXN_AGGREGATEERR:
      return "AggregateError";
    case JSEXN_EVALERR:
      return "EvalError";
    case JSEXN_RANGEERR:
      return "RangeError";
    case JSEXN_REFERENCEERR:
      return "ReferenceError";
    case JSEXN_SUPPRESSEDERR:
      return "SuppressedError";
    case JSEXN_SYNTAXERR:
      return "SyntaxError";
    case JSEXN_TYPEERR:
      return "TypeError";
    case JSEXN_URIERR:
      return "URIError";
    case JSEXN_DEBUGGEEWOULDRUN:
      return "DebuggeeWouldRun";
    case JSEXN_WASMCOMPILEERROR:
      return "CompileError";
    case JSEXN_WASMLINKERROR:
      return "LinkError";
    case JSEXN_WASMRUNTIMEERROR:
      return "RuntimeError";
    default:
      return "Error";
  }
}

ErrorInfo* ErrorInfoFor(JSContext* aCx, v8::Local<v8::Object> aError) {
  if (ErrorInfo* info = FindErrorInfo(aCx, aError)) {
    return info;
  }
  if (!aError->IsNativeError()) {
    return nullptr;
  }
  ErrorInfo* info = new ErrorInfo();
  v8::Local<v8::Message> message =
      v8::Exception::CreateMessage(aCx->isolate, aError);
  ErrorReportFromMessage(aCx, message, &info->report, &info->filename, nullptr,
                         &info->linebuf);
  v8::Local<v8::Value> text;
  if (GetOwnOrInheritedData(aCx, aError, "message", false, &text) &&
      text->IsString()) {
    info->message = Utf8Of(aCx->isolate, text);
  }
  info->exnType = ExnTypeOfError(aCx, aError);
  info->exnTypeKnown = true;
  info->report.exnType = int16_t(info->exnType);
  SetReportStrings(info);
  return AttachErrorInfo(aCx, aError, info);
}

void RunInterceptor(JSContext* aCx, JS::HandleValue aValue) {
  JSErrorInterceptor* interceptor = RuntimeState(aCx->runtime).errorInterceptor;
  if (!interceptor || gInterceptorRunning) {
    return;
  }
  gInterceptorRunning = true;
  interceptor->interceptError(aCx, aValue);
  gInterceptorRunning = false;
}

void PopulateReportBlame(JSContext* aCx, JSErrorReport* aReport,
                         std::string* aFilename) {
  v8::HandleScope scope(aCx->isolate);
  v8::Local<v8::StackTrace> trace =
      v8::StackTrace::CurrentStackTrace(aCx->isolate, 8);
  for (int i = 0; i < trace->GetFrameCount(); i++) {
    v8::Local<v8::StackFrame> frame = trace->GetFrame(aCx->isolate, uint32_t(i));
    if (!frame->IsUserJavaScript()) {
      continue;
    }
    *aFilename = Utf8Of(aCx->isolate, frame->GetScriptNameOrSourceURL());
    aReport->filename = JS::ConstUTF8CharsZ(aFilename->c_str());
    aReport->sourceId = uint32_t(frame->GetScriptId());
    aReport->lineno = uint32_t(frame->GetLineNumber());
    aReport->column = JS::ColumnNumberOneOrigin(uint32_t(frame->GetColumn()));
    if (ScriptData* data = ScriptDataByV8Id(frame->GetScriptId())) {
      aReport->isMuted = data->mutedErrors;
    }
    return;
  }
}

template <typename T>
bool ExpandErrorArguments(JSErrorCallback aCallback, void* aUserRef,
                          unsigned aErrorNumber, const void* aMessageArgs,
                          js::ErrorArgumentsType aArgumentsType, T* aReport,
                          va_list aAp) {
  if (!aCallback) {
    aCallback = js::GetErrorMessage;
  }
  const JSErrorFormatString* efs = aCallback(aUserRef, aErrorNumber);
  if (efs) {
    if constexpr (std::is_same_v<T, JSErrorReport>) {
      aReport->exnType = efs->exnType;
    }
    aReport->errorMessageName = efs->name;
    uint16_t argCount = efs->argCount;
    MOZ_RELEASE_ASSERT(argCount <= JS::MaxNumErrorArguments);
    if (argCount > 0 && efs->format) {
      std::string args[JS::MaxNumErrorArguments];
      for (uint16_t i = 0; i < argCount; i++) {
        switch (aArgumentsType) {
          case js::ArgumentsAreASCII:
          case js::ArgumentsAreUTF8: {
            const char* c =
                aMessageArgs ? static_cast<const char* const*>(aMessageArgs)[i]
                             : va_arg(aAp, const char*);
            args[i] = c ? c : "";
            break;
          }
          case js::ArgumentsAreLatin1: {
            const unsigned char* latin1 = va_arg(aAp, const unsigned char*);
            std::u16string wide;
            for (const unsigned char* p = latin1; p && *p; p++) {
              wide.push_back(char16_t(*p));
            }
            args[i] = Utf8FromUtf16(wide.data(), wide.size());
            break;
          }
          case js::ArgumentsAreUnicode: {
            const char16_t* uc =
                aMessageArgs
                    ? static_cast<const char16_t* const*>(aMessageArgs)[i]
                    : va_arg(aAp, const char16_t*);
            size_t length = uc ? std::char_traits<char16_t>::length(uc) : 0;
            args[i] = Utf8FromUtf16(uc, length);
            break;
          }
        }
      }
      std::string out;
      for (const char* fmt = efs->format; *fmt;) {
        if (*fmt == '{' && mozilla::IsAsciiDigit(fmt[1])) {
          int d = fmt[1] - '0';
          MOZ_RELEASE_ASSERT(d < argCount);
          out += args[d];
          fmt += 3;
          continue;
        }
        out.push_back(*fmt++);
      }
      char* owned = static_cast<char*>(js_malloc(out.size() + 1));
      if (!owned) {
        return false;
      }
      memcpy(owned, out.c_str(), out.size() + 1);
      aReport->initOwnedMessage(owned);
    } else if (efs->format) {
      aReport->initBorrowedMessage(efs->format);
    }
  }
  if (!aReport->message()) {
    char buffer[96];
    snprintf(buffer, sizeof(buffer),
             "No error message available for error number %u", aErrorNumber);
    char* owned = static_cast<char*>(js_malloc(strlen(buffer) + 1));
    if (!owned) {
      return false;
    }
    memcpy(owned, buffer, strlen(buffer) + 1);
    aReport->initOwnedMessage(owned);
  }
  return true;
}

bool ThrowReport(JSContext* aCx, JSErrorReport* aReport, JSExnType exnType) {
  ScriptsContext& state = ContextState(aCx);
  if (state.generatingError) {
    return false;
  }
  state.generatingError = true;
  AutoV8 scope(aCx);
  v8::Local<v8::String> message = V8String(
      aCx->isolate, aReport->message() ? aReport->message().c_str() : "");
  v8::Local<v8::Value> error = NewErrorValue(aCx, exnType, message);
  ErrorInfo* info = new ErrorInfo();
  info->exnType = exnType;
  info->exnTypeKnown = true;
  info->filename = aReport->filename ? aReport->filename.c_str() : "";
  info->message = aReport->message() ? aReport->message().c_str() : "";
  if (aReport->linebuf()) {
    info->linebuf.assign(aReport->linebuf(), aReport->linebufLength());
  }
  info->report.sourceId = aReport->sourceId;
  info->report.lineno = aReport->lineno;
  info->report.column = aReport->column;
  info->report.errorNumber = aReport->errorNumber;
  info->report.errorMessageName = aReport->errorMessageName;
  info->report.exnType = int16_t(exnType);
  info->report.isMuted = aReport->isMuted;
  if (aReport->linebuf()) {
    info->report.initBorrowedLinebuf(info->linebuf.c_str(), info->linebuf.size(),
                                     aReport->tokenOffset());
  }
  SetReportStrings(info);
  v8::Local<v8::Object> errorObject = error.As<v8::Object>();
  AttachErrorInfo(aCx, errorObject, info);
  v8::Local<v8::Context> context = scope.context();
  errorObject
      ->Set(context, V8String(aCx->isolate, "fileName"),
            V8String(aCx->isolate, info->filename.c_str()))
      .Check();
  errorObject
      ->Set(context, V8String(aCx->isolate, "lineNumber"),
            v8::Integer::NewFromUnsigned(aCx->isolate, aReport->lineno))
      .Check();
  errorObject
      ->Set(context, V8String(aCx->isolate, "columnNumber"),
            v8::Integer::NewFromUnsigned(aCx->isolate,
                                         aReport->column.oneOriginValue()))
      .Check();
  SetPendingException(aCx, error, v8::Exception::GetStackTrace(error));
  state.generatingError = false;
  return true;
}

bool ErrorToException(JSContext* aCx, JSErrorReport* aReport,
                      JSErrorCallback aCallback, void* aUserRef) {
  if (!aCallback) {
    aCallback = js::GetErrorMessage;
  }
  const JSErrorFormatString* errorString =
      aCallback(aUserRef, aReport->errorNumber);
  JSExnType exnType = errorString ? JSExnType(errorString->exnType) : JSEXN_ERR;
  if (exnType >= JSEXN_ERROR_LIMIT) {
    exnType = JSEXN_ERR;
  }
  return ThrowReport(aCx, aReport, exnType);
}

bool ReportErrorReport(JSContext* aCx, JSErrorReport* aReport,
                       JSErrorCallback aCallback, void* aUserRef) {
  if (aReport->isWarning()) {
    CallWarningReporter(aCx, aReport);
    return true;
  }
  return ErrorToException(aCx, aReport, aCallback, aUserRef);
}

bool ReportErrorNumberVA(JSContext* aCx, bool aWarning,
                         JSErrorCallback aCallback, void* aUserRef,
                         unsigned aErrorNumber,
                         js::ErrorArgumentsType aArgumentsType,
                         const void* aArgs, va_list aAp) {
  JSErrorReport report;
  report.isWarning_ = aWarning;
  report.errorNumber = aErrorNumber;
  std::string filename;
  PopulateReportBlame(aCx, &report, &filename);
  if (!ExpandErrorArguments(aCallback, aUserRef, aErrorNumber, aArgs,
                            aArgumentsType, &report, aAp)) {
    ReportOutOfMemoryWithStack(aCx);
    return false;
  }
  if (!aWarning && aCallback) {
    const JSErrorFormatString* efs = aCallback(aUserRef, aErrorNumber);
    if (efs && efs->exnType == JSEXN_WARN) {
      report.isWarning_ = true;
    }
  }
  if (!ReportErrorReport(aCx, &report, aCallback, aUserRef)) {
    return false;
  }
  return report.isWarning();
}

bool ReportErrorNumberArgs(JSContext* aCx, bool aWarning,
                           JSErrorCallback aCallback, void* aUserRef,
                           unsigned aErrorNumber,
                           js::ErrorArgumentsType aArgumentsType,
                           const void* aArgs, ...) {
  va_list ap;
  va_start(ap, aArgs);
  bool result = ReportErrorNumberVA(aCx, aWarning, aCallback, aUserRef,
                                    aErrorNumber, aArgumentsType, aArgs, ap);
  va_end(ap);
  return result;
}

bool ReportErrorFormatted(JSContext* aCx, bool aWarning, const char* aFormat,
                          js::ErrorArgumentsType aArgumentsType, va_list aAp) {
  JS::UniqueChars message(JS_vsmprintf(aFormat, aAp));
  if (!message) {
    ReportOutOfMemoryWithStack(aCx);
    return false;
  }
  JSErrorReport report;
  report.isWarning_ = aWarning;
  report.errorNumber = JSMSG_USER_DEFINED_ERROR;
  std::string text = message.get();
  if (aArgumentsType == js::ArgumentsAreLatin1) {
    std::u16string wide;
    for (const char* p = message.get(); *p; p++) {
      wide.push_back(char16_t(static_cast<unsigned char>(*p)));
    }
    text = Utf8FromUtf16(wide.data(), wide.size());
  }
  char* owned = static_cast<char*>(js_malloc(text.size() + 1));
  if (!owned) {
    ReportOutOfMemoryWithStack(aCx);
    return false;
  }
  memcpy(owned, text.c_str(), text.size() + 1);
  report.initOwnedMessage(owned);
  std::string filename;
  PopulateReportBlame(aCx, &report, &filename);
  if (!ReportErrorReport(aCx, &report, nullptr, nullptr)) {
    return false;
  }
  return report.isWarning();
}

JSObject* ResolvePendingStack(JSContext* aCx) {
  ScriptsContext& state = ContextState(aCx);
  if (state.pendingStackOwner.asRawBits() ==
      aCx->pendingException.asRawBits()) {
    if (!state.pendingStackKnown) {
      state.pendingStackKnown = true;
      if (!state.pendingTrace.IsEmpty()) {
        v8::HandleScope scope(aCx->isolate);
        JSObject* stack = SavedFrameFromTrace(
            aCx, state.pendingTrace.Get(aCx->isolate), 0, 0, nullptr, nullptr,
            nullptr, false);
        state.pendingStack = stack;
        if (stack) {
          state.pendingStackKeep.Reset(aCx->isolate, ToV8(aCx, stack));
        }
      }
    }
    return state.pendingStack;
  }
  if (aCx->pendingException.isObject()) {
    v8::HandleScope scope(aCx->isolate);
    v8::Local<v8::Object> error = ToV8(aCx, &aCx->pendingException.toObject());
    return StackOfErrorObject(aCx, error);
  }
  return nullptr;
}

}

void CallWarningReporter(JSContext* aCx, JSErrorReport* aReport) {
  if (aCx->warningReporter) {
    aCx->warningReporter(aCx, aReport);
  }
}

bool ErrorReportFromMessage(JSContext* aCx, v8::Local<v8::Message> aMessage,
                            JSErrorReport* aReport, std::string* aFilename,
                            std::string* aMessageText,
                            std::u16string* aLinebuf) {
  if (aMessage.IsEmpty()) {
    return false;
  }
  v8::Local<v8::Context> context = aCx->v8Context();
  *aFilename = Utf8Of(aCx->isolate, aMessage->GetScriptResourceName());
  aReport->filename = JS::ConstUTF8CharsZ(aFilename->c_str());
  aReport->lineno = uint32_t(aMessage->GetLineNumber(context).FromMaybe(0));
  int startColumn = aMessage->GetStartColumn(context).FromMaybe(0);
  aReport->column = JS::ColumnNumberOneOrigin(uint32_t(startColumn + 1));
  aReport->sourceId = uint32_t(aMessage->GetScriptOrigin().ScriptId());
  aReport->isMuted = !aMessage->IsSharedCrossOrigin() || aMessage->IsOpaque();
  if (aMessageText) {
    *aMessageText = Utf8Of(aCx->isolate, aMessage->Get());
  }
  v8::Local<v8::String> line;
  if (aLinebuf && aMessage->GetSourceLine(context).ToLocal(&line)) {
    *aLinebuf = Utf16Of(aCx->isolate, line);
    if (!aLinebuf->empty()) {
      size_t token = std::min<size_t>(size_t(std::max(startColumn, 0)),
                                      aLinebuf->size());
      aReport->initBorrowedLinebuf(aLinebuf->c_str(), aLinebuf->size(), token);
    }
  }
  return true;
}

JSExnType ExnTypeOfError(JSContext* aCx, v8::Local<v8::Object> aError) {
  if (ErrorInfo* info = FindErrorInfo(aCx, aError)) {
    if (info->exnTypeKnown) {
      return info->exnType;
    }
  }
  v8::Isolate* isolate = aCx->isolate;
  v8::Local<v8::Context> context;
  if (!aError->GetCreationContext().ToLocal(&context)) {
    context = aCx->v8Context();
  }
  v8::Context::Scope contextScope(context);
  v8::Local<v8::String> empty = v8::String::Empty(isolate);
  struct Intrinsic {
    JSExnType type;
    v8::Local<v8::Value> prototype;
  } intrinsics[] = {
      {JSEXN_TYPEERR, v8::Exception::TypeError(empty).As<v8::Object>()->GetPrototype()},
      {JSEXN_RANGEERR, v8::Exception::RangeError(empty).As<v8::Object>()->GetPrototype()},
      {JSEXN_REFERENCEERR,
       v8::Exception::ReferenceError(empty).As<v8::Object>()->GetPrototype()},
      {JSEXN_SYNTAXERR, v8::Exception::SyntaxError(empty).As<v8::Object>()->GetPrototype()},
      {JSEXN_WASMCOMPILEERROR,
       v8::Exception::WasmCompileError(empty).As<v8::Object>()->GetPrototype()},
      {JSEXN_WASMLINKERROR,
       v8::Exception::WasmLinkError(empty).As<v8::Object>()->GetPrototype()},
      {JSEXN_WASMRUNTIMEERROR,
       v8::Exception::WasmRuntimeError(empty).As<v8::Object>()->GetPrototype()},
      {JSEXN_ERR, v8::Exception::Error(empty).As<v8::Object>()->GetPrototype()},
  };
  v8::Local<v8::Value> current = aError->GetPrototype();
  for (int depth = 0; depth < 64 && current->IsObject(); depth++) {
    for (auto& intrinsic : intrinsics) {
      if (intrinsic.prototype->StrictEquals(current)) {
        if (intrinsic.type != JSEXN_ERR) {
          return intrinsic.type;
        }
        v8::Local<v8::Value> name;
        if (GetOwnOrInheritedData(aCx, aError, "name", false, &name) &&
            name->IsString()) {
          return ExnTypeFromName(Utf8Of(isolate, name));
        }
        return JSEXN_ERR;
      }
    }
    current = current.As<v8::Object>()->GetPrototype();
  }
  return JSEXN_ERR;
}

v8::Local<v8::Value> NewErrorValue(JSContext* aCx, JSExnType aType,
                                   v8::Local<v8::String> aMessage) {
  v8::Isolate* isolate = aCx->isolate;
  v8::Local<v8::Context> context = aCx->v8Context();
  switch (aType) {
    case JSEXN_TYPEERR:
      return v8::Exception::TypeError(aMessage);
    case JSEXN_RANGEERR:
      return v8::Exception::RangeError(aMessage);
    case JSEXN_REFERENCEERR:
      return v8::Exception::ReferenceError(aMessage);
    case JSEXN_SYNTAXERR:
      return v8::Exception::SyntaxError(aMessage);
    case JSEXN_WASMCOMPILEERROR:
      return v8::Exception::WasmCompileError(aMessage);
    case JSEXN_WASMLINKERROR:
      return v8::Exception::WasmLinkError(aMessage);
    case JSEXN_WASMRUNTIMEERROR:
      return v8::Exception::WasmRuntimeError(aMessage);
    case JSEXN_EVALERR:
    case JSEXN_URIERR:
    case JSEXN_AGGREGATEERR:
    case JSEXN_SUPPRESSEDERR: {
      v8::Local<v8::Value> constructor;
      v8::TryCatch tryCatch(isolate);
      if (context->Global()
              ->Get(context, V8String(isolate, NameOfExnType(aType)))
              .ToLocal(&constructor) &&
          constructor->IsFunction()) {
        v8::Local<v8::Value> args[2];
        int argc = 0;
        if (aType == JSEXN_AGGREGATEERR) {
          args[argc++] = v8::Array::New(isolate);
        }
        if (aType == JSEXN_SUPPRESSEDERR) {
          args[argc++] = v8::Undefined(isolate);
          args[argc++] = v8::Undefined(isolate);
        }
        args[argc++] = aMessage;
        if (aType == JSEXN_SUPPRESSEDERR) {
          argc = 3;
        }
        v8::Local<v8::Object> created;
        if (aType != JSEXN_SUPPRESSEDERR &&
            constructor.As<v8::Function>()
                ->NewInstance(context, argc, args)
                .ToLocal(&created)) {
          return created;
        }
      }
      break;
    }
    default:
      break;
  }
  v8::Local<v8::Value> error = v8::Exception::Error(aMessage);
  if (aType != JSEXN_ERR) {
    error.As<v8::Object>()
        ->DefineOwnProperty(context, V8String(isolate, "name"),
                            V8String(isolate, NameOfExnType(aType)),
                            v8::DontEnum)
        .Check();
  }
  return error;
}

void SetPendingException(JSContext* aCx, v8::Local<v8::Value> aException,
                         v8::Local<v8::StackTrace> aTrace) {
  ScriptsContext& state = ContextState(aCx);
  state.pendingKeep.Reset(aCx->isolate, aException);
  aCx->pendingException = ValueFromV8(aCx, aException);
  aCx->exceptionPending = true;
  aCx->throwingOutOfMemory = false;
  state.pendingStackOwner = aCx->pendingException;
  state.pendingStack = nullptr;
  state.pendingStackKeep.Reset();
  state.pendingTrace.Reset();
  if (!aTrace.IsEmpty()) {
    state.pendingTrace.Reset(aCx->isolate, aTrace);
    state.pendingStackKnown = false;
  } else if (aException->IsObject()) {
    state.pendingStackKnown = true;
    state.pendingStack = StackOfErrorObject(aCx, aException.As<v8::Object>());
    if (state.pendingStack) {
      state.pendingStackKeep.Reset(aCx->isolate, ToV8(aCx, state.pendingStack));
    }
  } else {
    state.pendingStackKnown = true;
  }
  JS::Rooted<JS::Value> value(aCx, aCx->pendingException);
  RunInterceptor(aCx, value);
}

void SetPendingValue(JSContext* aCx, JS::HandleValue aValue, JSObject* aStack,
                     bool aStackKnown) {
  ScriptsContext& state = ContextState(aCx);
  v8::HandleScope scope(aCx->isolate);
  state.pendingKeep.Reset(aCx->isolate, ToV8(aCx, aValue));
  aCx->pendingException = aValue;
  aCx->exceptionPending = true;
  aCx->throwingOutOfMemory = false;
  state.pendingStackOwner = aValue;
  state.pendingTrace.Reset();
  state.pendingStack = aStack;
  state.pendingStackKnown = aStackKnown;
  if (aStack) {
    state.pendingStackKeep.Reset(aCx->isolate, ToV8(aCx, aStack));
  } else {
    state.pendingStackKeep.Reset();
  }
  RunInterceptor(aCx, aValue);
}

void ClearPending(JSContext* aCx) {
  ScriptsContext& state = ContextState(aCx);
  aCx->pendingException = JS::UndefinedValue();
  aCx->exceptionPending = false;
  aCx->throwingOutOfMemory = false;
  state.pendingKeep.Reset();
  state.pendingStackOwner = JS::UndefinedValue();
  state.pendingStack = nullptr;
  state.pendingStackKeep.Reset();
  state.pendingTrace.Reset();
  state.pendingStackKnown = false;
}

bool ThrowErrorUtf8(JSContext* aCx, JSExnType aType, const char* aMessage) {
  AutoV8 scope(aCx);
  v8::Local<v8::Value> error =
      NewErrorValue(aCx, aType, V8String(aCx->isolate, aMessage));
  SetPendingException(aCx, error, v8::Exception::GetStackTrace(error));
  return false;
}

bool ThrowNumberedError(JSContext* aCx, JSErrorCallback aCallback,
                        void* aUserRef, unsigned aErrorNumber, ...) {
  va_list ap;
  va_start(ap, aErrorNumber);
  ReportErrorNumberVA(aCx, false, aCallback, aUserRef, aErrorNumber,
                      js::ArgumentsAreUTF8, nullptr, ap);
  va_end(ap);
  return false;
}

bool ThrowErrorAt(JSContext* aCx, JSExnType aType, const std::string& aMessage,
                  const std::string& aFilename, uint32_t aLine,
                  uint32_t aColumn, const std::u16string& aLinebuf,
                  size_t aTokenOffset, bool aMuted) {
  JSErrorReport report;
  report.filename = JS::ConstUTF8CharsZ(aFilename.c_str());
  report.lineno = aLine;
  report.column = JS::ColumnNumberOneOrigin(std::max<uint32_t>(aColumn, 1));
  report.isMuted = aMuted;
  report.exnType = int16_t(aType);
  report.initBorrowedMessage(aMessage.c_str());
  if (!aLinebuf.empty()) {
    report.initBorrowedLinebuf(aLinebuf.c_str(), aLinebuf.size(),
                               std::min(aTokenOffset, aLinebuf.size()));
  }
  return ThrowReport(aCx, &report, aType);
}

JSObject* StackOfErrorObject(JSContext* aCx, v8::Local<v8::Object> aError) {
  ErrorInfo* info = ErrorInfoFor(aCx, aError);
  if (!info) {
    return nullptr;
  }
  if (!info->stackKnown) {
    info->stackKnown = true;
    v8::Local<v8::StackTrace> trace = v8::Exception::GetStackTrace(aError);
    if (!trace.IsEmpty()) {
      info->stack = SavedFrameFromTrace(aCx, trace, 0, 0, nullptr, nullptr,
                                        nullptr, false);
      if (info->stack) {
        info->stackKeep.Reset(aCx->isolate, ToV8(aCx, info->stack));
      }
    }
  }
  return info->stack;
}

bool ReportPending(JSContext* aCx, v8::TryCatch& aTry) {
  if (!aTry.HasCaught()) {
    return true;
  }
  if (aTry.HasTerminated()) {
    ClearPending(aCx);
    v8::HandleScope scope(aCx->isolate);
    if (v8::StackTrace::CurrentStackTrace(aCx->isolate, 1)->GetFrameCount() ==
        0) {
      aCx->isolate->CancelTerminateExecution();
    }
    return false;
  }
  v8::HandleScope scope(aCx->isolate);
  v8::Local<v8::Value> exception = aTry.Exception();
  v8::Local<v8::Message> message = aTry.Message();
  v8::Local<v8::StackTrace> trace;
  if (exception->IsObject() && exception.As<v8::Object>()->IsNativeError()) {
    trace = v8::Exception::GetStackTrace(exception);
  }
  if (trace.IsEmpty() && !message.IsEmpty()) {
    trace = message->GetStackTrace();
  }
  if (exception->IsObject() && exception.As<v8::Object>()->IsNativeError() &&
      !FindErrorInfo(aCx, exception.As<v8::Object>()) && !message.IsEmpty()) {
    ErrorInfo* info = new ErrorInfo();
    ErrorReportFromMessage(aCx, message, &info->report, &info->filename,
                           nullptr, &info->linebuf);
    v8::Local<v8::Value> text;
    if (GetOwnOrInheritedData(aCx, exception.As<v8::Object>(), "message",
                              false, &text) &&
        text->IsString()) {
      info->message = Utf8Of(aCx->isolate, text);
    }
    info->exnType = ExnTypeOfError(aCx, exception.As<v8::Object>());
    info->exnTypeKnown = true;
    info->report.exnType = int16_t(info->exnType);
    SetReportStrings(info);
    AttachErrorInfo(aCx, exception.As<v8::Object>(), info);
  }
  SetPendingException(aCx, exception, trace);
  return false;
}

}

using namespace vge::jsapi;

const JSErrorFormatString* js::GetErrorMessage(void* userRef,
                                               unsigned errorNumber) {
  if (errorNumber > 0 && errorNumber < JSErr_Limit) {
    return &gErrorFormatStrings[errorNumber];
  }
  return nullptr;
}

void JSErrorBase::freeMessage() {
  if (ownsMessage_) {
    js_free(const_cast<char*>(message_.c_str()));
    ownsMessage_ = false;
  }
  message_ = JS::ConstUTF8CharsZ();
}

JSString* JSErrorBase::newMessageString(JSContext* cx) {
  if (!message_) {
    return NewJSStringUtf8(cx, "");
  }
  return NewJSStringUtf8(cx, message_.c_str());
}

void JSErrorReport::initBorrowedLinebuf(const char16_t* linebufArg,
                                        size_t linebufLengthArg,
                                        size_t tokenOffsetArg) {
  linebuf_ = linebufArg;
  linebufLength_ = linebufLengthArg;
  tokenOffset_ = tokenOffsetArg;
}

void JSErrorReport::freeLinebuf() {
  if (ownsLinebuf_ && linebuf_) {
    js_free(const_cast<char16_t*>(linebuf_));
    ownsLinebuf_ = false;
  }
  linebuf_ = nullptr;
}

size_t JSErrorNotes::length() { return notes_.length(); }

JSErrorNotes::iterator JSErrorNotes::begin() {
  return iterator(notes_.begin());
}

JSErrorNotes::iterator JSErrorNotes::end() { return iterator(notes_.end()); }

void JS_ReportErrorASCII(JSContext* cx, const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  ReportErrorFormatted(cx, false, format, js::ArgumentsAreASCII, ap);
  va_end(ap);
}

void JS_ReportErrorUTF8(JSContext* cx, const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  ReportErrorFormatted(cx, false, format, js::ArgumentsAreUTF8, ap);
  va_end(ap);
}

void JS_ReportErrorNumberASCII(JSContext* cx, JSErrorCallback errorCallback,
                               void* userRef, const unsigned errorNumber, ...) {
  va_list ap;
  va_start(ap, errorNumber);
  ReportErrorNumberVA(cx, false, errorCallback, userRef, errorNumber,
                      js::ArgumentsAreASCII, nullptr, ap);
  va_end(ap);
}

void JS_ReportErrorNumberLatin1(JSContext* cx, JSErrorCallback errorCallback,
                                void* userRef, const unsigned errorNumber,
                                ...) {
  va_list ap;
  va_start(ap, errorNumber);
  ReportErrorNumberVA(cx, false, errorCallback, userRef, errorNumber,
                      js::ArgumentsAreLatin1, nullptr, ap);
  va_end(ap);
}

void JS_ReportErrorNumberUTF8VA(JSContext* cx, JSErrorCallback errorCallback,
                                void* userRef, const unsigned errorNumber,
                                va_list ap) {
  ReportErrorNumberVA(cx, false, errorCallback, userRef, errorNumber,
                      js::ArgumentsAreUTF8, nullptr, ap);
}

void JS_ReportErrorNumberUTF8Array(JSContext* cx, JSErrorCallback errorCallback,
                                   void* userRef, const unsigned errorNumber,
                                   const char** args) {
  ReportErrorNumberArgs(cx, false, errorCallback, userRef, errorNumber,
                        js::ArgumentsAreUTF8, args);
}

void JS_ReportErrorNumberUC(JSContext* cx, JSErrorCallback errorCallback,
                            void* userRef, const unsigned errorNumber, ...) {
  va_list ap;
  va_start(ap, errorNumber);
  ReportErrorNumberVA(cx, false, errorCallback, userRef, errorNumber,
                      js::ArgumentsAreUnicode, nullptr, ap);
  va_end(ap);
}

bool JS::WarnASCII(JSContext* cx, const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  bool ok = ReportErrorFormatted(cx, true, format, js::ArgumentsAreASCII, ap);
  va_end(ap);
  return ok;
}

bool JS::WarnUTF8(JSContext* cx, const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  bool ok = ReportErrorFormatted(cx, true, format, js::ArgumentsAreUTF8, ap);
  va_end(ap);
  return ok;
}

void JS::ReportUncatchableException(JSContext* cx) {
  ClearPending(cx);
  v8::HandleScope scope(cx->isolate);
  if (v8::StackTrace::CurrentStackTrace(cx->isolate, 1)->GetFrameCount() > 0) {
    cx->isolate->TerminateExecution();
  }
}

bool JS_IsExceptionPending(JSContext* cx) { return cx->exceptionPending; }

bool JS_IsThrowingOutOfMemory(JSContext* cx) {
  return cx->exceptionPending && cx->throwingOutOfMemory;
}

bool JS_GetPendingException(JSContext* cx, JS::MutableHandleValue vp) {
  if (!cx->exceptionPending) {
    return false;
  }
  vp.set(cx->pendingException);
  return true;
}

void JS_SetPendingException(JSContext* cx, JS::HandleValue value,
                            JS::ExceptionStackBehavior behavior) {
  if (behavior == JS::ExceptionStackBehavior::Capture) {
    JS::Rooted<JSObject*> stack(cx);
    CaptureStackObject(cx, 0, &stack);
    SetPendingValue(cx, value, stack, true);
  } else {
    SetPendingValue(cx, value, nullptr, true);
  }
}

void JS_ClearPendingException(JSContext* cx) { ClearPending(cx); }

bool JS::GetPendingExceptionStack(JSContext* cx,
                                  JS::ExceptionStack* exceptionStack) {
  if (!cx->exceptionPending) {
    return false;
  }
  JS::Rooted<JS::Value> exception(cx, cx->pendingException);
  JS::Rooted<JSObject*> stack(cx, ResolvePendingStack(cx));
  exceptionStack->init(exception, stack);
  return true;
}

bool JS::StealPendingExceptionStack(JSContext* cx,
                                    JS::ExceptionStack* exceptionStack) {
  if (!GetPendingExceptionStack(cx, exceptionStack)) {
    return false;
  }
  ClearPending(cx);
  return true;
}

void JS::SetPendingExceptionStack(JSContext* cx,
                                  const JS::ExceptionStack& exceptionStack) {
  SetPendingValue(cx, exceptionStack.exception(), exceptionStack.stack(), true);
}

JSObject* JS::ExceptionStackOrNull(JS::HandleObject objArg) {
  JSContext* cx = CurrentContext();
  JSObject* unwrapped = js::UncheckedUnwrap(objArg);
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::Object> object = ToV8(cx, unwrapped);
  if (!object->IsNativeError()) {
    return nullptr;
  }
  return StackOfErrorObject(cx, object);
}

bool JS_ErrorFromException(JSContext* cx, JS::HandleObject obj,
                           JS::BorrowedErrorReport& errorReport) {
  JSObject* unwrapped = js::UncheckedUnwrap(obj);
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::Object> object = ToV8(cx, unwrapped);
  ErrorInfo* info = ErrorInfoFor(cx, object);
  if (!info) {
    return false;
  }
  errorReport.init(unwrapped, &info->report);
  return true;
}

JS::AutoSaveExceptionState::AutoSaveExceptionState(JSContext* cx)
    : context(cx),
      status(cx->exceptionPending
                 ? (cx->throwingOutOfMemory ? JS::ExceptionStatus::OutOfMemory
                                            : JS::ExceptionStatus::Throwing)
                 : JS::ExceptionStatus::None),
      exceptionValue(cx),
      exceptionStack(cx) {
  if (JS::IsCatchableExceptionStatus(status)) {
    exceptionValue = cx->pendingException;
    exceptionStack = ResolvePendingStack(cx);
  }
  ClearPending(cx);
}

void JS::AutoSaveExceptionState::drop() {
  status = JS::ExceptionStatus::None;
  exceptionValue.setUndefined();
  exceptionStack = nullptr;
}

void JS::AutoSaveExceptionState::restore() {
  if (JS::IsCatchableExceptionStatus(status)) {
    SetPendingValue(context, exceptionValue, exceptionStack, true);
    context->throwingOutOfMemory = status == JS::ExceptionStatus::OutOfMemory;
  } else {
    ClearPending(context);
  }
  drop();
}

JS::AutoSaveExceptionState::~AutoSaveExceptionState() {
  if (!context->exceptionPending &&
      JS::IsCatchableExceptionStatus(status)) {
    SetPendingValue(context, exceptionValue, exceptionStack, true);
    context->throwingOutOfMemory = status == JS::ExceptionStatus::OutOfMemory;
  }
}

bool JS::CreateError(JSContext* cx, JSExnType type, HandleObject stack,
                     HandleString fileName, uint32_t lineNumber,
                     JS::ColumnNumberOneOrigin column, JSErrorReport* report,
                     HandleString message,
                     Handle<mozilla::Maybe<Value>> cause,
                     MutableHandleValue rval) {
  AutoV8 scope(cx);
  v8::Isolate* isolate = cx->isolate;
  v8::Local<v8::Context> context = scope.context();
  v8::Local<v8::String> text =
      message ? ToV8(cx, message.get()) : v8::String::Empty(isolate);
  v8::Local<v8::Value> error = NewErrorValue(cx, type, text);
  v8::Local<v8::Object> object = error.As<v8::Object>();
  ErrorInfo* info = new ErrorInfo();
  info->exnType = type;
  info->exnTypeKnown = true;
  info->filename = fileName ? Utf8Of(isolate, ToV8(cx, fileName.get())) : "";
  info->message = Utf8Of(isolate, text);
  if (report) {
    info->report.errorNumber = report->errorNumber;
    info->report.errorMessageName = report->errorMessageName;
    info->report.isMuted = report->isMuted;
    info->report.sourceId = report->sourceId;
    if (report->linebuf()) {
      info->linebuf.assign(report->linebuf(), report->linebufLength());
      info->report.initBorrowedLinebuf(info->linebuf.c_str(),
                                       info->linebuf.size(),
                                       report->tokenOffset());
    }
  }
  info->report.lineno = lineNumber;
  info->report.column = column;
  info->report.exnType = int16_t(type);
  SetReportStrings(info);
  if (stack) {
    info->stack = stack;
    info->stackKnown = true;
    info->stackKeep.Reset(isolate, ToV8(cx, stack.get()));
  }
  AttachErrorInfo(cx, object, info);
  object
      ->Set(context, V8String(isolate, "fileName"),
            V8String(isolate, info->filename.c_str()))
      .Check();
  object
      ->Set(context, V8String(isolate, "lineNumber"),
            v8::Integer::NewFromUnsigned(isolate, lineNumber))
      .Check();
  object
      ->Set(context, V8String(isolate, "columnNumber"),
            v8::Integer::NewFromUnsigned(isolate, column.oneOriginValue()))
      .Check();
  if (cause.get().isSome()) {
    object
        ->DefineOwnProperty(context, V8String(isolate, "cause"),
                            ToV8(cx, *cause.get()), v8::DontEnum)
        .Check();
  }
  rval.set(ValueFromV8(cx, error));
  return true;
}

JS::ErrorReportBuilder::ErrorReportBuilder(JSContext* cx)
    : reportp(nullptr), borrowedReport(cx) {}

JS::ErrorReportBuilder::~ErrorReportBuilder() = default;

namespace {

JSString* FormatErrorMessage(JSContext* aCx, v8::Local<v8::Value> aName,
                             v8::Local<v8::Value> aMessage) {
  v8::Isolate* isolate = aCx->isolate;
  bool hasName = !aName.IsEmpty() && aName->IsString();
  bool hasMessage = !aMessage.IsEmpty() && aMessage->IsString();
  if (hasName && hasMessage) {
    v8::Local<v8::String> joined = v8::String::Concat(
        isolate, aName.As<v8::String>(),
        v8::String::Concat(isolate, v8::String::NewFromUtf8Literal(isolate, ": "),
                           aMessage.As<v8::String>()));
    return StringFromV8(aCx, joined);
  }
  if (hasName) {
    return StringFromV8(aCx, aName.As<v8::String>());
  }
  if (hasMessage) {
    return StringFromV8(aCx, aMessage.As<v8::String>());
  }
  return nullptr;
}

}

bool JS::ErrorReportBuilder::init(JSContext* cx,
                                  const JS::ExceptionStack& exnStack,
                                  SniffingBehavior sniffingBehavior) {
  AutoV8 scope(cx);
  v8::Isolate* isolate = cx->isolate;
  JS::Rooted<JSObject*> exnObject(cx);
  if (exnStack.exception().isObject()) {
    exnObject = &exnStack.exception().toObject();
    if (JS_ErrorFromException(cx, exnObject, borrowedReport)) {
      reportp = borrowedReport.get();
      if (reportp->isMuted) {
        sniffingBehavior = SniffingBehavior::NoSideEffects;
      }
    }
  }
  bool sideEffects = sniffingBehavior == SniffingBehavior::WithSideEffects;
  JS::Rooted<JSString*> str(cx);
  if (reportp) {
    v8::Local<v8::Object> object = ToV8(cx, exnObject.get());
    v8::Local<v8::Value> name;
    v8::Local<v8::Value> message;
    if (!GetOwnOrInheritedData(cx, object, "name", sideEffects, &name) ||
        !name->IsString()) {
      JSExnType type = JSExnType(reportp->exnType);
      if (type != JSEXN_WARN && type != JSEXN_NOTE) {
        name = V8String(isolate, NameOfExnType(type));
      } else {
        name = v8::Local<v8::Value>();
      }
    }
    if (!GetOwnOrInheritedData(cx, object, "message", sideEffects, &message) ||
        !message->IsString()) {
      message = V8String(isolate, reportp->message() ? reportp->message().c_str()
                                                     : "");
    }
    str = FormatErrorMessage(cx, name, message);
  } else if (exnObject &&
             JS::GetClass(exnObject)->isDOMClass()) {
    v8::Local<v8::Object> object = ToV8(cx, exnObject.get());
    v8::TryCatch tryCatch(isolate);
    v8::Local<v8::Value> name;
    v8::Local<v8::Value> message;
    v8::Local<v8::Value> fileNameValue;
    v8::Local<v8::Value> lineValue;
    v8::Local<v8::Value> columnValue;
    v8::Local<v8::Context> context = scope.context();
    if (object->Get(context, V8String(isolate, "name")).ToLocal(&name) &&
        object->Get(context, V8String(isolate, "message")).ToLocal(&message) &&
        message->IsString()) {
      str = FormatErrorMessage(cx, name, message);
      if (!object->Get(context, V8String(isolate, "filename"))
               .ToLocal(&fileNameValue)) {
        fileNameValue = v8::String::Empty(isolate);
      }
      if (!object->Get(context, V8String(isolate, "lineNumber"))
               .ToLocal(&lineValue)) {
        lineValue = v8::Integer::New(isolate, 0);
      }
      if (!object->Get(context, V8String(isolate, "columnNumber"))
               .ToLocal(&columnValue)) {
        columnValue = v8::Integer::New(isolate, 1);
      }
      std::string file = Utf8Of(isolate, fileNameValue);
      filename.reset(static_cast<char*>(js_malloc(file.size() + 1)));
      if (filename) {
        memcpy(filename.get(), file.c_str(), file.size() + 1);
      }
      reportp = &ownedReport;
      new (reportp) JSErrorReport();
      ownedReport.filename = JS::ConstUTF8CharsZ(filename.get());
      ownedReport.lineno =
          lineValue.IsEmpty() ? 0 : lineValue->Uint32Value(context).FromMaybe(0);
      ownedReport.exnType = JSEXN_INTERNALERR;
      ownedReport.column = JS::ColumnNumberOneOrigin(
          columnValue.IsEmpty() ? 1
                                : std::max<uint32_t>(
                                      1, columnValue->Uint32Value(context)
                                             .FromMaybe(1)));
      std::string text = Utf8Of(isolate, ToV8(cx, str.get()));
      char* owned = static_cast<char*>(js_malloc(text.size() + 1));
      if (owned) {
        memcpy(owned, text.c_str(), text.size() + 1);
        ownedReport.initOwnedMessage(owned);
      }
    }
  }
  if (!str) {
    if (exnStack.exception().isSymbol()) {
      v8::Local<v8::Symbol> symbol = ToV8(cx, exnStack.exception().toSymbol());
      v8::Local<v8::Value> description = symbol->Description(isolate);
      std::u16string text = u"Symbol(";
      if (description->IsString()) {
        text += Utf16Of(isolate, description);
      }
      text += u")";
      str = NewJSStringUtf16(cx, text);
    } else if (exnObject && !sideEffects) {
      if (sniffingBehavior == NoSideEffectsListPropertyNames) {
        v8::Local<v8::Object> object = ToV8(cx, exnObject.get());
        std::u16string text = Utf16FromUtf8(JS::GetClass(exnObject)->name,
                                            strlen(JS::GetClass(exnObject)->name));
        v8::Local<v8::Array> names;
        v8::TryCatch tryCatch(isolate);
        if (!object->IsProxy() &&
            object
                ->GetOwnPropertyNames(scope.context(),
                                      v8::PropertyFilter::SKIP_SYMBOLS,
                                      v8::KeyConversionMode::kKeepNumbers)
                .ToLocal(&names)) {
          uint32_t written = 0;
          for (uint32_t i = 0; i < names->Length(); i++) {
            v8::Local<v8::Value> key;
            if (!names->Get(scope.context(), i).ToLocal(&key) || !key->IsString()) {
              continue;
            }
            if (written == 10) {
              text += u", ...";
              break;
            }
            text += written == 0 ? u" { " : u", ";
            text += Utf16Of(isolate, key);
            written++;
          }
          if (written > 0) {
            text += u" }";
          }
        }
        str = NewJSStringUtf16(cx, text);
      } else {
        str = NewJSStringUtf8(cx, "Object");
      }
    } else {
      v8::TryCatch tryCatch(isolate);
      v8::Local<v8::String> converted;
      if (ToV8(cx, exnStack.exception())
              ->ToString(scope.context())
              .ToLocal(&converted)) {
        str = StringFromV8(cx, converted);
      }
    }
  }
  const char* utf8Message = nullptr;
  if (str) {
    std::string text = Utf8Of(isolate, ToV8(cx, str.get()));
    toStringResultBytesStorage.reset(static_cast<char*>(js_malloc(text.size() + 1)));
    if (toStringResultBytesStorage) {
      memcpy(toStringResultBytesStorage.get(), text.c_str(), text.size() + 1);
      utf8Message = toStringResultBytesStorage.get();
    }
  }
  if (!utf8Message) {
    utf8Message = "unknown (can't convert to string)";
  }
  if (!reportp) {
    return populateUncaughtExceptionReportUTF8(cx, exnStack.stack(),
                                               utf8Message);
  }
  toStringResult_ = JS::ConstUTF8CharsZ(utf8Message, strlen(utf8Message));
  return true;
}

bool JS::ErrorReportBuilder::populateUncaughtExceptionReportUTF8(
    JSContext* cx, JS::HandleObject stack, ...) {
  va_list ap;
  va_start(ap, stack);
  bool ok = populateUncaughtExceptionReportUTF8VA(cx, stack, ap);
  va_end(ap);
  return ok;
}

bool JS::ErrorReportBuilder::populateUncaughtExceptionReportUTF8VA(
    JSContext* cx, JS::HandleObject stack, va_list ap) {
  new (&ownedReport) JSErrorReport();
  ownedReport.isWarning_ = false;
  ownedReport.errorNumber = JSMSG_UNCAUGHT_EXCEPTION;
  JS::Rooted<JSObject*> frame(cx);
  if (stack) {
    frame = js::GetFirstSubsumedSavedFrame(
        cx, cx->realm() ? cx->realm()->principals : nullptr, stack,
        JS::SavedFrameSelfHosted::Exclude);
  }
  if (frame) {
    JS::Rooted<JSString*> source(cx);
    uint32_t line = 0;
    uint32_t sourceId = 0;
    JS::TaggedColumnNumberOneOrigin column;
    JS::GetSavedFrameSource(cx, nullptr, frame, &source);
    JS::GetSavedFrameLine(cx, nullptr, frame, &line);
    JS::GetSavedFrameSourceId(cx, nullptr, frame, &sourceId);
    JS::GetSavedFrameColumn(cx, nullptr, frame, &column);
    std::string file =
        source ? Utf8Of(cx->isolate, ToV8(cx, source.get())) : std::string();
    filename.reset(static_cast<char*>(js_malloc(file.size() + 1)));
    if (!filename) {
      return false;
    }
    memcpy(filename.get(), file.c_str(), file.size() + 1);
    ownedReport.filename = JS::ConstUTF8CharsZ(filename.get());
    ownedReport.sourceId = sourceId;
    ownedReport.lineno = line;
    ownedReport.column = JS::ColumnNumberOneOrigin(column.oneOriginValue());
  } else {
    std::string file;
    PopulateReportBlame(cx, &ownedReport, &file);
    if (!file.empty()) {
      filename.reset(static_cast<char*>(js_malloc(file.size() + 1)));
      if (!filename) {
        return false;
      }
      memcpy(filename.get(), file.c_str(), file.size() + 1);
      ownedReport.filename = JS::ConstUTF8CharsZ(filename.get());
    } else {
      ownedReport.filename = JS::ConstUTF8CharsZ();
    }
  }
  if (!ExpandErrorArguments(js::GetErrorMessage, nullptr,
                            JSMSG_UNCAUGHT_EXCEPTION, nullptr,
                            js::ArgumentsAreUTF8, &ownedReport, ap)) {
    return false;
  }
  toStringResult_ = ownedReport.message();
  reportp = &ownedReport;
  return true;
}

void JS_SetErrorInterceptorCallback(JSRuntime* rt,
                                    JSErrorInterceptor* callback) {
  RuntimeState(rt).errorInterceptor = callback;
}
