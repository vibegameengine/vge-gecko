#ifndef vge_bedrock_jsapi_VgeJsInternal_h
#define vge_bedrock_jsapi_VgeJsInternal_h

#include <stdint.h>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "jsapi.h"
#include "jsfriendapi.h"
#include "js/Class.h"
#include "js/GCAPI.h"
#include "js/Promise.h"
#include "js/Warnings.h"
#include "js/friend/CycleCollector.h"
#include "mozilla/Maybe.h"
#include "js/HeapAPI.h"
#include "js/Proxy.h"
#include "js/shadow/Object.h"
#include "js/shadow/Realm.h"
#include "js/shadow/Shape.h"
#include "js/shadow/String.h"
#include "js/shadow/Symbol.h"
#include "js/shadow/Zone.h"
#include "mozilla/LinkedList.h"
#include "cppgc/allocation.h"
#include "cppgc/garbage-collected.h"
#include "cppgc/visitor.h"
#include "v8-cppgc.h"
#include "v8.h"

namespace vge::jsapi {

class Anchor;
struct Shape;

enum class CellKind : uint8_t { Object, String, Symbol, BigInt };

constexpr size_t kObjectHeaderWords = 3;
constexpr size_t kFixedSlots = JS::shadow::Object::MAX_FIXED_SLOTS;

struct CellTail {
  CellKind kind;
  bool rootedThisGC;
  bool isAtom;
  uint8_t tracedEpoch;
  bool classFinalized;
  uint32_t serial;
  v8::Global<v8::Value> handle;
  v8::TracedReference<v8::Value> traced;
  Anchor* anchor;
  CellTail* nextFree;
};

struct ObjectCell {
  Shape* shape;
  uintptr_t words[kObjectHeaderWords - 1 + kFixedSlots];
  CellTail tail;

  JS::Value* fixedSlots() {
    return reinterpret_cast<JS::Value*>(&words[kObjectHeaderWords - 1]);
  }
  js::detail::ProxyDataLayout* proxyData() {
    return reinterpret_cast<js::detail::ProxyDataLayout*>(&words[0]);
  }
  JSObject* asObject() { return reinterpret_cast<JSObject*>(this); }
  static ObjectCell* from(const JSObject* aObj) {
    return reinterpret_cast<ObjectCell*>(const_cast<JSObject*>(aObj));
  }
};

struct StringCell {
  uintptr_t flags;
  const void* chars;
  const JSExternalStringCallbacks* external;
  uintptr_t pad;
  CellTail tail;

  JSString* asString() { return reinterpret_cast<JSString*>(this); }
  static StringCell* from(const JSString* aStr) {
    return reinterpret_cast<StringCell*>(const_cast<JSString*>(aStr));
  }
  uint32_t length() const { return uint32_t(flags >> 32); }
  bool latin1() const {
    return flags & JS::shadow::String::LATIN1_CHARS_BIT;
  }
};

struct SymbolCell {
  void* header;
  uint32_t code;
  uint32_t pad;
  uintptr_t pad2[2];
  CellTail tail;

  JS::Symbol* asSymbol() { return reinterpret_cast<JS::Symbol*>(this); }
  static SymbolCell* from(const JS::Symbol* aSym) {
    return reinterpret_cast<SymbolCell*>(const_cast<JS::Symbol*>(aSym));
  }
};

struct BigIntCell {
  uintptr_t header;
  uintptr_t pad[3];
  CellTail tail;

  JS::BigInt* asBigInt() { return reinterpret_cast<JS::BigInt*>(this); }
  static BigIntCell* from(const JS::BigInt* aBig) {
    return reinterpret_cast<BigIntCell*>(const_cast<JS::BigInt*>(aBig));
  }
};

struct CellPool {
  CellTail* freeList = nullptr;
  uint8_t* cursor = nullptr;
  uint8_t* limit = nullptr;
};

struct Shape {
  JS::shadow::Shape shadow;
  JS::shadow::BaseShape base;
  uint32_t reservedSlots;
  bool isProxy;
  bool isFunction;
};

Shape* ShapeFor(const JSClass* aClass, JS::Realm* aRealm);

extern const JSClass ArrayClass;
extern const uint16_t kWrapperEmbedderId;

namespace buffers {
const JSClass* ClassForV8(v8::Local<v8::Object> aObject);
}

JS::Value* DynamicSlots(ObjectCell* aCell);
JS::Realm* RealmOfScript(const JSScript* aScript);
void FreeObjectCell(ObjectCell* aCell);
void DrainDeferredObjectFrees(JSRuntime* aRuntime, size_t aLimit);
void FreeStringChars(StringCell* aCell);

JSContext* CurrentContext();
v8::Isolate* MainIsolate();
v8::Platform* CurrentPlatform();
void BindCellHandle(JSContext* aCx, CellTail* aTail, v8::Local<v8::Value> aValue, bool aStrong);
void SetTraced(v8::Isolate* aIsolate, CellTail* aTail,
               v8::Local<v8::Value> aValue);
void ForgetTraced(CellTail* aTail);
void NoteDeadAnchoredCell(CellTail* aTail);
JSString* AtomFromV8(JSContext* aCx, v8::Local<v8::String> aString);
JSString* AtomizeCell(JSContext* aCx, JSString* aStr);

void* AllocCell(size_t aSize);
void FreeCell(void* aCell, size_t aSize);
bool IsCellAddress(const void* aPtr);
void ReleaseCellChunks(JSRuntime* aRuntime);

CellTail* TailOf(js::gc::Cell* aCell, JS::TraceKind aKind);

ObjectCell* NewObjectCell(JSContext* aCx, Shape* aShape,
                          v8::Local<v8::Object> aObject, bool aAnchored);
JSObject* ObjectFromV8(JSContext* aCx, v8::Local<v8::Object> aObject);
v8::Local<v8::Object> ToV8(JSContext* aCx, JSObject* aObj);

JSString* StringFromV8(JSContext* aCx, v8::Local<v8::String> aString);
v8::Local<v8::String> ToV8(JSContext* aCx, JSString* aStr);
JS::Symbol* SymbolFromV8(JSContext* aCx, v8::Local<v8::Symbol> aSymbol);
v8::Local<v8::Symbol> ToV8(JSContext* aCx, JS::Symbol* aSym);
JS::BigInt* BigIntFromV8(JSContext* aCx, v8::Local<v8::BigInt> aBigInt);
v8::Local<v8::BigInt> ToV8(JSContext* aCx, JS::BigInt* aBig);

bool ValueFromV8(JSContext* aCx, v8::Local<v8::Value> aValue,
                 JS::MutableHandleValue aResult);
JS::Value ValueFromV8(JSContext* aCx, v8::Local<v8::Value> aValue);
v8::Local<v8::Value> ToV8(JSContext* aCx, const JS::Value& aValue);

bool IdFromV8(JSContext* aCx, v8::Local<v8::Value> aKey,
              JS::MutableHandleId aResult);
v8::Local<v8::Value> ToV8(JSContext* aCx, jsid aId);

class Anchor final : public cppgc::GarbageCollected<Anchor> {
 public:
  explicit Anchor(ObjectCell* aCell) : mCell(aCell) {}
  ~Anchor();
  void Trace(cppgc::Visitor* aVisitor) const;
  ObjectCell* Cell() const { return mCell; }
  void TraceSlotValue(cppgc::Visitor* aVisitor, const JS::Value& aValue) const;
  void SetWrapper(v8::Isolate* aIsolate, v8::Local<v8::Object> aWrapper);

 private:
  ObjectCell* mCell;
  v8::TracedReference<v8::Object> mWrapper;
};

constexpr uint16_t kAnchorWrapperClassId = 1;

}

struct JSRuntime {
  JSRuntime();

  v8::Isolate* isolate = nullptr;
  std::unique_ptr<v8::CppHeap> cppHeap;
  JSContext* mainContext = nullptr;
  JS::Zone* zone = nullptr;
  vge::jsapi::CellPool cellPools[2];
  std::vector<void*> cellChunks;
  v8::Global<v8::Private> cellKey;
  v8::Global<v8::Private> slotsKey;
  mozilla::LinkedList<js::PersistentRootedBase>
      heapRoots[size_t(JS::RootKind::Limit)];
  std::vector<std::pair<JSTraceDataOp, void*>> blackRootTracers;
  bool (*blackGlobalCallback)(JSObject*) = nullptr;
  JSGrayRootsTracer grayRootTracer = nullptr;
  void* grayRootTracerData = nullptr;
  std::vector<std::pair<JSGCCallback, void*>> gcCallbacks;
  std::vector<std::pair<JSFinalizeCallback, void*>> finalizeCallbacks;
  JSGCCallback gcCallback = nullptr;
  void* gcCallbackData = nullptr;
  const JSWrapObjectCallbacks* wrapObjectCallbacks = nullptr;
  JSDestroyCompartmentCallback destroyCompartmentCallback = nullptr;
  JSDestroyZoneCallback destroyZoneCallback = nullptr;
  JSSizeOfIncludingThisCompartmentCallback sizeOfCompartmentCallback = nullptr;
  JS::DestroyRealmCallback destroyRealmCallback = nullptr;
  JS::RealmNameCallback realmNameCallback = nullptr;
  const JSSecurityCallbacks* securityCallbacks = nullptr;
  const JSPrincipals* trustedPrincipals = nullptr;
  std::vector<vge::jsapi::CellTail*> strongThisGC;
  std::unordered_set<vge::jsapi::CellTail*> tracedTails;
  std::unordered_set<vge::jsapi::CellTail*> youngTails;
  uint8_t tracedEpoch = 1;
  std::vector<JS::Realm*> realms;
  std::vector<JS::Realm*> dyingRealms;
  std::unordered_set<vge::jsapi::ObjectCell*> objectCells;
  std::vector<std::pair<vge::jsapi::ObjectCell*, JS::Value*>> deferredObjectFrees;
  std::vector<vge::jsapi::ObjectCell*> pendingFinalize;
  std::vector<vge::jsapi::ObjectCell*> deadAnchored;
  void (*weakPointerSweep)(JSRuntime*) = nullptr;
  bool sweepingWeakPointers = false;
  std::unordered_map<const JSClass*, std::vector<vge::jsapi::Shape*>> shapes;
  std::unordered_map<std::u16string, vge::jsapi::StringCell*> atoms;
  std::unordered_multimap<int, vge::jsapi::SymbolCell*> symbols;
  uint32_t gcNumber = 0;
  bool inGC = false;
};

namespace JS {

class Zone : public JS::shadow::Zone {
 public:
  explicit Zone(JSRuntime* aRuntime)
      : JS::shadow::Zone(aRuntime, nullptr, NormalZone) {}
  void* data = nullptr;
};

class Compartment {
 public:
  explicit Compartment(JS::Zone* aZone) : zone(aZone) {}
  JS::Zone* zone;
  void* data = nullptr;
  std::vector<JS::Realm*> realms;
  bool systemCompartment = false;
};

class Realm : public JS::shadow::Realm {
 public:
  Realm(JS::Compartment* aCompartment, JSRuntime* aRuntime)
      : JS::shadow::Realm(aCompartment), runtime(aRuntime) {}
  JSRuntime* runtime;
  v8::Global<v8::Context> context;
  JSObject* global = nullptr;
  const JSClass* globalClass = nullptr;
  JSPrincipals* principals = nullptr;
  void* data = nullptr;
  bool isSystem = false;
  bool contextDead = false;
  uint32_t enterDepth = 0;
  size_t liveCells = 0;
  RealmCreationOptions creationOptions;
  RealmBehaviors behaviors;
};

}

struct JSContext : public JS::RootingContext {
  explicit JSContext(JSRuntime* aRuntime);

  JSRuntime* runtime;
  v8::Isolate* isolate;
  void* data = nullptr;
  alignas(v8::HandleScope) unsigned char taskScope[sizeof(v8::HandleScope)];
  JS::Value pendingException;
  bool exceptionPending = false;
  bool throwingOutOfMemory = false;
  std::vector<JS::Realm*> realmStack;
  JS::WarningReporter warningReporter = nullptr;
  JSInterruptCallback interruptCallbacks[4] = {};
  size_t interruptCallbackCount = 0;
  JS::JobQueue* jobQueue = nullptr;

  void enterRealm(JS::Realm* aRealm);
  void leaveRealm(JS::Realm* aOld);
  JS::Realm* realm() const { return realm_; }
  JS::Zone* zone() const { return zone_; }
  v8::Local<v8::Context> v8Context();
  void setPending(v8::Local<v8::Value> aException);
};

namespace vge::jsapi {

class AutoV8 {
 public:
  explicit AutoV8(JSContext* aCx);
  ~AutoV8();

  v8::Isolate* isolate() const { return mIsolate; }
  v8::Local<v8::Context> context() const { return mContext; }
  bool Check(const v8::TryCatch& aTry);

 private:
  JSContext* mCx;
  v8::Isolate* mIsolate;
  v8::HandleScope mScope;
  v8::Local<v8::Context> mContext;
  mozilla::Maybe<v8::Context::Scope> mContextScope;
};

bool ReportPending(JSContext* aCx, v8::TryCatch& aTry);

void PushNoResolve(JSContext* aCx, JSObject* aObj, jsid aId);
void PopNoResolve(JSContext* aCx);

class AutoNoResolve {
 public:
  AutoNoResolve(JSContext* aCx, JSObject* aObj, jsid aId) : mCx(aCx) {
    PushNoResolve(aCx, aObj, aId);
  }
  ~AutoNoResolve() { PopNoResolve(mCx); }

 private:
  JSContext* mCx;
};

JSObject* NewClassObject(JSContext* aCx, const JSClass* aClass,
                         JS::HandleObject aProto);
v8::Local<v8::FunctionTemplate> ClassTemplate(JSContext* aCx,
                                              const JSClass* aClass);
JSFunction* NewNativeFunction(JSContext* aCx, JSNative aNative,
                              unsigned aNargs, unsigned aFlags,
                              JS::HandleString aName, const JSJitInfo* aInfo);
JSObject* NewProxyCell(JSContext* aCx, const js::BaseProxyHandler* aHandler,
                       JS::HandleValue aPrivate, JS::HandleObject aProto,
                       const JSClass* aClass);
JS::Realm* RealmOfV8Context(JSContext* aCx, v8::Local<v8::Context> aContext);

void WatchRealmContext(JS::Realm* aRealm);
void TraceEnteredRealmGlobals(JSRuntime* aRuntime, JSTracer* aTracer);
void ForgetRealmClasses(JS::Realm* aRealm);
void ForgetCompartmentClasses(JS::Compartment* aCompartment);
void ForgetRealmProxies(JS::Realm* aRealm);
void ForgetCompartmentProxies(JS::Compartment* aCompartment);
void ForgetRealmScripts(JS::Realm* aRealm);

JSRuntime* Runtime();

}

#endif
