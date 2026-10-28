#include "VgeJsScripts.h"

#include <windows.h>

#include <stdio.h>

#include "js/BuildId.h"
#include "js/friend/ErrorMessages.h"
#include "js/friend/JSMEnvironment.h"
#include "js/Principals.h"

namespace vge::jsapi {

namespace {

std::mutex gGraveyardLock;
std::vector<std::function<void()>> gGraveyard;
std::unordered_map<JSRuntime*, ScriptsRuntime*> gRuntimeStates;
std::unordered_map<JSContext*, ScriptsContext*> gContextStates;
std::unordered_map<int, ScriptData*> gScriptsByV8Id;
std::vector<ScriptData*> gReferrers;
std::vector<v8::Isolate*> gSetUpIsolates;
JS::FilenameValidationCallback gFilenameValidationCallback = nullptr;
JS::BuildIdOp gBuildIdOp = nullptr;

struct JsmFunction {
  JSObject* environment;
  v8::Global<v8::Value> keep;
};
std::unordered_map<int, JsmFunction*> gJsmByScriptId;

}

ScriptsRuntime& RuntimeState(JSRuntime* aRuntime) {
  ScriptsRuntime*& state = gRuntimeStates[aRuntime];
  if (!state) {
    state = new ScriptsRuntime();
  }
  return *state;
}

ScriptsContext& ContextState(JSContext* aCx) {
  ScriptsContext*& state = gContextStates[aCx];
  if (!state) {
    state = new ScriptsContext();
  }
  return *state;
}

void EnsureIsolateSetup(JSContext* aCx) {
  for (v8::Isolate* isolate : gSetUpIsolates) {
    if (isolate == aCx->isolate) {
      return;
    }
  }
  gSetUpIsolates.push_back(aCx->isolate);
  aCx->isolate->SetCaptureStackTraceForUncaughtExceptions(
      true, 128, v8::StackTrace::kDetailed);
  InstallModuleCallbacks(aCx->isolate);
  if (RuntimeState(aCx->runtime).consumeStream) {
    InstallWasmStreaming(aCx);
  }
}

void Bury(std::function<void()> aDeleter) {
  std::lock_guard<std::mutex> lock(gGraveyardLock);
  gGraveyard.push_back(std::move(aDeleter));
}

void DrainGraveyard() {
  std::vector<std::function<void()>> pending;
  {
    std::lock_guard<std::mutex> lock(gGraveyardLock);
    pending.swap(gGraveyard);
  }
  for (auto& deleter : pending) {
    deleter();
  }
}

JSScript* NewScriptCell(JSContext* aCx, ScriptData* aData) {
  ScriptCell* cell = static_cast<ScriptCell*>(AllocCell(sizeof(ScriptCell)));
  cell->data = aData;
  aData->cell = cell->asScript();
  return cell->asScript();
}

ScriptData* ScriptDataOf(const JSScript* aScript) {
  return aScript ? ScriptCell::from(aScript)->data : nullptr;
}

ScriptData* ScriptDataByV8Id(int aScriptId) {
  auto it = gScriptsByV8Id.find(aScriptId);
  return it == gScriptsByV8Id.end() ? nullptr : it->second;
}

void RegisterV8ScriptId(ScriptData* aData, int aScriptId) {
  aData->v8ScriptId = aScriptId;
  if (aScriptId > 0) {
    gScriptsByV8Id[aScriptId] = aData;
  }
}

uint32_t NewReferrerId(ScriptData* aData) {
  gReferrers.push_back(aData);
  aData->referrerId = uint32_t(gReferrers.size());
  return aData->referrerId;
}

ScriptData* ScriptDataByReferrerId(uint32_t aId) {
  if (aId == 0 || aId > gReferrers.size()) {
    return nullptr;
  }
  return gReferrers[aId - 1];
}

JSScript* ScriptForFunction(JSContext* aCx, v8::Local<v8::Function> aFunction) {
  int id = aFunction->ScriptId();
  if (ScriptData* data = ScriptDataByV8Id(id)) {
    if (!data->cell) {
      NewScriptCell(aCx, data);
    }
    return data->cell;
  }
  ScriptData* data = new ScriptData();
  data->realm = aCx->realm();
  data->isFunction = true;
  data->filename =
      Utf8Of(aCx->isolate, aFunction->GetScriptOrigin().ResourceName());
  data->lineno = uint32_t(aFunction->GetScriptLineNumber() + 1);
  data->column = uint32_t(aFunction->GetScriptColumnNumber() + 1);
  data->function.Reset(aCx->isolate, aFunction);
  data->function.SetWeak();
  RegisterV8ScriptId(data, id);
  return NewScriptCell(aCx, data);
}

v8::Local<v8::String> V8String(v8::Isolate* aIsolate, const char* aUtf8) {
  return V8String(aIsolate, aUtf8, aUtf8 ? strlen(aUtf8) : 0);
}

v8::Local<v8::String> V8String(v8::Isolate* aIsolate, const char* aUtf8,
                               size_t aLength) {
  if (!aUtf8) {
    return v8::String::Empty(aIsolate);
  }
  v8::Local<v8::String> out;
  if (!v8::String::NewFromUtf8(aIsolate, aUtf8, v8::NewStringType::kNormal,
                               int(aLength))
           .ToLocal(&out)) {
    return v8::String::Empty(aIsolate);
  }
  return out;
}

v8::Local<v8::String> V8String(v8::Isolate* aIsolate, const char16_t* aChars,
                               size_t aLength) {
  v8::Local<v8::String> out;
  if (!v8::String::NewFromTwoByte(aIsolate,
                                  reinterpret_cast<const uint16_t*>(aChars),
                                  v8::NewStringType::kNormal, int(aLength))
           .ToLocal(&out)) {
    return v8::String::Empty(aIsolate);
  }
  return out;
}

std::string Utf8Of(v8::Isolate* aIsolate, v8::Local<v8::Value> aValue) {
  if (aValue.IsEmpty() || aValue->IsUndefined() || aValue->IsNull()) {
    return std::string();
  }
  v8::String::Utf8Value utf8(aIsolate, aValue);
  return *utf8 ? std::string(*utf8, utf8.length()) : std::string();
}

std::u16string Utf16Of(v8::Isolate* aIsolate, v8::Local<v8::Value> aValue) {
  if (aValue.IsEmpty() || aValue->IsUndefined() || aValue->IsNull()) {
    return std::u16string();
  }
  v8::String::Value chars(aIsolate, aValue);
  return *chars ? std::u16string(reinterpret_cast<const char16_t*>(*chars),
                                 chars.length())
                : std::u16string();
}

std::u16string Utf16FromUtf8(const char* aUtf8, size_t aLength) {
  std::u16string out;
  if (!aLength) {
    return out;
  }
  int count = MultiByteToWideChar(CP_UTF8, 0, aUtf8, int(aLength), nullptr, 0);
  out.resize(size_t(count));
  MultiByteToWideChar(CP_UTF8, 0, aUtf8, int(aLength),
                      reinterpret_cast<wchar_t*>(out.data()), count);
  return out;
}

std::string Utf8FromUtf16(const char16_t* aChars, size_t aLength) {
  std::string out;
  if (!aLength) {
    return out;
  }
  const wchar_t* wide = reinterpret_cast<const wchar_t*>(aChars);
  int count = WideCharToMultiByte(CP_UTF8, 0, wide, int(aLength), nullptr, 0,
                                  nullptr, nullptr);
  out.resize(size_t(count));
  WideCharToMultiByte(CP_UTF8, 0, wide, int(aLength), out.data(), count,
                      nullptr, nullptr);
  return out;
}

JSString* NewJSStringUtf8(JSContext* aCx, const char* aUtf8) {
  v8::HandleScope scope(aCx->isolate);
  return StringFromV8(aCx, V8String(aCx->isolate, aUtf8));
}

JSString* NewJSStringUtf16(JSContext* aCx, const std::u16string& aText) {
  v8::HandleScope scope(aCx->isolate);
  return StringFromV8(aCx, V8String(aCx->isolate, aText.data(), aText.size()));
}

void FillScriptData(JSContext* aCx, ScriptData* aData,
                    const JS::ReadOnlyCompileOptions& aOptions) {
  aData->realm = aCx->realm();
  if (aOptions.filename()) {
    aData->filename = aOptions.filename().c_str();
  }
  if (aOptions.introducerFilename()) {
    aData->introducerFilename = aOptions.introducerFilename().c_str();
  }
  if (aOptions.sourceMapURL()) {
    aData->sourceMapURL = aOptions.sourceMapURL();
  }
  aData->introductionType = aOptions.introductionType;
  aData->lineno = aOptions.lineno;
  aData->column = aOptions.column.oneOriginValue();
  aData->mutedErrors = aOptions.mutedErrors();
  aData->nonSyntacticScope = aOptions.nonSyntacticScope;
  aData->noScriptRval = aOptions.noScriptRval;
  aData->hideFromDebugger =
      JS::InstantiateOptions(aOptions).hideScriptFromDebugger;
}

v8::ScriptOrigin OriginFor(v8::Isolate* aIsolate, const ScriptData* aData,
                           bool aIsModule) {
  v8::Local<v8::PrimitiveArray> hostDefined =
      v8::PrimitiveArray::New(aIsolate, 2);
  hostDefined->Set(aIsolate, 0, v8::Integer::New(aIsolate, aIsModule ? 2 : 1));
  hostDefined->Set(aIsolate, 1,
                   v8::Integer::NewFromUnsigned(aIsolate, aData->referrerId));
  v8::Local<v8::Value> sourceMap;
  if (!aData->sourceMapURL.empty()) {
    sourceMap = V8String(aIsolate, aData->sourceMapURL.data(),
                         aData->sourceMapURL.size());
  }
  return v8::ScriptOrigin(
      aIsolate, V8String(aIsolate, aData->filename.c_str()),
      int(aData->lineno) - 1, int(aData->column) - 1, !aData->mutedErrors, -1,
      sourceMap, false, false, aIsModule, hostDefined);
}

bool ValidateFilename(JSContext* aCx, const char* aFilename,
                      bool aSkipValidation) {
  if (!gFilenameValidationCallback || !aFilename || aSkipValidation) {
    return true;
  }
  if (gFilenameValidationCallback(aCx, aFilename)) {
    return true;
  }
  const char* name = aFilename;
  if (!mozilla::IsUtf8(mozilla::MakeStringSpan(aFilename))) {
    name = "(invalid UTF-8 filename)";
  }
  ThrowNumberedError(aCx, js::GetErrorMessage, nullptr, JSMSG_UNSAFE_FILENAME,
                     name);
  return false;
}

JSScript* CompileScriptText(JSContext* aCx,
                            const JS::ReadOnlyCompileOptions& aOptions,
                            std::u16string&& aSource,
                            v8::ScriptCompiler::CachedData* aCache) {
  EnsureIsolateSetup(aCx);
  DrainGraveyard();
  if (!ValidateFilename(aCx, aOptions.filename().c_str(),
                        JS::InstantiateOptions(aOptions).skipFilenameValidation)) {
    delete aCache;
    return nullptr;
  }
  AutoV8 scope(aCx);
  v8::Isolate* isolate = aCx->isolate;
  ScriptData* data = new ScriptData();
  FillScriptData(aCx, data, aOptions);
  data->source = std::move(aSource);
  NewReferrerId(data);
  v8::ScriptCompiler::Source source(
      V8String(isolate, data->source.data(), data->source.size()),
      OriginFor(isolate, data, false), aCache);
  v8::TryCatch tryCatch(isolate);
  v8::Local<v8::UnboundScript> unbound;
  if (!v8::ScriptCompiler::CompileUnboundScript(
           isolate, &source,
           aCache ? v8::ScriptCompiler::kConsumeCodeCache
                  : v8::ScriptCompiler::kNoCompileOptions)
           .ToLocal(&unbound)) {
    gReferrers[data->referrerId - 1] = nullptr;
    delete data;
    ReportPending(aCx, tryCatch);
    return nullptr;
  }
  data->unbound.Reset(isolate, unbound);
  RegisterV8ScriptId(data, unbound->GetId());
  return NewScriptCell(aCx, data);
}

bool ExecuteScriptCell(JSContext* aCx, JSScript* aScript,
                       const std::vector<JSObject*>& aChain,
                       JSObject* aJsmEnvironment,
                       JS::MutableHandleValue aRval) {
  EnsureIsolateSetup(aCx);
  DrainGraveyard();
  ScriptData* data = ScriptDataOf(aScript);
  AutoV8 scope(aCx);
  v8::Isolate* isolate = aCx->isolate;
  v8::Local<v8::Context> context = scope.context();
  v8::TryCatch tryCatch(isolate);
  aRval.setUndefined();
  if (aChain.empty() && !aJsmEnvironment) {
    v8::Local<v8::Value> result;
    if (!data->unbound.IsEmpty()) {
      v8::Local<v8::Script> script =
          data->unbound.Get(isolate)->BindToCurrentContext();
      if (!script->Run(context).ToLocal(&result)) {
        return ReportPending(aCx, tryCatch);
      }
    } else if (!data->function.IsEmpty()) {
      if (!data->function.Get(isolate)
               ->Call(context, context->Global(), 0, nullptr)
               .ToLocal(&result)) {
        return ReportPending(aCx, tryCatch);
      }
    }
    if (!data->noScriptRval) {
      aRval.set(ValueFromV8(aCx, result));
    }
    return true;
  }
  std::vector<v8::Local<v8::Object>> extensions;
  if (aJsmEnvironment) {
    extensions.push_back(ToV8(aCx, aJsmEnvironment));
  }
  for (size_t i = aChain.size(); i > 0;) {
    extensions.push_back(ToV8(aCx, aChain[--i]));
  }
  v8::ScriptCompiler::Source source(
      V8String(isolate, data->source.data(), data->source.size()),
      OriginFor(isolate, data, false));
  v8::Local<v8::Function> body;
  if (!v8::ScriptCompiler::CompileFunction(context, &source, 0, nullptr,
                                           extensions.size(),
                                           extensions.data())
           .ToLocal(&body)) {
    return ReportPending(aCx, tryCatch);
  }
  RegisterV8ScriptId(data, body->ScriptId());
  ScriptsContext& state = ContextState(aCx);
  if (aJsmEnvironment) {
    JsmFunction*& entry = gJsmByScriptId[body->ScriptId()];
    if (!entry) {
      entry = new JsmFunction();
    }
    entry->environment = aJsmEnvironment;
    entry->keep.Reset(isolate, extensions[0]);
    state.jsmStack.emplace_back(aJsmEnvironment,
                                v8::Global<v8::Value>(isolate, extensions[0]));
  }
  v8::Local<v8::Value> result;
  bool ok =
      body->Call(context, context->Global(), 0, nullptr).ToLocal(&result);
  if (aJsmEnvironment) {
    state.jsmStack.pop_back();
  }
  if (!ok) {
    return ReportPending(aCx, tryCatch);
  }
  return true;
}

JSObject* JsmEnvironmentOfScriptedCaller(JSContext* aCx) {
  v8::HandleScope scope(aCx->isolate);
  v8::Local<v8::StackTrace> trace = v8::StackTrace::CurrentStackTrace(
      aCx->isolate, 64, v8::StackTrace::kScriptId);
  for (int i = 0; i < trace->GetFrameCount(); i++) {
    v8::Local<v8::StackFrame> frame =
        trace->GetFrame(aCx->isolate, uint32_t(i));
    auto it = gJsmByScriptId.find(frame->GetScriptId());
    if (it != gJsmByScriptId.end()) {
      return it->second->environment;
    }
    if (frame->IsUserJavaScript()) {
      break;
    }
  }
  ScriptsContext& state = ContextState(aCx);
  if (trace->GetFrameCount() == 0 && !state.jsmStack.empty()) {
    return state.jsmStack.back().first;
  }
  return nullptr;
}

const JSClass JSMEnvironmentClass = {
    "JSMEnvironment",
    JSCLASS_HAS_RESERVED_SLOTS(0),
};

}

using namespace vge::jsapi;

void JS::TransitiveCompileOptions::copyPODTransitiveOptions(
    const TransitiveCompileOptions& rhs) {
  mutedErrors_ = rhs.mutedErrors_;
  forceStrictMode_ = rhs.forceStrictMode_;
  alwaysUseFdlibm_ = rhs.alwaysUseFdlibm_;
  skipFilenameValidation_ = rhs.skipFilenameValidation_;
  hideScriptFromDebugger_ = rhs.hideScriptFromDebugger_;
  deferDebugMetadata_ = rhs.deferDebugMetadata_;
  eagerDelazificationStrategy_ = rhs.eagerDelazificationStrategy_;
  selfHostingMode = rhs.selfHostingMode;
  discardSource = rhs.discardSource;
  sourceIsLazy = rhs.sourceIsLazy;
  allowHTMLComments = rhs.allowHTMLComments;
  nonSyntacticScope = rhs.nonSyntacticScope;
  allowRedeclaringExistingLexicalBinding =
      rhs.allowRedeclaringExistingLexicalBinding;
  topLevelAwait = rhs.topLevelAwait;
  borrowBuffer = rhs.borrowBuffer;
  usePinnedBytecode = rhs.usePinnedBytecode;
  prefableOptions_ = rhs.prefableOptions_;
  introductionType = rhs.introductionType;
  introductionLineno = rhs.introductionLineno;
  introductionOffset = rhs.introductionOffset;
  hasIntroductionInfo = rhs.hasIntroductionInfo;
}

void JS::ReadOnlyCompileOptions::copyPODNonTransitiveOptions(
    const ReadOnlyCompileOptions& rhs) {
  lineno = rhs.lineno;
  column = rhs.column;
  scriptSourceOffset = rhs.scriptSourceOffset;
  isRunOnce = rhs.isRunOnce;
  noScriptRval = rhs.noScriptRval;
}

namespace {

char* DuplicateUtf8(const char* aText) {
  size_t length = strlen(aText);
  char* copy = static_cast<char*>(js_malloc(length + 1));
  if (copy) {
    memcpy(copy, aText, length + 1);
  }
  return copy;
}

char16_t* DuplicateUtf16(const char16_t* aText) {
  size_t length = std::char_traits<char16_t>::length(aText);
  char16_t* copy =
      static_cast<char16_t*>(js_malloc((length + 1) * sizeof(char16_t)));
  if (copy) {
    memcpy(copy, aText, (length + 1) * sizeof(char16_t));
  }
  return copy;
}

}

JS::OwningCompileOptions::OwningCompileOptions(JSContext* cx) {}

void JS::OwningCompileOptions::release() {
  js_free(const_cast<char*>(filename_.c_str()));
  js_free(const_cast<char16_t*>(sourceMapURL_));
  js_free(const_cast<char*>(introducerFilename_.c_str()));
  filename_ = JS::ConstUTF8CharsZ();
  sourceMapURL_ = nullptr;
  introducerFilename_ = JS::ConstUTF8CharsZ();
}

JS::OwningCompileOptions::~OwningCompileOptions() { release(); }

size_t JS::OwningCompileOptions::sizeOfExcludingThis(
    mozilla::MallocSizeOf mallocSizeOf) const {
  return mallocSizeOf(filename_.c_str()) + mallocSizeOf(sourceMapURL_) +
         mallocSizeOf(introducerFilename_.c_str());
}

void JS::OwningCompileOptions::steal(JS::OwningCompileOptions&& rhs) {
  release();
  copyPODNonTransitiveOptions(rhs);
  copyPODTransitiveOptions(rhs);
  filename_ = rhs.filename_;
  rhs.filename_ = JS::ConstUTF8CharsZ();
  introducerFilename_ = rhs.introducerFilename_;
  rhs.introducerFilename_ = JS::ConstUTF8CharsZ();
  sourceMapURL_ = rhs.sourceMapURL_;
  rhs.sourceMapURL_ = nullptr;
}

void JS::OwningCompileOptions::steal(JS::OwningDecodeOptions&& rhs) {
  release();
  rhs.copyPODOptionsTo(*this);
  introducerFilename_ = rhs.introducerFilename_;
  rhs.introducerFilename_ = JS::ConstUTF8CharsZ();
}

template <typename ContextT>
bool JS::OwningCompileOptions::copyImpl(ContextT* cx,
                                        const ReadOnlyCompileOptions& rhs) {
  release();
  copyPODNonTransitiveOptions(rhs);
  copyPODTransitiveOptions(rhs);
  if (rhs.filename()) {
    const char* str = DuplicateUtf8(rhs.filename().c_str());
    if (!str) {
      return false;
    }
    filename_ = JS::ConstUTF8CharsZ(str);
  }
  if (rhs.sourceMapURL()) {
    sourceMapURL_ = DuplicateUtf16(rhs.sourceMapURL());
    if (!sourceMapURL_) {
      return false;
    }
  }
  if (rhs.introducerFilename()) {
    const char* str = DuplicateUtf8(rhs.introducerFilename().c_str());
    if (!str) {
      return false;
    }
    introducerFilename_ = JS::ConstUTF8CharsZ(str);
  }
  return true;
}

bool JS::OwningCompileOptions::copy(JSContext* cx,
                                    const ReadOnlyCompileOptions& rhs) {
  if (!copyImpl(cx, rhs)) {
    ReportOutOfMemoryWithStack(cx);
    return false;
  }
  return true;
}

bool JS::OwningCompileOptions::copy(JS::FrontendContext* fc,
                                    const ReadOnlyCompileOptions& rhs) {
  return copyImpl(fc, rhs);
}

JS::CompileOptions::CompileOptions(JSContext* cx) {
  prefableOptions_ = ContextState(cx).options.compileOptions();
  if (JS::Realm* realm = cx->realm()) {
    alwaysUseFdlibm_ = realm->creationOptions.alwaysUseFdlibm();
    discardSource = realm->behaviors.discardSource();
  }
  if (ContextState(cx).options.disableFilenameSecurityChecks()) {
    skipFilenameValidation_ = true;
  }
}

JS::InstantiateOptions::InstantiateOptions() {}

JS::CompileOptions& JS::CompileOptions::setIntroductionInfoToCaller(
    JSContext* cx, const char* introductionType,
    JS::MutableHandle<JSScript*> introductionScript) {
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::StackTrace> trace =
      v8::StackTrace::CurrentStackTrace(cx->isolate, 16);
  for (int i = 0; i < trace->GetFrameCount(); i++) {
    v8::Local<v8::StackFrame> frame =
        trace->GetFrame(cx->isolate, uint32_t(i));
    if (!frame->IsUserJavaScript()) {
      continue;
    }
    ScriptData* data = ScriptDataByV8Id(frame->GetScriptId());
    if (!data) {
      return setIntroductionType(introductionType);
    }
    if (!data->cell) {
      NewScriptCell(cx, data);
    }
    introductionScript.set(data->cell);
    return setIntroductionInfo(data->filename.c_str(), introductionType,
                               uint32_t(frame->GetLineNumber()), 0);
  }
  return setIntroductionType(introductionType);
}

void JS::CompileOptions::warnAboutConflictingDelazification() const {
  fprintf(stderr,
          "WARNING: Parsing Everything Eagerly is already set "
          "and it cannot be disabled by other parsing strategy.\n"
          "The provided strategy is going to be ignored.\n\n"
          "Parse Everything Eagerly might be used by Code Coverage "
          "to ensure that all functions are known when the report is "
          "generated.\n");
}

JS::OwningDecodeOptions::~OwningDecodeOptions() { release(); }

void JS::OwningDecodeOptions::release() {
  js_free(const_cast<char*>(introducerFilename_.c_str()));
  introducerFilename_ = JS::ConstUTF8CharsZ();
}

bool JS::OwningDecodeOptions::copy(JS::FrontendContext* maybeFc,
                                   const JS::ReadOnlyDecodeOptions& rhs) {
  copyPODOptionsFrom(rhs);
  if (rhs.introducerFilename()) {
    const char* str = DuplicateUtf8(rhs.introducerFilename().c_str());
    if (!str) {
      return false;
    }
    introducerFilename_ = JS::ConstUTF8CharsZ(str);
  }
  return true;
}

void JS::OwningDecodeOptions::infallibleCopy(
    const JS::ReadOnlyDecodeOptions& rhs) {
  copyPODOptionsFrom(rhs);
}

size_t JS::OwningDecodeOptions::sizeOfExcludingThis(
    mozilla::MallocSizeOf mallocSizeOf) const {
  return mallocSizeOf(introducerFilename_.c_str());
}

JS::ContextOptions& JS::ContextOptions::setWasmDisablesDenormals() {
  wasmDisablesDenormals_ = true;
  return *this;
}

JS::ContextOptions& JS::ContextOptionsRef(JSContext* cx) {
  return ContextState(cx).options;
}

void JS::SetFilenameValidationCallback(JS::FilenameValidationCallback cb) {
  gFilenameValidationCallback = cb;
}

namespace {

std::u16string SourceUnits(JS::SourceText<char16_t>& aSrcBuf) {
  return std::u16string(aSrcBuf.get(), aSrcBuf.length());
}

std::u16string SourceUnits(JS::SourceText<mozilla::Utf8Unit>& aSrcBuf) {
  return Utf16FromUtf8(aSrcBuf.get(), aSrcBuf.length());
}

bool ReadFileUtf8(FILE* aFile, std::string* aOut) {
  char buffer[16384];
  size_t count;
  while ((count = fread(buffer, 1, sizeof(buffer), aFile)) > 0) {
    aOut->append(buffer, count);
  }
  return !ferror(aFile);
}

template <typename Unit>
bool EvaluateSource(JSContext* cx, const JS::ReadOnlyCompileOptions& options,
                    JS::SourceText<Unit>& srcBuf, JS::MutableHandleValue rval) {
  EnsureIsolateSetup(cx);
  DrainGraveyard();
  if (!ValidateFilename(cx, options.filename().c_str(),
                        JS::InstantiateOptions(options).skipFilenameValidation)) {
    return false;
  }
  AutoV8 scope(cx);
  v8::Isolate* isolate = cx->isolate;
  ScriptData data;
  FillScriptData(cx, &data, options);
  std::u16string text = SourceUnits(srcBuf);
  v8::ScriptCompiler::Source source(V8String(isolate, text.data(), text.size()),
                                    OriginFor(isolate, &data, false));
  v8::TryCatch tryCatch(isolate);
  v8::Local<v8::Script> script;
  if (!v8::ScriptCompiler::Compile(scope.context(), &source).ToLocal(&script)) {
    return ReportPending(cx, tryCatch);
  }
  v8::Local<v8::Value> result;
  if (!script->Run(scope.context()).ToLocal(&result)) {
    return ReportPending(cx, tryCatch);
  }
  rval.set(options.noScriptRval ? JS::UndefinedValue()
                                : ValueFromV8(cx, result));
  return true;
}

template <typename Unit>
JSFunction* CompileFunctionSource(JSContext* cx,
                                  const JS::EnvironmentChain& envChain,
                                  const JS::ReadOnlyCompileOptions& options,
                                  const char* name, unsigned nargs,
                                  const char* const* argnames,
                                  JS::SourceText<Unit>& srcBuf) {
  EnsureIsolateSetup(cx);
  DrainGraveyard();
  if (!ValidateFilename(cx, options.filename().c_str(),
                        JS::InstantiateOptions(options).skipFilenameValidation)) {
    return nullptr;
  }
  AutoV8 scope(cx);
  v8::Isolate* isolate = cx->isolate;
  ScriptData* data = new ScriptData();
  FillScriptData(cx, data, options);
  data->isFunction = true;
  NewReferrerId(data);
  std::u16string text = SourceUnits(srcBuf);
  v8::ScriptCompiler::Source source(V8String(isolate, text.data(), text.size()),
                                    OriginFor(isolate, data, false));
  std::vector<v8::Local<v8::String>> arguments;
  for (unsigned i = 0; i < nargs; i++) {
    arguments.push_back(V8String(isolate, argnames[i]));
  }
  std::vector<v8::Local<v8::Object>> extensions;
  for (size_t i = envChain.length(); i > 0;) {
    extensions.push_back(ToV8(cx, envChain.chain()[--i]));
  }
  v8::TryCatch tryCatch(isolate);
  v8::Local<v8::Function> function;
  if (!v8::ScriptCompiler::CompileFunction(
           scope.context(), &source, arguments.size(), arguments.data(),
           extensions.size(), extensions.data())
           .ToLocal(&function)) {
    ReportPending(cx, tryCatch);
    return nullptr;
  }
  if (name) {
    function->SetName(V8String(isolate, name));
  }
  data->source = std::move(text);
  data->function.Reset(isolate, function);
  data->function.SetWeak();
  RegisterV8ScriptId(data, function->ScriptId());
  return reinterpret_cast<JSFunction*>(ObjectFromV8(cx, function));
}

std::vector<JSObject*> ChainVector(const JS::EnvironmentChain& envChain) {
  std::vector<JSObject*> out;
  for (size_t i = 0; i < envChain.length(); i++) {
    out.push_back(envChain.chain()[i]);
  }
  return out;
}

}

JSScript* JS::Compile(JSContext* cx, const ReadOnlyCompileOptions& options,
                      SourceText<char16_t>& srcBuf) {
  return CompileScriptText(cx, options, SourceUnits(srcBuf), nullptr);
}

JSScript* JS::Compile(JSContext* cx, const ReadOnlyCompileOptions& options,
                      SourceText<mozilla::Utf8Unit>& srcBuf) {
  return CompileScriptText(cx, options, SourceUnits(srcBuf), nullptr);
}

JSScript* JS::CompileUtf8File(JSContext* cx,
                              const ReadOnlyCompileOptions& options,
                              FILE* file) {
  std::string bytes;
  if (!ReadFileUtf8(file, &bytes)) {
    JS_ReportErrorUTF8(cx, "can't read script file");
    return nullptr;
  }
  return CompileScriptText(cx, options,
                           Utf16FromUtf8(bytes.data(), bytes.size()), nullptr);
}

JSScript* JS::CompileUtf8Path(JSContext* cx,
                              const ReadOnlyCompileOptions& optionsArg,
                              const char* filename) {
  std::u16string widePath = Utf16FromUtf8(filename, strlen(filename));
  FILE* file =
      _wfopen(reinterpret_cast<const wchar_t*>(widePath.c_str()), L"rb");
  if (!file) {
    ThrowNumberedError(cx, js::GetErrorMessage, nullptr, JSMSG_CANT_OPEN,
                       filename, strerror(errno));
    return nullptr;
  }
  JS::CompileOptions options(cx, optionsArg);
  options.setFileAndLine(filename, 1);
  JSScript* script = CompileUtf8File(cx, options, file);
  fclose(file);
  return script;
}

bool JS::Evaluate(JSContext* cx, const ReadOnlyCompileOptions& options,
                  SourceText<char16_t>& srcBuf, MutableHandle<Value> rval) {
  return EvaluateSource(cx, options, srcBuf, rval);
}

bool JS::Evaluate(JSContext* cx, const ReadOnlyCompileOptions& options,
                  SourceText<mozilla::Utf8Unit>& srcBuf,
                  MutableHandle<Value> rval) {
  return EvaluateSource(cx, options, srcBuf, rval);
}

JSFunction* JS::CompileFunction(JSContext* cx,
                                const JS::EnvironmentChain& envChain,
                                const ReadOnlyCompileOptions& options,
                                const char* name, unsigned nargs,
                                const char* const* argnames,
                                SourceText<char16_t>& srcBuf) {
  return CompileFunctionSource(cx, envChain, options, name, nargs, argnames,
                               srcBuf);
}

bool JS_ExecuteScript(JSContext* cx, JS::Handle<JSScript*> script,
                      JS::MutableHandle<JS::Value> rval) {
  return ExecuteScriptCell(cx, script, {}, nullptr, rval);
}

bool JS_ExecuteScript(JSContext* cx, JS::Handle<JSScript*> script) {
  JS::Rooted<JS::Value> rval(cx);
  return ExecuteScriptCell(cx, script, {}, nullptr, &rval);
}

bool JS_ExecuteScript(JSContext* cx, const JS::EnvironmentChain& envChain,
                      JS::Handle<JSScript*> script,
                      JS::MutableHandle<JS::Value> rval) {
  return ExecuteScriptCell(cx, script, ChainVector(envChain), nullptr, rval);
}

bool JS_Utf8BufferIsCompilableUnit(JSContext* cx, JS::Handle<JSObject*> obj,
                                   const char* utf8, size_t length) {
  AutoV8 scope(cx);
  v8::Isolate* isolate = cx->isolate;
  v8::ScriptCompiler::Source source(V8String(isolate, utf8, length));
  v8::TryCatch tryCatch(isolate);
  v8::Local<v8::UnboundScript> unbound;
  if (v8::ScriptCompiler::CompileUnboundScript(isolate, &source)
          .ToLocal(&unbound)) {
    return true;
  }
  if (tryCatch.Message().IsEmpty()) {
    return true;
  }
  std::string message = Utf8Of(isolate, tryCatch.Message()->Get());
  int end = tryCatch.Message()->GetEndPosition();
  bool atEnd = end >= int(Utf16FromUtf8(utf8, length).size());
  return !(atEnd || message.find("end of input") != std::string::npos ||
           message.find("Unterminated") != std::string::npos);
}

bool JS::UpdateDebugMetadata(JSContext* cx, Handle<JSScript*> script,
                             const InstantiateOptions& options,
                             HandleValue privateValue,
                             HandleString elementAttributeName,
                             HandleScript introScript,
                             HandleScript scriptOrModule) {
  ScriptData* data = ScriptDataOf(script);
  if (!ValidateFilename(cx, data->filename.c_str(),
                        options.skipFilenameValidation)) {
    return false;
  }
  data->hideFromDebugger = options.hideScriptFromDebugger;
  data->privateValue = privateValue;
  if (privateValue.isGCThing()) {
    v8::HandleScope scope(cx->isolate);
    data->privateKeep.Reset(cx->isolate, ToV8(cx, privateValue));
  } else {
    data->privateKeep.Reset();
  }
  return true;
}

void JS::SetScriptPrivate(JSScript* script, const JS::Value& value) {
  ScriptData* data = ScriptDataOf(script);
  JSContext* cx = CurrentContext();
  data->privateValue = value;
  if (value.isGCThing() && cx) {
    v8::HandleScope scope(cx->isolate);
    data->privateKeep.Reset(cx->isolate, ToV8(cx, value));
  } else {
    data->privateKeep.Reset();
  }
}

bool JS::ExecuteInJSMEnvironment(JSContext* cx, Handle<JSScript*> script,
                                 Handle<JSObject*> jsmEnv) {
  JS::Rooted<JS::Value> rval(cx);
  return ExecuteScriptCell(cx, script, {}, jsmEnv, &rval);
}

bool JS::ExecuteInJSMEnvironment(JSContext* cx, Handle<JSScript*> script,
                                 Handle<JSObject*> jsmEnv,
                                 const EnvironmentChain& targetObj) {
  JS::Rooted<JS::Value> rval(cx);
  return ExecuteScriptCell(cx, script, ChainVector(targetObj), jsmEnv, &rval);
}

JSObject* JS::GetJSMEnvironmentOfScriptedCaller(JSContext* cx) {
  return JsmEnvironmentOfScriptedCaller(cx);
}

bool JS::IsJSMEnvironment(JSObject* obj) {
  return JS::GetClass(obj) == &JSMEnvironmentClass;
}

JSObject* JS_ExtensibleLexicalEnvironment(JSObject* obj) { return obj; }

void JS::SetProcessBuildIdOp(JS::BuildIdOp buildIdOp) {
  gBuildIdOp = buildIdOp;
}

namespace {

bool AppendHex(JS::BuildIdCharVector* aBuildId, uint32_t aValue) {
  static const char kDigits[] = "0123456789abcdef";
  for (int shift = 28; shift >= 0; shift -= 4) {
    if (!aBuildId->append(kDigits[(aValue >> shift) & 0xf])) {
      return false;
    }
  }
  return true;
}

}

bool JS::GetOptimizedEncodingBuildId(JS::BuildIdCharVector* buildId) {
  if (!gBuildIdOp || !gBuildIdOp(buildId)) {
    return false;
  }
  return buildId->append("(v8:", 4) &&
         AppendHex(buildId, v8::ScriptCompiler::CachedDataVersionTag()) &&
         buildId->append(')');
}

bool JS::GetScriptTranscodingBuildId(JS::BuildIdCharVector* buildId) {
  if (!gBuildIdOp || !gBuildIdOp(buildId)) {
    return false;
  }
  return buildId->append('-') &&
         buildId->append(sizeof(uintptr_t) == 4 ? '4' : '8') &&
         buildId->append('l') && buildId->append('-') &&
         AppendHex(buildId, v8::ScriptCompiler::CachedDataVersionTag());
}

void JS_SetNativeStackQuota(JSContext* cx,
                            JS::NativeStackSize systemCodeStackSize,
                            JS::NativeStackSize trustedScriptStackSize,
                            JS::NativeStackSize untrustedScriptStackSize) {
  if (!trustedScriptStackSize) {
    trustedScriptStackSize = systemCodeStackSize;
  }
  if (!untrustedScriptStackSize) {
    untrustedScriptStackSize = trustedScriptStackSize;
  }
  ULONG_PTR low = 0;
  ULONG_PTR high = 0;
  GetCurrentThreadStackLimits(&low, &high);
  ScriptsContext& state = ContextState(cx);
  state.stackBase = JS::NativeStackBase(high);
  JS::NativeStackSize sizes[JS::StackKindCount] = {
      systemCodeStackSize, trustedScriptStackSize, untrustedScriptStackSize};
  JS::RootingContext* rc = JS::RootingContext::get(cx);
  for (size_t kind = 0; kind < JS::StackKindCount; kind++) {
    if (sizes[kind] == 0) {
      rc->nativeStackLimit[kind] = JS::NativeStackLimitMax;
    } else {
      JS::NativeStackSize size =
          std::min<JS::NativeStackSize>(sizes[kind], high - low);
      rc->nativeStackLimit[kind] =
          JS::GetNativeStackLimit(state.stackBase, size - 1);
    }
  }
  if (untrustedScriptStackSize) {
    cx->isolate->SetStackLimit(
        uintptr_t(rc->nativeStackLimit[JS::StackForUntrustedScript]));
  }
  EnsureIsolateSetup(cx);
}

JS::NativeStackSize JS::ThreadStackQuotaForSize(size_t stackSize) {
  return JS::NativeStackSize(double(stackSize) * 0.9);
}

void js::SetSourceHook(JSContext* cx,
                       mozilla::UniquePtr<js::SourceHook> hook) {
  ContextState(cx).sourceHook = std::move(hook);
}

void JS::detail::ReportSourceTooLong(JSContext* cx) {
  ThrowNumberedError(cx, js::GetErrorMessage, nullptr, JSMSG_SOURCE_TOO_LONG);
}

JS::Realm* vge::jsapi::RealmOfScript(const JSScript* aScript) {
  ScriptData* data = ScriptDataOf(aScript);
  return data ? data->realm : nullptr;
}

void vge::jsapi::ForgetRealmScripts(JS::Realm* aRealm) {
  for (auto& [id, data] : gScriptsByV8Id) {
    if (data && data->realm == aRealm) {
      data->realm = nullptr;
    }
  }
  for (ScriptData* data : gReferrers) {
    if (data && data->realm == aRealm) {
      data->realm = nullptr;
    }
  }
}
