#ifndef vge_bedrock_jsapi_buffers_VgeJsBuffers_h
#define vge_bedrock_jsapi_buffers_VgeJsBuffers_h

#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "VgeJsInternal.h"
#include "js/ArrayBuffer.h"
#include "js/ArrayBufferMaybeShared.h"
#include "js/HelperThreadAPI.h"
#include "js/Promise.h"
#include "js/ScalarType.h"
#include "js/SharedArrayBuffer.h"
#include "js/StructuredClone.h"
#include "js/WaitCallbacks.h"
#include "js/experimental/TypedData.h"
#include "js/friend/ErrorMessages.h"
#include "js/friend/MicroTask.h"

namespace js {

class SharedArrayRawBuffer {
 public:
  explicit SharedArrayRawBuffer(std::shared_ptr<v8::BackingStore> aStore)
      : mRefCount(1), mStore(std::move(aStore)) {}

  bool addReference();
  void dropReference();
  const std::shared_ptr<v8::BackingStore>& store() const { return mStore; }
  bool isGrowableJS() const { return mStore->IsResizableByUserJavaScript(); }

 private:
  std::atomic<uint32_t> mRefCount;
  std::shared_ptr<v8::BackingStore> mStore;
};

class MicroTaskQueueElement {
 public:
  static JS::MicroTask MakeJS(const JS::Value& aValue) {
    return JS::MicroTask(JS::MicroTask::Kind(JS::MicroTask::FirstJSKind),
                         aValue);
  }
  static const JS::Value& ValueOf(const JS::MicroTask& aTask) {
    return aTask.value_;
  }
  static JS::Value* AddressOf(JS::MicroTask& aTask) { return &aTask.value_; }
};

}

namespace JS {

class HelperThreadTask {
 public:
  virtual ~HelperThreadTask() = default;
  virtual void runHelperThreadTask() = 0;
  virtual const char* getName() = 0;
};

}

namespace vge::jsapi::buffers {

extern const JSClass ArrayBufferClasses[5];
extern const JSClass TypedArrayFixedClasses[JS::Scalar::MaxTypedArrayViewType];
extern const JSClass TypedArrayResizableClasses[JS::Scalar::MaxTypedArrayViewType];
extern const JSClass TypedArrayImmutableClasses[JS::Scalar::MaxTypedArrayViewType];

enum ArrayBufferClassIndex : size_t {
  kFixedUnshared = 0,
  kResizableUnshared = 1,
  kImmutableUnshared = 2,
  kFixedShared = 3,
  kGrowableShared = 4,
};

JSObject* Unwrapped(JSObject* aObj);
v8::Local<v8::Object> V8Of(JSContext* aCx, JSObject* aObj);
const JSClass* ClassForV8(v8::Local<v8::Object> aObject);
bool IsBufferClass(const JSClass* aClass);
JSObject* BufferObjectFromV8(JSContext* aCx, v8::Local<v8::Object> aObject);
JS::Scalar::Type ScalarOf(v8::Local<v8::Value> aValue);
size_t ElementSize(JS::Scalar::Type aType);
const char* ScalarName(JS::Scalar::Type aType);
uint8_t* ViewData(v8::Local<v8::ArrayBufferView> aView);
uint8_t* BufferData(v8::Local<v8::Object> aBuffer);
size_t BufferLength(v8::Local<v8::Object> aBuffer);
bool BufferDetached(v8::Local<v8::Object> aBuffer);
JSObject* NewTypedArrayWithBuffer(JSContext* aCx, JS::Scalar::Type aType,
                                  JS::HandleObject aBuffer, size_t aByteOffset,
                                  int64_t aLength);
JSObject* NewArrayBufferFromStore(JSContext* aCx,
                                  std::shared_ptr<v8::BackingStore> aStore);
JSObject* NewDataViewObject(JSContext* aCx, JS::HandleObject aBuffer,
                            size_t aByteOffset, mozilla::Maybe<size_t> aLength);
v8::Local<v8::Private> PrivateKey(v8::Isolate* aIsolate, const char* aName);

struct SavedQueues {
  std::deque<JS::MicroTask> regular;
  std::deque<JS::MicroTask> debug;
};

struct ContextState {
  JSRuntime* runtime = nullptr;
  JSContext* cx = nullptr;
  std::deque<JS::MicroTask> regular;
  std::deque<JS::MicroTask> debug;
  std::vector<SavedQueues*> saved;
  bool v8JobPending = false;
  bool draining = false;
  bool canSkipEnqueuingJobs = false;
  bool hooksInstalled = false;
  uint64_t nextFlowId = 1;
  JS::PromiseRejectionTrackerCallback rejectionTracker = nullptr;
  void* rejectionTrackerData = nullptr;
  JS::DispatchToEventLoopCallback dispatchCallback = nullptr;
  JS::DelayedDispatchToEventLoopCallback delayedDispatchCallback = nullptr;
  JS::AsyncTaskStartedCallback asyncTaskStarted = nullptr;
  JS::AsyncTaskFinishedCallback asyncTaskFinished = nullptr;
  void* asyncTaskClosure = nullptr;
  std::vector<JS::Dispatchable*> liveTasks;
  bool shuttingDown = false;
  JS::BeforeWaitCallback beforeWait = nullptr;
  JS::AfterWaitCallback afterWait = nullptr;
};

ContextState* StateFor(JSContext* aCx);
ContextState* StateForRuntime(JSRuntime* aRuntime);
void InstallJobHooks(JSContext* aCx);
void NoteV8Job(JSContext* aCx, v8::Local<v8::Context> aJobContext);
bool ReportCloneError(JSContext* aCx,
                      const JSStructuredCloneCallbacks* aCallbacks,
                      uint32_t aErrorId, void* aClosure, const char* aArg);
bool DispatchToEventLoop(JSContext* aCx,
                         js::UniquePtr<JS::Dispatchable>&& aTask);
void DispatchHelperThreadTask(JS::HelperThreadTask* aTask);

}

#endif
