#include "VgeJsGcInternal.h"

#include <algorithm>
#include <string>

#include "VgeJsScripts.h"
#include "js/ProfilingFrameIterator.h"
#include "js/ProfilingSources.h"
#include "js/ProfilingStack.h"
#include "js/experimental/PCCountProfiling.h"
#include "mozilla/IntegerRange.h"
#include "mozilla/MathAlgorithms.h"

namespace js::jit {

class JitcodeGlobalEntry {
 public:
  uintptr_t start = 0;
  size_t length = 0;
  const char* name = nullptr;
  uint64_t realmID = 0;
  int scriptId = 0;
};

}

namespace vge::jsapi::gc {

namespace {

JS::RegisterThreadCallback sRegisterThread = nullptr;
JS::UnregisterThreadCallback sUnregisterThread = nullptr;

struct SamplePosition {
  uint32_t count;
  uint32_t index;
  void* sp;
};

static_assert(sizeof(SamplePosition) <= 9 * sizeof(void*));

const char* Intern(GcState& aState, const char* aChars, size_t aLength) {
  auto [it, inserted] = aState.namePool.emplace(aChars, aLength);
  return it->c_str();
}

uint64_t RealmIdForScript(int aScriptId) {
  ScriptData* data = ScriptDataByV8Id(aScriptId);
  if (!data || !data->realm) {
    return 0;
  }
  return data->realm->creationOptions.profilerRealmID();
}

void CodeEventHandler(const v8::JitCodeEvent* aEvent) {
  JSContext* cx = CurrentContext();
  if (!cx) {
    return;
  }
  GcState& state = StateOf(cx->runtime);
  std::lock_guard<std::mutex> lock(state.codeLock);
  uintptr_t start = uintptr_t(aEvent->code_start);
  switch (aEvent->type) {
    case v8::JitCodeEvent::CODE_ADDED: {
      if (aEvent->code_type == v8::JitCodeEvent::BYTE_CODE) {
        break;
      }
      int scriptId = 0;
      if (!aEvent->script.IsEmpty()) {
        scriptId = aEvent->script->GetId();
        state.profiledScriptIds.insert(scriptId);
      }
      CodeRange range{start, aEvent->code_len,
                      Intern(state, aEvent->name.str, aEvent->name.len),
                      RealmIdForScript(scriptId), scriptId};
      auto it = std::lower_bound(
          state.codeRanges.begin(), state.codeRanges.end(), start,
          [](const CodeRange& r, uintptr_t s) { return r.start < s; });
      state.codeRanges.insert(it, range);
      break;
    }
    case v8::JitCodeEvent::CODE_MOVED: {
      for (auto it = state.codeRanges.begin(); it != state.codeRanges.end();
           ++it) {
        if (it->start == start) {
          CodeRange moved = *it;
          moved.start = uintptr_t(aEvent->new_code_start);
          state.codeRanges.erase(it);
          auto pos = std::lower_bound(
              state.codeRanges.begin(), state.codeRanges.end(), moved.start,
              [](const CodeRange& r, uintptr_t s) { return r.start < s; });
          state.codeRanges.insert(pos, moved);
          break;
        }
      }
      break;
    }
    case v8::JitCodeEvent::CODE_REMOVED: {
      for (auto it = state.codeRanges.begin(); it != state.codeRanges.end();
           ++it) {
        if (it->start == start) {
          state.codeRanges.erase(it);
          break;
        }
      }
      break;
    }
    default:
      break;
  }
}

bool FindCode(GcState& aState, uintptr_t aAddr, CodeRange* aOut) {
  std::lock_guard<std::mutex> lock(aState.codeLock);
  auto it = std::upper_bound(
      aState.codeRanges.begin(), aState.codeRanges.end(), aAddr,
      [](uintptr_t a, const CodeRange& r) { return a < r.start; });
  if (it == aState.codeRanges.begin()) {
    return false;
  }
  --it;
  if (aAddr >= it->start + it->length) {
    return false;
  }
  *aOut = *it;
  return true;
}

void AppendJsonString(std::string& aOut, const std::string& aText) {
  aOut += '"';
  for (char c : aText) {
    switch (c) {
      case '"':
        aOut += "\\\"";
        break;
      case '\\':
        aOut += "\\\\";
        break;
      case '\n':
        aOut += "\\n";
        break;
      case '\r':
        aOut += "\\r";
        break;
      case '\t':
        aOut += "\\t";
        break;
      default:
        if (uint8_t(c) < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          aOut += buf;
        } else {
          aOut += c;
        }
    }
  }
  aOut += '"';
}

JSString* NewUtf8String(JSContext* aCx, const std::string& aText) {
  v8::HandleScope scope(aCx->isolate);
  v8::Local<v8::String> str;
  if (!v8::String::NewFromUtf8(aCx->isolate, aText.data(),
                               v8::NewStringType::kNormal, int(aText.size()))
           .ToLocal(&str)) {
    return nullptr;
  }
  return StringFromV8(aCx, str);
}

JS::UniqueChars DupChars(const std::string& aText) {
  char* out = js_pod_malloc<char>(aText.size() + 1);
  if (!out) {
    return nullptr;
  }
  memcpy(out, aText.c_str(), aText.size() + 1);
  return JS::UniqueChars(out);
}

JS::UniqueTwoByteChars DupChars16(const std::u16string& aText) {
  char16_t* out = js_pod_malloc<char16_t>(aText.size() + 1);
  if (!out) {
    return nullptr;
  }
  memcpy(out, aText.c_str(), (aText.size() + 1) * sizeof(char16_t));
  return JS::UniqueTwoByteChars(out);
}

ProfilerJSSourceData SourceDataFor(ScriptData* aData, int aScriptId,
                                   bool aGatherText) {
  JS::UniqueChars path = DupChars(aData->filename);
  JS::UniqueTwoByteChars map = DupChars16(aData->sourceMapURL);
  if (aGatherText && !aData->source.empty()) {
    JS::UniqueTwoByteChars text = DupChars16(aData->source);
    return ProfilerJSSourceData(uint32_t(aScriptId), std::move(text),
                                aData->source.size(), std::move(path),
                                aData->filename.size(), aData->lineno,
                                aData->column, std::move(map),
                                aData->sourceMapURL.size());
  }
  return ProfilerJSSourceData::CreateRetrievableFile(
      uint32_t(aScriptId), std::move(path), aData->filename.size(),
      aData->lineno, aData->column, std::move(map),
      aData->sourceMapURL.size());
}

}

}

using namespace vge::jsapi;
using namespace vge::jsapi::gc;

void js::GeckoProfilerThread::setProfilingStack(ProfilingStack* profilingStack,
                                                bool enabled) {
  profilingStack_ = profilingStack;
  profilingStackIfEnabled_ = enabled ? profilingStack : nullptr;
}

ProfilingStack::~ProfilingStack() {
  MOZ_RELEASE_ASSERT(stackPointer == 0);
  delete[] frames;
}

void ProfilingStack::ensureCapacitySlow() {
  const uint32_t kInitialCapacity = 4096 / sizeof(js::ProfilingStackFrame);
  uint32_t sp = stackPointer;
  uint32_t newCapacity;
  if (!capacity) {
    newCapacity = kInitialCapacity;
  } else {
    size_t memoryGoal = mozilla::RoundUpPow2(capacity * 2 *
                                             sizeof(js::ProfilingStackFrame));
    newCapacity = uint32_t(memoryGoal / sizeof(js::ProfilingStackFrame));
  }
  newCapacity = std::max(sp + 1, newCapacity);
  auto* newFrames = new js::ProfilingStackFrame[newCapacity];
  for (auto i : mozilla::IntegerRange(capacity)) {
    newFrames[i] = frames[i];
  }
  js::ProfilingStackFrame* oldFrames = frames;
  frames = newFrames;
  capacity = newCapacity;
  delete[] oldFrames;
}

int32_t js::ProfilingStackFrame::pcToOffset(JSScript* aScript,
                                            jsbytecode* aPc) {
  if (!aPc) {
    return NullPCOffset;
  }
  return int32_t(uintptr_t(aPc) - uintptr_t(aScript));
}

JS_PUBLIC_API JSScript* js::ProfilingStackFrame::script() const {
  return reinterpret_cast<JSScript*>(spOrScript.operator void*());
}

JS_PUBLIC_API JSFunction* js::ProfilingStackFrame::function() const {
  JSScript* s = script();
  if (!s) {
    return nullptr;
  }
  ScriptData* data = ScriptDataOf(s);
  JSContext* cx = CurrentContext();
  if (!data || !cx || data->function.IsEmpty()) {
    return nullptr;
  }
  v8::HandleScope scope(cx->isolate);
  return reinterpret_cast<JSFunction*>(
      ObjectFromV8(cx, data->function.Get(cx->isolate)));
}

JS_PUBLIC_API jsbytecode* js::ProfilingStackFrame::pc() const {
  if (pcOffsetIfJS_ == NullPCOffset) {
    return nullptr;
  }
  JSScript* s = script();
  return s ? reinterpret_cast<jsbytecode*>(uintptr_t(s) +
                                           uintptr_t(int32_t(pcOffsetIfJS_)))
           : nullptr;
}

JS_PUBLIC_API uint32_t js::ProfilingStackFrame::sourceId() const {
  return sourceId_;
}

JS_PUBLIC_API void JS::SetProfilingThreadCallbacks(
    JS::RegisterThreadCallback registerThread,
    JS::UnregisterThreadCallback unregisterThread) {
  sRegisterThread = registerThread;
  sUnregisterThread = unregisterThread;
}

JS_PUBLIC_API void js::SetContextProfilingStack(
    JSContext* cx, ProfilingStack* profilingStack) {
  GcState& state = StateOf(cx->runtime);
  JS::RootingContext::get(cx)->geckoProfiler().setProfilingStack(
      profilingStack, state.profilingEnabled);
}

JS_PUBLIC_API void js::EnableContextProfilingStack(JSContext* cx,
                                                   bool enabled) {
  GcState& state = StateOf(cx->runtime);
  JS::RootingContext::get(cx)->geckoProfiler().enable(enabled);
  if (state.profilingEnabled == enabled) {
    return;
  }
  state.profilingEnabled = enabled;
  if (enabled) {
    cx->isolate->SetJitCodeEventHandler(v8::kJitCodeEventEnumExisting,
                                        CodeEventHandler);
  } else {
    cx->isolate->SetJitCodeEventHandler(v8::kJitCodeEventDefault, nullptr);
    std::lock_guard<std::mutex> lock(state.codeLock);
    state.codeRanges.clear();
  }
}

JS_PUBLIC_API void js::RegisterContextProfilerMarkers(
    JSContext* cx,
    void (*eventMarker)(mozilla::MarkerCategory, const char*, const char*),
    void (*intervalMarker)(mozilla::MarkerCategory, const char*,
                           mozilla::TimeStamp, const char*),
    void (*flowMarker)(mozilla::MarkerCategory, const char*, uint64_t),
    void (*terminatingFlowMarker)(mozilla::MarkerCategory, const char*,
                                  uint64_t)) {
  GcState& state = StateOf(cx->runtime);
  state.eventMarker = eventMarker;
  state.intervalMarker = intervalMarker;
  state.flowMarker = flowMarker;
  state.terminatingFlowMarker = terminatingFlowMarker;
}

JS_PUBLIC_API bool JS::IsProfilingEnabledForContext(JSContext* cx) {
  GcState* state = MaybeStateOf(cx->runtime);
  return state && state->profilingEnabled;
}

JS_PUBLIC_API void JS::SetJSContextProfilerSampleBufferRangeStart(
    JSContext* cx, uint64_t rangeStart) {
  StateOf(cx->runtime).sampleBufferRangeStart = rangeStart;
}

JS::ProfilingFrameIterator::ProfilingFrameIterator(
    JSContext* cx, const RegisterState& state,
    const mozilla::Maybe<uint64_t>& samplePositionInProfilerBuffer)
    : cx_(cx),
      samplePositionInProfilerBuffer_(samplePositionInProfilerBuffer),
      activation_(nullptr) {
  auto* pos = new (storage()) SamplePosition{0, 0, state.sp};
  GcState* gcState = MaybeStateOf(cx->runtime);
  if (!gcState || !gcState->profilingEnabled) {
    return;
  }
  v8::RegisterState regs;
  regs.pc = state.pc;
  regs.sp = state.sp;
  regs.fp = state.fp;
  regs.lr = state.lr;
  v8::SampleInfo info{};
  cx->isolate->GetStackSample(regs, gcState->samplerFrames, kSampleFrames,
                              &info);
  pos->count = uint32_t(info.frames_count);
  endStackAddress_ = state.sp;
  if (pos->count) {
    activation_ = reinterpret_cast<js::Activation*>(gcState);
  }
}

JS::ProfilingFrameIterator::~ProfilingFrameIterator() = default;

void JS::ProfilingFrameIterator::operator++() {
  auto* pos = static_cast<SamplePosition*>(storage());
  pos->index++;
  if (pos->index >= pos->count) {
    activation_ = nullptr;
  }
}

bool JS::ProfilingFrameIterator::isWasm() const { return false; }

bool JS::ProfilingFrameIterator::isJSJit() const { return !done(); }

void* JS::ProfilingFrameIterator::stackAddress() const {
  return static_cast<const SamplePosition*>(storage())->sp;
}

mozilla::Maybe<JS::ProfilingFrameIterator::Frame>
JS::ProfilingFrameIterator::getPhysicalFrameWithoutLabel() const {
  if (done()) {
    return mozilla::Nothing();
  }
  auto* pos = static_cast<const SamplePosition*>(storage());
  auto* gcState = reinterpret_cast<GcState*>(activation_);
  Frame frame;
  frame.kind = Frame_Ion;
  frame.stackAddress = pos->sp;
  frame.returnAddress_ = gcState->samplerFrames[pos->index];
  frame.activation = activation_;
  frame.endStackAddress = endStackAddress_;
  frame.label = nullptr;
  frame.interpreterScript = nullptr;
  frame.realmID = 0;
  frame.sourceId = 0;
  frame.line = 0;
  frame.column = 0;
  return mozilla::Some(frame);
}

uint32_t JS::ProfilingFrameIterator::extractStack(Frame* frames,
                                                  uint32_t offset,
                                                  uint32_t end) const {
  if (offset >= end) {
    return 0;
  }
  mozilla::Maybe<Frame> frame = getPhysicalFrameWithoutLabel();
  if (!frame) {
    return 0;
  }
  frames[offset] = *frame;
  return 1;
}

mozilla::Maybe<JS::ProfilingFrameIterator::RegisterState>
JS::ProfilingFrameIterator::getCppEntryRegisters() const {
  return mozilla::Nothing();
}

JS::ProfiledFrameHandle::ProfiledFrameHandle(
    JSRuntime* rt, js::jit::JitcodeGlobalEntry& entry, void* addr,
    const js::jit::CallStackFrameInfo& frameInfo, uint32_t depth)
    : rt_(rt),
      entry_(entry),
      addr_(addr),
      canonicalAddr_(reinterpret_cast<void*>(entry.start)),
      frameInfo_(frameInfo),
      depth_(depth) {}

JS_PUBLIC_API JS::ProfilingFrameIterator::FrameKind
JS::ProfiledFrameHandle::frameKind() const {
  return JS::ProfilingFrameIterator::Frame_Ion;
}

JS_PUBLIC_API uint64_t JS::ProfiledFrameHandle::realmID() const {
  return entry_.realmID;
}

JS_PUBLIC_API JS::ProfiledFrameHandle JS::ProfiledFrameRange::Iter::operator*()
    const {
  return ProfiledFrameHandle(range_.rt_, *range_.entry_, range_.addr_,
                             range_.frames_[index_], index_);
}

JS_PUBLIC_API JS::ProfiledFrameRange JS::GetProfiledFrames(
    JSContext* cx, void* addr, uint64_t samplePositionInBuffer) {
  GcState& state = StateOf(cx->runtime);
  thread_local js::jit::JitcodeGlobalEntry entry;
  CodeRange range;
  ProfiledFrameRange result(cx->runtime, addr, &entry, samplePositionInBuffer);
  if (!FindCode(state, uintptr_t(addr), &range)) {
    return result;
  }
  entry.start = range.start;
  entry.length = range.length;
  entry.name = range.name;
  entry.realmID = range.realmID;
  entry.scriptId = range.scriptId;
  uint32_t line = 0;
  uint32_t column = 0;
  if (ScriptData* data = ScriptDataByV8Id(range.scriptId)) {
    line = data->lineno;
    column = data->column;
  }
  result.frames_[0] = js::jit::CallStackFrameInfo{
      range.name, uint32_t(range.scriptId), line, column};
  result.depth_ = 1;
  return result;
}

JS_PUBLIC_API js::ProfilerJSSources js::GetProfilerScriptSources(
    JSRuntime* rt, bool gatherSourceText) {
  GcState& state = StateOf(rt);
  std::vector<int> ids;
  {
    std::lock_guard<std::mutex> lock(state.codeLock);
    ids.assign(state.profiledScriptIds.begin(), state.profiledScriptIds.end());
  }
  std::sort(ids.begin(), ids.end());
  js::ProfilerJSSources sources;
  for (int id : ids) {
    ScriptData* data = ScriptDataByV8Id(id);
    if (!data) {
      continue;
    }
    if (!sources.append(SourceDataFor(data, id, gatherSourceText))) {
      break;
    }
  }
  return sources;
}

JS_PUBLIC_API ProfilerJSSourceData js::RetrieveProfilerSourceContent(
    JSContext* cx, const char* filename) {
  GcState& state = StateOf(cx->runtime);
  std::vector<int> ids;
  {
    std::lock_guard<std::mutex> lock(state.codeLock);
    ids.assign(state.profiledScriptIds.begin(), state.profiledScriptIds.end());
  }
  for (int id : ids) {
    ScriptData* data = ScriptDataByV8Id(id);
    if (!data || data->filename != filename || data->source.empty()) {
      continue;
    }
    std::string utf8 = Utf8FromUtf16(data->source.data(), data->source.size());
    size_t length = utf8.size();
    return ProfilerJSSourceData(DupChars(utf8), length);
  }
  return ProfilerJSSourceData();
}

JS_PUBLIC_API void JS::StartPCCountProfiling(JSContext* cx) {
  GcState& state = StateOf(cx->runtime);
  if (state.pcCountProfiling) {
    return;
  }
  state.pcCountProfiling = true;
  state.pcCountScripts.clear();
  if (!state.profilingEnabled) {
    cx->isolate->SetJitCodeEventHandler(v8::kJitCodeEventEnumExisting,
                                        CodeEventHandler);
  }
}

JS_PUBLIC_API void JS::StopPCCountProfiling(JSContext* cx) {
  GcState& state = StateOf(cx->runtime);
  if (!state.pcCountProfiling) {
    return;
  }
  state.pcCountProfiling = false;
  {
    std::lock_guard<std::mutex> lock(state.codeLock);
    state.pcCountScripts.assign(state.profiledScriptIds.begin(),
                                state.profiledScriptIds.end());
  }
  std::sort(state.pcCountScripts.begin(), state.pcCountScripts.end());
  if (!state.profilingEnabled) {
    cx->isolate->SetJitCodeEventHandler(v8::kJitCodeEventDefault, nullptr);
  }
}

JS_PUBLIC_API void JS::PurgePCCounts(JSContext* cx) {
  StateOf(cx->runtime).pcCountScripts.clear();
}

JS_PUBLIC_API size_t JS::GetPCCountScriptCount(JSContext* cx) {
  return StateOf(cx->runtime).pcCountScripts.size();
}

JS_PUBLIC_API JSString* JS::GetPCCountScriptSummary(JSContext* cx,
                                                    size_t index) {
  GcState& state = StateOf(cx->runtime);
  if (index >= state.pcCountScripts.size()) {
    v8::HandleScope scope(cx->isolate);
    cx->setPending(v8::Exception::RangeError(
        v8::String::NewFromUtf8Literal(cx->isolate, "buffer too small")));
    return nullptr;
  }
  int id = state.pcCountScripts[index];
  ScriptData* data = ScriptDataByV8Id(id);
  std::string json = "{\"file\":";
  AppendJsonString(json, data ? data->filename : std::string());
  json += ",\"line\":" + std::to_string(data ? data->lineno : 0);
  CodeRange named{};
  {
    std::lock_guard<std::mutex> lock(state.codeLock);
    for (const CodeRange& r : state.codeRanges) {
      if (r.scriptId == id) {
        named = r;
        break;
      }
    }
  }
  if (named.name) {
    json += ",\"name\":";
    AppendJsonString(json, named.name);
  }
  json += ",\"totals\":{}}";
  return NewUtf8String(cx, json);
}

JS_PUBLIC_API JSString* JS::GetPCCountScriptContents(JSContext* cx,
                                                     size_t index) {
  GcState& state = StateOf(cx->runtime);
  if (index >= state.pcCountScripts.size()) {
    v8::HandleScope scope(cx->isolate);
    cx->setPending(v8::Exception::RangeError(
        v8::String::NewFromUtf8Literal(cx->isolate, "buffer too small")));
    return nullptr;
  }
  int id = state.pcCountScripts[index];
  ScriptData* data = ScriptDataByV8Id(id);
  std::string json = "{\"text\":";
  AppendJsonString(json, data ? Utf8FromUtf16(data->source.data(),
                                              data->source.size())
                              : std::string());
  json += ",\"line\":" + std::to_string(data ? data->lineno : 0);
  json += ",\"opcodes\":[]}";
  return NewUtf8String(cx, json);
}
