#ifndef vge_bedrock_VgeBedrockV8_h
#define vge_bedrock_VgeBedrockV8_h

#include "nsString.h"

namespace v8 {
class Isolate;
}

namespace vge::bedrock {

class V8Engine {
 public:
  static v8::Isolate* Isolate();
  static void Shutdown();
};

}

#endif
