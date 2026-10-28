#include "VgeJsBuffers.h"

#include "mozilla/Assertions.h"

namespace vge::jsapi::buffers {

namespace {

struct HelperThreadPool {
  std::mutex lock;
  JS::HelperThreadTaskCallback callback = nullptr;
  size_t threadCount = 0;
  size_t stackSize = 0;
};

HelperThreadPool& Pool() {
  static HelperThreadPool pool;
  return pool;
}

struct WaitScratch {
  alignas(16) uint8_t memory[JS::WAIT_CALLBACK_CLIENT_MAXMEM];
  void* cookie = nullptr;
};

thread_local WaitScratch tWaitScratch;

void AtomicsWaitCallback(v8::Isolate::AtomicsWaitEvent aEvent,
                         v8::Local<v8::SharedArrayBuffer> aArrayBuffer,
                         size_t aOffsetInBytes, int64_t aValue,
                         double aTimeoutInMs,
                         v8::Isolate::AtomicsWaitWakeHandle* aStopHandle,
                         void* aData) {
  ContextState* state = static_cast<ContextState*>(aData);
  if (!state->beforeWait || !state->afterWait) {
    return;
  }
  if (aEvent == v8::Isolate::AtomicsWaitEvent::kStartWait) {
    tWaitScratch.cookie = state->beforeWait(tWaitScratch.memory);
    return;
  }
  state->afterWait(tWaitScratch.cookie);
  tWaitScratch.cookie = nullptr;
}

}

void DispatchHelperThreadTask(JS::HelperThreadTask* aTask) {
  JS::HelperThreadTaskCallback callback;
  {
    HelperThreadPool& pool = Pool();
    std::lock_guard<std::mutex> guard(pool.lock);
    callback = pool.callback;
  }
  if (callback) {
    callback(aTask);
    return;
  }
  JS::RunHelperThreadTask(aTask);
}

}

using namespace vge::jsapi;
using namespace vge::jsapi::buffers;

void JS::SetHelperThreadTaskCallback(JS::HelperThreadTaskCallback aCallback,
                                     size_t aThreadCount, size_t aStackSize) {
  MOZ_ASSERT(aThreadCount != 0);
  MOZ_ASSERT(aStackSize >= 16 * 1024);
  HelperThreadPool& pool = Pool();
  std::lock_guard<std::mutex> guard(pool.lock);
  pool.callback = aCallback;
  pool.threadCount = aThreadCount;
  pool.stackSize = aStackSize;
}

void JS::RunHelperThreadTask(JS::HelperThreadTask* aTask) {
  aTask->runHelperThreadTask();
}

const char* JS::GetHelperThreadTaskName(JS::HelperThreadTask* aTask) {
  return aTask->getName();
}

void JS::SetWaitCallback(JSRuntime* aRuntime, JS::BeforeWaitCallback aBefore,
                         JS::AfterWaitCallback aAfter,
                         size_t aRequiredMemory) {
  MOZ_RELEASE_ASSERT(aRequiredMemory <= JS::WAIT_CALLBACK_CLIENT_MAXMEM);
  MOZ_RELEASE_ASSERT((aBefore == nullptr) == (aAfter == nullptr));
  ContextState* state = StateForRuntime(aRuntime);
  state->beforeWait = aBefore;
  state->afterWait = aAfter;
  aRuntime->isolate->SetAtomicsWaitCallback(
      aBefore ? AtomicsWaitCallback : nullptr, state);
}
