#include "VgeJsClassesState.h"

#include <string>

#include "js/String.h"
#include "js/Symbol.h"
#include "js/experimental/JitInfo.h"
#include "jsfriendapi.h"

namespace vge::jsapi {

JSScript* ScriptForFunction(JSContext* aCx, v8::Local<v8::Function> aFunction);

namespace {

constexpr uint32_t kFlagExtended = 1 << 3;
constexpr uint32_t kFlagConstructor = 1 << 8;
constexpr uint32_t kArgCountShift = 16;

constexpr size_t kFlagsSlot = JS::shadow::Function::FlagsAndArgCountSlot;
constexpr size_t kNativeSlot = JS::shadow::Function::NativeFuncOrInterpretedEnvSlot;
constexpr size_t kJitInfoSlot =
    JS::shadow::Function::NativeJitInfoOrInterpretedScriptSlot;
constexpr size_t kAtomSlot = JS::shadow::Function::AtomSlot;
constexpr size_t kFirstExtendedSlot = 4;

#define VGE_SYMBOL_NAME(name) #name,
const char* const kWellKnownSymbolNames[] = {
    JS_FOR_EACH_WELL_KNOWN_SYMBOL(VGE_SYMBOL_NAME)};
#undef VGE_SYMBOL_NAME

JS::Value* Slots(JSObject* aFun) { return ObjectCell::from(aFun)->fixedSlots(); }

bool IsNativeCell(JSObject* aFun) {
  return JS::GetClass(aFun)->isJSFunction() && Slots(aFun)[kFlagsSlot].isInt32();
}

uint32_t FlagsAndArgs(JSObject* aFun) {
  return uint32_t(Slots(aFun)[kFlagsSlot].toInt32());
}

void NativeFunctionCallback(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  JSContext* cx = CurrentContext();
  v8::Local<v8::Object> holder = aInfo.Data().As<v8::Object>();
  Anchor* anchor =
      static_cast<Anchor*>(holder->GetAlignedPointerFromInternalField(1));
  ObjectCell* cell = anchor->Cell();
  JSNative native =
      reinterpret_cast<JSNative>(cell->fixedSlots()[kNativeSlot].toPrivate());
  InvokeNative(cx, native, cell->shape->base.realm,
               ToV8(cx, cell->asObject()), aInfo);
}

v8::Local<v8::Function> V8Function(JSContext* aCx, JSObject* aFun) {
  return ToV8(aCx, aFun).As<v8::Function>();
}

JSString* NameFromV8(JSContext* aCx, v8::Local<v8::Value> aName) {
  if (aName.IsEmpty() || !aName->IsString() ||
      aName.As<v8::String>()->Length() == 0) {
    return nullptr;
  }
  return AtomFromV8(aCx, aName.As<v8::String>());
}

bool IsBuiltinOf(JSObject* aFun, v8::Global<v8::Function> RealmExtra::*aMember) {
  JSContext* cx = CurrentContext();
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::Object> fn = ToV8(cx, aFun);
  ClassesState& state = State(cx);
  for (auto& [realm, extra] : state.realms) {
    v8::Global<v8::Function>& g = extra->*aMember;
    if (!g.IsEmpty() && g.Get(cx->isolate) == fn) {
      return true;
    }
  }
  return false;
}

}

void DirectSlowPath(const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
  NativeFunctionCallback(aInfo);
}

void DirectThrowPending(JSContext* aCx) { ThrowPendingToV8(aCx); }

JS::Symbol* WellKnownSymbol(JSContext* aCx, JS::SymbolCode aCode) {
  v8::Isolate* isolate = aCx->isolate;
  v8::EscapableHandleScope scope(isolate);
  uint32_t code = uint32_t(aCode);
  v8::Local<v8::Symbol> symbol;
  JS::Realm* realm = aCx->realm();
  v8::Local<v8::Context> ctx = aCx->v8Context();
  if (realm && !ctx.IsEmpty()) {
    RealmExtra& extra = RealmData(realm);
    v8::Local<v8::Object> ctor;
    if (!extra.ctors[JSProto_Symbol].IsEmpty()) {
      ctor = extra.ctors[JSProto_Symbol].Get(isolate);
    }
    if (!ctor.IsEmpty() && code < uint32_t(JS::SymbolCode::Limit)) {
      v8::Local<v8::Value> value;
      if (ctor->GetRealNamedProperty(
                  ctx, v8::String::NewFromUtf8(isolate,
                                               kWellKnownSymbolNames[code])
                           .ToLocalChecked())
              .ToLocal(&value) &&
          value->IsSymbol()) {
        symbol = value.As<v8::Symbol>();
      }
    }
  }
  if (symbol.IsEmpty()) {
    switch (aCode) {
      case JS::SymbolCode::isConcatSpreadable:
        symbol = v8::Symbol::GetIsConcatSpreadable(isolate);
        break;
      case JS::SymbolCode::iterator:
        symbol = v8::Symbol::GetIterator(isolate);
        break;
      case JS::SymbolCode::match:
        symbol = v8::Symbol::GetMatch(isolate);
        break;
      case JS::SymbolCode::replace:
        symbol = v8::Symbol::GetReplace(isolate);
        break;
      case JS::SymbolCode::search:
        symbol = v8::Symbol::GetSearch(isolate);
        break;
      case JS::SymbolCode::hasInstance:
        symbol = v8::Symbol::GetHasInstance(isolate);
        break;
      case JS::SymbolCode::split:
        symbol = v8::Symbol::GetSplit(isolate);
        break;
      case JS::SymbolCode::toPrimitive:
        symbol = v8::Symbol::GetToPrimitive(isolate);
        break;
      case JS::SymbolCode::toStringTag:
        symbol = v8::Symbol::GetToStringTag(isolate);
        break;
      case JS::SymbolCode::unscopables:
        symbol = v8::Symbol::GetUnscopables(isolate);
        break;
      case JS::SymbolCode::asyncIterator:
        symbol = v8::Symbol::GetAsyncIterator(isolate);
        break;
      default: {
        ClassesState& state = State(aCx);
        v8::Global<v8::Symbol>& fallback = state.fallbackSymbols[code];
        if (fallback.IsEmpty()) {
          std::string description = "Symbol.";
          if (code < uint32_t(JS::SymbolCode::Limit)) {
            description += kWellKnownSymbolNames[code];
          }
          fallback.Reset(
              isolate,
              v8::Symbol::New(isolate, v8::String::NewFromUtf8(
                                           isolate, description.c_str())
                                           .ToLocalChecked()));
        }
        symbol = fallback.Get(isolate);
        break;
      }
    }
  }
  JS::Symbol* result = SymbolFromV8(aCx, scope.Escape(symbol));
  SymbolCell::from(result)->code = code;
  return result;
}

bool PropertyNameToId(JSContext* aCx, JSPropertySpec::Name aName,
                      JS::MutableHandleId aId) {
  if (aName.isSymbol()) {
    aId.set(JS::PropertyKey::Symbol(WellKnownSymbol(aCx, aName.symbol())));
    return true;
  }
  v8::HandleScope scope(aCx->isolate);
  v8::Local<v8::String> name;
  if (!v8::String::NewFromUtf8(aCx->isolate, aName.string()).ToLocal(&name)) {
    JS_ReportOutOfMemory(aCx);
    return false;
  }
  return IdFromV8(aCx, name, aId);
}

bool IdToFunctionName(JSContext* aCx, JS::HandleId aId,
                      JS::MutableHandleString aName) {
  if (aId.isString()) {
    aName.set(aId.toString());
    return true;
  }
  v8::Isolate* isolate = aCx->isolate;
  v8::HandleScope scope(isolate);
  if (aId.isInt()) {
    v8::Local<v8::String> text =
        v8::Integer::New(isolate, aId.toInt())
            ->ToString(isolate->GetCurrentContext())
            .ToLocalChecked();
    aName.set(AtomFromV8(aCx, text));
    return true;
  }
  if (aId.isSymbol()) {
    v8::Local<v8::Symbol> symbol = ToV8(aCx, aId.toSymbol());
    v8::Local<v8::Value> description = symbol->Description(isolate);
    v8::Local<v8::String> text = v8::String::Empty(isolate);
    if (description->IsString()) {
      text = v8::String::Concat(
          isolate, v8::String::NewFromUtf8Literal(isolate, "["),
          v8::String::Concat(isolate, description.As<v8::String>(),
                             v8::String::NewFromUtf8Literal(isolate, "]")));
    }
    aName.set(AtomFromV8(aCx, text));
    return true;
  }
  aName.set(nullptr);
  return true;
}

JSFunction* NewNativeFunctionImpl(JSContext* aCx, JSNative aNative,
                                  unsigned aNargs, unsigned aFlags,
                                  JS::HandleString aName,
                                  const JSJitInfo* aInfo, bool aExtended,
                                  JS::HandleObject aProto) {
  AutoV8 v8(aCx);
  v8::Isolate* isolate = aCx->isolate;
  v8::Local<v8::Context> ctx = v8.context();
  v8::TryCatch tryCatch(isolate);
  bool constructor = aFlags & JSFUN_CONSTRUCTOR;
  v8::Local<v8::Object> holder;
  if (!HolderTemplate(aCx)->NewInstance(ctx).ToLocal(&holder)) {
    v8.Check(tryCatch);
    return nullptr;
  }
  v8::Local<v8::Function> fn;
  v8::FunctionCallback callback = aInfo && aInfo->vgeDirect
                                      ? aInfo->vgeDirect
                                      : NativeFunctionCallback;
  if (!v8::Function::New(ctx, callback, holder, int(aNargs),
                         constructor ? v8::ConstructorBehavior::kAllow
                                     : v8::ConstructorBehavior::kThrow)
           .ToLocal(&fn)) {
    v8.Check(tryCatch);
    return nullptr;
  }
  JS::RootedString atom(aCx);
  if (aName) {
    atom = AtomizeCell(aCx, aName);
    fn->SetName(ToV8(aCx, atom.get()));
  }
  if (aProto) {
    if (fn->SetPrototype(ctx, ToV8(aCx, aProto)).IsNothing()) {
      v8.Check(tryCatch);
      return nullptr;
    }
  }
  Shape* shape =
      ShapeFor(aExtended ? &FunctionExtendedClass : &FunctionClass, aCx->realm());
  ObjectCell* cell = NewObjectCell(aCx, shape, fn, false);
  uint32_t flags = (constructor ? kFlagConstructor : 0) |
                   (aExtended ? kFlagExtended : 0) |
                   (uint32_t(aNargs) << kArgCountShift);
  JS::Value* slots = cell->fixedSlots();
  slots[kFlagsSlot] = JS::PrivateUint32Value(flags);
  slots[kNativeSlot] = JS::PrivateValue(reinterpret_cast<void*>(aNative));
  slots[kJitInfoSlot] = JS::PrivateValue(const_cast<JSJitInfo*>(aInfo));
  slots[kAtomSlot] = atom ? JS::StringValue(atom) : JS::UndefinedValue();
  AnchorCellOn(aCx, cell, holder);
  return reinterpret_cast<JSFunction*>(cell->asObject());
}

JSFunction* NewNativeFunction(JSContext* aCx, JSNative aNative,
                              unsigned aNargs, unsigned aFlags,
                              JS::HandleString aName, const JSJitInfo* aInfo) {
  return NewNativeFunctionImpl(aCx, aNative, aNargs, aFlags, aName, aInfo,
                               false, nullptr);
}

JSFunction* SelfHostedFunction(JSContext* aCx, const char* aSelfHostedName,
                               JS::HandleId aId, unsigned aNargs) {
  JS::Realm* realm = aCx->realm();
  if (realm) {
    RealmExtra& extra = RealmData(realm);
    auto found = extra.selfHosted.find(aSelfHostedName);
    if (found != extra.selfHosted.end()) {
      v8::HandleScope scope(aCx->isolate);
      return reinterpret_cast<JSFunction*>(
          ObjectFromV8(aCx, found->second.Get(aCx->isolate)));
    }
  }
  JS_ReportErrorASCII(aCx, "self-hosted function %s is not available",
                      aSelfHostedName);
  return nullptr;
}

}

using namespace vge::jsapi;

JSFunction* JS_NewFunction(JSContext* aCx, JSNative aNative, unsigned aNargs,
                           unsigned aFlags, const char* aName) {
  JS::RootedString name(aCx);
  if (aName) {
    name = JS_AtomizeString(aCx, aName);
    if (!name) {
      return nullptr;
    }
  }
  return NewNativeFunctionImpl(aCx, aNative, aNargs, aFlags, name, nullptr,
                               false, nullptr);
}

JSFunction* JS::GetSelfHostedFunction(JSContext* aCx,
                                      const char* aSelfHostedName,
                                      JS::HandleId aId, unsigned aNargs) {
  return SelfHostedFunction(aCx, aSelfHostedName, aId, aNargs);
}

JSFunction* JS::NewFunctionFromSpec(JSContext* aCx, const JSFunctionSpec* aFs,
                                    JS::HandleId aId) {
  if (aFs->selfHostedName) {
    return SelfHostedFunction(aCx, aFs->selfHostedName, aId, aFs->nargs);
  }
  JS::RootedString name(aCx);
  if (!IdToFunctionName(aCx, aId, &name)) {
    return nullptr;
  }
  const JSJitInfo* info = aFs->call.info;
  if (info && info->type() == JSJitInfo::OpType::TrampolineNative) {
    info = nullptr;
  }
  return NewNativeFunctionImpl(aCx, aFs->call.op, aFs->nargs, aFs->flags, name,
                               info, false, nullptr);
}

JSFunction* JS::NewFunctionFromSpec(JSContext* aCx, const JSFunctionSpec* aFs) {
  JS::RootedId id(aCx);
  if (!PropertyNameToId(aCx, aFs->name, &id)) {
    return nullptr;
  }
  return JS::NewFunctionFromSpec(aCx, aFs, id);
}

JSObject* JS_GetFunctionObject(JSFunction* aFun) {
  return reinterpret_cast<JSObject*>(aFun);
}

JSFunction* JS_GetObjectFunction(JSObject* aObj) {
  if (JS::GetClass(aObj)->isJSFunction()) {
    return reinterpret_cast<JSFunction*>(aObj);
  }
  return nullptr;
}

bool JS_ObjectIsFunction(JSObject* aObj) {
  return JS::GetClass(aObj)->isJSFunction();
}

bool js::IsFunctionObject(JSObject* aObj) {
  return JS::GetClass(aObj)->isJSFunction();
}

bool JS_IsNativeFunction(JSObject* aFunObj, JSNative aCall) {
  if (!IsNativeCell(aFunObj)) {
    return false;
  }
  return Slots(aFunObj)[kNativeSlot].toPrivate() ==
         reinterpret_cast<void*>(aCall);
}

uint16_t JS_GetFunctionArity(JSFunction* aFun) {
  JSObject* obj = reinterpret_cast<JSObject*>(aFun);
  if (IsNativeCell(obj)) {
    return uint16_t(FlagsAndArgs(obj) >> kArgCountShift);
  }
  JSContext* cx = CurrentContext();
  AutoV8 v8(cx);
  v8::Local<v8::Value> length;
  v8::TryCatch tryCatch(cx->isolate);
  if (!V8Function(cx, obj)
           ->GetRealNamedProperty(v8.context(),
                                  v8::String::NewFromUtf8Literal(cx->isolate,
                                                                 "length"))
           .ToLocal(&length) ||
      !length->IsNumber()) {
    return 0;
  }
  return uint16_t(length.As<v8::Number>()->Value());
}

bool JS_GetFunctionLength(JSContext* aCx, JS::HandleFunction aFun,
                          uint16_t* aLength) {
  JSObject* obj = reinterpret_cast<JSObject*>(aFun.get());
  if (IsNativeCell(obj)) {
    *aLength = uint16_t(FlagsAndArgs(obj) >> kArgCountShift);
    return true;
  }
  AutoV8 v8(aCx);
  v8::TryCatch tryCatch(aCx->isolate);
  v8::Local<v8::Value> length;
  if (!V8Function(aCx, obj)
           ->Get(v8.context(),
                 v8::String::NewFromUtf8Literal(aCx->isolate, "length"))
           .ToLocal(&length)) {
    return v8.Check(tryCatch);
  }
  uint32_t value = 0;
  if (length->IsNumber()) {
    value = uint32_t(length.As<v8::Number>()->Value());
  }
  *aLength = uint16_t(value);
  return true;
}

bool JS_GetFunctionId(JSContext* aCx, JS::Handle<JSFunction*> aFun,
                      JS::MutableHandle<JSString*> aName) {
  aName.set(JS_GetMaybePartialFunctionId(aFun));
  return true;
}

JSString* JS_GetMaybePartialFunctionId(JSFunction* aFun) {
  JSObject* obj = reinterpret_cast<JSObject*>(aFun);
  if (IsNativeCell(obj)) {
    const JS::Value& atom = Slots(obj)[kAtomSlot];
    return atom.isString() ? atom.toString() : nullptr;
  }
  JSContext* cx = CurrentContext();
  v8::HandleScope scope(cx->isolate);
  return NameFromV8(cx, V8Function(cx, obj)->GetName());
}

bool JS_GetFunctionDisplayId(JSContext* aCx, JS::Handle<JSFunction*> aFun,
                             JS::MutableHandle<JSString*> aName) {
  aName.set(JS_GetMaybePartialFunctionDisplayId(aFun));
  return true;
}

JSString* JS_GetMaybePartialFunctionDisplayId(JSFunction* aFun) {
  JSObject* obj = reinterpret_cast<JSObject*>(aFun);
  if (IsNativeCell(obj)) {
    const JS::Value& atom = Slots(obj)[kAtomSlot];
    return atom.isString() ? atom.toString() : nullptr;
  }
  JSContext* cx = CurrentContext();
  v8::HandleScope scope(cx->isolate);
  return NameFromV8(cx, V8Function(cx, obj)->GetDebugName());
}

bool JS_IsBuiltinEvalFunction(JSFunction* aFun) {
  return IsBuiltinOf(reinterpret_cast<JSObject*>(aFun), &RealmExtra::eval);
}

bool JS_IsBuiltinFunctionConstructor(JSFunction* aFun) {
  return IsBuiltinOf(reinterpret_cast<JSObject*>(aFun),
                     &RealmExtra::functionCtor);
}

JSFunction* JS_ValueToFunction(JSContext* aCx, JS::HandleValue aValue) {
  if (aValue.isObject() && JS::GetClass(&aValue.toObject())->isJSFunction()) {
    return reinterpret_cast<JSFunction*>(&aValue.toObject());
  }
  js::ReportIsNotFunction(aCx, aValue);
  return nullptr;
}

bool js::ReportIsNotFunction(JSContext* aCx, JS::HandleValue aValue) {
  JS::RootedString source(aCx, JS_ValueToSource(aCx, aValue));
  if (!source) {
    return false;
  }
  JS::UniqueChars bytes = JS_EncodeStringToUTF8(aCx, source);
  if (!bytes) {
    return false;
  }
  return ReportJSError(aCx, JSMSG_NOT_FUNCTION, bytes.get());
}

bool js::ForwardToNative(JSContext* aCx, JSNative aNative,
                         const JS::CallArgs& aArgs) {
  return aNative(aCx, aArgs.length(), aArgs.base());
}

JSFunction* js::DefineFunctionWithReserved(JSContext* aCx, JSObject* aObjArg,
                                           const char* aName, JSNative aCall,
                                           unsigned aNargs, unsigned aAttrs) {
  JS::RootedObject obj(aCx, aObjArg);
  JS::RootedString name(aCx, JS_AtomizeString(aCx, aName));
  if (!name) {
    return nullptr;
  }
  JS::RootedId id(aCx, JS::PropertyKey::NonIntAtom(name));
  JS::RootedObject fun(aCx, reinterpret_cast<JSObject*>(NewNativeFunctionImpl(
                                aCx, aCall, aNargs, aAttrs, name, nullptr, true,
                                nullptr)));
  if (!fun) {
    return nullptr;
  }
  if (!JS_DefinePropertyById(aCx, obj, id, fun, aAttrs & ~JSFUN_FLAGS_MASK)) {
    return nullptr;
  }
  return reinterpret_cast<JSFunction*>(fun.get());
}

JSFunction* js::NewFunctionWithReserved(JSContext* aCx, JSNative aNative,
                                        unsigned aNargs, unsigned aFlags,
                                        const char* aName) {
  JS::RootedString name(aCx);
  if (aName) {
    name = JS_AtomizeString(aCx, aName);
    if (!name) {
      return nullptr;
    }
  }
  return NewNativeFunctionImpl(aCx, aNative, aNargs, aFlags, name, nullptr,
                               true, nullptr);
}

JSFunction* js::NewFunctionByIdWithReserved(JSContext* aCx, JSNative aNative,
                                            unsigned aNargs, unsigned aFlags,
                                            jsid aId) {
  JS::RootedString name(aCx, aId.isString() ? aId.toString() : nullptr);
  return NewNativeFunctionImpl(aCx, aNative, aNargs, aFlags, name, nullptr,
                               true, nullptr);
}

JSFunction* js::NewFunctionByIdWithReservedAndProto(
    JSContext* aCx, JSNative aNative, JS::HandleObject aProto, unsigned aNargs,
    unsigned aFlags, jsid aId) {
  JS::RootedString name(aCx, aId.isString() ? aId.toString() : nullptr);
  return NewNativeFunctionImpl(aCx, aNative, aNargs, aFlags, name, nullptr,
                               true, aProto);
}

const JS::Value& js::GetFunctionNativeReserved(JSObject* aFun, size_t aWhich) {
  return Slots(aFun)[kFirstExtendedSlot + aWhich];
}

void js::SetFunctionNativeReserved(JSObject* aFun, size_t aWhich,
                                   const JS::Value& aVal) {
  Slots(aFun)[kFirstExtendedSlot + aWhich] = aVal;
  SlotWriteBarrier(CurrentContext(), aVal);
}

bool js::FunctionHasNativeReserved(JSObject* aFun) {
  return JS::GetClass(aFun) == &FunctionExtendedClass;
}

JSScript* JS_GetFunctionScript(JSContext* aCx, JS::HandleFunction aFun) {
  JSObject* obj = reinterpret_cast<JSObject*>(aFun.get());
  if (IsNativeCell(obj)) {
    return nullptr;
  }
  v8::HandleScope scope(aCx->isolate);
  return ScriptForFunction(aCx, V8Function(aCx, obj));
}

bool JS::PropertySpecNameToPermanentId(JSContext* aCx,
                                       JSPropertySpec::Name aName,
                                       jsid* aIdp) {
  JS::MutableHandleId id = JS::MutableHandleId::fromMarkedLocation(aIdp);
  return PropertyNameToId(aCx, aName, id);
}

bool JS::PropertySpecNameEqualsId(JSPropertySpec::Name aName,
                                  JS::HandleId aId) {
  if (aName.isSymbol()) {
    if (!aId.isSymbol()) {
      return false;
    }
    SymbolCell* cell = SymbolCell::from(aId.toSymbol());
    if (cell->code == uint32_t(aName.symbol())) {
      return true;
    }
    if (cell->code != uint32_t(JS::SymbolCode::WellKnownAPILimit)) {
      return false;
    }
    JSContext* cx = CurrentContext();
    JS::Symbol* wellKnown = WellKnownSymbol(cx, aName.symbol());
    return wellKnown == aId.toSymbol();
  }
  return IdIsAsciiName(aId, aName.string());
}
