#include "VgeJsPlatform.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "libplatform/libplatform.h"
#include "mozilla/Mutex.h"
#include "mozilla/RefPtr.h"
#include "mozilla/TimeStamp.h"
#include "nsCOMPtr.h"
#include "nsIIdleRunnable.h"
#include "nsIRunnable.h"
#include "nsIThreadInternal.h"
#include "nsITimer.h"
#include "nsThread.h"
#include "nsThreadUtils.h"
#include "v8-platform.h"

namespace vge::jsapi {

namespace {

using mozilla::MutexAutoLock;
using mozilla::TimeDuration;
using mozilla::TimeStamp;

constexpr double kIdlePeriodWithoutDeadline = 0.05;

constexpr uint32_t kGeckoPriorityOf[] = {
    nsIRunnablePriority::PRIORITY_LOW,
    nsIRunnablePriority::PRIORITY_NORMAL,
    nsIRunnablePriority::PRIORITY_MEDIUMHIGH,
};

struct PendingTask {
  std::unique_ptr<v8::Task> task;
  std::unique_ptr<v8::IdleTask> idleTask;
  uint32_t priority = nsIRunnablePriority::PRIORITY_NORMAL;
  bool nonNestable = false;
  bool deferred = false;
  nsCOMPtr<nsITimer> timer;
};

class IsolateTasks final : public nsIThreadObserver {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSITHREADOBSERVER

  IsolateTasks(v8::Platform* aClock, nsThread* aThread, uint32_t aBaseDepth)
      : mClock(aClock),
        mThread(aThread),
        mBaseDepth(aBaseDepth),
        mMutex("vge::jsapi::IsolateTasks") {}

  void Post(PendingTask&& aTask, double aDelaySeconds);
  void Run(uint64_t aId, TimeStamp aDeadline);
  void Terminate();

 private:
  ~IsolateTasks() = default;

  bool Nested() const { return mThread->RecursionDepth() > mBaseDepth + 1; }
  bool Dispatch(uint64_t aId, uint32_t aPriority);
  void Drop(uint64_t aId);

  v8::Platform* const mClock;
  const RefPtr<nsThread> mThread;
  const uint32_t mBaseDepth;
  mozilla::Mutex mMutex MOZ_UNANNOTATED;
  bool mAlive = true;
  uint64_t mNextId = 1;
  std::unordered_map<uint64_t, PendingTask> mPending;
  std::vector<uint64_t> mDeferred;
};

NS_IMPL_ISUPPORTS(IsolateTasks, nsIThreadObserver)

class TaskRunnable final : public mozilla::Runnable,
                           public nsIRunnablePriority,
                           public nsIIdleRunnable {
 public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_NSIRUNNABLEPRIORITY

  TaskRunnable(IsolateTasks* aTasks, uint64_t aId, uint32_t aPriority)
      : mozilla::Runnable("vge::jsapi::V8ForegroundTask"),
        mTasks(aTasks),
        mId(aId),
        mPriority(aPriority) {}

  NS_IMETHOD Run() override {
    mTasks->Run(mId, mDeadline);
    return NS_OK;
  }

  void SetDeadline(TimeStamp aDeadline) override { mDeadline = aDeadline; }

 private:
  ~TaskRunnable() = default;

  const RefPtr<IsolateTasks> mTasks;
  const uint64_t mId;
  const uint32_t mPriority;
  TimeStamp mDeadline;
};

NS_IMPL_ISUPPORTS_INHERITED(TaskRunnable, mozilla::Runnable,
                            nsIRunnablePriority, nsIIdleRunnable)

NS_IMETHODIMP
TaskRunnable::GetPriority(uint32_t* aPriority) {
  *aPriority = mPriority;
  return NS_OK;
}

void IsolateTasks::Post(PendingTask&& aTask, double aDelaySeconds) {
  uint32_t priority = aTask.priority;
  uint64_t id = 0;
  {
    MutexAutoLock lock(mMutex);
    if (!mAlive) {
      return;
    }
    id = mNextId++;
    mPending.emplace(id, std::move(aTask));
  }
  if (aDelaySeconds > 0) {
    RefPtr<IsolateTasks> self = this;
    nsCOMPtr<nsITimer> timer;
    nsresult rv = NS_NewTimerWithCallback(
        getter_AddRefs(timer),
        [self, id](nsITimer*) { self->Run(id, TimeStamp()); },
        TimeDuration::FromSeconds(aDelaySeconds), nsITimer::TYPE_ONE_SHOT,
        "vge::jsapi::V8DelayedTask"_ns, mThread);
    if (NS_FAILED(rv)) {
      Drop(id);
      return;
    }
    MutexAutoLock lock(mMutex);
    auto it = mPending.find(id);
    if (it != mPending.end()) {
      it->second.timer.swap(timer);
    }
    return;
  }
  if (!Dispatch(id, priority)) {
    Drop(id);
  }
}

bool IsolateTasks::Dispatch(uint64_t aId, uint32_t aPriority) {
  RefPtr<TaskRunnable> runnable = new TaskRunnable(this, aId, aPriority);
  return NS_SUCCEEDED(
      mThread->Dispatch(runnable.forget(), NS_DISPATCH_FALLIBLE));
}

void IsolateTasks::Drop(uint64_t aId) {
  PendingTask dropped;
  MutexAutoLock lock(mMutex);
  auto it = mPending.find(aId);
  if (it == mPending.end()) {
    return;
  }
  dropped = std::move(it->second);
  mPending.erase(it);
}

void IsolateTasks::Run(uint64_t aId, TimeStamp aDeadline) {
  bool nested = Nested();
  PendingTask pending;
  {
    MutexAutoLock lock(mMutex);
    auto it = mPending.find(aId);
    if (it == mPending.end()) {
      return;
    }
    if (it->second.nonNestable && nested) {
      if (!it->second.deferred) {
        it->second.deferred = true;
        mDeferred.push_back(aId);
      }
      return;
    }
    pending = std::move(it->second);
    mPending.erase(it);
  }
  if (pending.task) {
    pending.task->Run();
    return;
  }
  if (pending.idleTask) {
    double remaining =
        aDeadline.IsNull()
            ? kIdlePeriodWithoutDeadline
            : std::max(0.0, (aDeadline - TimeStamp::Now()).ToSeconds());
    pending.idleTask->Run(mClock->MonotonicallyIncreasingTime() + remaining);
  }
}

void IsolateTasks::Terminate() {
  mThread->RemoveObserver(this);
  std::unordered_map<uint64_t, PendingTask> pending;
  {
    MutexAutoLock lock(mMutex);
    mAlive = false;
    pending.swap(mPending);
  }
  mDeferred.clear();
  for (auto& [id, task] : pending) {
    if (task.timer) {
      task.timer->Cancel();
    }
  }
}

NS_IMETHODIMP
IsolateTasks::OnDispatchedEvent() { return NS_OK; }

NS_IMETHODIMP
IsolateTasks::OnProcessNextEvent(nsIThreadInternal* aThread, bool aMayWait) {
  return NS_OK;
}

NS_IMETHODIMP
IsolateTasks::AfterProcessNextEvent(nsIThreadInternal* aThread,
                                    bool aEventWasProcessed) {
  if (mDeferred.empty() || Nested()) {
    return NS_OK;
  }
  std::vector<uint64_t> ids;
  ids.swap(mDeferred);
  for (uint64_t id : ids) {
    uint32_t priority = 0;
    {
      MutexAutoLock lock(mMutex);
      auto it = mPending.find(id);
      if (it == mPending.end()) {
        continue;
      }
      it->second.deferred = false;
      priority = it->second.priority;
    }
    if (!Dispatch(id, priority)) {
      Drop(id);
    }
  }
  return NS_OK;
}

class EventLoopTaskRunner final : public v8::TaskRunner {
 public:
  EventLoopTaskRunner(IsolateTasks* aTasks, uint32_t aPriority)
      : mTasks(aTasks), mPriority(aPriority) {}

  void PostTask(std::unique_ptr<v8::Task> aTask) override {
    Post(std::move(aTask), false, 0);
  }

  void PostNonNestableTask(std::unique_ptr<v8::Task> aTask) override {
    Post(std::move(aTask), true, 0);
  }

  void PostDelayedTask(std::unique_ptr<v8::Task> aTask,
                       double aDelaySeconds) override {
    Post(std::move(aTask), false, aDelaySeconds);
  }

  void PostNonNestableDelayedTask(std::unique_ptr<v8::Task> aTask,
                                  double aDelaySeconds) override {
    Post(std::move(aTask), true, aDelaySeconds);
  }

  void PostIdleTask(std::unique_ptr<v8::IdleTask> aTask) override {
    PendingTask pending;
    pending.idleTask = std::move(aTask);
    pending.priority = nsIRunnablePriority::PRIORITY_IDLE;
    pending.nonNestable = true;
    mTasks->Post(std::move(pending), 0);
  }

  bool IdleTasksEnabled() override { return true; }
  bool NonNestableTasksEnabled() const override { return true; }
  bool NonNestableDelayedTasksEnabled() const override { return true; }

 private:
  void Post(std::unique_ptr<v8::Task> aTask, bool aNonNestable,
            double aDelaySeconds) {
    PendingTask pending;
    pending.task = std::move(aTask);
    pending.priority = mPriority;
    pending.nonNestable = aNonNestable;
    mTasks->Post(std::move(pending), aDelaySeconds);
  }

  const RefPtr<IsolateTasks> mTasks;
  const uint32_t mPriority;
};

class EventLoopPlatform final : public v8::Platform {
 public:
  EventLoopPlatform() : mInner(v8::platform::NewDefaultPlatform()) {}

  void Attach(v8::Isolate* aIsolate) {
    nsCOMPtr<nsIThread> current = do_GetCurrentThread();
    MOZ_RELEASE_ASSERT(current);
    RefPtr<nsThread> thread = current.forget().downcast<nsThread>();
    RefPtr<IsolateTasks> tasks =
        new IsolateTasks(mInner.get(), thread, thread->RecursionDepth());
    MOZ_ALWAYS_SUCCEEDS(thread->AddObserver(tasks));
    IsolateEntry entry;
    entry.tasks = tasks;
    for (size_t p = 0; p < std::size(kGeckoPriorityOf); p++) {
      entry.runners[p] =
          std::make_shared<EventLoopTaskRunner>(tasks, kGeckoPriorityOf[p]);
    }
    std::lock_guard<std::mutex> lock(mMutex);
    mIsolates[aIsolate] = std::move(entry);
  }

  void Detach(v8::Isolate* aIsolate) {
    RefPtr<IsolateTasks> tasks;
    {
      std::lock_guard<std::mutex> lock(mMutex);
      auto it = mIsolates.find(aIsolate);
      if (it == mIsolates.end()) {
        return;
      }
      tasks = it->second.tasks;
      mIsolates.erase(it);
    }
    tasks->Terminate();
  }

  using v8::Platform::GetForegroundTaskRunner;

  std::shared_ptr<v8::TaskRunner> GetForegroundTaskRunner(
      v8::Isolate* aIsolate, v8::TaskPriority aPriority) override {
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = mIsolates.find(aIsolate);
    MOZ_RELEASE_ASSERT(it != mIsolates.end());
    return it->second.runners[size_t(aPriority)];
  }

  bool IdleTasksEnabled(v8::Isolate* aIsolate) override { return true; }

  v8::PageAllocator* GetPageAllocator() override {
    return mInner->GetPageAllocator();
  }

  v8::ThreadIsolatedAllocator* GetThreadIsolatedAllocator() override {
    return mInner->GetThreadIsolatedAllocator();
  }

  v8::ZoneBackingAllocator* GetZoneBackingAllocator() override {
    return mInner->GetZoneBackingAllocator();
  }

  void OnCriticalMemoryPressure() override {
    mInner->OnCriticalMemoryPressure();
  }

  int NumberOfWorkerThreads() override {
    return mInner->NumberOfWorkerThreads();
  }

  void CallOnWorkerThread(std::unique_ptr<v8::Task> aTask) override {
    mInner->CallOnWorkerThread(std::move(aTask));
  }

  void CallBlockingTaskOnWorkerThread(
      std::unique_ptr<v8::Task> aTask) override {
    mInner->CallBlockingTaskOnWorkerThread(std::move(aTask));
  }

  void CallLowPriorityTaskOnWorkerThread(
      std::unique_ptr<v8::Task> aTask) override {
    mInner->CallLowPriorityTaskOnWorkerThread(std::move(aTask));
  }

  void CallDelayedOnWorkerThread(std::unique_ptr<v8::Task> aTask,
                                 double aDelaySeconds) override {
    mInner->CallDelayedOnWorkerThread(std::move(aTask), aDelaySeconds);
  }

  std::unique_ptr<v8::JobHandle> PostJob(
      v8::TaskPriority aPriority,
      std::unique_ptr<v8::JobTask> aJobTask) override {
    return mInner->PostJob(aPriority, std::move(aJobTask));
  }

  std::unique_ptr<v8::JobHandle> CreateJob(
      v8::TaskPriority aPriority,
      std::unique_ptr<v8::JobTask> aJobTask) override {
    return mInner->CreateJob(aPriority, std::move(aJobTask));
  }

  std::unique_ptr<v8::ScopedBlockingCall> CreateBlockingScope(
      v8::BlockingType aBlockingType) override {
    return mInner->CreateBlockingScope(aBlockingType);
  }

  double MonotonicallyIncreasingTime() override {
    return mInner->MonotonicallyIncreasingTime();
  }

  int64_t CurrentClockTimeMilliseconds() override {
    return mInner->CurrentClockTimeMilliseconds();
  }

  double CurrentClockTimeMillis() override {
    return mInner->CurrentClockTimeMillis();
  }

  double CurrentClockTimeMillisecondsHighResolution() override {
    return mInner->CurrentClockTimeMillisecondsHighResolution();
  }

  StackTracePrinter GetStackTracePrinter() override {
    return mInner->GetStackTracePrinter();
  }

  v8::TracingController* GetTracingController() override {
    return mInner->GetTracingController();
  }

  void DumpWithoutCrashing() override { mInner->DumpWithoutCrashing(); }

  v8::HighAllocationThroughputObserver* GetHighAllocationThroughputObserver()
      override {
    return mInner->GetHighAllocationThroughputObserver();
  }

 protected:
  std::unique_ptr<v8::JobHandle> CreateJobImpl(
      v8::TaskPriority aPriority, std::unique_ptr<v8::JobTask> aJobTask,
      const v8::SourceLocation& aLocation) override {
    return mInner->CreateJob(aPriority, std::move(aJobTask));
  }

  void PostTaskOnWorkerThreadImpl(
      v8::TaskPriority aPriority, std::unique_ptr<v8::Task> aTask,
      const v8::SourceLocation& aLocation) override {
    switch (aPriority) {
      case v8::TaskPriority::kBestEffort:
        mInner->CallLowPriorityTaskOnWorkerThread(std::move(aTask));
        break;
      case v8::TaskPriority::kUserVisible:
        mInner->CallOnWorkerThread(std::move(aTask));
        break;
      case v8::TaskPriority::kUserBlocking:
        mInner->CallBlockingTaskOnWorkerThread(std::move(aTask));
        break;
    }
  }

  void PostDelayedTaskOnWorkerThreadImpl(
      v8::TaskPriority aPriority, std::unique_ptr<v8::Task> aTask,
      double aDelaySeconds, const v8::SourceLocation& aLocation) override {
    mInner->CallDelayedOnWorkerThread(std::move(aTask), aDelaySeconds);
  }

 private:
  struct IsolateEntry {
    RefPtr<IsolateTasks> tasks;
    std::shared_ptr<v8::TaskRunner> runners[std::size(kGeckoPriorityOf)];
  };

  std::unique_ptr<v8::Platform> mInner;
  std::mutex mMutex;
  std::unordered_map<v8::Isolate*, IsolateEntry> mIsolates;
};

}

std::unique_ptr<v8::Platform> NewEventLoopPlatform() {
  return std::make_unique<EventLoopPlatform>();
}

void AttachIsolateToCurrentThread(v8::Platform* aPlatform,
                                  v8::Isolate* aIsolate) {
  static_cast<EventLoopPlatform*>(aPlatform)->Attach(aIsolate);
}

void DetachIsolate(v8::Platform* aPlatform, v8::Isolate* aIsolate) {
  static_cast<EventLoopPlatform*>(aPlatform)->Detach(aIsolate);
}

}
