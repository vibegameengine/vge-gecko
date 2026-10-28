#include "VgeJsObjectsInternal.h"

#include <string>
#include <vector>

#include "cppgc/heap-state.h"
#include "js/Array.h"
#include "js/CallAndConstruct.h"
#include "js/CallArgs.h"
#include "js/CallNonGenericMethod.h"
#include "js/Class.h"
#include "js/Conversions.h"
#include "js/Id.h"
#include "js/Object.h"
#include "js/PropertyAndElement.h"
#include "js/PropertyDescriptor.h"
#include "js/PropertySpec.h"
#include "js/Proxy.h"
#include "js/Value.h"
#include "js/ValueArray.h"
#include "js/friend/ErrorMessages.h"

namespace vge::jsapi {

const JSClass ArrayClass = {"Array", JSCLASS_HAS_CACHED_PROTO(JSProto_Array)};

}

using namespace vge::jsapi;
using namespace vge::jsapi::objects;

namespace vge::jsapi::objects {

namespace {

v8::Local<v8::Private> IntrinsicsKey(v8::Isolate* aIsolate) {
  return v8::Private::ForApi(
      aIsolate, v8::String::NewFromUtf8Literal(aIsolate,
                                               "vge.jsapi.objects.intrinsics"));
}

v8::Local<v8::Value> GetNamed(v8::Local<v8::Context> aCtx,
                              v8::Local<v8::Value> aBase, const char* aName) {
  if (aBase.IsEmpty() || !aBase->IsObject()) {
    return v8::Local<v8::Value>();
  }
  v8::Isolate* isolate = aCtx->GetIsolate();
  v8::Local<v8::String> key =
      v8::String::NewFromUtf8(isolate, aName).ToLocalChecked();
  v8::Local<v8::Value> out;
  if (!aBase.As<v8::Object>()->Get(aCtx, key).ToLocal(&out)) {
    return v8::Local<v8::Value>();
  }
  return out;
}

v8::Local<v8::Array> CaptureIntrinsics(v8::Local<v8::Context> aCtx) {
  v8::Isolate* isolate = aCtx->GetIsolate();
  v8::TryCatch tc(isolate);
  v8::Local<v8::Array> store = v8::Array::New(isolate, int(IntrinsicId::Limit));
  auto put = [&](IntrinsicId aId, v8::Local<v8::Value> aValue) {
    if (aValue.IsEmpty()) {
      aValue = v8::Undefined(isolate);
    }
    store->Set(aCtx, uint32_t(aId), aValue).FromMaybe(false);
  };
  v8::Local<v8::Object> global = aCtx->Global();
  v8::Local<v8::Value> reflect = GetNamed(aCtx, global, "Reflect");
  put(IntrinsicId::ReflectGet, GetNamed(aCtx, reflect, "get"));
  put(IntrinsicId::ReflectSet, GetNamed(aCtx, reflect, "set"));
  put(IntrinsicId::ReflectConstruct, GetNamed(aCtx, reflect, "construct"));
  put(IntrinsicId::ReflectDefineProperty,
      GetNamed(aCtx, reflect, "defineProperty"));
  put(IntrinsicId::JsonStringify,
      GetNamed(aCtx, GetNamed(aCtx, global, "JSON"), "stringify"));
  v8::Local<v8::Value> mapProto = v8::Map::New(isolate)->GetPrototype();
  put(IntrinsicId::MapEntries, GetNamed(aCtx, mapProto, "entries"));
  put(IntrinsicId::MapKeys, GetNamed(aCtx, mapProto, "keys"));
  put(IntrinsicId::MapValues, GetNamed(aCtx, mapProto, "values"));
  put(IntrinsicId::MapForEach, GetNamed(aCtx, mapProto, "forEach"));
  v8::Local<v8::Value> setProto = v8::Set::New(isolate)->GetPrototype();
  put(IntrinsicId::SetEntries, GetNamed(aCtx, setProto, "entries"));
  put(IntrinsicId::SetKeys, GetNamed(aCtx, setProto, "keys"));
  put(IntrinsicId::SetValues, GetNamed(aCtx, setProto, "values"));
  put(IntrinsicId::SetForEach, GetNamed(aCtx, setProto, "forEach"));
  v8::Local<v8::Value> weakMap = GetNamed(aCtx, global, "WeakMap");
  v8::Local<v8::Value> weakMapProto = GetNamed(aCtx, weakMap, "prototype");
  put(IntrinsicId::WeakMapConstructor, weakMap);
  put(IntrinsicId::WeakMapGet, GetNamed(aCtx, weakMapProto, "get"));
  put(IntrinsicId::WeakMapSet, GetNamed(aCtx, weakMapProto, "set"));
  v8::Local<v8::Array> emptyArray = v8::Array::New(isolate);
  v8::Local<v8::Value> arrayValues =
      GetNamed(aCtx, emptyArray->GetPrototype(), "values");
  put(IntrinsicId::ArrayValues, arrayValues);
  v8::Local<v8::Value> arrayIterNext;
  if (!arrayValues.IsEmpty() && arrayValues->IsFunction()) {
    v8::Local<v8::Value> iter;
    if (arrayValues.As<v8::Function>()
            ->Call(aCtx, emptyArray, 0, nullptr)
            .ToLocal(&iter) &&
        iter->IsObject()) {
      arrayIterNext =
          GetNamed(aCtx, iter.As<v8::Object>()->GetPrototype(), "next");
    }
  }
  put(IntrinsicId::ArrayIteratorNext, arrayIterNext);
  put(IntrinsicId::SymbolConstructor, GetNamed(aCtx, global, "Symbol"));
  return store;
}

}

v8::Local<v8::Value> Intrinsic(JSContext* aCx, IntrinsicId aId) {
  v8::Isolate* isolate = aCx->isolate;
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  if (ctx.IsEmpty()) {
    ctx = aCx->v8Context();
  }
  v8::Local<v8::Object> global = ctx->Global();
  v8::Local<v8::Private> key = IntrinsicsKey(isolate);
  v8::Local<v8::Value> existing;
  v8::Local<v8::Array> store;
  if (global->GetPrivate(ctx, key).ToLocal(&existing) && existing->IsArray()) {
    store = existing.As<v8::Array>();
  } else {
    store = CaptureIntrinsics(ctx);
    global->SetPrivate(ctx, key, store).FromMaybe(false);
  }
  v8::Local<v8::Value> out;
  if (!store->Get(ctx, uint32_t(aId)).ToLocal(&out)) {
    return v8::Undefined(isolate);
  }
  return out;
}

v8::Local<v8::Name> NameOf(JSContext* aCx, JS::HandleId aId) {
  if (aId.isInt()) {
    std::string digits = std::to_string(aId.toInt());
    return v8::String::NewFromOneByte(
               aCx->isolate, reinterpret_cast<const uint8_t*>(digits.data()),
               v8::NewStringType::kInternalized, int(digits.size()))
        .ToLocalChecked();
  }
  if (aId.isSymbol()) {
    return ToV8(aCx, aId.toSymbol());
  }
  if (aId.isString()) {
    return ToV8(aCx, aId.toString());
  }
  return v8::String::Empty(aCx->isolate);
}

bool IdFromChars(JSContext* aCx, const char16_t* aChars, size_t aLength,
                 JS::MutableHandleId aId) {
  v8::HandleScope scope(aCx->isolate);
  v8::Local<v8::String> str;
  if (!v8::String::NewFromTwoByte(aCx->isolate,
                                  reinterpret_cast<const uint16_t*>(aChars),
                                  v8::NewStringType::kInternalized,
                                  int(aLength))
           .ToLocal(&str)) {
    JS_ReportOutOfMemory(aCx);
    return false;
  }
  return IdFromV8(aCx, str, aId);
}

bool IdFromLatin1(JSContext* aCx, const char* aChars, size_t aLength,
                  JS::MutableHandleId aId) {
  v8::HandleScope scope(aCx->isolate);
  v8::Local<v8::String> str;
  if (!v8::String::NewFromOneByte(aCx->isolate,
                                  reinterpret_cast<const uint8_t*>(aChars),
                                  v8::NewStringType::kInternalized,
                                  int(aLength))
           .ToLocal(&str)) {
    JS_ReportOutOfMemory(aCx);
    return false;
  }
  return IdFromV8(aCx, str, aId);
}

JSString* AtomizeChars(JSContext* aCx, const char16_t* aChars,
                       size_t aLength) {
  v8::HandleScope scope(aCx->isolate);
  v8::Local<v8::String> str;
  if (!v8::String::NewFromTwoByte(aCx->isolate,
                                  reinterpret_cast<const uint16_t*>(aChars),
                                  v8::NewStringType::kInternalized,
                                  int(aLength))
           .ToLocal(&str)) {
    JS_ReportOutOfMemory(aCx);
    return nullptr;
  }
  return AtomFromV8(aCx, str);
}

JSString* AtomizeLatin1(JSContext* aCx, const char* aChars, size_t aLength) {
  v8::HandleScope scope(aCx->isolate);
  v8::Local<v8::String> str;
  if (!v8::String::NewFromOneByte(aCx->isolate,
                                  reinterpret_cast<const uint8_t*>(aChars),
                                  v8::NewStringType::kInternalized,
                                  int(aLength))
           .ToLocal(&str)) {
    JS_ReportOutOfMemory(aCx);
    return nullptr;
  }
  return AtomFromV8(aCx, str);
}

std::u16string StringContent(JSString* aStr) {
  StringCell* cell = StringCell::from(aStr);
  uint32_t length = cell->length();
  std::u16string out;
  out.resize(length);
  if (cell->latin1()) {
    const uint8_t* chars = static_cast<const uint8_t*>(cell->chars);
    for (uint32_t i = 0; i < length; i++) {
      out[i] = chars[i];
    }
  } else if (length) {
    memcpy(out.data(), cell->chars, length * sizeof(char16_t));
  }
  return out;
}

std::string Utf16ToUtf8(const char16_t* aChars, size_t aLength) {
  std::string out;
  out.reserve(aLength);
  for (size_t i = 0; i < aLength; i++) {
    uint32_t c = aChars[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < aLength &&
        aChars[i + 1] >= 0xDC00 && aChars[i + 1] <= 0xDFFF) {
      c = 0x10000 + ((c - 0xD800) << 10) + (aChars[i + 1] - 0xDC00);
      i++;
    } else if (c >= 0xD800 && c <= 0xDFFF) {
      c = 0xFFFD;
    }
    if (c < 0x80) {
      out.push_back(char(c));
    } else if (c < 0x800) {
      out.push_back(char(0xC0 | (c >> 6)));
      out.push_back(char(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
      out.push_back(char(0xE0 | (c >> 12)));
      out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
      out.push_back(char(0x80 | (c & 0x3F)));
    } else {
      out.push_back(char(0xF0 | (c >> 18)));
      out.push_back(char(0x80 | ((c >> 12) & 0x3F)));
      out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
      out.push_back(char(0x80 | (c & 0x3F)));
    }
  }
  return out;
}

std::string IdToUtf8(JSContext* aCx, JS::HandleId aId) {
  if (aId.isInt()) {
    return std::to_string(aId.toInt());
  }
  if (aId.isString()) {
    std::u16string s = StringContent(aId.toString());
    return Utf16ToUtf8(s.data(), s.size());
  }
  if (aId.isSymbol()) {
    v8::HandleScope scope(aCx->isolate);
    v8::Local<v8::Value> desc =
        ToV8(aCx, aId.toSymbol())->Description(aCx->isolate);
    std::string out = "Symbol(";
    if (desc->IsString()) {
      v8::String::Value chars(aCx->isolate, desc);
      out += Utf16ToUtf8(reinterpret_cast<const char16_t*>(*chars),
                         size_t(chars.length()));
    }
    out += ")";
    return out;
  }
  return std::string();
}

JSString* IdToFunctionName(JSContext* aCx, JS::HandleId aId,
                           const char* aPrefix) {
  if (!*aPrefix && aId.isString()) {
    return aId.toString();
  }
  std::u16string name;
  for (const char* p = aPrefix; *p; p++) {
    name.push_back(char16_t(uint8_t(*p)));
  }
  if (aId.isInt()) {
    std::string digits = std::to_string(aId.toInt());
    for (char c : digits) {
      name.push_back(char16_t(c));
    }
  } else if (aId.isString()) {
    name += StringContent(aId.toString());
  } else if (aId.isSymbol()) {
    v8::HandleScope scope(aCx->isolate);
    v8::Local<v8::Value> desc =
        ToV8(aCx, aId.toSymbol())->Description(aCx->isolate);
    if (desc->IsString()) {
      v8::String::Value chars(aCx->isolate, desc);
      name.push_back(u'[');
      name.append(reinterpret_cast<const char16_t*>(*chars),
                  size_t(chars.length()));
      name.push_back(u']');
    }
  }
  return AtomizeChars(aCx, name.data(), name.size());
}

bool ReportOpFailure(JSContext* aCx, JS::HandleId aId, uint32_t aCode) {
  std::string name = IdToUtf8(aCx, aId);
  const char* args[JS::MaxNumErrorArguments + 1];
  for (auto& arg : args) {
    arg = name.c_str();
  }
  args[JS::MaxNumErrorArguments] = nullptr;
  JS_ReportErrorNumberUTF8Array(aCx, js::GetErrorMessage, nullptr, aCode,
                                args);
  return false;
}

bool DescriptorFromV8(
    JSContext* aCx, v8::Local<v8::Value> aDesc,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> aResult) {
  if (aDesc.IsEmpty() || !aDesc->IsObject()) {
    aResult.set(mozilla::Nothing());
    return true;
  }
  v8::Isolate* isolate = aCx->isolate;
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  v8::Local<v8::Object> desc = aDesc.As<v8::Object>();
  auto field = [&](const char* aName, v8::Local<v8::Value>* aOut) {
    v8::Local<v8::String> key =
        v8::String::NewFromUtf8(isolate, aName).ToLocalChecked();
    if (!desc->HasOwnProperty(ctx, key).FromMaybe(false)) {
      return false;
    }
    return desc->Get(ctx, key).ToLocal(aOut);
  };
  v8::Local<v8::Value> configurable, enumerable, get, set, value, writable;
  unsigned attrs = 0;
  if (!(field("configurable", &configurable) &&
        configurable->BooleanValue(isolate))) {
    attrs |= JSPROP_PERMANENT;
  }
  if (field("enumerable", &enumerable) && enumerable->BooleanValue(isolate)) {
    attrs |= JSPROP_ENUMERATE;
  }
  bool hasGet = field("get", &get);
  bool hasSet = field("set", &set);
  if (hasGet || hasSet) {
    JS::Rooted<JSObject*> getter(aCx);
    JS::Rooted<JSObject*> setter(aCx);
    if (hasGet && get->IsObject()) {
      getter = ObjectFromV8(aCx, get.As<v8::Object>());
    }
    if (hasSet && set->IsObject()) {
      setter = ObjectFromV8(aCx, set.As<v8::Object>());
    }
    aResult.set(mozilla::Some(
        JS::PropertyDescriptor::Accessor(getter, setter, attrs)));
    return true;
  }
  if (!(field("writable", &writable) && writable->BooleanValue(isolate))) {
    attrs |= JSPROP_READONLY;
  }
  JS::Rooted<JS::Value> v(aCx);
  if (field("value", &value)) {
    v = ValueFromV8(aCx, value);
  }
  aResult.set(mozilla::Some(JS::PropertyDescriptor::Data(v, attrs)));
  return true;
}

bool IsPlainV8Object(v8::Local<v8::Object> aObject) {
  if (aObject->IsArray() || aObject->IsFunction() || aObject->IsDate() ||
      aObject->IsRegExp() || aObject->IsMap() || aObject->IsSet() ||
      aObject->IsWeakMap() || aObject->IsWeakSet() || aObject->IsPromise() ||
      aObject->IsProxy() || aObject->IsArrayBuffer() ||
      aObject->IsArrayBufferView() || aObject->IsSharedArrayBuffer() ||
      aObject->IsBooleanObject() || aObject->IsNumberObject() ||
      aObject->IsStringObject() || aObject->IsSymbolObject() ||
      aObject->IsBigIntObject() || aObject->IsNativeError() ||
      aObject->IsArgumentsObject() || aObject->IsGeneratorObject() ||
      aObject->IsMapIterator() || aObject->IsSetIterator() ||
      aObject->IsModuleNamespaceObject() || aObject->IsWasmMemoryObject() ||
      aObject->IsWasmModuleObject() || aObject->IsApiWrapper() ||
      aObject->IsUndetectable() || aObject->InternalFieldCount() > 0) {
    return false;
  }
  return true;
}

bool BuiltinClassOf(JSContext* aCx, JS::HandleObject aObj, js::ESClass* aCls) {
  if (js::IsProxy(aObj)) {
    return js::GetProxyHandler(aObj)->getBuiltinClass(aCx, aObj, aCls);
  }
  v8::HandleScope scope(aCx->isolate);
  v8::Local<v8::Object> o = ToV8(aCx, aObj.get());
  using js::ESClass;
  if (o->IsArray()) {
    *aCls = ESClass::Array;
  } else if (o->IsNumberObject()) {
    *aCls = ESClass::Number;
  } else if (o->IsStringObject()) {
    *aCls = ESClass::String;
  } else if (o->IsBooleanObject()) {
    *aCls = ESClass::Boolean;
  } else if (o->IsRegExp()) {
    *aCls = ESClass::RegExp;
  } else if (o->IsArrayBuffer()) {
    *aCls = ESClass::ArrayBuffer;
  } else if (o->IsSharedArrayBuffer()) {
    *aCls = ESClass::SharedArrayBuffer;
  } else if (o->IsDate()) {
    *aCls = ESClass::Date;
  } else if (o->IsSet()) {
    *aCls = ESClass::Set;
  } else if (o->IsMap()) {
    *aCls = ESClass::Map;
  } else if (o->IsPromise()) {
    *aCls = ESClass::Promise;
  } else if (o->IsMapIterator()) {
    *aCls = ESClass::MapIterator;
  } else if (o->IsSetIterator()) {
    *aCls = ESClass::SetIterator;
  } else if (o->IsArgumentsObject()) {
    *aCls = ESClass::Arguments;
  } else if (o->IsNativeError()) {
    *aCls = ESClass::Error;
  } else if (o->IsBigIntObject()) {
    *aCls = ESClass::BigInt;
  } else if (o->IsFunction()) {
    *aCls = ESClass::Function;
  } else if (JS::GetClass(aObj) == js::ObjectClassPtr && IsPlainV8Object(o)) {
    *aCls = ESClass::Object;
  } else {
    *aCls = ESClass::Other;
  }
  return true;
}

void MarkForBarrier(const JS::Value& aValue) {
  if (!aValue.isGCThing()) {
    return;
  }
  JSRuntime* rt = Runtime();
  if (!rt || !rt->cppHeap ||
      !cppgc::subtle::HeapState::IsMarking(rt->cppHeap->GetHeapHandle())) {
    return;
  }
  JS::GCCellPtr thing = aValue.toGCCellPtr();
  CellTail* tail = TailOf(thing.asCell(), thing.kind());
  if (!tail || tail->handle.IsEmpty()) {
    return;
  }
  v8::HandleScope scope(rt->isolate);
  v8::TracedReference<v8::Value> barrier;
  barrier.Reset(rt->isolate, tail->handle.Get(rt->isolate));
  barrier.Reset();
}

}

namespace {

bool DefineById(JSContext* aCx, JS::HandleObject aObj, JS::HandleId aId,
                JS::Handle<JS::PropertyDescriptor> aDesc,
                JS::ObjectOpResult& aResult) {
  AutoV8 v8(aCx);
  v8::Isolate* isolate = v8.isolate();
  v8::Local<v8::Context> ctx = v8.context();
  v8::TryCatch tc(isolate);
  v8::Local<v8::Object> target = ToV8(aCx, aObj.get());
  v8::Local<v8::Name> key = NameOf(aCx, aId);
  const JS::PropertyDescriptor& desc = aDesc.get();
  v8::Maybe<bool> ok = v8::Nothing<bool>();
  auto accessorValue = [&](JSObject* aFun) -> v8::Local<v8::Value> {
    if (!aFun) {
      return v8::Undefined(isolate);
    }
    return ToV8(aCx, aFun);
  };
  auto apply = [&](v8::PropertyDescriptor& aV8Desc) {
    if (desc.hasEnumerable()) {
      aV8Desc.set_enumerable(desc.enumerable());
    }
    if (desc.hasConfigurable()) {
      aV8Desc.set_configurable(desc.configurable());
    }
    ok = target->DefineProperty(ctx, key, aV8Desc);
  };
  if (desc.hasWritable() && !desc.hasValue()) {
    v8::Local<v8::Object> descObj = v8::Object::New(isolate);
    auto setField = [&](const char* aName, v8::Local<v8::Value> aValue) {
      descObj
          ->CreateDataProperty(
              ctx, v8::String::NewFromUtf8(isolate, aName).ToLocalChecked(),
              aValue)
          .FromMaybe(false);
    };
    setField("writable", v8::Boolean::New(isolate, desc.writable()));
    if (desc.hasEnumerable()) {
      setField("enumerable", v8::Boolean::New(isolate, desc.enumerable()));
    }
    if (desc.hasConfigurable()) {
      setField("configurable", v8::Boolean::New(isolate, desc.configurable()));
    }
    v8::Local<v8::Value> define =
        Intrinsic(aCx, IntrinsicId::ReflectDefineProperty);
    v8::Local<v8::Value> args[] = {target, key, descObj};
    v8::Local<v8::Value> res;
    if (define->IsFunction() &&
        define.As<v8::Function>()
            ->Call(ctx, v8::Undefined(isolate), 3, args)
            .ToLocal(&res)) {
      ok = v8::Just(res->BooleanValue(isolate));
    }
  } else if (desc.hasValue() && desc.hasWritable()) {
    v8::PropertyDescriptor d(ToV8(aCx, desc.value()), desc.writable());
    apply(d);
  } else if (desc.hasValue()) {
    v8::PropertyDescriptor d(ToV8(aCx, desc.value()));
    apply(d);
  } else if (desc.hasGetter() || desc.hasSetter()) {
    v8::PropertyDescriptor d(desc.hasGetter() ? accessorValue(desc.getter())
                                              : v8::Local<v8::Value>(),
                             desc.hasSetter() ? accessorValue(desc.setter())
                                              : v8::Local<v8::Value>());
    apply(d);
  } else {
    v8::PropertyDescriptor d;
    apply(d);
  }
  if (ok.IsNothing()) {
    v8.Check(tc);
    return false;
  }
  if (!ok.FromJust()) {
    return aResult.fail(JSMSG_CANT_REDEFINE_PROP);
  }
  return aResult.succeed();
}

bool DefineStrict(JSContext* aCx, JS::HandleObject aObj, JS::HandleId aId,
                  JS::Handle<JS::PropertyDescriptor> aDesc) {
  JS::ObjectOpResult result;
  if (!DefineById(aCx, aObj, aId, aDesc, result)) {
    return false;
  }
  if (!result.ok()) {
    return ReportOpFailure(aCx, aId, result.failureCode());
  }
  return true;
}

bool DefineDataById(JSContext* aCx, JS::HandleObject aObj, JS::HandleId aId,
                    JS::HandleValue aValue, unsigned aAttrs) {
  JS::Rooted<JS::PropertyDescriptor> desc(
      aCx, JS::PropertyDescriptor::Data(
               aValue, aAttrs & (JSPROP_ENUMERATE | JSPROP_READONLY |
                                 JSPROP_PERMANENT | JSPROP_RESOLVING)));
  return DefineStrict(aCx, aObj, aId, desc);
}

bool DefineAccessorById(JSContext* aCx, JS::HandleObject aObj,
                        JS::HandleId aId, JS::HandleObject aGetter,
                        JS::HandleObject aSetter, unsigned aAttrs) {
  JS::Rooted<JS::PropertyDescriptor> desc(
      aCx, JS::PropertyDescriptor::Accessor(
               aGetter, aSetter,
               aAttrs & (JSPROP_ENUMERATE | JSPROP_PERMANENT |
                         JSPROP_RESOLVING)));
  return DefineStrict(aCx, aObj, aId, desc);
}

bool DefineNativeAccessorById(JSContext* aCx, JS::HandleObject aObj,
                              JS::HandleId aId, const JSNativeWrapper& aGet,
                              const JSNativeWrapper& aSet, unsigned aAttrs) {
  JS::Rooted<JSObject*> getter(aCx);
  JS::Rooted<JSObject*> setter(aCx);
  if (aGet.op) {
    JS::Rooted<JSString*> name(aCx, IdToFunctionName(aCx, aId, "get "));
    if (!name) {
      return false;
    }
    JSFunction* fun = NewNativeFunction(aCx, aGet.op, 0, 0, name, aGet.info);
    if (!fun) {
      return false;
    }
    getter = reinterpret_cast<JSObject*>(fun);
  }
  if (aSet.op) {
    JS::Rooted<JSString*> name(aCx, IdToFunctionName(aCx, aId, "set "));
    if (!name) {
      return false;
    }
    JSFunction* fun = NewNativeFunction(aCx, aSet.op, 1, 0, name, aSet.info);
    if (!fun) {
      return false;
    }
    setter = reinterpret_cast<JSObject*>(fun);
  }
  return DefineAccessorById(aCx, aObj, aId, getter, setter, aAttrs);
}

bool NameToId(JSContext* aCx, const char* aName, JS::MutableHandleId aId) {
  return IdFromLatin1(aCx, aName, strlen(aName), aId);
}

bool UCNameToId(JSContext* aCx, const char16_t* aName, size_t aLength,
                JS::MutableHandleId aId) {
  if (aLength == size_t(-1)) {
    aLength = std::char_traits<char16_t>::length(aName);
  }
  return IdFromChars(aCx, aName, aLength, aId);
}

bool GetById(JSContext* aCx, JS::HandleObject aObj, JS::HandleId aId,
             JS::HandleValue aReceiver, JS::MutableHandleValue aVp) {
  AutoV8 v8(aCx);
  v8::Isolate* isolate = v8.isolate();
  v8::Local<v8::Context> ctx = v8.context();
  v8::TryCatch tc(isolate);
  v8::Local<v8::Object> target = ToV8(aCx, aObj.get());
  v8::Local<v8::Value> out;
  bool sameReceiver = aReceiver.isObject() && &aReceiver.toObject() == aObj;
  if (sameReceiver) {
    if (aId.isInt()) {
      if (!target->Get(ctx, uint32_t(aId.toInt())).ToLocal(&out)) {
        return v8.Check(tc);
      }
    } else if (!target->Get(ctx, ToV8(aCx, aId.get())).ToLocal(&out)) {
      return v8.Check(tc);
    }
  } else {
    v8::Local<v8::Value> reflectGet = Intrinsic(aCx, IntrinsicId::ReflectGet);
    v8::Local<v8::Value> args[] = {target, ToV8(aCx, aId.get()),
                                   ToV8(aCx, aReceiver.get())};
    if (!reflectGet->IsFunction() ||
        !reflectGet.As<v8::Function>()
             ->Call(ctx, v8::Undefined(isolate), 3, args)
             .ToLocal(&out)) {
      return v8.Check(tc);
    }
  }
  aVp.set(ValueFromV8(aCx, out));
  return true;
}

bool SetById(JSContext* aCx, JS::HandleObject aObj, JS::HandleId aId,
             JS::HandleValue aValue) {
  AutoV8 v8(aCx);
  v8::Local<v8::Context> ctx = v8.context();
  v8::TryCatch tc(v8.isolate());
  v8::Local<v8::Object> target = ToV8(aCx, aObj.get());
  v8::Maybe<bool> ok =
      aId.isInt()
          ? target->Set(ctx, uint32_t(aId.toInt()), ToV8(aCx, aValue.get()))
          : target->Set(ctx, ToV8(aCx, aId.get()), ToV8(aCx, aValue.get()));
  if (ok.IsNothing()) {
    return v8.Check(tc);
  }
  return true;
}

bool HasById(JSContext* aCx, JS::HandleObject aObj, JS::HandleId aId,
             bool* aFound) {
  AutoV8 v8(aCx);
  v8::Local<v8::Context> ctx = v8.context();
  v8::TryCatch tc(v8.isolate());
  v8::Local<v8::Object> target = ToV8(aCx, aObj.get());
  v8::Maybe<bool> found = aId.isInt()
                              ? target->Has(ctx, uint32_t(aId.toInt()))
                              : target->Has(ctx, ToV8(aCx, aId.get()));
  if (found.IsNothing()) {
    return v8.Check(tc);
  }
  *aFound = found.FromJust();
  return true;
}

bool HasOwnById(JSContext* aCx, JS::HandleObject aObj, JS::HandleId aId,
                bool* aFound) {
  AutoV8 v8(aCx);
  v8::Local<v8::Context> ctx = v8.context();
  v8::TryCatch tc(v8.isolate());
  v8::Local<v8::Object> target = ToV8(aCx, aObj.get());
  v8::Maybe<bool> found =
      aId.isInt() ? target->HasOwnProperty(ctx, uint32_t(aId.toInt()))
                  : target->HasOwnProperty(ctx, NameOf(aCx, aId));
  if (found.IsNothing()) {
    return v8.Check(tc);
  }
  *aFound = found.FromJust();
  return true;
}

bool OwnDescriptorById(
    JSContext* aCx, JS::HandleObject aObj, JS::HandleId aId,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> aDesc) {
  AutoV8 v8(aCx);
  v8::Local<v8::Context> ctx = v8.context();
  v8::TryCatch tc(v8.isolate());
  v8::Local<v8::Object> target = ToV8(aCx, aObj.get());
  v8::Local<v8::Value> desc;
  if (!target->GetOwnPropertyDescriptor(ctx, NameOf(aCx, aId))
           .ToLocal(&desc)) {
    return v8.Check(tc);
  }
  return DescriptorFromV8(aCx, desc, aDesc);
}

bool DeleteById(JSContext* aCx, JS::HandleObject aObj, JS::HandleId aId,
                JS::ObjectOpResult& aResult) {
  AutoV8 v8(aCx);
  v8::Local<v8::Context> ctx = v8.context();
  v8::TryCatch tc(v8.isolate());
  v8::Local<v8::Object> target = ToV8(aCx, aObj.get());
  v8::Maybe<bool> ok = aId.isInt()
                           ? target->Delete(ctx, uint32_t(aId.toInt()))
                           : target->Delete(ctx, ToV8(aCx, aId.get()));
  if (ok.IsNothing()) {
    return v8.Check(tc);
  }
  return ok.FromJust() ? aResult.succeed()
                       : aResult.fail(JSMSG_CANT_DELETE);
}

const char* InformalTypeName(const JS::Value& aValue) {
  if (aValue.isObject()) {
    return JS::GetClass(&aValue.toObject())->name;
  }
  if (aValue.isNull()) {
    return "null";
  }
  if (aValue.isUndefined()) {
    return "undefined";
  }
  if (aValue.isString()) {
    return "string";
  }
  if (aValue.isSymbol()) {
    return "symbol";
  }
  if (aValue.isBigInt()) {
    return "bigint";
  }
  if (aValue.isBoolean()) {
    return "boolean";
  }
  return "number";
}

bool ToLengthOf(JSContext* aCx, JS::HandleObject aObj, uint64_t* aLength) {
  JS::Rooted<jsid> id(aCx);
  if (!NameToId(aCx, "length", &id)) {
    return false;
  }
  JS::Rooted<JS::Value> receiver(aCx, JS::ObjectValue(*aObj));
  JS::Rooted<JS::Value> v(aCx);
  if (!GetById(aCx, aObj, id, receiver, &v)) {
    return false;
  }
  double d;
  if (!JS::ToNumber(aCx, v, &d)) {
    return false;
  }
  d = JS::ToInteger(d);
  if (d <= 0) {
    *aLength = 0;
  } else if (d >= 9007199254740991.0) {
    *aLength = 9007199254740991ULL;
  } else {
    *aLength = uint64_t(d);
  }
  return true;
}

const JS::Value kNullValue = JS::NullValue();
const JS::Value kUndefinedValue = JS::UndefinedValue();
const JS::Value kTrueValue = JS::BooleanValue(true);
const JS::Value kFalseValue = JS::BooleanValue(false);
const mozilla::Maybe<JS::Value> kNothingValue;
const jsid kVoidId = JS::PropertyKey::Void();

}

const JS::HandleValue JS::NullHandleValue =
    JS::HandleValue::fromMarkedLocation(&kNullValue);
const JS::HandleValue JS::UndefinedHandleValue =
    JS::HandleValue::fromMarkedLocation(&kUndefinedValue);
const JS::HandleValue JS::TrueHandleValue =
    JS::HandleValue::fromMarkedLocation(&kTrueValue);
const JS::HandleValue JS::FalseHandleValue =
    JS::HandleValue::fromMarkedLocation(&kFalseValue);
const JS::Handle<mozilla::Maybe<JS::Value>> JS::NothingHandleValue =
    JS::Handle<mozilla::Maybe<JS::Value>>::fromMarkedLocation(&kNothingValue);
const JS::HandleId JS::VoidHandlePropertyKey =
    JS::HandleId::fromMarkedLocation(&kVoidId);

JS_PUBLIC_API bool JS_DefinePropertyById(
    JSContext* cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id,
    JS::Handle<JS::PropertyDescriptor> desc, JS::ObjectOpResult& result) {
  return DefineById(cx, obj, id, desc, result);
}

JS_PUBLIC_API bool JS_DefinePropertyById(
    JSContext* cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id,
    JS::Handle<JS::PropertyDescriptor> desc) {
  return DefineStrict(cx, obj, id, desc);
}

JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx,
                                         JS::Handle<JSObject*> obj,
                                         JS::Handle<jsid> id,
                                         JS::Handle<JS::Value> value,
                                         unsigned attrs) {
  return DefineDataById(cx, obj, id, value, attrs);
}

JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx,
                                         JS::Handle<JSObject*> obj,
                                         JS::Handle<jsid> id, JSNative getter,
                                         JSNative setter, unsigned attrs) {
  return DefineNativeAccessorById(cx, obj, id, JSNativeWrapper(getter, nullptr),
                                  JSNativeWrapper(setter, nullptr), attrs);
}

JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx,
                                         JS::Handle<JSObject*> obj,
                                         JS::Handle<jsid> id,
                                         JS::Handle<JSObject*> getter,
                                         JS::Handle<JSObject*> setter,
                                         unsigned attrs) {
  return DefineAccessorById(cx, obj, id, getter, setter,
                            attrs & ~JSPROP_READONLY);
}

JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx,
                                         JS::Handle<JSObject*> obj,
                                         JS::Handle<jsid> id,
                                         JS::Handle<JSObject*> valueArg,
                                         unsigned attrs) {
  JS::Rooted<JS::Value> value(cx, JS::ObjectValue(*valueArg));
  return DefineDataById(cx, obj, id, value, attrs);
}

JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx,
                                         JS::Handle<JSObject*> obj,
                                         JS::Handle<jsid> id,
                                         JS::Handle<JSString*> valueArg,
                                         unsigned attrs) {
  JS::Rooted<JS::Value> value(cx, JS::StringValue(valueArg));
  return DefineDataById(cx, obj, id, value, attrs);
}

JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx,
                                         JS::Handle<JSObject*> obj,
                                         JS::Handle<jsid> id, uint32_t valueArg,
                                         unsigned attrs) {
  JS::Rooted<JS::Value> value(cx, JS::NumberValue(valueArg));
  return DefineDataById(cx, obj, id, value, attrs);
}

JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                     const char* name,
                                     JS::Handle<JS::Value> value,
                                     unsigned attrs) {
  JS::Rooted<jsid> id(cx);
  return NameToId(cx, name, &id) && DefineDataById(cx, obj, id, value, attrs);
}

JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                     const char* name, JSNative getter,
                                     JSNative setter, unsigned attrs) {
  JS::Rooted<jsid> id(cx);
  return NameToId(cx, name, &id) &&
         DefineNativeAccessorById(cx, obj, id, JSNativeWrapper(getter, nullptr),
                                  JSNativeWrapper(setter, nullptr), attrs);
}

JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                     const char* name,
                                     JS::Handle<JSObject*> getter,
                                     JS::Handle<JSObject*> setter,
                                     unsigned attrs) {
  JS::Rooted<jsid> id(cx);
  return NameToId(cx, name, &id) &&
         DefineAccessorById(cx, obj, id, getter, setter,
                            attrs & ~JSPROP_READONLY);
}

JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                     const char* name,
                                     JS::Handle<JSObject*> valueArg,
                                     unsigned attrs) {
  JS::Rooted<JS::Value> value(cx, JS::ObjectValue(*valueArg));
  return JS_DefineProperty(cx, obj, name, value, attrs);
}

JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                     const char* name,
                                     JS::Handle<JSString*> valueArg,
                                     unsigned attrs) {
  JS::Rooted<JS::Value> value(cx, JS::StringValue(valueArg));
  return JS_DefineProperty(cx, obj, name, value, attrs);
}

JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                     const char* name, int32_t valueArg,
                                     unsigned attrs) {
  JS::Rooted<JS::Value> value(cx, JS::Int32Value(valueArg));
  return JS_DefineProperty(cx, obj, name, value, attrs);
}

JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                     const char* name, uint32_t valueArg,
                                     unsigned attrs) {
  JS::Rooted<JS::Value> value(cx, JS::NumberValue(valueArg));
  return JS_DefineProperty(cx, obj, name, value, attrs);
}

JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                     const char* name, double valueArg,
                                     unsigned attrs) {
  JS::Rooted<JS::Value> value(cx, JS::NumberValue(valueArg));
  return JS_DefineProperty(cx, obj, name, value, attrs);
}

JS_PUBLIC_API bool JS_DefineUCProperty(JSContext* cx,
                                       JS::Handle<JSObject*> obj,
                                       const char16_t* name, size_t namelen,
                                       JS::Handle<JS::Value> value,
                                       unsigned attrs) {
  JS::Rooted<jsid> id(cx);
  return UCNameToId(cx, name, namelen, &id) &&
         DefineDataById(cx, obj, id, value, attrs);
}

JS_PUBLIC_API bool JS_DefineUCProperty(JSContext* cx,
                                       JS::Handle<JSObject*> obj,
                                       const char16_t* name, size_t namelen,
                                       JS::Handle<JSObject*> valueArg,
                                       unsigned attrs) {
  JS::Rooted<JS::Value> value(cx, JS::ObjectValue(*valueArg));
  return JS_DefineUCProperty(cx, obj, name, namelen, value, attrs);
}

JS_PUBLIC_API bool JS_DefineElement(JSContext* cx, JS::Handle<JSObject*> obj,
                                    uint32_t index,
                                    JS::Handle<JS::Value> value,
                                    unsigned attrs) {
  JS::Rooted<jsid> id(cx);
  if (index <= uint32_t(JS::PropertyKey::IntMax)) {
    id = JS::PropertyKey::Int(int32_t(index));
  } else {
    std::string digits = std::to_string(index);
    if (!IdFromLatin1(cx, digits.data(), digits.size(), &id)) {
      return false;
    }
  }
  return DefineDataById(cx, obj, id, value, attrs);
}

JS_PUBLIC_API bool JS_DefineElement(JSContext* cx, JS::Handle<JSObject*> obj,
                                    uint32_t index,
                                    JS::Handle<JSObject*> valueArg,
                                    unsigned attrs) {
  JS::Rooted<JS::Value> value(cx, JS::ObjectValue(*valueArg));
  return JS_DefineElement(cx, obj, index, value, attrs);
}

JS_PUBLIC_API bool JS_DefineElement(JSContext* cx, JS::Handle<JSObject*> obj,
                                    uint32_t index,
                                    JS::Handle<JSString*> valueArg,
                                    unsigned attrs) {
  JS::Rooted<JS::Value> value(cx, JS::StringValue(valueArg));
  return JS_DefineElement(cx, obj, index, value, attrs);
}

JS_PUBLIC_API bool JS_DefineElement(JSContext* cx, JS::Handle<JSObject*> obj,
                                    uint32_t index, int32_t valueArg,
                                    unsigned attrs) {
  JS::Rooted<JS::Value> value(cx, JS::Int32Value(valueArg));
  return JS_DefineElement(cx, obj, index, value, attrs);
}

JS_PUBLIC_API bool JS_DefineElement(JSContext* cx, JS::Handle<JSObject*> obj,
                                    uint32_t index, uint32_t valueArg,
                                    unsigned attrs) {
  JS::Rooted<JS::Value> value(cx, JS::NumberValue(valueArg));
  return JS_DefineElement(cx, obj, index, value, attrs);
}

JS_PUBLIC_API bool JS_DefineElement(JSContext* cx, JS::Handle<JSObject*> obj,
                                    uint32_t index, double valueArg,
                                    unsigned attrs) {
  JS::Rooted<JS::Value> value(cx, JS::NumberValue(valueArg));
  return JS_DefineElement(cx, obj, index, value, attrs);
}

JS_PUBLIC_API bool JS_DefineProperties(JSContext* cx,
                                       JS::Handle<JSObject*> obj,
                                       const JSPropertySpec* ps) {
  JS::Rooted<jsid> id(cx);
  for (; ps->name; ps++) {
    if (!JS::PropertySpecNameToPermanentId(cx, ps->name, id.address())) {
      return false;
    }
    if (ps->isAccessor()) {
      if (ps->isSelfHosted()) {
        JSFunctionSpec getterSpec = {
            ps->name, {nullptr, nullptr}, 0, 0,
            ps->u.accessors.getter.selfHosted.funname};
        JS::Rooted<JSObject*> getter(
            cx, reinterpret_cast<JSObject*>(
                    JS::NewFunctionFromSpec(cx, &getterSpec, id)));
        if (!getter) {
          return false;
        }
        JS::Rooted<JSObject*> setter(cx);
        if (const char* setterName =
                ps->u.accessors.setter.selfHosted.funname) {
          JSFunctionSpec setterSpec = {
              ps->name, {nullptr, nullptr}, 1, 0, setterName};
          setter = reinterpret_cast<JSObject*>(
              JS::NewFunctionFromSpec(cx, &setterSpec, id));
          if (!setter) {
            return false;
          }
        }
        if (!DefineAccessorById(cx, obj, id, getter, setter,
                                ps->attributes())) {
          return false;
        }
      } else if (!DefineNativeAccessorById(cx, obj, id,
                                           ps->u.accessors.getter.native,
                                           ps->u.accessors.setter.native,
                                           ps->attributes())) {
        return false;
      }
    } else {
      JS::Rooted<JS::Value> v(cx);
      if (!ps->getValue(cx, &v)) {
        return false;
      }
      if (!DefineDataById(cx, obj, id, v, ps->attributes())) {
        return false;
      }
    }
  }
  return true;
}

JS_PUBLIC_API bool JS_DefineFunctions(JSContext* cx, JS::Handle<JSObject*> obj,
                                      const JSFunctionSpec* fs) {
  JS::Rooted<jsid> id(cx);
  for (; fs->name; fs++) {
    if (!JS::PropertySpecNameToPermanentId(cx, fs->name, id.address())) {
      return false;
    }
    JSFunction* fun = JS::NewFunctionFromSpec(cx, fs, id);
    if (!fun) {
      return false;
    }
    JS::Rooted<JS::Value> funVal(
        cx, JS::ObjectValue(*reinterpret_cast<JSObject*>(fun)));
    if (!DefineDataById(cx, obj, id, funVal, fs->flags & ~JSFUN_FLAGS_MASK)) {
      return false;
    }
  }
  return true;
}

JS_PUBLIC_API JSFunction* JS_DefineFunctionById(JSContext* cx,
                                                JS::Handle<JSObject*> obj,
                                                JS::Handle<jsid> id,
                                                JSNative call, unsigned nargs,
                                                unsigned attrs) {
  JS::Rooted<JSString*> name(cx, IdToFunctionName(cx, id, ""));
  if (!name) {
    return nullptr;
  }
  JSFunction* fun = NewNativeFunction(cx, call, nargs,
                                      attrs & JSFUN_FLAGS_MASK, name, nullptr);
  if (!fun) {
    return nullptr;
  }
  JS::Rooted<JSObject*> funObj(cx, reinterpret_cast<JSObject*>(fun));
  JS::Rooted<JS::Value> funVal(cx, JS::ObjectValue(*funObj));
  if (!DefineDataById(cx, obj, id, funVal, attrs & ~JSFUN_FLAGS_MASK)) {
    return nullptr;
  }
  return reinterpret_cast<JSFunction*>(funObj.get());
}

JS_PUBLIC_API JSFunction* JS_DefineFunction(JSContext* cx,
                                            JS::Handle<JSObject*> obj,
                                            const char* name, JSNative call,
                                            unsigned nargs, unsigned attrs) {
  JS::Rooted<jsid> id(cx);
  if (!NameToId(cx, name, &id)) {
    return nullptr;
  }
  return JS_DefineFunctionById(cx, obj, id, call, nargs, attrs);
}

JS_PUBLIC_API JSObject* JS_DefineObject(JSContext* cx,
                                        JS::Handle<JSObject*> obj,
                                        const char* name, const JSClass* clasp,
                                        unsigned attrs) {
  JS::Rooted<JSObject*> nobj(
      cx, clasp ? JS_NewObject(cx, clasp) : JS_NewPlainObject(cx));
  if (!nobj) {
    return nullptr;
  }
  JS::Rooted<JS::Value> nobjValue(cx, JS::ObjectValue(*nobj));
  if (!JS_DefineProperty(cx, obj, name, nobjValue, attrs)) {
    return nullptr;
  }
  return nobj;
}

JS_PUBLIC_API bool JS_AlreadyHasOwnPropertyById(JSContext* cx,
                                                JS::Handle<JSObject*> obj,
                                                JS::Handle<jsid> id,
                                                bool* foundp) {
  if (js::IsProxy(obj)) {
    return HasOwnById(cx, obj, id, foundp);
  }
  AutoV8 v8(cx);
  v8::Local<v8::Context> ctx = v8.context();
  v8::TryCatch tc(v8.isolate());
  v8::Local<v8::Object> target = ToV8(cx, obj.get());
  v8::Maybe<bool> found =
      id.isInt() ? target->HasRealIndexedProperty(ctx, uint32_t(id.toInt()))
                 : target->HasRealNamedProperty(ctx, NameOf(cx, id));
  if (found.IsNothing()) {
    return v8.Check(tc);
  }
  *foundp = found.FromJust();
  return true;
}

JS_PUBLIC_API bool JS_DeletePropertyById(JSContext* cx,
                                         JS::Handle<JSObject*> obj,
                                         JS::Handle<jsid> id,
                                         JS::ObjectOpResult& result) {
  return DeleteById(cx, obj, id, result);
}

JS_PUBLIC_API bool JS_DeleteUCProperty(JSContext* cx,
                                       JS::Handle<JSObject*> obj,
                                       const char16_t* name, size_t namelen,
                                       JS::ObjectOpResult& result) {
  JS::Rooted<jsid> id(cx);
  return UCNameToId(cx, name, namelen, &id) && DeleteById(cx, obj, id, result);
}

JS_PUBLIC_API bool JS_ForwardGetPropertyTo(JSContext* cx,
                                           JS::Handle<JSObject*> obj,
                                           JS::Handle<jsid> id,
                                           JS::Handle<JS::Value> receiver,
                                           JS::MutableHandle<JS::Value> vp) {
  return GetById(cx, obj, id, receiver, vp);
}

JS_PUBLIC_API bool JS_GetPropertyById(JSContext* cx, JS::Handle<JSObject*> obj,
                                      JS::Handle<jsid> id,
                                      JS::MutableHandle<JS::Value> vp) {
  JS::Rooted<JS::Value> receiver(cx, JS::ObjectValue(*obj));
  return GetById(cx, obj, id, receiver, vp);
}

JS_PUBLIC_API bool JS_GetProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                  const char* name,
                                  JS::MutableHandle<JS::Value> vp) {
  JS::Rooted<jsid> id(cx);
  return NameToId(cx, name, &id) && JS_GetPropertyById(cx, obj, id, vp);
}

JS_PUBLIC_API bool JS_GetUCProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                    const char16_t* name, size_t namelen,
                                    JS::MutableHandle<JS::Value> vp) {
  JS::Rooted<jsid> id(cx);
  return UCNameToId(cx, name, namelen, &id) &&
         JS_GetPropertyById(cx, obj, id, vp);
}

JS_PUBLIC_API bool JS_GetElement(JSContext* cx, JS::Handle<JSObject*> obj,
                                 uint32_t index,
                                 JS::MutableHandle<JS::Value> vp) {
  AutoV8 v8(cx);
  v8::TryCatch tc(v8.isolate());
  v8::Local<v8::Value> out;
  if (!ToV8(cx, obj.get())->Get(v8.context(), index).ToLocal(&out)) {
    return v8.Check(tc);
  }
  vp.set(ValueFromV8(cx, out));
  return true;
}

JS_PUBLIC_API bool JS_SetPropertyById(JSContext* cx, JS::Handle<JSObject*> obj,
                                      JS::Handle<jsid> id,
                                      JS::Handle<JS::Value> v) {
  return SetById(cx, obj, id, v);
}

JS_PUBLIC_API bool JS_SetProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                  const char* name, JS::Handle<JS::Value> v) {
  JS::Rooted<jsid> id(cx);
  return NameToId(cx, name, &id) && SetById(cx, obj, id, v);
}

JS_PUBLIC_API bool JS_SetElement(JSContext* cx, JS::Handle<JSObject*> obj,
                                 uint32_t index, JS::Handle<JS::Value> v) {
  AutoV8 v8(cx);
  v8::TryCatch tc(v8.isolate());
  if (ToV8(cx, obj.get())
          ->Set(v8.context(), index, ToV8(cx, v.get()))
          .IsNothing()) {
    return v8.Check(tc);
  }
  return true;
}

JS_PUBLIC_API bool JS_SetElement(JSContext* cx, JS::Handle<JSObject*> obj,
                                 uint32_t index, JS::Handle<JSObject*> v) {
  JS::Rooted<JS::Value> value(cx, JS::ObjectOrNullValue(v));
  return JS_SetElement(cx, obj, index, value);
}

JS_PUBLIC_API bool JS_HasPropertyById(JSContext* cx, JS::Handle<JSObject*> obj,
                                      JS::Handle<jsid> id, bool* foundp) {
  return HasById(cx, obj, id, foundp);
}

JS_PUBLIC_API bool JS_HasProperty(JSContext* cx, JS::Handle<JSObject*> obj,
                                  const char* name, bool* foundp) {
  JS::Rooted<jsid> id(cx);
  return NameToId(cx, name, &id) && HasById(cx, obj, id, foundp);
}

JS_PUBLIC_API bool JS_HasOwnPropertyById(JSContext* cx,
                                         JS::Handle<JSObject*> obj,
                                         JS::Handle<jsid> id, bool* foundp) {
  return HasOwnById(cx, obj, id, foundp);
}

JS_PUBLIC_API bool JS_GetOwnPropertyDescriptorById(
    JSContext* cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc) {
  return OwnDescriptorById(cx, obj, id, desc);
}

JS_PUBLIC_API bool JS_GetOwnPropertyDescriptor(
    JSContext* cx, JS::Handle<JSObject*> obj, const char* name,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc) {
  JS::Rooted<jsid> id(cx);
  return NameToId(cx, name, &id) && OwnDescriptorById(cx, obj, id, desc);
}

JS_PUBLIC_API bool JS_GetOwnUCPropertyDescriptor(
    JSContext* cx, JS::Handle<JSObject*> obj, const char16_t* name,
    size_t namelen,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc) {
  JS::Rooted<jsid> id(cx);
  return UCNameToId(cx, name, namelen, &id) &&
         OwnDescriptorById(cx, obj, id, desc);
}

JS_PUBLIC_API bool JS_GetPropertyDescriptorById(
    JSContext* cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc,
    JS::MutableHandle<JSObject*> holder) {
  JS::Rooted<JSObject*> current(cx, obj);
  while (current) {
    if (!OwnDescriptorById(cx, current, id, desc)) {
      return false;
    }
    if (desc.isSome()) {
      holder.set(current);
      return true;
    }
    JS::Rooted<JSObject*> proto(cx);
    if (!JS_GetPrototype(cx, current, &proto)) {
      return false;
    }
    current = proto;
  }
  desc.set(mozilla::Nothing());
  holder.set(nullptr);
  return true;
}

JS_PUBLIC_API bool JS::IsCallable(JSObject* obj) {
  JSContext* cx = CurrentContext();
  v8::HandleScope scope(cx->isolate);
  return ToV8(cx, obj)->IsCallable();
}

JS_PUBLIC_API bool JS::IsConstructor(JSObject* obj) {
  JSContext* cx = CurrentContext();
  v8::HandleScope scope(cx->isolate);
  return ToV8(cx, obj)->IsConstructor();
}

JS_PUBLIC_API bool JS_CallFunctionValue(JSContext* cx,
                                        JS::Handle<JSObject*> obj,
                                        JS::Handle<JS::Value> fval,
                                        const JS::HandleValueArray& args,
                                        JS::MutableHandle<JS::Value> rval) {
  JS::Rooted<JS::Value> thisv(cx, JS::ObjectOrNullValue(obj));
  return JS::Call(cx, thisv, fval, args, rval);
}

JS_PUBLIC_API bool JS_CallFunctionName(JSContext* cx,
                                       JS::Handle<JSObject*> obj,
                                       const char* name,
                                       const JS::HandleValueArray& args,
                                       JS::MutableHandle<JS::Value> rval) {
  JS::Rooted<JS::Value> fval(cx);
  if (!JS_GetProperty(cx, obj, name, &fval)) {
    return false;
  }
  JS::Rooted<JS::Value> thisv(cx, JS::ObjectOrNullValue(obj));
  return JS::Call(cx, thisv, fval, args, rval);
}

namespace {

bool ConstructImpl(JSContext* cx, JS::HandleValue fun,
                   JS::HandleObject newTarget,
                   const JS::HandleValueArray& args,
                   JS::MutableHandleObject objp) {
  if (!fun.isObject() || !JS::IsConstructor(&fun.toObject())) {
    JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                              JSMSG_NOT_CONSTRUCTOR, InformalTypeName(fun));
    return false;
  }
  if (newTarget && !JS::IsConstructor(newTarget)) {
    JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                              JSMSG_NOT_CONSTRUCTOR,
                              JS::GetClass(newTarget)->name);
    return false;
  }
  AutoV8 v8(cx);
  v8::Isolate* isolate = v8.isolate();
  v8::Local<v8::Context> ctx = v8.context();
  v8::TryCatch tc(isolate);
  std::vector<v8::Local<v8::Value>> argv(args.length());
  for (size_t i = 0; i < args.length(); i++) {
    argv[i] = ToV8(cx, args[i].get());
  }
  v8::Local<v8::Object> target = ToV8(cx, &fun.toObject());
  v8::Local<v8::Value> out;
  if (!newTarget || newTarget.get() == &fun.toObject()) {
    if (!target->CallAsConstructor(ctx, int(argv.size()), argv.data())
             .ToLocal(&out)) {
      return v8.Check(tc);
    }
  } else {
    v8::Local<v8::Value> construct =
        Intrinsic(cx, IntrinsicId::ReflectConstruct);
    v8::Local<v8::Array> list =
        v8::Array::New(isolate, argv.data(), argv.size());
    v8::Local<v8::Value> callArgs[] = {target, list,
                                       ToV8(cx, newTarget.get())};
    if (!construct->IsFunction() ||
        !construct.As<v8::Function>()
             ->Call(ctx, v8::Undefined(isolate), 3, callArgs)
             .ToLocal(&out)) {
      return v8.Check(tc);
    }
  }
  objp.set(ObjectFromV8(cx, out.As<v8::Object>()));
  return true;
}

}

JS_PUBLIC_API bool JS::Construct(JSContext* cx, JS::Handle<JS::Value> fun,
                                 JS::Handle<JSObject*> newTarget,
                                 const JS::HandleValueArray& args,
                                 JS::MutableHandle<JSObject*> objp) {
  return ConstructImpl(cx, fun, newTarget, args, objp);
}

JS_PUBLIC_API bool JS::Construct(JSContext* cx, JS::Handle<JS::Value> fun,
                                 const JS::HandleValueArray& args,
                                 JS::MutableHandle<JSObject*> objp) {
  return ConstructImpl(cx, fun, nullptr, args, objp);
}

JS_PUBLIC_API bool JS::detail::ComputeThis(
    JSContext* cx, JS::Value* vp, JS::MutableHandle<JSObject*> thisObject) {
  JS::HandleValue thisv = JS::HandleValue::fromMarkedLocation(&vp[1]);
  if (thisv.isObject()) {
    thisObject.set(&thisv.toObject());
    return true;
  }
  AutoV8 v8(cx);
  v8::TryCatch tc(v8.isolate());
  if (thisv.isNullOrUndefined()) {
    thisObject.set(ObjectFromV8(cx, v8.context()->Global()));
    return true;
  }
  v8::Local<v8::Object> boxed;
  if (!ToV8(cx, thisv.get())->ToObject(v8.context()).ToLocal(&boxed)) {
    return v8.Check(tc);
  }
  thisObject.set(ObjectFromV8(cx, boxed));
  return true;
}

JS_PUBLIC_API void JS::CallArgs::reportMoreArgsNeeded(JSContext* cx,
                                                      const char* fnname,
                                                      unsigned required,
                                                      unsigned actual) {
  char requiredArgsStr[40];
  snprintf(requiredArgsStr, sizeof(requiredArgsStr), "%u", required);
  char actualArgsStr[40];
  snprintf(actualArgsStr, sizeof(actualArgsStr), "%u", actual);
  JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                            JSMSG_MORE_ARGS_NEEDED, fnname, requiredArgsStr,
                            required == 1 ? "" : "s", actualArgsStr);
}

JS_PUBLIC_API bool JS::detail::CallMethodIfWrapped(JSContext* cx,
                                                   JS::IsAcceptableThis test,
                                                   JS::NativeImpl impl,
                                                   const JS::CallArgs& args) {
  JS::HandleValue thisv = args.thisv();
  if (thisv.isObject()) {
    JS::Rooted<JSObject*> thisObj(cx, &thisv.toObject());
    if (js::IsProxy(thisObj)) {
      return js::GetProxyHandler(thisObj)->nativeCall(cx, test, impl, args);
    }
  }
  std::string funName = "Function";
  if (args.calleev().isObject()) {
    v8::HandleScope scope(cx->isolate);
    v8::Local<v8::Object> callee = ToV8(cx, &args.calleev().toObject());
    if (callee->IsFunction()) {
      v8::Local<v8::Value> name = callee.As<v8::Function>()->GetName();
      if (name->IsString()) {
        v8::String::Value chars(cx->isolate, name);
        funName = Utf16ToUtf8(reinterpret_cast<const char16_t*>(*chars),
                              size_t(chars.length()));
      }
    }
  }
  std::string typeName = InformalTypeName(thisv.get());
  const char* errArgs[] = {funName.c_str(), "method", typeName.c_str(),
                           nullptr};
  JS_ReportErrorNumberUTF8Array(cx, js::GetErrorMessage, nullptr,
                                JSMSG_INCOMPATIBLE_METHOD, errArgs);
  return false;
}

JS_PUBLIC_API JSObject* JS::NewArrayObject(
    JSContext* cx, const JS::HandleValueArray& contents) {
  AutoV8 v8(cx);
  std::vector<v8::Local<v8::Value>> elements(contents.length());
  for (size_t i = 0; i < contents.length(); i++) {
    elements[i] = ToV8(cx, contents[i].get());
  }
  v8::Local<v8::Array> array =
      v8::Array::New(v8.isolate(), elements.data(), elements.size());
  return ObjectFromV8(cx, array);
}

JS_PUBLIC_API JSObject* JS::NewArrayObject(JSContext* cx, size_t length) {
  if (length > UINT32_MAX) {
    JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                              JSMSG_BAD_ARRAY_LENGTH);
    return nullptr;
  }
  AutoV8 v8(cx);
  v8::TryCatch tc(v8.isolate());
  v8::Local<v8::Array> array;
  if (length <= size_t(INT32_MAX)) {
    array = v8::Array::New(v8.isolate(), int(length));
  } else {
    array = v8::Array::New(v8.isolate());
    if (array
            ->Set(v8.context(),
                  v8::String::NewFromUtf8Literal(v8.isolate(), "length"),
                  v8::Number::New(v8.isolate(), double(length)))
            .IsNothing()) {
      v8.Check(tc);
      return nullptr;
    }
  }
  return ObjectFromV8(cx, array);
}

JS_PUBLIC_API bool JS::IsArrayObject(JSContext* cx, JS::Handle<JSObject*> obj,
                                     bool* isArray) {
  js::ESClass cls;
  if (!BuiltinClassOf(cx, obj, &cls)) {
    return false;
  }
  *isArray = cls == js::ESClass::Array;
  return true;
}

JS_PUBLIC_API bool JS::IsArrayObject(JSContext* cx,
                                     JS::Handle<JS::Value> value,
                                     bool* isArray) {
  if (!value.isObject()) {
    *isArray = false;
    return true;
  }
  JS::Rooted<JSObject*> obj(cx, &value.toObject());
  return JS::IsArrayObject(cx, obj, isArray);
}

JS_PUBLIC_API bool JS::GetArrayLength(JSContext* cx, JS::Handle<JSObject*> obj,
                                      uint32_t* lengthp) {
  uint64_t len = 0;
  if (!ToLengthOf(cx, obj, &len)) {
    return false;
  }
  if (len > UINT32_MAX) {
    JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                              JSMSG_BAD_ARRAY_LENGTH);
    return false;
  }
  *lengthp = uint32_t(len);
  return true;
}

JS_PUBLIC_API bool JS::SetArrayLength(JSContext* cx, JS::Handle<JSObject*> obj,
                                      uint32_t length) {
  bool succeeded;
  {
    AutoV8 v8(cx);
    v8::Isolate* isolate = v8.isolate();
    v8::Local<v8::Context> ctx = v8.context();
    v8::TryCatch tc(isolate);
    v8::Local<v8::Object> target = ToV8(cx, obj.get());
    v8::Local<v8::Value> reflectSet = Intrinsic(cx, IntrinsicId::ReflectSet);
    v8::Local<v8::Value> args[] = {
        target, v8::String::NewFromUtf8Literal(isolate, "length"),
        v8::Number::New(isolate, double(length)), target};
    v8::Local<v8::Value> res;
    if (!reflectSet->IsFunction() ||
        !reflectSet.As<v8::Function>()
             ->Call(ctx, v8::Undefined(isolate), 4, args)
             .ToLocal(&res)) {
      return v8.Check(tc);
    }
    succeeded = res->BooleanValue(isolate);
  }
  if (!succeeded) {
    JS::Rooted<jsid> id(cx);
    if (!NameToId(cx, "length", &id)) {
      return false;
    }
    return ReportOpFailure(cx, id, JSMSG_READ_ONLY);
  }
  return true;
}

JS_PUBLIC_API bool JS::GetBuiltinClass(JSContext* cx,
                                       JS::Handle<JSObject*> obj,
                                       js::ESClass* cls) {
  return BuiltinClassOf(cx, obj, cls);
}

JS_PUBLIC_API bool JS::IsPlainObject(JSObject* obj) {
  if (js::IsProxy(obj) || JS::GetClass(obj) != js::ObjectClassPtr) {
    return false;
  }
  JSContext* cx = CurrentContext();
  v8::HandleScope scope(cx->isolate);
  return IsPlainV8Object(ToV8(cx, obj));
}

JS_PUBLIC_API bool JS::NativeObjectHasOwnProperties(const JSObject* obj) {
  JSContext* cx = CurrentContext();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::Object> o = ToV8(cx, const_cast<JSObject*>(obj));
  v8::Local<v8::Context> ctx;
  if (!o->GetCreationContext().ToLocal(&ctx)) {
    ctx = cx->v8Context();
  }
  v8::Context::Scope contextScope(ctx);
  v8::TryCatch tc(cx->isolate);
  v8::Local<v8::Array> names;
  if (!o->GetPropertyNames(ctx, v8::KeyCollectionMode::kOwnOnly,
                           v8::PropertyFilter::ALL_PROPERTIES,
                           v8::IndexFilter::kSkipIndices,
                           v8::KeyConversionMode::kKeepNumbers)
           .ToLocal(&names)) {
    return false;
  }
  return names->Length() > 0;
}

JS_PUBLIC_API void JS::detail::SetNativeObjectReservedSlotWithBarrier(
    JSObject* obj, size_t slot, const JS::Value& value) {
  reinterpret_cast<JS::shadow::NativeObject*>(obj)->reservedSlotRef(slot) =
      value;
  MarkForBarrier(value);
}

JS_PUBLIC_API void JS::HeapValueWriteBarriers(JS::Value* valuep,
                                              const JS::Value& prev,
                                              const JS::Value& next) {
  MarkForBarrier(next);
}

JS_PUBLIC_API void js::TraceValueArray(JSTracer* trc, size_t length,
                                       JS::Value* elements) {
  for (size_t i = 0; i < length; i++) {
    JS::TraceRoot(trc, &elements[i], "JS::RootedValueArray");
  }
}
