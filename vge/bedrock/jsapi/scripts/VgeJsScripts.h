#ifndef vge_bedrock_jsapi_scripts_VgeJsScripts_h
#define vge_bedrock_jsapi_scripts_VgeJsScripts_h

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "VgeJsInternal.h"
#include "js/CompilationAndEvaluation.h"
#include "js/CompileOptions.h"
#include "js/ContextOptions.h"
#include "js/EnvironmentChain.h"
#include "js/ErrorInterceptor.h"
#include "js/ErrorReport.h"
#include "js/Exception.h"
#include "js/Modules.h"
#include "js/SavedFrameAPI.h"
#include "js/SourceText.h"
#include "js/Stack.h"
#include "js/StreamConsumer.h"
#include "js/experimental/CompileScript.h"
#include "js/experimental/JSStencil.h"
#include "js/experimental/SourceHook.h"
#include "mozilla/Utf8.h"

namespace vge::jsapi {

class SharedSource {
 public:
  SharedSource& operator=(std::u16string aText) {
    mText = std::make_shared<const std::u16string>(std::move(aText));
    return *this;
  }
  const char16_t* data() const { return text().data(); }
  size_t size() const { return text().size(); }
  bool empty() const { return text().empty(); }
  size_t capacity() const { return text().capacity(); }
  operator const std::u16string&() const { return text(); }
  const std::shared_ptr<const std::u16string>& shared() const { return mText; }

 private:
  const std::u16string& text() const {
    static const std::u16string kEmpty;
    return mText ? *mText : kEmpty;
  }
  std::shared_ptr<const std::u16string> mText;
};

struct ScriptData {
  JS::Realm* realm = nullptr;
  std::string filename;
  std::string introducerFilename;
  const char* introductionType = nullptr;
  std::u16string sourceMapURL;
  uint32_t lineno = 1;
  uint32_t column = 1;
  bool mutedErrors = false;
  bool nonSyntacticScope = false;
  bool noScriptRval = false;
  bool isModule = false;
  bool isFunction = false;
  bool hideFromDebugger = false;
  bool collectingDelazifications = false;
  SharedSource source;
  v8::Global<v8::UnboundScript> unbound;
  v8::Global<v8::Function> function;
  JS::Value privateValue = JS::UndefinedValue();
  v8::Global<v8::Value> privateKeep;
  JSObject* module = nullptr;
  int v8ScriptId = 0;
  uint32_t referrerId = 0;
  JSScript* cell = nullptr;
};

struct ScriptCell {
  uintptr_t header;
  ScriptData* data;
  uintptr_t pad[2];
  CellTail tail;

  JSScript* asScript() { return reinterpret_cast<JSScript*>(this); }
  static ScriptCell* from(const JSScript* aScript) {
    return reinterpret_cast<ScriptCell*>(const_cast<JSScript*>(aScript));
  }
};

static_assert(sizeof(ScriptCell) == sizeof(StringCell));
static_assert(offsetof(ScriptCell, tail) == offsetof(StringCell, tail));

struct ScriptsRuntime {
  JS::ModuleLoadHook moduleLoadHook = nullptr;
  JS::ModuleMetadataHook moduleMetadataHook = nullptr;
  JSErrorInterceptor* errorInterceptor = nullptr;
  JS::ConsumeStreamCallback consumeStream = nullptr;
  JS::ReportStreamErrorCallback reportStreamError = nullptr;
};

struct ScriptsContext {
  JS::ContextOptions options;
  v8::Global<v8::Value> pendingKeep;
  JS::Value pendingStackOwner = JS::UndefinedValue();
  JSObject* pendingStack = nullptr;
  v8::Global<v8::Value> pendingStackKeep;
  v8::Global<v8::StackTrace> pendingTrace;
  bool pendingStackKnown = false;
  JSObject* asyncStack = nullptr;
  v8::Global<v8::Value> asyncStackKeep;
  const char* asyncCause = nullptr;
  bool asyncCallIsExplicit = false;
  bool generatingError = false;
  mozilla::UniquePtr<js::SourceHook> sourceHook;
  mozilla::MallocSizeOf debuggerMallocSizeOf = nullptr;
  JS::NativeStackBase stackBase = 0;
  std::vector<std::pair<JSObject*, v8::Global<v8::Value>>> jsmStack;
  std::string oomStackTrace;
};

ScriptsRuntime& RuntimeState(JSRuntime* aRuntime);
ScriptsContext& ContextState(JSContext* aCx);
void EnsureIsolateSetup(JSContext* aCx);

void Bury(std::function<void()> aDeleter);
void DrainGraveyard();

JSScript* NewScriptCell(JSContext* aCx, ScriptData* aData);
ScriptData* ScriptDataOf(const JSScript* aScript);
ScriptData* ScriptDataByV8Id(int aScriptId);
void RegisterV8ScriptId(ScriptData* aData, int aScriptId);
uint32_t NewReferrerId(ScriptData* aData);
ScriptData* ScriptDataByReferrerId(uint32_t aId);
JSScript* ScriptForFunction(JSContext* aCx, v8::Local<v8::Function> aFunction);

v8::Local<v8::String> V8String(v8::Isolate* aIsolate, const char* aUtf8);
v8::Local<v8::String> V8Source(v8::Isolate* aIsolate, const SharedSource& aSource);
v8::Local<v8::String> V8String(v8::Isolate* aIsolate, const char* aUtf8,
                               size_t aLength);
v8::Local<v8::String> V8String(v8::Isolate* aIsolate, const char16_t* aChars,
                               size_t aLength);
std::string Utf8Of(v8::Isolate* aIsolate, v8::Local<v8::Value> aValue);
std::u16string Utf16Of(v8::Isolate* aIsolate, v8::Local<v8::Value> aValue);
std::u16string Utf16FromUtf8(const char* aUtf8, size_t aLength);
std::string Utf8FromUtf16(const char16_t* aChars, size_t aLength);
JSString* NewJSStringUtf8(JSContext* aCx, const char* aUtf8);
JSString* NewJSStringUtf16(JSContext* aCx, const std::u16string& aText);

void FillScriptData(JSContext* aCx, ScriptData* aData,
                    const JS::ReadOnlyCompileOptions& aOptions);
v8::ScriptOrigin OriginFor(v8::Isolate* aIsolate, const ScriptData* aData,
                           bool aIsModule);
bool ValidateFilename(JSContext* aCx, const char* aFilename,
                      bool aSkipValidation);
JSScript* CompileScriptText(JSContext* aCx,
                            const JS::ReadOnlyCompileOptions& aOptions,
                            std::u16string&& aSource,
                            v8::ScriptCompiler::CachedData* aCache);
bool ExecuteScriptCell(JSContext* aCx, JSScript* aScript,
                       const std::vector<JSObject*>& aChain,
                       JSObject* aJsmEnvironment, JS::MutableHandleValue aRval);

void SetPendingException(JSContext* aCx, v8::Local<v8::Value> aException,
                         v8::Local<v8::StackTrace> aTrace);
void SetPendingValue(JSContext* aCx, JS::HandleValue aValue, JSObject* aStack,
                     bool aStackKnown);
void ClearPending(JSContext* aCx);
bool ThrowErrorUtf8(JSContext* aCx, JSExnType aType, const char* aMessage);
v8::Local<v8::Value> NewErrorValue(JSContext* aCx, JSExnType aType,
                                   v8::Local<v8::String> aMessage);
bool ThrowNumberedError(JSContext* aCx, JSErrorCallback aCallback,
                        void* aUserRef, unsigned aErrorNumber, ...);
bool ErrorReportFromMessage(JSContext* aCx, v8::Local<v8::Message> aMessage,
                            JSErrorReport* aReport, std::string* aFilename,
                            std::string* aMessageText,
                            std::u16string* aLinebuf);
JSExnType ExnTypeOfError(JSContext* aCx, v8::Local<v8::Object> aError);
JSExnType ExnTypeFromName(const std::string& aName);
bool ThrowErrorAt(JSContext* aCx, JSExnType aType, const std::string& aMessage,
                  const std::string& aFilename, uint32_t aLine,
                  uint32_t aColumn, const std::u16string& aLinebuf,
                  size_t aTokenOffset, bool aMuted);
bool RealmIsDebuggee(JS::Realm* aRealm);
void CallWarningReporter(JSContext* aCx, JSErrorReport* aReport);
void ReportOutOfMemoryWithStack(JSContext* aCx);

extern const JSClass SavedFrameClass;
extern const JSClass ModuleObjectClass;
extern const JSClass ModuleRequestClass;
extern const JSClass JSMEnvironmentClass;
extern const JSClass DebuggerObjectClass;

bool IsSavedFrameObject(JSObject* aObj);
JSObject* SavedFrameFromTrace(JSContext* aCx, v8::Local<v8::StackTrace> aTrace,
                              size_t aSkip, size_t aMaxFrames,
                              JSObject* aAsyncParent, const char* aAsyncCause,
                              JSPrincipals* aFirstSubsumed, bool aUseSubsumed);
bool CaptureStackObject(JSContext* aCx, size_t aMaxFrames,
                        JS::MutableHandleObject aStack);
JSObject* StackOfErrorObject(JSContext* aCx, v8::Local<v8::Object> aError);

JSObject* ModuleObjectForV8(JSContext* aCx, v8::Local<v8::Module> aModule);
JSObject* NewModuleObjectFor(JSContext* aCx, v8::Local<v8::Module> aModule,
                             ScriptData* aScript);
void InstallModuleCallbacks(v8::Isolate* aIsolate);
void InstallWasmStreaming(JSContext* aCx);
void RunOnHelperIsolate(
    const std::function<void(v8::Isolate*, v8::Local<v8::Context>)>& aWork);
JSObject* NewWasmSourcePhaseModule(JSContext* aCx, JS::HandleObject aWasm);

}

struct js::frontend::InitialStencilAndDelazifications {
  std::atomic<uint32_t> refCount{1};
  bool isModule = false;
  bool borrowed = false;
  bool cacheable = true;
  vge::jsapi::SharedSource source;
  size_t sourceLength = 0;
  std::string filename;
  std::string introducerFilename;
  std::u16string sourceMapURL;
  const char* introductionType = nullptr;
  uint32_t lineno = 1;
  uint32_t column = 1;
  bool mutedErrors = false;
  bool nonSyntacticScope = false;
  bool noScriptRval = false;
  bool hideFromDebugger = false;
  std::vector<uint8_t> codeCache;
  v8::Isolate* ownerIsolate = nullptr;
  vge::jsapi::ScriptData* mainData = nullptr;
  v8::Global<v8::Module> firstModule;
};

struct js::frontend::PreallocatedCompilationGCOutput {
  bool prepared = false;
};

#endif
