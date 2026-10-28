#include "VgeJsProxyInternal.h"

#include <vector>

#include "js/CallAndConstruct.h"
#include "js/ErrorReport.h"
#include "js/Object.h"
#include "js/PropertyAndElement.h"
#include "js/PropertyDescriptor.h"

namespace vge::jsapi::proxy {

namespace {

const char* const kReflectNames[kReflectCount] = {
    "get",
    "set",
    "has",
    "defineProperty",
    "deleteProperty",
    "getOwnPropertyDescriptor",
    "ownKeys",
    "getPrototypeOf",
    "setPrototypeOf",
    "isExtensible",
    "preventExtensions",
    "apply",
    "construct",
};

void EnsureIntrinsics(JSContext* aCx) {
  RuntimeState& st = State();
  if (!st.intrinsics.IsEmpty()) {
    return;
  }
  v8::Isolate* isolate = aCx->isolate;
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> ctx = v8::Context::New(isolate);
  v8::Context::Scope contextScope(ctx);
  v8::Local<v8::Value> reflect =
      ctx->Global()
          ->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "Reflect"))
          .ToLocalChecked();
  MOZ_RELEASE_ASSERT(reflect->IsObject());
  for (int i = 0; i < kReflectCount; i++) {
    v8::Local<v8::Value> fn =
        reflect.As<v8::Object>()
            ->Get(ctx, v8::String::NewFromUtf8(isolate, kReflectNames[i])
                           .ToLocalChecked())
            .ToLocalChecked();
    MOZ_RELEASE_ASSERT(fn->IsFunction());
    st.reflect[i].Reset(isolate, fn.As<v8::Function>());
  }
  st.intrinsics.Reset(isolate, ctx);
}

v8::Local<v8::Context> CallingContext(JSContext* aCx) {
  v8::Local<v8::Context> ctx = aCx->v8Context();
  if (ctx.IsEmpty()) {
    ctx = State().intrinsics.Get(aCx->isolate);
  }
  return ctx;
}

const char* InformalTypeName(const JS::Value& aValue) {
  if (aValue.isObject()) {
    return JS::GetClass(&aValue.toObject())->name;
  }
  if (aValue.isString()) {
    return "string";
  }
  if (aValue.isNumber()) {
    return "number";
  }
  if (aValue.isBoolean()) {
    return "boolean";
  }
  if (aValue.isNull()) {
    return "null";
  }
  if (aValue.isSymbol()) {
    return "symbol";
  }
  if (aValue.isBigInt()) {
    return "bigint";
  }
  return "undefined";
}

std::string Utf8(v8::Isolate* aIsolate, v8::Local<v8::Value> aValue) {
  v8::String::Utf8Value utf8(aIsolate, aValue);
  if (!*utf8) {
    return std::string();
  }
  return std::string(*utf8, utf8.length());
}

bool ArrayToIds(JSContext* aCx, v8::Local<v8::Value> aArray,
                JS::MutableHandleIdVector aProps) {
  if (!aArray->IsArray()) {
    return true;
  }
  v8::Local<v8::Array> array = aArray.As<v8::Array>();
  v8::Local<v8::Context> ctx = CallingContext(aCx);
  uint32_t length = array->Length();
  JS::RootedId id(aCx);
  for (uint32_t i = 0; i < length; i++) {
    v8::Local<v8::Value> key;
    if (!array->Get(ctx, i).ToLocal(&key)) {
      return false;
    }
    if (!IdFromV8(aCx, key, &id)) {
      return false;
    }
    if (!aProps.append(id)) {
      return false;
    }
  }
  return true;
}

bool IsProxyObject(JSObject* aObj) { return js::IsProxy(aObj); }

}

JS::Realm* ObjectRealm(JSObject* aObj) {
  return reinterpret_cast<JS::shadow::Object*>(aObj)->shape->base->realm;
}

JS::Compartment* ObjectCompartment(JSObject* aObj) {
  JS::Realm* realm = ObjectRealm(aObj);
  return realm ? JS::GetCompartmentForRealm(realm) : nullptr;
}

AutoEnterRealmOf::AutoEnterRealmOf(JSContext* aCx, JS::Realm* aRealm)
    : mCx(aCx), mOld(aCx->realm()) {
  aCx->enterRealm(aRealm);
}

AutoEnterRealmOf::AutoEnterRealmOf(JSContext* aCx, JSObject* aObj)
    : AutoEnterRealmOf(aCx, ObjectRealm(aObj)) {}

AutoEnterRealmOf::~AutoEnterRealmOf() { mCx->leaveRealm(mOld); }

void ThrowPendingToV8(JSContext* aCx) {
  v8::Isolate* isolate = aCx->isolate;
  if (aCx->exceptionPending) {
    JS::Value exception = aCx->pendingException;
    aCx->exceptionPending = false;
    aCx->pendingException = JS::UndefinedValue();
    isolate->ThrowException(ToV8(aCx, exception));
    return;
  }
  if (!isolate->IsExecutionTerminating()) {
    isolate->TerminateExecution();
  }
}

bool CallReflect(JSContext* aCx, ReflectOp aOp,
                 std::initializer_list<v8::Local<v8::Value>> aArgs,
                 v8::Local<v8::Value>* aResult) {
  EnsureIntrinsics(aCx);
  v8::Isolate* isolate = aCx->isolate;
  RuntimeState& st = State();
  v8::Local<v8::Context> intrinsics = st.intrinsics.Get(isolate);
  v8::Local<v8::Value> first = *aArgs.begin();
  if (first->IsObject()) {
    v8::Local<v8::Context> creation;
    if (first.As<v8::Object>()->GetCreationContext().ToLocal(&creation)) {
      intrinsics->SetSecurityToken(creation->GetSecurityToken());
    }
  }
  std::vector<v8::Local<v8::Value>> argv(aArgs);
  v8::Local<v8::Function> fn = st.reflect[aOp].Get(isolate);
  v8::TryCatch tryCatch(isolate);
  v8::MaybeLocal<v8::Value> result =
      fn->Call(CallingContext(aCx), v8::Undefined(isolate), int(argv.size()),
               argv.data());
  if (result.IsEmpty()) {
    if (tryCatch.HasCaught() && !tryCatch.HasTerminated()) {
      aCx->setPending(tryCatch.Exception());
    }
    return false;
  }
  *aResult = result.ToLocalChecked();
  return true;
}

std::string IdToUTF8(JSContext* aCx, JS::HandleId aId) {
  if (aId.isInt()) {
    return std::to_string(aId.toInt());
  }
  if (aId.isVoid()) {
    return std::string();
  }
  v8::Isolate* isolate = aCx->isolate;
  v8::HandleScope scope(isolate);
  v8::Local<v8::Value> key = ToV8(aCx, aId.get());
  if (key->IsSymbol()) {
    std::string out = "Symbol(";
    v8::Local<v8::Value> description = key.As<v8::Symbol>()->Description(isolate);
    if (!description->IsUndefined()) {
      out += Utf8(isolate, description);
    }
    out += ")";
    return out;
  }
  return Utf8(isolate, key);
}

std::string ValueToPrintable(JSContext* aCx, JS::HandleValue aValue) {
  v8::Isolate* isolate = aCx->isolate;
  v8::HandleScope scope(isolate);
  if (aValue.isObject()) {
    JSObject* obj = &aValue.toObject();
    if (!IsProxyObject(obj)) {
      v8::Local<v8::Object> v8obj = ToV8(aCx, obj);
      if (v8obj->IsFunction()) {
        std::string name =
            Utf8(isolate, v8obj.As<v8::Function>()->GetDebugName());
        return name.empty() ? std::string("function") : name;
      }
    }
    return std::string("[object ") + JS::GetClass(obj)->name + "]";
  }
  if (aValue.isString()) {
    return "\"" + Utf8(isolate, ToV8(aCx, aValue.get())) + "\"";
  }
  if (aValue.isSymbol()) {
    v8::Local<v8::Value> description =
        ToV8(aCx, aValue.get()).As<v8::Symbol>()->Description(isolate);
    return "Symbol(" +
           (description->IsUndefined() ? std::string()
                                       : Utf8(isolate, description)) +
           ")";
  }
  return Utf8(isolate, ToV8(aCx, aValue.get()));
}

void ReportErrorNumber(JSContext* aCx, unsigned aCode,
                       std::initializer_list<const char*> aArgs) {
  const char* argv[8] = {};
  size_t i = 0;
  for (const char* arg : aArgs) {
    if (i < 7) {
      argv[i++] = arg;
    }
  }
  JS_ReportErrorNumberUTF8Array(aCx, js::GetErrorMessage, nullptr, aCode,
                                argv);
}

bool ReportOpFailure(JSContext* aCx, JS::HandleObject aObj, JS::HandleId aId,
                     const JS::ObjectOpResult& aResult) {
  unsigned code = aResult.failureCode();
  JS::RootedValue objValue(aCx, JS::ObjectValue(*aObj));
  if (code == JSMSG_OBJECT_NOT_EXTENSIBLE) {
    std::string printable = ValueToPrintable(aCx, objValue);
    ReportErrorNumber(aCx, code, {printable.c_str()});
    return false;
  }
  const JSErrorFormatString* format = js::GetErrorMessage(nullptr, code);
  if (format && format->argCount > 0) {
    std::string name = IdToUTF8(aCx, aId);
    if (code == JSMSG_SET_NON_OBJECT_RECEIVER) {
      std::string printable = ValueToPrintable(aCx, objValue);
      ReportErrorNumber(aCx, code, {printable.c_str(), name.c_str()});
      return false;
    }
    if (format->argCount == 2) {
      JSObject* unwrapped = js::CheckedUnwrapStatic(aObj);
      const char* className =
          unwrapped ? JS::GetClass(unwrapped)->name : "Object";
      ReportErrorNumber(aCx, code, {className, name.c_str()});
      return false;
    }
    ReportErrorNumber(aCx, code, {name.c_str()});
    return false;
  }
  ReportErrorNumber(aCx, code, {});
  return false;
}

bool ReportIncompatible(JSContext* aCx, const JS::CallArgs& aArgs) {
  JS::RootedValue callee(aCx, aArgs.calleev());
  if (!callee.isObject() || !IsCallableObject(&callee.toObject())) {
    ReportNotFunction(aCx, callee, JSMSG_NOT_FUNCTION);
    return false;
  }
  std::string name = ValueToPrintable(aCx, callee);
  ReportErrorNumber(aCx, JSMSG_INCOMPATIBLE_METHOD,
                    {name.c_str(), "method", InformalTypeName(aArgs.thisv())});
  return false;
}

void ReportNotFunction(JSContext* aCx, JS::HandleValue aValue,
                       unsigned aCode) {
  std::string printable = ValueToPrintable(aCx, aValue);
  ReportErrorNumber(aCx, aCode, {printable.c_str()});
}

v8::Local<v8::Object> DescriptorToV8(JSContext* aCx,
                                     JS::Handle<JS::PropertyDescriptor> aDesc) {
  v8::Isolate* isolate = aCx->isolate;
  v8::Local<v8::Name> names[6];
  v8::Local<v8::Value> values[6];
  size_t n = 0;
  if (aDesc.hasValue()) {
    names[n] = v8::String::NewFromUtf8Literal(isolate, "value");
    values[n++] = ToV8(aCx, aDesc.value());
  }
  if (aDesc.hasWritable()) {
    names[n] = v8::String::NewFromUtf8Literal(isolate, "writable");
    values[n++] = v8::Boolean::New(isolate, aDesc.writable());
  }
  if (aDesc.hasGetter()) {
    names[n] = v8::String::NewFromUtf8Literal(isolate, "get");
    values[n++] = aDesc.getter()
                      ? v8::Local<v8::Value>(ToV8(aCx, aDesc.getter()))
                      : v8::Local<v8::Value>(v8::Undefined(isolate));
  }
  if (aDesc.hasSetter()) {
    names[n] = v8::String::NewFromUtf8Literal(isolate, "set");
    values[n++] = aDesc.setter()
                      ? v8::Local<v8::Value>(ToV8(aCx, aDesc.setter()))
                      : v8::Local<v8::Value>(v8::Undefined(isolate));
  }
  if (aDesc.hasEnumerable()) {
    names[n] = v8::String::NewFromUtf8Literal(isolate, "enumerable");
    values[n++] = v8::Boolean::New(isolate, aDesc.enumerable());
  }
  if (aDesc.hasConfigurable()) {
    names[n] = v8::String::NewFromUtf8Literal(isolate, "configurable");
    values[n++] = v8::Boolean::New(isolate, aDesc.configurable());
  }
  return v8::Object::New(isolate, v8::Null(isolate), names, values, n);
}

bool DescriptorFromV8(
    JSContext* aCx, v8::Local<v8::Value> aObject,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> aDesc) {
  if (!aObject->IsObject()) {
    aDesc.reset();
    return true;
  }
  v8::Isolate* isolate = aCx->isolate;
  v8::Local<v8::Object> obj = aObject.As<v8::Object>();
  v8::Local<v8::Context> ctx = CallingContext(aCx);
  auto field = [&](const char* aName, v8::Local<v8::Value>* aOut) -> int {
    v8::Local<v8::String> key =
        v8::String::NewFromUtf8(isolate, aName).ToLocalChecked();
    bool has = false;
    if (!obj->HasOwnProperty(ctx, key).To(&has)) {
      return -1;
    }
    if (!has) {
      return 0;
    }
    if (!obj->Get(ctx, key).ToLocal(aOut)) {
      return -1;
    }
    return 1;
  };
  JS::Rooted<JS::PropertyDescriptor> desc(aCx, JS::PropertyDescriptor::Empty());
  v8::Local<v8::Value> v;
  int r;
  if ((r = field("configurable", &v)) < 0) {
    return false;
  }
  if (r) {
    desc.setConfigurable(v->BooleanValue(isolate));
  }
  if ((r = field("enumerable", &v)) < 0) {
    return false;
  }
  if (r) {
    desc.setEnumerable(v->BooleanValue(isolate));
  }
  if ((r = field("value", &v)) < 0) {
    return false;
  }
  if (r) {
    JS::RootedValue value(aCx, ValueFromV8(aCx, v));
    desc.setValue(value);
  }
  if ((r = field("writable", &v)) < 0) {
    return false;
  }
  if (r) {
    desc.setWritable(v->BooleanValue(isolate));
  }
  if ((r = field("get", &v)) < 0) {
    return false;
  }
  if (r) {
    desc.setGetter(v->IsObject() ? ObjectFromV8(aCx, v.As<v8::Object>())
                                 : nullptr);
  }
  if ((r = field("set", &v)) < 0) {
    return false;
  }
  if (r) {
    desc.setSetter(v->IsObject() ? ObjectFromV8(aCx, v.As<v8::Object>())
                                 : nullptr);
  }
  aDesc.set(mozilla::Some(desc.get()));
  return true;
}

bool GetOwnPropertyDescriptor(
    JSContext* aCx, JS::HandleObject aObj, JS::HandleId aId,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> aDesc) {
  if (IsProxyObject(aObj)) {
    return ProxyGetOwnPropertyDescriptor(aCx, aObj, aId, aDesc);
  }
  AutoV8 v8(aCx);
  v8::Local<v8::Value> result;
  if (!CallReflect(aCx, kReflectGetOwnPropertyDescriptor,
                   {ToV8(aCx, aObj.get()), ToV8(aCx, aId.get())}, &result)) {
    return false;
  }
  return DescriptorFromV8(aCx, result, aDesc);
}

bool DefineProperty(JSContext* aCx, JS::HandleObject aObj, JS::HandleId aId,
                    JS::Handle<JS::PropertyDescriptor> aDesc,
                    JS::ObjectOpResult& aResult) {
  if (IsProxyObject(aObj)) {
    return ProxyDefineProperty(aCx, aObj, aId, aDesc, aResult);
  }
  AutoV8 v8(aCx);
  v8::Local<v8::Value> result;
  if (!CallReflect(aCx, kReflectDefineProperty,
                   {ToV8(aCx, aObj.get()), ToV8(aCx, aId.get()),
                    DescriptorToV8(aCx, aDesc)},
                   &result)) {
    return false;
  }
  return result->IsTrue() ? aResult.succeed()
                          : aResult.fail(JSMSG_CANT_REDEFINE_PROP);
}

bool OwnPropertyKeys(JSContext* aCx, JS::HandleObject aObj,
                     JS::MutableHandleIdVector aProps) {
  if (IsProxyObject(aObj)) {
    return ProxyOwnPropertyKeys(aCx, aObj, aProps);
  }
  AutoV8 v8(aCx);
  v8::Local<v8::Value> result;
  if (!CallReflect(aCx, kReflectOwnKeys, {ToV8(aCx, aObj.get())}, &result)) {
    return false;
  }
  return ArrayToIds(aCx, result, aProps);
}

bool OwnEnumerableStringKeys(JSContext* aCx, JS::HandleObject aObj,
                             JS::MutableHandleIdVector aProps) {
  if (IsProxyObject(aObj)) {
    return ProxyGetOwnEnumerablePropertyKeys(aCx, aObj, aProps);
  }
  JS::RootedIdVector all(aCx);
  if (!OwnPropertyKeys(aCx, aObj, &all)) {
    return false;
  }
  JS::Rooted<mozilla::Maybe<JS::PropertyDescriptor>> desc(aCx);
  JS::RootedId id(aCx);
  for (size_t i = 0; i < all.length(); i++) {
    id = all[i];
    if (id.isSymbol()) {
      continue;
    }
    if (!GetOwnPropertyDescriptor(aCx, aObj, id, &desc)) {
      return false;
    }
    if (desc.isSome() && desc->enumerable() && !aProps.append(id)) {
      return false;
    }
  }
  return true;
}

bool ForInKeys(JSContext* aCx, JS::HandleObject aObj,
               JS::MutableHandleIdVector aProps) {
  std::unordered_set<uint64_t> seen;
  for (size_t i = 0; i < aProps.length(); i++) {
    seen.insert(aProps[i].get().asRawBits());
  }
  JS::RootedObject current(aCx, aObj);
  JS::RootedIdVector keys(aCx);
  JS::Rooted<mozilla::Maybe<JS::PropertyDescriptor>> desc(aCx);
  JS::RootedId id(aCx);
  while (current) {
    keys.clear();
    if (IsProxyObject(current)) {
      if (!ProxyGetOwnEnumerablePropertyKeys(aCx, current, &keys)) {
        return false;
      }
      for (size_t i = 0; i < keys.length(); i++) {
        id = keys[i];
        if (id.isSymbol() || !seen.insert(id.get().asRawBits()).second) {
          continue;
        }
        if (!aProps.append(id)) {
          return false;
        }
      }
    } else {
      if (!OwnPropertyKeys(aCx, current, &keys)) {
        return false;
      }
      for (size_t i = 0; i < keys.length(); i++) {
        id = keys[i];
        if (id.isSymbol() || !seen.insert(id.get().asRawBits()).second) {
          continue;
        }
        if (!GetOwnPropertyDescriptor(aCx, current, id, &desc)) {
          return false;
        }
        if (desc.isSome() && desc->enumerable() && !aProps.append(id)) {
          return false;
        }
      }
    }
    JS::RootedObject proto(aCx);
    if (!GetPrototype(aCx, current, &proto)) {
      return false;
    }
    current = proto;
  }
  return true;
}

bool DeleteProperty(JSContext* aCx, JS::HandleObject aObj, JS::HandleId aId,
                    JS::ObjectOpResult& aResult) {
  if (IsProxyObject(aObj)) {
    return ProxyDelete(aCx, aObj, aId, aResult);
  }
  AutoV8 v8(aCx);
  v8::Local<v8::Value> result;
  if (!CallReflect(aCx, kReflectDeleteProperty,
                   {ToV8(aCx, aObj.get()), ToV8(aCx, aId.get())}, &result)) {
    return false;
  }
  return result->IsTrue() ? aResult.succeed() : aResult.fail(JSMSG_CANT_DELETE);
}

bool HasProperty(JSContext* aCx, JS::HandleObject aObj, JS::HandleId aId,
                 bool* aFound) {
  if (IsProxyObject(aObj)) {
    return ProxyHas(aCx, aObj, aId, aFound);
  }
  AutoV8 v8(aCx);
  v8::Local<v8::Value> result;
  if (!CallReflect(aCx, kReflectHas,
                   {ToV8(aCx, aObj.get()), ToV8(aCx, aId.get())}, &result)) {
    return false;
  }
  *aFound = result->IsTrue();
  return true;
}

bool HasOwnProperty(JSContext* aCx, JS::HandleObject aObj, JS::HandleId aId,
                    bool* aFound) {
  if (IsProxyObject(aObj)) {
    return ProxyHasOwn(aCx, aObj, aId, aFound);
  }
  AutoV8 v8(aCx);
  v8::Local<v8::Value> result;
  if (!CallReflect(aCx, kReflectGetOwnPropertyDescriptor,
                   {ToV8(aCx, aObj.get()), ToV8(aCx, aId.get())}, &result)) {
    return false;
  }
  *aFound = !result->IsUndefined();
  return true;
}

bool GetProperty(JSContext* aCx, JS::HandleObject aObj,
                 JS::HandleValue aReceiver, JS::HandleId aId,
                 JS::MutableHandleValue aVp) {
  if (IsProxyObject(aObj)) {
    return ProxyGet(aCx, aObj, aReceiver, aId, aVp);
  }
  AutoV8 v8(aCx);
  v8::Local<v8::Value> result;
  if (!CallReflect(aCx, kReflectGet,
                   {ToV8(aCx, aObj.get()), ToV8(aCx, aId.get()),
                    ToV8(aCx, aReceiver.get())},
                   &result)) {
    return false;
  }
  aVp.set(ValueFromV8(aCx, result));
  return true;
}

bool SetProperty(JSContext* aCx, JS::HandleObject aObj, JS::HandleId aId,
                 JS::HandleValue aValue, JS::HandleValue aReceiver,
                 JS::ObjectOpResult& aResult) {
  if (IsProxyObject(aObj)) {
    return ProxySet(aCx, aObj, aId, aValue, aReceiver, aResult);
  }
  AutoV8 v8(aCx);
  v8::Local<v8::Value> result;
  if (!CallReflect(aCx, kReflectSet,
                   {ToV8(aCx, aObj.get()), ToV8(aCx, aId.get()),
                    ToV8(aCx, aValue.get()), ToV8(aCx, aReceiver.get())},
                   &result)) {
    return false;
  }
  return result->IsTrue() ? aResult.succeed() : aResult.fail(JSMSG_READ_ONLY);
}

bool SetPropertyIgnoringNamedGetter(
    JSContext* aCx, JS::HandleObject aObj, JS::HandleId aId,
    JS::HandleValue aValue, JS::HandleValue aReceiver,
    JS::Handle<mozilla::Maybe<JS::PropertyDescriptor>> aOwnDesc,
    JS::ObjectOpResult& aResult) {
  JS::Rooted<JS::PropertyDescriptor> ownDesc(aCx);
  if (aOwnDesc.isNothing()) {
    JS::RootedObject proto(aCx);
    if (!GetPrototype(aCx, aObj, &proto)) {
      return false;
    }
    if (proto) {
      return SetProperty(aCx, proto, aId, aValue, aReceiver, aResult);
    }
    ownDesc.set(JS::PropertyDescriptor::Data(
        JS::UndefinedValue(),
        {JS::PropertyAttribute::Configurable, JS::PropertyAttribute::Enumerable,
         JS::PropertyAttribute::Writable}));
  } else {
    ownDesc.set(*aOwnDesc);
  }
  if (ownDesc.isDataDescriptor()) {
    if (!ownDesc.writable()) {
      return aResult.fail(JSMSG_READ_ONLY);
    }
    if (!aReceiver.isObject()) {
      return aResult.fail(JSMSG_SET_NON_OBJECT_RECEIVER);
    }
    JS::RootedObject receiverObj(aCx, &aReceiver.toObject());
    JS::Rooted<mozilla::Maybe<JS::PropertyDescriptor>> existing(aCx);
    if (!GetOwnPropertyDescriptor(aCx, receiverObj, aId, &existing)) {
      return false;
    }
    if (existing.isSome()) {
      if (existing->isAccessorDescriptor()) {
        return aResult.fail(JSMSG_OVERWRITING_ACCESSOR);
      }
      if (!existing->writable()) {
        return aResult.fail(JSMSG_READ_ONLY);
      }
    }
    JS::Rooted<JS::PropertyDescriptor> desc(aCx);
    if (existing.isSome()) {
      desc = JS::PropertyDescriptor::Empty();
      desc.setValue(aValue);
    } else {
      desc = JS::PropertyDescriptor::Data(
          aValue, {JS::PropertyAttribute::Configurable,
                   JS::PropertyAttribute::Enumerable,
                   JS::PropertyAttribute::Writable});
    }
    return DefineProperty(aCx, receiverObj, aId, desc, aResult);
  }
  JS::RootedObject setter(aCx);
  if (ownDesc.hasSetter()) {
    setter = ownDesc.setter();
  }
  if (!setter) {
    return aResult.fail(JSMSG_GETTER_ONLY);
  }
  if (!CallSetter(aCx, aReceiver, setter, aValue)) {
    return false;
  }
  return aResult.succeed();
}

bool GetPrototype(JSContext* aCx, JS::HandleObject aObj,
                  JS::MutableHandleObject aProto) {
  if (IsProxyObject(aObj) && IsLazyProto(aObj)) {
    return ProxyGetPrototype(aCx, aObj, aProto);
  }
  AutoV8 v8(aCx);
  v8::Local<v8::Value> result;
  if (!CallReflect(aCx, kReflectGetPrototypeOf, {ToV8(aCx, aObj.get())},
                   &result)) {
    return false;
  }
  aProto.set(result->IsObject() ? ObjectFromV8(aCx, result.As<v8::Object>())
                                : nullptr);
  return true;
}

bool SetPrototype(JSContext* aCx, JS::HandleObject aObj, JS::HandleObject aProto,
                  JS::ObjectOpResult& aResult) {
  if (IsProxyObject(aObj) && IsLazyProto(aObj)) {
    return ProxySetPrototype(aCx, aObj, aProto, aResult);
  }
  AutoV8 v8(aCx);
  v8::Local<v8::Value> proto =
      aProto ? v8::Local<v8::Value>(ToV8(aCx, aProto.get()))
             : v8::Local<v8::Value>(v8::Null(aCx->isolate));
  v8::Local<v8::Value> result;
  if (!CallReflect(aCx, kReflectSetPrototypeOf, {ToV8(aCx, aObj.get()), proto},
                   &result)) {
    return false;
  }
  return result->IsTrue() ? aResult.succeed()
                          : aResult.fail(JSMSG_CANT_SET_PROTO);
}

bool GetPrototypeIfOrdinary(JSContext* aCx, JS::HandleObject aObj,
                            bool* aIsOrdinary, JS::MutableHandleObject aProto) {
  if (IsProxyObject(aObj)) {
    return ProxyGetPrototypeIfOrdinary(aCx, aObj, aIsOrdinary, aProto);
  }
  *aIsOrdinary = true;
  return GetPrototype(aCx, aObj, aProto);
}

bool SetImmutablePrototype(JSContext* aCx, JS::HandleObject aObj,
                           bool* aSucceeded) {
  if (IsProxyObject(aObj)) {
    if (IsLazyProto(aObj)) {
      return ProxySetImmutablePrototype(aCx, aObj, aSucceeded);
    }
    *aSucceeded = false;
    return true;
  }
  return JS_SetImmutablePrototype(aCx, aObj, aSucceeded);
}

bool PreventExtensions(JSContext* aCx, JS::HandleObject aObj,
                       JS::ObjectOpResult& aResult) {
  if (IsProxyObject(aObj)) {
    return ProxyPreventExtensions(aCx, aObj, aResult);
  }
  AutoV8 v8(aCx);
  v8::Local<v8::Value> result;
  if (!CallReflect(aCx, kReflectPreventExtensions, {ToV8(aCx, aObj.get())},
                   &result)) {
    return false;
  }
  return result->IsTrue() ? aResult.succeed()
                          : aResult.fail(JSMSG_CANT_CHANGE_EXTENSIBILITY);
}

bool IsExtensible(JSContext* aCx, JS::HandleObject aObj, bool* aExtensible) {
  if (IsProxyObject(aObj)) {
    return ProxyIsExtensible(aCx, aObj, aExtensible);
  }
  AutoV8 v8(aCx);
  v8::Local<v8::Value> result;
  if (!CallReflect(aCx, kReflectIsExtensible, {ToV8(aCx, aObj.get())},
                   &result)) {
    return false;
  }
  *aExtensible = result->IsTrue();
  return true;
}

namespace {

bool CallValues(JSContext* aCx, JS::HandleValue aCallee, JS::HandleValue aThis,
                unsigned aArgc, const JS::Value* aArgv,
                JS::MutableHandleValue aRval) {
  if (!aCallee.isObject() || !IsCallableObject(&aCallee.toObject())) {
    ReportNotFunction(aCx, aCallee, JSMSG_NOT_FUNCTION);
    return false;
  }
  JS::RootedObject callee(aCx, &aCallee.toObject());
  if (IsProxyObject(callee)) {
    JS::RootedValueVector vp(aCx);
    if (!vp.resize(aArgc + 2)) {
      return false;
    }
    vp[0].set(aCallee);
    vp[1].set(aThis);
    for (unsigned i = 0; i < aArgc; i++) {
      vp[2 + i].set(aArgv[i]);
    }
    JS::CallArgs args = JS::CallArgsFromVp(aArgc, vp.begin());
    if (!ProxyCall(aCx, callee, args)) {
      return false;
    }
    aRval.set(args.rval());
    return true;
  }
  AutoV8 v8(aCx);
  v8::Isolate* isolate = aCx->isolate;
  std::vector<v8::Local<v8::Value>> argv(aArgc);
  for (unsigned i = 0; i < aArgc; i++) {
    argv[i] = ToV8(aCx, aArgv[i]);
  }
  v8::TryCatch tryCatch(isolate);
  v8::MaybeLocal<v8::Value> result = ToV8(aCx, callee.get())->CallAsFunction(
      CallingContext(aCx), ToV8(aCx, aThis.get()), int(aArgc), argv.data());
  if (result.IsEmpty()) {
    if (tryCatch.HasCaught() && !tryCatch.HasTerminated()) {
      aCx->setPending(tryCatch.Exception());
    }
    return false;
  }
  aRval.set(ValueFromV8(aCx, result.ToLocalChecked()));
  return true;
}

bool ConstructValues(JSContext* aCx, JS::HandleValue aCallee, unsigned aArgc,
                     const JS::Value* aArgv, JS::HandleValue aNewTarget,
                     JS::MutableHandleObject aResult) {
  if (!aCallee.isObject() || !IsConstructorObject(&aCallee.toObject())) {
    ReportNotFunction(aCx, aCallee, JSMSG_NOT_CONSTRUCTOR);
    return false;
  }
  JS::RootedObject callee(aCx, &aCallee.toObject());
  if (IsProxyObject(callee)) {
    JS::RootedValueVector vp(aCx);
    if (!vp.resize(aArgc + 3)) {
      return false;
    }
    vp[0].set(aCallee);
    vp[1].set(JS::MagicValue(JS_IS_CONSTRUCTING));
    for (unsigned i = 0; i < aArgc; i++) {
      vp[2 + i].set(aArgv[i]);
    }
    vp[2 + aArgc].set(aNewTarget);
    JS::CallArgs args = JS::CallArgsFromVp(aArgc, vp.begin());
    if (!ProxyConstruct(aCx, callee, args)) {
      return false;
    }
    if (!args.rval().isObject()) {
      ReportNotFunction(aCx, aCallee, JSMSG_NOT_CONSTRUCTOR);
      return false;
    }
    aResult.set(&args.rval().toObject());
    return true;
  }
  AutoV8 v8(aCx);
  v8::Isolate* isolate = aCx->isolate;
  std::vector<v8::Local<v8::Value>> argv(aArgc);
  for (unsigned i = 0; i < aArgc; i++) {
    argv[i] = ToV8(aCx, aArgv[i]);
  }
  v8::Local<v8::Array> list = v8::Array::New(isolate, argv.data(), aArgc);
  v8::Local<v8::Value> result;
  if (!CallReflect(aCx, kReflectConstruct,
                   {ToV8(aCx, callee.get()), list, ToV8(aCx, aNewTarget.get())},
                   &result)) {
    return false;
  }
  aResult.set(ObjectFromV8(aCx, result.As<v8::Object>()));
  return true;
}

}

bool Call(JSContext* aCx, JS::HandleValue aCallee, JS::HandleValue aThis,
          const JS::CallArgs& aArgs, JS::MutableHandleValue aRval) {
  return CallValues(aCx, aCallee, aThis, aArgs.length(), aArgs.array(), aRval);
}

bool Construct(JSContext* aCx, JS::HandleValue aCallee,
               const JS::CallArgs& aArgs, JS::HandleValue aNewTarget,
               JS::MutableHandleObject aResult) {
  return ConstructValues(aCx, aCallee, aArgs.length(), aArgs.array(),
                         aNewTarget, aResult);
}

bool CallGetter(JSContext* aCx, JS::HandleValue aReceiver,
                JS::HandleObject aGetter, JS::MutableHandleValue aVp) {
  JS::RootedValue callee(aCx, JS::ObjectValue(*aGetter));
  return CallValues(aCx, callee, aReceiver, 0, nullptr, aVp);
}

bool CallSetter(JSContext* aCx, JS::HandleValue aReceiver,
                JS::HandleObject aSetter, JS::HandleValue aValue) {
  JS::RootedValue callee(aCx, JS::ObjectValue(*aSetter));
  JS::RootedValue ignored(aCx);
  JS::Value arg = aValue.get();
  return CallValues(aCx, callee, aReceiver, 1, &arg, &ignored);
}

bool IsCallableObject(JSObject* aObj) {
  if (IsProxyObject(aObj)) {
    return js::GetProxyHandler(aObj)->isCallable(aObj);
  }
  JSContext* cx = CurrentContext();
  v8::HandleScope scope(cx->isolate);
  return ToV8(cx, aObj)->IsCallable();
}

bool IsConstructorObject(JSObject* aObj) {
  if (IsProxyObject(aObj)) {
    return js::GetProxyHandler(aObj)->isConstructor(aObj);
  }
  JSContext* cx = CurrentContext();
  v8::HandleScope scope(cx->isolate);
  return ToV8(cx, aObj)->IsConstructor();
}

bool IsArray(JSContext* aCx, JS::HandleObject aObj,
             JS::IsArrayAnswer* aAnswer) {
  if (IsProxyObject(aObj)) {
    return ProxyIsArray(aCx, aObj, aAnswer);
  }
  v8::HandleScope scope(aCx->isolate);
  *aAnswer = ToV8(aCx, aObj.get())->IsArray() ? JS::IsArrayAnswer::Array
                                              : JS::IsArrayAnswer::NotArray;
  return true;
}

bool GetBuiltinClass(JSContext* aCx, JS::HandleObject aObj,
                     js::ESClass* aCls) {
  if (IsProxyObject(aObj)) {
    return ProxyGetBuiltinClass(aCx, aObj, aCls);
  }
  return JS::GetBuiltinClass(aCx, aObj, aCls);
}

const char* ClassName(JSContext* aCx, JS::HandleObject aObj) {
  if (IsProxyObject(aObj)) {
    return ProxyClassName(aCx, aObj);
  }
  return JS::GetClass(aObj)->name;
}

JSString* FunToString(JSContext* aCx, JS::HandleObject aObj,
                      bool aIsToSource) {
  if (IsProxyObject(aObj)) {
    return ProxyFunToString(aCx, aObj, aIsToSource);
  }
  AutoV8 v8(aCx);
  v8::Local<v8::Object> obj = ToV8(aCx, aObj.get());
  if (!obj->IsFunction()) {
    ReportErrorNumber(aCx, JSMSG_INCOMPATIBLE_PROTO,
                      {"Function", "toString", "object"});
    return nullptr;
  }
  v8::TryCatch tryCatch(aCx->isolate);
  v8::Local<v8::String> source;
  if (!obj.As<v8::Function>()
           ->FunctionProtoToString(CallingContext(aCx))
           .ToLocal(&source)) {
    v8.Check(tryCatch);
    return nullptr;
  }
  return StringFromV8(aCx, source);
}

bool Unbox(JSContext* aCx, JS::HandleObject aObj, JS::MutableHandleValue aVp) {
  if (IsProxyObject(aObj)) {
    return ProxyBoxedValueUnbox(aCx, aObj, aVp);
  }
  AutoV8 v8(aCx);
  v8::Local<v8::Object> obj = ToV8(aCx, aObj.get());
  if (obj->IsBooleanObject()) {
    aVp.setBoolean(obj.As<v8::BooleanObject>()->ValueOf());
  } else if (obj->IsNumberObject()) {
    aVp.setNumber(obj.As<v8::NumberObject>()->ValueOf());
  } else if (obj->IsStringObject()) {
    aVp.set(ValueFromV8(aCx, obj.As<v8::StringObject>()->ValueOf()));
  } else if (obj->IsSymbolObject()) {
    aVp.set(ValueFromV8(aCx, obj.As<v8::SymbolObject>()->ValueOf()));
  } else if (obj->IsBigIntObject()) {
    aVp.set(ValueFromV8(aCx, obj.As<v8::BigIntObject>()->ValueOf()));
  } else {
    aVp.setUndefined();
  }
  return true;
}

}
