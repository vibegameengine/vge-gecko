#include "vge/bedrock/VgeV8Events.h"

#include <stdio.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mozilla/EventListenerManager.h"
#include "mozilla/PresShell.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/DocumentInlines.h"
#include "mozilla/dom/Event.h"
#include "mozilla/dom/EventTarget.h"
#include "mozilla/dom/Performance.h"
#include "nsGlobalWindowInner.h"
#include "nsIDOMEventListener.h"
#include "nsINamed.h"
#include "nsITimer.h"
#include "nsPresContext.h"
#include "nsRefreshDriver.h"
#include "nsRefreshObservers.h"
#include "nsThreadUtils.h"
#include "vge/bedrock/VgeBedrockV8.h"
#include "vge/bedrock/VgeV8BindingSupport.h"

using namespace mozilla;
using namespace mozilla::dom;

namespace vge::bedrock::v8b {

namespace {

void ReportException(v8::Isolate* aIsolate, v8::Local<v8::Context> aContext,
                     v8::TryCatch& aTryCatch, const char* aWhere) {
  v8::String::Utf8Value error(aIsolate, aTryCatch.Exception());
  int line = 0;
  v8::Local<v8::Message> message = aTryCatch.Message();
  nsAutoCString resource;
  if (!message.IsEmpty()) {
    line = message->GetLineNumber(aContext).FromMaybe(0);
    v8::String::Utf8Value name(aIsolate, message->GetScriptResourceName());
    if (*name) {
      resource.Assign(*name);
    }
  }
  printf("Uncaught %s in %s (%s:%d)\n", *error ? *error : "<exception>",
         aWhere, resource.get(), line);
}

nsGlobalWindowInner* ReceiverWindow(
    const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  return static_cast<nsGlobalWindowInner*>(
      Unwrap(aInfo.GetIsolate(), aInfo.This(), WindowProto()));
}

class V8EventListener;

std::unordered_set<V8EventListener*>& LiveListeners() {
  static std::unordered_set<V8EventListener*> sListeners;
  return sListeners;
}

class V8EventListener final : public nsIDOMEventListener {
 public:
  NS_DECL_ISUPPORTS

  V8EventListener(v8::Isolate* aIsolate, v8::Local<v8::Object> aCallable,
                  bool aIsHandler)
      : mCallable(aIsolate, aCallable),
        mContext(aIsolate, aIsolate->GetCurrentContext()),
        mIsHandler(aIsHandler) {
    LiveListeners().insert(this);
  }

  NS_IMETHOD HandleEvent(Event* aEvent) override {
    if (mCallable.IsEmpty()) {
      return NS_OK;
    }
    RefPtr<V8EventListener> kungFuDeathGrip(this);
    v8::Isolate* isolate = V8Engine::Isolate();
    v8::HandleScope handleScope(isolate);
    v8::Local<v8::Context> context = mContext.Get(isolate);
    v8::Context::Scope contextScope(context);
    v8::Local<v8::Value> event = Wrap(isolate, aEvent);
    v8::Local<v8::Value> thisValue =
        WrapEventTarget(isolate, aEvent->GetCurrentTarget());
    v8::Local<v8::Object> callable = mCallable.Get(isolate);
    v8::TryCatch tryCatch(isolate);
    v8::Local<v8::Value> result;
    bool ok = false;
    if (callable->IsFunction()) {
      ok = callable.As<v8::Function>()
               ->Call(context, thisValue, 1, &event)
               .ToLocal(&result);
    } else {
      v8::Local<v8::Value> handleEvent;
      if (callable
              ->Get(context,
                    v8::String::NewFromUtf8Literal(isolate, "handleEvent"))
              .ToLocal(&handleEvent) &&
          handleEvent->IsFunction()) {
        ok = handleEvent.As<v8::Function>()
                 ->Call(context, callable, 1, &event)
                 .ToLocal(&result);
      } else if (!tryCatch.HasCaught()) {
        ThrowTypeError(isolate, "listener has no handleEvent method");
      }
    }
    if (!ok) {
      ReportException(isolate, context, tryCatch, "event listener");
      return NS_OK;
    }
    if (mIsHandler && result->IsFalse()) {
      aEvent->PreventDefault();
    }
    return NS_OK;
  }

  bool Matches(v8::Isolate* aIsolate, v8::Local<v8::Value> aCallable) const {
    return !mCallable.IsEmpty() && mCallable.Get(aIsolate)->StrictEquals(aCallable);
  }

  v8::Local<v8::Value> Callable(v8::Isolate* aIsolate) const {
    return mCallable.IsEmpty() ? v8::Local<v8::Value>(v8::Null(aIsolate))
                               : v8::Local<v8::Value>(mCallable.Get(aIsolate));
  }

  void Disconnect() {
    mCallable.Reset();
    mContext.Reset();
  }

 private:
  ~V8EventListener() {
    LiveListeners().erase(this);
    Disconnect();
  }

  v8::Global<v8::Object> mCallable;
  v8::Global<v8::Context> mContext;
  bool mIsHandler;
};

NS_IMPL_ISUPPORTS(V8EventListener, nsIDOMEventListener)

struct ListenerEntry {
  EventTarget* mTarget;
  nsString mType;
  bool mCapture;
  RefPtr<V8EventListener> mListener;
  bool mIsHandler;
};

std::vector<ListenerEntry>& Listeners() {
  static std::vector<ListenerEntry> sListeners;
  return sListeners;
}

bool ReadCapture(v8::Isolate* aIsolate, v8::Local<v8::Value> aOptions,
                 EventListenerFlags* aFlags) {
  if (aOptions.IsEmpty() || aOptions->IsUndefined()) {
    return true;
  }
  if (!aOptions->IsObject()) {
    aFlags->mCapture = ToBoolean(aIsolate, aOptions);
    return true;
  }
  v8::Local<v8::Context> context = aIsolate->GetCurrentContext();
  v8::Local<v8::Object> options = aOptions.As<v8::Object>();
  auto read = [&](const char* aName, bool* aOut) {
    v8::Local<v8::Value> value;
    if (!options->Get(context, v8::String::NewFromUtf8(aIsolate, aName)
                                   .ToLocalChecked())
             .ToLocal(&value)) {
      return false;
    }
    if (!value->IsUndefined()) {
      *aOut = ToBoolean(aIsolate, value);
    }
    return true;
  };
  bool capture = false;
  bool once = false;
  bool passive = false;
  if (!read("capture", &capture) || !read("once", &once) ||
      !read("passive", &passive)) {
    return false;
  }
  aFlags->mCapture = capture;
  aFlags->mOnce = once;
  aFlags->mPassive = passive;
  return true;
}

EventTarget* ReceiverTarget(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  static int sProto = -1;
  if (sProto < 0) {
    for (int i = 0; i < gProtoCount; ++i) {
      if (!strcmp(gProtoInfos[i].mName, "EventTarget")) {
        sProto = i;
      }
    }
  }
  return static_cast<EventTarget*>(
      Unwrap(aInfo.GetIsolate(), aInfo.This(), sProto));
}

class V8Timer final : public nsITimerCallback, public nsINamed {
 public:
  NS_DECL_ISUPPORTS

  V8Timer(v8::Isolate* aIsolate, uint32_t aId, v8::Local<v8::Function> aFunction,
          std::vector<v8::Local<v8::Value>>& aArgs, bool aRepeat)
      : mId(aId),
        mFunction(aIsolate, aFunction),
        mContext(aIsolate, aIsolate->GetCurrentContext()),
        mRepeat(aRepeat) {
    for (auto& arg : aArgs) {
      mArgs.emplace_back(aIsolate, arg);
    }
  }

  NS_IMETHOD Notify(nsITimer* aTimer) override;
  NS_IMETHOD GetName(nsACString& aName) override {
    aName.AssignLiteral("VgeBedrockV8Timer");
    return NS_OK;
  }

  void Start(uint32_t aDelay) {
    NS_NewTimerWithCallback(
        getter_AddRefs(mTimer), this, aDelay,
        mRepeat ? nsITimer::TYPE_REPEATING_SLACK : nsITimer::TYPE_ONE_SHOT,
        GetMainThreadSerialEventTarget());
  }

  void Cancel() {
    if (mTimer) {
      mTimer->Cancel();
      mTimer = nullptr;
    }
    mFunction.Reset();
    mContext.Reset();
    mArgs.clear();
  }

 private:
  ~V8Timer() { Cancel(); }

  uint32_t mId;
  v8::Global<v8::Function> mFunction;
  v8::Global<v8::Context> mContext;
  std::vector<v8::Global<v8::Value>> mArgs;
  bool mRepeat;
  nsCOMPtr<nsITimer> mTimer;
};

NS_IMPL_ISUPPORTS(V8Timer, nsITimerCallback, nsINamed)

std::unordered_map<uint32_t, RefPtr<V8Timer>>& Timers() {
  static std::unordered_map<uint32_t, RefPtr<V8Timer>> sTimers;
  return sTimers;
}

uint32_t sNextTimerId = 1;

NS_IMETHODIMP V8Timer::Notify(nsITimer* aTimer) {
  if (mFunction.IsEmpty()) {
    return NS_OK;
  }
  RefPtr<V8Timer> kungFuDeathGrip(this);
  if (!mRepeat) {
    Timers().erase(mId);
  }
  v8::Isolate* isolate = V8Engine::Isolate();
  v8::HandleScope handleScope(isolate);
  v8::Local<v8::Context> context = mContext.Get(isolate);
  v8::Context::Scope contextScope(context);
  std::vector<v8::Local<v8::Value>> args;
  for (auto& arg : mArgs) {
    args.push_back(arg.Get(isolate));
  }
  v8::TryCatch tryCatch(isolate);
  v8::Local<v8::Value> result;
  if (!mFunction.Get(isolate)
           ->Call(context, context->Global(), int(args.size()), args.data())
           .ToLocal(&result)) {
    ReportException(isolate, context, tryCatch, "timer");
  }
  if (!mRepeat) {
    Cancel();
  }
  return NS_OK;
}

void StartTimer(const v8::FunctionCallbackInfo<v8::Value>& aInfo,
                bool aRepeat) {
  v8::Isolate* isolate = aInfo.GetIsolate();
  if (aInfo.Length() < 1 || !aInfo[0]->IsFunction()) {
    ThrowTypeError(isolate, "timer handler must be a function");
    return;
  }
  int32_t delay = 0;
  if (aInfo.Length() > 1 && !ToInteger<int32_t>(isolate, aInfo[1], &delay)) {
    return;
  }
  std::vector<v8::Local<v8::Value>> args;
  for (int i = 2; i < aInfo.Length(); ++i) {
    args.push_back(aInfo[i]);
  }
  uint32_t id = sNextTimerId++;
  RefPtr<V8Timer> timer = new V8Timer(isolate, id, aInfo[0].As<v8::Function>(),
                                      args, aRepeat);
  Timers()[id] = timer;
  timer->Start(uint32_t(std::max(delay, 0)));
  aInfo.GetReturnValue().Set(id);
}

void ClearTimer(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  uint32_t id = 0;
  if (aInfo.Length() < 1 ||
      !ToInteger<uint32_t>(aInfo.GetIsolate(), aInfo[0], &id)) {
    return;
  }
  auto found = Timers().find(id);
  if (found == Timers().end()) {
    return;
  }
  RefPtr<V8Timer> timer = found->second;
  Timers().erase(found);
  timer->Cancel();
}

class V8FrameRequests final : public nsARefreshObserver {
 public:
  NS_INLINE_DECL_REFCOUNTING(V8FrameRequests, override)

  explicit V8FrameRequests(nsGlobalWindowInner* aWindow) : mWindow(aWindow) {}

  uint32_t Add(v8::Isolate* aIsolate, v8::Local<v8::Function> aCallback) {
    uint32_t id = ++mLastId;
    mCallbacks.push_back(Callback{
        id, v8::Global<v8::Function>(aIsolate, aCallback),
        v8::Global<v8::Context>(aIsolate, aIsolate->GetCurrentContext())});
    EnsureObserving();
    return id;
  }

  void Cancel(uint32_t aId) {
    for (auto& callback : mCallbacks) {
      if (callback.mId == aId) {
        callback.mFunction.Reset();
      }
    }
  }

  MOZ_CAN_RUN_SCRIPT_BOUNDARY void WillRefresh(TimeStamp aTime) override {
    RefPtr<V8FrameRequests> kungFuDeathGrip(this);
    std::vector<Callback> callbacks;
    callbacks.swap(mCallbacks);
    StopObserving();
    Performance* performance = mWindow ? mWindow->GetPerformance() : nullptr;
    double now = performance ? performance->Now() : 0.0;
    v8::Isolate* isolate = V8Engine::Isolate();
    for (auto& callback : callbacks) {
      if (callback.mFunction.IsEmpty()) {
        continue;
      }
      v8::HandleScope handleScope(isolate);
      v8::Local<v8::Context> context = callback.mContext.Get(isolate);
      v8::Context::Scope contextScope(context);
      v8::Local<v8::Value> argument = v8::Number::New(isolate, now);
      v8::TryCatch tryCatch(isolate);
      v8::Local<v8::Value> result;
      if (!callback.mFunction.Get(isolate)
               ->Call(context, context->Global(), 1, &argument)
               .ToLocal(&result)) {
        ReportException(isolate, context, tryCatch, "requestAnimationFrame");
      }
    }
    if (!mCallbacks.empty()) {
      EnsureObserving();
    }
  }

  void Clear() {
    StopObserving();
    mCallbacks.clear();
    mWindow = nullptr;
  }

 private:
  struct Callback {
    uint32_t mId;
    v8::Global<v8::Function> mFunction;
    v8::Global<v8::Context> mContext;
  };

  ~V8FrameRequests() = default;

  nsRefreshDriver* RefreshDriver() {
    Document* document = mWindow ? mWindow->GetExtantDoc() : nullptr;
    nsPresContext* presContext =
        document ? document->GetPresContext() : nullptr;
    return presContext ? presContext->RefreshDriver() : nullptr;
  }

  void EnsureObserving() {
    if (mObserving) {
      return;
    }
    if (nsRefreshDriver* driver = RefreshDriver()) {
      driver->AddRefreshObserver(this, FlushType::Style,
                                 "VgeBedrock requestAnimationFrame");
      mObserving = true;
    }
  }

  void StopObserving() {
    if (!mObserving) {
      return;
    }
    if (nsRefreshDriver* driver = RefreshDriver()) {
      driver->RemoveRefreshObserver(this, FlushType::Style);
    }
    mObserving = false;
  }

  RefPtr<nsGlobalWindowInner> mWindow;
  std::vector<Callback> mCallbacks;
  uint32_t mLastId = 0;
  bool mObserving = false;
};

std::unordered_map<nsGlobalWindowInner*, RefPtr<V8FrameRequests>>&
FrameRequests() {
  static std::unordered_map<nsGlobalWindowInner*, RefPtr<V8FrameRequests>>
      sRequests;
  return sRequests;
}

}

void EventTarget_addEventListener(
    const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  v8::Isolate* isolate = aInfo.GetIsolate();
  EventTarget* target = ReceiverTarget(aInfo);
  if (!target) {
    ThrowTypeError(isolate, "EventTarget.addEventListener: illegal invocation");
    return;
  }
  if (aInfo.Length() < 2) {
    ThrowTypeError(isolate,
                   "EventTarget.addEventListener: not enough arguments");
    return;
  }
  nsAutoString type;
  if (!ToString(isolate, aInfo[0], type, StringNull::Stringify)) {
    return;
  }
  if (!aInfo[1]->IsObject()) {
    return;
  }
  EventListenerFlags flags;
  flags.mAllowUntrustedEvents = true;
  if (!ReadCapture(isolate, aInfo[2], &flags)) {
    return;
  }
  for (auto& entry : Listeners()) {
    if (entry.mTarget == target && !entry.mIsHandler &&
        entry.mType == type && entry.mCapture == flags.mCapture &&
        entry.mListener->Matches(isolate, aInfo[1])) {
      return;
    }
  }
  RefPtr<V8EventListener> listener =
      new V8EventListener(isolate, aInfo[1].As<v8::Object>(), false);
  EventListenerManager* manager = target->GetOrCreateListenerManager();
  if (!manager) {
    return;
  }
  manager->AddEventListenerByType(listener, type, flags);
  Listeners().push_back(
      ListenerEntry{target, type, flags.mCapture, listener, false});
}

void EventTarget_removeEventListener(
    const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  v8::Isolate* isolate = aInfo.GetIsolate();
  EventTarget* target = ReceiverTarget(aInfo);
  if (!target || aInfo.Length() < 2) {
    return;
  }
  nsAutoString type;
  if (!ToString(isolate, aInfo[0], type, StringNull::Stringify)) {
    return;
  }
  EventListenerFlags flags;
  if (!ReadCapture(isolate, aInfo[2], &flags)) {
    return;
  }
  auto& listeners = Listeners();
  for (auto it = listeners.begin(); it != listeners.end(); ++it) {
    if (it->mTarget == target && !it->mIsHandler && it->mType == type &&
        it->mCapture == flags.mCapture &&
        it->mListener->Matches(isolate, aInfo[1])) {
      if (EventListenerManager* manager = target->GetExistingListenerManager()) {
        manager->RemoveEventListenerByType(it->mListener, type, flags);
      }
      listeners.erase(it);
      return;
    }
  }
}

v8::Local<v8::Value> GetEventHandler(v8::Isolate* aIsolate,
                                     EventTarget* aTarget,
                                     const nsAString& aType) {
  for (auto& entry : Listeners()) {
    if (entry.mTarget == aTarget && entry.mIsHandler && entry.mType == aType) {
      return entry.mListener->Callable(aIsolate);
    }
  }
  return v8::Null(aIsolate);
}

void SetEventHandler(v8::Isolate* aIsolate, EventTarget* aTarget,
                     const nsAString& aType, v8::Local<v8::Value> aValue) {
  auto& listeners = Listeners();
  EventListenerFlags flags;
  flags.mAllowUntrustedEvents = true;
  for (auto it = listeners.begin(); it != listeners.end(); ++it) {
    if (it->mTarget == aTarget && it->mIsHandler && it->mType == aType) {
      if (EventListenerManager* manager =
              aTarget->GetExistingListenerManager()) {
        manager->RemoveEventListenerByType(it->mListener, aType, flags);
      }
      listeners.erase(it);
      break;
    }
  }
  if (!aValue->IsFunction()) {
    return;
  }
  RefPtr<V8EventListener> listener =
      new V8EventListener(aIsolate, aValue.As<v8::Object>(), true);
  EventListenerManager* manager = aTarget->GetOrCreateListenerManager();
  if (!manager) {
    return;
  }
  manager->AddEventListenerByType(listener, aType, flags);
  listeners.push_back(ListenerEntry{aTarget, nsString(aType), false, listener,
                                    true});
}

void Window_setTimeout(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  StartTimer(aInfo, false);
}

void Window_setInterval(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  StartTimer(aInfo, true);
}

void Window_clearTimeout(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  ClearTimer(aInfo);
}

void Window_clearInterval(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  ClearTimer(aInfo);
}

void Window_requestAnimationFrame(
    const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  v8::Isolate* isolate = aInfo.GetIsolate();
  nsGlobalWindowInner* window = ReceiverWindow(aInfo);
  if (!window || aInfo.Length() < 1 || !aInfo[0]->IsFunction()) {
    ThrowTypeError(isolate,
                   "Window.requestAnimationFrame: argument is not a function");
    return;
  }
  RefPtr<V8FrameRequests>& requests = FrameRequests()[window];
  if (!requests) {
    requests = new V8FrameRequests(window);
  }
  aInfo.GetReturnValue().Set(
      requests->Add(isolate, aInfo[0].As<v8::Function>()));
}

void Window_cancelAnimationFrame(
    const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  nsGlobalWindowInner* window = ReceiverWindow(aInfo);
  uint32_t id = 0;
  if (!window || aInfo.Length() < 1 ||
      !ToInteger<uint32_t>(aInfo.GetIsolate(), aInfo[0], &id)) {
    return;
  }
  auto found = FrameRequests().find(window);
  if (found != FrameRequests().end()) {
    found->second->Cancel(id);
  }
}

void Window_queueMicrotask(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  v8::Isolate* isolate = aInfo.GetIsolate();
  if (aInfo.Length() < 1 || !aInfo[0]->IsFunction()) {
    ThrowTypeError(isolate, "Window.queueMicrotask: argument is not a function");
    return;
  }
  isolate->EnqueueMicrotask(aInfo[0].As<v8::Function>());
}

void ReleaseEventState() {
  for (auto& entry : Timers()) {
    entry.second->Cancel();
  }
  Timers().clear();
  for (auto& entry : FrameRequests()) {
    entry.second->Clear();
  }
  FrameRequests().clear();
  Listeners().clear();
  for (V8EventListener* listener : LiveListeners()) {
    listener->Disconnect();
  }
}

}
