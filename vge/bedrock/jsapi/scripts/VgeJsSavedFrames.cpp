#include "VgeJsScripts.h"

#include "js/Object.h"
#include "js/Principals.h"
#include "js/Wrapper.h"

namespace vge::jsapi {

bool RealmIsDebuggee(JS::Realm* aRealm);

namespace {

enum FrameSlot : uint32_t {
  kSource,
  kSourceId,
  kLine,
  kColumn,
  kDisplayName,
  kAsyncCause,
  kParent,
  kPrincipals,
  kMuted,
  kFrameSlotCount
};

void FinalizeSavedFrame(JS::GCContext* aGcx, JSObject* aObj) {
  JS::Value principals = JS::GetReservedSlot(aObj, kPrincipals);
  if (principals.isUndefined()) {
    return;
  }
  JSPrincipals* held = static_cast<JSPrincipals*>(principals.toPrivate());
  Bury([held] {
    if (JSContext* cx = CurrentContext()) {
      JS_DropPrincipals(cx, held);
    }
  });
}

const JSClassOps kSavedFrameOps = {
    nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, FinalizeSavedFrame, nullptr, nullptr, nullptr,
};

struct FrameFields {
  std::u16string source;
  uint32_t sourceId = 0;
  uint32_t line = 0;
  uint32_t column = 1;
  std::u16string displayName;
  bool hasDisplayName = false;
  const char16_t* asyncCause = nullptr;
  std::u16string asyncCauseText;
  JSPrincipals* principals = nullptr;
  bool muted = false;
};

JSObject* FrameParent(JSObject* aFrame) {
  JS::Value parent = JS::GetReservedSlot(aFrame, kParent);
  return parent.isObject() ? &parent.toObject() : nullptr;
}

bool FrameHasAsyncCause(JSObject* aFrame) {
  return JS::GetReservedSlot(aFrame, kAsyncCause).isString();
}

JSPrincipals* FramePrincipals(JSObject* aFrame) {
  JS::Value principals = JS::GetReservedSlot(aFrame, kPrincipals);
  return principals.isUndefined()
             ? nullptr
             : static_cast<JSPrincipals*>(principals.toPrivate());
}

v8::Local<v8::Value> SlotToV8(JSContext* aCx, JSObject* aFrame, uint32_t aSlot) {
  return ToV8(aCx, JS::GetReservedSlot(aFrame, aSlot));
}

JSObject* NewFrame(JSContext* aCx, const FrameFields& aFields,
                   JSObject* aParent) {
  JS::Rooted<JSObject*> parent(aCx, aParent);
  JS::Rooted<JSObject*> frame(
      aCx, NewClassObject(aCx, &SavedFrameClass, nullptr));
  if (!frame) {
    return nullptr;
  }
  v8::Isolate* isolate = aCx->isolate;
  JS::Rooted<JSString*> source(aCx, NewJSStringUtf16(aCx, aFields.source));
  JS::SetReservedSlot(frame, kSource, JS::StringValue(source));
  JS::SetReservedSlot(frame, kSourceId, JS::Int32Value(int32_t(aFields.sourceId)));
  JS::SetReservedSlot(frame, kLine, JS::Int32Value(int32_t(aFields.line)));
  JS::SetReservedSlot(frame, kColumn, JS::Int32Value(int32_t(aFields.column)));
  if (aFields.hasDisplayName) {
    JSString* name = AtomFromV8(
        aCx, V8String(isolate, aFields.displayName.data(),
                      aFields.displayName.size()));
    JS::SetReservedSlot(frame, kDisplayName, JS::StringValue(name));
  } else {
    JS::SetReservedSlot(frame, kDisplayName, JS::NullValue());
  }
  if (aFields.asyncCause) {
    JSString* cause = AtomFromV8(aCx, V8String(isolate, aFields.asyncCause,
                                               std::char_traits<char16_t>::length(
                                                   aFields.asyncCause)));
    JS::SetReservedSlot(frame, kAsyncCause, JS::StringValue(cause));
  } else {
    JS::SetReservedSlot(frame, kAsyncCause, JS::NullValue());
  }
  JS::SetReservedSlot(frame, kParent,
                      parent ? JS::ObjectValue(*parent) : JS::NullValue());
  if (aFields.principals) {
    JS_HoldPrincipals(aFields.principals);
    JS::SetReservedSlot(frame, kPrincipals,
                        JS::PrivateValue(aFields.principals));
  }
  JS::SetReservedSlot(frame, kMuted, JS::BooleanValue(aFields.muted));
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> context = aCx->v8Context();
  v8::Local<v8::Object> object = ToV8(aCx, frame);
  auto define = [&](const char* aName, v8::Local<v8::Value> aValue) {
    object
        ->DefineOwnProperty(context, V8String(isolate, aName), aValue,
                            v8::PropertyAttribute(v8::ReadOnly | v8::DontEnum))
        .Check();
  };
  define("source", SlotToV8(aCx, frame, kSource));
  define("sourceId", SlotToV8(aCx, frame, kSourceId));
  define("line", SlotToV8(aCx, frame, kLine));
  define("column", SlotToV8(aCx, frame, kColumn));
  define("functionDisplayName", SlotToV8(aCx, frame, kDisplayName));
  define("asyncCause", SlotToV8(aCx, frame, kAsyncCause));
  bool asyncParent = parent && FrameHasAsyncCause(parent);
  define("parent", parent && !asyncParent ? v8::Local<v8::Value>(ToV8(aCx, parent.get()))
                                          : v8::Local<v8::Value>(v8::Null(isolate)));
  define("asyncParent", asyncParent ? v8::Local<v8::Value>(ToV8(aCx, parent.get()))
                                    : v8::Local<v8::Value>(v8::Null(isolate)));
  v8::Local<v8::Function> toString;
  if (v8::Function::New(
          context,
          [](const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
            JSContext* cx = CurrentContext();
            if (!aInfo.This()->IsObject()) {
              return;
            }
            JS::Rooted<JSObject*> self(cx, ObjectFromV8(cx, aInfo.This()));
            JS::Rooted<JSString*> text(cx);
            if (!IsSavedFrameObject(js::UncheckedUnwrap(self)) ||
                !JS::BuildStackString(cx, cx->realm() ? cx->realm()->principals
                                                      : nullptr,
                                      self, &text)) {
              return;
            }
            aInfo.GetReturnValue().Set(ToV8(cx, text.get()));
          })
          .ToLocal(&toString)) {
    define("toString", toString);
  }
  return frame;
}

bool Subsumes(JSContext* aCx, JSPrincipals* aPrincipals, JSObject* aFrame) {
  const JSSecurityCallbacks* callbacks = aCx->runtime->securityCallbacks;
  if (!callbacks || !callbacks->subsumes) {
    return true;
  }
  return callbacks->subsumes(aPrincipals, FramePrincipals(aFrame));
}

JSObject* FirstSubsumed(JSContext* aCx, JSPrincipals* aPrincipals,
                        JSObject* aFrame, bool* aSkippedAsync) {
  *aSkippedAsync = false;
  for (JSObject* frame = aFrame; frame; frame = FrameParent(frame)) {
    if (Subsumes(aCx, aPrincipals, frame)) {
      return frame;
    }
    if (FrameHasAsyncCause(frame)) {
      *aSkippedAsync = true;
    }
  }
  return nullptr;
}

JSObject* UnwrapFrame(JSContext* aCx, JSPrincipals* aPrincipals,
                      JS::HandleObject aObj, bool* aSkippedAsync) {
  *aSkippedAsync = false;
  if (!aObj) {
    return nullptr;
  }
  JSObject* frame = js::UncheckedUnwrap(aObj);
  if (!IsSavedFrameObject(frame)) {
    return nullptr;
  }
  return FirstSubsumed(aCx, aPrincipals, frame, aSkippedAsync);
}

FrameFields FieldsOfFrame(JSObject* aFrame) {
  FrameFields fields;
  JSContext* cx = CurrentContext();
  v8::Isolate* isolate = cx->isolate;
  v8::HandleScope scope(isolate);
  fields.source = Utf16Of(isolate, SlotToV8(cx, aFrame, kSource));
  fields.sourceId = uint32_t(JS::GetReservedSlot(aFrame, kSourceId).toInt32());
  fields.line = uint32_t(JS::GetReservedSlot(aFrame, kLine).toInt32());
  fields.column = uint32_t(JS::GetReservedSlot(aFrame, kColumn).toInt32());
  if (JS::GetReservedSlot(aFrame, kDisplayName).isString()) {
    fields.hasDisplayName = true;
    fields.displayName = Utf16Of(isolate, SlotToV8(cx, aFrame, kDisplayName));
  }
  fields.principals = FramePrincipals(aFrame);
  fields.muted = JS::GetReservedSlot(aFrame, kMuted).toBoolean();
  return fields;
}

bool FramesMatch(const FrameFields& aA, const FrameFields& aB) {
  return aA.source == aB.source && aA.line == aB.line &&
         aA.column == aB.column;
}

FrameFields FieldsOfV8Frame(JSContext* aCx, v8::Local<v8::StackFrame> aFrame) {
  FrameFields fields;
  v8::Isolate* isolate = aCx->isolate;
  fields.source = Utf16Of(isolate, aFrame->GetScriptNameOrSourceURL());
  fields.sourceId = uint32_t(aFrame->GetScriptId());
  fields.line = uint32_t(aFrame->GetLineNumber());
  fields.column = uint32_t(std::max(aFrame->GetColumn(), 1));
  v8::Local<v8::String> name = aFrame->GetFunctionName();
  if (!name.IsEmpty() && name->Length() > 0) {
    fields.hasDisplayName = true;
    fields.displayName = Utf16Of(isolate, name);
  }
  ScriptData* data = ScriptDataByV8Id(aFrame->GetScriptId());
  JS::Realm* realm = data && data->realm ? data->realm : aCx->realm();
  fields.principals = realm ? realm->principals : nullptr;
  fields.muted = data && data->mutedErrors;
  return fields;
}

JSObject* CopyAsyncChain(JSContext* aCx, JSObject* aAsyncStack,
                         const char* aCause) {
  FrameFields fields = FieldsOfFrame(aAsyncStack);
  std::u16string cause = Utf16FromUtf8(aCause, strlen(aCause));
  fields.asyncCause = cause.c_str();
  return NewFrame(aCx, fields, FrameParent(aAsyncStack));
}

}

const JSClass SavedFrameClass = {
    "SavedFrame",
    JSCLASS_HAS_RESERVED_SLOTS(kFrameSlotCount) | JSCLASS_FOREGROUND_FINALIZE,
    &kSavedFrameOps,
};

bool IsSavedFrameObject(JSObject* aObj) {
  return aObj && JS::GetClass(aObj) == &SavedFrameClass;
}

JSObject* SavedFrameFromTrace(JSContext* aCx, v8::Local<v8::StackTrace> aTrace,
                              size_t aSkip, size_t aMaxFrames,
                              JSObject* aAsyncParent, const char* aAsyncCause,
                              JSPrincipals* aFirstSubsumed,
                              bool aUseSubsumed) {
  if (aTrace.IsEmpty()) {
    return nullptr;
  }
  v8::Isolate* isolate = aCx->isolate;
  std::vector<FrameFields> frames;
  int count = aTrace->GetFrameCount();
  bool foundSubsumed = !aUseSubsumed;
  for (int i = int(aSkip); i < count; i++) {
    v8::Local<v8::StackFrame> frame = aTrace->GetFrame(isolate, uint32_t(i));
    if (!frame->IsUserJavaScript() && !frame->IsWasm()) {
      continue;
    }
    FrameFields fields = FieldsOfV8Frame(aCx, frame);
    if (!foundSubsumed) {
      const JSSecurityCallbacks* callbacks = aCx->runtime->securityCallbacks;
      if (callbacks && callbacks->subsumes &&
          !callbacks->subsumes(aFirstSubsumed, fields.principals)) {
        continue;
      }
      foundSubsumed = true;
    }
    frames.push_back(std::move(fields));
    if (aMaxFrames && frames.size() >= aMaxFrames) {
      break;
    }
  }
  JS::Rooted<JSObject*> parent(aCx);
  if (aAsyncParent && aAsyncCause &&
      (!aMaxFrames || frames.size() < aMaxFrames)) {
    parent = CopyAsyncChain(aCx, aAsyncParent, aAsyncCause);
  }
  for (size_t i = frames.size(); i > 0;) {
    JSObject* frame = NewFrame(aCx, frames[--i], parent);
    if (!frame) {
      return nullptr;
    }
    parent = frame;
  }
  return parent;
}

bool CaptureStackObject(JSContext* aCx, size_t aMaxFrames,
                        JS::MutableHandleObject aStack) {
  v8::HandleScope scope(aCx->isolate);
  int limit = aMaxFrames ? int(aMaxFrames) : 4096;
  v8::Local<v8::StackTrace> trace = v8::StackTrace::CurrentStackTrace(
      aCx->isolate, limit, v8::StackTrace::kDetailed);
  ScriptsContext& state = ContextState(aCx);
  JSObject* asyncStack =
      state.options.asyncStack() ? state.asyncStack : nullptr;
  aStack.set(SavedFrameFromTrace(aCx, trace, 0, aMaxFrames, asyncStack,
                                 state.asyncCause, nullptr, false));
  return true;
}

}

using namespace vge::jsapi;

JS::FirstSubsumedFrame::FirstSubsumedFrame(JSContext* cx,
                                           bool ignoreSelfHostedFrames)
    : JS::FirstSubsumedFrame(cx, cx->realm() ? cx->realm()->principals : nullptr,
                             ignoreSelfHostedFrames) {}

bool JS::CaptureCurrentStack(JSContext* cx, JS::MutableHandleObject stackp,
                             JS::StackCapture&& capture,
                             JS::HandleObject startAfter) {
  v8::HandleScope scope(cx->isolate);
  size_t maxFrames = 0;
  JSPrincipals* principals = nullptr;
  bool useSubsumed = false;
  if (capture.is<JS::MaxFrames>()) {
    maxFrames = capture.as<JS::MaxFrames>().maxFrames;
  } else if (capture.is<JS::FirstSubsumedFrame>()) {
    principals = capture.as<JS::FirstSubsumedFrame>().principals;
    useSubsumed = true;
  }
  v8::Local<v8::StackTrace> trace =
      v8::StackTrace::CurrentStackTrace(cx->isolate, 4096,
                                        v8::StackTrace::kDetailed);
  size_t skip = 0;
  if (startAfter) {
    JSObject* after = js::UncheckedUnwrap(startAfter);
    if (IsSavedFrameObject(after)) {
      FrameFields target = FieldsOfFrame(after);
      for (int i = 0; i < trace->GetFrameCount(); i++) {
        FrameFields fields =
            FieldsOfV8Frame(cx, trace->GetFrame(cx->isolate, uint32_t(i)));
        if (FramesMatch(fields, target)) {
          skip = size_t(i) + 1;
          break;
        }
      }
    }
  }
  ScriptsContext& state = ContextState(cx);
  JSObject* asyncStack = state.options.asyncStack() ? state.asyncStack : nullptr;
  stackp.set(SavedFrameFromTrace(cx, trace, skip, maxFrames, asyncStack,
                                 state.asyncCause, principals, useSubsumed));
  return true;
}

bool JS::IsAsyncStackCaptureEnabledForRealm(JSContext* cx) {
  ScriptsContext& state = ContextState(cx);
  if (!state.options.asyncStack()) {
    return false;
  }
  return !state.options.asyncStackCaptureDebuggeeOnly() ||
         RealmIsDebuggee(cx->realm());
}

JS::AutoSetAsyncStackForNewCalls::AutoSetAsyncStackForNewCalls(
    JSContext* cx, JSObject* stack, const char* asyncCause,
    JS::AutoSetAsyncStackForNewCalls::AsyncCallKind kind)
    : cx(cx),
      oldAsyncStack(cx, ContextState(cx).asyncStack),
      oldAsyncCause(ContextState(cx).asyncCause),
      oldAsyncCallIsExplicit(ContextState(cx).asyncCallIsExplicit) {
  ScriptsContext& state = ContextState(cx);
  if (!state.options.asyncStack()) {
    return;
  }
  JSObject* unwrapped = js::UncheckedUnwrap(stack);
  state.asyncStack = unwrapped;
  {
    v8::HandleScope scope(cx->isolate);
    state.asyncStackKeep.Reset(cx->isolate, ToV8(cx, unwrapped));
  }
  state.asyncCause = asyncCause;
  state.asyncCallIsExplicit = kind == AsyncCallKind::EXPLICIT;
}

JS::AutoSetAsyncStackForNewCalls::~AutoSetAsyncStackForNewCalls() {
  ScriptsContext& state = ContextState(cx);
  state.asyncCause = oldAsyncCause;
  state.asyncStack = oldAsyncStack;
  if (oldAsyncStack) {
    v8::HandleScope scope(cx->isolate);
    state.asyncStackKeep.Reset(cx->isolate, ToV8(cx, oldAsyncStack.get()));
  } else {
    state.asyncStackKeep.Reset();
  }
  state.asyncCallIsExplicit = oldAsyncCallIsExplicit;
}

JSObject* js::GetFirstSubsumedSavedFrame(JSContext* cx,
                                         JSPrincipals* principals,
                                         JS::Handle<JSObject*> savedFrame,
                                         JS::SavedFrameSelfHosted selfHosted) {
  if (!savedFrame) {
    return nullptr;
  }
  const JSSecurityCallbacks* callbacks = cx->runtime->securityCallbacks;
  if (!callbacks || !callbacks->subsumes) {
    return nullptr;
  }
  JSObject* frame = js::UncheckedUnwrap(savedFrame);
  for (; frame; frame = FrameParent(frame)) {
    if (callbacks->subsumes(principals, FramePrincipals(frame))) {
      return frame;
    }
  }
  return nullptr;
}

JS::SavedFrameResult JS::GetSavedFrameSource(JSContext* cx,
                                             JSPrincipals* principals,
                                             Handle<JSObject*> savedFrame,
                                             MutableHandle<JSString*> sourcep,
                                             SavedFrameSelfHosted selfHosted) {
  bool skippedAsync;
  JSObject* frame = UnwrapFrame(cx, principals, savedFrame, &skippedAsync);
  if (!frame) {
    sourcep.set(NewJSStringUtf8(cx, ""));
    return SavedFrameResult::AccessDenied;
  }
  sourcep.set(JS::GetReservedSlot(frame, kSource).toString());
  return SavedFrameResult::Ok;
}

JS::SavedFrameResult JS::GetSavedFrameSourceId(JSContext* cx,
                                               JSPrincipals* principals,
                                               Handle<JSObject*> savedFrame,
                                               uint32_t* sourceIdp,
                                               SavedFrameSelfHosted selfHosted) {
  bool skippedAsync;
  JSObject* frame = UnwrapFrame(cx, principals, savedFrame, &skippedAsync);
  if (!frame) {
    *sourceIdp = 0;
    return SavedFrameResult::AccessDenied;
  }
  *sourceIdp = uint32_t(JS::GetReservedSlot(frame, kSourceId).toInt32());
  return SavedFrameResult::Ok;
}

JS::SavedFrameResult JS::GetSavedFrameLine(JSContext* cx,
                                           JSPrincipals* principals,
                                           Handle<JSObject*> savedFrame,
                                           uint32_t* linep,
                                           SavedFrameSelfHosted selfHosted) {
  bool skippedAsync;
  JSObject* frame = UnwrapFrame(cx, principals, savedFrame, &skippedAsync);
  if (!frame) {
    *linep = 0;
    return SavedFrameResult::AccessDenied;
  }
  *linep = uint32_t(JS::GetReservedSlot(frame, kLine).toInt32());
  return SavedFrameResult::Ok;
}

JS::SavedFrameResult JS::GetSavedFrameColumn(
    JSContext* cx, JSPrincipals* principals, Handle<JSObject*> savedFrame,
    JS::TaggedColumnNumberOneOrigin* columnp,
    SavedFrameSelfHosted selfHosted) {
  bool skippedAsync;
  JSObject* frame = UnwrapFrame(cx, principals, savedFrame, &skippedAsync);
  if (!frame) {
    *columnp = JS::TaggedColumnNumberOneOrigin();
    return SavedFrameResult::AccessDenied;
  }
  *columnp = JS::TaggedColumnNumberOneOrigin(JS::LimitedColumnNumberOneOrigin(
      uint32_t(JS::GetReservedSlot(frame, kColumn).toInt32())));
  return SavedFrameResult::Ok;
}

JS::SavedFrameResult JS::GetSavedFrameFunctionDisplayName(
    JSContext* cx, JSPrincipals* principals, Handle<JSObject*> savedFrame,
    MutableHandle<JSString*> namep, SavedFrameSelfHosted selfHosted) {
  bool skippedAsync;
  JSObject* frame = UnwrapFrame(cx, principals, savedFrame, &skippedAsync);
  if (!frame) {
    namep.set(nullptr);
    return SavedFrameResult::AccessDenied;
  }
  JS::Value name = JS::GetReservedSlot(frame, kDisplayName);
  namep.set(name.isString() ? name.toString() : nullptr);
  return SavedFrameResult::Ok;
}

JS::SavedFrameResult JS::GetSavedFrameAsyncCause(
    JSContext* cx, JSPrincipals* principals, Handle<JSObject*> savedFrame,
    MutableHandle<JSString*> asyncCausep, SavedFrameSelfHosted unused) {
  bool skippedAsync;
  JSObject* frame = UnwrapFrame(cx, principals, savedFrame, &skippedAsync);
  if (!frame) {
    asyncCausep.set(nullptr);
    return SavedFrameResult::AccessDenied;
  }
  JS::Value cause = JS::GetReservedSlot(frame, kAsyncCause);
  asyncCausep.set(cause.isString() ? cause.toString() : nullptr);
  if (!asyncCausep && skippedAsync) {
    v8::HandleScope scope(cx->isolate);
    asyncCausep.set(AtomFromV8(cx, V8String(cx->isolate, "Async")));
  }
  return SavedFrameResult::Ok;
}

JS::SavedFrameResult JS::GetSavedFrameAsyncParent(
    JSContext* cx, JSPrincipals* principals, Handle<JSObject*> savedFrame,
    MutableHandle<JSObject*> asyncParentp, SavedFrameSelfHosted selfHosted) {
  bool skippedAsync;
  JSObject* frame = UnwrapFrame(cx, principals, savedFrame, &skippedAsync);
  if (!frame) {
    asyncParentp.set(nullptr);
    return SavedFrameResult::AccessDenied;
  }
  JSObject* parent = FrameParent(frame);
  JSObject* subsumedParent =
      parent ? FirstSubsumed(cx, principals, parent, &skippedAsync) : nullptr;
  if (subsumedParent &&
      (FrameHasAsyncCause(subsumedParent) || skippedAsync)) {
    asyncParentp.set(parent);
  } else {
    asyncParentp.set(nullptr);
  }
  return SavedFrameResult::Ok;
}

JS::SavedFrameResult JS::GetSavedFrameParent(JSContext* cx,
                                             JSPrincipals* principals,
                                             Handle<JSObject*> savedFrame,
                                             MutableHandle<JSObject*> parentp,
                                             SavedFrameSelfHosted selfHosted) {
  bool skippedAsync;
  JSObject* frame = UnwrapFrame(cx, principals, savedFrame, &skippedAsync);
  if (!frame) {
    parentp.set(nullptr);
    return SavedFrameResult::AccessDenied;
  }
  JSObject* parent = FrameParent(frame);
  JSObject* subsumedParent =
      parent ? FirstSubsumed(cx, principals, parent, &skippedAsync) : nullptr;
  if (subsumedParent &&
      !(FrameHasAsyncCause(subsumedParent) || skippedAsync)) {
    parentp.set(parent);
  } else {
    parentp.set(nullptr);
  }
  return SavedFrameResult::Ok;
}

bool JS::BuildStackString(JSContext* cx, JSPrincipals* principals,
                          HandleObject stack, MutableHandleString stringp,
                          size_t indent, js::StackFormat stackFormat) {
  bool skippedAsync;
  JSObject* frame = UnwrapFrame(cx, principals, stack, &skippedAsync);
  if (!frame) {
    stringp.set(NewJSStringUtf8(cx, ""));
    return true;
  }
  if (stackFormat == js::StackFormat::Default) {
    stackFormat = js::StackFormat::SpiderMonkey;
  }
  v8::HandleScope scope(cx->isolate);
  std::u16string out;
  while (frame) {
    FrameFields fields = FieldsOfFrame(frame);
    JSObject* parent = FrameParent(frame);
    bool skippedNextAsync = false;
    JSObject* next =
        parent ? FirstSubsumed(cx, principals, parent, &skippedNextAsync)
               : nullptr;
    std::string lineText = std::to_string(fields.line);
    std::string columnText = std::to_string(fields.column);
    std::u16string lineChars(lineText.begin(), lineText.end());
    std::u16string columnChars(columnText.begin(), columnText.end());
    if (stackFormat == js::StackFormat::SpiderMonkey) {
      out.append(indent, u' ');
      std::u16string cause;
      if (FrameHasAsyncCause(frame)) {
        cause = Utf16Of(cx->isolate, ToV8(cx, JS::GetReservedSlot(frame, kAsyncCause)));
      } else if (skippedAsync) {
        cause = u"Async";
      }
      if (!cause.empty()) {
        out += cause;
        out += u'*';
      }
      if (fields.hasDisplayName) {
        out += fields.displayName;
      }
      out += u'@';
      out += fields.source;
      out += u':';
      out += lineChars;
      out += u':';
      out += columnChars;
      out += u'\n';
    } else {
      out.append(indent + 4, u' ');
      out += u"at ";
      if (fields.hasDisplayName) {
        out += fields.displayName;
        out += u" (";
      }
      out += fields.source;
      out += u':';
      out += lineChars;
      out += u':';
      out += columnChars;
      if (fields.hasDisplayName) {
        out += u')';
      }
      if (next) {
        out += u'\n';
      }
    }
    frame = next;
    skippedAsync = skippedNextAsync;
  }
  stringp.set(NewJSStringUtf16(cx, out));
  return true;
}

JSObject* JS::ConvertSavedFrameToPlainObject(
    JSContext* cx, JS::HandleObject savedFrameArg,
    JS::SavedFrameSelfHosted selfHosted) {
  AutoV8 scope(cx);
  v8::Isolate* isolate = cx->isolate;
  v8::Local<v8::Context> context = scope.context();
  v8::Local<v8::Object> base = v8::Object::New(isolate);
  v8::Local<v8::Object> last = base;
  JSObject* frame = js::UncheckedUnwrap(savedFrameArg);
  while (frame && IsSavedFrameObject(frame)) {
    v8::Local<v8::Object> source = ToV8(cx, frame);
    for (const char* property : {"source", "sourceId", "line", "column",
                                 "functionDisplayName", "asyncCause"}) {
      v8::Local<v8::Value> value;
      if (!source->Get(context, V8String(isolate, property)).ToLocal(&value)) {
        return nullptr;
      }
      last->Set(context, V8String(isolate, property), value).Check();
    }
    JSObject* parent = FrameParent(frame);
    if (!parent) {
      break;
    }
    v8::Local<v8::Object> next = v8::Object::New(isolate);
    last->Set(context,
              V8String(isolate, FrameHasAsyncCause(parent) ? "asyncParent"
                                                           : "parent"),
              next)
        .Check();
    last = next;
    frame = parent;
  }
  return ObjectFromV8(cx, base);
}
