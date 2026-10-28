#ifndef vge_bedrock_jsapi_classes_VgeJsClassesState_h
#define vge_bedrock_jsapi_classes_VgeJsClassesState_h

#include <unordered_map>
#include <string>
#include <utility>
#include <vector>

#include "VgeJsInternal.h"
#include "js/CallAndConstruct.h"
#include "js/ErrorReport.h"
#include "js/Exception.h"
#include "js/GlobalObject.h"
#include "js/Principals.h"
#include "js/PropertyAndElement.h"
#include "js/PropertyDescriptor.h"
#include "js/PropertySpec.h"
#include "js/ProtoKey.h"
#include "js/Realm.h"
#include "js/RealmOptions.h"
#include "js/Wrapper.h"
#include "js/friend/ErrorMessages.h"
#include "js/friend/WindowProxy.h"

namespace vge::jsapi {

constexpr int kRealmEmbedderIndex = 24;

struct alignas(8) WrapperTypeTag {
  uint16_t embedderId;
};

extern const WrapperTypeTag kWrapperTypeTag;

extern const JSClass FunctionClass;
extern const JSClass FunctionExtendedClass;
extern const JSClass PlainObjectClass;
extern const JSClass DataViewClasses[3];

struct RealmExtra {
  v8::Global<v8::Object> ctors[JSProto_LIMIT];
  v8::Global<v8::Object> protos[JSProto_LIMIT];
  JSObject* objectProto = nullptr;
  JSObject* functionProto = nullptr;
  v8::Global<v8::Function> eval;
  v8::Global<v8::Function> functionCtor;
  v8::Global<v8::Function> isExtensible;
  v8::Global<v8::Function> reflectSet;
  v8::Global<v8::Object> iteratorProto;
  v8::Global<v8::Object> asyncIteratorProto;
  v8::Global<v8::Object> mapIteratorProto;
  v8::Global<v8::Object> setIteratorProto;
  v8::Global<v8::Object> keyObject;
  v8::Global<v8::Object> innerGlobal;
  std::unordered_map<std::string, v8::Global<v8::Function>> selfHosted;
  bool firedOnNewGlobal = false;
};

struct CompartmentExtra {
  std::unordered_map<JSObject*, v8::Global<v8::Object>> wrappers;
};

struct ClassesState {
  JSRuntime* runtime = nullptr;
  std::unordered_map<const JSClass*, v8::Global<v8::FunctionTemplate>>
      templates;
  v8::Global<v8::ObjectTemplate> holderTemplate;
  v8::Global<v8::Value> securityToken;
  v8::Global<v8::Private> immutableProtoKey;
  std::unordered_map<JS::Realm*, RealmExtra*> realms;
  std::unordered_map<JS::Compartment*, CompartmentExtra*> compartments;
  JSDestroyPrincipalsOp destroyPrincipals = nullptr;
  JSReadPrincipalsOp readPrincipals = nullptr;
  const js::DOMCallbacks* domCallbacks = nullptr;
  js::ScriptEnvironmentPreparer* environmentPreparer = nullptr;
  js::PreserveWrapperCallback preserveWrapper = nullptr;
  js::HasReleasedWrapperCallback hasReleasedWrapper = nullptr;
  std::vector<JSObject*> pendingPreservations;
  std::vector<std::pair<JS::Value*, const char*>> rawRoots;
  bool rawRootTracerInstalled = false;
  bool allowContentJS = true;
  void (*scriptPrivateAddRef)(const JS::Value&) = nullptr;
  void (*scriptPrivateRelease)(const JS::Value&) = nullptr;
  std::vector<int> hiddenCallerDepths;
  std::vector<std::pair<JSObject*, jsid>> resolving;
  JSObject* testingFunctions = nullptr;
  std::unordered_map<uint32_t, v8::Global<v8::Symbol>> fallbackSymbols;
};

ClassesState& State(JSContext* aCx);
RealmExtra& RealmData(JS::Realm* aRealm);
CompartmentExtra& CompartmentData(JS::Compartment* aCompartment);

JS::Realm* RealmOfCell(JSObject* aObj);
v8::Local<v8::Object> ScriptTarget(JSContext* aCx, JSObject* aObj);
bool IsGlobalCell(JSObject* aObj);

void ThrowPendingToV8(JSContext* aCx);
void SlotWriteBarrier(JSContext* aCx, const JS::Value& aValue);
bool IdIsAsciiName(jsid aId, const char* aName);
JSObject* NewPlainObjectWithProto(JSContext* aCx, JS::HandleObject aProto);
bool ReportJSError(JSContext* aCx, unsigned aErrorNumber, const char* aArg0 = nullptr,
                   const char* aArg1 = nullptr, const char* aArg2 = nullptr);

v8::Local<v8::ObjectTemplate> HolderTemplate(JSContext* aCx);
void AnchorCellOn(JSContext* aCx, ObjectCell* aCell, v8::Local<v8::Object> aHolder);

void InvokeNative(JSContext* aCx, JSNative aNative, JS::Realm* aRealm,
                  v8::Local<v8::Object> aCallee,
                  const v8::FunctionCallbackInfo<v8::Value>& aInfo);

JSFunction* NewNativeFunctionImpl(JSContext* aCx, JSNative aNative,
                                  unsigned aNargs, unsigned aFlags,
                                  JS::HandleString aName,
                                  const JSJitInfo* aInfo, bool aExtended,
                                  JS::HandleObject aProto);
JSFunction* SelfHostedFunction(JSContext* aCx, const char* aSelfHostedName,
                               JS::HandleId aId, unsigned aNargs);
bool IdToFunctionName(JSContext* aCx, JS::HandleId aId,
                      JS::MutableHandleString aName);
JS::Symbol* WellKnownSymbol(JSContext* aCx, JS::SymbolCode aCode);
bool PropertyNameToId(JSContext* aCx, JSPropertySpec::Name aName,
                      JS::MutableHandleId aId);

void CaptureRealmIntrinsics(JSContext* aCx, JS::Realm* aRealm,
                            v8::Local<v8::Context> aContext);
void KeepRealmIntrinsics(JSContext* aCx, JS::Realm* aRealm,
                         v8::Local<v8::Context> aContext);
v8::Local<v8::Private> RealmPrivateKey(v8::Isolate* aIsolate, const char* aName);
JSProtoKey StandardKeyOf(JSContext* aCx, JSObject* aObj, bool* aIsPrototype);
const char* StandardClassName(JSProtoKey aKey);
bool IsDeselectedConstructor(JS::Realm* aRealm, JSProtoKey aKey);

}

#endif
