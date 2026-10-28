#include "vge/bedrock/VgeBedrockV8.h"

#include "v8.h"

namespace vge::jsapi {
v8::Isolate* MainIsolate();
}

namespace vge::bedrock {

v8::Isolate* V8Engine::Isolate() { return vge::jsapi::MainIsolate(); }

void V8Engine::Shutdown() {}

}
