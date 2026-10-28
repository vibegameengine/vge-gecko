#ifndef vge_bedrock_VgeV8Config_h
#define vge_bedrock_VgeV8Config_h

#define CPPGC_CAGED_HEAP 1
#define CPPGC_POINTER_COMPRESSION 1
#define CPPGC_YOUNG_GENERATION 1

#include "v8config.h"

#undef V8_PRESERVE_MOST
#define V8_PRESERVE_MOST

#endif
