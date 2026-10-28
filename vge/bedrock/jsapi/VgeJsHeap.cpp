#include "VgeJsInternal.h"

#include <windows.h>

#include "mozilla/Assertions.h"

namespace vge::jsapi {

alignas(8) const uint16_t kWrapperEmbedderId = 0x5647;

namespace {

struct Chunk : public js::gc::ArenaChunkBase {
  explicit Chunk(JSRuntime* aRuntime) : js::gc::ArenaChunkBase(aRuntime) {}
};

struct SizeClass {
  size_t size;
  CellTail* freeList = nullptr;
  uint8_t* cursor = nullptr;
  uint8_t* limit = nullptr;
};

SizeClass gClasses[] = {{sizeof(StringCell)}, {sizeof(ObjectCell)}};
std::vector<Chunk*> gChunks;

static_assert(sizeof(StringCell) == sizeof(SymbolCell));
static_assert(sizeof(StringCell) == sizeof(BigIntCell));
static_assert(offsetof(StringCell, tail) == offsetof(SymbolCell, tail));
static_assert(offsetof(StringCell, tail) == offsetof(BigIntCell, tail));
static_assert(sizeof(StringCell) % js::gc::CellAlignBytes == 0);
static_assert(sizeof(ObjectCell) % js::gc::CellAlignBytes == 0);

SizeClass& ClassFor(size_t aSize) {
  for (SizeClass& c : gClasses) {
    if (c.size == aSize) {
      return c;
    }
  }
  MOZ_CRASH("vge jsapi: unknown cell size");
}

Chunk* NewChunk() {
  void* reserve = VirtualAlloc(nullptr, js::gc::ChunkSize * 2, MEM_RESERVE,
                               PAGE_NOACCESS);
  MOZ_RELEASE_ASSERT(reserve);
  uintptr_t aligned = (uintptr_t(reserve) + js::gc::ChunkMask) &
                      ~uintptr_t(js::gc::ChunkMask);
  VirtualFree(reserve, 0, MEM_RELEASE);
  void* chunk = VirtualAlloc(reinterpret_cast<void*>(aligned),
                             js::gc::ChunkSize, MEM_RESERVE | MEM_COMMIT,
                             PAGE_READWRITE);
  if (!chunk) {
    return NewChunk();
  }
  Chunk* c = new (chunk) Chunk(Runtime());
  c->info.zone = Runtime()->zone;
  gChunks.push_back(c);
  return c;
}

}

void* AllocCell(size_t aSize) {
  SizeClass& c = ClassFor(aSize);
  if (c.freeList) {
    CellTail* tail = c.freeList;
    c.freeList = tail->nextFree;
    uint8_t* cell =
        reinterpret_cast<uint8_t*>(tail) -
        (aSize == sizeof(ObjectCell) ? offsetof(ObjectCell, tail)
                                     : offsetof(StringCell, tail));
    memset(cell, 0, aSize);
    return cell;
  }
  if (!c.cursor || c.cursor + aSize > c.limit) {
    Chunk* chunk = NewChunk();
    c.cursor = reinterpret_cast<uint8_t*>(chunk) + js::gc::FirstArenaOffset;
    c.limit = reinterpret_cast<uint8_t*>(chunk) + js::gc::ChunkSize;
  }
  void* cell = c.cursor;
  c.cursor += aSize;
  memset(cell, 0, aSize);
  return cell;
}

void FreeCell(void* aCell, size_t aSize) {
  SizeClass& c = ClassFor(aSize);
  CellTail* tail =
      aSize == sizeof(ObjectCell)
          ? &static_cast<ObjectCell*>(aCell)->tail
          : &static_cast<StringCell*>(aCell)->tail;
  tail->handle.~Global();
  ForgetTraced(tail);
  tail->nextFree = c.freeList;
  c.freeList = tail;
}

bool IsCellAddress(const void* aPtr) {
  uintptr_t chunk = uintptr_t(aPtr) & ~uintptr_t(js::gc::ChunkMask);
  for (Chunk* c : gChunks) {
    if (uintptr_t(c) == chunk) {
      return true;
    }
  }
  return false;
}

CellTail* TailOf(js::gc::Cell* aCell, JS::TraceKind aKind) {
  switch (aKind) {
    case JS::TraceKind::Object:
      return &reinterpret_cast<ObjectCell*>(aCell)->tail;
    case JS::TraceKind::String:
      return &reinterpret_cast<StringCell*>(aCell)->tail;
    case JS::TraceKind::Symbol:
      return &reinterpret_cast<SymbolCell*>(aCell)->tail;
    case JS::TraceKind::BigInt:
      return &reinterpret_cast<BigIntCell*>(aCell)->tail;
    case JS::TraceKind::Script:
      return &reinterpret_cast<StringCell*>(aCell)->tail;
    default:
      return nullptr;
  }
}

Shape* ShapeFor(const JSClass* aClass, JS::Realm* aRealm) {
  std::vector<Shape*>& list = Runtime()->shapes[aClass];
  for (Shape* s : list) {
    if (s->base.realm == aRealm) {
      return s;
    }
  }
  Shape* s = new Shape();
  s->base.clasp = aClass;
  s->base.realm = aRealm;
  s->shadow.base = &s->base;
  s->isProxy = aClass->isProxyObject();
  s->isFunction = aClass == js::FunctionClassPtr ||
                  aClass == js::FunctionExtendedClassPtr;
  s->reservedSlots = JSCLASS_RESERVED_SLOTS(aClass);
  uint32_t kind = s->isProxy ? uint32_t(JS::shadow::Shape::Kind::Proxy)
                             : uint32_t(JS::shadow::Shape::Kind::Shared);
  uint32_t fixed = s->isProxy ? 0 : uint32_t(kFixedSlots);
  s->shadow.immutableFlags =
      (kind << JS::shadow::Shape::KIND_SHIFT) |
      (fixed << JS::shadow::Shape::FIXED_SLOTS_SHIFT);
  list.push_back(s);
  return s;
}

JS::Value* DynamicSlots(ObjectCell* aCell) {
  if (aCell->shape->isProxy || aCell->shape->reservedSlots <= kFixedSlots) {
    return nullptr;
  }
  return reinterpret_cast<JS::Value*>(aCell->words[0]);
}

void FreeObjectCell(ObjectCell* aCell) {
  if (JS::Realm* realm = aCell->shape->base.realm) {
    MOZ_ASSERT(realm->liveCells > 0);
    realm->liveCells--;
  }
  if (JSRuntime* rt = Runtime()) {
    rt->objectCells.erase(aCell);
  }
  delete[] DynamicSlots(aCell);
  FreeCell(aCell, sizeof(ObjectCell));
}

void FreeStringChars(StringCell* aCell) {
  if (!aCell->external) {
    free(const_cast<void*>(aCell->chars));
    return;
  }
  if (aCell->latin1()) {
    aCell->external->finalize(const_cast<JS::Latin1Char*>(
        static_cast<const JS::Latin1Char*>(aCell->chars)));
  } else {
    aCell->external->finalize(
        const_cast<char16_t*>(static_cast<const char16_t*>(aCell->chars)));
  }
}

}
