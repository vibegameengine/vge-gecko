#ifndef vge_bedrock_jsapi_objects_VgeJsObjectsInternal_h
#define vge_bedrock_jsapi_objects_VgeJsObjectsInternal_h

#include "VgeJsInternal.h"

#include <string>

#include "js/PropertyDescriptor.h"
#include "js/Symbol.h"

namespace vge::jsapi::objects {

enum class IntrinsicId : uint32_t {
  ReflectGet,
  ReflectSet,
  ReflectConstruct,
  ReflectDefineProperty,
  JsonStringify,
  MapEntries,
  MapKeys,
  MapValues,
  MapForEach,
  SetEntries,
  SetKeys,
  SetValues,
  SetForEach,
  WeakMapConstructor,
  WeakMapGet,
  WeakMapSet,
  ArrayValues,
  ArrayIteratorNext,
  SymbolConstructor,
  Limit
};

v8::Local<v8::Value> Intrinsic(JSContext* aCx, IntrinsicId aId);

v8::Local<v8::Name> NameOf(JSContext* aCx, JS::HandleId aId);

bool IdFromChars(JSContext* aCx, const char16_t* aChars, size_t aLength,
                 JS::MutableHandleId aId);
bool IdFromLatin1(JSContext* aCx, const char* aChars, size_t aLength,
                  JS::MutableHandleId aId);

JSString* AtomizeChars(JSContext* aCx, const char16_t* aChars, size_t aLength);
JSString* AtomizeLatin1(JSContext* aCx, const char* aChars, size_t aLength);

JSString* IdToFunctionName(JSContext* aCx, JS::HandleId aId,
                           const char* aPrefix);

std::u16string StringContent(JSString* aStr);
std::string IdToUtf8(JSContext* aCx, JS::HandleId aId);
std::string Utf16ToUtf8(const char16_t* aChars, size_t aLength);

bool ReportOpFailure(JSContext* aCx, JS::HandleId aId, uint32_t aCode);

bool DescriptorFromV8(JSContext* aCx, v8::Local<v8::Value> aDesc,
                      JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>>
                          aResult);

bool BuiltinClassOf(JSContext* aCx, JS::HandleObject aObj, js::ESClass* aCls);

bool IsPlainV8Object(v8::Local<v8::Object> aObject);

void MarkForBarrier(const JS::Value& aValue);

JS::Symbol* WellKnownSymbol(JSContext* aCx, JS::SymbolCode aWhich);

}

#endif
