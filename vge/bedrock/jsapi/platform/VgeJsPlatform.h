#ifndef vge_bedrock_jsapi_platform_VgeJsPlatform_h
#define vge_bedrock_jsapi_platform_VgeJsPlatform_h

#include <memory>

namespace v8 {
class Isolate;
class Platform;
}

namespace vge::jsapi {

std::unique_ptr<v8::Platform> NewEventLoopPlatform();

void AttachIsolateToCurrentThread(v8::Platform* aPlatform,
                                  v8::Isolate* aIsolate);

void DetachIsolate(v8::Platform* aPlatform, v8::Isolate* aIsolate);

}

#endif
