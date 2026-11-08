#include "VgeJsGcInternal.h"

#include <stdio.h>

#include <string>

#include "VgeJsScripts.h"
#include "js/AllocPolicy.h"
#include "js/AllocationRecording.h"
#include "js/Context.h"
#include "js/ErrorReport.h"
#include "js/Initialization.h"
#include "js/MemoryCallbacks.h"
#include "js/MemoryFunctions.h"
#include "js/Modules.h"
#include "js/ProfilingCategory.h"
#include "js/TelemetryTimers.h"
#include "js/Utility.h"
#include "js/Warnings.h"
#include "js/Wrapper.h"
#include "js/Zone.h"
#include "js/experimental/LoggingInterface.h"
#include "js/friend/DumpFunctions.h"
#include "js/friend/Wrapper.h"
#include "js/friend/StackLimits.h"
#include "js/friend/UsageStatistics.h"
#include "mozmemory.h"
#include "unicode/uclean.h"

namespace vge::jsapi::gc {

namespace {

arena_id_t CreateMallocArena() {
  arena_params_t params;
  params.mMaxDirtyIncreaseOverride = 5;
  params.mLabel = "JS malloc";
  return moz_create_arena_with_params(&params);
}

constexpr size_t kLargeAllocation = 25 * 1024 * 1024;

JS::LargeAllocationFailureCallback sLargeAllocationFailureCallback = nullptr;
JS::LoggingInterface sLoggingInterface;
bool sLoggingInstalled = false;

void ThrowRangeError(JSContext* aCx, const char* aMessage) {
  v8::HandleScope scope(aCx->isolate);
  v8::Local<v8::String> message;
  if (!v8::String::NewFromUtf8(aCx->isolate, aMessage).ToLocal(&message)) {
    return;
  }
  aCx->setPending(v8::Exception::RangeError(message));
}

void* RetryAllocation(js::AllocFunction aFunc, arena_id_t aArena,
                      size_t aBytes, void* aRealloc) {
  switch (aFunc) {
    case js::AllocFunction::Malloc:
      return js_arena_malloc(aArena, aBytes);
    case js::AllocFunction::Calloc:
      return js_arena_calloc(aArena, aBytes, 1);
    case js::AllocFunction::Realloc:
      return js_arena_realloc(aArena, aRealloc, aBytes);
  }
  return nullptr;
}

void InterruptTrampoline(v8::Isolate* aIsolate, void* aData) {
  JSContext* cx = static_cast<JSContext*>(aData);
  if (!JS_CheckForInterrupt(cx)) {
    aIsolate->TerminateExecution();
  }
}

void OomHandler(const char* aLocation, const v8::OOMDetails& aDetails) {
  JSContext* cx = CurrentContext();
  if (!cx) {
    return;
  }
  GcState* state = MaybeStateOf(cx->runtime);
  if (state && state->oomCallback) {
    state->oomCallback(cx, state->oomData);
  }
}

enum class NormalizationForm : int { NFC = 0, NFKC = 1, NFD = 2, NFKD = 3 };

}

}

using namespace vge::jsapi;
using namespace vge::jsapi::gc;

JS_PUBLIC_DATA arena_id_t js::MallocArena = CreateMallocArena();

mozilla::Atomic<js::AutoEnterOOMUnsafeRegion::AnnotateOOMAllocationSizeCallback,
                mozilla::Relaxed>
    js::AutoEnterOOMUnsafeRegion::annotateOOMSizeCallback(nullptr);

void* js::TempAllocPolicy::onOutOfMemory(arena_id_t arenaId,
                                         js::AllocFunction allocFunc,
                                         size_t nbytes, void* reallocPtr) {
  if (!hasJSContext()) {
    return nullptr;
  }
  JSContext* ctx = cx();
  GcState& state = StateOf(ctx->runtime);
  if (!state.inV8Gc) {
    if (nbytes >= kLargeAllocation && sLargeAllocationFailureCallback) {
      sLargeAllocationFailureCallback();
    }
    ctx->isolate->LowMemoryNotification();
    if (void* p = RetryAllocation(allocFunc, arenaId, nbytes, reallocPtr)) {
      return p;
    }
  }
  if (js::AutoEnterOOMUnsafeRegion::annotateOOMSizeCallback) {
    js::AutoEnterOOMUnsafeRegion::annotateOOMSizeCallback(nbytes);
  }
  JS_ReportOutOfMemory(ctx);
  return nullptr;
}

void js::TempAllocPolicy::reportAllocOverflow() const {
  if (hasJSContext()) {
    ThrowRangeError(cx(), "allocation size overflow");
  }
}

JS_PUBLIC_API void JS_ReportOutOfMemory(JSContext* cx) {
  GcState& state = StateOf(cx->runtime);
  if (state.oomCallback) {
    state.oomCallback(cx, state.oomData);
  }
  v8::HandleScope scope(cx->isolate);
  JSString* message =
      AtomFromV8(cx, v8::String::NewFromUtf8Literal(cx->isolate,
                                                    "out of memory"));
  cx->pendingException = JS::StringValue(message);
  cx->exceptionPending = true;
  cx->throwingOutOfMemory = true;
}

JS_PUBLIC_API void js::ReportOverRecursed(JSContext* maybecx) {
  if (maybecx) {
    ThrowRangeError(maybecx, "too much recursion");
  }
}

JS::StackKind js::AutoCheckRecursionLimit::stackKindForCurrentPrincipal(
    JSContext* cx) const {
  JS::Realm* realm = cx->realm();
  if (!realm || realm->isSystem ||
      (realm->principals && realm->principals == cx->runtime->trustedPrincipals)) {
    return JS::StackForTrustedScript;
  }
  return JS::StackForUntrustedScript;
}

JS_PUBLIC_API void* JS_malloc(JSContext* cx, size_t nbytes) {
  return js_arena_malloc(js::MallocArena, nbytes);
}

JS_PUBLIC_API void* JS_string_malloc(JSContext* cx, size_t nbytes) {
  return js_arena_malloc(js::StringBufferArena, nbytes);
}

JS_PUBLIC_API JSString* JS_NewLatin1String(
    JSContext* cx, js::UniquePtr<JS::Latin1Char[], JS::FreePolicy> chars,
    size_t length) {
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::String> str;
  if (!v8::String::NewFromOneByte(cx->isolate, chars.get(),
                                  v8::NewStringType::kNormal, int(length))
           .ToLocal(&str)) {
    JS_ReportOutOfMemory(cx);
    return nullptr;
  }
  return StringFromV8(cx, str);
}

JS_PUBLIC_API void JS::AddAssociatedMemory(JSObject* obj, size_t nbytes,
                                           JS::MemoryUse use) {
  if (!nbytes) {
    return;
  }
  JSContext* cx = CurrentContext();
  if (!cx) {
    return;
  }
  StateOf(cx->runtime).associatedMemory += nbytes;
  cx->isolate->AdjustAmountOfExternalAllocatedMemory(int64_t(nbytes));
}

JS_PUBLIC_API void JS::RemoveAssociatedMemory(JSObject* obj, size_t nbytes,
                                              JS::MemoryUse use) {
  if (!nbytes) {
    return;
  }
  JSContext* cx = CurrentContext();
  if (!cx) {
    return;
  }
  GcState& state = StateOf(cx->runtime);
  state.associatedMemory -= std::min(state.associatedMemory, nbytes);
  cx->isolate->AdjustAmountOfExternalAllocatedMemory(-int64_t(nbytes));
}

JS_PUBLIC_API void JS::SetOutOfMemoryCallback(JSContext* cx,
                                              JS::OutOfMemoryCallback cb,
                                              void* data) {
  GcState& state = StateOf(cx->runtime);
  state.oomCallback = cb;
  state.oomData = data;
  cx->isolate->SetOOMErrorHandler(OomHandler);
}

JS_PUBLIC_API void JS::SetProcessLargeAllocationFailureCallback(
    JS::LargeAllocationFailureCallback afc) {
  sLargeAllocationFailureCallback = afc;
}

JS_PUBLIC_API bool JS_AddInterruptCallback(JSContext* cx,
                                           JSInterruptCallback callback) {
  if (cx->interruptCallbackCount >= std::size(cx->interruptCallbacks)) {
    return false;
  }
  cx->interruptCallbacks[cx->interruptCallbackCount++] = callback;
  return true;
}

JS_PUBLIC_API bool JS_DisableInterruptCallback(JSContext* cx) {
  GcState& state = StateOf(cx->runtime);
  bool result = state.interruptCallbackDisabled;
  state.interruptCallbackDisabled = true;
  return result;
}

JS_PUBLIC_API void JS_ResetInterruptCallback(JSContext* cx, bool enable) {
  StateOf(cx->runtime).interruptCallbackDisabled = enable;
}

JS_PUBLIC_API void JS_RequestInterruptCallback(JSContext* cx) {
  GcState& state = StateOf(cx->runtime);
  state.interruptRequested = true;
  cx->isolate->RequestInterrupt(InterruptTrampoline, cx);
}

JS_PUBLIC_API void JS_RequestInterruptCallbackCanWait(JSContext* cx) {
  JS_RequestInterruptCallback(cx);
}

JS_PUBLIC_API bool JS_CheckForInterrupt(JSContext* cx) {
  GcState& state = StateOf(cx->runtime);
  if (!state.interruptRequested.exchange(false)) {
    return true;
  }
  if (state.interruptCallbackDisabled) {
    return true;
  }
  bool stop = false;
  for (size_t i = 0; i < cx->interruptCallbackCount; i++) {
    if (!cx->interruptCallbacks[i](cx)) {
      stop = true;
    }
  }
  return !stop;
}

JS_PUBLIC_API void JS_SetDestroyZoneCallback(JSContext* cx,
                                             JSDestroyZoneCallback callback) {
  cx->runtime->destroyZoneCallback = callback;
}

JS_PUBLIC_API void JS_SetDestroyCompartmentCallback(
    JSContext* cx, JSDestroyCompartmentCallback callback) {
  cx->runtime->destroyCompartmentCallback = callback;
}

JS_PUBLIC_API void JS_SetSizeOfIncludingThisCompartmentCallback(
    JSContext* cx, JSSizeOfIncludingThisCompartmentCallback callback) {
  cx->runtime->sizeOfCompartmentCallback = callback;
}

JS_PUBLIC_API void JS_SetCompartmentPrivate(JS::Compartment* compartment,
                                            void* data) {
  compartment->data = data;
}

JS_PUBLIC_API void* JS_GetCompartmentPrivate(JS::Compartment* compartment) {
  return compartment->data;
}

JS_PUBLIC_API bool JS_RefreshCrossCompartmentWrappers(JSContext* cx,
                                                      JS::HandleObject obj) {
  return js::RecomputeWrappers(
      cx, js::AllCompartments(),
      js::SingleCompartment(JS::GetCompartment(obj)));
}

JS_PUBLIC_API bool JS::SetBrittleMode(JSContext* cx, bool setting) {
  GcState& state = StateOf(cx->runtime);
  bool was = state.brittleMode;
  state.brittleMode = setting;
  return was;
}

JS_PUBLIC_API JS::WarningReporter JS::SetWarningReporter(
    JSContext* cx, JS::WarningReporter reporter) {
  JS::WarningReporter older = cx->warningReporter;
  cx->warningReporter = reporter;
  return older;
}

JS_PUBLIC_API JS::WarningReporter JS::GetWarningReporter(JSContext* cx) {
  return cx->warningReporter;
}

JS_PUBLIC_API JS::ModuleLoadHook JS::GetModuleLoadHook(JSRuntime* rt) {
  return RuntimeState(rt).moduleLoadHook;
}

JS_PUBLIC_API void JS_SetAccumulateTelemetryCallback(
    JSContext* cx, JSAccumulateTelemetryDataCallback callback) {
  StateOf(cx->runtime).telemetryCallback = callback;
}

JS_PUBLIC_API void JS_SetSetUseCounterCallback(
    JSContext* cx, JSSetUseCounterCallback callback) {
  StateOf(cx->runtime).useCounterCallback = callback;
}

JS_PUBLIC_API void JS::SetMeasuringExecutionTimeEnabled(JSContext* cx,
                                                        bool value) {
  GcState& state = StateOf(cx->runtime);
  state.measuringExecutionTime = value;
}

JS_PUBLIC_API JS::JSTimers JS::GetJSTimers(JSContext* cx) {
  return StateOf(cx->runtime).timers;
}

JS_PUBLIC_API void JS::EnableRecordingAllocations(
    JSContext* cx, JS::RecordAllocationsCallback callback, double probability) {
  GcState& state = StateOf(cx->runtime);
  state.recordAllocations = callback;
  state.recordProbability = probability;
}

JS_PUBLIC_API void JS::DisableRecordingAllocations(JSContext* cx) {
  GcState& state = StateOf(cx->runtime);
  state.recordAllocations = nullptr;
  state.recordProbability = 0.0;
}

JS_PUBLIC_API bool JS::SetLoggingInterface(JS::LoggingInterface& iface) {
  sLoggingInterface = iface;
  sLoggingInstalled = iface.isComplete();
  return true;
}

JS_PUBLIC_API bool JS_SetICUMemoryFunctions(JS_ICUAllocFn allocFn,
                                            JS_ICUReallocFn reallocFn,
                                            JS_ICUFreeFn freeFn) {
  UErrorCode status = U_ZERO_ERROR;
  u_setMemoryFunctions(nullptr, allocFn, reallocFn, freeFn, &status);
  return U_SUCCESS(status);
}

JS_PUBLIC_API void JS::DisableJitBackend() {
  if (JS::detail::libraryInitState == JS::detail::InitState::Uninitialized) {
    static const char kFlag[] = "--jitless";
    v8::V8::SetFlagsFromString(kFlag, sizeof(kFlag) - 1);
  }
}

JS_PUBLIC_API void JS_FrontendOnlyShutDown() { JS_ShutDown(); }

namespace {

#define SUBCATEGORY_ENUMS_BEGIN_CATEGORY(name, labelAsString, color) \
  enum class ProfilingSubcategory_##name : uint32_t {
#define SUBCATEGORY_ENUMS_SUBCATEGORY(category, name, labelAsString) name,
#define SUBCATEGORY_ENUMS_END_CATEGORY \
  }                                    \
  ;
MOZ_PROFILING_CATEGORY_LIST(SUBCATEGORY_ENUMS_BEGIN_CATEGORY,
                            SUBCATEGORY_ENUMS_SUBCATEGORY,
                            SUBCATEGORY_ENUMS_END_CATEGORY)
#undef SUBCATEGORY_ENUMS_BEGIN_CATEGORY
#undef SUBCATEGORY_ENUMS_SUBCATEGORY
#undef SUBCATEGORY_ENUMS_END_CATEGORY

#define CATEGORY_INFO_BEGIN_CATEGORY(name, labelAsString, color)
#define CATEGORY_INFO_SUBCATEGORY(category, name, labelAsString) \
  {JS::ProfilingCategory::category,                              \
   uint32_t(ProfilingSubcategory_##category::name), labelAsString},
#define CATEGORY_INFO_END_CATEGORY
const JS::ProfilingCategoryPairInfo sProfilingCategoryPairInfo[] = {
    MOZ_PROFILING_CATEGORY_LIST(CATEGORY_INFO_BEGIN_CATEGORY,
                                CATEGORY_INFO_SUBCATEGORY,
                                CATEGORY_INFO_END_CATEGORY)};
#undef CATEGORY_INFO_BEGIN_CATEGORY
#undef CATEGORY_INFO_SUBCATEGORY
#undef CATEGORY_INFO_END_CATEGORY

}

JS_PUBLIC_API const JS::ProfilingCategoryPairInfo&
JS::GetProfilingCategoryPairInfo(JS::ProfilingCategoryPair aCategoryPair) {
  static_assert(std::size(sProfilingCategoryPairInfo) ==
                uint32_t(JS::ProfilingCategoryPair::COUNT));
  uint32_t index = uint32_t(aCategoryPair);
  MOZ_RELEASE_ASSERT(index <= uint32_t(JS::ProfilingCategoryPair::LAST));
  return sProfilingCategoryPairInfo[index];
}

JS_PUBLIC_API JS::UniqueChars JS::FormatStackDump(JSContext* cx, bool showArgs,
                                                  bool showLocals,
                                                  bool showThisProps) {
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::StackTrace> trace =
      v8::StackTrace::CurrentStackTrace(cx->isolate, 128);
  std::string out;
  int count = trace->GetFrameCount();
  for (int i = 0; i < count; i++) {
    v8::Local<v8::StackFrame> frame = trace->GetFrame(cx->isolate, i);
    v8::String::Utf8Value name(cx->isolate, frame->GetFunctionName());
    v8::String::Utf8Value file(cx->isolate, frame->GetScriptName());
    char line[1024];
    snprintf(line, sizeof(line), "%d %s() [\"%s\":%d:%d]\n", i,
             (*name && **name) ? *name : "<TOP LEVEL>",
             *file ? *file : "<unknown>", frame->GetLineNumber(),
             frame->GetColumn());
    out += line;
  }
  char* result = js_pod_malloc<char>(out.size() + 1);
  if (!result) {
    return nullptr;
  }
  memcpy(result, out.c_str(), out.size() + 1);
  return JS::UniqueChars(result);
}

JS_PUBLIC_API void js::DumpHeap(JSContext* cx, FILE* fp,
                                js::DumpHeapNurseryBehaviour nurseryBehaviour,
                                mozilla::MallocSizeOf mallocSizeOf) {
  if (nurseryBehaviour == js::CollectNurseryBeforeDump) {
    RunFullGC(cx, JS::GCOptions::Normal, JS::GCReason::API);
  }
  v8::HandleScope scope(cx->isolate);
  const v8::HeapSnapshot* snapshot =
      TakeReportSnapshot(cx, v8::HeapProfiler::HeapSnapshotOptions());
  if (!snapshot) {
    return;
  }
  fprintf(fp, "# Roots.\n");
  const v8::HeapGraphNode* root = snapshot->GetRoot();
  for (int i = 0; i < root->GetChildrenCount(); i++) {
    const v8::HeapGraphNode* to = root->GetChild(i)->GetToNode();
    v8::String::Utf8Value name(cx->isolate, to->GetName());
    fprintf(fp, "%u %s\n", to->GetId(), *name ? *name : "");
  }
  fprintf(fp, "==========\n# Zone %p.\n", cx->runtime->zone);
  int count = snapshot->GetNodesCount();
  for (int n = 0; n < count; n++) {
    const v8::HeapGraphNode* node = snapshot->GetNode(n);
    v8::String::Utf8Value name(cx->isolate, node->GetName());
    fprintf(fp, "%u B %s <%zu>\n", node->GetId(), *name ? *name : "",
            node->GetShallowSize());
    for (int e = 0; e < node->GetChildrenCount(); e++) {
      const v8::HeapGraphEdge* edge = node->GetChild(e);
      v8::String::Utf8Value edgeName(cx->isolate, edge->GetName());
      fprintf(fp, "> %u %s%s\n", edge->GetToNode()->GetId(),
              edge->GetType() == v8::HeapGraphEdge::kWeak ? "weak:" : "",
              *edgeName ? *edgeName : "");
    }
  }
  fflush(fp);
  const_cast<v8::HeapSnapshot*>(snapshot)->Delete();
}

extern "C" {
bool js_normalize_utf16(int form, const uint16_t* ptr, size_t len,
                        void* buffer);
bool js_normalize_latin1(int form, const uint8_t* ptr, size_t len,
                         void* buffer);

MOZ_EXPORT bool js_call_js_normalize_utf16(JSContext* cx, int form,
                                           JSLinearString* str,
                                           void* buffer) {
  StringCell* cell = StringCell::from(reinterpret_cast<JSString*>(str));
  JS::AutoCheckCannotGC nogc;
  if (!js_normalize_utf16(form, static_cast<const uint16_t*>(cell->chars),
                          cell->length(), buffer)) {
    JS_ReportOutOfMemory(cx);
    return false;
  }
  return true;
}

MOZ_EXPORT bool js_call_js_normalize_latin1(JSContext* cx, int form,
                                            JSLinearString* str,
                                            void* buffer) {
  StringCell* cell = StringCell::from(reinterpret_cast<JSString*>(str));
  JS::AutoCheckCannotGC nogc;
  if (!js_normalize_latin1(form, static_cast<const uint8_t*>(cell->chars),
                           cell->length(), buffer)) {
    JS_ReportOutOfMemory(cx);
    return false;
  }
  return true;
}

MOZ_EXPORT JSLinearString* js_new_ucstring_copy_n(JSContext* cx,
                                                  const char16_t* ptr,
                                                  size_t len) {
  bool latin1 = true;
  for (size_t i = 0; i < len && latin1; i++) {
    latin1 = ptr[i] < 256;
  }
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::String> str;
  if (latin1) {
    std::string bytes(len, '\0');
    for (size_t i = 0; i < len; i++) {
      bytes[i] = char(ptr[i]);
    }
    if (!v8::String::NewFromOneByte(
             cx->isolate, reinterpret_cast<const uint8_t*>(bytes.data()),
             v8::NewStringType::kNormal, int(len))
             .ToLocal(&str)) {
      JS_ReportOutOfMemory(cx);
      return nullptr;
    }
  } else if (!v8::String::NewFromTwoByte(
                  cx->isolate, reinterpret_cast<const uint16_t*>(ptr),
                  v8::NewStringType::kNormal, int(len))
                  .ToLocal(&str)) {
    JS_ReportOutOfMemory(cx);
    return nullptr;
  }
  return reinterpret_cast<JSLinearString*>(StringFromV8(cx, str));
}

MOZ_EXPORT JSLinearString* js_new_ucstring_copy_n_dont_deflate(
    JSContext* cx, const char16_t* ptr, size_t len) {
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::String> str;
  if (!v8::String::NewFromTwoByte(cx->isolate,
                                  reinterpret_cast<const uint16_t*>(ptr),
                                  v8::NewStringType::kNormal, int(len))
           .ToLocal(&str)) {
    JS_ReportOutOfMemory(cx);
    return nullptr;
  }
  return reinterpret_cast<JSLinearString*>(StringFromV8(cx, str));
}
}
