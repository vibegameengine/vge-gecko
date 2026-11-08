#include "VgeJsScripts.h"

#include "js/Debug.h"
#include "js/GCAPI.h"
#include "js/GCVector.h"
#include "js/Object.h"
#include "js/Realm.h"
#include "js/TracingAPI.h"
#include "js/Wrapper.h"
#include "mozilla/BaseProfilerUtils.h"
#include "mozilla/TimeStamp.h"

struct JS_TracerSummaryWriterImpl {
  std::vector<uint8_t>* buffer;
};

namespace vge::jsapi {

namespace {

struct DebuggerData {
  std::vector<JS::Heap<JSObject*>> debuggees;
};

thread_local std::vector<JSObject*> gDebuggers;

DebuggerData* DebuggerDataOf(JSObject* aObj) {
  JS::Value slot = JS::GetReservedSlot(aObj, 0);
  return slot.isUndefined() ? nullptr
                            : static_cast<DebuggerData*>(slot.toPrivate());
}

void FinalizeDebugger(JS::GCContext* aGcx, JSObject* aObj) {
  DebuggerData* data = DebuggerDataOf(aObj);
  for (size_t i = 0; i < gDebuggers.size(); i++) {
    if (gDebuggers[i] == aObj) {
      gDebuggers.erase(gDebuggers.begin() + i);
      break;
    }
  }
  if (data) {
    Bury([data] { delete data; });
  }
}

void TraceDebugger(JSTracer* aTrc, JSObject* aObj) {
  if (DebuggerData* data = DebuggerDataOf(aObj)) {
    for (auto& global : data->debuggees) {
      JS::TraceEdge(aTrc, &global, "debuggee global");
    }
  }
}

const JSClassOps kDebuggerOps = {
    nullptr, nullptr, nullptr,          nullptr, nullptr,
    nullptr, FinalizeDebugger, nullptr, nullptr, TraceDebugger,
};

JSObject* GlobalOf(JSContext* aCx, v8::Local<v8::Value> aValue) {
  if (!aValue->IsObject()) {
    return nullptr;
  }
  JSObject* object = js::UncheckedUnwrap(ObjectFromV8(aCx, aValue.As<v8::Object>()));
  JS::Realm* realm = JS::GetObjectRealmOrNull(object);
  return realm ? realm->global : nullptr;
}

JSObject* ThisDebugger(JSContext* aCx,
                       const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  if (!aInfo.This()->IsObject()) {
    return nullptr;
  }
  JSObject* self = ObjectFromV8(aCx, aInfo.This());
  return JS::GetClass(self) == &DebuggerObjectClass ? self : nullptr;
}

bool AddDebuggee(JSContext* aCx, JSObject* aDebugger, JSObject* aGlobal) {
  DebuggerData* data = DebuggerDataOf(aDebugger);
  for (auto& global : data->debuggees) {
    if (global.unbarrieredGet() == aGlobal) {
      return true;
    }
  }
  data->debuggees.emplace_back(aGlobal);
  return true;
}

void AddDebuggeeMethod(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  JSContext* cx = CurrentContext();
  JSObject* self = ThisDebugger(cx, aInfo);
  JSObject* global = self ? GlobalOf(cx, aInfo[0]) : nullptr;
  if (!self || !global) {
    aInfo.GetIsolate()->ThrowException(v8::Exception::TypeError(
        V8String(aInfo.GetIsolate(), "Debugger.addDebuggee: expected a global")));
    return;
  }
  AddDebuggee(cx, self, global);
  aInfo.GetReturnValue().Set(ToV8(cx, global));
}

void RemoveDebuggeeMethod(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  JSContext* cx = CurrentContext();
  JSObject* self = ThisDebugger(cx, aInfo);
  JSObject* global = self ? GlobalOf(cx, aInfo[0]) : nullptr;
  if (!self || !global) {
    return;
  }
  DebuggerData* data = DebuggerDataOf(self);
  for (size_t i = 0; i < data->debuggees.size(); i++) {
    if (data->debuggees[i].unbarrieredGet() == global) {
      data->debuggees.erase(data->debuggees.begin() + i);
      break;
    }
  }
}

void HasDebuggeeMethod(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  JSContext* cx = CurrentContext();
  JSObject* self = ThisDebugger(cx, aInfo);
  JSObject* global = self ? GlobalOf(cx, aInfo[0]) : nullptr;
  bool found = false;
  if (self && global) {
    for (auto& entry : DebuggerDataOf(self)->debuggees) {
      found = found || entry.unbarrieredGet() == global;
    }
  }
  aInfo.GetReturnValue().Set(found);
}

void GetDebuggeesMethod(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  JSContext* cx = CurrentContext();
  JSObject* self = ThisDebugger(cx, aInfo);
  v8::Local<v8::Array> out = v8::Array::New(aInfo.GetIsolate());
  if (self) {
    uint32_t index = 0;
    for (auto& entry : DebuggerDataOf(self)->debuggees) {
      out->Set(cx->v8Context(), index++, ToV8(cx, entry.get())).Check();
    }
  }
  aInfo.GetReturnValue().Set(out);
}

void RemoveAllDebuggeesMethod(
    const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  JSContext* cx = CurrentContext();
  if (JSObject* self = ThisDebugger(cx, aInfo)) {
    DebuggerDataOf(self)->debuggees.clear();
  }
}

void DebuggerConstruct(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  JSContext* cx = CurrentContext();
  v8::Isolate* isolate = aInfo.GetIsolate();
  if (!aInfo.IsConstructCall()) {
    isolate->ThrowException(v8::Exception::TypeError(
        V8String(isolate, "Debugger constructor requires 'new'")));
    return;
  }
  JS::Rooted<JSObject*> debugger(
      cx, NewClassObject(cx, &DebuggerObjectClass, nullptr));
  if (!debugger) {
    return;
  }
  JS::SetReservedSlot(debugger, 0, JS::PrivateValue(new DebuggerData()));
  gDebuggers.push_back(debugger);
  v8::Local<v8::Context> context = cx->v8Context();
  v8::Local<v8::Object> object = ToV8(cx, debugger.get());
  struct Method {
    const char* name;
    v8::FunctionCallback callback;
  } methods[] = {{"addDebuggee", AddDebuggeeMethod},
                 {"removeDebuggee", RemoveDebuggeeMethod},
                 {"hasDebuggee", HasDebuggeeMethod},
                 {"getDebuggees", GetDebuggeesMethod},
                 {"removeAllDebuggees", RemoveAllDebuggeesMethod}};
  for (const Method& method : methods) {
    v8::Local<v8::Function> function;
    if (v8::Function::New(context, method.callback, v8::Local<v8::Value>(), 1,
                          v8::ConstructorBehavior::kThrow)
            .ToLocal(&function)) {
      object
          ->DefineOwnProperty(context, V8String(isolate, method.name), function,
                              v8::DontEnum)
          .Check();
    }
  }
  for (int i = 0; i < aInfo.Length(); i++) {
    if (JSObject* global = GlobalOf(cx, aInfo[i])) {
      AddDebuggee(cx, debugger, global);
    }
  }
  aInfo.GetReturnValue().Set(object);
}

v8::Local<v8::Function> GcHookOf(JSContext* aCx, JSObject* aDebugger) {
  v8::Local<v8::Object> object = ToV8(aCx, aDebugger);
  v8::Local<v8::Value> hook;
  v8::TryCatch tryCatch(aCx->isolate);
  if (!object
           ->Get(aCx->v8Context(), V8String(aCx->isolate, "onGarbageCollection"))
           .ToLocal(&hook) ||
      !hook->IsFunction()) {
    return v8::Local<v8::Function>();
  }
  return hook.As<v8::Function>();
}

struct LabelEvent {
  JS::ExecutionTrace::EventKind kind;
  std::string label;
  double time;
};

struct TracerState {
  bool tracing = false;
  JS::ExecutionTrace::EventKind unused;
  std::vector<LabelEvent> events;
  std::vector<uint8_t> values;
  CustomObjectSummaryCallback customCallback = nullptr;
  mozilla::baseprofiler::BaseProfilerThreadId thread;
};

std::unordered_map<JSContext*, TracerState*> gTracers;
std::mutex gTracerLock;

TracerState& TracerOf(JSContext* aCx) {
  std::lock_guard<std::mutex> lock(gTracerLock);
  TracerState*& state = gTracers[aCx];
  if (!state) {
    state = new TracerState();
  }
  return *state;
}

double NowMilliseconds() {
  return (mozilla::TimeStamp::Now() - mozilla::TimeStamp::ProcessCreation())
      .ToMilliseconds();
}

void RecordLabel(JSContext* aCx, JS::ExecutionTrace::EventKind aKind,
                 std::string&& aLabel) {
  TracerState& state = TracerOf(aCx);
  if (!state.tracing) {
    return;
  }
  std::lock_guard<std::mutex> lock(gTracerLock);
  state.events.push_back(LabelEvent{aKind, std::move(aLabel), NowMilliseconds()});
}

std::string Latin1ToUtf8(const char* aLabel) {
  std::u16string wide;
  for (const char* p = aLabel; *p; p++) {
    wide.push_back(char16_t(static_cast<unsigned char>(*p)));
  }
  return Utf8FromUtf16(wide.data(), wide.size());
}

void PushBytes(std::vector<uint8_t>* aBuffer, const void* aData,
               size_t aLength) {
  const uint8_t* bytes = static_cast<const uint8_t*>(aData);
  aBuffer->insert(aBuffer->end(), bytes, bytes + aLength);
}

void WriteHeader(std::vector<uint8_t>* aBuffer, JS::ValueType aType,
                 uint8_t aFlags) {
  JS::ValueSummary header;
  header.type = aType;
  header.flags = aFlags;
  PushBytes(aBuffer, &header, 1);
}

void WriteSmallString(std::vector<uint8_t>* aBuffer,
                      const std::u16string& aText) {
  bool latin1 = true;
  for (char16_t c : aText) {
    latin1 = latin1 && c < 256;
  }
  size_t length =
      std::min(aText.size(), JS::ValueSummary::SMALL_STRING_LENGTH_LIMIT);
  JS::TracerStringEncoding encoding = latin1 ? JS::TracerStringEncoding::Latin1
                                             : JS::TracerStringEncoding::TwoByte;
  uint16_t typed = uint16_t(length) | uint16_t(uint16_t(encoding) << 14);
  PushBytes(aBuffer, &typed, sizeof(typed));
  if (latin1) {
    for (size_t i = 0; i < length; i++) {
      uint8_t c = uint8_t(aText[i]);
      aBuffer->push_back(c);
    }
  } else {
    PushBytes(aBuffer, aText.data(), length * sizeof(char16_t));
  }
}

template <typename CharT>
void WriteSmallCString(std::vector<uint8_t>* aBuffer, const CharT* aChars,
                       JS::TracerStringEncoding aEncoding) {
  size_t length = std::char_traits<CharT>::length(aChars);
  length = std::min(length, JS::ValueSummary::SMALL_STRING_LENGTH_LIMIT);
  uint16_t typed = uint16_t(length) | uint16_t(uint16_t(aEncoding) << 14);
  PushBytes(aBuffer, &typed, sizeof(typed));
  PushBytes(aBuffer, aChars, length * sizeof(CharT));
}

bool WriteValueSummary(JSContext* aCx, std::vector<uint8_t>* aBuffer,
                       JS::HandleValue aValue, bool aNested);

}

const JSClass DebuggerObjectClass = {
    "Debugger",
    JSCLASS_HAS_RESERVED_SLOTS(1) | JSCLASS_FOREGROUND_FINALIZE,
    &kDebuggerOps,
};

void ReportOutOfMemoryWithStack(JSContext* aCx) {
  {
    v8::HandleScope scope(aCx->isolate);
    v8::Local<v8::StackTrace> trace = v8::StackTrace::CurrentStackTrace(
        aCx->isolate, 64, v8::StackTrace::kDetailed);
    std::string text;
    for (int i = 0; i < trace->GetFrameCount(); i++) {
      v8::Local<v8::StackFrame> frame =
          trace->GetFrame(aCx->isolate, uint32_t(i));
      text += Utf8Of(aCx->isolate, frame->GetFunctionName());
      text += '@';
      text += Utf8Of(aCx->isolate, frame->GetScriptNameOrSourceURL());
      text += ':';
      text += std::to_string(frame->GetLineNumber());
      text += ':';
      text += std::to_string(frame->GetColumn());
      text += '\n';
    }
    ContextState(aCx).oomStackTrace = std::move(text);
  }
  JS_ReportOutOfMemory(aCx);
}

bool RealmIsDebuggee(JS::Realm* aRealm) {
  if (!aRealm) {
    return false;
  }
  for (JSObject* debugger : gDebuggers) {
    for (auto& global : DebuggerDataOf(debugger)->debuggees) {
      if (JS::GetObjectRealmOrNull(global.unbarrieredGet()) == aRealm) {
        return true;
      }
    }
  }
  return false;
}

namespace {

bool WriteValueSummary(JSContext* aCx, std::vector<uint8_t>* aBuffer,
                       JS::HandleValue aValue, bool aNested) {
  if (aValue.isDouble()) {
    double d = aValue.toDouble();
    if (d == 0 && !std::signbit(d)) {
      WriteHeader(aBuffer, JS::ValueType::Double, 0);
    } else {
      WriteHeader(aBuffer, JS::ValueType::Double,
                  JS::ValueSummary::NUMBER_IS_OUT_OF_LINE_MAGIC);
      PushBytes(aBuffer, &d, sizeof(d));
    }
    return true;
  }
  if (aValue.isInt32()) {
    int32_t i = aValue.toInt32();
    if (i > JS::ValueSummary::MAX_INLINE_INT ||
        i < JS::ValueSummary::MIN_INLINE_INT) {
      WriteHeader(aBuffer, JS::ValueType::Int32,
                  JS::ValueSummary::NUMBER_IS_OUT_OF_LINE_MAGIC);
      PushBytes(aBuffer, &i, sizeof(i));
    } else {
      WriteHeader(aBuffer, JS::ValueType::Int32,
                  uint8_t(i - JS::ValueSummary::MIN_INLINE_INT));
    }
    return true;
  }
  if (aValue.isBoolean()) {
    WriteHeader(aBuffer, JS::ValueType::Boolean, uint8_t(aValue.toBoolean()));
    return true;
  }
  if (aValue.isUndefined()) {
    WriteHeader(aBuffer, JS::ValueType::Undefined, 0);
    return true;
  }
  if (aValue.isNull()) {
    WriteHeader(aBuffer, JS::ValueType::Null, 0);
    return true;
  }
  if (aValue.isMagic()) {
    WriteHeader(aBuffer, JS::ValueType::Magic, 0);
    return true;
  }
  v8::HandleScope scope(aCx->isolate);
  v8::Local<v8::Value> value = ToV8(aCx, aValue);
  if (aValue.isString()) {
    WriteHeader(aBuffer, JS::ValueType::String, 0);
    WriteSmallString(aBuffer, Utf16Of(aCx->isolate, value));
    return true;
  }
  if (aValue.isBigInt()) {
    WriteHeader(aBuffer, JS::ValueType::BigInt, 0);
    WriteSmallString(aBuffer, Utf16Of(aCx->isolate, value));
    return true;
  }
  if (aValue.isSymbol()) {
    v8::Local<v8::Value> description =
        value.As<v8::Symbol>()->Description(aCx->isolate);
    if (!description->IsString()) {
      WriteHeader(aBuffer, JS::ValueType::Symbol,
                  JS::ValueSummary::SYMBOL_NO_DESCRIPTION);
      return true;
    }
    WriteHeader(aBuffer, JS::ValueType::Symbol, 0);
    WriteSmallString(aBuffer, Utf16Of(aCx->isolate, description));
    return true;
  }
  JS::Rooted<JSObject*> object(aCx, js::UncheckedUnwrap(&aValue.toObject()));
  TracerState& state = TracerOf(aCx);
  JS::ObjectSummary summary;
  if (state.customCallback && JS::GetClass(object)->isDOMClass()) {
    WriteHeader(aBuffer, JS::ValueType::Object, 0);
    summary.kind = JS::ObjectSummary::Kind::External;
    PushBytes(aBuffer, &summary, sizeof(summary));
    JS_TracerSummaryWriterImpl impl{aBuffer};
    JS_TracerSummaryWriter writer{&impl};
    return state.customCallback(aCx, object, aNested, &writer);
  }
  WriteHeader(aBuffer, JS::ValueType::Object, 0);
  summary.kind = JS::ObjectSummary::Kind::NotImplemented;
  PushBytes(aBuffer, &summary, sizeof(summary));
  return true;
}

}

}

using namespace vge::jsapi;

bool JS_DefineDebuggerObject(JSContext* cx, JS::HandleObject obj) {
  AutoV8 scope(cx);
  v8::Local<v8::Context> context = scope.context();
  v8::Local<v8::Function> constructor;
  if (!v8::Function::New(context, DebuggerConstruct, v8::Local<v8::Value>(), 0)
           .ToLocal(&constructor)) {
    return false;
  }
  constructor->SetName(V8String(cx->isolate, "Debugger"));
  v8::TryCatch tryCatch(cx->isolate);
  if (ToV8(cx, obj.get())
          ->DefineOwnProperty(context, V8String(cx->isolate, "Debugger"),
                              constructor, v8::DontEnum)
          .IsNothing()) {
    return ReportPending(cx, tryCatch);
  }
  return true;
}

bool JS::dbg::IsDebugger(JSObject& obj) {
  return JS::GetClass(js::UncheckedUnwrap(&obj)) == &DebuggerObjectClass;
}

bool JS::dbg::GetDebuggeeGlobals(JSContext* cx, JSObject& dbgObj,
                                 JS::MutableHandleObjectVector vector) {
  JSObject* debugger = js::UncheckedUnwrap(&dbgObj);
  for (auto& global : DebuggerDataOf(debugger)->debuggees) {
    if (!vector.append(global.get())) {
      JS_ReportOutOfMemory(cx);
      return false;
    }
  }
  return true;
}

bool JS::dbg::FireOnGarbageCollectionHookRequired(JSContext* cx) {
  v8::HandleScope scope(cx->isolate);
  for (JSObject* debugger : gDebuggers) {
    if (!GcHookOf(cx, debugger).IsEmpty()) {
      return true;
    }
  }
  return false;
}

JSObject* JS::dbg::GarbageCollectionEvent::toJSObject(JSContext* cx) const {
  v8::Isolate* isolate = cx->isolate;
  v8::EscapableHandleScope scope(isolate);
  v8::Local<v8::Context> context = cx->v8Context();
  v8::Local<v8::Object> event = v8::Object::New(isolate);
  event
      ->Set(context, V8String(isolate, "nonincrementalReason"),
            nonincrementalReason
                ? v8::Local<v8::Value>(V8String(isolate, nonincrementalReason))
                : v8::Local<v8::Value>(v8::Null(isolate)))
      .Check();
  v8::Local<v8::Array> slices = v8::Array::New(isolate);
  uint32_t index = 0;
  for (const Collection& collection : collections) {
    v8::Local<v8::Object> slice = v8::Object::New(isolate);
    slice
        ->Set(context, V8String(isolate, "startTimestamp"),
              v8::Number::New(isolate, (collection.startTimestamp -
                                        mozilla::TimeStamp::ProcessCreation())
                                           .ToMilliseconds()))
        .Check();
    slice
        ->Set(context, V8String(isolate, "endTimestamp"),
              v8::Number::New(isolate, (collection.endTimestamp -
                                        mozilla::TimeStamp::ProcessCreation())
                                           .ToMilliseconds()))
        .Check();
    slices->Set(context, index++, slice).Check();
  }
  event->Set(context, V8String(isolate, "collections"), slices).Check();
  event
      ->Set(context, V8String(isolate, "reason"),
            reason ? v8::Local<v8::Value>(V8String(isolate, reason))
                   : v8::Local<v8::Value>(v8::Null(isolate)))
      .Check();
  event
      ->Set(context, V8String(isolate, "majorGCNumber"),
            v8::Number::New(isolate, double(majorGCNumber_)))
      .Check();
  return ObjectFromV8(cx, scope.Escape(event));
}

bool JS::dbg::FireOnGarbageCollectionHook(
    JSContext* cx, JS::dbg::GarbageCollectionEvent::Ptr&& data) {
  AutoV8 scope(cx);
  std::vector<JSObject*> debuggers = gDebuggers;
  for (JSObject* debugger : debuggers) {
    v8::Local<v8::Function> hook = GcHookOf(cx, debugger);
    if (hook.IsEmpty()) {
      continue;
    }
    JSObject* event = data->toJSObject(cx);
    v8::Local<v8::Value> argument = ToV8(cx, event);
    v8::TryCatch tryCatch(cx->isolate);
    v8::Local<v8::Value> result;
    if (!hook->Call(scope.context(), ToV8(cx, debugger), 1, &argument)
             .ToLocal(&result)) {
      return ReportPending(cx, tryCatch);
    }
  }
  return true;
}

void JS::dbg::SetDebuggerMallocSizeOf(JSContext* cx,
                                      mozilla::MallocSizeOf mallocSizeOf) {
  ContextState(cx).debuggerMallocSizeOf = mallocSizeOf;
}

mozilla::MallocSizeOf JS::dbg::GetDebuggerMallocSizeOf(JSContext* cx) {
  return ContextState(cx).debuggerMallocSizeOf;
}

const char* JS_GetLastOOMStackTrace(JSContext* cx) {
  ScriptsContext& state = ContextState(cx);
  return state.oomStackTrace.empty() ? nullptr : state.oomStackTrace.c_str();
}

void JS_TracerEnterLabelLatin1(JSContext* cx, const char* label) {
  RecordLabel(cx, JS::ExecutionTrace::EventKind::LabelEnter,
              Latin1ToUtf8(label));
}

void JS_TracerEnterLabelTwoByte(JSContext* cx, const char16_t* label) {
  RecordLabel(cx, JS::ExecutionTrace::EventKind::LabelEnter,
              Utf8FromUtf16(label, std::char_traits<char16_t>::length(label)));
}

void JS_TracerLeaveLabelLatin1(JSContext* cx, const char* label) {
  RecordLabel(cx, JS::ExecutionTrace::EventKind::LabelLeave,
              Latin1ToUtf8(label));
}

void JS_TracerLeaveLabelTwoByte(JSContext* cx, const char16_t* label) {
  RecordLabel(cx, JS::ExecutionTrace::EventKind::LabelLeave,
              Utf8FromUtf16(label, std::char_traits<char16_t>::length(label)));
}

bool JS_TracerIsTracing(JSContext* cx) { return TracerOf(cx).tracing; }

bool JS_TracerBeginTracing(JSContext* cx) {
  TracerState& state = TracerOf(cx);
  std::lock_guard<std::mutex> lock(gTracerLock);
  state.tracing = true;
  state.thread = mozilla::baseprofiler::profiler_current_thread_id();
  return true;
}

bool JS_TracerEndTracing(JSContext* cx) {
  TracerState& state = TracerOf(cx);
  std::lock_guard<std::mutex> lock(gTracerLock);
  state.tracing = false;
  return true;
}

bool JS_TracerSnapshotTrace(JS::ExecutionTrace& trace) {
  std::lock_guard<std::mutex> lock(gTracerLock);
  for (auto& [cx, state] : gTracers) {
    if (state->events.empty() && !state->tracing) {
      continue;
    }
    if (!trace.contexts.emplaceBack()) {
      return false;
    }
    JS::ExecutionTrace::TracedJSContext& context = trace.contexts.back();
    context.id = state->thread;
    uint32_t version = JS::ValueSummary::VERSION;
    if (!context.valueBuffer.append(reinterpret_cast<const uint8_t*>(&version),
                                    sizeof(version)) ||
        !context.valueBuffer.append(state->values.data(),
                                    state->values.size())) {
      return false;
    }
    for (const LabelEvent& event : state->events) {
      JS::ExecutionTrace::TracedEvent traced;
      traced.kind = event.kind;
      traced.labelEvent.label = trace.stringBuffer.length();
      traced.time = event.time;
      if (!trace.stringBuffer.append(event.label.c_str(),
                                     event.label.size() + 1) ||
          !context.events.append(traced)) {
        return false;
      }
    }
    state->events.clear();
    state->values.clear();
  }
  return true;
}

void JS_TracerSummaryWriter::writeUint8(uint8_t val) {
  PushBytes(impl->buffer, &val, sizeof(val));
}

void JS_TracerSummaryWriter::writeUint16(uint16_t val) {
  PushBytes(impl->buffer, &val, sizeof(val));
}

void JS_TracerSummaryWriter::writeUint32(uint32_t val) {
  PushBytes(impl->buffer, &val, sizeof(val));
}

void JS_TracerSummaryWriter::writeUTF8String(const char* val) {
  WriteSmallCString(impl->buffer, val, JS::TracerStringEncoding::UTF8);
}

void JS_TracerSummaryWriter::writeTwoByteString(const char16_t* val) {
  WriteSmallCString(impl->buffer, val, JS::TracerStringEncoding::TwoByte);
}

bool JS_TracerSummaryWriter::writeValue(JSContext* cx,
                                        JS::Handle<JS::Value> val) {
  return WriteValueSummary(cx, impl->buffer, val, true);
}

void JS_SetCustomObjectSummaryCallback(JSContext* cx,
                                       CustomObjectSummaryCallback callback) {
  TracerOf(cx).customCallback = callback;
}
