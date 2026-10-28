#include "VgeJsObjectsInternal.h"

#include <atomic>
#include <cmath>
#include <ostream>
#include <vector>

#include "js/BigInt.h"
#include "js/CallArgs.h"
#include "js/Date.h"
#include "js/ForOfIterator.h"
#include "js/Iterator.h"
#include "js/MapAndSet.h"
#include "js/PropertyAndElement.h"
#include "js/RealmOptions.h"
#include "js/RegExp.h"
#include "js/RegExpFlags.h"
#include "js/WeakMap.h"
#include "js/WeakMapPtr.h"
#include "js/Wrapper.h"
#include "js/experimental/Intl.h"
#include "js/friend/ErrorMessages.h"

using namespace vge::jsapi;
using namespace vge::jsapi::objects;

namespace vge::jsapi::objects {

namespace {

struct WellKnownCache {
  JSRuntime* runtime = nullptr;
  v8::Isolate* isolate = nullptr;
  JS::Symbol* symbols[JS::WellKnownSymbolLimit] = {};
};

thread_local WellKnownCache tWellKnown;

v8::Local<v8::Symbol> SymbolFromConstructor(JSContext* aCx, const char* aName,
                                            const char* aDescription) {
  v8::Isolate* isolate = aCx->isolate;
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  if (ctx.IsEmpty()) {
    ctx = aCx->v8Context();
  }
  if (!ctx.IsEmpty()) {
    v8::Context::Scope scope(ctx);
    v8::TryCatch tc(isolate);
    v8::Local<v8::Value> symbolCtor =
        Intrinsic(aCx, IntrinsicId::SymbolConstructor);
    v8::Local<v8::Value> sym;
    if (symbolCtor->IsObject() &&
        symbolCtor.As<v8::Object>()
            ->Get(ctx, v8::String::NewFromUtf8(isolate, aName).ToLocalChecked())
            .ToLocal(&sym) &&
        sym->IsSymbol()) {
      return sym.As<v8::Symbol>();
    }
  }
  return v8::Symbol::New(
      isolate, v8::String::NewFromUtf8(isolate, aDescription).ToLocalChecked());
}

}

JS::Symbol* WellKnownSymbol(JSContext* aCx, JS::SymbolCode aWhich) {
  size_t index = size_t(aWhich);
  MOZ_RELEASE_ASSERT(index < JS::WellKnownSymbolLimit);
  if (tWellKnown.runtime != aCx->runtime ||
      tWellKnown.isolate != aCx->isolate) {
    tWellKnown = WellKnownCache();
    tWellKnown.runtime = aCx->runtime;
    tWellKnown.isolate = aCx->isolate;
  }
  if (JS::Symbol* cached = tWellKnown.symbols[index]) {
    return cached;
  }
  v8::Isolate* isolate = aCx->isolate;
  v8::HandleScope scope(isolate);
  v8::Local<v8::Symbol> sym;
  switch (aWhich) {
    case JS::SymbolCode::isConcatSpreadable:
      sym = v8::Symbol::GetIsConcatSpreadable(isolate);
      break;
    case JS::SymbolCode::iterator:
      sym = v8::Symbol::GetIterator(isolate);
      break;
    case JS::SymbolCode::match:
      sym = v8::Symbol::GetMatch(isolate);
      break;
    case JS::SymbolCode::replace:
      sym = v8::Symbol::GetReplace(isolate);
      break;
    case JS::SymbolCode::search:
      sym = v8::Symbol::GetSearch(isolate);
      break;
    case JS::SymbolCode::species:
      sym = SymbolFromConstructor(aCx, "species", "Symbol.species");
      break;
    case JS::SymbolCode::hasInstance:
      sym = v8::Symbol::GetHasInstance(isolate);
      break;
    case JS::SymbolCode::split:
      sym = v8::Symbol::GetSplit(isolate);
      break;
    case JS::SymbolCode::toPrimitive:
      sym = v8::Symbol::GetToPrimitive(isolate);
      break;
    case JS::SymbolCode::toStringTag:
      sym = v8::Symbol::GetToStringTag(isolate);
      break;
    case JS::SymbolCode::unscopables:
      sym = v8::Symbol::GetUnscopables(isolate);
      break;
    case JS::SymbolCode::asyncIterator:
      sym = v8::Symbol::GetAsyncIterator(isolate);
      break;
    case JS::SymbolCode::matchAll:
      sym = SymbolFromConstructor(aCx, "matchAll", "Symbol.matchAll");
      break;
    case JS::SymbolCode::dispose:
      sym = SymbolFromConstructor(aCx, "dispose", "Symbol.dispose");
      break;
    case JS::SymbolCode::asyncDispose:
      sym = SymbolFromConstructor(aCx, "asyncDispose", "Symbol.asyncDispose");
      break;
    default:
      MOZ_CRASH("vge jsapi: unknown well-known symbol");
  }
  JS::Symbol* result = SymbolFromV8(aCx, sym);
  SymbolCell::from(result)->code = uint32_t(aWhich);
  tWellKnown.symbols[index] = result;
  return result;
}

}

namespace {

v8::Local<v8::Value> CallIntrinsic(JSContext* aCx, IntrinsicId aId,
                                   v8::Local<v8::Value> aThis, int aArgc,
                                   v8::Local<v8::Value>* aArgv) {
  v8::Local<v8::Context> ctx = aCx->isolate->GetCurrentContext();
  v8::Local<v8::Value> fn = Intrinsic(aCx, aId);
  if (!fn->IsFunction()) {
    aCx->isolate->ThrowException(v8::Exception::TypeError(
        v8::String::NewFromUtf8Literal(aCx->isolate,
                                       "intrinsic is not available")));
    return v8::Local<v8::Value>();
  }
  v8::Local<v8::Value> out;
  if (!fn.As<v8::Function>()->Call(ctx, aThis, aArgc, aArgv).ToLocal(&out)) {
    return v8::Local<v8::Value>();
  }
  return out;
}

template <typename Body>
bool WithTable(JSContext* cx, JS::HandleObject obj, bool aMap, Body aBody) {
  bool direct;
  {
    v8::HandleScope scope(cx->isolate);
    v8::Local<v8::Object> o = ToV8(cx, obj.get());
    direct = aMap ? o->IsMap() : o->IsSet();
  }
  if (direct) {
    return aBody(obj.get(), false);
  }
  JS::Rooted<JSObject*> unwrapped(cx, js::UncheckedUnwrap(obj));
  {
    v8::HandleScope scope(cx->isolate);
    v8::Local<v8::Object> o = ToV8(cx, unwrapped.get());
    MOZ_RELEASE_ASSERT(aMap ? o->IsMap() : o->IsSet());
  }
  JSAutoRealm ar(cx, unwrapped);
  return aBody(unwrapped.get(), true);
}

bool WrapIn(JSContext* cx, bool aWrapped, JS::MutableHandleValue aValue) {
  return !aWrapped || JS_WrapValue(cx, aValue);
}

bool TableIterator(JSContext* cx, JS::HandleObject obj, bool aMap,
                   IntrinsicId aId, JS::MutableHandleValue rval) {
  bool wrapped = false;
  bool ok = WithTable(cx, obj, aMap, [&](JSObject* aTable, bool aWrapped) {
    wrapped = aWrapped;
    AutoV8 v8(cx);
    v8::TryCatch tc(v8.isolate());
    v8::Local<v8::Value> out =
        CallIntrinsic(cx, aId, ToV8(cx, aTable), 0, nullptr);
    if (out.IsEmpty()) {
      return v8.Check(tc);
    }
    rval.set(ValueFromV8(cx, out));
    return true;
  });
  return ok && WrapIn(cx, wrapped, rval);
}

bool TableForEach(JSContext* cx, JS::HandleObject obj, bool aMap,
                  IntrinsicId aId, JS::HandleValue callbackFn,
                  JS::HandleValue thisVal) {
  return WithTable(cx, obj, aMap, [&](JSObject* aTable, bool aWrapped) {
    JS::Rooted<JS::Value> callback(cx, callbackFn);
    JS::Rooted<JS::Value> thisArg(cx, thisVal);
    if (!WrapIn(cx, aWrapped, &callback) || !WrapIn(cx, aWrapped, &thisArg)) {
      return false;
    }
    AutoV8 v8(cx);
    v8::TryCatch tc(v8.isolate());
    v8::Local<v8::Value> args[] = {ToV8(cx, callback.get()),
                                   ToV8(cx, thisArg.get())};
    if (CallIntrinsic(cx, aId, ToV8(cx, aTable), 2, args).IsEmpty()) {
      return v8.Check(tc);
    }
    return true;
  });
}

bool TableKeyOp(JSContext* cx, JS::HandleObject obj, bool aMap,
                JS::HandleValue key, bool* rval,
                v8::Maybe<bool> (*aOp)(v8::Local<v8::Object>,
                                       v8::Local<v8::Context>,
                                       v8::Local<v8::Value>)) {
  return WithTable(cx, obj, aMap, [&](JSObject* aTable, bool aWrapped) {
    JS::Rooted<JS::Value> wrappedKey(cx, key);
    if (!WrapIn(cx, aWrapped, &wrappedKey)) {
      return false;
    }
    AutoV8 v8(cx);
    v8::TryCatch tc(v8.isolate());
    v8::Maybe<bool> res =
        aOp(ToV8(cx, aTable), v8.context(), ToV8(cx, wrappedKey.get()));
    if (res.IsNothing()) {
      return v8.Check(tc);
    }
    *rval = res.FromJust();
    return true;
  });
}

std::atomic<JS::ReduceMicrosecondTimePrecisionCallback>
    sReduceMicrosecondTimePrecisionCallback{nullptr};

constexpr double msPerDay = 86400000.0;

double PositiveModuloD(double a, double b) {
  double r = std::fmod(a, b);
  if (r < 0) {
    r += b;
  }
  return r + 0.0;
}

double ToIntegerD(double d) {
  if (!std::isfinite(d)) {
    return std::isnan(d) ? 0 : d;
  }
  return std::trunc(d) + 0.0;
}

bool IsLeapYearD(double year) {
  return std::fmod(year, 4) == 0 &&
         (std::fmod(year, 100) != 0 || std::fmod(year, 400) == 0);
}

double DayFromYearD(double y) {
  return 365 * (y - 1970) + std::floor((y - 1969) / 4.0) -
         std::floor((y - 1901) / 100.0) + std::floor((y - 1601) / 400.0);
}

constexpr int kFirstDayOfMonth[2][13] = {
    {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365},
    {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366}};

double MakeDayD(double year, double month, double date) {
  if (!std::isfinite(year) || !std::isfinite(month) || !std::isfinite(date)) {
    return JS::GenericNaN();
  }
  double y = ToIntegerD(year);
  double m = ToIntegerD(month);
  double dt = ToIntegerD(date);
  double ym = y + std::floor(m / 12);
  if (!std::isfinite(ym)) {
    return JS::GenericNaN();
  }
  int mn = int(PositiveModuloD(m, 12));
  bool leap = IsLeapYearD(ym);
  double yearday = std::floor(DayFromYearD(ym) * msPerDay / msPerDay);
  double monthday = kFirstDayOfMonth[leap][mn];
  return yearday + monthday + dt - 1;
}

double MakeDateD(double day, double time) {
  if (!std::isfinite(day) || !std::isfinite(time)) {
    return JS::GenericNaN();
  }
  return day * msPerDay + time;
}

struct YearMonthDay {
  int32_t year;
  int32_t month;
  int32_t day;
};

YearMonthDay ToYearMonthDay(int64_t time) {
  constexpr uint32_t cycleInYears = 400;
  constexpr uint32_t cycleInDays = 146097;
  constexpr uint32_t rataDie1970Jan1 = 719468;
  constexpr uint32_t maxU32 = std::numeric_limits<uint32_t>::max();
  constexpr uint32_t s = 3670;
  constexpr uint32_t K = rataDie1970Jan1 + s * cycleInDays;
  constexpr uint32_t L = s * cycleInYears;
  constexpr int32_t minDays = -int32_t(K);
  constexpr int32_t maxDays = (maxU32 - 3) / 4 - K;
  static_assert(minDays == -536'895'458);
  static_assert(maxDays == 536'846'365);
  constexpr int64_t minTime = minDays * int64_t(msPerDay);
  const uint64_t u = uint64_t(time - minTime);
  const int32_t N_U = int32_t(u / uint64_t(msPerDay)) + minDays;
  const uint32_t N = uint32_t(N_U) + K;
  const uint32_t N_1 = 4 * N + 3;
  const uint32_t C = N_1 / 146097;
  const uint32_t N_C = N_1 % 146097 / 4;
  const uint32_t N_2 = 4 * N_C + 3;
  const uint64_t P_2 = uint64_t(2939745) * N_2;
  const uint32_t Z = uint32_t(P_2 / 4294967296);
  const uint32_t N_Y = uint32_t(P_2 % 4294967296) / 2939745 / 4;
  const uint32_t Y = 100 * C + Z;
  const uint32_t N_3 = 2141 * N_Y + 132377;
  const uint32_t M = N_3 / 65536;
  const uint32_t D = N_3 % 65536 / 2141;
  constexpr uint32_t daysFromMar01ToJan01 = 306;
  const uint32_t J = N_Y >= daysFromMar01ToJan01;
  const int32_t Y_G = int32_t((Y - L) + J);
  const int32_t M_G = int32_t(J ? M - 12 : M);
  const int32_t D_G = int32_t(D + 1);
  return {Y_G, M_G, D_G};
}

bool ClippedTimeValue(double aTime, int64_t* aOut) {
  JS::ClippedTime clipped = JS::TimeClip(aTime);
  if (!clipped.isValid()) {
    return false;
  }
  *aOut = int64_t(clipped.toDouble());
  return true;
}

template <typename CharT>
bool ParseISOStyleDate(const CharT* s, size_t length) {
  size_t index = 0;
  auto isDigit = [](CharT c) { return c >= '0' && c <= '9'; };
  auto match = [&](char ch) {
    if (index < length && s[index] == CharT(ch)) {
      ++index;
      return true;
    }
    return false;
  };
  auto matchSign = [&](int32_t* sign) {
    if (match('+')) {
      *sign = 1;
      return true;
    }
    if (match('-')) {
      *sign = -1;
      return true;
    }
    return false;
  };
  auto parseDigits = [&](size_t n, int32_t* result) {
    if (index + n > length) {
      return false;
    }
    int32_t acc = 0;
    for (size_t i = 0; i < n; i++) {
      if (!isDigit(s[index + i])) {
        return false;
      }
      acc = acc * 10 + int32_t(s[index + i] - '0');
    }
    *result = acc;
    index += n;
    return true;
  };
  auto parseFractional = [&](int32_t* result) {
    size_t i = 0;
    int32_t acc = 0;
    for (; i < 3 && index + i < length && isDigit(s[index + i]); i++) {
      acc += int32_t(s[index + i] - '0') * (i == 0 ? 100 : i == 1 ? 10 : 1);
    }
    for (; index + i < length && isDigit(s[index + i]); i++) {
    }
    if (!i) {
      return false;
    }
    *result = acc;
    index += i;
    return true;
  };
  int32_t yearSign = 1;
  int32_t year = 0;
  int32_t month = 1;
  int32_t day = 1;
  int32_t hour = 0;
  int32_t min = 0;
  int32_t sec = 0;
  int32_t msec = 0;
  int32_t tzSign = 1;
  int32_t tzHour = 0;
  int32_t tzMin = 0;
  if (matchSign(&yearSign)) {
    if (!parseDigits(6, &year)) {
      return false;
    }
    if (year == 0 && yearSign == -1) {
      return false;
    }
  } else if (!parseDigits(4, &year)) {
    return false;
  }
  if (match('-')) {
    if (!parseDigits(2, &month)) {
      return false;
    }
    if (match('-')) {
      if (!parseDigits(2, &day)) {
        return false;
      }
    }
  }
  if (match('T')) {
    if (!parseDigits(2, &hour) || !match(':') || !parseDigits(2, &min)) {
      return false;
    }
    if (match(':')) {
      if (!parseDigits(2, &sec)) {
        return false;
      }
      if (match('.')) {
        if (!parseFractional(&msec)) {
          return false;
        }
      }
    }
    if (match('Z')) {
    } else if (matchSign(&tzSign)) {
      if (!parseDigits(2, &tzHour)) {
        return false;
      }
      match(':');
      if (!parseDigits(2, &tzMin)) {
        return false;
      }
    }
  }
  if (month == 0 || month > 12 || day == 0 || day > 31 || hour > 24 ||
      (hour == 24 && (min > 0 || sec > 0 || msec > 0)) || min > 59 ||
      sec > 59 || tzHour > 23 || tzMin > 59) {
    return false;
  }
  return index == length;
}

template <class RefCountedString>
RefCountedString* CopyStringZ(const char* aStr) {
  const size_t size = strlen(aStr) + 1;
  char* memory = js_pod_malloc<char>(sizeof(RefCountedString) + size);
  MOZ_RELEASE_ASSERT(memory, "CopyStringZ");
  char* strPtr = memory + sizeof(RefCountedString);
  memcpy(strPtr, aStr, size);
  return new (memory) RefCountedString(strPtr);
}

v8::RegExp::Flags ToV8Flags(JS::RegExpFlags aFlags) {
  int flags = v8::RegExp::kNone;
  if (aFlags.global()) {
    flags |= v8::RegExp::kGlobal;
  }
  if (aFlags.ignoreCase()) {
    flags |= v8::RegExp::kIgnoreCase;
  }
  if (aFlags.multiline()) {
    flags |= v8::RegExp::kMultiline;
  }
  if (aFlags.sticky()) {
    flags |= v8::RegExp::kSticky;
  }
  if (aFlags.unicode()) {
    flags |= v8::RegExp::kUnicode;
  }
  if (aFlags.dotAll()) {
    flags |= v8::RegExp::kDotAll;
  }
  if (aFlags.hasIndices()) {
    flags |= v8::RegExp::kHasIndices;
  }
  if (aFlags.unicodeSets()) {
    flags |= v8::RegExp::kUnicodeSets;
  }
  return v8::RegExp::Flags(flags);
}

struct WeakMapPtrImpl {
  v8::Global<v8::Object> map;
  v8::Global<v8::Function> get;
  v8::Global<v8::Function> set;
  JSObject* mapCell = nullptr;
  JSObject* getCell = nullptr;
  JSObject* setCell = nullptr;
};

v8::Local<v8::Context> FunctionContext(v8::Isolate* aIsolate,
                                       v8::Local<v8::Function> aFn) {
  v8::Local<v8::Context> ctx;
  if (!aFn->GetCreationContext().ToLocal(&ctx)) {
    ctx = aIsolate->GetCurrentContext();
  }
  return ctx;
}

v8::Local<v8::Object> IterResult(v8::Isolate* aIsolate,
                                 v8::Local<v8::Context> aCtx,
                                 v8::Local<v8::Value> aValue, bool aDone) {
  v8::Local<v8::Object> result = v8::Object::New(aIsolate);
  result
      ->CreateDataProperty(aCtx,
                           v8::String::NewFromUtf8Literal(aIsolate, "value"),
                           aValue)
      .FromMaybe(false);
  result
      ->CreateDataProperty(aCtx,
                           v8::String::NewFromUtf8Literal(aIsolate, "done"),
                           v8::Boolean::New(aIsolate, aDone))
      .FromMaybe(false);
  return result;
}

enum class AsyncFromSyncKind { Next, Return, Throw };

void RejectWith(v8::Local<v8::Context> aCtx,
                v8::Local<v8::Promise::Resolver> aResolver,
                v8::Local<v8::Value> aReason) {
  aResolver->Reject(aCtx, aReason).FromMaybe(false);
}

void AsyncFromSyncContinuation(v8::Isolate* aIsolate,
                               v8::Local<v8::Context> aCtx,
                               v8::Local<v8::Value> aResult,
                               v8::Local<v8::Promise::Resolver> aResolver) {
  v8::TryCatch tc(aIsolate);
  if (!aResult->IsObject()) {
    RejectWith(aCtx, aResolver,
               v8::Exception::TypeError(v8::String::NewFromUtf8Literal(
                   aIsolate, "iterator result is not an object")));
    return;
  }
  v8::Local<v8::Object> result = aResult.As<v8::Object>();
  v8::Local<v8::Value> doneValue;
  v8::Local<v8::Value> value;
  if (!result->Get(aCtx, v8::String::NewFromUtf8Literal(aIsolate, "done"))
           .ToLocal(&doneValue) ||
      !result->Get(aCtx, v8::String::NewFromUtf8Literal(aIsolate, "value"))
           .ToLocal(&value)) {
    RejectWith(aCtx, aResolver, tc.Exception());
    return;
  }
  bool done = doneValue->BooleanValue(aIsolate);
  v8::Local<v8::Promise> wrapper;
  if (value->IsPromise()) {
    wrapper = value.As<v8::Promise>();
  } else {
    v8::Local<v8::Promise::Resolver> valueResolver;
    if (!v8::Promise::Resolver::New(aCtx).ToLocal(&valueResolver) ||
        valueResolver->Resolve(aCtx, value).IsNothing()) {
      RejectWith(aCtx, aResolver, tc.Exception());
      return;
    }
    wrapper = valueResolver->GetPromise();
  }
  v8::Local<v8::Function> onFulfilled;
  if (!v8::Function::New(
           aCtx,
           [](const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
             v8::Isolate* isolate = aInfo.GetIsolate();
             v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
             aInfo.GetReturnValue().Set(
                 IterResult(isolate, ctx, aInfo[0], aInfo.Data()->IsTrue()));
           },
           v8::Boolean::New(aIsolate, done), 1)
           .ToLocal(&onFulfilled)) {
    RejectWith(aCtx, aResolver, tc.Exception());
    return;
  }
  v8::Local<v8::Promise> chained;
  if (!wrapper->Then(aCtx, onFulfilled).ToLocal(&chained)) {
    RejectWith(aCtx, aResolver, tc.Exception());
    return;
  }
  aResolver->Resolve(aCtx, chained).FromMaybe(false);
}

void AsyncFromSyncMethod(const v8::FunctionCallbackInfo<v8::Value>& aInfo,
                         AsyncFromSyncKind aKind) {
  v8::Isolate* isolate = aInfo.GetIsolate();
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  v8::Local<v8::Array> state = aInfo.Data().As<v8::Array>();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(ctx).ToLocal(&resolver)) {
    return;
  }
  aInfo.GetReturnValue().Set(resolver->GetPromise());
  v8::TryCatch tc(isolate);
  v8::Local<v8::Value> syncIterator;
  v8::Local<v8::Value> nextMethod;
  if (!state->Get(ctx, 0).ToLocal(&syncIterator) ||
      !state->Get(ctx, 1).ToLocal(&nextMethod)) {
    RejectWith(ctx, resolver, tc.Exception());
    return;
  }
  v8::Local<v8::Value> value =
      aInfo.Length() > 0 ? aInfo[0] : v8::Undefined(isolate).As<v8::Value>();
  v8::Local<v8::Value> method;
  if (aKind == AsyncFromSyncKind::Next) {
    method = nextMethod;
  } else {
    const char* name = aKind == AsyncFromSyncKind::Return ? "return" : "throw";
    if (!syncIterator.As<v8::Object>()
             ->Get(ctx, v8::String::NewFromUtf8(isolate, name).ToLocalChecked())
             .ToLocal(&method)) {
      RejectWith(ctx, resolver, tc.Exception());
      return;
    }
    if (method->IsNullOrUndefined()) {
      if (aKind == AsyncFromSyncKind::Return) {
        resolver->Resolve(ctx, IterResult(isolate, ctx, value, true))
            .FromMaybe(false);
      } else {
        RejectWith(ctx, resolver, value);
      }
      return;
    }
  }
  if (!method->IsFunction()) {
    RejectWith(ctx, resolver,
               v8::Exception::TypeError(v8::String::NewFromUtf8Literal(
                   isolate, "iterator method is not a function")));
    return;
  }
  int argc = (aKind == AsyncFromSyncKind::Next && aInfo.Length() == 0) ? 0 : 1;
  v8::Local<v8::Value> result;
  if (!method.As<v8::Function>()->Call(ctx, syncIterator, argc, &value).ToLocal(
          &result)) {
    if (tc.HasTerminated()) {
      tc.ReThrow();
      return;
    }
    RejectWith(ctx, resolver, tc.Exception());
    return;
  }
  AsyncFromSyncContinuation(isolate, ctx, result, resolver);
}

v8::Local<v8::Object> CreateAsyncFromSyncIterator(
    v8::Isolate* aIsolate, v8::Local<v8::Context> aCtx,
    v8::Local<v8::Value> aSyncIterator, v8::Local<v8::Value> aNextMethod) {
  v8::Local<v8::Value> stateValues[] = {aSyncIterator, aNextMethod};
  v8::Local<v8::Array> state = v8::Array::New(aIsolate, stateValues, 2);
  v8::Local<v8::Object> iter = v8::Object::New(aIsolate);
  auto install = [&](const char* aName, v8::FunctionCallback aCallback,
                     int aLength) {
    v8::Local<v8::Function> fn;
    if (v8::Function::New(aCtx, aCallback, state, aLength).ToLocal(&fn)) {
      iter->DefineOwnProperty(aCtx,
                              v8::String::NewFromUtf8(aIsolate, aName)
                                  .ToLocalChecked(),
                              fn, v8::DontEnum)
          .FromMaybe(false);
    }
  };
  install("next",
          [](const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
            AsyncFromSyncMethod(aInfo, AsyncFromSyncKind::Next);
          },
          1);
  install("return",
          [](const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
            AsyncFromSyncMethod(aInfo, AsyncFromSyncKind::Return);
          },
          1);
  install("throw",
          [](const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
            AsyncFromSyncMethod(aInfo, AsyncFromSyncKind::Throw);
          },
          1);
  v8::Local<v8::Function> self;
  if (v8::Function::New(
          aCtx,
          [](const v8::FunctionCallbackInfo<v8::Value>& aInfo) {
            aInfo.GetReturnValue().Set(aInfo.This());
          },
          v8::Local<v8::Value>(), 0)
          .ToLocal(&self)) {
    iter->DefineOwnProperty(aCtx, v8::Symbol::GetAsyncIterator(aIsolate), self,
                            v8::DontEnum)
        .FromMaybe(false);
  }
  return iter;
}

v8::Local<v8::Object> IteratorRecord(v8::Isolate* aIsolate,
                                     v8::Local<v8::Value> aIterator,
                                     v8::Local<v8::Value> aNextMethod) {
  v8::Local<v8::Name> names[] = {
      v8::String::NewFromUtf8Literal(aIsolate, "iterator"),
      v8::String::NewFromUtf8Literal(aIsolate, "nextMethod"),
      v8::String::NewFromUtf8Literal(aIsolate, "done")};
  v8::Local<v8::Value> values[] = {aIterator, aNextMethod,
                                   v8::False(aIsolate)};
  return v8::Object::New(aIsolate, v8::Null(aIsolate), names, values, 3);
}

bool GetMethodV8(JSContext* cx, v8::Local<v8::Context> aCtx,
                 v8::Local<v8::Value> aValue, v8::Local<v8::Name> aKey,
                 v8::Local<v8::Value>* aOut, bool* aNotCallable) {
  *aNotCallable = false;
  v8::Local<v8::Object> obj;
  if (!aValue->ToObject(aCtx).ToLocal(&obj)) {
    return false;
  }
  v8::Local<v8::Value> fn;
  if (!obj->Get(aCtx, aKey).ToLocal(&fn)) {
    return false;
  }
  if (fn->IsNullOrUndefined()) {
    *aOut = v8::Undefined(cx->isolate);
    return true;
  }
  if (!fn->IsFunction() &&
      !(fn->IsObject() && fn.As<v8::Object>()->IsCallable())) {
    *aNotCallable = true;
    return true;
  }
  *aOut = fn;
  return true;
}

const char* TypeOfName(v8::Local<v8::Value> aValue) {
  if (aValue->IsNull()) {
    return "null";
  }
  if (aValue->IsUndefined()) {
    return "undefined";
  }
  if (aValue->IsString()) {
    return "string";
  }
  if (aValue->IsNumber()) {
    return "number";
  }
  if (aValue->IsBoolean()) {
    return "boolean";
  }
  if (aValue->IsSymbol()) {
    return "symbol";
  }
  if (aValue->IsBigInt()) {
    return "bigint";
  }
  if (aValue->IsFunction()) {
    return "function";
  }
  return "object";
}

bool GetNamedProperty(JSContext* cx, JS::HandleObject aObj, const char* aName,
                      JS::MutableHandleValue aOut) {
  return JS_GetProperty(cx, aObj, aName, aOut);
}

bool IntlMember(JSContext* cx, const char* aName,
                JS::MutableHandleValue aOut) {
  {
    AutoV8 v8(cx);
    v8::Isolate* isolate = v8.isolate();
    v8::Local<v8::Context> ctx = v8.context();
    v8::TryCatch tc(isolate);
    v8::Local<v8::Value> intl;
    if (!ctx->Global()
             ->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "Intl"))
             .ToLocal(&intl)) {
      return v8.Check(tc);
    }
    if (intl->IsObject()) {
      v8::Local<v8::Value> member;
      if (!intl.As<v8::Object>()
               ->Get(ctx, v8::String::NewFromUtf8(isolate, aName)
                              .ToLocalChecked())
               .ToLocal(&member)) {
        return v8.Check(tc);
      }
      if (member->IsFunction()) {
        aOut.set(ValueFromV8(cx, member));
        return true;
      }
    }
  }
  JS_ReportErrorASCII(cx, "Intl.%s is not available in this V8 build", aName);
  return false;
}

bool GetCalendarInfoNative(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
  JS::Rooted<JS::Value> dtfCtor(cx);
  JS::Rooted<JS::Value> localeCtor(cx);
  if (!IntlMember(cx, "DateTimeFormat", &dtfCtor) ||
      !IntlMember(cx, "Locale", &localeCtor)) {
    return false;
  }
  AutoV8 v8(cx);
  v8::Isolate* isolate = v8.isolate();
  v8::Local<v8::Context> ctx = v8.context();
  v8::TryCatch tc(isolate);
  auto str = [&](const char* aName) {
    return v8::String::NewFromUtf8(isolate, aName).ToLocalChecked();
  };
  v8::Local<v8::Value> locales = ToV8(cx, args.get(0).get());
  v8::Local<v8::Value> dtfValue;
  if (!ToV8(cx, &dtfCtor.toObject())
           ->CallAsConstructor(ctx, 1, &locales)
           .ToLocal(&dtfValue)) {
    return v8.Check(tc);
  }
  v8::Local<v8::Object> dtf = dtfValue.As<v8::Object>();
  v8::Local<v8::Value> resolvedOptionsFn;
  v8::Local<v8::Value> resolved;
  if (!dtf->Get(ctx, str("resolvedOptions")).ToLocal(&resolvedOptionsFn) ||
      !resolvedOptionsFn->IsFunction() ||
      !resolvedOptionsFn.As<v8::Function>()
           ->Call(ctx, dtf, 0, nullptr)
           .ToLocal(&resolved) ||
      !resolved->IsObject()) {
    return v8.Check(tc);
  }
  v8::Local<v8::Value> locale;
  v8::Local<v8::Value> calendar;
  if (!resolved.As<v8::Object>()->Get(ctx, str("locale")).ToLocal(&locale) ||
      !resolved.As<v8::Object>()->Get(ctx, str("calendar")).ToLocal(&calendar)) {
    return v8.Check(tc);
  }
  v8::Local<v8::Value> locValue;
  if (!ToV8(cx, &localeCtor.toObject())
           ->CallAsConstructor(ctx, 1, &locale)
           .ToLocal(&locValue)) {
    return v8.Check(tc);
  }
  v8::Local<v8::Object> loc = locValue.As<v8::Object>();
  v8::Local<v8::Value> weekInfo;
  v8::Local<v8::Value> getWeekInfo;
  if (!loc->Get(ctx, str("getWeekInfo")).ToLocal(&getWeekInfo)) {
    return v8.Check(tc);
  }
  if (getWeekInfo->IsFunction()) {
    if (!getWeekInfo.As<v8::Function>()->Call(ctx, loc, 0, nullptr).ToLocal(
            &weekInfo)) {
      return v8.Check(tc);
    }
  } else if (!loc->Get(ctx, str("weekInfo")).ToLocal(&weekInfo)) {
    return v8.Check(tc);
  }
  if (!weekInfo->IsObject()) {
    isolate->ThrowException(v8::Exception::TypeError(
        str("Intl.Locale week information is not available")));
    return v8.Check(tc);
  }
  v8::Local<v8::Object> week = weekInfo.As<v8::Object>();
  v8::Local<v8::Value> firstDay, minimalDays, weekend;
  if (!week->Get(ctx, str("firstDay")).ToLocal(&firstDay) ||
      !week->Get(ctx, str("minimalDays")).ToLocal(&minimalDays) ||
      !week->Get(ctx, str("weekend")).ToLocal(&weekend)) {
    return v8.Check(tc);
  }
  v8::Local<v8::Object> result = v8::Object::New(isolate);
  auto put = [&](const char* aName, v8::Local<v8::Value> aValue) {
    return result->CreateDataProperty(ctx, str(aName), aValue).FromMaybe(false);
  };
  if (!put("locale", locale) || !put("calendar", calendar) ||
      !put("firstDayOfWeek", firstDay) || !put("minDays", minimalDays) ||
      !put("weekend", weekend)) {
    return v8.Check(tc);
  }
  args.rval().set(ValueFromV8(cx, result));
  return true;
}

}

JS_PUBLIC_API JSObject* JS::NewMapObject(JSContext* cx) {
  AutoV8 v8(cx);
  return ObjectFromV8(cx, v8::Map::New(v8.isolate()));
}

JS_PUBLIC_API uint32_t JS::MapSize(JSContext* cx, JS::HandleObject obj) {
  uint32_t size = 0;
  WithTable(cx, obj, true, [&](JSObject* aTable, bool) {
    v8::HandleScope scope(cx->isolate);
    size = uint32_t(ToV8(cx, aTable).As<v8::Map>()->Size());
    return true;
  });
  return size;
}

JS_PUBLIC_API bool JS::MapGet(JSContext* cx, JS::HandleObject obj,
                              JS::HandleValue key,
                              JS::MutableHandleValue rval) {
  bool wrapped = false;
  bool ok = WithTable(cx, obj, true, [&](JSObject* aTable, bool aWrapped) {
    wrapped = aWrapped;
    JS::Rooted<JS::Value> wrappedKey(cx, key);
    if (!WrapIn(cx, aWrapped, &wrappedKey)) {
      return false;
    }
    AutoV8 v8(cx);
    v8::TryCatch tc(v8.isolate());
    v8::Local<v8::Value> out;
    if (!ToV8(cx, aTable)
             .As<v8::Map>()
             ->Get(v8.context(), ToV8(cx, wrappedKey.get()))
             .ToLocal(&out)) {
      return v8.Check(tc);
    }
    rval.set(ValueFromV8(cx, out));
    return true;
  });
  return ok && WrapIn(cx, wrapped, rval);
}

JS_PUBLIC_API bool JS::MapHas(JSContext* cx, JS::HandleObject obj,
                              JS::HandleValue key, bool* rval) {
  return TableKeyOp(cx, obj, true, key, rval,
                    [](v8::Local<v8::Object> aTable, v8::Local<v8::Context> aCtx,
                       v8::Local<v8::Value> aKey) {
                      return aTable.As<v8::Map>()->Has(aCtx, aKey);
                    });
}

JS_PUBLIC_API bool JS::MapDelete(JSContext* cx, JS::HandleObject obj,
                                 JS::HandleValue key, bool* rval) {
  return TableKeyOp(cx, obj, true, key, rval,
                    [](v8::Local<v8::Object> aTable, v8::Local<v8::Context> aCtx,
                       v8::Local<v8::Value> aKey) {
                      return aTable.As<v8::Map>()->Delete(aCtx, aKey);
                    });
}

JS_PUBLIC_API bool JS::MapSet(JSContext* cx, JS::HandleObject obj,
                              JS::HandleValue key, JS::HandleValue val) {
  return WithTable(cx, obj, true, [&](JSObject* aTable, bool aWrapped) {
    JS::Rooted<JS::Value> wrappedKey(cx, key);
    JS::Rooted<JS::Value> wrappedValue(cx, val);
    if (!WrapIn(cx, aWrapped, &wrappedKey) ||
        !WrapIn(cx, aWrapped, &wrappedValue)) {
      return false;
    }
    AutoV8 v8(cx);
    v8::TryCatch tc(v8.isolate());
    if (ToV8(cx, aTable)
            .As<v8::Map>()
            ->Set(v8.context(), ToV8(cx, wrappedKey.get()),
                  ToV8(cx, wrappedValue.get()))
            .IsEmpty()) {
      return v8.Check(tc);
    }
    return true;
  });
}

JS_PUBLIC_API bool JS::MapClear(JSContext* cx, JS::HandleObject obj) {
  return WithTable(cx, obj, true, [&](JSObject* aTable, bool) {
    v8::HandleScope scope(cx->isolate);
    ToV8(cx, aTable).As<v8::Map>()->Clear();
    return true;
  });
}

JS_PUBLIC_API bool JS::MapKeys(JSContext* cx, JS::HandleObject obj,
                               JS::MutableHandleValue rval) {
  return TableIterator(cx, obj, true, IntrinsicId::MapKeys, rval);
}

JS_PUBLIC_API bool JS::MapValues(JSContext* cx, JS::HandleObject obj,
                                 JS::MutableHandleValue rval) {
  return TableIterator(cx, obj, true, IntrinsicId::MapValues, rval);
}

JS_PUBLIC_API bool JS::MapEntries(JSContext* cx, JS::HandleObject obj,
                                  JS::MutableHandleValue rval) {
  return TableIterator(cx, obj, true, IntrinsicId::MapEntries, rval);
}

JS_PUBLIC_API bool JS::MapForEach(JSContext* cx, JS::HandleObject obj,
                                  JS::HandleValue callbackFn,
                                  JS::HandleValue thisVal) {
  return TableForEach(cx, obj, true, IntrinsicId::MapForEach, callbackFn,
                      thisVal);
}

JS_PUBLIC_API JSObject* JS::NewSetObject(JSContext* cx) {
  AutoV8 v8(cx);
  return ObjectFromV8(cx, v8::Set::New(v8.isolate()));
}

JS_PUBLIC_API uint32_t JS::SetSize(JSContext* cx, JS::HandleObject obj) {
  uint32_t size = 0;
  WithTable(cx, obj, false, [&](JSObject* aTable, bool) {
    v8::HandleScope scope(cx->isolate);
    size = uint32_t(ToV8(cx, aTable).As<v8::Set>()->Size());
    return true;
  });
  return size;
}

JS_PUBLIC_API bool JS::SetHas(JSContext* cx, JS::HandleObject obj,
                              JS::HandleValue key, bool* rval) {
  return TableKeyOp(cx, obj, false, key, rval,
                    [](v8::Local<v8::Object> aTable, v8::Local<v8::Context> aCtx,
                       v8::Local<v8::Value> aKey) {
                      return aTable.As<v8::Set>()->Has(aCtx, aKey);
                    });
}

JS_PUBLIC_API bool JS::SetDelete(JSContext* cx, JS::HandleObject obj,
                                 JS::HandleValue key, bool* rval) {
  return TableKeyOp(cx, obj, false, key, rval,
                    [](v8::Local<v8::Object> aTable, v8::Local<v8::Context> aCtx,
                       v8::Local<v8::Value> aKey) {
                      return aTable.As<v8::Set>()->Delete(aCtx, aKey);
                    });
}

JS_PUBLIC_API bool JS::SetAdd(JSContext* cx, JS::HandleObject obj,
                              JS::HandleValue key) {
  return WithTable(cx, obj, false, [&](JSObject* aTable, bool aWrapped) {
    JS::Rooted<JS::Value> wrappedKey(cx, key);
    if (!WrapIn(cx, aWrapped, &wrappedKey)) {
      return false;
    }
    AutoV8 v8(cx);
    v8::TryCatch tc(v8.isolate());
    if (ToV8(cx, aTable)
            .As<v8::Set>()
            ->Add(v8.context(), ToV8(cx, wrappedKey.get()))
            .IsEmpty()) {
      return v8.Check(tc);
    }
    return true;
  });
}

JS_PUBLIC_API bool JS::SetClear(JSContext* cx, JS::HandleObject obj) {
  return WithTable(cx, obj, false, [&](JSObject* aTable, bool) {
    v8::HandleScope scope(cx->isolate);
    ToV8(cx, aTable).As<v8::Set>()->Clear();
    return true;
  });
}

JS_PUBLIC_API bool JS::SetKeys(JSContext* cx, JS::HandleObject obj,
                               JS::MutableHandleValue rval) {
  return TableIterator(cx, obj, false, IntrinsicId::SetKeys, rval);
}

JS_PUBLIC_API bool JS::SetValues(JSContext* cx, JS::HandleObject obj,
                                 JS::MutableHandleValue rval) {
  return TableIterator(cx, obj, false, IntrinsicId::SetValues, rval);
}

JS_PUBLIC_API bool JS::SetEntries(JSContext* cx, JS::HandleObject obj,
                                  JS::MutableHandleValue rval) {
  return TableIterator(cx, obj, false, IntrinsicId::SetEntries, rval);
}

JS_PUBLIC_API bool JS::SetForEach(JSContext* cx, JS::HandleObject obj,
                                  JS::HandleValue callbackFn,
                                  JS::HandleValue thisVal) {
  return TableForEach(cx, obj, false, IntrinsicId::SetForEach, callbackFn,
                      thisVal);
}

JS_PUBLIC_API JSObject* JS::NewWeakMapObject(JSContext* cx) {
  AutoV8 v8(cx);
  v8::TryCatch tc(v8.isolate());
  v8::Local<v8::Value> ctor = Intrinsic(cx, IntrinsicId::WeakMapConstructor);
  v8::Local<v8::Object> map;
  if (!ctor->IsFunction() ||
      !ctor.As<v8::Function>()->NewInstance(v8.context()).ToLocal(&map)) {
    v8.Check(tc);
    return nullptr;
  }
  return ObjectFromV8(cx, map);
}

JS_PUBLIC_API bool JS::GetWeakMapEntry(JSContext* cx, JS::HandleObject mapObj,
                                       JS::HandleValue key,
                                       JS::MutableHandleValue val) {
  val.setUndefined();
  if (!key.isObject() && !key.isSymbol()) {
    return true;
  }
  AutoV8 v8(cx);
  v8::TryCatch tc(v8.isolate());
  v8::Local<v8::Value> arg = ToV8(cx, key.get());
  v8::Local<v8::Value> out =
      CallIntrinsic(cx, IntrinsicId::WeakMapGet, ToV8(cx, mapObj.get()), 1, &arg);
  if (out.IsEmpty()) {
    return v8.Check(tc);
  }
  val.set(ValueFromV8(cx, out));
  return true;
}

JS_PUBLIC_API bool JS::SetWeakMapEntry(JSContext* cx, JS::HandleObject mapObj,
                                       JS::HandleValue key,
                                       JS::HandleValue val) {
  AutoV8 v8(cx);
  v8::TryCatch tc(v8.isolate());
  v8::Local<v8::Value> args[] = {ToV8(cx, key.get()), ToV8(cx, val.get())};
  if (CallIntrinsic(cx, IntrinsicId::WeakMapSet, ToV8(cx, mapObj.get()), 2,
                    args)
          .IsEmpty()) {
    return v8.Check(tc);
  }
  return true;
}

template <>
bool JS::WeakMapPtr<JSObject*, JSObject*>::init(JSContext* cx) {
  AutoV8 v8(cx);
  v8::Isolate* isolate = v8.isolate();
  v8::TryCatch tc(isolate);
  v8::Local<v8::Value> ctor = Intrinsic(cx, IntrinsicId::WeakMapConstructor);
  v8::Local<v8::Value> get = Intrinsic(cx, IntrinsicId::WeakMapGet);
  v8::Local<v8::Value> set = Intrinsic(cx, IntrinsicId::WeakMapSet);
  v8::Local<v8::Object> map;
  if (!ctor->IsFunction() || !get->IsFunction() || !set->IsFunction() ||
      !ctor.As<v8::Function>()->NewInstance(v8.context()).ToLocal(&map)) {
    v8.Check(tc);
    return false;
  }
  WeakMapPtrImpl* impl = new WeakMapPtrImpl();
  impl->map.Reset(isolate, map);
  impl->get.Reset(isolate, get.As<v8::Function>());
  impl->set.Reset(isolate, set.As<v8::Function>());
  impl->map.SetWeak();
  impl->get.SetWeak();
  impl->set.SetWeak();
  impl->mapCell = ObjectFromV8(cx, map);
  impl->getCell = ObjectFromV8(cx, get.As<v8::Object>());
  impl->setCell = ObjectFromV8(cx, set.As<v8::Object>());
  ptr = impl;
  return true;
}

template <>
void JS::WeakMapPtr<JSObject*, JSObject*>::trace(JSTracer* trc) {
  WeakMapPtrImpl* impl = static_cast<WeakMapPtrImpl*>(ptr);
  if (!impl) {
    return;
  }
  JS::TraceRoot(trc, &impl->mapCell, "weakmapptr map");
  JS::TraceRoot(trc, &impl->getCell, "weakmapptr get");
  JS::TraceRoot(trc, &impl->setCell, "weakmapptr set");
}

template <>
void JS::WeakMapPtr<JSObject*, JSObject*>::destroy() {
  WeakMapPtrImpl* impl = static_cast<WeakMapPtrImpl*>(ptr);
  if (impl) {
    impl->map.Reset();
    impl->get.Reset();
    impl->set.Reset();
    delete impl;
  }
  ptr = nullptr;
}

template <>
JSObject* JS::WeakMapPtr<JSObject*, JSObject*>::lookup(JSObject* const& key) {
  WeakMapPtrImpl* impl = static_cast<WeakMapPtrImpl*>(ptr);
  JSContext* cx = CurrentContext();
  v8::Isolate* isolate = cx->isolate;
  v8::HandleScope scope(isolate);
  v8::Local<v8::Function> get = impl->get.Get(isolate);
  v8::Local<v8::Context> ctx = FunctionContext(isolate, get);
  v8::Context::Scope contextScope(ctx);
  v8::TryCatch tc(isolate);
  v8::Local<v8::Value> arg = ToV8(cx, key);
  v8::Local<v8::Value> out;
  if (!get->Call(ctx, impl->map.Get(isolate), 1, &arg).ToLocal(&out) ||
      !out->IsObject()) {
    return nullptr;
  }
  return ObjectFromV8(cx, out.As<v8::Object>());
}

template <>
bool JS::WeakMapPtr<JSObject*, JSObject*>::put(JSContext* cx,
                                               JSObject* const& key,
                                               JSObject* const& value) {
  WeakMapPtrImpl* impl = static_cast<WeakMapPtrImpl*>(ptr);
  v8::Isolate* isolate = cx->isolate;
  v8::HandleScope scope(isolate);
  v8::Local<v8::Function> set = impl->set.Get(isolate);
  v8::Local<v8::Context> ctx = FunctionContext(isolate, set);
  v8::Context::Scope contextScope(ctx);
  v8::TryCatch tc(isolate);
  v8::Local<v8::Value> args[] = {ToV8(cx, key), ToV8(cx, value)};
  if (set->Call(ctx, impl->map.Get(isolate), 2, args).IsEmpty()) {
    if (tc.HasCaught() && !tc.HasTerminated()) {
      cx->setPending(tc.Exception());
    }
    return false;
  }
  return true;
}

JS_PUBLIC_API JSObject* JS::NewDateObject(JSContext* cx, JS::ClippedTime time) {
  AutoV8 v8(cx);
  v8::TryCatch tc(v8.isolate());
  v8::Local<v8::Value> date;
  if (!v8::Date::New(v8.context(), time.toDouble()).ToLocal(&date)) {
    v8.Check(tc);
    return nullptr;
  }
  return ObjectFromV8(cx, date.As<v8::Object>());
}

JS_PUBLIC_API bool JS::ObjectIsDate(JSContext* cx, JS::Handle<JSObject*> obj,
                                    bool* isDate) {
  js::ESClass cls;
  if (!BuiltinClassOf(cx, obj, &cls)) {
    return false;
  }
  *isDate = cls == js::ESClass::Date;
  return true;
}

JS_PUBLIC_API double JS::MakeDate(double year, unsigned month, unsigned day) {
  return MakeDateD(MakeDayD(year, month, day), 0);
}

JS_PUBLIC_API double JS::MakeDate(double year, unsigned month, unsigned day,
                                  double time) {
  return MakeDateD(MakeDayD(year, month, day), time);
}

JS_PUBLIC_API double JS::YearFromTime(double time) {
  int64_t tv;
  if (!ClippedTimeValue(time, &tv)) {
    return JS::GenericNaN();
  }
  return ToYearMonthDay(tv).year;
}

JS_PUBLIC_API double JS::MonthFromTime(double time) {
  int64_t tv;
  if (!ClippedTimeValue(time, &tv)) {
    return JS::GenericNaN();
  }
  return ToYearMonthDay(tv).month;
}

JS_PUBLIC_API double JS::DayFromTime(double time) {
  int64_t tv;
  if (!ClippedTimeValue(time, &tv)) {
    return JS::GenericNaN();
  }
  return ToYearMonthDay(tv).day;
}

JS_PUBLIC_API double JS::DayFromYear(double year) { return DayFromYearD(year); }

JS_PUBLIC_API double JS::DayWithinYear(double time, double year) {
  int64_t tv;
  if (!ClippedTimeValue(time, &tv)) {
    return JS::GenericNaN();
  }
  return std::floor(double(tv) / msPerDay) - DayFromYearD(year);
}

JS_PUBLIC_API bool JS::IsISOStyleDate(JSContext* cx,
                                      const JS::Latin1Chars& str) {
  return ParseISOStyleDate(str.begin().get(), str.length());
}

JS_PUBLIC_API void JS::ResetTimeZone() {
  if (v8::Isolate* isolate = MainIsolate()) {
    isolate->DateTimeConfigurationChangeNotification(
        v8::Isolate::TimeZoneDetection::kRedetect);
  }
}

JS_PUBLIC_API void JS::SetReduceMicrosecondTimePrecisionCallback(
    JS::ReduceMicrosecondTimePrecisionCallback callback) {
  sReduceMicrosecondTimePrecisionCallback = callback;
}

JS_PUBLIC_API void JS::SetRealmReduceTimerPrecisionCallerType(
    JS::Realm* realm, JS::RTPCallerTypeToken type) {
  realm->behaviors.setReduceTimerPrecisionCallerType(type);
}

JS::RealmBehaviors& JS::RealmBehaviors::setLocaleOverride(const char* locale) {
  if (locale) {
    localeOverride_ = CopyStringZ<JS::LocaleString>(locale);
  } else {
    localeOverride_ = nullptr;
  }
  return *this;
}

JS::RealmBehaviors& JS::RealmBehaviors::setTimeZoneOverride(
    const char* timeZone) {
  if (timeZone) {
    timeZoneOverride_ = CopyStringZ<JS::TimeZoneString>(timeZone);
  } else {
    timeZoneOverride_ = nullptr;
  }
  return *this;
}

void JS::RealmBehaviors::copyOverrideStrings() {
  if (localeOverride_) {
    setLocaleOverride(localeOverride_->chars());
  }
  if (timeZoneOverride_) {
    setTimeZoneOverride(timeZoneOverride_->chars());
  }
}

std::ostream& JS::operator<<(std::ostream& os, JS::RegExpFlags flags) {
  uint8_t raw = flags.value();
  for (uint8_t i = 1; i; i = uint8_t(i << 1)) {
    if (!(raw & i)) {
      continue;
    }
    switch (raw & i) {
      case JS::RegExpFlag::HasIndices:
        os << 'd';
        break;
      case JS::RegExpFlag::Global:
        os << 'g';
        break;
      case JS::RegExpFlag::IgnoreCase:
        os << 'i';
        break;
      case JS::RegExpFlag::Multiline:
        os << 'm';
        break;
      case JS::RegExpFlag::DotAll:
        os << 's';
        break;
      case JS::RegExpFlag::Unicode:
        os << 'u';
        break;
      case JS::RegExpFlag::UnicodeSets:
        os << 'v';
        break;
      case JS::RegExpFlag::Sticky:
        os << 'y';
        break;
      default:
        os << '?';
        break;
    }
  }
  return os;
}

JS_PUBLIC_API JSObject* JS::NewUCRegExpObject(JSContext* cx,
                                              const char16_t* chars,
                                              size_t length,
                                              JS::RegExpFlags flags) {
  AutoV8 v8(cx);
  v8::TryCatch tc(v8.isolate());
  v8::Local<v8::String> pattern;
  if (!v8::String::NewFromTwoByte(v8.isolate(),
                                  reinterpret_cast<const uint16_t*>(chars),
                                  v8::NewStringType::kNormal, int(length))
           .ToLocal(&pattern)) {
    v8.Check(tc);
    return nullptr;
  }
  v8::Local<v8::RegExp> re;
  if (!v8::RegExp::New(v8.context(), pattern, ToV8Flags(flags)).ToLocal(&re)) {
    v8.Check(tc);
    return nullptr;
  }
  return ObjectFromV8(cx, re);
}

JS_PUBLIC_API bool JS::CheckRegExpSyntax(JSContext* cx, const char16_t* chars,
                                         size_t length, JS::RegExpFlags flags,
                                         JS::MutableHandleValue error) {
  AutoV8 v8(cx);
  v8::TryCatch tc(v8.isolate());
  error.setUndefined();
  v8::Local<v8::String> pattern;
  if (!v8::String::NewFromTwoByte(v8.isolate(),
                                  reinterpret_cast<const uint16_t*>(chars),
                                  v8::NewStringType::kNormal, int(length))
           .ToLocal(&pattern)) {
    return v8.Check(tc);
  }
  v8::Local<v8::RegExp> re;
  if (!v8::RegExp::New(v8.context(), pattern, ToV8Flags(flags)).ToLocal(&re)) {
    if (!tc.HasCaught() || tc.HasTerminated()) {
      return false;
    }
    v8::Local<v8::Value> exception = tc.Exception();
    if (!exception->IsNativeError()) {
      return v8.Check(tc);
    }
    error.set(ValueFromV8(cx, exception));
  }
  return true;
}

JS_PUBLIC_API bool JS::ExecuteRegExpNoStatics(JSContext* cx,
                                              JS::HandleObject obj,
                                              const char16_t* chars,
                                              size_t length, size_t* indexp,
                                              bool test,
                                              JS::MutableHandleValue rval) {
  AutoV8 v8(cx);
  v8::Isolate* isolate = v8.isolate();
  v8::Local<v8::Context> ctx = v8.context();
  v8::TryCatch tc(isolate);
  v8::Local<v8::RegExp> source = ToV8(cx, obj.get()).As<v8::RegExp>();
  v8::Local<v8::String> input;
  if (!v8::String::NewFromTwoByte(isolate,
                                  reinterpret_cast<const uint16_t*>(chars),
                                  v8::NewStringType::kNormal, int(length))
           .ToLocal(&input)) {
    return v8.Check(tc);
  }
  v8::RegExp::Flags flags =
      v8::RegExp::Flags(source->GetFlags() | v8::RegExp::kGlobal);
  v8::Local<v8::RegExp> re;
  if (!v8::RegExp::New(ctx, source->GetSource(), flags).ToLocal(&re)) {
    return v8.Check(tc);
  }
  v8::Local<v8::String> lastIndexKey =
      v8::String::NewFromUtf8Literal(isolate, "lastIndex");
  if (re->Set(ctx, lastIndexKey, v8::Number::New(isolate, double(*indexp)))
          .IsNothing()) {
    return v8.Check(tc);
  }
  v8::Local<v8::Object> match;
  if (!re->Exec(ctx, input).ToLocal(&match)) {
    return v8.Check(tc);
  }
  if (match.IsEmpty() || match->IsNull()) {
    rval.setNull();
    return true;
  }
  v8::Local<v8::Value> index;
  v8::Local<v8::Value> matched;
  if (!match->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "index"))
           .ToLocal(&index) ||
      !match->Get(ctx, 0).ToLocal(&matched)) {
    return v8.Check(tc);
  }
  size_t matchLength =
      matched->IsString() ? size_t(matched.As<v8::String>()->Length()) : 0;
  *indexp = size_t(index->NumberValue(ctx).FromMaybe(0)) + matchLength;
  if (test) {
    rval.setBoolean(true);
    return true;
  }
  rval.set(ValueFromV8(cx, match));
  return true;
}

JS_PUBLIC_API bool JS::ObjectIsRegExp(JSContext* cx, JS::HandleObject obj,
                                      bool* isRegExp) {
  js::ESClass cls;
  if (!BuiltinClassOf(cx, obj, &cls)) {
    return false;
  }
  *isRegExp = cls == js::ESClass::RegExp;
  return true;
}

JS_PUBLIC_API int64_t JS::ToBigInt64(const JS::BigInt* bi) {
  JSContext* cx = CurrentContext();
  v8::HandleScope scope(cx->isolate);
  return ToV8(cx, const_cast<JS::BigInt*>(bi))->Int64Value();
}

JS_PUBLIC_API uint64_t JS::ToBigUint64(const JS::BigInt* bi) {
  JSContext* cx = CurrentContext();
  v8::HandleScope scope(cx->isolate);
  return ToV8(cx, const_cast<JS::BigInt*>(bi))->Uint64Value();
}

bool JS::AddMozDateTimeFormatConstructor(JSContext* cx,
                                         JS::Handle<JSObject*> intl) {
  JS::Rooted<JS::Value> ctor(cx);
  if (!IntlMember(cx, "DateTimeFormat", &ctor)) {
    return false;
  }
  return JS_DefineProperty(cx, intl, "DateTimeFormat", ctor, 0);
}

bool JS::AddMozDisplayNamesConstructor(JSContext* cx,
                                       JS::Handle<JSObject*> intl) {
  JS::Rooted<JS::Value> ctor(cx);
  if (!IntlMember(cx, "DisplayNames", &ctor)) {
    return false;
  }
  return JS_DefineProperty(cx, intl, "DisplayNames", ctor, 0);
}

bool JS::AddMozGetCalendarInfo(JSContext* cx, JS::Handle<JSObject*> intl) {
  return !!JS_DefineFunction(cx, intl, "getCalendarInfo",
                             GetCalendarInfoNative, 1, 0);
}

JSObject* JS::GetIteratorObject(JSContext* cx, JS::HandleValue obj,
                                bool isAsync) {
  const char* notIterable = nullptr;
  JS::Rooted<JSObject*> record(cx);
  {
    AutoV8 v8(cx);
    v8::Isolate* isolate = v8.isolate();
    v8::Local<v8::Context> ctx = v8.context();
    v8::TryCatch tc(isolate);
    v8::Local<v8::Value> target = ToV8(cx, obj.get());
    bool notCallable = false;
    v8::Local<v8::Value> method;
    bool fromSync = false;
    if (isAsync) {
      if (!GetMethodV8(cx, ctx, target, v8::Symbol::GetAsyncIterator(isolate),
                       &method, &notCallable)) {
        v8.Check(tc);
        return nullptr;
      }
      if (!notCallable && method->IsUndefined()) {
        fromSync = true;
      }
    }
    if (!isAsync || fromSync) {
      if (!GetMethodV8(cx, ctx, target, v8::Symbol::GetIterator(isolate),
                       &method, &notCallable)) {
        v8.Check(tc);
        return nullptr;
      }
    }
    v8::Local<v8::Value> iterator;
    if (notCallable || method->IsUndefined()) {
      notIterable = TypeOfName(target);
    } else if (!method.As<v8::Object>()
                    ->CallAsFunction(ctx, target, 0, nullptr)
                    .ToLocal(&iterator)) {
      v8.Check(tc);
      return nullptr;
    } else if (!iterator->IsObject()) {
      notIterable = TypeOfName(target);
    }
    if (!notIterable) {
      v8::Local<v8::Value> nextMethod;
      if (!iterator.As<v8::Object>()
               ->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "next"))
               .ToLocal(&nextMethod)) {
        v8.Check(tc);
        return nullptr;
      }
      if (fromSync) {
        v8::Local<v8::Object> asyncIter =
            CreateAsyncFromSyncIterator(isolate, ctx, iterator, nextMethod);
        v8::Local<v8::Value> asyncNext;
        if (!asyncIter->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "next"))
                 .ToLocal(&asyncNext)) {
          v8.Check(tc);
          return nullptr;
        }
        iterator = asyncIter;
        nextMethod = asyncNext;
      }
      record = ObjectFromV8(cx, IteratorRecord(isolate, iterator, nextMethod));
    }
  }
  if (notIterable) {
    JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                              JSMSG_NOT_ITERABLE, notIterable);
    return nullptr;
  }
  return record;
}

bool JS::IteratorNext(JSContext* cx, JS::HandleObject iteratorRecord,
                      JS::MutableHandleValue result) {
  JS::Rooted<JS::Value> iterator(cx);
  JS::Rooted<JS::Value> nextMethod(cx);
  if (!GetNamedProperty(cx, iteratorRecord, "iterator", &iterator) ||
      !GetNamedProperty(cx, iteratorRecord, "nextMethod", &nextMethod)) {
    return false;
  }
  if (!JS::Call(cx, iterator, nextMethod, JS::HandleValueArray::empty(),
                result)) {
    return false;
  }
  if (!result.isObject()) {
    JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                              JSMSG_OBJECT_REQUIRED,
                              result.isNull() ? "null" : "value");
    return false;
  }
  return true;
}

bool JS::IteratorComplete(JSContext* cx, JS::HandleObject iterResult,
                          bool* done) {
  JS::Rooted<JS::Value> doneV(cx);
  if (!GetNamedProperty(cx, iterResult, "done", &doneV)) {
    return false;
  }
  *done = JS::ToBoolean(doneV);
  return true;
}

bool JS::IteratorValue(JSContext* cx, JS::HandleObject iterResult,
                       JS::MutableHandleValue value) {
  return GetNamedProperty(cx, iterResult, "value", value);
}

bool JS::GetIteratorRecordIterator(JSContext* cx,
                                   JS::HandleObject iteratorRecord,
                                   JS::MutableHandleValue iterator) {
  return GetNamedProperty(cx, iteratorRecord, "iterator", iterator);
}

bool JS::GetReturnMethod(JSContext* cx, JS::HandleValue iterator,
                         JS::MutableHandleValue result) {
  bool notCallable = false;
  {
    AutoV8 v8(cx);
    v8::TryCatch tc(v8.isolate());
    v8::Local<v8::Value> method;
    if (!GetMethodV8(cx, v8.context(), ToV8(cx, iterator.get()),
                     v8::String::NewFromUtf8Literal(v8.isolate(), "return"),
                     &method, &notCallable)) {
      return v8.Check(tc);
    }
    if (!notCallable) {
      result.set(ValueFromV8(cx, method));
    }
  }
  if (notCallable) {
    JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                              JSMSG_NOT_FUNCTION, "return method");
    return false;
  }
  return true;
}

bool JS::ForOfIterator::init(JS::Handle<JS::Value> iterable,
                             NonIterableBehavior nonIterableBehavior) {
  JSContext* cx = cx_;
  const char* notIterable = nullptr;
  {
    AutoV8 v8(cx);
    v8::Isolate* isolate = v8.isolate();
    v8::Local<v8::Context> ctx = v8.context();
    v8::TryCatch tc(isolate);
    v8::Local<v8::Value> value = ToV8(cx, iterable.get());
    v8::Local<v8::Object> obj;
    if (!value->ToObject(ctx).ToLocal(&obj)) {
      return v8.Check(tc);
    }
    v8::Local<v8::Value> callee;
    if (!obj->Get(ctx, v8::Symbol::GetIterator(isolate)).ToLocal(&callee)) {
      return v8.Check(tc);
    }
    if (obj->IsArray()) {
      v8::Local<v8::Value> arrayValues =
          Intrinsic(cx, IntrinsicId::ArrayValues);
      v8::Local<v8::Value> arrayIterNext =
          Intrinsic(cx, IntrinsicId::ArrayIteratorNext);
      v8::Local<v8::Value> probe;
      v8::Local<v8::Value> currentNext;
      if (callee->StrictEquals(arrayValues) && arrayIterNext->IsFunction() &&
          arrayValues.As<v8::Function>()
              ->Call(ctx, v8::Array::New(isolate), 0, nullptr)
              .ToLocal(&probe) &&
          probe->IsObject() &&
          probe.As<v8::Object>()
              ->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "next"))
              .ToLocal(&currentNext) &&
          currentNext->StrictEquals(arrayIterNext)) {
        isOptimizedArray_ = true;
        arrayIndex_ = 0;
        iteratorOrArray_ = ObjectFromV8(cx, obj);
        nextMethod_.setUndefined();
        return true;
      }
    }
    if (nonIterableBehavior == AllowNonIterable && callee->IsUndefined()) {
      return true;
    }
    if (!callee->IsObject() || !callee.As<v8::Object>()->IsCallable()) {
      notIterable = TypeOfName(value);
    } else {
      v8::Local<v8::Value> res;
      if (!callee.As<v8::Object>()
               ->CallAsFunction(ctx, value, 0, nullptr)
               .ToLocal(&res)) {
        return v8.Check(tc);
      }
      if (!res->IsObject()) {
        isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(
                isolate, "iterator is not a non-null object")));
        return v8.Check(tc);
      }
      v8::Local<v8::Value> next;
      if (!res.As<v8::Object>()
               ->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "next"))
               .ToLocal(&next)) {
        return v8.Check(tc);
      }
      iteratorOrArray_ = ObjectFromV8(cx, res.As<v8::Object>());
      nextMethod_ = ValueFromV8(cx, next);
      return true;
    }
  }
  JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                            JSMSG_NOT_ITERABLE, notIterable);
  return false;
}

bool JS::ForOfIterator::next(JS::MutableHandle<JS::Value> val, bool* done) {
  JSContext* cx = cx_;
  if (isOptimizedArray_) {
    uint32_t length = 0;
    {
      v8::HandleScope scope(cx->isolate);
      length = ToV8(cx, iteratorOrArray_.get()).As<v8::Array>()->Length();
    }
    if (arrayIndex_ >= length) {
      val.setUndefined();
      *done = true;
      return true;
    }
    *done = false;
    return JS_GetElement(cx, iteratorOrArray_, arrayIndex_++, val);
  }
  JS::Rooted<JS::Value> iterator(cx, JS::ObjectValue(*iteratorOrArray_));
  JS::Rooted<JS::Value> v(cx);
  if (!JS::Call(cx, iterator, nextMethod_, JS::HandleValueArray::empty(), &v)) {
    return false;
  }
  if (!v.isObject()) {
    JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                              JSMSG_OBJECT_REQUIRED, "iterator result");
    return false;
  }
  JS::Rooted<JSObject*> resultObj(cx, &v.toObject());
  JS::Rooted<JS::Value> doneV(cx);
  if (!JS_GetProperty(cx, resultObj, "done", &doneV)) {
    return false;
  }
  *done = JS::ToBoolean(doneV);
  if (*done) {
    val.setUndefined();
    return true;
  }
  return JS_GetProperty(cx, resultObj, "value", val);
}

mozilla::Maybe<uint32_t> JS::ForOfIterator::sizeHint() const {
  if (!isOptimizedArray_) {
    return mozilla::Nothing();
  }
  JSContext* cx = cx_;
  v8::HandleScope scope(cx->isolate);
  return mozilla::Some(
      ToV8(cx, iteratorOrArray_.get()).As<v8::Array>()->Length());
}
