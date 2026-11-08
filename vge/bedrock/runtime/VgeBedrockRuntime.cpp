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
#include "nsGkAtoms.h"
#include "nsGlobalWindowOuter.h"
#include "nsIBrowserDOMWindow.h"
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
  printf("stage: opener where=%d\n", int(aWhere));
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
  nsCOMPtr<nsIBrowserDOMWindow> opener = new SingleWindowOpener(content);
  nsGlobalWindowOuter::Cast(chromeDocument->GetWindow())
      ->SetBrowserDOMWindowOuter(opener);
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
