#ifndef vge_bedrock_VgeBedrockScriptHook_h
#define vge_bedrock_VgeBedrockScriptHook_h

#include "nsError.h"
#include "nsStringFwd.h"

namespace mozilla::dom {
class Document;
}

namespace vge::bedrock {

bool PageScriptsRunInV8(mozilla::dom::Document* aDocument);

nsresult EvaluatePageScript(mozilla::dom::Document* aDocument,
                            const nsAString& aSource, const nsACString& aURL,
                            uint32_t aLine);

void ShutdownPageScripts();

}

#endif
