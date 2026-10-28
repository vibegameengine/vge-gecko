#include "VgeJsGcInternal.h"

#include "VgeJsScripts.h"
#include "js/Object.h"
#include "js/Realm.h"
#include "js/UbiNode.h"
#include "js/UbiNodeShortestPaths.h"
#include "js/Utility.h"

namespace vge::jsapi::gc {

namespace {

enum SavedFrameSlot : uint32_t {
  kSource,
  kSourceId,
  kLine,
  kColumn,
  kDisplayName,
  kAsyncCause,
  kParent,
  kPrincipals,
  kMuted,
};

char16_t* DuplicateTwoByte(const char16_t* aChars) {
  size_t length = std::char_traits<char16_t>::length(aChars);
  char16_t* out = js_pod_malloc<char16_t>(length + 1);
  if (!out) {
    return nullptr;
  }
  memcpy(out, aChars, (length + 1) * sizeof(char16_t));
  return out;
}

class EdgeVectorTracer final : public JS::CallbackTracer {
 public:
  EdgeVectorTracer(JSRuntime* aRuntime, JS::ubi::EdgeVector* aVec,
                   bool aWantNames)
      : JS::CallbackTracer(aRuntime), mVec(aVec), mWantNames(aWantNames) {}

  bool okay = true;

  bool onChild(JS::GCCellPtr aThing, const char* aName) override {
    if (!okay || !aThing.is<JSObject>() || !LiveTail(aThing)) {
      return true;
    }
    char16_t* name16 = nullptr;
    if (mWantNames) {
      char buffer[1024];
      context().getEdgeName(aName, buffer, sizeof(buffer));
      size_t length = strlen(buffer);
      name16 = js_pod_malloc<char16_t>(length + 1);
      if (!name16) {
        okay = false;
        return true;
      }
      for (size_t i = 0; i <= length; i++) {
        name16[i] = char16_t(uint8_t(buffer[i]));
      }
    }
    if (!mVec->append(
            JS::ubi::Edge(name16, JS::ubi::Node(&aThing.as<JSObject>())))) {
      okay = false;
    }
    return true;
  }

 private:
  JS::ubi::EdgeVector* mVec;
  bool mWantNames;
};

class SimpleEdgeRange final : public JS::ubi::EdgeRange {
 public:
  JS::ubi::EdgeVector edges;
  size_t i = 0;

  void settle() { front_ = i < edges.length() ? &edges[i] : nullptr; }
  void popFront() override {
    i++;
    settle();
  }
};

JSString* NewTwoByteString(JSContext* aCx, JS::ubi::AtomOrTwoByteChars aChars) {
  size_t length = aChars.length();
  std::u16string text(length, u'\0');
  if (length) {
    aChars.copyToBuffer(mozilla::RangedPtr<char16_t>(text.data(), length),
                        length);
  }
  return NewJSStringUtf16(aCx, text);
}

}

}

using namespace vge::jsapi;
using namespace vge::jsapi::gc;

namespace JS::ubi {

size_t AtomOrTwoByteChars::length() {
  if (is<JSAtom*>()) {
    JSAtom* atom = as<JSAtom*>();
    return atom ? StringCell::from(reinterpret_cast<JSString*>(atom))->length()
                : 0;
  }
  const char16_t* chars = as<const char16_t*>();
  return chars ? std::char_traits<char16_t>::length(chars) : 0;
}

size_t AtomOrTwoByteChars::copyToBuffer(mozilla::RangedPtr<char16_t> destination,
                                        size_t length) {
  if (is<JSAtom*>()) {
    JSAtom* atom = as<JSAtom*>();
    if (!atom) {
      return 0;
    }
    StringCell* cell = StringCell::from(reinterpret_cast<JSString*>(atom));
    size_t n = std::min<size_t>(cell->length(), length);
    for (size_t i = 0; i < n; i++) {
      destination[i] = cell->latin1()
                           ? char16_t(static_cast<const uint8_t*>(cell->chars)[i])
                           : static_cast<const char16_t*>(cell->chars)[i];
    }
    return n;
  }
  const char16_t* chars = as<const char16_t*>();
  if (!chars) {
    return 0;
  }
  size_t n = std::min(std::char_traits<char16_t>::length(chars), length);
  for (size_t i = 0; i < n; i++) {
    destination[i] = chars[i];
  }
  return n;
}

bool ConstructSavedFrameStackSlow(JSContext* cx, StackFrame& frame,
                                  MutableHandleObject outSavedFrameStack) {
  std::vector<StackFrame> frames;
  for (StackFrame f = frame; f; f = f.parent()) {
    frames.push_back(f);
  }
  JS::RootedObject parent(cx, nullptr);
  for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
    StackFrame& f = *it;
    JS::RootedObject saved(cx, NewClassObject(cx, &SavedFrameClass, nullptr));
    if (!saved) {
      return false;
    }
    JS::RootedString source(cx, NewTwoByteString(cx, f.source()));
    if (!source) {
      return false;
    }
    JS::SetReservedSlot(saved, kSource, JS::StringValue(source));
    JS::SetReservedSlot(saved, kSourceId, JS::Int32Value(int32_t(f.sourceId())));
    JS::SetReservedSlot(saved, kLine, JS::Int32Value(int32_t(f.line())));
    JS::SetReservedSlot(
        saved, kColumn,
        JS::Int32Value(int32_t(f.column().oneOriginValue())));
    AtomOrTwoByteChars name = f.functionDisplayName();
    if (name.length()) {
      JSString* nameStr = NewTwoByteString(cx, name);
      if (!nameStr) {
        return false;
      }
      JS::SetReservedSlot(saved, kDisplayName,
                          JS::StringValue(AtomizeCell(cx, nameStr)));
    } else {
      JS::SetReservedSlot(saved, kDisplayName, JS::NullValue());
    }
    JS::SetReservedSlot(saved, kAsyncCause, JS::NullValue());
    JS::SetReservedSlot(saved, kParent,
                        parent ? JS::ObjectValue(*parent) : JS::NullValue());
    JS::SetReservedSlot(saved, kMuted, JS::BooleanValue(false));
    parent = saved;
  }
  outSavedFrameStack.set(parent);
  return true;
}

CoarseType Concrete<void>::coarseType() const { MOZ_CRASH("null ubi::Node"); }

const char16_t* Concrete<void>::typeName() const {
  MOZ_CRASH("null ubi::Node");
}

JS::Zone* Concrete<void>::zone() const { MOZ_CRASH("null ubi::Node"); }

JS::Compartment* Concrete<void>::compartment() const {
  MOZ_CRASH("null ubi::Node");
}

JS::Realm* Concrete<void>::realm() const { MOZ_CRASH("null ubi::Node"); }

js::UniquePtr<EdgeRange> Concrete<void>::edges(JSContext*, bool) const {
  MOZ_CRASH("null ubi::Node");
}

Node::Size Concrete<void>::size(mozilla::MallocSizeOf mallocSizeof) const {
  MOZ_CRASH("null ubi::Node");
}

template <>
JS::Zone* TracerConcrete<JSObject>::zone() const {
  return JS::GetObjectZone(&get());
}

template <>
js::UniquePtr<EdgeRange> TracerConcrete<JSObject>::edges(JSContext* cx,
                                                         bool wantNames) const {
  auto range = js::MakeUnique<SimpleEdgeRange>();
  if (!range) {
    return nullptr;
  }
  EdgeVectorTracer tracer(cx->runtime, &range->edges, wantNames);
  JS::TraceChildren(&tracer, JS::GCCellPtr(&get()));
  if (!tracer.okay) {
    return nullptr;
  }
  range->settle();
  return js::UniquePtr<EdgeRange>(range.release());
}

const char16_t Concrete<JSObject>::concreteTypeName[] = u"JSObject";
const char16_t Concrete<RootList>::concreteTypeName[] = u"JS::ubi::RootList";

void Concrete<JSObject>::construct(void* storage, JSObject* ptr) {
  if (ptr) {
    const JSClass* clasp = JS::GetClass(ptr);
    JSRuntime* rt = vge::jsapi::Runtime();
    GcState* state = rt ? MaybeStateOf(rt) : nullptr;
    if (clasp->isDOMClass() && state && state->constructUbiNodeForDOMObject) {
      JS::AutoSuppressGCAnalysis suppress;
      state->constructUbiNodeForDOMObject(storage, ptr);
      return;
    }
  }
  new (storage) Concrete(ptr);
}

JS::Compartment* Concrete<JSObject>::compartment() const {
  return JS::GetCompartment(&get());
}

JS::Realm* Concrete<JSObject>::realm() const {
  return JS::GetObjectRealmOrNull(&get());
}

const char* Concrete<JSObject>::jsObjectClassName() const {
  return JS::GetClass(&get())->name;
}

Node::Size Concrete<JSObject>::size(mozilla::MallocSizeOf mallocSizeOf) const {
  Node::Size size = sizeof(ObjectCell);
  JSRuntime* rt = vge::jsapi::Runtime();
  GcState* state = rt ? MaybeStateOf(rt) : nullptr;
  if (state && state->ubiValid) {
    auto it = state->ubiSizes.find(reinterpret_cast<js::gc::Cell*>(&get()));
    if (it != state->ubiSizes.end()) {
      size += it->second;
    }
  }
  return size;
}

bool Concrete<JSObject>::hasAllocationStack() const { return false; }

StackFrame Concrete<JSObject>::allocationStack() const { return StackFrame(); }

RootList::RootList(JSContext* cx, bool wantNames)
    : cx(cx), wantNames(wantNames), inited(false) {}

std::pair<bool, JS::AutoCheckCannotGC> RootList::init() {
  BuildFullCellGraph(cx);
  GcState& state = StateOf(cx->runtime);
  EdgeVectorTracer tracer(cx->runtime, &edges, wantNames);
  std::vector<JS::GCCellPtr> roots = state.ubiRoots;
  for (JS::GCCellPtr root : roots) {
    tracer.onChild(root, "root");
  }
  inited = tracer.okay;
  return {tracer.okay, JS::AutoCheckCannotGC(cx)};
}

std::pair<bool, JS::AutoCheckCannotGC> RootList::init(
    CompartmentSet& debuggees) {
  EdgeVector allRootEdges;
  BuildFullCellGraph(cx);
  GcState& state = StateOf(cx->runtime);
  EdgeVectorTracer tracer(cx->runtime, &allRootEdges, wantNames);
  std::vector<JS::GCCellPtr> roots = state.ubiRoots;
  for (JS::GCCellPtr root : roots) {
    tracer.onChild(root, "root");
  }
  if (!tracer.okay) {
    return {false, JS::AutoCheckCannotGC(cx)};
  }
  for (EdgeVector::Range r = allRootEdges.all(); !r.empty(); r.popFront()) {
    Edge& edge = r.front();
    JS::Compartment* compartment = edge.referent.compartment();
    if (compartment && !debuggees.has(compartment)) {
      continue;
    }
    if (!edges.append(std::move(edge))) {
      return {false, JS::AutoCheckCannotGC(cx)};
    }
  }
  inited = true;
  return {true, JS::AutoCheckCannotGC(cx)};
}

bool RootList::addRoot(Node node, const char16_t* edgeName) {
  UniqueTwoByteChars name;
  if (edgeName) {
    name.reset(DuplicateTwoByte(edgeName));
    if (!name) {
      return false;
    }
  }
  return edges.append(Edge(name.release(), node));
}

js::UniquePtr<EdgeRange> Concrete<RootList>::edges(JSContext* cx,
                                                   bool wantNames) const {
  return js::MakeUnique<PreComputedEdgeRange>(get().edges);
}

void SetConstructUbiNodeForDOMObjectCallback(JSContext* cx,
                                             void (*callback)(void*,
                                                              JSObject*)) {
  StateOf(cx->runtime).constructUbiNodeForDOMObject = callback;
}

JS_PUBLIC_API BackEdge::Ptr BackEdge::clone() const {
  auto copy = js::MakeUnique<BackEdge>();
  if (!copy) {
    return nullptr;
  }
  copy->predecessor_ = predecessor();
  if (name()) {
    copy->name_.reset(DuplicateTwoByte(name().get()));
    if (!copy->name_) {
      return nullptr;
    }
  }
  return copy;
}

}
