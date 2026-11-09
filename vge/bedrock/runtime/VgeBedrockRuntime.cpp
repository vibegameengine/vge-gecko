#include "vge/bedrock/VgeBedrockRuntime.h"

#include <stdio.h>

#include "VgeJsInternal.h"
#include "js/Realm.h"
#include "nsCycleCollectionParticipant.h"
#include "nsCycleCollector.h"
#include "nsICycleCollectorListener.h"
#include "mozilla/dom/ScriptSettings.h"
#include "nsIGlobalObject.h"
#include "nsJSEnvironment.h"
#include "nsThreadUtils.h"
#include "prinrval.h"

#include "mozilla/IOInterposer.h"
#include "mozilla/Logging.h"
#include "mozilla/Preferences.h"
#include "ProfilerControl.h"
#include "mozilla/PresShell.h"
#include "mozilla/SpinEventLoopUntil.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/Element.h"
#include "mozilla/dom/LoadURIOptionsBinding.h"
#include "mozilla/mscom/ProcessRuntime.h"
#include "gfxConfig.h"
#include "gfxFeature.h"
#include "gfxTelemetry.h"
#include "gfxPlatform.h"
#include "mozilla/AutoSQLiteLifetime.h"
#include "mozilla/Services.h"
#include "mozilla/gfx/GPUProcessManager.h"
#include "nsIAppWindow.h"
#include "nsIObserverService.h"
#include "nsIWebBrowserChrome.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/dom/HTMLBodyElement.h"
#include "nsFrameLoader.h"
#include "nsFrameLoaderOwner.h"
#include "nsStreamUtils.h"
#include <algorithm>
#include <atomic>
#include <thread>
#include <windows.h>
#include <psapi.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include "mozilla/gfx/DeviceManagerDx.h"
#include "mozmemory.h"
#include "mozilla/StackWalk.h"
#include "mozilla/StackWalkThread.h"
#include "nsIThreadInternal.h"
#include "nsGkAtoms.h"
#include "prenv.h"
#include "nsGlobalWindowOuter.h"
#include "nsIBrowserDOMWindow.h"
#include "nsIFocusManager.h"
#include "nsQueryObject.h"
#include "nsIWindowCreator.h"
#include "nsIWindowWatcher.h"
#include "nsEmbedCID.h"
#include "nsAppDirectoryServiceDefs.h"
#include "nsAppShellCID.h"
#include "nsArrayEnumerator.h"
#include "nsCOMArray.h"
#include "nsContentUtils.h"
#include "nsDirectoryServiceDefs.h"
#include "nsIAppShellService.h"
#include "nsIBaseWindow.h"
#include "nsIDirectoryService.h"
#include "nsIDocShell.h"
#include "nsIFile.h"
#include "nsIFrame.h"
#include "nsIMemoryInfoDumper.h"
#include "GeckoProfiler.h"
#include "VgeAllocTrace.h"
#include "nsIWebNavigation.h"
#include "nsIWindowlessBrowser.h"
#include "nsLayoutUtils.h"
#include "nsNetUtil.h"
#include "nsPresContext.h"
#include "nsSimpleEnumerator.h"
#include "nsXULAppAPI.h"

using namespace mozilla;
using mozilla::dom::Document;
using mozilla::dom::Element;

namespace vge::bedrock {

namespace {

class RuntimeDirProvider final : public nsIDirectoryServiceProvider2 {
 public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_NSIDIRECTORYSERVICEPROVIDER
  NS_DECL_NSIDIRECTORYSERVICEPROVIDER2

  void SetBinaryFile(nsIFile* aFile) {
    mBinaryFile = aFile;
    aFile->GetParent(getter_AddRefs(mGreDir));
  }

  nsIFile* GreDir() const { return mGreDir; }

 private:
  nsCOMPtr<nsIFile> mBinaryFile;
  nsCOMPtr<nsIFile> mGreDir;
};

NS_IMETHODIMP_(MozExternalRefCountType)
RuntimeDirProvider::AddRef() { return 2; }

NS_IMETHODIMP_(MozExternalRefCountType)
RuntimeDirProvider::Release() { return 1; }

NS_IMPL_QUERY_INTERFACE(RuntimeDirProvider, nsIDirectoryServiceProvider,
                        nsIDirectoryServiceProvider2)

NS_IMETHODIMP
RuntimeDirProvider::GetFile(const char* aProp, bool* aPersistent,
                            nsIFile** aResult) {
  *aPersistent = true;
  if (!strcmp(aProp, NS_GRE_DIR) || !strcmp(aProp, NS_GRE_BIN_DIR)) {
    return mGreDir->Clone(aResult);
  }
  if (!strcmp(aProp, XRE_EXECUTABLE_FILE)) {
    return mBinaryFile->Clone(aResult);
  }
  if (!strcmp(aProp, NS_APP_USER_PROFILE_50_DIR) ||
      !strcmp(aProp, NS_APP_USER_PROFILE_LOCAL_50_DIR)) {
    nsCOMPtr<nsIFile> file;
    if (NS_FAILED(NS_GetSpecialDirectory(NS_OS_TEMP_DIR,
                                         getter_AddRefs(file))) ||
        NS_FAILED(file->AppendNative("vge-bedrock-profile"_ns))) {
      return NS_ERROR_FAILURE;
    }
    nsresult rv = file->Create(nsIFile::DIRECTORY_TYPE, 0700);
    if (NS_FAILED(rv) && rv != NS_ERROR_FILE_ALREADY_EXISTS) {
      return rv;
    }
    file.forget(aResult);
    return NS_OK;
  }
  if (!strcmp(aProp, NS_APP_PREF_DEFAULTS_50_DIR)) {
    nsCOMPtr<nsIFile> file;
    if (NS_FAILED(mGreDir->Clone(getter_AddRefs(file))) ||
        NS_FAILED(file->AppendNative("defaults"_ns)) ||
        NS_FAILED(file->AppendNative("pref"_ns))) {
      return NS_ERROR_FAILURE;
    }
    file.forget(aResult);
    return NS_OK;
  }
  return NS_ERROR_FAILURE;
}

NS_IMETHODIMP
RuntimeDirProvider::GetFiles(const char* aProp,
                             nsISimpleEnumerator** aResult) {
  if (!strcmp(aProp, "ChromeML")) {
    nsCOMArray<nsIFile> dirs;
    nsCOMPtr<nsIFile> file;
    mGreDir->Clone(getter_AddRefs(file));
    file->AppendNative("chrome"_ns);
    dirs.AppendObject(file);
    return NS_NewArrayEnumerator(aResult, dirs, NS_GET_IID(nsIFile));
  }
  return NS_ERROR_FAILURE;
}

JS::Realm* sPageRealm = nullptr;
uint32_t sPageRealmDestroyed = 0;
JS::DestroyRealmCallback sForwardDestroyRealm = nullptr;

void CountDestroyedRealm(JS::GCContext* aGcx, JS::Realm* aRealm) {
  if (aRealm == sPageRealm) {
    sPageRealmDestroyed++;
  }
  if (sForwardDestroyRealm) {
    sForwardDestroyRealm(aGcx, aRealm);
  }
}

const char* FrameTypeName(LayoutFrameType aType) {
  switch (aType) {
#define FRAME_TYPE(ty_, ...)   \
  case LayoutFrameType::ty_: \
    return #ty_;
#include "mozilla/FrameTypeList.h"
#undef FRAME_TYPE
  }
  return "?";
}

void PrintFrameGeometry(Document* aDocument) {
  PresShell* presShell = aDocument->GetPresShell();
  nsIFrame* rootFrame = presShell ? presShell->GetRootFrame() : nullptr;
  if (!rootFrame) {
    printf("no root frame\n");
    return;
  }
  for (nsINode* node = aDocument->GetRootElement(); node;
       node = node->GetNextNode()) {
    Element* element = Element::FromNode(node);
    if (!element) {
      continue;
    }
    nsIFrame* frame = element->GetPrimaryFrame();
    nsAutoCString name;
    CopyUTF16toUTF8(element->LocalName(), name);
    nsAutoString id;
    element->GetId(id);
    if (!id.IsEmpty()) {
      name.Append('#');
      AppendUTF16toUTF8(id, name);
    }
    if (!frame) {
      printf("%s no frame\n", name.get());
      continue;
    }
    nsRect rect = frame->GetRect();
    rect.MoveTo(frame->GetOffsetTo(rootFrame));
    printf("%s frame=%s x=%g y=%g w=%g h=%g\n", name.get(),
           FrameTypeName(frame->Type()),
           nsPresContext::AppUnitsToFloatCSSPixels(rect.x),
           nsPresContext::AppUnitsToFloatCSSPixels(rect.y),
           nsPresContext::AppUnitsToFloatCSSPixels(rect.width),
           nsPresContext::AppUnitsToFloatCSSPixels(rect.height));
  }
}

void ScheduleProfileSave() {
  const char* path = PR_GetEnv("VGE_PROFILE");
  if (!path) {
    return;
  }
  const char* delay = PR_GetEnv("VGE_PROFILE_DELAY");
  NS_DelayedDispatchToCurrentThread(
      NS_NewRunnableFunction("VgeProfileSave",
                             [path] { profiler_save_profile_to_file(path); }),
      (delay ? atoi(delay) : 30) * 1000);
}

void ScheduleMemoryReport() {
  const char* path = PR_GetEnv("VGE_MEMORY_REPORT");
  if (!path) {
    return;
  }
  const char* delay = PR_GetEnv("VGE_MEMORY_REPORT_DELAY");
  NS_DelayedDispatchToCurrentThread(
      NS_NewRunnableFunction(
          "VgeMemoryReport",
          [path = nsString(NS_ConvertUTF8toUTF16(path))] {
            nsCOMPtr<nsIMemoryInfoDumper> dumper =
                do_GetService("@mozilla.org/memory-info-dumper;1");
            dumper->DumpMemoryReportsToNamedFile(path, nullptr, nullptr, false,
                                                 false);
          }),
      (delay ? atoi(delay) : 0) * 1000);
}

void PrintPressureStep(const char* aName) {
  PROCESS_MEMORY_COUNTERS_EX counters = {};
  GetProcessMemoryInfo(GetCurrentProcess(),
                       reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                       sizeof(counters));
  printf("VGEPRESSURE %s ws_mb %zu commit_mb %zu\n", aName,
         counters.WorkingSetSize / 1048576, counters.PrivateUsage / 1048576);
  fflush(stdout);
}

void TrimDevice(RefPtr<ID3D11Device> aDevice) {
  if (!aDevice) {
    return;
  }
  RefPtr<IDXGIDevice3> dxgi;
  if (SUCCEEDED(aDevice->QueryInterface(__uuidof(IDXGIDevice3),
                                        getter_AddRefs(dxgi)))) {
    dxgi->Trim();
  }
}

void RunPressureStep(size_t aStep) {
  constexpr uint32_t kStepDelayMs = 3000;
  switch (aStep) {
    case 0:
      PrintPressureStep("before");
      break;
    case 1:
      if (nsCOMPtr<nsIObserverService> os = mozilla::services::GetObserverService()) {
        os->NotifyObservers(nullptr, "memory-pressure", u"low-memory");
      }
      break;
    case 2:
      PrintPressureStep("after_notify");
      jemalloc_free_dirty_pages();
      break;
    case 3: {
      PrintPressureStep("after_jemalloc");
      auto* manager = mozilla::gfx::DeviceManagerDx::Get();
      TrimDevice(manager->GetCompositorDevice());
      TrimDevice(manager->GetContentDevice());
      TrimDevice(manager->GetCanvasDevice());
      break;
    }
    case 4:
      PrintPressureStep("after_trim");
      SetProcessWorkingSetSize(GetCurrentProcess(), SIZE_T(-1), SIZE_T(-1));
      PrintPressureStep("after_empty_ws");
      break;
    case 5:
      PrintPressureStep("rebound_3s");
      break;
    case 6:
    case 7:
    case 8:
      PrintPressureStep("rebound_later");
      break;
    default:
      return;
  }
  NS_DelayedDispatchToCurrentThread(
      NS_NewRunnableFunction("VgeMemoryPressureStep",
                             [aStep] { RunPressureStep(aStep + 1); }),
      kStepDelayMs);
}

class ColdMemoryTrimmer {
 public:
  static void Start() {
    if (PR_GetEnv("VGE_MEMORY_PRESSURE_DELAY") || PR_GetEnv("VGE_NO_MEMORY_TRIM")) {
      return;
    }
    Dispatch(kPollDelayMs, [] { Poll(0, 0); });
  }

 private:
  static constexpr uint32_t kPollDelayMs = 2000;
  static constexpr uint32_t kSliceDelayMs = 16;
  static constexpr uint32_t kStablePolls = 5;
  static constexpr size_t kStableBytes = 8 * 1048576;
  static constexpr size_t kSliceBytes = 8 * 1048576;

  template <typename Function>
  static void Dispatch(uint32_t aDelay, Function aFunction) {
    NS_DelayedDispatchToCurrentThread(
        NS_NewRunnableFunction("VgeColdMemoryTrim", std::move(aFunction)), aDelay);
  }

  static void Poll(size_t aLastCommit, uint32_t aStable) {
    PROCESS_MEMORY_COUNTERS_EX counters = {};
    GetProcessMemoryInfo(GetCurrentProcess(),
                         reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                         sizeof(counters));
    size_t commit = counters.PrivateUsage;
    size_t change = commit > aLastCommit ? commit - aLastCommit : aLastCommit - commit;
    uint32_t stable = aLastCommit && change < kStableBytes ? aStable + 1 : 0;
    if (stable >= kStablePolls) {
      Dispatch(kSliceDelayMs, [] { Sweep(0); });
      return;
    }
    Dispatch(kPollDelayMs, [commit, stable] { Poll(commit, stable); });
  }

  static void Sweep(uintptr_t aCursor) {
    uintptr_t cursor = aCursor;
    size_t budget = kSliceBytes;
    MEMORY_BASIC_INFORMATION info;
    while (budget &&
           VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info))) {
      uintptr_t end = uintptr_t(info.BaseAddress) + info.RegionSize;
      bool trimmable = info.State == MEM_COMMIT && info.Type == MEM_PRIVATE &&
                       !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS));
      if (!trimmable) {
        cursor = end;
        continue;
      }
      uintptr_t from = std::max(cursor, uintptr_t(info.BaseAddress));
      size_t length = std::min(size_t(end - from), budget);
      VirtualUnlock(reinterpret_cast<void*>(from), length);
      cursor = from + length;
      budget -= length;
    }
    if (budget) {
      return;
    }
    Dispatch(kSliceDelayMs, [cursor] { Sweep(cursor); });
  }
};

void ScheduleMemoryPressure() {
  const char* delay = PR_GetEnv("VGE_MEMORY_PRESSURE_DELAY");
  if (!delay) {
    return;
  }
  NS_DelayedDispatchToCurrentThread(
      NS_NewRunnableFunction("VgeMemoryPressure", [] { RunPressureStep(0); }),
      atoi(delay) * 1000);
}

class StallProbe final : public nsIThreadObserver {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS

  NS_IMETHOD OnDispatchedEvent() override { return NS_OK; }
  NS_IMETHOD OnProcessNextEvent(nsIThreadInternal*, bool) override {
    mLastTick = GetTickCount64();
    return NS_OK;
  }
  NS_IMETHOD AfterProcessNextEvent(nsIThreadInternal*, bool) override {
    mLastTick = GetTickCount64();
    return NS_OK;
  }

  void Start() {
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                    GetCurrentProcess(), &mMainThread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    mLastTick = GetTickCount64();
    std::thread([this] { Watch(); }).detach();
  }

 private:
  ~StallProbe() = default;

  static void PrintFrame(uint32_t aFrameNumber, void* aPC, void* aSP,
                         void* aClosure) {
    MozCodeAddressDetails details;
    MozDescribeCodeAddress(aPC, &details);
    char line[512];
    MozFormatCodeAddressDetails(line, sizeof(line), aFrameNumber, aPC,
                                &details);
    printf("stall %s\n", line);
  }

  void Watch() {
    uint64_t reportedTick = 0;
    uint64_t reportedAt = 0;
    for (;;) {
      Sleep(3);
      uint64_t tick = mLastTick;
      uint64_t now = GetTickCount64();
      uint64_t age = now - tick;
      if (age >= 25 && (reportedTick != tick || now - reportedAt >= 500)) {
        reportedTick = tick;
        reportedAt = now;
        printf("stall begin %llums\n", (unsigned long long)age);
        MozStackWalkThread(PrintFrame, 28, nullptr, mMainThread, nullptr);
      }
    }
  }

  std::atomic<uint64_t> mLastTick{0};
  HANDLE mMainThread = nullptr;
};

NS_IMPL_ISUPPORTS(StallProbe, nsIThreadObserver)

class SingleWindowOpener final : public nsIBrowserDOMWindow {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIBROWSERDOMWINDOW

  explicit SingleWindowOpener(nsIDocShell* aContent) : mContent(aContent) {}

 private:
  ~SingleWindowOpener() = default;
  nsCOMPtr<nsIDocShell> mContent;
};

NS_IMPL_ISUPPORTS(SingleWindowOpener, nsIBrowserDOMWindow)

NS_IMETHODIMP
SingleWindowOpener::CreateContentWindow(
    nsIURI* aURI, nsIOpenWindowInfo* aOpenWindowInfo, int16_t aWhere,
    int32_t aFlags, nsIPrincipal* aTriggeringPrincipal,
    nsIPolicyContainer* aPolicyContainer,
    mozilla::dom::BrowsingContext** aResult) {
  RefPtr<mozilla::dom::BrowsingContext> context =
      mContent->GetBrowsingContext();
  context.forget(aResult);
  return NS_OK;
}

NS_IMETHODIMP
SingleWindowOpener::CreateContentWindowInFrame(
    nsIURI* aURI, nsIOpenURIInFrameParams* aParams, int16_t aWhere,
    int32_t aFlags, const nsAString& aName, Element** aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
SingleWindowOpener::OpenURI(nsIURI* aURI, nsIOpenWindowInfo* aOpenWindowInfo,
                            int16_t aWhere, int32_t aFlags,
                            nsIPrincipal* aTriggeringPrincipal,
                            nsIPolicyContainer* aPolicyContainer,
                            mozilla::dom::BrowsingContext** aResult) {
  return CreateContentWindow(aURI, aOpenWindowInfo, aWhere, aFlags,
                             aTriggeringPrincipal, aPolicyContainer, aResult);
}

NS_IMETHODIMP
SingleWindowOpener::OpenURIInFrame(nsIURI* aURI,
                                   nsIOpenURIInFrameParams* aParams,
                                   int16_t aWhere, int32_t aFlags,
                                   const nsAString& aName, Element** aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
SingleWindowOpener::CanClose(bool* aResult) {
  *aResult = true;
  return NS_OK;
}

NS_IMETHODIMP
SingleWindowOpener::GetTabCount(uint32_t* aResult) {
  *aResult = 1;
  return NS_OK;
}

nsCOMPtr<nsIURI> ChromeShellUri() {
  nsCOMPtr<nsIURI> uri;
  NS_NewURI(getter_AddRefs(uri), "about:blank"_ns);
  return uri;
}

already_AddRefed<nsIDocShell> CreateContentDocShell(nsIAppWindow* aWindow) {
  nsCOMPtr<nsIDocShell> chromeShell;
  aWindow->GetDocShell(getter_AddRefs(chromeShell));
  SpinEventLoopUntil("vge-bedrock-chrome-shell"_ns, [&] {
    Document* document = chromeShell->GetDocument();
    return document && document->GetPresShell() && document->GetBody() &&
           document->GetReadyStateEnum() == Document::READYSTATE_COMPLETE;
  });
  RefPtr<Document> chromeDocument = chromeShell->GetDocument();
  RefPtr<Element> frame = chromeDocument->CreateHTMLElement(nsGkAtoms::iframe);
  IgnoredErrorResult attributeError;
  frame->SetAttribute(u"type"_ns, u"content"_ns, nullptr, attributeError);
  frame->SetAttribute(
      u"style"_ns,
      u"position:fixed;left:0;top:0;width:100%;height:100%;border:0"_ns,
      nullptr, attributeError);
  RefPtr<Element> body = chromeDocument->GetBody();
  body->AppendChildTo(frame, true, IgnoreErrors());
  chromeDocument->FlushPendingNotifications(FlushType::Layout);
  RefPtr<nsFrameLoaderOwner> owner = do_QueryObject(frame);
  RefPtr<nsFrameLoader> loader = owner->GetFrameLoader();
  nsCOMPtr<nsIDocShell> content = loader->GetDocShell(IgnoreErrors());
  content->GetBrowsingContext()->SetExplicitActive(
      dom::ExplicitActiveStatus::Active, IgnoreErrors());
  nsCOMPtr<nsIBrowserDOMWindow> opener = new SingleWindowOpener(content);
  nsGlobalWindowOuter::Cast(chromeDocument->GetWindow())
      ->SetBrowserDOMWindowOuter(opener);
  nsCOMPtr<nsIFocusManager> focusManager =
      do_GetService("@mozilla.org/focus-manager;1");
  focusManager->SetFocus(frame, nsIFocusManager::FLAG_NOSCROLL);
  return content.forget();
}

int RunWindow(const char* aUrl, int aWidth, int aHeight) {
  nsCOMPtr<nsIAppShellService> appShell =
      do_GetService(NS_APPSHELLSERVICE_CONTRACTID);
  nsCOMPtr<nsIAppWindow> window;
  if (NS_FAILED(appShell->CreateTopLevelWindow(
          nullptr, ChromeShellUri().get(), nsIWebBrowserChrome::CHROME_ALL,
          aWidth, aHeight,
          getter_AddRefs(window)))) {
    printf("CreateTopLevelWindow failed\n");
    return 1;
  }
  nsCOMPtr<nsIBaseWindow> baseWindow = do_QueryInterface(window);
  baseWindow->SetVisibility(true);
  nsCOMPtr<nsIWidget> mainWidget;
  baseWindow->GetMainWidget(getter_AddRefs(mainWidget));
  if (mainWidget) {
    mainWidget->SetFocus(nsIWidget::Raise::Yes, dom::CallerType::System);
  }
  nsCOMPtr<nsIDocShell> docShell = CreateContentDocShell(window);
  nsCOMPtr<nsIURI> uri;
  if (NS_FAILED(NS_NewURI(getter_AddRefs(uri), aUrl))) {
    printf("bad page url %s\n", aUrl);
    return 1;
  }
  dom::LoadURIOptions options;
  options.mTriggeringPrincipal = nsContentUtils::GetSystemPrincipal();
  nsCOMPtr<nsIWebNavigation> navigation = do_QueryInterface(docShell);
  if (NS_FAILED(navigation->LoadURI(uri, options))) {
    printf("LoadURI failed\n");
    return 1;
  }
  printf("stage: window open\n");
  if (PR_GetEnv("VGE_STALL_PROBE")) {
    RefPtr<StallProbe> probe = new StallProbe();
    nsCOMPtr<nsIThreadInternal> mainThread =
        do_QueryInterface(NS_GetCurrentThread());
    mainThread->AddObserver(probe);
    probe->Start();
    probe.forget().leak();
  }
  bool reported = false;
  SpinEventLoopUntil("vge-bedrock-window"_ns, [&] {
    nsCOMPtr<nsIDocShell> current;
    window->GetDocShell(getter_AddRefs(current));
    if (!current) {
      return true;
    }
    Document* document = docShell->GetDocument();
    if (!reported && document &&
        document->GetReadyStateEnum() == Document::READYSTATE_COMPLETE &&
        !NS_IsAboutBlank(document->GetDocumentURI())) {
      reported = true;
      nsAutoString title;
      document->GetTitle(title);
      printf("stage: window page loaded, title: %s\n",
             NS_ConvertUTF16toUTF8(title).get());
      PrintFrameGeometry(document);
      if (const char* probePath = PR_GetEnv("VGE_FRAME_PROBE")) {
        nsCOMPtr<nsIFile> probeFile;
        nsCOMPtr<nsIInputStream> probeStream;
        nsCString probeSource;
        if (NS_SUCCEEDED(XRE_GetFileFromPath(probePath,
                                             getter_AddRefs(probeFile))) &&
            NS_SUCCEEDED(NS_NewLocalFileInputStream(
                getter_AddRefs(probeStream), probeFile)) &&
            NS_SUCCEEDED(
                NS_ConsumeStream(probeStream, UINT32_MAX, probeSource))) {
          RefPtr<Element> script =
              document->CreateHTMLElement(nsGkAtoms::script);
          script->SetTextContent(NS_ConvertUTF8toUTF16(probeSource),
                                 IgnoreErrors());
          document->GetRootElement()->AppendChildTo(script, true,
                                                    IgnoreErrors());
        }
      }
      ScheduleMemoryReport();
  ScheduleMemoryPressure();
  ColdMemoryTrimmer::Start();
      ScheduleAllocTraceReport();
      ScheduleProfileSave();
      gfx::gfxConfig::ForEachFeature(
          [](const char* aName, const char* aDescription,
             gfx::FeatureState& aFeature) {
            printf("gfx feature %s: %s %s\n", aName,
                   gfx::FeatureStatusToString(aFeature.GetValue()),
                   aFeature.GetFailureMessage() ? aFeature.GetFailureMessage()
                                                : "");
          });
    }
    return false;
  });
  return 0;
}

int RunLayoutTest(const char* aPagePath, int aWidth, int aHeight) {
  nsCOMPtr<nsIAppShellService> appShell =
      do_GetService(NS_APPSHELLSERVICE_CONTRACTID);
  nsCOMPtr<nsIAppWindow> window;
  if (NS_FAILED(appShell->CreateTopLevelWindow(
          nullptr, ChromeShellUri().get(), nsIWebBrowserChrome::CHROME_ALL,
          aWidth, aHeight,
          getter_AddRefs(window)))) {
    printf("CreateTopLevelWindow failed\n");
    return 1;
  }
  printf("stage: window\n");
  nsCOMPtr<nsIBaseWindow> baseWindow = do_QueryInterface(window);
  baseWindow->SetVisibility(true);
  nsCOMPtr<nsIDocShell> docShell = CreateContentDocShell(window);

  nsCOMPtr<nsIURI> uri;
  if (!strncmp(aPagePath, "http", 4)) {
    if (NS_FAILED(NS_NewURI(getter_AddRefs(uri), aPagePath))) {
      printf("bad page url %s\n", aPagePath);
      return 1;
    }
  } else {
    nsCOMPtr<nsIFile> pageFile;
    if (NS_FAILED(XRE_GetFileFromPath(aPagePath, getter_AddRefs(pageFile)))) {
      printf("bad page path %s\n", aPagePath);
      return 1;
    }
    NS_NewFileURI(getter_AddRefs(uri), pageFile);
  }

  dom::LoadURIOptions options;
  options.mTriggeringPrincipal = nsContentUtils::GetSystemPrincipal();
  nsCOMPtr<nsIWebNavigation> navigation = do_QueryInterface(docShell);
  if (NS_FAILED(navigation->LoadURI(uri, options))) {
    printf("LoadURI failed\n");
    return 1;
  }

  RefPtr<Document> document;
  SpinEventLoopUntil("vge-bedrock-layout-test"_ns, [&] {
    document = docShell->GetDocument();
    if (!document) {
      return false;
    }
    bool equals = false;
    return NS_SUCCEEDED(document->GetDocumentURI()->EqualsExceptRef(uri, &equals)) &&
           equals &&
           document->GetReadyStateEnum() == Document::READYSTATE_COMPLETE;
  });

  nsAutoString pageTitle;
  document->GetTitle(pageTitle);
  printf("stage: page loaded, title: %s\n",
         NS_ConvertUTF16toUTF8(pageTitle).get());
  TimeStamp waitStart = TimeStamp::Now();
  SpinEventLoopUntil("vge-bedrock-page-done"_ns, [&] {
    Element* root = document->GetRootElement();
    return (root && root->HasAttribute(u"data-vge-done"_ns)) ||
           (TimeStamp::Now() - waitStart).ToSeconds() > 5.0;
  });
  Element* root = document->GetRootElement();
  bool pageDone = root && root->HasAttribute(u"data-vge-done"_ns);
  printf("stage: page %s\n", pageDone ? "done" : "timed out");
  nsAutoString pageFailures;
  if (root) {
    root->GetAttribute(u"data-vge-failures"_ns, pageFailures);
  }
  bool pagePassed = pageDone && root->HasAttribute(u"data-vge-failures"_ns) &&
                    pageFailures.IsEmpty();
  printf("page checks: %s\n",
         pagePassed ? "passed" : NS_ConvertUTF16toUTF8(pageFailures).get());
  document->FlushPendingNotifications(FlushType::Layout);
  PrintFrameGeometry(document);

  nsJSContext::GarbageCollectNow(JS::GCReason::API);
  TimeStamp liveCycleCollectStart = TimeStamp::Now();
  nsJSContext::CycleCollectNow(CCReason::API);
  printf("cycle collection with live page: %.1f ms\n",
         (TimeStamp::Now() - liveCycleCollectStart).ToMilliseconds());
  bool expectsFinalization =
      root && root->HasAttribute(u"data-vge-expect-finalization"_ns);
  TimeStamp cleanupStart = TimeStamp::Now();
  SpinEventLoopUntil("vge-bedrock-finalization-cleanup"_ns, [&] {
    Element* element = document->GetRootElement();
    return !expectsFinalization ||
           (element && element->HasAttribute(u"data-vge-finalized"_ns)) ||
           (TimeStamp::Now() - cleanupStart).ToSeconds() > 5.0;
  });
  nsAutoString finalized;
  root = document->GetRootElement();
  bool finalizationRan = root && root->HasAttribute(u"data-vge-finalized"_ns);
  if (finalizationRan) {
    root->GetAttribute(u"data-vge-finalized"_ns, finalized);
    printf("finalization registry cleanup: ran, held value %s\n",
           NS_ConvertUTF16toUTF8(finalized).get());
  } else {
    printf("finalization registry cleanup: did not run\n");
  }

  JSContext* cx = dom::danger::GetJSContext();
  sPageRealm = JS::GetObjectRealmOrNull(
      document->GetScopeObject()->GetGlobalJSObjectPreserveColor());
  sForwardDestroyRealm = JS_GetRuntime(cx)->destroyRealmCallback;
  JS::SetDestroyRealmCallback(cx, CountDestroyedRealm);

  root = nullptr;
  document = nullptr;
  navigation = nullptr;
  docShell = nullptr;
  baseWindow->Destroy();
  baseWindow = nullptr;
  window = nullptr;
  printf("stage: window closed\n");
  for (int round = 0; round < 10 && !sPageRealmDestroyed; round++) {
    printf("stage: collect round %d\n", round);
    NS_ProcessPendingEvents(nullptr, PR_MillisecondsToInterval(100));
    nsJSContext::GarbageCollectNow(JS::GCReason::API);
    TimeStamp cycleCollectStart = TimeStamp::Now();
    nsJSContext::CycleCollectNow(CCReason::API);
    printf("cycle collection: %.1f ms\n",
           (TimeStamp::Now() - cycleCollectStart).ToMilliseconds());
    nsJSContext::GarbageCollectNow(JS::GCReason::API);
  }
  bool listed = false;
  for (JS::Realm* realm : JS_GetRuntime(cx)->realms) {
    listed = listed || realm == sPageRealm;
  }
  printf("page realm destroy callbacks: %u\n", sPageRealmDestroyed);
  printf("page realm still listed: %s\n", listed ? "yes" : "no");
  bool remotePage = !strncmp(aPagePath, "http", 4);
  bool passed = (remotePage || pagePassed) &&
                (!expectsFinalization || finalizationRan) &&
                sPageRealmDestroyed && !listed;
  printf("test result: %s\n", passed ? "PASS" : "FAIL");
  return passed ? 0 : 1;
}

}

}

int VgeBedrock_RunLayoutTest(int aArgc, char** aArgv) {
  if (aArgc < 2) {
    printf("usage: VgeBedrockTest <page.html>\n");
    return 2;
  }

  setvbuf(stdout, nullptr, _IONBF, 0);
  vge::bedrock::InstallAllocTrace();
  printf("stage: start\n");
  NS_LogInit();
  LogModule::Init(aArgc, aArgv);
  AutoIOInterposer ioInterposerGuard;
  ioInterposerGuard.Init();
  XRE_InitCommandLine(aArgc, aArgv);
  char stackTop;
  profiler_init(&stackTop);
  mscom::ProcessRuntime mscom;
  AutoSQLiteLifetime sqliteLifetime;

  vge::bedrock::RuntimeDirProvider dirProvider;
  int result = 1;
  {
    nsCOMPtr<nsIFile> binaryFile;
    if (NS_FAILED(XRE_GetBinaryPath(getter_AddRefs(binaryFile)))) {
      printf("no binary path\n");
      return 1;
    }
    dirProvider.SetBinaryFile(binaryFile);
    if (NS_FAILED(NS_InitXPCOM(nullptr, dirProvider.GreDir(), &dirProvider))) {
      printf("NS_InitXPCOM failed\n");
      return 1;
    }
    printf("stage: xpcom ready\n");
    Preferences::SetBool("devtools.console.stdout.content", true);
    nsCOMPtr<nsIWindowCreator> windowCreator =
        do_GetService("@mozilla.org/toolkit/app-startup;1");
    nsCOMPtr<nsIWindowWatcher> windowWatcher =
        do_GetService(NS_WINDOWWATCHER_CONTRACTID);
    windowWatcher->SetWindowCreator(windowCreator);
    nsCOMPtr<nsIObserverService> observers = services::GetObserverService();
    observers->NotifyObservers(nullptr, "profile-do-change", u"startup");
    observers->NotifyObservers(nullptr, "profile-after-change", u"startup");
    Preferences::SetBool("webgl.allow-in-parent", true);
    Preferences::SetUint("webgl.out-of-process.shmem-size", 1024 * 1024);
    Preferences::SetInt("browser.link.open_newwindow",
                        nsIBrowserDOMWindow::OPEN_CURRENTWINDOW);
    Preferences::SetInt("browser.link.open_newwindow.restriction", 0);
    gfxPlatform::GetPlatform();
    gfx::GPUProcessManager::Get()->EnsureProtocolsReady();
    result = aArgc > 2 && !strcmp(aArgv[1], "--window")
                 ? vge::bedrock::RunWindow(aArgv[2], 1280, 800)
                 : vge::bedrock::RunLayoutTest(aArgv[1], 1280, 720);
    observers->NotifyObservers(nullptr, "profile-before-change", u"shutdown");
    observers->NotifyObservers(nullptr, "profile-before-change-qm",
                               u"shutdown");
  }
  NS_ShutdownXPCOM(nullptr);
  profiler_shutdown();
  XRE_DeinitCommandLine();
  return result;
}
