/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/UbiNodeCensus.h"

#include <stdlib.h>
#include <string.h>

#include "mozilla/HashFunctions.h"
#include "mozilla/ScopeExit.h"

#include "jsapi.h"
#include "jsfriendapi.h"
#include "js/Array.h"
#include "js/Conversions.h"
#include "js/ErrorReport.h"
#include "js/MapAndSet.h"
#include "js/PropertyAndElement.h"
#include "js/String.h"
#include "js/UniquePtr.h"
#include "js/Utility.h"
#include "js/ValueArray.h"
#include "js/shadow/Zone.h"

namespace JS {
namespace ubi {

namespace {

enum VgeCensusErrNum : unsigned {
  VgeCensusErrBreakdown,
  VgeCensusErrBreakdownNested,
  VgeCensusErrLimit
};

const JSErrorFormatString VgeCensusErrorFormats[VgeCensusErrLimit] = {
    {"JSMSG_DEBUG_CENSUS_BREAKDOWN",
     "unrecognized 'by' value in takeCensus breakdown: {0}", 1, JSEXN_TYPEERR},
    {"JSMSG_DEBUG_CENSUS_BREAKDOWN_NESTED",
     "takeCensus breakdown 'by' value nested within itself: {0}", 1,
     JSEXN_TYPEERR},
};

const JSErrorFormatString* GetCensusErrorMessage(void* userRef,
                                                 const unsigned errorNumber) {
  if (errorNumber < VgeCensusErrLimit) {
    return &VgeCensusErrorFormats[errorNumber];
  }
  return nullptr;
}

size_t Char16Length(const char16_t* chars) {
  const char16_t* p = chars;
  while (*p) {
    p++;
  }
  return size_t(p - chars);
}

UniqueChars DuplicateCString(const char* s) {
  size_t n = strlen(s) + 1;
  UniqueChars ret(js_pod_malloc<char>(n));
  if (!ret) {
    return nullptr;
  }
  memcpy(ret.get(), s, n);
  return ret;
}

UniqueTwoByteChars DuplicateChar16String(const char16_t* s) {
  size_t n = Char16Length(s) + 1;
  UniqueTwoByteChars ret(js_pod_malloc<char16_t>(n));
  if (!ret) {
    return nullptr;
  }
  memcpy(ret.get(), s, n * sizeof(char16_t));
  return ret;
}

UniqueChars QuoteLinearString(JSContext* cx, JSLinearString* str) {
  size_t needed = JS_PutEscapedLinearString(nullptr, 0, str, '"');
  UniqueChars buffer(js_pod_malloc<char>(needed + 1));
  if (!buffer) {
    JS_ReportOutOfMemory(cx);
    return nullptr;
  }
  JS_PutEscapedLinearString(buffer.get(), needed + 1, str, '"');
  return buffer;
}

bool LinearStringsEqual(JSLinearString* a, JSLinearString* b) {
  if (a == b) {
    return true;
  }
  size_t length = GetLinearStringLength(a);
  if (length != GetLinearStringLength(b)) {
    return false;
  }
  for (size_t i = 0; i < length; i++) {
    if (GetLinearStringCharAt(a, i) != GetLinearStringCharAt(b, i)) {
      return false;
    }
  }
  return true;
}

template <class T, class... Args>
T* NewCountType(JSContext* cx, Args&&... args) {
  T* p = js_new<T>(std::forward<Args>(args)...);
  if (!p) {
    JS_ReportOutOfMemory(cx);
  }
  return p;
}

bool DefineReportProperty(JSContext* cx, HandleObject obj, const char* name,
                          HandleValue value) {
  return JS_DefineProperty(cx, obj, name, value, JSPROP_ENUMERATE);
}

bool DefineReportPropertyByString(JSContext* cx, HandleObject obj,
                                  JSString* name, HandleValue value) {
  if (!name) {
    return false;
  }
  RootedString nameString(cx, name);
  RootedId id(cx);
  if (!JS_StringToId(cx, nameString, &id)) {
    return false;
  }
  return JS_DefinePropertyById(cx, obj, id, value, JSPROP_ENUMERATE);
}

}

JS_PUBLIC_API void CountDeleter::operator()(CountBase* ptr) {
  if (!ptr) {
    return;
  }

  ptr->destruct();
  js_free(ptr);
}

class SimpleCount : public CountType {
  struct Count : CountBase {
    size_t totalBytes_;

    explicit Count(SimpleCount& count) : CountBase(count), totalBytes_(0) {}
  };

  UniqueTwoByteChars label;
  bool reportCount : 1;
  bool reportBytes : 1;

 public:
  explicit SimpleCount(UniqueTwoByteChars& label, bool reportCount = true,
                       bool reportBytes = true)
      : label(std::move(label)),
        reportCount(reportCount),
        reportBytes(reportBytes) {}

  explicit SimpleCount()
      : label(nullptr), reportCount(true), reportBytes(true) {}

  void destructCount(CountBase& countBase) override {
    Count& count = static_cast<Count&>(countBase);
    count.~Count();
  }

  CountBasePtr makeCount() override {
    return CountBasePtr(js_new<Count>(*this));
  }
  void traceCount(CountBase& countBase, JSTracer* trc) override {}
  bool count(CountBase& countBase, mozilla::MallocSizeOf mallocSizeOf,
             const Node& node) override;
  bool report(JSContext* cx, CountBase& countBase,
              MutableHandleValue report) override;
};

bool SimpleCount::count(CountBase& countBase,
                        mozilla::MallocSizeOf mallocSizeOf, const Node& node) {
  Count& count = static_cast<Count&>(countBase);
  if (reportBytes) {
    count.totalBytes_ += node.size(mallocSizeOf);
  }
  return true;
}

bool SimpleCount::report(JSContext* cx, CountBase& countBase,
                         MutableHandleValue report) {
  Count& count = static_cast<Count&>(countBase);

  RootedObject obj(cx, JS_NewPlainObject(cx));
  if (!obj) {
    return false;
  }

  RootedValue countValue(cx, NumberValue(count.total_));
  if (reportCount && !DefineReportProperty(cx, obj, "count", countValue)) {
    return false;
  }

  RootedValue bytesValue(cx, NumberValue(count.totalBytes_));
  if (reportBytes && !DefineReportProperty(cx, obj, "bytes", bytesValue)) {
    return false;
  }

  if (label) {
    JSString* labelString = JS_NewUCStringCopyZ(cx, label.get());
    if (!labelString) {
      return false;
    }
    RootedValue labelValue(cx, StringValue(labelString));
    if (!DefineReportProperty(cx, obj, "label", labelValue)) {
      return false;
    }
  }

  report.setObject(*obj);
  return true;
}

class BucketCount : public CountType {
  struct Count : CountBase {
    JS::ubi::Vector<JS::ubi::Node::Id> ids_;

    explicit Count(BucketCount& count) : CountBase(count) {}
  };

 public:
  explicit BucketCount() = default;

  void destructCount(CountBase& countBase) override {
    Count& count = static_cast<Count&>(countBase);
    count.~Count();
  }

  CountBasePtr makeCount() override {
    return CountBasePtr(js_new<Count>(*this));
  }
  void traceCount(CountBase& countBase, JSTracer* trc) final {}
  bool count(CountBase& countBase, mozilla::MallocSizeOf mallocSizeOf,
             const Node& node) override;
  bool report(JSContext* cx, CountBase& countBase,
              MutableHandleValue report) override;
};

bool BucketCount::count(CountBase& countBase,
                        mozilla::MallocSizeOf mallocSizeOf, const Node& node) {
  Count& count = static_cast<Count&>(countBase);
  return count.ids_.append(node.identifier());
}

bool BucketCount::report(JSContext* cx, CountBase& countBase,
                         MutableHandleValue report) {
  Count& count = static_cast<Count&>(countBase);

  size_t length = count.ids_.length();
  RootedValueVector elements(cx);
  if (!elements.reserve(length)) {
    JS_ReportOutOfMemory(cx);
    return false;
  }
  for (size_t i = 0; i < length; i++) {
    elements.infallibleAppend(NumberValue(count.ids_[i]));
  }

  JSObject* arr = NewArrayObject(cx, elements);
  if (!arr) {
    return false;
  }

  report.setObject(*arr);
  return true;
}

class ByCoarseType : public CountType {
  CountTypePtr objects;
  CountTypePtr scripts;
  CountTypePtr strings;
  CountTypePtr other;
  CountTypePtr domNode;

  struct Count : CountBase {
    Count(CountType& type, CountBasePtr& objects, CountBasePtr& scripts,
          CountBasePtr& strings, CountBasePtr& other, CountBasePtr& domNode)
        : CountBase(type),
          objects(std::move(objects)),
          scripts(std::move(scripts)),
          strings(std::move(strings)),
          other(std::move(other)),
          domNode(std::move(domNode)) {}

    CountBasePtr objects;
    CountBasePtr scripts;
    CountBasePtr strings;
    CountBasePtr other;
    CountBasePtr domNode;
  };

 public:
  ByCoarseType(CountTypePtr& objects, CountTypePtr& scripts,
               CountTypePtr& strings, CountTypePtr& other,
               CountTypePtr& domNode)
      : objects(std::move(objects)),
        scripts(std::move(scripts)),
        strings(std::move(strings)),
        other(std::move(other)),
        domNode(std::move(domNode)) {}

  void destructCount(CountBase& countBase) override {
    Count& count = static_cast<Count&>(countBase);
    count.~Count();
  }

  CountBasePtr makeCount() override;
  void traceCount(CountBase& countBase, JSTracer* trc) override;
  bool count(CountBase& countBase, mozilla::MallocSizeOf mallocSizeOf,
             const Node& node) override;
  bool report(JSContext* cx, CountBase& countBase,
              MutableHandleValue report) override;
};

CountBasePtr ByCoarseType::makeCount() {
  CountBasePtr objectsCount(objects->makeCount());
  CountBasePtr scriptsCount(scripts->makeCount());
  CountBasePtr stringsCount(strings->makeCount());
  CountBasePtr otherCount(other->makeCount());
  CountBasePtr domNodeCount(domNode->makeCount());

  if (!objectsCount || !scriptsCount || !stringsCount || !otherCount ||
      !domNodeCount) {
    return CountBasePtr(nullptr);
  }

  return CountBasePtr(js_new<Count>(*this, objectsCount, scriptsCount,
                                    stringsCount, otherCount, domNodeCount));
}

void ByCoarseType::traceCount(CountBase& countBase, JSTracer* trc) {
  Count& count = static_cast<Count&>(countBase);
  count.objects->trace(trc);
  count.scripts->trace(trc);
  count.strings->trace(trc);
  count.other->trace(trc);
  count.domNode->trace(trc);
}

bool ByCoarseType::count(CountBase& countBase,
                         mozilla::MallocSizeOf mallocSizeOf, const Node& node) {
  Count& count = static_cast<Count&>(countBase);

  switch (node.coarseType()) {
    case JS::ubi::CoarseType::Object:
      return count.objects->count(mallocSizeOf, node);
    case JS::ubi::CoarseType::Script:
      return count.scripts->count(mallocSizeOf, node);
    case JS::ubi::CoarseType::String:
      return count.strings->count(mallocSizeOf, node);
    case JS::ubi::CoarseType::Other:
      return count.other->count(mallocSizeOf, node);
    case JS::ubi::CoarseType::DOMNode:
      return count.domNode->count(mallocSizeOf, node);
    default:
      MOZ_CRASH("bad JS::ubi::CoarseType in JS::ubi::ByCoarseType::count");
      return false;
  }
}

bool ByCoarseType::report(JSContext* cx, CountBase& countBase,
                          MutableHandleValue report) {
  Count& count = static_cast<Count&>(countBase);

  RootedObject obj(cx, JS_NewPlainObject(cx));
  if (!obj) {
    return false;
  }

  RootedValue objectsReport(cx);
  if (!count.objects->report(cx, &objectsReport) ||
      !DefineReportProperty(cx, obj, "objects", objectsReport)) {
    return false;
  }

  RootedValue scriptsReport(cx);
  if (!count.scripts->report(cx, &scriptsReport) ||
      !DefineReportProperty(cx, obj, "scripts", scriptsReport)) {
    return false;
  }

  RootedValue stringsReport(cx);
  if (!count.strings->report(cx, &stringsReport) ||
      !DefineReportProperty(cx, obj, "strings", stringsReport)) {
    return false;
  }

  RootedValue otherReport(cx);
  if (!count.other->report(cx, &otherReport) ||
      !DefineReportProperty(cx, obj, "other", otherReport)) {
    return false;
  }

  RootedValue domReport(cx);
  if (!count.domNode->report(cx, &domReport) ||
      !DefineReportProperty(cx, obj, "domNode", domReport)) {
    return false;
  }

  report.setObject(*obj);
  return true;
}

template <typename Entry>
static int compareEntries(const void* lhsVoid, const void* rhsVoid) {
  auto lhs = (*static_cast<const Entry* const*>(lhsVoid))
                 ->value()
                 ->smallestNodeIdCounted_;
  auto rhs = (*static_cast<const Entry* const*>(rhsVoid))
                 ->value()
                 ->smallestNodeIdCounted_;

  if (lhs < rhs) {
    return 1;
  }
  if (lhs > rhs) {
    return -1;
  }
  return 0;
}

using CStringCountMap = js::HashMap<const char*, CountBasePtr,
                                    mozilla::CStringHasher,
                                    js::SystemAllocPolicy>;

template <class Map, class GetName>
static JSObject* countMapToObject(JSContext* cx, Map& map, GetName getName) {
  JS::ubi::Vector<typename Map::Entry*> entries;
  if (!entries.reserve(map.count())) {
    JS_ReportOutOfMemory(cx);
    return nullptr;
  }

  for (auto iter = map.iter(); !iter.done(); iter.next()) {
    entries.infallibleAppend(&iter.get());
  }

  if (entries.length()) {
    qsort(entries.begin(), entries.length(), sizeof(*entries.begin()),
          compareEntries<typename Map::Entry>);
  }

  RootedObject obj(cx, JS_NewPlainObject(cx));
  if (!obj) {
    return nullptr;
  }

  for (auto& entry : entries) {
    CountBasePtr& thenCount = entry->value();
    RootedValue thenReport(cx);
    if (!thenCount->report(cx, &thenReport)) {
      return nullptr;
    }

    JSString* name = getName(entry->key());
    if (!DefineReportPropertyByString(cx, obj, name, thenReport)) {
      return nullptr;
    }
  }

  return obj;
}

class ByObjectClass : public CountType {
  using Table = CStringCountMap;
  using Entry = Table::Entry;

  struct Count : public CountBase {
    Table table;
    CountBasePtr other;

    Count(CountType& type, CountBasePtr& other)
        : CountBase(type), other(std::move(other)) {}
  };

  CountTypePtr classesType;
  CountTypePtr otherType;

 public:
  ByObjectClass(CountTypePtr& classesType, CountTypePtr& otherType)
      : classesType(std::move(classesType)), otherType(std::move(otherType)) {}

  void destructCount(CountBase& countBase) override {
    Count& count = static_cast<Count&>(countBase);
    count.~Count();
  }

  CountBasePtr makeCount() override;
  void traceCount(CountBase& countBase, JSTracer* trc) override;
  bool count(CountBase& countBase, mozilla::MallocSizeOf mallocSizeOf,
             const Node& node) override;
  bool report(JSContext* cx, CountBase& countBase,
              MutableHandleValue report) override;
};

CountBasePtr ByObjectClass::makeCount() {
  CountBasePtr otherCount(otherType->makeCount());
  if (!otherCount) {
    return nullptr;
  }

  auto count = js::MakeUnique<Count>(*this, otherCount);
  if (!count) {
    return nullptr;
  }

  return CountBasePtr(count.release());
}

void ByObjectClass::traceCount(CountBase& countBase, JSTracer* trc) {
  Count& count = static_cast<Count&>(countBase);
  for (auto iter = count.table.iter(); !iter.done(); iter.next()) {
    iter.get().value()->trace(trc);
  }
  count.other->trace(trc);
}

bool ByObjectClass::count(CountBase& countBase,
                          mozilla::MallocSizeOf mallocSizeOf,
                          const Node& node) {
  Count& count = static_cast<Count&>(countBase);

  const char* className = node.jsObjectClassName();
  if (!className) {
    return count.other->count(mallocSizeOf, node);
  }

  Table::AddPtr p = count.table.lookupForAdd(className);
  if (!p) {
    CountBasePtr classCount(classesType->makeCount());
    if (!classCount || !count.table.add(p, className, std::move(classCount))) {
      return false;
    }
  }
  return p->value()->count(mallocSizeOf, node);
}

bool ByObjectClass::report(JSContext* cx, CountBase& countBase,
                           MutableHandleValue report) {
  Count& count = static_cast<Count&>(countBase);

  RootedObject obj(cx,
                   countMapToObject(cx, count.table, [cx](const char* key) {
                     MOZ_ASSERT(key);
                     return JS_AtomizeStringN(cx, key, strlen(key));
                   }));
  if (!obj) {
    return false;
  }

  RootedValue otherReport(cx);
  if (!count.other->report(cx, &otherReport) ||
      !DefineReportProperty(cx, obj, "other", otherReport)) {
    return false;
  }

  report.setObject(*obj);
  return true;
}

class ByDomObjectClass : public CountType {
  using UniqueC16String = JS::UniqueTwoByteChars;

  struct UniqueC16StringHasher {
    using Lookup = UniqueC16String;

    static js::HashNumber hash(const Lookup& lookup) {
      return mozilla::HashString(lookup.get(), Char16Length(lookup.get()));
    }

    static bool match(const UniqueC16String& key, const Lookup& lookup) {
      size_t keyLength = Char16Length(key.get());
      size_t lookupLength = Char16Length(lookup.get());
      return keyLength == lookupLength &&
             memcmp(key.get(), lookup.get(), keyLength * sizeof(char16_t)) ==
                 0;
    }
  };

  using Table = js::HashMap<UniqueC16String, CountBasePtr,
                            UniqueC16StringHasher, js::SystemAllocPolicy>;
  using Entry = Table::Entry;

  struct Count : public CountBase {
    Table table;

    explicit Count(CountType& type) : CountBase(type) {}
  };

  CountTypePtr classesType;

 public:
  explicit ByDomObjectClass(CountTypePtr& classesType)
      : classesType(std::move(classesType)) {}

  void destructCount(CountBase& countBase) override {
    Count& count = static_cast<Count&>(countBase);
    count.~Count();
  }

  CountBasePtr makeCount() override;
  void traceCount(CountBase& countBase, JSTracer* trc) override;
  bool count(CountBase& countBase, mozilla::MallocSizeOf mallocSizeOf,
             const Node& node) override;
  bool report(JSContext* cx, CountBase& countBase,
              MutableHandleValue report) override;
};

CountBasePtr ByDomObjectClass::makeCount() {
  auto count = js::MakeUnique<Count>(*this);
  if (!count) {
    return nullptr;
  }

  return CountBasePtr(count.release());
}

void ByDomObjectClass::traceCount(CountBase& countBase, JSTracer* trc) {
  Count& count = static_cast<Count&>(countBase);
  for (auto iter = count.table.iter(); !iter.done(); iter.next()) {
    iter.get().value()->trace(trc);
  }
}

bool ByDomObjectClass::count(CountBase& countBase,
                             mozilla::MallocSizeOf mallocSizeOf,
                             const Node& node) {
  Count& count = static_cast<Count&>(countBase);

  const char16_t* nodeName = node.descriptiveTypeName();
  if (!nodeName) {
    return false;
  }

  UniqueC16String name = DuplicateChar16String(nodeName);
  if (!name) {
    return false;
  }

  Table::AddPtr p = count.table.lookupForAdd(name);
  if (!p) {
    CountBasePtr classesCount(classesType->makeCount());
    if (!classesCount ||
        !count.table.add(p, std::move(name), std::move(classesCount))) {
      return false;
    }
  }
  return p->value()->count(mallocSizeOf, node);
}

bool ByDomObjectClass::report(JSContext* cx, CountBase& countBase,
                              MutableHandleValue report) {
  Count& count = static_cast<Count&>(countBase);

  JSObject* obj =
      countMapToObject(cx, count.table, [cx](const UniqueC16String& key) {
        const char16_t* chars = key.get();
        MOZ_ASSERT(chars);
        return JS_AtomizeUCStringN(cx, chars, Char16Length(chars));
      });
  if (!obj) {
    return false;
  }

  report.setObject(*obj);
  return true;
}

class ByUbinodeType : public CountType {
  using Table = js::HashMap<const char16_t*, CountBasePtr,
                            js::DefaultHasher<const char16_t*>,
                            js::SystemAllocPolicy>;
  using Entry = Table::Entry;

  struct Count : public CountBase {
    Table table;

    explicit Count(CountType& type) : CountBase(type) {}
  };

  CountTypePtr entryType;

 public:
  explicit ByUbinodeType(CountTypePtr& entryType)
      : entryType(std::move(entryType)) {}

  void destructCount(CountBase& countBase) override {
    Count& count = static_cast<Count&>(countBase);
    count.~Count();
  }

  CountBasePtr makeCount() override;
  void traceCount(CountBase& countBase, JSTracer* trc) override;
  bool count(CountBase& countBase, mozilla::MallocSizeOf mallocSizeOf,
             const Node& node) override;
  bool report(JSContext* cx, CountBase& countBase,
              MutableHandleValue report) override;
};

CountBasePtr ByUbinodeType::makeCount() {
  auto count = js::MakeUnique<Count>(*this);
  if (!count) {
    return nullptr;
  }

  return CountBasePtr(count.release());
}

void ByUbinodeType::traceCount(CountBase& countBase, JSTracer* trc) {
  Count& count = static_cast<Count&>(countBase);
  for (auto iter = count.table.iter(); !iter.done(); iter.next()) {
    iter.get().value()->trace(trc);
  }
}

bool ByUbinodeType::count(CountBase& countBase,
                          mozilla::MallocSizeOf mallocSizeOf,
                          const Node& node) {
  Count& count = static_cast<Count&>(countBase);

  const char16_t* key = node.typeName();
  MOZ_ASSERT(key);
  Table::AddPtr p = count.table.lookupForAdd(key);
  if (!p) {
    CountBasePtr typesCount(entryType->makeCount());
    if (!typesCount || !count.table.add(p, key, std::move(typesCount))) {
      return false;
    }
  }
  return p->value()->count(mallocSizeOf, node);
}

bool ByUbinodeType::report(JSContext* cx, CountBase& countBase,
                           MutableHandleValue report) {
  Count& count = static_cast<Count&>(countBase);

  JS::ubi::Vector<Entry*> entries;
  if (!entries.reserve(count.table.count())) {
    return false;
  }
  for (auto iter = count.table.iter(); !iter.done(); iter.next()) {
    entries.infallibleAppend(&iter.get());
  }
  if (entries.length()) {
    qsort(entries.begin(), entries.length(), sizeof(*entries.begin()),
          compareEntries<Entry>);
  }

  RootedObject obj(cx, JS_NewPlainObject(cx));
  if (!obj) {
    return false;
  }
  for (Entry** entryPtr = entries.begin(); entryPtr < entries.end();
       entryPtr++) {
    Entry& entry = **entryPtr;
    CountBasePtr& typeCount = entry.value();
    RootedValue typeReport(cx);
    if (!typeCount->report(cx, &typeReport)) {
      return false;
    }

    const char16_t* name = entry.key();
    MOZ_ASSERT(name);
    JSString* atom = JS_AtomizeUCStringN(cx, name, Char16Length(name));
    if (!DefineReportPropertyByString(cx, obj, atom, typeReport)) {
      return false;
    }
  }

  report.setObject(*obj);
  return true;
}

class ByAllocationStack : public CountType {
  using Table = js::HashMap<StackFrame, CountBasePtr,
                            js::DefaultHasher<StackFrame>,
                            js::SystemAllocPolicy>;
  using Entry = Table::Entry;

  struct Count : public CountBase {
    Table table;
    CountBasePtr noStack;

    Count(CountType& type, CountBasePtr& noStack)
        : CountBase(type), noStack(std::move(noStack)) {}
  };

  CountTypePtr entryType;
  CountTypePtr noStackType;

 public:
  ByAllocationStack(CountTypePtr& entryType, CountTypePtr& noStackType)
      : entryType(std::move(entryType)), noStackType(std::move(noStackType)) {}

  void destructCount(CountBase& countBase) override {
    Count& count = static_cast<Count&>(countBase);
    count.~Count();
  }

  CountBasePtr makeCount() override;
  void traceCount(CountBase& countBase, JSTracer* trc) override;
  bool count(CountBase& countBase, mozilla::MallocSizeOf mallocSizeOf,
             const Node& node) override;
  bool report(JSContext* cx, CountBase& countBase,
              MutableHandleValue report) override;
};

CountBasePtr ByAllocationStack::makeCount() {
  CountBasePtr noStackCount(noStackType->makeCount());
  if (!noStackCount) {
    return nullptr;
  }

  auto count = js::MakeUnique<Count>(*this, noStackCount);
  if (!count) {
    return nullptr;
  }
  return CountBasePtr(count.release());
}

void ByAllocationStack::traceCount(CountBase& countBase, JSTracer* trc) {
  Count& count = static_cast<Count&>(countBase);
  for (auto iter = count.table.iter(); !iter.done(); iter.next()) {
    iter.get().value()->trace(trc);

    const StackFrame* key = &iter.get().key();
    auto& k = *const_cast<StackFrame*>(key);
    k.trace(trc);
  }
  count.noStack->trace(trc);
}

bool ByAllocationStack::count(CountBase& countBase,
                              mozilla::MallocSizeOf mallocSizeOf,
                              const Node& node) {
  Count& count = static_cast<Count&>(countBase);

  if (node.hasAllocationStack()) {
    auto allocationStack = node.allocationStack();
    auto p = count.table.lookupForAdd(allocationStack);
    if (!p) {
      CountBasePtr stackCount(entryType->makeCount());
      if (!stackCount ||
          !count.table.add(p, allocationStack, std::move(stackCount))) {
        return false;
      }
    }
    MOZ_ASSERT(p);
    return p->value()->count(mallocSizeOf, node);
  }

  return count.noStack->count(mallocSizeOf, node);
}

bool ByAllocationStack::report(JSContext* cx, CountBase& countBase,
                               MutableHandleValue report) {
  Count& count = static_cast<Count&>(countBase);

#ifdef DEBUG
  mozilla::Generation generation = count.table.generation();
#endif

  JS::ubi::Vector<Entry*> entries;
  if (!entries.reserve(count.table.count())) {
    return false;
  }
  for (auto iter = count.table.iter(); !iter.done(); iter.next()) {
    entries.infallibleAppend(&iter.get());
  }
  if (entries.length()) {
    qsort(entries.begin(), entries.length(), sizeof(*entries.begin()),
          compareEntries<Entry>);
  }

  RootedObject map(cx, NewMapObject(cx));
  if (!map) {
    return false;
  }
  for (Entry** entryPtr = entries.begin(); entryPtr < entries.end();
       entryPtr++) {
    Entry& entry = **entryPtr;
    MOZ_ASSERT(entry.key());

    RootedObject stack(cx);
    if (!entry.key().constructSavedFrameStack(cx, &stack) ||
        !JS_WrapObject(cx, &stack)) {
      return false;
    }
    RootedValue stackVal(cx, ObjectValue(*stack));

    CountBasePtr& stackCount = entry.value();
    RootedValue stackReport(cx);
    if (!stackCount->report(cx, &stackReport)) {
      return false;
    }

    if (!MapSet(cx, map, stackVal, stackReport)) {
      return false;
    }
  }

  if (count.noStack->total_ > 0) {
    RootedValue noStackReport(cx);
    if (!count.noStack->report(cx, &noStackReport)) {
      return false;
    }
    JSString* noStackName = JS_AtomizeString(cx, "noStack");
    if (!noStackName) {
      return false;
    }
    RootedValue noStack(cx, StringValue(noStackName));
    if (!MapSet(cx, map, noStack, noStackReport)) {
      return false;
    }
  }

  MOZ_ASSERT(generation == count.table.generation());

  report.setObject(*map);
  return true;
}

class ByFilename : public CountType {
  using UniqueCString = JS::UniqueChars;

  struct UniqueCStringHasher {
    using Lookup = UniqueCString;

    static js::HashNumber hash(const Lookup& lookup) {
      return mozilla::CStringHasher::hash(lookup.get());
    }

    static bool match(const UniqueCString& key, const Lookup& lookup) {
      return mozilla::CStringHasher::match(key.get(), lookup.get());
    }
  };

  using Table = js::HashMap<UniqueCString, CountBasePtr, UniqueCStringHasher,
                            js::SystemAllocPolicy>;
  using Entry = Table::Entry;

  struct Count : public CountBase {
    Table table;
    CountBasePtr then;
    CountBasePtr noFilename;

    Count(CountType& type, CountBasePtr&& then, CountBasePtr&& noFilename)
        : CountBase(type),
          then(std::move(then)),
          noFilename(std::move(noFilename)) {}
  };

  CountTypePtr thenType;
  CountTypePtr noFilenameType;

 public:
  ByFilename(CountTypePtr&& thenType, CountTypePtr&& noFilenameType)
      : thenType(std::move(thenType)),
        noFilenameType(std::move(noFilenameType)) {}

  void destructCount(CountBase& countBase) override {
    Count& count = static_cast<Count&>(countBase);
    count.~Count();
  }

  CountBasePtr makeCount() override;
  void traceCount(CountBase& countBase, JSTracer* trc) override;
  bool count(CountBase& countBase, mozilla::MallocSizeOf mallocSizeOf,
             const Node& node) override;
  bool report(JSContext* cx, CountBase& countBase,
              MutableHandleValue report) override;
};

CountBasePtr ByFilename::makeCount() {
  CountBasePtr thenCount(thenType->makeCount());
  if (!thenCount) {
    return nullptr;
  }

  CountBasePtr noFilenameCount(noFilenameType->makeCount());
  if (!noFilenameCount) {
    return nullptr;
  }

  auto count = js::MakeUnique<Count>(*this, std::move(thenCount),
                                     std::move(noFilenameCount));
  if (!count) {
    return nullptr;
  }

  return CountBasePtr(count.release());
}

void ByFilename::traceCount(CountBase& countBase, JSTracer* trc) {
  Count& count = static_cast<Count&>(countBase);
  for (auto iter = count.table.iter(); !iter.done(); iter.next()) {
    iter.get().value()->trace(trc);
  }
  count.noFilename->trace(trc);
}

bool ByFilename::count(CountBase& countBase, mozilla::MallocSizeOf mallocSizeOf,
                       const Node& node) {
  Count& count = static_cast<Count&>(countBase);

  const char* filename = node.scriptFilename();
  if (!filename) {
    return count.noFilename->count(mallocSizeOf, node);
  }

  UniqueCString myFilename = DuplicateCString(filename);
  if (!myFilename) {
    return false;
  }

  Table::AddPtr p = count.table.lookupForAdd(myFilename);
  if (!p) {
    CountBasePtr thenCount(thenType->makeCount());
    if (!thenCount ||
        !count.table.add(p, std::move(myFilename), std::move(thenCount))) {
      return false;
    }
  }
  return p->value()->count(mallocSizeOf, node);
}

bool ByFilename::report(JSContext* cx, CountBase& countBase,
                        MutableHandleValue report) {
  Count& count = static_cast<Count&>(countBase);

  RootedObject obj(
      cx, countMapToObject(cx, count.table, [cx](const UniqueCString& key) {
        const char* utf8chars = key.get();
        return JS_NewStringCopyUTF8Z(
            cx, JS::ConstUTF8CharsZ(utf8chars, strlen(utf8chars)));
      }));
  if (!obj) {
    return false;
  }

  RootedValue noFilenameReport(cx);
  if (!count.noFilename->report(cx, &noFilenameReport) ||
      !DefineReportProperty(cx, obj, "noFilename", noFilenameReport)) {
    return false;
  }

  report.setObject(*obj);
  return true;
}

JS_PUBLIC_API bool CensusHandler::operator()(
    BreadthFirst<CensusHandler>& traversal, Node origin, const Edge& edge,
    NodeData* referentData, bool first) {
  if (!first) {
    return true;
  }

  const Node& referent = edge.referent;
  Zone* zone = referent.zone();

  if (census.targetZones.count() == 0 || census.targetZones.has(zone)) {
    return rootCount->count(mallocSizeOf, referent);
  }

  if (zone && JS::shadow::Zone::from(zone)->isAtomsZone()) {
    traversal.abandonReferent();
    return rootCount->count(mallocSizeOf, referent);
  }

  traversal.abandonReferent();
  return true;
}

static CountTypePtr ParseChildBreakdown(
    JSContext* cx, HandleObject breakdown, const char* prop,
    MutableHandle<GCVector<JSLinearString*>> seen) {
  RootedValue v(cx);
  if (!JS_GetProperty(cx, breakdown, prop, &v)) {
    return nullptr;
  }
  return ParseBreakdown(cx, v, seen);
}

JS_PUBLIC_API CountTypePtr
ParseBreakdown(JSContext* cx, HandleValue breakdownValue,
               MutableHandle<GCVector<JSLinearString*>> seen) {
  if (breakdownValue.isUndefined()) {
    CountTypePtr simple(NewCountType<SimpleCount>(cx));
    return simple;
  }

  RootedObject breakdown(cx, ToObject(cx, breakdownValue));
  if (!breakdown) {
    return nullptr;
  }

  RootedValue byValue(cx);
  if (!JS_GetProperty(cx, breakdown, "by", &byValue)) {
    return nullptr;
  }
  RootedString byString(cx, ToString(cx, byValue));
  if (!byString) {
    return nullptr;
  }
  Rooted<JSLinearString*> by(cx, JS_EnsureLinearString(cx, byString));
  if (!by) {
    return nullptr;
  }

  for (auto candidate : seen.get()) {
    if (LinearStringsEqual(by, candidate)) {
      UniqueChars byBytes = QuoteLinearString(cx, by);
      if (!byBytes) {
        return nullptr;
      }

      JS_ReportErrorNumberASCII(cx, GetCensusErrorMessage, nullptr,
                                VgeCensusErrBreakdownNested, byBytes.get());
      return nullptr;
    }
  }
  if (!seen.append(by)) {
    return nullptr;
  }
  auto popper = mozilla::MakeScopeExit([&]() { seen.popBack(); });

  if (JS_LinearStringEqualsLiteral(by, "count")) {
    RootedValue countValue(cx), bytesValue(cx);
    if (!JS_GetProperty(cx, breakdown, "count", &countValue) ||
        !JS_GetProperty(cx, breakdown, "bytes", &bytesValue)) {
      return nullptr;
    }

    if (countValue.isUndefined()) countValue.setBoolean(true);
    if (bytesValue.isUndefined()) bytesValue.setBoolean(true);

    RootedValue label(cx);
    if (!JS_GetProperty(cx, breakdown, "label", &label)) {
      return nullptr;
    }

    UniqueTwoByteChars labelUnique(nullptr);
    if (!label.isUndefined()) {
      RootedString labelString(cx, ToString(cx, label));
      if (!labelString) {
        return nullptr;
      }

      labelUnique = JS_CopyStringCharsZ(cx, labelString);
      if (!labelUnique) {
        return nullptr;
      }
    }

    CountTypePtr simple(NewCountType<SimpleCount>(
        cx, labelUnique, ToBoolean(countValue), ToBoolean(bytesValue)));
    return simple;
  }

  if (JS_LinearStringEqualsLiteral(by, "bucket")) {
    return CountTypePtr(NewCountType<BucketCount>(cx));
  }

  if (JS_LinearStringEqualsLiteral(by, "objectClass")) {
    CountTypePtr thenType(ParseChildBreakdown(cx, breakdown, "then", seen));
    if (!thenType) {
      return nullptr;
    }

    CountTypePtr otherType(ParseChildBreakdown(cx, breakdown, "other", seen));
    if (!otherType) {
      return nullptr;
    }

    return CountTypePtr(NewCountType<ByObjectClass>(cx, thenType, otherType));
  }

  if (JS_LinearStringEqualsLiteral(by, "coarseType")) {
    CountTypePtr objectsType(
        ParseChildBreakdown(cx, breakdown, "objects", seen));
    if (!objectsType) {
      return nullptr;
    }
    CountTypePtr scriptsType(
        ParseChildBreakdown(cx, breakdown, "scripts", seen));
    if (!scriptsType) {
      return nullptr;
    }
    CountTypePtr stringsType(
        ParseChildBreakdown(cx, breakdown, "strings", seen));
    if (!stringsType) {
      return nullptr;
    }
    CountTypePtr otherType(ParseChildBreakdown(cx, breakdown, "other", seen));
    if (!otherType) {
      return nullptr;
    }
    CountTypePtr domNodeType(
        ParseChildBreakdown(cx, breakdown, "domNode", seen));
    if (!domNodeType) {
      return nullptr;
    }

    return CountTypePtr(NewCountType<ByCoarseType>(
        cx, objectsType, scriptsType, stringsType, otherType, domNodeType));
  }

  if (JS_LinearStringEqualsLiteral(by, "internalType")) {
    CountTypePtr thenType(ParseChildBreakdown(cx, breakdown, "then", seen));
    if (!thenType) {
      return nullptr;
    }

    return CountTypePtr(NewCountType<ByUbinodeType>(cx, thenType));
  }

  if (JS_LinearStringEqualsLiteral(by, "descriptiveType")) {
    CountTypePtr thenType(ParseChildBreakdown(cx, breakdown, "then", seen));
    if (!thenType) {
      return nullptr;
    }
    return CountTypePtr(NewCountType<ByDomObjectClass>(cx, thenType));
  }

  if (JS_LinearStringEqualsLiteral(by, "allocationStack")) {
    CountTypePtr thenType(ParseChildBreakdown(cx, breakdown, "then", seen));
    if (!thenType) {
      return nullptr;
    }
    CountTypePtr noStackType(
        ParseChildBreakdown(cx, breakdown, "noStack", seen));
    if (!noStackType) {
      return nullptr;
    }

    return CountTypePtr(
        NewCountType<ByAllocationStack>(cx, thenType, noStackType));
  }

  if (JS_LinearStringEqualsLiteral(by, "filename")) {
    CountTypePtr thenType(ParseChildBreakdown(cx, breakdown, "then", seen));
    if (!thenType) {
      return nullptr;
    }

    CountTypePtr noFilenameType(
        ParseChildBreakdown(cx, breakdown, "noFilename", seen));
    if (!noFilenameType) {
      return nullptr;
    }

    return CountTypePtr(NewCountType<ByFilename>(cx, std::move(thenType),
                                                 std::move(noFilenameType)));
  }

  UniqueChars byBytes = QuoteLinearString(cx, by);
  if (!byBytes) {
    return nullptr;
  }

  JS_ReportErrorNumberASCII(cx, GetCensusErrorMessage, nullptr,
                            VgeCensusErrBreakdown, byBytes.get());
  return nullptr;
}

static CountTypePtr GetDefaultBreakdown(JSContext* cx) {
  CountTypePtr byDomClass(NewCountType<SimpleCount>(cx));
  if (!byDomClass) {
    return nullptr;
  }
  CountTypePtr byClass(NewCountType<SimpleCount>(cx));
  if (!byClass) {
    return nullptr;
  }

  CountTypePtr byClassElse(NewCountType<SimpleCount>(cx));
  if (!byClassElse) {
    return nullptr;
  }

  CountTypePtr objects(NewCountType<ByObjectClass>(cx, byClass, byClassElse));
  if (!objects) {
    return nullptr;
  }

  CountTypePtr scripts(NewCountType<SimpleCount>(cx));
  if (!scripts) {
    return nullptr;
  }

  CountTypePtr strings(NewCountType<SimpleCount>(cx));
  if (!strings) {
    return nullptr;
  }

  CountTypePtr byType(NewCountType<SimpleCount>(cx));
  if (!byType) {
    return nullptr;
  }

  CountTypePtr other(NewCountType<ByUbinodeType>(cx, byType));
  if (!other) {
    return nullptr;
  }
  CountTypePtr domNode(NewCountType<ByDomObjectClass>(cx, byDomClass));
  if (!domNode) {
    return nullptr;
  }

  return CountTypePtr(NewCountType<ByCoarseType>(cx, objects, scripts, strings,
                                                 other, domNode));
}

JS_PUBLIC_API bool ParseCensusOptions(JSContext* cx, Census& census,
                                      HandleObject options,
                                      CountTypePtr& outResult) {
  RootedValue breakdown(cx, UndefinedValue());
  if (options && !JS_GetProperty(cx, options, "breakdown", &breakdown)) {
    return false;
  }

  Rooted<GCVector<JSLinearString*>> seen(cx, cx);
  outResult = breakdown.isUndefined() ? GetDefaultBreakdown(cx)
                                      : ParseBreakdown(cx, breakdown, &seen);
  return !!outResult;
}

}
}
