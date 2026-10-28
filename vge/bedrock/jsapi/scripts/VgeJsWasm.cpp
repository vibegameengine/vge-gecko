#include "VgeJsScripts.h"

#include "js/StreamConsumer.h"
#include "js/WasmModule.h"
#include "js/Wrapper.h"
#include "js/friend/ErrorMessages.h"
#include "v8-wasm.h"

namespace vge::jsapi {

namespace {

struct VgeWasmModule final : JS::WasmModule {
  explicit VgeWasmModule(const v8::CompiledWasmModule& aCompiled)
      : compiled(aCompiled) {}

  JSObject* createObject(JSContext* cx) const override {
    AutoV8 scope(cx);
    v8::TryCatch tryCatch(cx->isolate);
    v8::Local<v8::WasmModuleObject> object;
    if (!v8::WasmModuleObject::FromCompiledModule(cx->isolate, compiled)
             .ToLocal(&object)) {
      ReportPending(cx, tryCatch);
      return nullptr;
    }
    return ObjectFromV8(cx, object);
  }

  v8::CompiledWasmModule compiled;
};

struct VgeWasmCompileArgs final : JS::WasmCompileArgs {
  std::string filename;
};

JS::UniqueChars DuplicateChars(const std::string& aText) {
  char* copy = static_cast<char*>(js_malloc(aText.size() + 1));
  if (copy) {
    memcpy(copy, aText.c_str(), aText.size() + 1);
  }
  return JS::UniqueChars(copy);
}

class VgeStreamConsumer final : public JS::StreamConsumer {
 public:
  VgeStreamConsumer(std::shared_ptr<v8::WasmStreaming> aStreaming,
                    JSContext* aCx)
      : mStreaming(std::move(aStreaming)), mCx(aCx) {}

  bool consumeChunk(const uint8_t* aBegin, size_t aLength) override {
    std::lock_guard<std::mutex> lock(mLock);
    if (mDone) {
      return false;
    }
    mStreaming->OnBytesReceived(aBegin, aLength);
    return true;
  }

  void streamEnd(JS::OptimizedEncodingListener* aListener) override {
    {
      std::lock_guard<std::mutex> lock(mLock);
      if (mDone) {
        return;
      }
      mDone = true;
      if (aListener) {
        aListener->AddRef();
        std::shared_ptr<JS::OptimizedEncodingListener> listener(
            aListener,
            [](JS::OptimizedEncodingListener* aHeld) { aHeld->Release(); });
        mStreaming->SetMoreFunctionsCanBeSerializedCallback(
            [listener](v8::CompiledWasmModule aCompiled) {
              v8::OwnedBuffer serialized = aCompiled.Serialize();
              if (serialized.size) {
                listener->storeOptimizedEncoding(serialized.buffer.get(),
                                                 serialized.size);
              }
            });
      }
      mStreaming->Finish(!mCompiledBytes.empty());
    }
    Bury([this] { delete this; });
  }

  void streamError(size_t aErrorCode) override {
    {
      std::lock_guard<std::mutex> lock(mLock);
      if (mDone) {
        return;
      }
      mDone = true;
    }
    std::shared_ptr<v8::WasmStreaming> streaming = mStreaming;
    JSContext* cx = mCx;
    Bury([streaming, cx, aErrorCode, this] {
      JS::ReportStreamErrorCallback report =
          RuntimeState(cx->runtime).reportStreamError;
      AutoV8 scope(cx);
      v8::Local<v8::Value> error;
      if (report) {
        report(cx, aErrorCode);
        JS::Rooted<JS::Value> pending(cx);
        if (JS_GetPendingException(cx, &pending)) {
          error = ToV8(cx, pending);
        }
        JS_ClearPendingException(cx);
      }
      if (error.IsEmpty()) {
        error = v8::Exception::TypeError(
            V8String(cx->isolate, "WebAssembly stream failed"));
      }
      streaming->Abort(error);
      delete this;
    });
  }

  void consumeOptimizedEncoding(const uint8_t* aBegin,
                                size_t aLength) override {
    std::lock_guard<std::mutex> lock(mLock);
    if (mDone) {
      return;
    }
    mCompiledBytes.assign(aBegin, aBegin + aLength);
    if (!mStreaming->SetCompiledModuleBytes(mCompiledBytes.data(),
                                            mCompiledBytes.size())) {
      mCompiledBytes.clear();
    }
  }

  void noteResponseURLs(const char* aMaybeUrl,
                        const char* aMaybeSourceMapUrl) override {
    std::lock_guard<std::mutex> lock(mLock);
    if (aMaybeUrl && !mDone) {
      mStreaming->SetUrl(aMaybeUrl, strlen(aMaybeUrl));
    }
  }

 private:
  std::mutex mLock;
  std::shared_ptr<v8::WasmStreaming> mStreaming;
  JSContext* mCx;
  bool mDone = false;
  std::vector<uint8_t> mCompiledBytes;
};

void WasmStreamingCallback(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  JSContext* cx = CurrentContext();
  v8::Isolate* isolate = aInfo.GetIsolate();
  std::shared_ptr<v8::WasmStreaming> streaming =
      v8::WasmStreaming::Unpack(isolate, aInfo.Data());
  JS::ConsumeStreamCallback consume = RuntimeState(cx->runtime).consumeStream;
  if (!consume || !aInfo[0]->IsObject()) {
    streaming->Abort(v8::Exception::TypeError(
        V8String(isolate, "WebAssembly streaming requires a Response")));
    return;
  }
  JS::Rooted<JSObject*> response(cx,
                                 ObjectFromV8(cx, aInfo[0].As<v8::Object>()));
  VgeStreamConsumer* consumer = new VgeStreamConsumer(streaming, cx);
  if (!consume(cx, response, JS::MimeType::Wasm, consumer)) {
    JS::Rooted<JS::Value> error(cx);
    v8::Local<v8::Value> exception;
    if (JS_GetPendingException(cx, &error)) {
      exception = ToV8(cx, error);
    } else {
      exception = v8::Exception::TypeError(
          V8String(isolate, "WebAssembly streaming failed"));
    }
    JS_ClearPendingException(cx);
    streaming->Abort(exception);
    delete consumer;
  }
}

}

void InstallWasmStreaming(JSContext* aCx) {
  aCx->isolate->SetWasmStreamingCallback(WasmStreamingCallback);
}

}

using namespace vge::jsapi;

bool JS::IsWasmModuleObject(JS::HandleObject obj) {
  JSContext* cx = CurrentContext();
  JSObject* unwrapped = js::UncheckedUnwrap(obj);
  v8::HandleScope scope(cx->isolate);
  return ToV8(cx, unwrapped)->IsWasmModuleObject();
}

RefPtr<JS::WasmModule> JS::GetWasmModule(JS::HandleObject obj) {
  JSContext* cx = CurrentContext();
  JSObject* unwrapped = js::UncheckedUnwrap(obj);
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::Object> object = ToV8(cx, unwrapped);
  if (!object->IsWasmModuleObject()) {
    return nullptr;
  }
  return new VgeWasmModule(
      object.As<v8::WasmModuleObject>()->GetCompiledModule());
}

JS::SharedWasmCompileArgs JS::BuildCompileArgsForESM(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options) {
  RefPtr<VgeWasmCompileArgs> args = new VgeWasmCompileArgs();
  if (options.filename()) {
    args->filename = options.filename().c_str();
  }
  return args;
}

JS::ESMCompileResult JS::CompileForESM(const JS::WasmCompileArgs& compileArgs,
                                       const uint8_t* bytes, size_t length) {
  JS::ESMCompileResult result;
  RunOnHelperIsolate([&](v8::Isolate* aIsolate,
                         v8::Local<v8::Context> aContext) {
    v8::TryCatch tryCatch(aIsolate);
    v8::Local<v8::WasmModuleObject> module;
    if (v8::WasmModuleObject::Compile(aIsolate,
                                      v8::MemorySpan<const uint8_t>(bytes, length))
            .ToLocal(&module)) {
      result.module = new VgeWasmModule(module->GetCompiledModule());
      result.status = JS::ESMCompileResult::Status::Success;
      return;
    }
    if (tryCatch.HasCaught() && !tryCatch.HasTerminated()) {
      v8::Local<v8::Value> exception = tryCatch.Exception();
      v8::Local<v8::Value> message;
      std::string text;
      if (exception->IsObject() &&
          exception.As<v8::Object>()
              ->Get(aContext, V8String(aIsolate, "message"))
              .ToLocal(&message)) {
        text = Utf8Of(aIsolate, message);
      } else {
        text = Utf8Of(aIsolate, exception);
      }
      result.error = DuplicateChars(text);
      result.status = result.error ? JS::ESMCompileResult::Status::Failed
                                   : JS::ESMCompileResult::Status::OutOfMemory;
      return;
    }
    result.status = JS::ESMCompileResult::Status::OutOfMemory;
  });
  return result;
}

bool JS::FinishCompileForESM(JSContext* cx,
                             const JS::WasmCompileArgs& compileArgs,
                             const JS::ESMCompileResult& compileResult,
                             JS::MutableHandleObject moduleObj) {
  size_t warnings = std::min<size_t>(compileResult.warnings.length(), 3);
  for (size_t i = 0; i < warnings; i++) {
    ThrowNumberedError(cx, js::GetErrorMessage, nullptr,
                             JSMSG_WASM_COMPILE_WARNING,
                             compileResult.warnings[i].get());
  }
  if (compileResult.warnings.length() > warnings) {
    ThrowNumberedError(cx, js::GetErrorMessage, nullptr,
                             JSMSG_WASM_COMPILE_WARNING,
                             "other warnings suppressed");
  }
  switch (compileResult.status) {
    case JS::ESMCompileResult::Status::OutOfMemory:
      ThrowNumberedError(cx, js::GetErrorMessage, nullptr,
                               JSMSG_OUT_OF_MEMORY);
      return false;
    case JS::ESMCompileResult::Status::Failed: {
      const VgeWasmCompileArgs& args =
          static_cast<const VgeWasmCompileArgs&>(compileArgs);
      ThrowErrorAt(cx, JSEXN_WASMCOMPILEERROR, compileResult.error.get(),
                   args.filename, 0, 1, std::u16string(), 0, false);
      return false;
    }
    case JS::ESMCompileResult::Status::Success:
      break;
  }
  JSObject* object = compileResult.module->createObject(cx);
  if (!object) {
    return false;
  }
  moduleObj.set(object);
  return true;
}

void JS::InitConsumeStreamCallback(JSContext* cx,
                                   JS::ConsumeStreamCallback consume,
                                   JS::ReportStreamErrorCallback report) {
  ScriptsRuntime& state = RuntimeState(cx->runtime);
  state.consumeStream = consume;
  state.reportStreamError = report;
  EnsureIsolateSetup(cx);
  InstallWasmStreaming(cx);
}
