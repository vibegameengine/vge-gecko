#include "VgeJsScripts.h"

#include <windows.h>

#include "js/Transcoding.h"
#include "js/friend/ErrorMessages.h"

namespace js {

class FrontendContext {
 public:
  std::atomic<bool> cancelRequested{false};
  bool allowCancel = false;
  bool cancelled = false;
  bool hadError = false;
  bool outOfMemory = false;
  JSExnType errorType = JSEXN_SYNTAXERR;
  std::string message;
  std::string filename;
  uint32_t line = 0;
  uint32_t column = 1;
  std::u16string linebuf;
  size_t tokenOffset = 0;
  bool muted = false;
  JS::NativeStackSize stackQuota = 0;
};

}

namespace vge::jsapi {

namespace {

struct HelperIsolate {
  v8::Isolate* isolate = nullptr;
  v8::ArrayBuffer::Allocator* allocator = nullptr;
  v8::Global<v8::Context> context;

  HelperIsolate() {
    v8::Isolate::CreateParams params;
    allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
    params.array_buffer_allocator = allocator;
    isolate = v8::Isolate::New(params);
    v8::Isolate::Scope isolateScope(isolate);
    v8::HandleScope scope(isolate);
    context.Reset(isolate, v8::Context::New(isolate));
  }

  ~HelperIsolate() {
    context.Reset();
    isolate->Dispose();
    delete allocator;
  }
};

thread_local std::unique_ptr<HelperIsolate> tHelper;

HelperIsolate& Helper() {
  if (!tHelper) {
    tHelper = std::make_unique<HelperIsolate>();
  }
  return *tHelper;
}

}

void RunOnHelperIsolate(
    const std::function<void(v8::Isolate*, v8::Local<v8::Context>)>& aWork) {
  HelperIsolate& helper = Helper();
  v8::Isolate::Scope isolateScope(helper.isolate);
  v8::HandleScope scope(helper.isolate);
  v8::Local<v8::Context> context = helper.context.Get(helper.isolate);
  v8::Context::Scope contextScope(context);
  aWork(helper.isolate, context);
}

namespace {

void FillStencil(JS::Stencil* aStencil, const JS::ReadOnlyCompileOptions& aOptions,
                 bool aIsModule) {
  aStencil->isModule = aIsModule;
  if (aOptions.filename()) {
    aStencil->filename = aOptions.filename().c_str();
  }
  if (aOptions.introducerFilename()) {
    aStencil->introducerFilename = aOptions.introducerFilename().c_str();
  }
  if (aOptions.sourceMapURL()) {
    aStencil->sourceMapURL = aOptions.sourceMapURL();
  }
  aStencil->introductionType = aOptions.introductionType;
  aStencil->lineno = aOptions.lineno;
  aStencil->column = aOptions.column.oneOriginValue();
  aStencil->mutedErrors = aOptions.mutedErrors();
  aStencil->nonSyntacticScope = aOptions.nonSyntacticScope;
  aStencil->noScriptRval = aOptions.noScriptRval;
  aStencil->hideFromDebugger =
      JS::InstantiateOptions(aOptions).hideScriptFromDebugger;
}

void FillDataFromStencil(JSContext* aCx, ScriptData* aData,
                         const JS::Stencil* aStencil) {
  aData->realm = aCx->realm();
  aData->filename = aStencil->filename;
  aData->introducerFilename = aStencil->introducerFilename;
  aData->introductionType = aStencil->introductionType;
  aData->sourceMapURL = aStencil->sourceMapURL;
  aData->lineno = aStencil->lineno;
  aData->column = aStencil->column;
  aData->mutedErrors = aStencil->mutedErrors;
  aData->nonSyntacticScope = aStencil->nonSyntacticScope;
  aData->noScriptRval = aStencil->noScriptRval;
  aData->hideFromDebugger = aStencil->hideFromDebugger;
  aData->isModule = aStencil->isModule;
  aData->source = aStencil->source;
}

void RecordFrontendError(js::FrontendContext* aFc, v8::Isolate* aIsolate,
                         v8::Local<v8::Context> aContext,
                         v8::TryCatch& aTry) {
  aFc->hadError = true;
  if (aTry.HasTerminated() || aTry.Exception().IsEmpty()) {
    aFc->outOfMemory = true;
    return;
  }
  v8::Local<v8::Value> exception = aTry.Exception();
  aFc->errorType = JSEXN_SYNTAXERR;
  if (exception->IsObject()) {
    v8::Local<v8::Object> error = exception.As<v8::Object>();
    v8::Local<v8::Value> name;
    v8::Local<v8::Value> text;
    if (error->Get(aContext, V8String(aIsolate, "name")).ToLocal(&name)) {
      aFc->errorType = ExnTypeFromName(Utf8Of(aIsolate, name));
    }
    if (error->Get(aContext, V8String(aIsolate, "message")).ToLocal(&text)) {
      aFc->message = Utf8Of(aIsolate, text);
    }
  } else {
    aFc->message = Utf8Of(aIsolate, exception);
  }
  v8::Local<v8::Message> message = aTry.Message();
  if (!message.IsEmpty()) {
    aFc->filename = Utf8Of(aIsolate, message->GetScriptResourceName());
    aFc->line = uint32_t(message->GetLineNumber(aContext).FromMaybe(0));
    int start = message->GetStartColumn(aContext).FromMaybe(0);
    aFc->column = uint32_t(start + 1);
    aFc->tokenOffset = size_t(std::max(start, 0));
    aFc->muted = !message->IsSharedCrossOrigin();
    v8::Local<v8::String> line;
    if (message->GetSourceLine(aContext).ToLocal(&line)) {
      aFc->linebuf = Utf16Of(aIsolate, line);
    }
  }
}

v8::ScriptOrigin StencilOrigin(v8::Isolate* aIsolate, const JS::Stencil* aStencil,
                               uint32_t aReferrerId) {
  ScriptData data;
  data.filename = aStencil->filename;
  data.sourceMapURL = aStencil->sourceMapURL;
  data.lineno = aStencil->lineno;
  data.column = aStencil->column;
  data.mutedErrors = aStencil->mutedErrors;
  data.referrerId = aReferrerId;
  return OriginFor(aIsolate, &data, aStencil->isModule);
}

bool CompileInIsolate(v8::Isolate* aIsolate, v8::Local<v8::Context> aContext,
                      JS::Stencil* aStencil, uint32_t aReferrerId,
                      v8::Local<v8::UnboundScript>* aUnbound,
                      v8::Local<v8::Module>* aModule, bool aMakeCache,
                      v8::TryCatch& aTry) {
  v8::ScriptCompiler::Source source(
      V8String(aIsolate, aStencil->source.data(), aStencil->source.size()),
      StencilOrigin(aIsolate, aStencil, aReferrerId));
  v8::ScriptCompiler::CachedData* cache = nullptr;
  if (aStencil->isModule) {
    if (!v8::ScriptCompiler::CompileModule(aIsolate, &source).ToLocal(aModule)) {
      return false;
    }
    cache = v8::ScriptCompiler::CreateCodeCache(
        (*aModule)->GetUnboundModuleScript());
  } else {
    if (!v8::ScriptCompiler::CompileUnboundScript(aIsolate, &source)
             .ToLocal(aUnbound)) {
      return false;
    }
    if (aMakeCache) {
      cache = v8::ScriptCompiler::CreateCodeCache(*aUnbound);
    }
  }
  if (cache) {
    aStencil->codeCache.assign(cache->data, cache->data + cache->length);
    delete cache;
  }
  return true;
}

template <typename Unit>
std::u16string SourceUnits(JS::SourceText<Unit>& aSrcBuf) {
  if constexpr (std::is_same_v<Unit, char16_t>) {
    return std::u16string(aSrcBuf.get(), aSrcBuf.length());
  } else {
    return Utf16FromUtf8(aSrcBuf.get(), aSrcBuf.length());
  }
}

template <typename Unit>
already_AddRefed<JS::Stencil> CompileOffThread(
    js::FrontendContext* aFc, const JS::ReadOnlyCompileOptions& aOptions,
    JS::SourceText<Unit>& aSrcBuf, bool aIsModule) {
  if (aFc->cancelRequested) {
    aFc->cancelled = true;
    return nullptr;
  }
  RefPtr<JS::Stencil> stencil = already_AddRefed<JS::Stencil>(new JS::Stencil());
  FillStencil(stencil, aOptions, aIsModule);
  stencil->source = SourceUnits(aSrcBuf);
  stencil->sourceLength = aSrcBuf.length();
  HelperIsolate& helper = Helper();
  v8::Isolate::Scope isolateScope(helper.isolate);
  v8::HandleScope scope(helper.isolate);
  v8::Local<v8::Context> context = helper.context.Get(helper.isolate);
  v8::Context::Scope contextScope(context);
  if (aFc->stackQuota) {
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    uintptr_t here = uintptr_t(&low);
    JS::NativeStackSize quota =
        std::min<JS::NativeStackSize>(aFc->stackQuota, here - low);
    helper.isolate->SetStackLimit(here - quota);
  }
  v8::TryCatch tryCatch(helper.isolate);
  v8::Local<v8::UnboundScript> unbound;
  v8::Local<v8::Module> module;
  if (!CompileInIsolate(helper.isolate, context, stencil, 0, &unbound, &module,
                        true, tryCatch)) {
    RecordFrontendError(aFc, helper.isolate, context, tryCatch);
    return nullptr;
  }
  if (aFc->cancelRequested) {
    aFc->cancelled = true;
    return nullptr;
  }
  return stencil.forget();
}

template <typename Unit>
already_AddRefed<JS::Stencil> CompileOnContext(
    JSContext* aCx, const JS::ReadOnlyCompileOptions& aOptions,
    JS::SourceText<Unit>& aSrcBuf, bool aIsModule) {
  EnsureIsolateSetup(aCx);
  DrainGraveyard();
  if (!ValidateFilename(aCx, aOptions.filename().c_str(),
                        JS::InstantiateOptions(aOptions).skipFilenameValidation)) {
    return nullptr;
  }
  RefPtr<JS::Stencil> stencil = already_AddRefed<JS::Stencil>(new JS::Stencil());
  FillStencil(stencil, aOptions, aIsModule);
  stencil->source = SourceUnits(aSrcBuf);
  stencil->sourceLength = aSrcBuf.length();
  AutoV8 scope(aCx);
  ScriptData* data = new ScriptData();
  FillDataFromStencil(aCx, data, stencil);
  data->realm = nullptr;
  NewReferrerId(data);
  v8::TryCatch tryCatch(aCx->isolate);
  v8::Local<v8::UnboundScript> unbound;
  v8::Local<v8::Module> module;
  if (!CompileInIsolate(aCx->isolate, scope.context(), stencil,
                        data->referrerId, &unbound, &module, false, tryCatch)) {
    ReportPending(aCx, tryCatch);
    return nullptr;
  }
  stencil->ownerIsolate = aCx->isolate;
  stencil->mainData = data;
  if (aIsModule) {
    stencil->firstModule.Reset(aCx->isolate, module);
    RegisterV8ScriptId(data, module->ScriptId());
  } else {
    data->unbound.Reset(aCx->isolate, unbound);
    RegisterV8ScriptId(data, unbound->GetId());
  }
  return stencil.forget();
}

bool EnsureCodeCache(JS::Stencil* aStencil) {
  if (!aStencil->codeCache.empty() || aStencil->isModule ||
      !aStencil->mainData || aStencil->mainData->unbound.IsEmpty()) {
    return true;
  }
  JSContext* cx = CurrentContext();
  if (!cx || cx->isolate != aStencil->ownerIsolate) {
    return true;
  }
  v8::HandleScope scope(cx->isolate);
  std::unique_ptr<v8::ScriptCompiler::CachedData> cache(
      v8::ScriptCompiler::CreateCodeCache(
          aStencil->mainData->unbound.Get(cx->isolate)));
  if (cache) {
    aStencil->codeCache.assign(cache->data, cache->data + cache->length);
  }
  return true;
}

constexpr char kStencilMagic[8] = {'V', 'G', 'E', 'S', 'T', 'N', 'C', '1'};

struct Writer {
  JS::TranscodeBuffer& buffer;
  bool ok = true;

  void bytes(const void* aData, size_t aLength) {
    ok = ok && buffer.append(static_cast<const uint8_t*>(aData), aLength);
  }
  void u32(uint32_t aValue) { bytes(&aValue, sizeof(aValue)); }
  void str(const std::string& aValue) {
    u32(uint32_t(aValue.size()));
    bytes(aValue.data(), aValue.size());
  }
  void str16(const std::u16string& aValue) {
    u32(uint32_t(aValue.size()));
    bytes(aValue.data(), aValue.size() * sizeof(char16_t));
  }
};

struct Reader {
  const uint8_t* cursor;
  const uint8_t* end;
  bool ok = true;

  bool bytes(void* aOut, size_t aLength) {
    if (!ok || size_t(end - cursor) < aLength) {
      ok = false;
      return false;
    }
    memcpy(aOut, cursor, aLength);
    cursor += aLength;
    return true;
  }
  uint32_t u32() {
    uint32_t value = 0;
    bytes(&value, sizeof(value));
    return value;
  }
  std::string str() {
    uint32_t length = u32();
    std::string out;
    if (ok && size_t(end - cursor) >= length) {
      out.assign(reinterpret_cast<const char*>(cursor), length);
      cursor += length;
    } else {
      ok = false;
    }
    return out;
  }
  std::u16string str16() {
    uint32_t length = u32();
    std::u16string out;
    if (ok && size_t(end - cursor) / sizeof(char16_t) >= length) {
      out.resize(length);
      memcpy(out.data(), cursor, size_t(length) * sizeof(char16_t));
      cursor += size_t(length) * sizeof(char16_t);
    } else {
      ok = false;
    }
    return out;
  }
};

JS::TranscodeResult Encode(JS::Stencil* aStencil, JS::TranscodeBuffer& aBuffer) {
  EnsureCodeCache(aStencil);
  Writer writer{aBuffer};
  writer.bytes(kStencilMagic, sizeof(kStencilMagic));
  writer.u32(v8::ScriptCompiler::CachedDataVersionTag());
  uint32_t flags = (aStencil->isModule ? 1 : 0) |
                   (aStencil->mutedErrors ? 2 : 0) |
                   (aStencil->nonSyntacticScope ? 4 : 0) |
                   (aStencil->noScriptRval ? 8 : 0) |
                   (aStencil->hideFromDebugger ? 16 : 0);
  writer.u32(flags);
  writer.u32(aStencil->lineno);
  writer.u32(aStencil->column);
  writer.u32(uint32_t(aStencil->sourceLength));
  writer.str(aStencil->filename);
  writer.str16(aStencil->sourceMapURL);
  writer.str16(aStencil->source);
  writer.u32(uint32_t(aStencil->codeCache.size()));
  writer.bytes(aStencil->codeCache.data(), aStencil->codeCache.size());
  return writer.ok ? JS::TranscodeResult::Ok : JS::TranscodeResult::Throw;
}

JS::TranscodeResult Decode(const JS::ReadOnlyDecodeOptions& aOptions,
                           const JS::TranscodeRange& aRange,
                           JS::Stencil** aStencilOut) {
  Reader reader{aRange.begin().get(), aRange.end().get()};
  char magic[sizeof(kStencilMagic)];
  if (!reader.bytes(magic, sizeof(magic)) ||
      memcmp(magic, kStencilMagic, sizeof(magic)) != 0) {
    return JS::TranscodeResult::Failure_BadDecode;
  }
  if (reader.u32() != v8::ScriptCompiler::CachedDataVersionTag()) {
    return JS::TranscodeResult::Failure_BadBuildId;
  }
  RefPtr<JS::Stencil> stencil = already_AddRefed<JS::Stencil>(new JS::Stencil());
  uint32_t flags = reader.u32();
  stencil->isModule = flags & 1;
  stencil->mutedErrors = flags & 2;
  stencil->nonSyntacticScope = flags & 4;
  stencil->noScriptRval = flags & 8;
  stencil->hideFromDebugger = flags & 16;
  stencil->lineno = reader.u32();
  stencil->column = reader.u32();
  stencil->sourceLength = reader.u32();
  stencil->filename = reader.str();
  stencil->sourceMapURL = reader.str16();
  stencil->source = reader.str16();
  uint32_t cacheLength = reader.u32();
  if (reader.ok && size_t(reader.end - reader.cursor) >= cacheLength) {
    stencil->codeCache.assign(reader.cursor, reader.cursor + cacheLength);
    reader.cursor += cacheLength;
  } else {
    reader.ok = false;
  }
  if (!reader.ok) {
    return JS::TranscodeResult::Failure_BadDecode;
  }
  if (aOptions.introducerFilename()) {
    stencil->introducerFilename = aOptions.introducerFilename().c_str();
  }
  stencil->introductionType = aOptions.introductionType;
  *aStencilOut = stencil.forget().take();
  return JS::TranscodeResult::Ok;
}

v8::MaybeLocal<v8::UnboundScript> CompileFromStencilCache(
    JSContext* aCx, JS::Stencil* aStencil, ScriptData* aData) {
  v8::Isolate* isolate = aCx->isolate;
  v8::ScriptCompiler::CachedData* cache = nullptr;
  if (!aStencil->codeCache.empty()) {
    cache = new v8::ScriptCompiler::CachedData(
        aStencil->codeCache.data(), int(aStencil->codeCache.size()),
        v8::ScriptCompiler::CachedData::BufferNotOwned);
  }
  v8::ScriptCompiler::Source source(
      V8String(isolate, aData->source.data(), aData->source.size()),
      OriginFor(isolate, aData, false), cache);
  return v8::ScriptCompiler::CompileUnboundScript(
      isolate, &source,
      cache ? v8::ScriptCompiler::kConsumeCodeCache
            : v8::ScriptCompiler::kNoCompileOptions);
}

v8::MaybeLocal<v8::Module> CompileModuleFromStencilCache(
    JSContext* aCx, JS::Stencil* aStencil, ScriptData* aData) {
  v8::Isolate* isolate = aCx->isolate;
  v8::ScriptCompiler::CachedData* cache = nullptr;
  if (!aStencil->codeCache.empty()) {
    cache = new v8::ScriptCompiler::CachedData(
        aStencil->codeCache.data(), int(aStencil->codeCache.size()),
        v8::ScriptCompiler::CachedData::BufferNotOwned);
  }
  v8::ScriptCompiler::Source source(
      V8String(isolate, aData->source.data(), aData->source.size()),
      OriginFor(isolate, aData, true), cache);
  return v8::ScriptCompiler::CompileModule(
      isolate, &source,
      cache ? v8::ScriptCompiler::kConsumeCodeCache
            : v8::ScriptCompiler::kNoCompileOptions);
}

}

}

using namespace vge::jsapi;

JS::FrontendContext* JS::NewFrontendContext(
    JS::AllowCancellingCompilation allowCancellingCompilation) {
  js::FrontendContext* fc = new js::FrontendContext();
  fc->allowCancel =
      allowCancellingCompilation == JS::AllowCancellingCompilation::Yes;
  return fc;
}

void JS::DestroyFrontendContext(JS::FrontendContext* fc) { delete fc; }

void JS::SetNativeStackQuota(JS::FrontendContext* fc,
                             JS::NativeStackSize stackSize) {
  fc->stackQuota = stackSize;
}

bool JS::HadFrontendErrors(JS::FrontendContext* fc) {
  return fc->hadError || fc->outOfMemory || fc->cancelled;
}

bool JS::ConvertFrontendErrorsToRuntimeErrors(
    JSContext* cx, JS::FrontendContext* fc,
    const JS::ReadOnlyCompileOptions& options) {
  if (fc->outOfMemory) {
    ReportOutOfMemoryWithStack(cx);
    return false;
  }
  if (fc->cancelled) {
    return false;
  }
  if (fc->hadError) {
    ThrowErrorAt(cx, fc->errorType, fc->message, fc->filename, fc->line,
                 fc->column, fc->linebuf, fc->tokenOffset, fc->muted);
  }
  return true;
}

void JS::RequestFrontendCompilationCancellation(JS::FrontendContext* fc) {
  fc->cancelRequested = true;
}

void JS::ClearFrontendErrors(JS::FrontendContext* fc) {
  fc->hadError = false;
  fc->outOfMemory = false;
  fc->cancelled = false;
  fc->message.clear();
  fc->filename.clear();
  fc->linebuf.clear();
  fc->line = 0;
  fc->column = 1;
  fc->tokenOffset = 0;
}

void JS::detail::ReportSourceTooLong(JS::FrontendContext* fc) {
  fc->hadError = true;
  fc->errorType = JSEXN_RANGEERR;
  fc->message = js::GetErrorMessage(nullptr, JSMSG_SOURCE_TOO_LONG)->format;
  fc->filename = "<unknown>";
  fc->line = 0;
  fc->column = 1;
  fc->linebuf.clear();
  fc->tokenOffset = 0;
  fc->muted = false;
}

already_AddRefed<JS::Stencil> JS::CompileGlobalScriptToStencil(
    JS::FrontendContext* fc, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<mozilla::Utf8Unit>& srcBuf) {
  return CompileOffThread(fc, options, srcBuf, false);
}

already_AddRefed<JS::Stencil> JS::CompileGlobalScriptToStencil(
    JS::FrontendContext* fc, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf) {
  return CompileOffThread(fc, options, srcBuf, false);
}

already_AddRefed<JS::Stencil> JS::CompileModuleScriptToStencil(
    JS::FrontendContext* fc, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<mozilla::Utf8Unit>& srcBuf) {
  return CompileOffThread(fc, options, srcBuf, true);
}

already_AddRefed<JS::Stencil> JS::CompileModuleScriptToStencil(
    JS::FrontendContext* fc, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf) {
  return CompileOffThread(fc, options, srcBuf, true);
}

already_AddRefed<JS::Stencil> JS::CompileGlobalScriptToStencil(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    SourceText<mozilla::Utf8Unit>& srcBuf) {
  return CompileOnContext(cx, options, srcBuf, false);
}

already_AddRefed<JS::Stencil> JS::CompileGlobalScriptToStencil(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    SourceText<char16_t>& srcBuf) {
  return CompileOnContext(cx, options, srcBuf, false);
}

already_AddRefed<JS::Stencil> JS::CompileModuleScriptToStencil(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    SourceText<mozilla::Utf8Unit>& srcBuf) {
  return CompileOnContext(cx, options, srcBuf, true);
}

already_AddRefed<JS::Stencil> JS::CompileModuleScriptToStencil(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    SourceText<char16_t>& srcBuf) {
  return CompileOnContext(cx, options, srcBuf, true);
}

bool JS::PrepareForInstantiate(JS::FrontendContext* fc, JS::Stencil& stencil,
                               JS::InstantiationStorage& storage) {
  if (!storage.gcOutput_) {
    storage.gcOutput_ = new js::frontend::PreallocatedCompilationGCOutput();
  }
  storage.gcOutput_->prepared = true;
  return true;
}

JS::InstantiationStorage::~InstantiationStorage() { delete gcOutput_; }

void JS::StencilAddRef(JS::Stencil* stencil) { stencil->refCount++; }

void JS::StencilRelease(JS::Stencil* stencil) {
  if (--stencil->refCount == 0) {
    if (stencil->firstModule.IsEmpty()) {
      delete stencil;
    } else {
      Bury([stencil] { delete stencil; });
    }
  }
}

bool JS::StencilIsBorrowed(JS::Stencil* stencil) { return stencil->borrowed; }

size_t JS::SizeOfStencil(JS::Stencil* stencil,
                         mozilla::MallocSizeOf mallocSizeOf) {
  return mallocSizeOf(stencil) +
         stencil->source.capacity() * sizeof(char16_t) +
         stencil->codeCache.capacity() + stencil->filename.capacity() +
         stencil->introducerFilename.capacity() +
         stencil->sourceMapURL.capacity() * sizeof(char16_t);
}

JSScript* JS::InstantiateGlobalStencil(JSContext* cx,
                                       const InstantiateOptions& options,
                                       Stencil* stencil,
                                       InstantiationStorage* storage) {
  EnsureIsolateSetup(cx);
  DrainGraveyard();
  if (!ValidateFilename(cx, stencil->filename.c_str(),
                        options.skipFilenameValidation)) {
    return nullptr;
  }
  ScriptData* main = stencil->mainData;
  if (main && stencil->ownerIsolate == cx->isolate && !main->unbound.IsEmpty() &&
      (!main->cell || main->realm == cx->realm())) {
    main->realm = cx->realm();
    main->hideFromDebugger = options.hideScriptFromDebugger;
    if (!main->cell) {
      NewScriptCell(cx, main);
    }
    return main->cell;
  }
  AutoV8 scope(cx);
  ScriptData* data = new ScriptData();
  FillDataFromStencil(cx, data, stencil);
  data->hideFromDebugger = options.hideScriptFromDebugger;
  NewReferrerId(data);
  v8::TryCatch tryCatch(cx->isolate);
  v8::Local<v8::UnboundScript> unbound;
  if (!CompileFromStencilCache(cx, stencil, data).ToLocal(&unbound)) {
    ReportPending(cx, tryCatch);
    return nullptr;
  }
  data->unbound.Reset(cx->isolate, unbound);
  RegisterV8ScriptId(data, unbound->GetId());
  return NewScriptCell(cx, data);
}

JSObject* JS::InstantiateModuleStencil(JSContext* cx,
                                       const InstantiateOptions& options,
                                       Stencil* stencil,
                                       InstantiationStorage* storage) {
  EnsureIsolateSetup(cx);
  DrainGraveyard();
  if (!ValidateFilename(cx, stencil->filename.c_str(),
                        options.skipFilenameValidation)) {
    return nullptr;
  }
  AutoV8 scope(cx);
  if (stencil->mainData && stencil->ownerIsolate == cx->isolate &&
      !stencil->firstModule.IsEmpty()) {
    ScriptData* data = stencil->mainData;
    data->realm = cx->realm();
    data->hideFromDebugger = options.hideScriptFromDebugger;
    v8::Local<v8::Module> module = stencil->firstModule.Get(cx->isolate);
    stencil->firstModule.Reset();
    stencil->mainData = nullptr;
    NewScriptCell(cx, data);
    return NewModuleObjectFor(cx, module, data);
  }
  ScriptData* data = new ScriptData();
  FillDataFromStencil(cx, data, stencil);
  data->hideFromDebugger = options.hideScriptFromDebugger;
  NewReferrerId(data);
  v8::TryCatch tryCatch(cx->isolate);
  v8::Local<v8::Module> module;
  if (!CompileModuleFromStencilCache(cx, stencil, data).ToLocal(&module)) {
    ReportPending(cx, tryCatch);
    return nullptr;
  }
  RegisterV8ScriptId(data, module->ScriptId());
  NewScriptCell(cx, data);
  return NewModuleObjectFor(cx, module, data);
}

JS::TranscodeResult JS::EncodeStencil(JSContext* cx, Stencil* stencil,
                                      TranscodeBuffer& buffer) {
  JS::TranscodeResult result = Encode(stencil, buffer);
  if (result == JS::TranscodeResult::Throw) {
    ReportOutOfMemoryWithStack(cx);
  }
  return result;
}

JS::TranscodeResult JS::EncodeStencil(JS::FrontendContext* fc,
                                      Stencil* stencil,
                                      TranscodeBuffer& buffer) {
  JS::TranscodeResult result = Encode(stencil, buffer);
  if (result == JS::TranscodeResult::Throw) {
    fc->outOfMemory = true;
  }
  return result;
}

JS::TranscodeResult JS::DecodeStencil(JSContext* cx,
                                      const ReadOnlyDecodeOptions& options,
                                      const TranscodeRange& range,
                                      Stencil** stencilOut) {
  return Decode(options, range, stencilOut);
}

JS::TranscodeResult JS::DecodeStencil(JS::FrontendContext* fc,
                                      const ReadOnlyDecodeOptions& options,
                                      const TranscodeRange& range,
                                      Stencil** stencilOut) {
  return Decode(options, range, stencilOut);
}

bool JS::StartCollectingDelazifications(JSContext* cx,
                                        JS::Handle<JSScript*> script,
                                        Stencil* stencil,
                                        CollectDelazificationsResult& result) {
  ScriptData* data = ScriptDataOf(script);
  if (data->collectingDelazifications) {
    result = CollectDelazificationsResult::AlreadyStarted;
    return true;
  }
  data->collectingDelazifications = true;
  result = CollectDelazificationsResult::NewlyStarted;
  return true;
}

bool JS::StartCollectingDelazifications(JSContext* cx,
                                        JS::Handle<JSObject*> module,
                                        Stencil* stencil,
                                        CollectDelazificationsResult& result) {
  JS::Rooted<JSScript*> script(cx, JS::GetModuleScript(module));
  if (!script) {
    result = CollectDelazificationsResult::NotSupported;
    return true;
  }
  return StartCollectingDelazifications(cx, script, stencil, result);
}

void JS::AbortCollectingDelazifications(JSScript* script) {
  ScriptDataOf(script)->collectingDelazifications = false;
}

void JS::AbortCollectingDelazifications(JSObject* module) {
  JSContext* cx = CurrentContext();
  JS::Rooted<JSObject*> rooted(cx, module);
  if (JSScript* script = JS::GetModuleScript(rooted)) {
    ScriptDataOf(script)->collectingDelazifications = false;
  }
}

bool JS::IsStencilCacheable(JS::Stencil* stencil) { return stencil->cacheable; }

size_t JS::GetScriptSourceLength(JS::Stencil* stencil) {
  return stencil->sourceLength;
}

bool JS::GetScriptSourceText(JSContext* cx, JS::Stencil* stencil,
                             JS::MutableHandle<JS::Value> result) {
  JSString* text = NewJSStringUtf16(cx, stencil->source);
  if (!text) {
    return false;
  }
  result.setString(text);
  return true;
}
