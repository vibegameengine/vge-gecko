#include "vge/bedrock/VgeBedrockScriptHook.h"

#include <stdio.h>

#include "mozilla/Preferences.h"
#include "mozilla/dom/Document.h"
#include "nsContentUtils.h"
#include "nsGlobalWindowInner.h"
#include "vge/bedrock/VgeBedrockV8.h"
#include "vge/bedrock/VgeV8BindingSupport.h"
#include "vge/bedrock/VgeV8Events.h"

namespace vge::bedrock {

bool PageScriptsRunInV8(mozilla::dom::Document* aDocument) {
  return aDocument &&
         mozilla::Preferences::GetBool("vge.bedrock.v8_page_scripts", false) &&
         !nsContentUtils::IsChromeDoc(aDocument);
}

nsresult EvaluatePageScript(mozilla::dom::Document* aDocument,
                            const nsAString& aSource, const nsACString& aURL,
                            uint32_t aLine) {
  nsGlobalWindowInner* window =
      nsGlobalWindowInner::Cast(aDocument->GetInnerWindow());
  if (!window) {
    return NS_OK;
  }
  v8::Isolate* isolate = V8Engine::Isolate();
  v8::HandleScope handleScope(isolate);
  v8::Local<v8::Context> context = v8b::WindowContext(isolate, window);
  v8::Context::Scope contextScope(context);

  v8::Local<v8::Value> source = v8b::FromString(isolate, aSource);
  v8::Local<v8::Value> name = v8b::FromUTF8String(isolate, aURL);
  v8::ScriptOrigin origin(isolate, name, aLine > 0 ? int(aLine) - 1 : 0);
  v8::TryCatch tryCatch(isolate);
  v8::Local<v8::Script> script;
  v8::Local<v8::Value> result;
  if (!v8::Script::Compile(context, source.As<v8::String>(), &origin)
           .ToLocal(&script) ||
      !script->Run(context).ToLocal(&result)) {
    v8::String::Utf8Value error(isolate, tryCatch.Exception());
    int line = 0;
    v8::Local<v8::Message> message = tryCatch.Message();
    if (!message.IsEmpty()) {
      line = message->GetLineNumber(context).FromMaybe(0);
    }
    printf("Uncaught %s (%s:%d)\n", *error ? *error : "<exception>",
           PromiseFlatCString(aURL).get(), line);
  }
  return NS_OK;
}

void ShutdownPageScripts() {
  v8b::ReleaseEventState();
  v8b::ReleaseAllWrappers();
}

}
