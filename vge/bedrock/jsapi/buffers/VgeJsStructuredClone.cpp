#include "VgeJsBuffers.h"

#include <string.h>

#include <string>

#include "js/Array.h"
#include "js/Date.h"
#include "js/ErrorReport.h"
#include "js/Exception.h"
#include "js/PropertyAndElement.h"
#include "js/Proxy.h"
#include "js/RegExpFlags.h"
#include "js/Wrapper.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/EndianUtils.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/StringBuffer.h"
#include "v8-date.h"
#include "v8-regexp.h"
#include "v8-typed-array.h"

using mozilla::NativeEndian;
using namespace vge::jsapi;
using namespace vge::jsapi::buffers;

namespace {

enum StructuredDataType : uint32_t {
  SCTAG_FLOAT_MAX = 0xFFF00000,
  SCTAG_HEADER = 0xFFF10000,
  SCTAG_NULL = 0xFFFF0000,
  SCTAG_UNDEFINED,
  SCTAG_BOOLEAN,
  SCTAG_INT32,
  SCTAG_STRING,
  SCTAG_DATE_OBJECT,
  SCTAG_REGEXP_OBJECT,
  SCTAG_ARRAY_OBJECT,
  SCTAG_OBJECT_OBJECT,
  SCTAG_ARRAY_BUFFER_OBJECT_V2,
  SCTAG_BOOLEAN_OBJECT,
  SCTAG_STRING_OBJECT,
  SCTAG_NUMBER_OBJECT,
  SCTAG_BACK_REFERENCE_OBJECT,
  SCTAG_DO_NOT_USE_1,
  SCTAG_DO_NOT_USE_2,
  SCTAG_TYPED_ARRAY_OBJECT_V2,
  SCTAG_MAP_OBJECT,
  SCTAG_SET_OBJECT,
  SCTAG_END_OF_KEYS,
  SCTAG_DO_NOT_USE_3,
  SCTAG_DATA_VIEW_OBJECT_V2,
  SCTAG_SAVED_FRAME_OBJECT,
  SCTAG_JSPRINCIPALS,
  SCTAG_NULL_JSPRINCIPALS,
  SCTAG_RECONSTRUCTED_SAVED_FRAME_PRINCIPALS_IS_SYSTEM,
  SCTAG_RECONSTRUCTED_SAVED_FRAME_PRINCIPALS_IS_NOT_SYSTEM,
  SCTAG_SHARED_ARRAY_BUFFER_OBJECT,
  SCTAG_SHARED_WASM_MEMORY_OBJECT,
  SCTAG_BIGINT,
  SCTAG_BIGINT_OBJECT,
  SCTAG_ARRAY_BUFFER_OBJECT,
  SCTAG_TYPED_ARRAY_OBJECT,
  SCTAG_DATA_VIEW_OBJECT,
  SCTAG_ERROR_OBJECT,
  SCTAG_RESIZABLE_ARRAY_BUFFER_OBJECT,
  SCTAG_GROWABLE_SHARED_ARRAY_BUFFER_OBJECT,
  SCTAG_IMMUTABLE_ARRAY_BUFFER_OBJECT,
  SCTAG_TYPED_ARRAY_V1_MIN = 0xFFFF0100,
  SCTAG_TYPED_ARRAY_V1_MAX = SCTAG_TYPED_ARRAY_V1_MIN + JS::Scalar::Uint8Clamped,
  SCTAG_TRANSFER_MAP_HEADER = 0xFFFF0200,
  SCTAG_TRANSFER_MAP_PENDING_ENTRY,
  SCTAG_TRANSFER_MAP_ARRAY_BUFFER,
  SCTAG_TRANSFER_MAP_STORED_ARRAY_BUFFER,
  SCTAG_TRANSFER_MAP_END_OF_BUILTIN_TYPES,
  SCTAG_END_OF_BUILTIN_TYPES
};

enum TransferableMapHeader {
  SCTAG_TM_UNREAD = 0,
  SCTAG_TM_TRANSFERRING,
  SCTAG_TM_TRANSFERRED,
  SCTAG_TM_END
};

static_assert(SCTAG_END_OF_BUILTIN_TYPES <= JS_SCTAG_USER_MIN);

constexpr uint64_t kByteLengthLimit = v8::TypedArray::kMaxLength;
constexpr uint32_t kMaxRefCount = 0x7FFFFFFF;

inline uint64_t PairToUInt64(uint32_t aTag, uint32_t aData) {
  return uint64_t(aData) | (uint64_t(aTag) << 32);
}

size_t ComputePadding(size_t aNelems, size_t aElemSize) {
  size_t leftoverLength = (aNelems % sizeof(uint64_t)) * aElemSize;
  return (-leftoverLength) & (sizeof(uint64_t) - 1);
}

v8::Local<v8::Context> CurrentV8Context(JSContext* aCx) {
  v8::Local<v8::Context> ctx = aCx->v8Context();
  if (ctx.IsEmpty()) {
    for (JS::Realm* realm : aCx->runtime->realms) {
      if (!realm->context.IsEmpty()) {
        return realm->context.Get(aCx->isolate);
      }
    }
  }
  return ctx;
}

v8::Local<v8::String> Literal(v8::Isolate* aIsolate, const char* aText) {
  return v8::String::NewFromUtf8(aIsolate, aText).ToLocalChecked();
}

void ReportBadData(JSContext* aCx, const char* aWhat) {
  JS_ReportErrorNumberASCII(aCx, js::GetErrorMessage, nullptr,
                            JSMSG_SC_BAD_SERIALIZED_DATA, aWhat);
}

}

namespace js {

template <typename T, typename AllocPolicy>
struct BufferIterator {
  using BufferList = mozilla::BufferList<AllocPolicy>;

  explicit BufferIterator(const BufferList& aBuffer)
      : mBuffer(aBuffer), mIter(aBuffer.Iter()) {}

  explicit BufferIterator(const JSStructuredCloneData& aData)
      : mBuffer(aData.bufList_), mIter(aData.Start()) {}

  BufferIterator& operator=(const BufferIterator& aOther) {
    mIter = aOther.mIter;
    return *this;
  }

  [[nodiscard]] bool advance(size_t aSize = sizeof(T)) {
    return mIter.AdvanceAcrossSegments(mBuffer, aSize);
  }

  BufferIterator operator++(int) {
    BufferIterator ret = *this;
    (void)advance(sizeof(T));
    return ret;
  }

  BufferIterator& operator+=(size_t aSize) {
    (void)advance(aSize);
    return *this;
  }

  size_t operator-(const BufferIterator& aOther) const {
    return mBuffer.RangeLength(aOther.mIter, mIter);
  }

  bool operator==(const BufferIterator& aOther) const {
    return mBuffer.Start() == aOther.mBuffer.Start() && mIter == aOther.mIter;
  }
  bool operator!=(const BufferIterator& aOther) const {
    return !(*this == aOther);
  }

  bool done() const { return mIter.Done(); }

  [[nodiscard]] bool readBytes(char* aOutData, size_t aSize) {
    return mBuffer.ReadBytes(mIter, aOutData, aSize);
  }

  void write(const T& aData) {
    *reinterpret_cast<T*>(mIter.Data()) = aData;
  }

  T peek() const { return *reinterpret_cast<T*>(mIter.Data()); }

  bool canPeek() const { return mIter.HasRoomFor(sizeof(T)); }

  const BufferList& mBuffer;
  typename BufferList::IterImpl mIter;
};

bool SharedArrayRawBuffer::addReference() {
  uint32_t current = mRefCount.load();
  do {
    if (current >= kMaxRefCount) {
      return false;
    }
  } while (!mRefCount.compare_exchange_weak(current, current + 1));
  return true;
}

void SharedArrayRawBuffer::dropReference() {
  if (--mRefCount == 0) {
    delete this;
  }
}

SharedArrayRawBufferRefs& SharedArrayRawBufferRefs::operator=(
    SharedArrayRawBufferRefs&& aOther) {
  takeOwnership(std::move(aOther));
  return *this;
}

SharedArrayRawBufferRefs::~SharedArrayRawBufferRefs() { releaseAll(); }

bool SharedArrayRawBufferRefs::acquire(JSContext* aCx,
                                       SharedArrayRawBuffer* aRawbuf) {
  if (!refs_.append(aRawbuf)) {
    JS_ReportOutOfMemory(aCx);
    return false;
  }
  if (!aRawbuf->addReference()) {
    refs_.popBack();
    JS_ReportErrorNumberASCII(aCx, js::GetErrorMessage, nullptr,
                              JSMSG_SC_SAB_REFCNT_OFLO);
    return false;
  }
  return true;
}

bool SharedArrayRawBufferRefs::acquireAll(
    JSContext* aCx, const SharedArrayRawBufferRefs& aThat) {
  if (!refs_.reserve(refs_.length() + aThat.refs_.length())) {
    JS_ReportOutOfMemory(aCx);
    return false;
  }
  for (SharedArrayRawBuffer* ref : aThat.refs_) {
    if (!ref->addReference()) {
      JS_ReportErrorNumberASCII(aCx, js::GetErrorMessage, nullptr,
                                JSMSG_SC_SAB_REFCNT_OFLO);
      return false;
    }
    MOZ_ALWAYS_TRUE(refs_.append(ref));
  }
  return true;
}

void SharedArrayRawBufferRefs::takeOwnership(
    SharedArrayRawBufferRefs&& aOther) {
  refs_ = std::move(aOther.refs_);
}

void SharedArrayRawBufferRefs::releaseAll() {
  for (SharedArrayRawBuffer* ref : refs_) {
    ref->dropReference();
  }
  refs_.clear();
}

struct SCOutput {
  using Iter = BufferIterator<uint64_t, SystemAllocPolicy>;

  SCOutput(JSContext* aCx, JS::StructuredCloneScope aScope)
      : cx(aCx), buf(aScope) {}

  JSContext* context() const { return cx; }
  JS::StructuredCloneScope scope() const { return buf.scope(); }
  void sameProcessScopeRequired() { buf.sameProcessScopeRequired(); }

  [[nodiscard]] bool write(uint64_t aU) {
    uint64_t v = NativeEndian::swapToLittleEndian(aU);
    if (!buf.AppendBytes(reinterpret_cast<char*>(&v), sizeof(v))) {
      JS_ReportOutOfMemory(cx);
      return false;
    }
    return true;
  }

  [[nodiscard]] bool writePair(uint32_t aTag, uint32_t aData) {
    return write(PairToUInt64(aTag, aData));
  }

  [[nodiscard]] bool writeDouble(double aD) {
    return write(mozilla::BitwiseCast<uint64_t>(JS::CanonicalizeNaN(aD)));
  }

  template <class T>
  [[nodiscard]] bool writeArray(const T* aP, size_t aNelems) {
    if (aNelems == 0) {
      return true;
    }
    for (size_t i = 0; i < aNelems; i++) {
      T value = NativeEndian::swapToLittleEndian(aP[i]);
      if (!buf.AppendBytes(reinterpret_cast<char*>(&value), sizeof(value))) {
        JS_ReportOutOfMemory(cx);
        return false;
      }
    }
    return pad(ComputePadding(aNelems, sizeof(T)));
  }

  [[nodiscard]] bool writeBytes(const void* aP, size_t aNbytes) {
    if (aNbytes == 0) {
      return true;
    }
    if (!buf.AppendBytes(static_cast<const char*>(aP), aNbytes)) {
      JS_ReportOutOfMemory(cx);
      return false;
    }
    return pad(ComputePadding(aNbytes, 1));
  }

  [[nodiscard]] bool pad(size_t aPadBytes) {
    char zeroes[sizeof(uint64_t)] = {0};
    if (!buf.AppendBytes(zeroes, aPadBytes)) {
      JS_ReportOutOfMemory(cx);
      return false;
    }
    return true;
  }

  void setCallbacks(const JSStructuredCloneCallbacks* aCallbacks,
                    void* aClosure, OwnTransferablePolicy aPolicy) {
    buf.setCallbacks(aCallbacks, aClosure, aPolicy);
  }
  void extractBuffer(JSStructuredCloneData* aData) { *aData = std::move(buf); }

  uint64_t tell() const { return buf.Size(); }
  uint64_t count() const { return buf.Size() / sizeof(uint64_t); }
  Iter iter() { return Iter(buf); }
  size_t offset(Iter aDest) { return aDest - iter(); }

  JSContext* cx;
  JSStructuredCloneData buf;
};

class SCInput {
 public:
  using BufferIterator = js::BufferIterator<uint64_t, SystemAllocPolicy>;

  SCInput(JSContext* aCx, const JSStructuredCloneData& aData)
      : cx(aCx), point(aData) {}

  JSContext* context() const { return cx; }

  static void getPtr(uint64_t aData, void** aPtr) {
    *aPtr = reinterpret_cast<void*>(NativeEndian::swapFromLittleEndian(aData));
  }

  static void getPair(uint64_t aData, uint32_t* aTag, uint32_t* aDatap) {
    uint64_t u = NativeEndian::swapFromLittleEndian(aData);
    *aTag = uint32_t(u >> 32);
    *aDatap = uint32_t(u);
  }

  [[nodiscard]] bool read(uint64_t* aP) {
    if (!point.canPeek()) {
      *aP = 0;
      return reportTruncated();
    }
    *aP = NativeEndian::swapFromLittleEndian(point.peek());
    MOZ_ALWAYS_TRUE(point.advance());
    return true;
  }

  [[nodiscard]] bool readPair(uint32_t* aTag, uint32_t* aData) {
    uint64_t u;
    if (!read(&u)) {
      return false;
    }
    *aTag = uint32_t(u >> 32);
    *aData = uint32_t(u);
    return true;
  }

  [[nodiscard]] bool readDouble(double* aP) {
    uint64_t u;
    if (!read(&u)) {
      return false;
    }
    *aP = JS::CanonicalizeNaN(mozilla::BitwiseCast<double>(u));
    return true;
  }

  template <class T>
  [[nodiscard]] bool readArray(T* aP, size_t aNelems) {
    if (!aNelems) {
      return true;
    }
    mozilla::CheckedInt<size_t> size =
        mozilla::CheckedInt<size_t>(aNelems) * sizeof(T);
    if (!size.isValid()) {
      return reportTruncated();
    }
    if (!point.readBytes(reinterpret_cast<char*>(aP), size.value())) {
      memset(aP, 0, aNelems * sizeof(T));
      return reportTruncated();
    }
    if (sizeof(T) > 1) {
      NativeEndian::swapFromLittleEndianInPlace(aP, aNelems);
    }
    point += ComputePadding(aNelems, sizeof(T));
    return true;
  }

  [[nodiscard]] bool readBytes(void* aP, size_t aNbytes) {
    return readArray(static_cast<uint8_t*>(aP), aNbytes);
  }

  [[nodiscard]] bool readPtr(void** aP) {
    uint64_t u;
    if (!read(&u)) {
      return false;
    }
    *aP = reinterpret_cast<void*>(u);
    return true;
  }

  [[nodiscard]] bool get(uint64_t* aP) {
    if (!point.canPeek()) {
      return reportTruncated();
    }
    *aP = NativeEndian::swapFromLittleEndian(point.peek());
    return true;
  }

  [[nodiscard]] bool getPair(uint32_t* aTag, uint32_t* aData) {
    uint64_t u = 0;
    if (!get(&u)) {
      return false;
    }
    *aTag = uint32_t(u >> 32);
    *aData = uint32_t(u);
    return true;
  }

  const BufferIterator& tell() const { return point; }
  void seekTo(const BufferIterator& aPos) { point = aPos; }
  [[nodiscard]] bool seekBy(size_t aPos) {
    if (!point.advance(aPos)) {
      reportTruncated();
      return false;
    }
    return true;
  }

  bool reportTruncated() {
    ReportBadData(cx, "truncated");
    return false;
  }

 private:
  JSContext* cx;
  BufferIterator point;
};

}

namespace vge::jsapi::buffers {

bool ReportCloneError(JSContext* aCx,
                      const JSStructuredCloneCallbacks* aCallbacks,
                      uint32_t aErrorId, void* aClosure, const char* aArg) {
  unsigned errorNumber;
  switch (aErrorId) {
    case JS_SCERR_DUP_TRANSFERABLE:
      errorNumber = JSMSG_SC_DUP_TRANSFERABLE;
      break;
    case JS_SCERR_TRANSFERABLE:
      errorNumber = JSMSG_SC_NOT_TRANSFERABLE;
      break;
    case JS_SCERR_UNSUPPORTED_TYPE:
      errorNumber = JSMSG_SC_UNSUPPORTED_TYPE;
      break;
    case JS_SCERR_SHMEM_TRANSFERABLE:
      errorNumber = JSMSG_SC_SHMEM_TRANSFERABLE;
      break;
    case JS_SCERR_TRANSFERABLE_TWICE:
      errorNumber = JSMSG_SC_TRANSFERABLE_TWICE;
      break;
    case JS_SCERR_TYPED_ARRAY_DETACHED:
      errorNumber = JSMSG_TYPED_ARRAY_DETACHED;
      break;
    case JS_SCERR_WASM_NO_TRANSFER:
      errorNumber = JSMSG_WASM_NO_TRANSFER;
      break;
    case JS_SCERR_NOT_CLONABLE:
      errorNumber = JSMSG_SC_NOT_CLONABLE;
      break;
    case JS_SCERR_NOT_CLONABLE_WITH_COOP_COEP:
      errorNumber = JSMSG_SC_NOT_CLONABLE_WITH_COOP_COEP;
      break;
    default:
      MOZ_CRASH("Unknown errorId");
  }
  if (aCallbacks && aCallbacks->reportError) {
    const JSErrorFormatString* format =
        js::GetErrorMessage(nullptr, errorNumber);
    std::string message;
    for (const char* p = format ? format->format : ""; *p; p++) {
      if (p[0] == '{' && p[1] >= '0' && p[1] <= '9' && p[2] == '}') {
        message += aArg ? aArg : "";
        p += 2;
        continue;
      }
      message += *p;
    }
    aCallbacks->reportError(aCx, aErrorId, aClosure, message.c_str());
    return false;
  }
  JS_ReportErrorNumberASCII(aCx, js::GetErrorMessage, nullptr, errorNumber,
                            aArg ? aArg : "");
  return false;
}

}

namespace {

enum class Kind : uint8_t {
  Object,
  Array,
  Number,
  String,
  Boolean,
  RegExp,
  ArrayBuffer,
  SharedArrayBuffer,
  Date,
  Set,
  Map,
  Error,
  BigInt,
  TypedArray,
  DataView,
  Custom,
};

bool IsScriptClass(const JSClass* aClass) {
  return aClass == js::ObjectClassPtr || aClass == &ArrayClass ||
         aClass == js::FunctionClassPtr ||
         aClass == js::FunctionExtendedClassPtr || IsBufferClass(aClass);
}

Kind KindOfV8(v8::Local<v8::Object> aObj) {
  if (aObj->IsArray()) {
    return Kind::Array;
  }
  if (aObj->IsNumberObject()) {
    return Kind::Number;
  }
  if (aObj->IsStringObject()) {
    return Kind::String;
  }
  if (aObj->IsBooleanObject()) {
    return Kind::Boolean;
  }
  if (aObj->IsBigIntObject()) {
    return Kind::BigInt;
  }
  if (aObj->IsRegExp()) {
    return Kind::RegExp;
  }
  if (aObj->IsArrayBuffer()) {
    return Kind::ArrayBuffer;
  }
  if (aObj->IsSharedArrayBuffer()) {
    return Kind::SharedArrayBuffer;
  }
  if (aObj->IsDate()) {
    return Kind::Date;
  }
  if (aObj->IsSet()) {
    return Kind::Set;
  }
  if (aObj->IsMap()) {
    return Kind::Map;
  }
  if (aObj->IsNativeError()) {
    return Kind::Error;
  }
  if (aObj->IsTypedArray()) {
    return Kind::TypedArray;
  }
  if (aObj->IsDataView()) {
    return Kind::DataView;
  }
  if (aObj->IsPromise() || aObj->IsMapIterator() || aObj->IsSetIterator() ||
      aObj->IsArgumentsObject() || aObj->IsFunction() || aObj->IsProxy() ||
      aObj->IsSymbolObject() || aObj->IsWasmMemoryObject() ||
      aObj->IsWasmModuleObject() || aObj->IsGeneratorObject() ||
      aObj->IsWeakMap() || aObj->IsWeakSet() || aObj->IsWeakRef() ||
      aObj->IsModuleNamespaceObject()) {
    return Kind::Custom;
  }
  return Kind::Object;
}

}

struct JSStructuredCloneWriter {
 public:
  JSStructuredCloneWriter(JSContext* aCx, JS::StructuredCloneScope aScope,
                          const JS::CloneDataPolicy& aPolicy,
                          const JSStructuredCloneCallbacks* aCallbacks,
                          void* aClosure, v8::Local<v8::Value> aTransferable)
      : out(aCx, aScope),
        callbacks(aCallbacks),
        closure(aClosure),
        cloneDataPolicy(aPolicy),
        transferable(aTransferable) {
    out.setCallbacks(aCallbacks, aClosure,
                     OwnTransferablePolicy::OwnsTransferablesIfAny);
    ctx = CurrentV8Context(aCx);
    memory = v8::Map::New(aCx->isolate);
  }

  bool init() {
    return parseTransferable() && writeHeader() && writeTransferMap();
  }

  bool write(v8::Local<v8::Value> aValue);

  js::SCOutput& output() { return out; }
  JSContext* context() { return out.context(); }
  v8::Isolate* isolate() { return out.context()->isolate; }

  void extractBuffer(JSStructuredCloneData* aData) {
    out.extractBuffer(aData);
  }

  bool writeString(uint32_t aTag, v8::Local<v8::String> aString);
  bool writeStringChars(uint32_t aTag, const void* aChars, uint32_t aLength,
                        bool aLatin1);
  bool writeBigInt(uint32_t aTag, v8::Local<v8::BigInt> aBigInt);
  bool writeArrayBuffer(v8::Local<v8::Object> aBuffer);
  bool writeTypedArray(v8::Local<v8::Object> aView);
  bool writeDataView(v8::Local<v8::Object> aView);
  bool writeSharedArrayBuffer(v8::Local<v8::Object> aBuffer);
  bool startObject(v8::Local<v8::Object> aObj, bool* aBackref);
  bool writePrimitive(v8::Local<v8::Value> aValue);
  bool startWrite(v8::Local<v8::Value> aValue);
  bool traverseObject(v8::Local<v8::Object> aObj, v8::Local<v8::Object> aTarget,
                      Kind aKind);
  bool traverseMap(v8::Local<v8::Object> aObj, v8::Local<v8::Map> aTarget);
  bool traverseSet(v8::Local<v8::Object> aObj, v8::Local<v8::Set> aTarget);
  bool traverseError(v8::Local<v8::Object> aObj, v8::Local<v8::Object> aTarget);
  bool reportDataCloneError(uint32_t aErrorId, const char* aArg = nullptr) {
    return ReportCloneError(context(), out.buf.callbacks_, aErrorId,
                            out.buf.closure_, aArg);
  }
  bool parseTransferable();
  bool writeHeader();
  bool writeTransferMap();
  bool transferOwnership();
  bool checkV8(v8::TryCatch& aTry) {
    if (aTry.HasCaught() && !aTry.HasTerminated()) {
      context()->setPending(aTry.Exception());
    }
    return false;
  }
  Kind classify(v8::Local<v8::Object> aObj, v8::Local<v8::Object>* aTarget);

  js::SCOutput out;
  const JSStructuredCloneCallbacks* callbacks;
  void* closure;
  const JS::CloneDataPolicy cloneDataPolicy;
  v8::Local<v8::Value> transferable;
  v8::Local<v8::Context> ctx;
  v8::Local<v8::Map> memory;
  std::vector<v8::Local<v8::Object>> objs;
  std::vector<Kind> objKinds;
  std::vector<size_t> counts;
  std::vector<v8::Local<v8::Value>> objectEntries;
  std::vector<v8::Local<v8::Value>> otherEntries;
  std::vector<v8::Local<v8::Object>> transferableObjects;
};

Kind JSStructuredCloneWriter::classify(v8::Local<v8::Object> aObj,
                                       v8::Local<v8::Object>* aTarget) {
  JSContext* cx = context();
  JSObject* cell = ObjectFromV8(cx, aObj);
  JSObject* unwrapped = cell;
  if (js::IsProxy(cell)) {
    unwrapped = js::CheckedUnwrapStatic(cell);
    if (!unwrapped || js::IsProxy(unwrapped)) {
      *aTarget = aObj;
      return Kind::Custom;
    }
  }
  if (!IsScriptClass(JS::GetClass(unwrapped))) {
    *aTarget = aObj;
    return Kind::Custom;
  }
  *aTarget = V8Of(cx, unwrapped);
  return KindOfV8(*aTarget);
}

bool JSStructuredCloneWriter::writeStringChars(uint32_t aTag,
                                               const void* aChars,
                                               uint32_t aLength, bool aLatin1) {
  uint32_t lengthAndBits = aLength | (uint32_t(aLatin1) << 31);
  if (!out.writePair(aTag, lengthAndBits)) {
    return false;
  }
  if (aLatin1) {
    return out.writeBytes(aChars, aLength);
  }
  return out.writeArray(static_cast<const uint16_t*>(aChars), aLength);
}

bool JSStructuredCloneWriter::writeString(uint32_t aTag,
                                          v8::Local<v8::String> aString) {
  uint32_t length = uint32_t(aString->Length());
  if (aString->ContainsOnlyOneByte()) {
    std::vector<uint8_t> chars(length);
    aString->WriteOneByte(isolate(), chars.data(), 0, int(length),
                          v8::String::NO_NULL_TERMINATION);
    return writeStringChars(aTag, chars.data(), length, true);
  }
  std::vector<uint16_t> chars(length);
  aString->Write(isolate(), chars.data(), 0, int(length),
                 v8::String::NO_NULL_TERMINATION);
  return writeStringChars(aTag, chars.data(), length, false);
}

bool JSStructuredCloneWriter::writeBigInt(uint32_t aTag,
                                          v8::Local<v8::BigInt> aBigInt) {
  int wordCount = aBigInt->WordCount();
  std::vector<uint64_t> words(size_t(wordCount) + 1);
  int sign = 0;
  aBigInt->ToWordsArray(&sign, &wordCount, words.data());
  uint32_t lengthAndSign =
      uint32_t(wordCount) | (static_cast<uint32_t>(sign != 0) << 31);
  if (!out.writePair(aTag, lengthAndSign)) {
    return false;
  }
  return out.writeArray(words.data(), size_t(wordCount));
}

bool JSStructuredCloneWriter::writeTypedArray(v8::Local<v8::Object> aView) {
  JSContext* cx = context();
  v8::Local<v8::TypedArray> view = aView.As<v8::TypedArray>();
  JSObject* cell = BufferObjectFromV8(cx, aView);
  JS::Scalar::Type type = JS_GetArrayBufferViewType(cell);
  v8::Local<v8::ArrayBuffer> buffer = view->Buffer();
  if (!out.writePair(SCTAG_TYPED_ARRAY_OBJECT, uint32_t(type))) {
    return false;
  }
  if (buffer->WasDetached()) {
    return reportDataCloneError(JS_SCERR_TYPED_ARRAY_DETACHED);
  }
  if (!out.write(uint64_t(view->Length()))) {
    return false;
  }
  if (!startWrite(buffer)) {
    return false;
  }
  return out.write(uint64_t(view->ByteOffset()));
}

bool JSStructuredCloneWriter::writeDataView(v8::Local<v8::Object> aView) {
  v8::Local<v8::DataView> view = aView.As<v8::DataView>();
  if (!out.writePair(SCTAG_DATA_VIEW_OBJECT, 0)) {
    return false;
  }
  v8::Local<v8::ArrayBuffer> buffer = view->Buffer();
  if (buffer->WasDetached()) {
    return reportDataCloneError(JS_SCERR_TYPED_ARRAY_DETACHED);
  }
  if (!out.write(uint64_t(view->ByteLength()))) {
    return false;
  }
  if (!startWrite(buffer)) {
    return false;
  }
  return out.write(uint64_t(view->ByteOffset()));
}

bool JSStructuredCloneWriter::writeArrayBuffer(v8::Local<v8::Object> aBuffer) {
  v8::Local<v8::ArrayBuffer> buffer = aBuffer.As<v8::ArrayBuffer>();
  bool resizable = buffer->GetBackingStore()->IsResizableByUserJavaScript();
  StructuredDataType type = resizable ? SCTAG_RESIZABLE_ARRAY_BUFFER_OBJECT
                                      : SCTAG_ARRAY_BUFFER_OBJECT;
  if (!out.writePair(type, 0)) {
    return false;
  }
  uint64_t byteLength = buffer->ByteLength();
  if (!out.write(byteLength)) {
    return false;
  }
  if (resizable && !out.write(uint64_t(buffer->MaxByteLength()))) {
    return false;
  }
  return out.writeBytes(buffer->Data(), size_t(byteLength));
}

bool JSStructuredCloneWriter::writeSharedArrayBuffer(
    v8::Local<v8::Object> aBuffer) {
  JSContext* cx = context();
  if (!cloneDataPolicy.areSharedMemoryObjectsAllowed()) {
    bool coop = cx->realm() &&
                cx->realm()->creationOptions.getCoopAndCoepEnabled();
    return reportDataCloneError(
        coop ? JS_SCERR_NOT_CLONABLE_WITH_COOP_COEP : JS_SCERR_NOT_CLONABLE,
        "SharedArrayBuffer");
  }
  out.sameProcessScopeRequired();
  if (out.scope() > JS::StructuredCloneScope::SameProcess) {
    JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                              JSMSG_SC_SHMEM_POLICY);
    return false;
  }
  v8::Local<v8::SharedArrayBuffer> sab = aBuffer.As<v8::SharedArrayBuffer>();
  js::SharedArrayRawBuffer* rawbuf =
      new js::SharedArrayRawBuffer(sab->GetBackingStore());
  bool acquired = out.buf.refsHeld_.acquire(cx, rawbuf);
  rawbuf->dropReference();
  if (!acquired) {
    return false;
  }
  StructuredDataType type = rawbuf->isGrowableJS()
                                ? SCTAG_GROWABLE_SHARED_ARRAY_BUFFER_OBJECT
                                : SCTAG_SHARED_ARRAY_BUFFER_OBJECT;
  intptr_t p = reinterpret_cast<intptr_t>(rawbuf);
  uint64_t byteLength = rawbuf->isGrowableJS()
                            ? uint64_t(sab->MaxByteLength())
                            : uint64_t(sab->ByteLength());
  if (!(out.writePair(type, 0) &&
        out.writeBytes(&byteLength, sizeof(byteLength)) &&
        out.writeBytes(&p, sizeof(p)))) {
    return false;
  }
  if (callbacks && callbacks->sabCloned &&
      !callbacks->sabCloned(cx, false, closure)) {
    return false;
  }
  return true;
}

bool JSStructuredCloneWriter::startObject(v8::Local<v8::Object> aObj,
                                          bool* aBackref) {
  v8::Local<v8::Value> index;
  if (memory->Has(ctx, aObj).FromMaybe(false) &&
      memory->Get(ctx, aObj).ToLocal(&index)) {
    *aBackref = true;
    return out.writePair(SCTAG_BACK_REFERENCE_OBJECT,
                         index.As<v8::Uint32>()->Value());
  }
  *aBackref = false;
  size_t count = memory->Size();
  if (count >= UINT32_MAX - 1) {
    JS_ReportErrorNumberASCII(context(), js::GetErrorMessage, nullptr,
                              JSMSG_NEED_DIET, "object graph to serialize");
    return false;
  }
  if (memory
          ->Set(ctx, aObj,
                v8::Integer::NewFromUnsigned(isolate(), uint32_t(count)))
          .IsEmpty()) {
    JS_ReportOutOfMemory(context());
    return false;
  }
  return true;
}

bool JSStructuredCloneWriter::traverseObject(v8::Local<v8::Object> aObj,
                                             v8::Local<v8::Object> aTarget,
                                             Kind aKind) {
  v8::TryCatch tryCatch(isolate());
  v8::Local<v8::Array> keys;
  if (!aObj->GetOwnPropertyNames(
               ctx,
               v8::PropertyFilter(v8::ONLY_ENUMERABLE | v8::SKIP_SYMBOLS),
               v8::KeyConversionMode::kKeepNumbers)
           .ToLocal(&keys)) {
    return checkV8(tryCatch);
  }
  uint32_t length = keys->Length();
  for (uint32_t i = length; i > 0; --i) {
    v8::Local<v8::Value> key;
    if (!keys->Get(ctx, i - 1).ToLocal(&key)) {
      return checkV8(tryCatch);
    }
    objectEntries.push_back(key);
  }
  objs.push_back(aObj);
  objKinds.push_back(Kind::Object);
  counts.push_back(length);
  if (aKind == Kind::Array) {
    return out.writePair(SCTAG_ARRAY_OBJECT,
                         NativeEndian::swapToLittleEndian(
                             aTarget.As<v8::Array>()->Length()));
  }
  return out.writePair(SCTAG_OBJECT_OBJECT, 0);
}

bool JSStructuredCloneWriter::traverseMap(v8::Local<v8::Object> aObj,
                                          v8::Local<v8::Map> aTarget) {
  v8::Local<v8::Array> entries = aTarget->AsArray();
  uint32_t length = entries->Length();
  for (uint32_t i = length; i > 0; --i) {
    v8::Local<v8::Value> entry;
    if (!entries->Get(ctx, i - 1).ToLocal(&entry)) {
      return false;
    }
    otherEntries.push_back(entry);
  }
  objs.push_back(aObj);
  objKinds.push_back(Kind::Map);
  counts.push_back(length);
  return out.writePair(SCTAG_MAP_OBJECT, 0);
}

bool JSStructuredCloneWriter::traverseSet(v8::Local<v8::Object> aObj,
                                          v8::Local<v8::Set> aTarget) {
  v8::Local<v8::Array> keys = aTarget->AsArray();
  uint32_t length = keys->Length();
  for (uint32_t i = length; i > 0; --i) {
    v8::Local<v8::Value> key;
    if (!keys->Get(ctx, i - 1).ToLocal(&key)) {
      return false;
    }
    otherEntries.push_back(key);
  }
  objs.push_back(aObj);
  objKinds.push_back(Kind::Set);
  counts.push_back(length);
  return out.writePair(SCTAG_SET_OBJECT, 0);
}

bool JSStructuredCloneWriter::traverseError(v8::Local<v8::Object> aObj,
                                            v8::Local<v8::Object> aTarget) {
  v8::Isolate* iso = isolate();
  v8::TryCatch tryCatch(iso);
  v8::Local<v8::Value> name;
  if (!aObj->Get(ctx, Literal(iso, "name")).ToLocal(&name)) {
    return checkV8(tryCatch);
  }
  JSExnType type = JSEXN_ERR;
  if (name->IsString()) {
    v8::String::Utf8Value text(iso, name);
    struct {
      const char* name;
      JSExnType type;
    } const kinds[] = {{"Error", JSEXN_ERR},
                       {"EvalError", JSEXN_EVALERR},
                       {"RangeError", JSEXN_RANGEERR},
                       {"ReferenceError", JSEXN_REFERENCEERR},
                       {"SyntaxError", JSEXN_SYNTAXERR},
                       {"TypeError", JSEXN_TYPEERR},
                       {"URIError", JSEXN_URIERR},
                       {"AggregateError", JSEXN_AGGREGATEERR}};
    for (const auto& k : kinds) {
      if (*text && strcmp(*text, k.name) == 0) {
        type = k.type;
        break;
      }
    }
  }
  auto dataValue = [&](const char* aKey, v8::Local<v8::Value>* aOut,
                       bool* aFound) -> bool {
    *aFound = false;
    v8::Local<v8::Value> desc;
    if (!aObj->GetOwnPropertyDescriptor(ctx, Literal(iso, aKey))
             .ToLocal(&desc)) {
      return false;
    }
    if (!desc->IsObject()) {
      return true;
    }
    v8::Local<v8::Object> d = desc.As<v8::Object>();
    v8::Maybe<bool> hasValue = d->Has(ctx, Literal(iso, "value"));
    if (hasValue.IsNothing()) {
      return false;
    }
    if (!hasValue.FromJust()) {
      return true;
    }
    if (!d->Get(ctx, Literal(iso, "value")).ToLocal(aOut)) {
      return false;
    }
    *aFound = true;
    return true;
  };
  v8::Local<v8::Value> messageValue;
  bool hasMessage;
  if (!dataValue("message", &messageValue, &hasMessage)) {
    return checkV8(tryCatch);
  }
  v8::Local<v8::String> message;
  if (hasMessage && !messageValue->ToString(ctx).ToLocal(&message)) {
    return checkV8(tryCatch);
  }
  objs.push_back(aObj);
  objKinds.push_back(Kind::Error);
  otherEntries.push_back(v8::Null(iso));
  if (type == JSEXN_AGGREGATEERR) {
    v8::Local<v8::Value> errors;
    if (!aObj->Get(ctx, Literal(iso, "errors")).ToLocal(&errors)) {
      return checkV8(tryCatch);
    }
    otherEntries.push_back(errors);
  } else {
    otherEntries.push_back(v8::Null(iso));
  }
  v8::Local<v8::Value> cause;
  bool hasCause;
  if (!dataValue("cause", &cause, &hasCause)) {
    return checkV8(tryCatch);
  }
  otherEntries.push_back(hasCause ? cause : v8::Local<v8::Value>(v8::Null(iso)));
  counts.push_back(3);
  if (!out.writePair(SCTAG_ERROR_OBJECT, type)) {
    return false;
  }
  if (!writePrimitive(message.IsEmpty() ? v8::Local<v8::Value>(v8::Null(iso))
                                        : v8::Local<v8::Value>(message))) {
    return false;
  }
  if (!writePrimitive(v8::Boolean::New(iso, hasCause))) {
    return false;
  }
  v8::Local<v8::Message> location = v8::Exception::CreateMessage(iso, aTarget);
  v8::Local<v8::Value> fileName = location->GetScriptResourceName();
  v8::Local<v8::String> fileNameString;
  if (!fileName->IsString()) {
    fileNameString = v8::String::Empty(iso);
  } else {
    fileNameString = fileName.As<v8::String>();
  }
  if (!writePrimitive(fileNameString)) {
    return false;
  }
  int line = location->GetLineNumber(ctx).FromMaybe(0);
  int column = location->GetStartColumn(ctx).FromMaybe(0) + 1;
  return writePrimitive(v8::Integer::New(iso, line)) &&
         writePrimitive(v8::Integer::New(iso, column));
}

bool JSStructuredCloneWriter::writePrimitive(v8::Local<v8::Value> aValue) {
  if (aValue->IsString()) {
    return writeString(SCTAG_STRING, aValue.As<v8::String>());
  }
  if (aValue->IsInt32()) {
    return out.writePair(SCTAG_INT32,
                         uint32_t(aValue.As<v8::Int32>()->Value()));
  }
  if (aValue->IsNumber()) {
    return out.writeDouble(aValue.As<v8::Number>()->Value());
  }
  if (aValue->IsBoolean()) {
    return out.writePair(SCTAG_BOOLEAN, aValue->IsTrue());
  }
  if (aValue->IsNull()) {
    return out.writePair(SCTAG_NULL, 0);
  }
  if (aValue->IsUndefined()) {
    return out.writePair(SCTAG_UNDEFINED, 0);
  }
  if (aValue->IsBigInt()) {
    return writeBigInt(SCTAG_BIGINT, aValue.As<v8::BigInt>());
  }
  return reportDataCloneError(JS_SCERR_UNSUPPORTED_TYPE);
}

bool JSStructuredCloneWriter::startWrite(v8::Local<v8::Value> aValue) {
  if (!aValue->IsObject()) {
    return writePrimitive(aValue);
  }
  v8::Local<v8::Object> obj = aValue.As<v8::Object>();
  bool backref;
  if (!startObject(obj, &backref)) {
    return false;
  }
  if (backref) {
    return true;
  }
  v8::Local<v8::Object> target;
  Kind kind = classify(obj, &target);
  switch (kind) {
    case Kind::Object:
    case Kind::Array:
      return traverseObject(obj, target, kind);
    case Kind::Number:
      return out.writePair(SCTAG_NUMBER_OBJECT, 0) &&
             out.writeDouble(target.As<v8::NumberObject>()->ValueOf());
    case Kind::String:
      return writeString(SCTAG_STRING_OBJECT,
                         target.As<v8::StringObject>()->ValueOf());
    case Kind::Boolean:
      return out.writePair(SCTAG_BOOLEAN_OBJECT,
                           target.As<v8::BooleanObject>()->ValueOf());
    case Kind::RegExp: {
      v8::Local<v8::RegExp> re = target.As<v8::RegExp>();
      int v8Flags = re->GetFlags();
      uint32_t flags = 0;
      if (v8Flags & v8::RegExp::kGlobal) {
        flags |= JS::RegExpFlag::Global;
      }
      if (v8Flags & v8::RegExp::kIgnoreCase) {
        flags |= JS::RegExpFlag::IgnoreCase;
      }
      if (v8Flags & v8::RegExp::kMultiline) {
        flags |= JS::RegExpFlag::Multiline;
      }
      if (v8Flags & v8::RegExp::kSticky) {
        flags |= JS::RegExpFlag::Sticky;
      }
      if (v8Flags & v8::RegExp::kUnicode) {
        flags |= JS::RegExpFlag::Unicode;
      }
      if (v8Flags & v8::RegExp::kDotAll) {
        flags |= JS::RegExpFlag::DotAll;
      }
      if (v8Flags & v8::RegExp::kHasIndices) {
        flags |= JS::RegExpFlag::HasIndices;
      }
      if (v8Flags & v8::RegExp::kUnicodeSets) {
        flags |= JS::RegExpFlag::UnicodeSets;
      }
      return out.writePair(SCTAG_REGEXP_OBJECT, flags) &&
             writeString(SCTAG_STRING, re->GetSource());
    }
    case Kind::ArrayBuffer:
      if (!target.As<v8::ArrayBuffer>()->WasDetached()) {
        return writeArrayBuffer(target);
      }
      break;
    case Kind::SharedArrayBuffer:
      return writeSharedArrayBuffer(target);
    case Kind::Date:
      return out.writePair(SCTAG_DATE_OBJECT, 0) &&
             out.writeDouble(target.As<v8::Date>()->ValueOf());
    case Kind::Set:
      return traverseSet(obj, target.As<v8::Set>());
    case Kind::Map:
      return traverseMap(obj, target.As<v8::Map>());
    case Kind::Error:
      return traverseError(obj, target);
    case Kind::BigInt:
      return writeBigInt(SCTAG_BIGINT_OBJECT,
                         target.As<v8::BigIntObject>()->ValueOf());
    case Kind::TypedArray:
      return writeTypedArray(target);
    case Kind::DataView:
      return writeDataView(target);
    case Kind::Custom:
      break;
  }
  if (out.buf.callbacks_ && out.buf.callbacks_->write) {
    bool sameProcessScopeRequired = false;
    JS::RootedObject cell(context(), ObjectFromV8(context(), obj));
    if (!out.buf.callbacks_->write(context(), this, cell,
                                   &sameProcessScopeRequired,
                                   out.buf.closure_)) {
      return false;
    }
    if (sameProcessScopeRequired) {
      out.sameProcessScopeRequired();
    }
    return true;
  }
  return reportDataCloneError(JS_SCERR_UNSUPPORTED_TYPE);
}

bool JSStructuredCloneWriter::parseTransferable() {
  if (transferable.IsEmpty() || transferable->IsNull() ||
      transferable->IsUndefined()) {
    return true;
  }
  if (!transferable->IsObject()) {
    return reportDataCloneError(JS_SCERR_TRANSFERABLE);
  }
  JSContext* cx = context();
  JS::RootedObject array(cx,
                         ObjectFromV8(cx, transferable.As<v8::Object>()));
  bool isArray;
  if (!JS::IsArrayObject(cx, array, &isArray)) {
    return false;
  }
  if (!isArray) {
    return reportDataCloneError(JS_SCERR_TRANSFERABLE);
  }
  uint32_t length;
  if (!JS::GetArrayLength(cx, array, &length)) {
    return false;
  }
  JS::RootedValue v(cx);
  JS::RootedObject tObj(cx);
  for (uint32_t i = 0; i < length; ++i) {
    if (!JS_GetElement(cx, array, i, &v)) {
      return false;
    }
    if (!v.isObject()) {
      return reportDataCloneError(JS_SCERR_TRANSFERABLE);
    }
    tObj = &v.toObject();
    JS::RootedObject unwrapped(cx, js::CheckedUnwrapStatic(tObj));
    if (!unwrapped) {
      js::ReportAccessDenied(cx);
      return false;
    }
    v8::Local<v8::Object> target = V8Of(cx, unwrapped);
    if (target->IsSharedArrayBuffer()) {
      return reportDataCloneError(JS_SCERR_SHMEM_TRANSFERABLE);
    }
    if (target->IsWasmMemoryObject()) {
      v8::Local<v8::ArrayBuffer> memoryBuffer =
          target.As<v8::WasmMemoryObject>()->Buffer();
      if (memoryBuffer->GetBackingStore()->IsShared()) {
        return reportDataCloneError(JS_SCERR_SHMEM_TRANSFERABLE);
      }
    }
    if (!target->IsArrayBuffer()) {
      if (!out.buf.callbacks_ || !out.buf.callbacks_->canTransfer) {
        return reportDataCloneError(JS_SCERR_TRANSFERABLE);
      }
      JSAutoRealm ar(cx, unwrapped);
      bool sameProcessScopeRequired = false;
      if (!out.buf.callbacks_->canTransfer(
              cx, unwrapped, &sameProcessScopeRequired, out.buf.closure_)) {
        return reportDataCloneError(JS_SCERR_TRANSFERABLE);
      }
      if (sameProcessScopeRequired) {
        out.sameProcessScopeRequired();
      }
    }
    v8::Local<v8::Object> element = V8Of(cx, tObj);
    for (v8::Local<v8::Object>& seen : transferableObjects) {
      if (seen == element) {
        return reportDataCloneError(JS_SCERR_DUP_TRANSFERABLE);
      }
    }
    transferableObjects.push_back(element);
  }
  return true;
}

bool JSStructuredCloneWriter::writeHeader() {
  return out.writePair(SCTAG_HEADER, uint32_t(out.scope()));
}

bool JSStructuredCloneWriter::writeTransferMap() {
  if (transferableObjects.empty()) {
    return true;
  }
  if (!out.writePair(SCTAG_TRANSFER_MAP_HEADER, uint32_t(SCTAG_TM_UNREAD)) ||
      !out.write(transferableObjects.size())) {
    return false;
  }
  for (v8::Local<v8::Object>& obj : transferableObjects) {
    size_t count = memory->Size();
    if (memory
            ->Set(ctx, obj,
                  v8::Integer::NewFromUnsigned(isolate(), uint32_t(count)))
            .IsEmpty()) {
      JS_ReportOutOfMemory(context());
      return false;
    }
    if (!out.writePair(SCTAG_TRANSFER_MAP_PENDING_ENTRY,
                       JS::SCTAG_TMO_UNFILLED) ||
        !out.write(0) || !out.write(0)) {
      return false;
    }
  }
  return true;
}

bool JSStructuredCloneWriter::transferOwnership() {
  if (transferableObjects.empty()) {
    return true;
  }
  JSContext* cx = context();
  auto point = out.iter();
  point++;
  point++;
  point++;
  JS::StructuredCloneScope scope = out.scope();
  for (v8::Local<v8::Object>& element : transferableObjects) {
    JS::RootedObject obj(cx, ObjectFromV8(cx, element));
    JS::RootedObject unwrapped(cx, js::CheckedUnwrapStatic(obj));
    if (!unwrapped) {
      js::ReportAccessDenied(cx);
      return false;
    }
    v8::Local<v8::Object> target = V8Of(cx, unwrapped);
    uint32_t tag;
    JS::TransferableOwnership ownership;
    void* content;
    uint64_t extraData;
    if (target->IsArrayBuffer()) {
      v8::Local<v8::ArrayBuffer> buffer = target.As<v8::ArrayBuffer>();
      if (buffer->WasDetached()) {
        return reportDataCloneError(JS_SCERR_TYPED_ARRAY_DETACHED);
      }
      bool resizable = buffer->GetBackingStore()->IsResizableByUserJavaScript();
      if (scope == JS::StructuredCloneScope::DifferentProcess ||
          scope == JS::StructuredCloneScope::DifferentProcessForIndexedDB ||
          resizable) {
        size_t pointOffset = out.offset(point);
        tag = SCTAG_TRANSFER_MAP_STORED_ARRAY_BUFFER;
        ownership = JS::SCTAG_TMO_UNOWNED;
        content = nullptr;
        extraData = out.tell() - pointOffset;
        if (!writeArrayBuffer(target)) {
          JS_ReportOutOfMemory(cx);
          return false;
        }
        point = out.iter();
        point += pointOffset;
        if (!JS::DetachArrayBuffer(cx, unwrapped)) {
          return false;
        }
      } else {
        size_t nbytes = buffer->ByteLength();
        content = JS::StealArrayBufferContents(cx, unwrapped);
        if (!content) {
          return false;
        }
        ownership = JS::SCTAG_TMO_ALLOC_DATA;
        tag = SCTAG_TRANSFER_MAP_ARRAY_BUFFER;
        extraData = nbytes;
      }
    } else {
      if (!out.buf.callbacks_ || !out.buf.callbacks_->writeTransfer) {
        return reportDataCloneError(JS_SCERR_TRANSFERABLE);
      }
      if (!out.buf.callbacks_->writeTransfer(cx, obj, out.buf.closure_, &tag,
                                             &ownership, &content,
                                             &extraData)) {
        return false;
      }
    }
    point.write(NativeEndian::swapToLittleEndian(PairToUInt64(tag, ownership)));
    MOZ_ALWAYS_TRUE(point.advance());
    point.write(
        NativeEndian::swapToLittleEndian(reinterpret_cast<uint64_t>(content)));
    MOZ_ALWAYS_TRUE(point.advance());
    point.write(NativeEndian::swapToLittleEndian(extraData));
    MOZ_ALWAYS_TRUE(point.advance());
  }
  return true;
}

bool JSStructuredCloneWriter::write(v8::Local<v8::Value> aValue) {
  if (!startWrite(aValue)) {
    return false;
  }
  v8::Isolate* iso = isolate();
  while (!counts.empty()) {
    v8::Local<v8::Object> obj = objs.back();
    Kind kind = objKinds.back();
    if (counts.back()) {
      counts.back()--;
      if (kind == Kind::Map) {
        v8::Local<v8::Value> key = otherEntries.back();
        otherEntries.pop_back();
        counts.back()--;
        v8::Local<v8::Value> val = otherEntries.back();
        otherEntries.pop_back();
        if (!startWrite(key) || !startWrite(val)) {
          return false;
        }
      } else if (kind == Kind::Set) {
        v8::Local<v8::Value> key = otherEntries.back();
        otherEntries.pop_back();
        if (!startWrite(key)) {
          return false;
        }
      } else if (kind == Kind::Error) {
        v8::Local<v8::Value> cause = otherEntries.back();
        otherEntries.pop_back();
        counts.back()--;
        v8::Local<v8::Value> errors = otherEntries.back();
        otherEntries.pop_back();
        counts.back()--;
        v8::Local<v8::Value> stack = otherEntries.back();
        otherEntries.pop_back();
        if (!startWrite(cause) || !startWrite(errors) || !startWrite(stack)) {
          return false;
        }
      } else {
        v8::Local<v8::Value> key = objectEntries.back();
        objectEntries.pop_back();
        v8::TryCatch tryCatch(iso);
        v8::Local<v8::Value> keyValue = key;
        v8::Maybe<bool> found = v8::Nothing<bool>();
        if (key->IsUint32()) {
          uint32_t index = key.As<v8::Uint32>()->Value();
          found = obj->HasOwnProperty(ctx, index);
          if (index > uint32_t(INT32_MAX)) {
            v8::Local<v8::String> keyString;
            if (!key->ToString(ctx).ToLocal(&keyString)) {
              return checkV8(tryCatch);
            }
            keyValue = keyString;
          }
        } else {
          v8::Local<v8::String> keyString;
          if (!key->ToString(ctx).ToLocal(&keyString)) {
            return checkV8(tryCatch);
          }
          key = keyString;
          keyValue = keyString;
          found = obj->HasOwnProperty(ctx, keyString);
        }
        if (found.IsNothing()) {
          return checkV8(tryCatch);
        }
        if (!found.FromJust()) {
          continue;
        }
        v8::Local<v8::Value> val;
        if (!obj->Get(ctx, key).ToLocal(&val)) {
          return checkV8(tryCatch);
        }
        if (!writePrimitive(keyValue) || !startWrite(val)) {
          return false;
        }
      }
    } else {
      if (!out.writePair(SCTAG_END_OF_KEYS, 0)) {
        return false;
      }
      objs.pop_back();
      objKinds.pop_back();
      counts.pop_back();
    }
  }
  memory->Clear();
  return transferOwnership();
}

namespace {

enum class ReadKind : uint8_t { Object, Map, Set, Error, SavedFrame };

}

struct JSStructuredCloneReader {
 public:
  JSStructuredCloneReader(js::SCInput& aIn, JS::StructuredCloneScope aScope,
                          const JS::CloneDataPolicy& aPolicy,
                          const JSStructuredCloneCallbacks* aCallbacks,
                          void* aClosure)
      : in(aIn),
        allowedScope(aScope),
        cloneDataPolicy(aPolicy),
        callbacks(aCallbacks),
        closure(aClosure) {
    MOZ_RELEASE_ASSERT(!(aScope == JS::StructuredCloneScope::DifferentProcess &&
                         aPolicy.areSharedMemoryObjectsAllowed()));
    ctx = CurrentV8Context(aIn.context());
  }

  js::SCInput& input() { return in; }
  bool read(v8::Local<v8::Value>* aVp);

  JSContext* context() { return in.context(); }
  v8::Isolate* isolate() { return in.context()->isolate; }

  bool checkV8(v8::TryCatch& aTry) {
    if (aTry.HasCaught() && !aTry.HasTerminated()) {
      context()->setPending(aTry.Exception());
    }
    return false;
  }

  bool readHeader();
  bool readTransferMap();
  bool readUint32(uint32_t* aNum);
  bool readString(uint32_t aData, v8::Local<v8::String>* aOut);
  bool readBigInt(uint32_t aData, v8::Local<v8::BigInt>* aOut);
  bool readTypedArray(uint32_t aArrayType, uint64_t aNelems,
                      v8::Local<v8::Value>* aVp, bool aV1Read = false);
  bool readDataView(uint64_t aByteLength, v8::Local<v8::Value>* aVp);
  bool readArrayBuffer(StructuredDataType aType, uint32_t aData,
                       v8::Local<v8::Value>* aVp);
  bool readV1ArrayBuffer(uint32_t aArrayType, uint32_t aNelems,
                         v8::Local<v8::Value>* aVp);
  bool readSharedArrayBuffer(StructuredDataType aType,
                             v8::Local<v8::Value>* aVp);
  bool readSavedFrameHeader(uint32_t aPrincipalsTag,
                            v8::Local<v8::Object>* aOut);
  bool readErrorHeader(uint32_t aType, v8::Local<v8::Object>* aOut);
  bool readErrorFields(v8::Local<v8::Object> aError,
                       v8::Local<v8::Value> aCause, bool* aState);
  bool readObjectField(v8::Local<v8::Object> aObj, v8::Local<v8::Value> aKey);
  bool startReadUnchecked(v8::Local<v8::Value>* aVp, bool* aUsedBackRef);
  bool startRead(v8::Local<v8::Value>* aVp);
  bool primitiveToObject(v8::Local<v8::Value>* aVp);
  void pushObject(v8::Local<v8::Object> aObj, ReadKind aKind) {
    objs.push_back(aObj);
    objKinds.push_back(aKind);
  }

  js::SCInput& in;
  JS::StructuredCloneScope allowedScope;
  const JS::CloneDataPolicy cloneDataPolicy;
  const JSStructuredCloneCallbacks* callbacks;
  void* closure;
  v8::Local<v8::Context> ctx;
  std::vector<v8::Local<v8::Object>> objs;
  std::vector<ReadKind> objKinds;
  std::vector<std::pair<v8::Local<v8::Object>, bool>> objState;
  std::vector<v8::Local<v8::Value>> allObjs;
  std::vector<v8::Local<v8::Object>> savedFrames;
  std::vector<std::pair<v8::Local<v8::Object>, v8::Local<v8::Object>>>
      errorStacks;
  std::vector<v8::Local<v8::Object>> errorsWithCause;
  mozilla::Maybe<js::SCInput::BufferIterator> tailStartPos;
  mozilla::Maybe<js::SCInput::BufferIterator> tailEndPos;
};

bool JSStructuredCloneReader::readString(uint32_t aData,
                                         v8::Local<v8::String>* aOut) {
  uint32_t nchars = aData & ((1u << 30) - 1);
  bool latin1 = aData & (1u << 31);
  bool hasBuffer = aData & (1u << 30);
  if (nchars > uint32_t(v8::String::kMaxLength)) {
    ReportBadData(context(), "string length");
    return false;
  }
  v8::MaybeLocal<v8::String> result;
  if (hasBuffer) {
    if (allowedScope > JS::StructuredCloneScope::SameProcess) {
      ReportBadData(context(), "invalid scope for string buffer");
      return false;
    }
    uintptr_t p;
    if (!in.readBytes(&p, sizeof(p))) {
      return false;
    }
    RefPtr<mozilla::StringBuffer> buffer(
        reinterpret_cast<mozilla::StringBuffer*>(p));
    result = latin1 ? v8::String::NewFromOneByte(
                          isolate(), static_cast<uint8_t*>(buffer->Data()),
                          v8::NewStringType::kNormal, int(nchars))
                    : v8::String::NewFromTwoByte(
                          isolate(), static_cast<uint16_t*>(buffer->Data()),
                          v8::NewStringType::kNormal, int(nchars));
  } else if (latin1) {
    std::vector<uint8_t> chars(nchars);
    if (!in.readArray(chars.data(), nchars)) {
      return false;
    }
    result = v8::String::NewFromOneByte(isolate(), chars.data(),
                                        v8::NewStringType::kNormal, int(nchars));
  } else {
    std::vector<uint16_t> chars(nchars);
    if (!in.readArray(chars.data(), nchars)) {
      return false;
    }
    result = v8::String::NewFromTwoByte(isolate(), chars.data(),
                                        v8::NewStringType::kNormal, int(nchars));
  }
  if (!result.ToLocal(aOut)) {
    JS_ReportOutOfMemory(context());
    return false;
  }
  return true;
}

bool JSStructuredCloneReader::readBigInt(uint32_t aData,
                                         v8::Local<v8::BigInt>* aOut) {
  size_t length = aData & ((1u << 31) - 1);
  bool negative = aData & (1u << 31);
  if (length == 0) {
    *aOut = v8::BigInt::New(isolate(), 0);
    return true;
  }
  if (length > size_t(INT32_MAX) / 64) {
    ReportBadData(context(), "BigInt length");
    return false;
  }
  std::vector<uint64_t> words(length);
  if (!in.readArray(words.data(), length)) {
    return false;
  }
  v8::TryCatch tryCatch(isolate());
  if (!v8::BigInt::NewFromWords(ctx, negative ? 1 : 0, int(length),
                                words.data())
           .ToLocal(aOut)) {
    return checkV8(tryCatch);
  }
  return true;
}

bool JSStructuredCloneReader::readUint32(uint32_t* aNum) {
  v8::Local<v8::Value> value;
  if (!startRead(&value)) {
    return false;
  }
  if (!value->IsInt32()) {
    ReportBadData(context(), "integer required");
    return false;
  }
  *aNum = uint32_t(value.As<v8::Int32>()->Value());
  return true;
}

bool JSStructuredCloneReader::readTypedArray(uint32_t aArrayType,
                                             uint64_t aNelems,
                                             v8::Local<v8::Value>* aVp,
                                             bool aV1Read) {
  JSContext* cx = context();
  if (aArrayType > (aV1Read ? uint32_t(JS::Scalar::Uint8Clamped)
                            : uint32_t(JS::Scalar::Float16))) {
    ReportBadData(cx, "unhandled typed array element type");
    return false;
  }
  size_t placeholderIndex = allObjs.size();
  allObjs.push_back(v8::Undefined(isolate()));
  bool isAutoLength = aNelems == uint64_t(-1);
  if (isAutoLength) {
    aNelems = 0;
  }
  v8::Local<v8::Value> v;
  uint64_t byteOffset;
  if (aV1Read) {
    if (!readV1ArrayBuffer(aArrayType, uint32_t(aNelems), &v)) {
      return false;
    }
    byteOffset = 0;
  } else {
    if (!startRead(&v) || !in.read(&byteOffset)) {
      return false;
    }
  }
  if (aNelems > kByteLengthLimit || byteOffset > kByteLengthLimit) {
    ReportBadData(cx, "invalid typed array length or offset");
    return false;
  }
  if (!v->IsArrayBuffer() && !v->IsSharedArrayBuffer()) {
    ReportBadData(cx, "typed array must be backed by an ArrayBuffer");
    return false;
  }
  JS::RootedObject buffer(cx, BufferObjectFromV8(cx, v.As<v8::Object>()));
  int64_t length = isAutoLength ? -1 : int64_t(aNelems);
  JS::RootedObject obj(cx, NewTypedArrayWithBuffer(
                               cx, JS::Scalar::Type(aArrayType), buffer,
                               size_t(byteOffset), length));
  if (!obj) {
    return false;
  }
  *aVp = V8Of(cx, obj);
  allObjs[placeholderIndex] = *aVp;
  return true;
}

bool JSStructuredCloneReader::readDataView(uint64_t aByteLength,
                                           v8::Local<v8::Value>* aVp) {
  JSContext* cx = context();
  size_t placeholderIndex = allObjs.size();
  allObjs.push_back(v8::Undefined(isolate()));
  v8::Local<v8::Value> v;
  if (!startRead(&v)) {
    return false;
  }
  if (!v->IsArrayBuffer() && !v->IsSharedArrayBuffer()) {
    ReportBadData(cx, "DataView must be backed by an ArrayBuffer");
    return false;
  }
  uint64_t byteOffset;
  if (!in.read(&byteOffset)) {
    return false;
  }
  bool isAutoLength = aByteLength == uint64_t(-1);
  if (isAutoLength) {
    aByteLength = 0;
  }
  if (aByteLength > kByteLengthLimit || byteOffset > kByteLengthLimit) {
    ReportBadData(cx, "invalid DataView length or offset");
    return false;
  }
  JS::RootedObject buffer(cx, BufferObjectFromV8(cx, v.As<v8::Object>()));
  JS::RootedObject obj(
      cx, NewDataViewObject(cx, buffer, size_t(byteOffset),
                            isAutoLength ? mozilla::Nothing()
                                         : mozilla::Some(size_t(aByteLength))));
  if (!obj) {
    return false;
  }
  *aVp = V8Of(cx, obj);
  allObjs[placeholderIndex] = *aVp;
  return true;
}

bool JSStructuredCloneReader::readArrayBuffer(StructuredDataType aType,
                                              uint32_t aData,
                                              v8::Local<v8::Value>* aVp) {
  uint64_t nbytes = 0;
  uint64_t maxbytes = 0;
  if (aType == SCTAG_ARRAY_BUFFER_OBJECT ||
      aType == SCTAG_IMMUTABLE_ARRAY_BUFFER_OBJECT) {
    if (!in.read(&nbytes)) {
      return false;
    }
  } else if (aType == SCTAG_RESIZABLE_ARRAY_BUFFER_OBJECT) {
    if (!in.read(&nbytes) || !in.read(&maxbytes)) {
      return false;
    }
  } else {
    nbytes = aData;
  }
  if (nbytes > kByteLengthLimit || maxbytes > kByteLengthLimit ||
      (aType == SCTAG_RESIZABLE_ARRAY_BUFFER_OBJECT && nbytes > maxbytes)) {
    JS_ReportErrorNumberASCII(context(), js::GetErrorMessage, nullptr,
                              JSMSG_BAD_ARRAY_LENGTH);
    return false;
  }
  v8::Local<v8::ArrayBuffer> buffer;
  if (aType == SCTAG_RESIZABLE_ARRAY_BUFFER_OBJECT) {
    buffer = v8::ArrayBuffer::New(
        isolate(), std::shared_ptr<v8::BackingStore>(
                       v8::ArrayBuffer::NewResizableBackingStore(
                           size_t(nbytes), size_t(maxbytes))));
  } else {
    buffer = v8::ArrayBuffer::New(isolate(), size_t(nbytes));
  }
  *aVp = buffer;
  return in.readArray(static_cast<uint8_t*>(buffer->Data()), size_t(nbytes));
}

bool JSStructuredCloneReader::readSharedArrayBuffer(StructuredDataType aType,
                                                    v8::Local<v8::Value>* aVp) {
  JSContext* cx = context();
  if (!cloneDataPolicy.areIntraClusterClonableSharedObjectsAllowed() ||
      !cloneDataPolicy.areSharedMemoryObjectsAllowed()) {
    bool coop = cx->realm() &&
                cx->realm()->creationOptions.getCoopAndCoepEnabled();
    return ReportCloneError(
        cx, callbacks,
        coop ? JS_SCERR_NOT_CLONABLE_WITH_COOP_COEP : JS_SCERR_NOT_CLONABLE,
        closure, "SharedArrayBuffer");
  }
  uint64_t byteLength;
  if (!in.readBytes(&byteLength, sizeof(byteLength))) {
    return in.reportTruncated();
  }
  if (byteLength > kByteLengthLimit) {
    JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                              JSMSG_BAD_ARRAY_LENGTH);
    return false;
  }
  intptr_t p;
  if (!in.readBytes(&p, sizeof(p))) {
    return in.reportTruncated();
  }
  bool isGrowable = aType == SCTAG_GROWABLE_SHARED_ARRAY_BUFFER_OBJECT;
  js::SharedArrayRawBuffer* rawbuf =
      reinterpret_cast<js::SharedArrayRawBuffer*>(p);
  MOZ_RELEASE_ASSERT(isGrowable == rawbuf->isGrowableJS());
  if (cx->realm() &&
      !cx->realm()->creationOptions.getSharedMemoryAndAtomicsEnabled()) {
    JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                              JSMSG_SC_SAB_DISABLED);
    return false;
  }
  if (!rawbuf->addReference()) {
    JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                              JSMSG_SC_SAB_REFCNT_OFLO);
    return false;
  }
  v8::Local<v8::SharedArrayBuffer> sab =
      v8::SharedArrayBuffer::New(isolate(), rawbuf->store());
  rawbuf->dropReference();
  if (callbacks && callbacks->sabCloned &&
      !callbacks->sabCloned(cx, true, closure)) {
    return false;
  }
  allObjs.push_back(sab);
  *aVp = sab;
  return true;
}

bool JSStructuredCloneReader::readV1ArrayBuffer(uint32_t aArrayType,
                                                uint32_t aNelems,
                                                v8::Local<v8::Value>* aVp) {
  if (aArrayType > JS::Scalar::Uint8Clamped) {
    ReportBadData(context(), "invalid TypedArray type");
    return false;
  }
  mozilla::CheckedInt<size_t> nbytes =
      mozilla::CheckedInt<size_t>(aNelems) *
      ElementSize(JS::Scalar::Type(aArrayType));
  if (!nbytes.isValid() || nbytes.value() > UINT32_MAX) {
    ReportBadData(context(), "invalid typed array size");
    return false;
  }
  v8::Local<v8::ArrayBuffer> buffer =
      v8::ArrayBuffer::New(isolate(), nbytes.value());
  *aVp = buffer;
  void* data = buffer->Data();
  switch (aArrayType) {
    case JS::Scalar::Int8:
    case JS::Scalar::Uint8:
    case JS::Scalar::Uint8Clamped:
      return in.readArray(static_cast<uint8_t*>(data), aNelems);
    case JS::Scalar::Int16:
    case JS::Scalar::Uint16:
      return in.readArray(static_cast<uint16_t*>(data), aNelems);
    case JS::Scalar::Int32:
    case JS::Scalar::Uint32:
    case JS::Scalar::Float32:
      return in.readArray(static_cast<uint32_t*>(data), aNelems);
    default:
      return in.readArray(static_cast<uint64_t*>(data), aNelems);
  }
}

bool JSStructuredCloneReader::primitiveToObject(v8::Local<v8::Value>* aVp) {
  v8::TryCatch tryCatch(isolate());
  v8::Local<v8::Object> obj;
  if (!(*aVp)->ToObject(ctx).ToLocal(&obj)) {
    return checkV8(tryCatch);
  }
  *aVp = obj;
  return true;
}

bool JSStructuredCloneReader::readSavedFrameHeader(
    uint32_t aPrincipalsTag, v8::Local<v8::Object>* aOut) {
  JSContext* cx = context();
  v8::Isolate* iso = isolate();
  if (aPrincipalsTag == SCTAG_JSPRINCIPALS) {
    JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                              JSMSG_SC_UNSUPPORTED_TYPE);
    return false;
  }
  if (aPrincipalsTag != SCTAG_RECONSTRUCTED_SAVED_FRAME_PRINCIPALS_IS_SYSTEM &&
      aPrincipalsTag !=
          SCTAG_RECONSTRUCTED_SAVED_FRAME_PRINCIPALS_IS_NOT_SYSTEM &&
      aPrincipalsTag != SCTAG_NULL_JSPRINCIPALS) {
    ReportBadData(cx, "bad SavedFrame principals");
    return false;
  }
  v8::Local<v8::Value> mutedErrors;
  v8::Local<v8::Value> source;
  if (!startRead(&mutedErrors)) {
    return false;
  }
  if (mutedErrors->IsBoolean()) {
    if (!startRead(&source)) {
      return false;
    }
    if (!source->IsString()) {
      ReportBadData(cx, "bad source string");
      return false;
    }
  } else if (mutedErrors->IsString()) {
    source = mutedErrors;
    mutedErrors = v8::True(iso);
  } else {
    ReportBadData(cx, "invalid mutedErrors");
    return false;
  }
  uint32_t line;
  uint32_t column;
  if (!readUint32(&line) || !readUint32(&column)) {
    return false;
  }
  v8::Local<v8::Value> name;
  if (!startRead(&name)) {
    return false;
  }
  if (!name->IsString() && !name->IsNull()) {
    ReportBadData(cx, "invalid saved frame cause");
    return false;
  }
  v8::Local<v8::Value> cause;
  if (!startRead(&cause)) {
    return false;
  }
  if (!cause->IsString() && !cause->IsNull()) {
    ReportBadData(cx, "invalid saved frame cause");
    return false;
  }
  v8::Local<v8::Object> frame = v8::Object::New(iso);
  v8::TryCatch tryCatch(iso);
  if (frame->CreateDataProperty(ctx, Literal(iso, "source"), source)
          .IsNothing() ||
      frame->CreateDataProperty(ctx, Literal(iso, "line"),
                                v8::Integer::NewFromUnsigned(iso, line))
          .IsNothing() ||
      frame->CreateDataProperty(ctx, Literal(iso, "column"),
                                v8::Integer::NewFromUnsigned(iso, column))
          .IsNothing() ||
      frame->CreateDataProperty(ctx, Literal(iso, "functionDisplayName"), name)
          .IsNothing() ||
      frame->CreateDataProperty(ctx, Literal(iso, "asyncCause"), cause)
          .IsNothing() ||
      frame->CreateDataProperty(ctx, Literal(iso, "mutedErrors"), mutedErrors)
          .IsNothing() ||
      frame->CreateDataProperty(ctx, Literal(iso, "parent"), v8::Null(iso))
          .IsNothing()) {
    return checkV8(tryCatch);
  }
  savedFrames.push_back(frame);
  *aOut = frame;
  return true;
}

bool JSStructuredCloneReader::readErrorHeader(uint32_t aType,
                                              v8::Local<v8::Object>* aOut) {
  JSContext* cx = context();
  v8::Isolate* iso = isolate();
  switch (aType) {
    case JSEXN_ERR:
    case JSEXN_EVALERR:
    case JSEXN_RANGEERR:
    case JSEXN_REFERENCEERR:
    case JSEXN_SYNTAXERR:
    case JSEXN_TYPEERR:
    case JSEXN_URIERR:
    case JSEXN_AGGREGATEERR:
      break;
    default:
      ReportBadData(cx, "invalid error type");
      return false;
  }
  v8::Local<v8::Value> messageValue;
  if (!startRead(&messageValue)) {
    return false;
  }
  if (!messageValue->IsString() && !messageValue->IsNull()) {
    ReportBadData(cx, "invalid 'message' field for Error object");
    return false;
  }
  v8::Local<v8::Value> val;
  if (!startRead(&val)) {
    return false;
  }
  if (!val->IsBoolean()) {
    ReportBadData(cx, "hasCause must be a boolean");
    return false;
  }
  bool hasCause = val->IsTrue();
  if (!startRead(&val)) {
    return false;
  }
  if (!val->IsString()) {
    ReportBadData(cx, "invalid 'fileName' field for Error object");
    return false;
  }
  uint32_t lineNumber;
  uint32_t columnNumber;
  if (!readUint32(&lineNumber) || !readUint32(&columnNumber)) {
    return false;
  }
  v8::TryCatch tryCatch(iso);
  v8::Local<v8::String> message = messageValue->IsString()
                                      ? messageValue.As<v8::String>()
                                      : v8::String::Empty(iso);
  v8::Local<v8::Value> error;
  switch (aType) {
    case JSEXN_RANGEERR:
      error = v8::Exception::RangeError(message);
      break;
    case JSEXN_REFERENCEERR:
      error = v8::Exception::ReferenceError(message);
      break;
    case JSEXN_SYNTAXERR:
      error = v8::Exception::SyntaxError(message);
      break;
    case JSEXN_TYPEERR:
      error = v8::Exception::TypeError(message);
      break;
    case JSEXN_ERR:
      error = v8::Exception::Error(message);
      break;
    default: {
      const char* ctorName = aType == JSEXN_EVALERR  ? "EvalError"
                             : aType == JSEXN_URIERR ? "URIError"
                                                     : "AggregateError";
      v8::Local<v8::Value> ctor;
      if (!ctx->Global()->Get(ctx, Literal(iso, ctorName)).ToLocal(&ctor)) {
        return checkV8(tryCatch);
      }
      if (!ctor->IsFunction()) {
        ReportBadData(cx, "invalid error type");
        return false;
      }
      v8::Local<v8::Object> created;
      if (aType == JSEXN_AGGREGATEERR) {
        v8::Local<v8::Value> argv[] = {v8::Array::New(iso), message};
        if (!ctor.As<v8::Function>()->NewInstance(ctx, 2, argv).ToLocal(
                &created)) {
          return checkV8(tryCatch);
        }
        (void)created->Delete(ctx, Literal(iso, "errors"));
      } else {
        v8::Local<v8::Value> argv[] = {message};
        if (!ctor.As<v8::Function>()->NewInstance(ctx, 1, argv).ToLocal(
                &created)) {
          return checkV8(tryCatch);
        }
      }
      error = created;
      break;
    }
  }
  v8::Local<v8::Object> errorObj = error.As<v8::Object>();
  if (messageValue->IsNull()) {
    (void)errorObj->Delete(ctx, Literal(iso, "message"));
  }
  if (hasCause) {
    if (errorObj
            ->DefineOwnProperty(ctx, Literal(iso, "cause"), v8::True(iso),
                                v8::DontEnum)
            .IsNothing()) {
      return checkV8(tryCatch);
    }
    errorsWithCause.push_back(errorObj);
  }
  *aOut = errorObj;
  return true;
}

bool JSStructuredCloneReader::readErrorFields(v8::Local<v8::Object> aError,
                                              v8::Local<v8::Value> aCause,
                                              bool* aState) {
  JSContext* cx = context();
  v8::Isolate* iso = isolate();
  if (*aState) {
    ReportBadData(cx, "unexpected child value seen for Error object");
    return false;
  }
  v8::Local<v8::Value> errors;
  v8::Local<v8::Value> stack;
  if (!startRead(&errors) || !startRead(&stack)) {
    return false;
  }
  bool hasCause = false;
  for (v8::Local<v8::Object>& e : errorsWithCause) {
    if (e == aError) {
      hasCause = true;
      break;
    }
  }
  v8::TryCatch tryCatch(iso);
  if (hasCause) {
    if (aError
            ->DefineOwnProperty(ctx, Literal(iso, "cause"), aCause,
                                v8::DontEnum)
            .IsNothing()) {
      return checkV8(tryCatch);
    }
  } else if (!aCause->IsNull()) {
    ReportBadData(cx, "invalid 'cause' field for Error object");
    return false;
  }
  v8::Local<v8::Value> name;
  bool aggregate =
      aError->Get(ctx, Literal(iso, "name")).ToLocal(&name) &&
      name->IsString() &&
      name.As<v8::String>()->StringEquals(Literal(iso, "AggregateError"));
  if (aggregate) {
    if (!errors->IsArray()) {
      ReportBadData(cx, "AggregateError 'errors' field must be an Array");
      return false;
    }
    if (aError
            ->DefineOwnProperty(ctx, Literal(iso, "errors"), errors,
                                v8::DontEnum)
            .IsNothing()) {
      return checkV8(tryCatch);
    }
  } else if (!errors->IsNull()) {
    ReportBadData(cx, "unexpected 'errors' field seen for non-AggregateError");
    return false;
  }
  if (stack->IsObject()) {
    bool isFrame = false;
    for (v8::Local<v8::Object>& f : savedFrames) {
      if (f == stack) {
        isFrame = true;
        break;
      }
    }
    if (!isFrame) {
      ReportBadData(cx, "invalid 'stack' field for Error object");
      return false;
    }
    errorStacks.emplace_back(aError, stack.As<v8::Object>());
  } else if (!stack->IsNull()) {
    ReportBadData(cx, "invalid 'stack' field for Error object");
    return false;
  }
  *aState = true;
  return true;
}

bool JSStructuredCloneReader::readObjectField(v8::Local<v8::Object> aObj,
                                              v8::Local<v8::Value> aKey) {
  if (!aKey->IsString() && !aKey->IsInt32()) {
    ReportBadData(context(), "property key expected");
    return false;
  }
  v8::Local<v8::Value> val;
  if (!startRead(&val)) {
    return false;
  }
  v8::TryCatch tryCatch(isolate());
  v8::Maybe<bool> ok = v8::Nothing<bool>();
  if (aKey->IsInt32() && aKey.As<v8::Int32>()->Value() >= 0) {
    ok = aObj->CreateDataProperty(ctx, uint32_t(aKey.As<v8::Int32>()->Value()),
                                  val);
  } else {
    v8::Local<v8::String> name;
    if (!aKey->ToString(ctx).ToLocal(&name)) {
      return checkV8(tryCatch);
    }
    ok = aObj->CreateDataProperty(ctx, name, val);
  }
  if (ok.IsNothing()) {
    return checkV8(tryCatch);
  }
  return true;
}

bool JSStructuredCloneReader::startReadUnchecked(v8::Local<v8::Value>* aVp,
                                                 bool* aUsedBackRef) {
  JSContext* cx = context();
  v8::Isolate* iso = isolate();
  uint32_t tag;
  uint32_t data;
  if (!in.readPair(&tag, &data)) {
    return false;
  }
  switch (tag) {
    case SCTAG_NULL:
      *aVp = v8::Null(iso);
      break;
    case SCTAG_UNDEFINED:
      *aVp = v8::Undefined(iso);
      break;
    case SCTAG_INT32:
      *aVp = v8::Integer::New(iso, int32_t(data));
      break;
    case SCTAG_BOOLEAN:
    case SCTAG_BOOLEAN_OBJECT:
      *aVp = v8::Boolean::New(iso, !!data);
      if (tag == SCTAG_BOOLEAN_OBJECT && !primitiveToObject(aVp)) {
        return false;
      }
      break;
    case SCTAG_STRING:
    case SCTAG_STRING_OBJECT: {
      v8::Local<v8::String> str;
      if (!readString(data, &str)) {
        return false;
      }
      *aVp = str;
      if (tag == SCTAG_STRING_OBJECT && !primitiveToObject(aVp)) {
        return false;
      }
      break;
    }
    case SCTAG_NUMBER_OBJECT: {
      double d;
      if (!in.readDouble(&d)) {
        return false;
      }
      *aVp = v8::Number::New(iso, d);
      if (!primitiveToObject(aVp)) {
        return false;
      }
      break;
    }
    case SCTAG_BIGINT:
    case SCTAG_BIGINT_OBJECT: {
      v8::Local<v8::BigInt> bi;
      if (!readBigInt(data, &bi)) {
        return false;
      }
      *aVp = bi;
      if (tag == SCTAG_BIGINT_OBJECT && !primitiveToObject(aVp)) {
        return false;
      }
      break;
    }
    case SCTAG_DATE_OBJECT: {
      double d;
      if (!in.readDouble(&d)) {
        return false;
      }
      JS::ClippedTime t = JS::TimeClip(d);
      if (!mozilla::NumbersAreIdentical(d, t.toDouble())) {
        ReportBadData(cx, "date");
        return false;
      }
      v8::TryCatch tryCatch(iso);
      v8::Local<v8::Value> date;
      if (!v8::Date::New(ctx, t.toDouble()).ToLocal(&date)) {
        return checkV8(tryCatch);
      }
      *aVp = date;
      break;
    }
    case SCTAG_REGEXP_OBJECT: {
      if ((data & JS::RegExpFlag::AllFlags) != data ||
          ((data & JS::RegExpFlag::Unicode) &&
           (data & JS::RegExpFlag::UnicodeSets))) {
        ReportBadData(cx, "regexp");
        return false;
      }
      uint32_t tag2;
      uint32_t stringData;
      if (!in.readPair(&tag2, &stringData)) {
        return false;
      }
      if (tag2 != SCTAG_STRING) {
        ReportBadData(cx, "regexp");
        return false;
      }
      v8::Local<v8::String> source;
      if (!readString(stringData, &source)) {
        return false;
      }
      int flags = v8::RegExp::kNone;
      if (data & JS::RegExpFlag::Global) {
        flags |= v8::RegExp::kGlobal;
      }
      if (data & JS::RegExpFlag::IgnoreCase) {
        flags |= v8::RegExp::kIgnoreCase;
      }
      if (data & JS::RegExpFlag::Multiline) {
        flags |= v8::RegExp::kMultiline;
      }
      if (data & JS::RegExpFlag::Sticky) {
        flags |= v8::RegExp::kSticky;
      }
      if (data & JS::RegExpFlag::Unicode) {
        flags |= v8::RegExp::kUnicode;
      }
      if (data & JS::RegExpFlag::DotAll) {
        flags |= v8::RegExp::kDotAll;
      }
      if (data & JS::RegExpFlag::HasIndices) {
        flags |= v8::RegExp::kHasIndices;
      }
      if (data & JS::RegExpFlag::UnicodeSets) {
        flags |= v8::RegExp::kUnicodeSets;
      }
      v8::TryCatch tryCatch(iso);
      v8::Local<v8::RegExp> re;
      if (!v8::RegExp::New(ctx, source, v8::RegExp::Flags(flags))
               .ToLocal(&re)) {
        return checkV8(tryCatch);
      }
      *aVp = re;
      break;
    }
    case SCTAG_ARRAY_OBJECT:
    case SCTAG_OBJECT_OBJECT: {
      v8::Local<v8::Object> obj;
      if (tag == SCTAG_ARRAY_OBJECT) {
        uint32_t length = NativeEndian::swapFromLittleEndian(data);
        if (length <= uint32_t(INT32_MAX)) {
          obj = v8::Array::New(iso, int(length));
        } else {
          v8::TryCatch tryCatch(iso);
          obj = v8::Array::New(iso, 0);
          if (obj->Set(ctx, Literal(iso, "length"),
                       v8::Integer::NewFromUnsigned(iso, length))
                  .IsNothing()) {
            return checkV8(tryCatch);
          }
        }
      } else {
        obj = v8::Object::New(iso);
      }
      pushObject(obj, ReadKind::Object);
      *aVp = obj;
      break;
    }
    case SCTAG_BACK_REFERENCE_OBJECT: {
      if (data >= allObjs.size() || !allObjs[data]->IsObject()) {
        ReportBadData(cx, "invalid back reference in input");
        return false;
      }
      *aVp = allObjs[data];
      *aUsedBackRef = true;
      return true;
    }
    case SCTAG_TRANSFER_MAP_HEADER:
    case SCTAG_TRANSFER_MAP_PENDING_ENTRY:
      ReportBadData(cx, "invalid input");
      return false;
    case SCTAG_ARRAY_BUFFER_OBJECT_V2:
    case SCTAG_ARRAY_BUFFER_OBJECT:
    case SCTAG_RESIZABLE_ARRAY_BUFFER_OBJECT:
    case SCTAG_IMMUTABLE_ARRAY_BUFFER_OBJECT:
      if (!readArrayBuffer(StructuredDataType(tag), data, aVp)) {
        return false;
      }
      break;
    case SCTAG_SHARED_ARRAY_BUFFER_OBJECT:
    case SCTAG_GROWABLE_SHARED_ARRAY_BUFFER_OBJECT:
      return readSharedArrayBuffer(StructuredDataType(tag), aVp);
    case SCTAG_SHARED_WASM_MEMORY_OBJECT:
      ReportBadData(cx, "shared wasm memory");
      return false;
    case SCTAG_TYPED_ARRAY_OBJECT_V2: {
      uint64_t arrayType;
      if (!in.read(&arrayType)) {
        return false;
      }
      if (arrayType > UINT32_MAX) {
        ReportBadData(cx, "unhandled typed array element type");
        return false;
      }
      return readTypedArray(uint32_t(arrayType), data, aVp);
    }
    case SCTAG_TYPED_ARRAY_OBJECT: {
      uint64_t nelems;
      if (!in.read(&nelems)) {
        return false;
      }
      return readTypedArray(data, nelems, aVp);
    }
    case SCTAG_DATA_VIEW_OBJECT_V2:
      return readDataView(data, aVp);
    case SCTAG_DATA_VIEW_OBJECT: {
      uint64_t byteLength;
      if (!in.read(&byteLength)) {
        return false;
      }
      return readDataView(byteLength, aVp);
    }
    case SCTAG_MAP_OBJECT: {
      v8::Local<v8::Map> map = v8::Map::New(iso);
      pushObject(map, ReadKind::Map);
      *aVp = map;
      break;
    }
    case SCTAG_SET_OBJECT: {
      v8::Local<v8::Set> set = v8::Set::New(iso);
      pushObject(set, ReadKind::Set);
      *aVp = set;
      break;
    }
    case SCTAG_SAVED_FRAME_OBJECT: {
      v8::Local<v8::Object> frame;
      if (!readSavedFrameHeader(data, &frame)) {
        return false;
      }
      pushObject(frame, ReadKind::SavedFrame);
      objState.emplace_back(frame, false);
      *aVp = frame;
      break;
    }
    case SCTAG_ERROR_OBJECT: {
      v8::Local<v8::Object> error;
      if (!readErrorHeader(data, &error)) {
        return false;
      }
      pushObject(error, ReadKind::Error);
      objState.emplace_back(error, false);
      *aVp = error;
      break;
    }
    case SCTAG_END_OF_KEYS:
      ReportBadData(cx, "truncated input");
      return false;
    default: {
      if (tag <= SCTAG_FLOAT_MAX) {
        double d = mozilla::BitwiseCast<double>(PairToUInt64(tag, data));
        *aVp = v8::Number::New(iso, d);
        break;
      }
      if (SCTAG_TYPED_ARRAY_V1_MIN <= tag && tag <= SCTAG_TYPED_ARRAY_V1_MAX) {
        return readTypedArray(tag - SCTAG_TYPED_ARRAY_V1_MIN, data, aVp, true);
      }
      if (!callbacks || !callbacks->read) {
        ReportBadData(cx, "unsupported type");
        return false;
      }
      size_t placeholderIndex = allObjs.size();
      allObjs.push_back(v8::Undefined(iso));
      JSObject* obj =
          callbacks->read(cx, this, cloneDataPolicy, tag, data, closure);
      if (!obj) {
        return false;
      }
      *aVp = V8Of(cx, obj);
      allObjs[placeholderIndex] = *aVp;
      return true;
    }
  }
  if ((*aVp)->IsObject()) {
    allObjs.push_back(*aVp);
  }
  return true;
}

bool JSStructuredCloneReader::startRead(v8::Local<v8::Value>* aVp) {
  bool usedBackRef = false;
  return startReadUnchecked(aVp, &usedBackRef);
}

bool JSStructuredCloneReader::readHeader() {
  uint32_t tag;
  uint32_t data;
  if (!in.getPair(&tag, &data)) {
    return in.reportTruncated();
  }
  JS::StructuredCloneScope storedScope;
  if (tag == SCTAG_HEADER) {
    MOZ_ALWAYS_TRUE(in.readPair(&tag, &data));
    storedScope = JS::StructuredCloneScope(data);
  } else {
    storedScope = JS::StructuredCloneScope::DifferentProcessForIndexedDB;
  }
  if (allowedScope == JS::StructuredCloneScope::DifferentProcessForIndexedDB) {
    allowedScope = JS::StructuredCloneScope::DifferentProcess;
    if (int(storedScope) == 0) {
      storedScope = JS::StructuredCloneScope::DifferentProcess;
    }
  }
  if (storedScope < JS::StructuredCloneScope::SameProcess ||
      storedScope > JS::StructuredCloneScope::DifferentProcessForIndexedDB) {
    ReportBadData(context(), "invalid structured clone scope");
    return false;
  }
  if (storedScope < allowedScope) {
    ReportBadData(context(), "incompatible structured clone scope");
    return false;
  }
  if (allowedScope == JS::StructuredCloneScope::DifferentProcess) {
    MOZ_RELEASE_ASSERT(
        !cloneDataPolicy.areIntraClusterClonableSharedObjectsAllowed());
    MOZ_RELEASE_ASSERT(!cloneDataPolicy.areSharedMemoryObjectsAllowed());
  }
  return true;
}

bool JSStructuredCloneReader::readTransferMap() {
  JSContext* cx = context();
  auto headerPos = in.tell();
  uint32_t tag;
  uint32_t data;
  if (!in.getPair(&tag, &data)) {
    return in.reportTruncated();
  }
  if (tag != SCTAG_TRANSFER_MAP_HEADER) {
    return true;
  }
  if (data >= SCTAG_TM_END) {
    ReportBadData(cx, "invalid transfer map header");
    return false;
  }
  auto transferState = static_cast<TransferableMapHeader>(data);
  if (transferState == SCTAG_TM_TRANSFERRED) {
    return true;
  }
  if (transferState == SCTAG_TM_TRANSFERRING) {
    return ReportCloneError(cx, callbacks, JS_SCERR_TRANSFERABLE_TWICE,
                            closure, nullptr);
  }
  headerPos.write(
      PairToUInt64(SCTAG_TRANSFER_MAP_HEADER, SCTAG_TM_TRANSFERRING));
  uint64_t numTransferables;
  MOZ_ALWAYS_TRUE(in.readPair(&tag, &data));
  if (!in.read(&numTransferables)) {
    return false;
  }
  for (uint64_t i = 0; i < numTransferables; i++) {
    auto pos = in.tell();
    if (!in.readPair(&tag, &data)) {
      return false;
    }
    if (tag == SCTAG_TRANSFER_MAP_PENDING_ENTRY) {
      return ReportCloneError(cx, callbacks, JS_SCERR_TRANSFERABLE, closure,
                              nullptr);
    }
    void* content;
    uint64_t extraData;
    if (!in.readPtr(&content) || !in.read(&extraData)) {
      return false;
    }
    JS::RootedObject obj(cx);
    if (tag == SCTAG_TRANSFER_MAP_ARRAY_BUFFER) {
      if (allowedScope == JS::StructuredCloneScope::DifferentProcess) {
        return ReportCloneError(cx, callbacks, JS_SCERR_TRANSFERABLE, closure,
                                nullptr);
      }
      MOZ_RELEASE_ASSERT(extraData <= kByteLengthLimit);
      size_t nbytes = size_t(extraData);
      if (data == JS::SCTAG_TMO_ALLOC_DATA) {
        obj = JS::NewArrayBufferWithContents(
            cx, nbytes, content,
            JS::NewArrayBufferOutOfMemory::CallerMustFreeMemory);
      } else if (data == JS::SCTAG_TMO_MAPPED_DATA) {
        obj = JS::NewMappedArrayBufferWithContents(cx, nbytes, content);
      }
    } else if (tag == SCTAG_TRANSFER_MAP_STORED_ARRAY_BUFFER) {
      auto savedPos = in.tell();
      auto guard = mozilla::MakeScopeExit([&] { in.seekTo(savedPos); });
      in.seekTo(pos);
      if (!in.seekBy(static_cast<size_t>(extraData))) {
        return false;
      }
      if (tailStartPos.isNothing()) {
        tailStartPos = mozilla::Some(in.tell());
      }
      uint32_t storedTag;
      uint32_t storedData;
      if (!in.readPair(&storedTag, &storedData)) {
        return false;
      }
      if (storedTag != SCTAG_ARRAY_BUFFER_OBJECT_V2 &&
          storedTag != SCTAG_ARRAY_BUFFER_OBJECT &&
          storedTag != SCTAG_RESIZABLE_ARRAY_BUFFER_OBJECT &&
          storedTag != SCTAG_IMMUTABLE_ARRAY_BUFFER_OBJECT) {
        return ReportCloneError(cx, callbacks, JS_SCERR_TRANSFERABLE, closure,
                                nullptr);
      }
      v8::Local<v8::Value> val;
      if (!readArrayBuffer(StructuredDataType(storedTag), storedData, &val)) {
        return false;
      }
      obj = BufferObjectFromV8(cx, val.As<v8::Object>());
      tailEndPos = mozilla::Some(in.tell());
    } else {
      if (!callbacks || !callbacks->readTransfer) {
        return ReportCloneError(cx, callbacks, JS_SCERR_TRANSFERABLE, closure,
                                nullptr);
      }
      if (!callbacks->readTransfer(cx, this, cloneDataPolicy, tag, content,
                                   extraData, closure, &obj)) {
        if (!JS_IsExceptionPending(cx)) {
          ReportCloneError(cx, callbacks, JS_SCERR_TRANSFERABLE, closure,
                           nullptr);
        }
        return false;
      }
    }
    if (!obj) {
      return false;
    }
    pos.write(PairToUInt64(tag, JS::SCTAG_TMO_UNOWNED));
    allObjs.push_back(V8Of(cx, obj));
  }
  headerPos.write(
      PairToUInt64(SCTAG_TRANSFER_MAP_HEADER, SCTAG_TM_TRANSFERRED));
  return true;
}

bool JSStructuredCloneReader::read(v8::Local<v8::Value>* aVp) {
  JSContext* cx = context();
  v8::Isolate* iso = isolate();
  if (!readHeader() || !readTransferMap()) {
    return false;
  }
  if (!startRead(aVp)) {
    return false;
  }
  while (!objs.empty()) {
    v8::Local<v8::Object> obj = objs.back();
    ReadKind kind = objKinds.back();
    uint32_t tag;
    uint32_t data;
    if (!in.getPair(&tag, &data)) {
      return false;
    }
    if (tag == SCTAG_END_OF_KEYS) {
      MOZ_ALWAYS_TRUE(in.readPair(&tag, &data));
      objs.pop_back();
      objKinds.pop_back();
      if (!objState.empty() && objState.back().first == obj) {
        objState.pop_back();
      }
      continue;
    }
    size_t objStateIdx = objState.size();
    if (objStateIdx && objState.back().first == obj) {
      objStateIdx--;
    } else {
      objStateIdx = size_t(-1);
    }
    v8::Local<v8::Value> key;
    if (!startRead(&key)) {
      return false;
    }
    if (key->IsNull() && kind == ReadKind::Object) {
      objs.pop_back();
      objKinds.pop_back();
      continue;
    }
    v8::TryCatch tryCatch(iso);
    switch (kind) {
      case ReadKind::Set:
        if (obj.As<v8::Set>()->Add(ctx, key).IsEmpty()) {
          return checkV8(tryCatch);
        }
        break;
      case ReadKind::Map: {
        v8::Local<v8::Value> val;
        if (!startRead(&val)) {
          return false;
        }
        if (obj.As<v8::Map>()->Set(ctx, key, val).IsEmpty()) {
          return checkV8(tryCatch);
        }
        break;
      }
      case ReadKind::SavedFrame: {
        if (objStateIdx == size_t(-1) || objState[objStateIdx].second) {
          ReportBadData(cx, "multiple SavedFrame parents");
          return false;
        }
        if (!key->IsNull()) {
          bool isFrame = false;
          for (v8::Local<v8::Object>& f : savedFrames) {
            if (f == key) {
              isFrame = true;
              break;
            }
          }
          if (!isFrame) {
            ReportBadData(cx, "invalid SavedFrame parent");
            return false;
          }
        }
        if (obj->CreateDataProperty(ctx, Literal(iso, "parent"), key)
                .IsNothing()) {
          return checkV8(tryCatch);
        }
        objState[objStateIdx].second = true;
        break;
      }
      case ReadKind::Error: {
        if (objStateIdx == size_t(-1)) {
          ReportBadData(cx, "unexpected child value seen for Error object");
          return false;
        }
        bool state = objState[objStateIdx].second;
        if (!readErrorFields(obj, key, &state)) {
          return false;
        }
        objState[objStateIdx].second = state;
        break;
      }
      case ReadKind::Object:
        if (!readObjectField(obj, key)) {
          return false;
        }
        break;
    }
  }
  for (auto& [error, frame] : errorStacks) {
    std::string text;
    v8::Local<v8::Value> current = frame;
    while (current->IsObject()) {
      v8::Local<v8::Object> f = current.As<v8::Object>();
      v8::Local<v8::Value> fn;
      v8::Local<v8::Value> source;
      v8::Local<v8::Value> line;
      v8::Local<v8::Value> column;
      if (!f->Get(ctx, Literal(iso, "functionDisplayName")).ToLocal(&fn) ||
          !f->Get(ctx, Literal(iso, "source")).ToLocal(&source) ||
          !f->Get(ctx, Literal(iso, "line")).ToLocal(&line) ||
          !f->Get(ctx, Literal(iso, "column")).ToLocal(&column) ||
          !f->Get(ctx, Literal(iso, "parent")).ToLocal(&current)) {
        return false;
      }
      if (fn->IsString()) {
        text += *v8::String::Utf8Value(iso, fn);
      }
      text += "@";
      text += *v8::String::Utf8Value(iso, source);
      text += ":";
      text += *v8::String::Utf8Value(iso, line);
      text += ":";
      text += *v8::String::Utf8Value(iso, column);
      text += "\n";
    }
    v8::TryCatch tryCatch(iso);
    v8::Local<v8::String> stack;
    if (!v8::String::NewFromUtf8(iso, text.c_str()).ToLocal(&stack) ||
        error->DefineOwnProperty(ctx, Literal(iso, "stack"), stack,
                                 v8::DontEnum)
            .IsNothing()) {
      return checkV8(tryCatch);
    }
  }
  allObjs.clear();
  bool extraData;
  if (tailStartPos.isSome()) {
    extraData = (in.tell() != *tailStartPos || !tailEndPos->done());
  } else {
    extraData = !in.tell().done();
  }
  if (extraData) {
    ReportBadData(cx, "extra data after end");
    return false;
  }
  return true;
}

namespace {

bool WriteStructuredClone(JSContext* aCx, JS::HandleValue aValue,
                          JSStructuredCloneData* aBuf,
                          JS::StructuredCloneScope aScope,
                          const JS::CloneDataPolicy& aPolicy,
                          const JSStructuredCloneCallbacks* aCallbacks,
                          void* aClosure, JS::HandleValue aTransferable) {
  AutoV8 v8(aCx);
  JSStructuredCloneWriter w(aCx, aScope, aPolicy, aCallbacks, aClosure,
                            ToV8(aCx, aTransferable.get()));
  if (!w.init()) {
    return false;
  }
  if (!w.write(ToV8(aCx, aValue.get()))) {
    return false;
  }
  w.extractBuffer(aBuf);
  return true;
}

}

JSStructuredCloneData::~JSStructuredCloneData() { discardTransferables(); }

void JSStructuredCloneData::discardTransferables() {
  if (!Size()) {
    return;
  }
  if (ownTransferables_ != OwnTransferablePolicy::OwnsTransferablesIfAny) {
    return;
  }
  if (scope() == JS::StructuredCloneScope::DifferentProcess) {
    return;
  }
  FreeTransferStructuredCloneOp freeTransfer = nullptr;
  if (callbacks_) {
    freeTransfer = callbacks_->freeTransfer;
  }
  auto point = js::BufferIterator<uint64_t, js::SystemAllocPolicy>(*this);
  if (point.done()) {
    return;
  }
  uint32_t tag;
  uint32_t data;
  MOZ_RELEASE_ASSERT(point.canPeek());
  js::SCInput::getPair(point.peek(), &tag, &data);
  MOZ_ALWAYS_TRUE(point.advance());
  if (tag == SCTAG_HEADER) {
    if (point.done()) {
      return;
    }
    MOZ_RELEASE_ASSERT(point.canPeek());
    js::SCInput::getPair(point.peek(), &tag, &data);
    MOZ_ALWAYS_TRUE(point.advance());
  }
  if (tag != SCTAG_TRANSFER_MAP_HEADER) {
    return;
  }
  if (TransferableMapHeader(data) == SCTAG_TM_TRANSFERRED) {
    return;
  }
  if (point.done()) {
    return;
  }
  MOZ_RELEASE_ASSERT(point.canPeek());
  uint64_t numTransferables = NativeEndian::swapFromLittleEndian(point.peek());
  MOZ_ALWAYS_TRUE(point.advance());
  while (numTransferables--) {
    if (!point.canPeek()) {
      return;
    }
    uint32_t ownership;
    js::SCInput::getPair(point.peek(), &tag, &ownership);
    MOZ_ALWAYS_TRUE(point.advance());
    if (!point.canPeek()) {
      return;
    }
    void* content;
    js::SCInput::getPtr(point.peek(), &content);
    MOZ_ALWAYS_TRUE(point.advance());
    if (!point.canPeek()) {
      return;
    }
    uint64_t extraData = NativeEndian::swapFromLittleEndian(point.peek());
    MOZ_ALWAYS_TRUE(point.advance());
    if (ownership < JS::SCTAG_TMO_FIRST_OWNED) {
      continue;
    }
    if (ownership == JS::SCTAG_TMO_ALLOC_DATA) {
      js_free(content);
    } else if (ownership == JS::SCTAG_TMO_MAPPED_DATA) {
      JS::ReleaseMappedArrayBufferContents(content, size_t(extraData));
    } else if (freeTransfer) {
      freeTransfer(tag, JS::TransferableOwnership(ownership), content,
                   extraData, closure_);
    }
  }
}

bool JS_ReadStructuredClone(JSContext* aCx, const JSStructuredCloneData& aBuf,
                            uint32_t aVersion, JS::StructuredCloneScope aScope,
                            JS::MutableHandleValue aVp,
                            const JS::CloneDataPolicy& aPolicy,
                            const JSStructuredCloneCallbacks* aCallbacks,
                            void* aClosure) {
  if (aVersion > JS_STRUCTURED_CLONE_VERSION) {
    JS_ReportErrorNumberASCII(aCx, js::GetErrorMessage, nullptr,
                              JSMSG_SC_BAD_CLONE_VERSION);
    return false;
  }
  if (aBuf.Size() % 8) {
    ReportBadData(aCx, "misaligned");
    return false;
  }
  AutoV8 v8(aCx);
  js::SCInput in(aCx, aBuf);
  JSStructuredCloneReader r(in, aScope, aPolicy, aCallbacks, aClosure);
  v8::Local<v8::Value> result;
  if (!r.read(&result)) {
    return false;
  }
  aVp.set(ValueFromV8(aCx, result));
  return true;
}

bool JS_StructuredClone(JSContext* aCx, JS::HandleValue aValue,
                        JS::MutableHandleValue aVp,
                        const JSStructuredCloneCallbacks* aCallbacks,
                        void* aClosure) {
  if (aValue.isString()) {
    aVp.setString(aValue.toString());
    return true;
  }
  JSAutoStructuredCloneBuffer buf(JS::StructuredCloneScope::SameProcess,
                                  aCallbacks, aClosure);
  if (aValue.isObject()) {
    JS::RootedObject obj(aCx, js::CheckedUnwrapStatic(&aValue.toObject()));
    if (!obj) {
      js::ReportAccessDenied(aCx);
      return false;
    }
    JSAutoRealm ar(aCx, obj);
    JS::RootedValue unwrappedVal(aCx, JS::ObjectValue(*obj));
    if (!buf.write(aCx, unwrappedVal, aCallbacks, aClosure)) {
      return false;
    }
  } else if (!buf.write(aCx, aValue, aCallbacks, aClosure)) {
    return false;
  }
  return buf.read(aCx, aVp, JS::CloneDataPolicy(), aCallbacks, aClosure);
}

void JSAutoStructuredCloneBuffer::clear() {
  data_.discardTransferables();
  data_.ownTransferables_ = OwnTransferablePolicy::NoTransferables;
  data_.refsHeld_.releaseAll();
  data_.stringBufferRefsHeld_.clear();
  data_.Clear();
  version_ = 0;
}

void JSAutoStructuredCloneBuffer::adopt(
    JSStructuredCloneData&& aData, uint32_t aVersion,
    const JSStructuredCloneCallbacks* aCallbacks, void* aClosure) {
  clear();
  data_ = std::move(aData);
  version_ = aVersion;
  data_.setCallbacks(aCallbacks, aClosure,
                     OwnTransferablePolicy::OwnsTransferablesIfAny);
}

bool JSAutoStructuredCloneBuffer::read(
    JSContext* aCx, JS::MutableHandleValue aVp,
    const JS::CloneDataPolicy& aPolicy,
    const JSStructuredCloneCallbacks* aOptionalCallbacks, void* aClosure) {
  return JS_ReadStructuredClone(
      aCx, data_, version_, data_.scope(), aVp, aPolicy,
      aOptionalCallbacks ? aOptionalCallbacks : data_.callbacks_,
      aOptionalCallbacks ? aClosure : data_.closure_);
}

bool JSAutoStructuredCloneBuffer::write(
    JSContext* aCx, JS::HandleValue aValue,
    const JSStructuredCloneCallbacks* aOptionalCallbacks, void* aClosure) {
  return write(aCx, aValue, JS::UndefinedHandleValue, JS::CloneDataPolicy(),
               aOptionalCallbacks ? aOptionalCallbacks : data_.callbacks_,
               aOptionalCallbacks ? aClosure : data_.closure_);
}

bool JSAutoStructuredCloneBuffer::write(
    JSContext* aCx, JS::HandleValue aValue, JS::HandleValue aTransferable,
    const JS::CloneDataPolicy& aPolicy,
    const JSStructuredCloneCallbacks* aOptionalCallbacks, void* aClosure) {
  clear();
  version_ = JS_STRUCTURED_CLONE_VERSION;
  return WriteStructuredClone(
      aCx, aValue, &data_, data_.scopeForInternalWriting(), aPolicy,
      aOptionalCallbacks ? aOptionalCallbacks : data_.callbacks_,
      aOptionalCallbacks ? aClosure : data_.closure_, aTransferable);
}

bool JS_ReadUint32Pair(JSStructuredCloneReader* aR, uint32_t* aP1,
                       uint32_t* aP2) {
  return aR->input().readPair(aP1, aP2);
}

bool JS_ReadBytes(JSStructuredCloneReader* aR, void* aP, size_t aLen) {
  return aR->input().readBytes(aP, aLen);
}

bool JS_ReadString(JSStructuredCloneReader* aR, JS::MutableHandleString aStr) {
  uint32_t tag;
  uint32_t data;
  if (!aR->input().readPair(&tag, &data)) {
    return false;
  }
  if (tag != SCTAG_STRING) {
    ReportBadData(aR->context(), "expected string");
    return false;
  }
  v8::HandleScope scope(aR->isolate());
  v8::Local<v8::String> str;
  if (!aR->readString(data, &str)) {
    return false;
  }
  aStr.set(StringFromV8(aR->context(), str));
  return true;
}

bool JS_ReadDouble(JSStructuredCloneReader* aR, double* aV) {
  return aR->input().readDouble(aV);
}

bool JS_ReadTypedArray(JSStructuredCloneReader* aR,
                       JS::MutableHandleValue aVp) {
  uint32_t tag;
  uint32_t data;
  if (!aR->input().readPair(&tag, &data)) {
    return false;
  }
  v8::Local<v8::Value> result;
  bool ok;
  if (tag >= SCTAG_TYPED_ARRAY_V1_MIN && tag <= SCTAG_TYPED_ARRAY_V1_MAX) {
    ok = aR->readTypedArray(tag - SCTAG_TYPED_ARRAY_V1_MIN, data, &result,
                            true);
  } else if (tag == SCTAG_TYPED_ARRAY_OBJECT_V2) {
    uint64_t arrayType;
    if (!aR->input().read(&arrayType)) {
      return false;
    }
    if (arrayType > UINT32_MAX) {
      ReportBadData(aR->context(), "unhandled typed array element type");
      return false;
    }
    ok = aR->readTypedArray(uint32_t(arrayType), data, &result);
  } else if (tag == SCTAG_TYPED_ARRAY_OBJECT) {
    uint64_t nelems;
    if (!aR->input().read(&nelems)) {
      return false;
    }
    ok = aR->readTypedArray(data, nelems, &result);
  } else {
    ReportBadData(aR->context(), "expected type array");
    return false;
  }
  if (!ok) {
    return false;
  }
  aVp.set(ValueFromV8(aR->context(), result));
  return true;
}

bool JS_WriteUint32PairUnchecked(JSStructuredCloneWriter* aW, uint32_t aTag,
                                 uint32_t aData) {
  return aW->output().writePair(aTag, aData);
}

bool JS_WriteBytes(JSStructuredCloneWriter* aW, const void* aP, size_t aLen) {
  return aW->output().writeBytes(aP, aLen);
}

bool JS_WriteString(JSStructuredCloneWriter* aW, JS::HandleString aStr) {
  StringCell* cell = StringCell::from(aStr);
  return aW->writeStringChars(SCTAG_STRING, cell->chars, cell->length(),
                              cell->latin1());
}

bool JS_WriteDouble(JSStructuredCloneWriter* aW, double aV) {
  return aW->output().writeDouble(aV);
}

bool JS_WriteTypedArray(JSStructuredCloneWriter* aW, JS::HandleValue aV) {
  JSContext* cx = aW->context();
  JS::RootedObject obj(cx, &aV.toObject());
  JSObject* unwrapped = Unwrapped(obj);
  if (!unwrapped || !V8Of(cx, unwrapped)->IsTypedArray()) {
    js::ReportAccessDenied(cx);
    return false;
  }
  return aW->startWrite(ToV8(cx, aV.get()));
}

bool JS_ObjectNotWritten(JSStructuredCloneWriter* aW, JS::HandleObject aObj) {
  (void)aW->memory->Delete(aW->ctx, V8Of(aW->context(), aObj));
  return true;
}

uint64_t js::GetSCOffset(JSStructuredCloneWriter* aWriter) {
  return aWriter->output().buf.Size();
}
