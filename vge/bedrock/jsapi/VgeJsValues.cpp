#include "VgeJsInternal.h"

#include <string>
#include <unordered_map>

#include "js/Id.h"
#include "js/Value.h"

namespace vge::jsapi {

namespace {

v8::Local<v8::Private> CellKey(JSContext* aCx) {
  return aCx->runtime->cellKey.Get(aCx->isolate);
}

}

void BindCellHandle(JSContext* aCx, CellTail* aTail, v8::Local<v8::Value> aValue,
                bool aStrong) {
  aTail->handle.Reset(aCx->isolate, aValue);
  SetTraced(aCx->isolate, aTail, aValue);
  if (!aStrong) {
    aTail->handle.SetWeak(
        aTail,
        [](const v8::WeakCallbackInfo<CellTail>& aInfo) {
          CellTail* tail = aInfo.GetParameter();
          tail->handle.Reset();
          if (tail->anchor) {
            NoteDeadAnchoredCell(tail);
            return;
          }
          if (tail->kind == CellKind::Object) {
            FreeObjectCell(reinterpret_cast<ObjectCell*>(
                reinterpret_cast<uint8_t*>(tail) - offsetof(ObjectCell, tail)));
            return;
          }
          StringCell* cell = reinterpret_cast<StringCell*>(
              reinterpret_cast<uint8_t*>(tail) - offsetof(StringCell, tail));
          if (tail->kind == CellKind::String) {
            FreeStringChars(cell);
          }
          FreeCell(cell, sizeof(StringCell));
        },
        v8::WeakCallbackType::kParameter);
    aCx->runtime->youngTails.insert(aTail);
  }
}

namespace {

const JSClass* ClassForScriptObject(v8::Local<v8::Object> aObject) {
  if (aObject->IsArrayBuffer() || aObject->IsSharedArrayBuffer() ||
      aObject->IsArrayBufferView()) {
    return buffers::ClassForV8(aObject);
  }
  if (aObject->IsFunction()) {
    return js::FunctionClassPtr;
  }
  if (aObject->IsArray()) {
    return &ArrayClass;
  }
  return js::ObjectClassPtr;
}

StringCell* FillString(JSContext* aCx, v8::Local<v8::String> aString,
                       bool aAtom) {
  StringCell* cell = static_cast<StringCell*>(AllocCell(sizeof(StringCell)));
  uint32_t length = aString->Length();
  uintptr_t flags = JS::shadow::String::LINEAR_BIT;
  if (aString->IsOneByte()) {
    auto* chars = static_cast<uint8_t*>(malloc(length + 1));
    aString->WriteOneByte(aCx->isolate, chars, 0, length,
                          v8::String::NO_NULL_TERMINATION);
    chars[length] = 0;
    cell->chars = chars;
    flags |= JS::shadow::String::LATIN1_CHARS_BIT;
  } else {
    auto* chars = static_cast<uint16_t*>(malloc((length + 1) * 2));
    aString->Write(aCx->isolate, chars, 0, length,
                   v8::String::NO_NULL_TERMINATION);
    chars[length] = 0;
    cell->chars = chars;
  }
  if (aAtom) {
    flags |= JS::shadow::String::ATOM_BIT;
  }
  cell->flags = flags | (uintptr_t(length) << 32);
  cell->tail.kind = CellKind::String;
  cell->tail.isAtom = aAtom;
  return cell;
}

std::u16string Content(StringCell* aCell) {
  std::u16string out;
  uint32_t length = aCell->length();
  out.resize(length);
  if (aCell->latin1()) {
    auto* chars = static_cast<const uint8_t*>(aCell->chars);
    for (uint32_t i = 0; i < length; i++) {
      out[i] = chars[i];
    }
  } else {
    memcpy(out.data(), aCell->chars, length * 2);
  }
  return out;
}

}

ObjectCell* NewObjectCell(JSContext* aCx, Shape* aShape,
                          v8::Local<v8::Object> aObject, bool aAnchored) {
  ObjectCell* cell = static_cast<ObjectCell*>(AllocCell(sizeof(ObjectCell)));
  cell->shape = aShape;
  cell->tail.kind = CellKind::Object;
  if (aShape->base.realm) {
    aShape->base.realm->liveCells++;
  }
  aCx->runtime->objectCells.insert(cell);
  if (aShape->isProxy) {
    cell->proxyData()->handler = nullptr;
    cell->proxyData()->expando = nullptr;
    cell->proxyData()->values()->init(aShape->reservedSlots);
  } else {
    JS::Value* slots = cell->fixedSlots();
    for (size_t i = 0; i < kFixedSlots; i++) {
      slots[i] = JS::UndefinedValue();
    }
    if (aShape->reservedSlots > kFixedSlots) {
      size_t extra = aShape->reservedSlots - kFixedSlots;
      JS::Value* dynamic = new JS::Value[extra];
      for (size_t i = 0; i < extra; i++) {
        dynamic[i] = JS::UndefinedValue();
      }
      cell->words[0] = reinterpret_cast<uintptr_t>(dynamic);
    }
  }
  v8::Local<v8::Context> ctx = aCx->v8Context();
  aObject->SetPrivate(ctx, CellKey(aCx), v8::External::New(aCx->isolate, cell))
      .Check();
  if (aAnchored) {
    Anchor* anchor = cppgc::MakeGarbageCollected<Anchor>(
        aCx->isolate->GetCppHeap()->GetAllocationHandle(), cell);
    cell->tail.anchor = anchor;
    aObject->SetAlignedPointerInInternalField(
        0, const_cast<uint16_t*>(&kWrapperEmbedderId));
    aObject->SetAlignedPointerInInternalField(1, anchor);
    anchor->SetWrapper(aCx->isolate, aObject);
  }
  BindCellHandle(aCx, &cell->tail, aObject, false);
  return cell;
}

JSObject* ObjectFromV8(JSContext* aCx, v8::Local<v8::Object> aObject) {
  if (aObject.IsEmpty()) {
    return nullptr;
  }
  v8::Local<v8::Context> ctx = aCx->v8Context();
  v8::Local<v8::Value> existing;
  if (aObject->GetPrivate(ctx, CellKey(aCx)).ToLocal(&existing) &&
      existing->IsExternal()) {
    return static_cast<ObjectCell*>(existing.As<v8::External>()->Value())
        ->asObject();
  }
  JS::Realm* realm = aCx->realm();
  v8::Local<v8::Context> creation;
  if (aObject->GetCreationContext().ToLocal(&creation)) {
    for (JS::Realm* r : aCx->runtime->realms) {
      if (r->context.Get(aCx->isolate) == creation) {
        realm = r;
        break;
      }
    }
  }
  Shape* shape = ShapeFor(ClassForScriptObject(aObject), realm);
  return NewObjectCell(aCx, shape, aObject, false)->asObject();
}

v8::Local<v8::Object> ToV8(JSContext* aCx, JSObject* aObj) {
  if (!aObj) {
    return v8::Local<v8::Object>();
  }
  ObjectCell* cell = ObjectCell::from(aObj);
  return cell->tail.handle.Get(aCx->isolate).As<v8::Object>();
}

JSString* StringFromV8(JSContext* aCx, v8::Local<v8::String> aString) {
  StringCell* cell = FillString(aCx, aString, false);
  BindCellHandle(aCx, &cell->tail, aString, false);
  return cell->asString();
}

JSString* AtomFromV8(JSContext* aCx, v8::Local<v8::String> aString) {
  std::u16string content;
  content.resize(aString->Length());
  aString->Write(aCx->isolate, reinterpret_cast<uint16_t*>(content.data()), 0,
                 int(content.size()), v8::String::NO_NULL_TERMINATION);
  auto it = aCx->runtime->atoms.find(content);
  if (it != aCx->runtime->atoms.end()) {
    return it->second->asString();
  }
  v8::Local<v8::String> internalized =
      v8::String::NewFromTwoByte(aCx->isolate,
                                 reinterpret_cast<const uint16_t*>(content.data()),
                                 v8::NewStringType::kInternalized,
                                 int(content.size()))
          .ToLocalChecked();
  StringCell* cell = FillString(aCx, internalized, true);
  BindCellHandle(aCx, &cell->tail, internalized, true);
  aCx->runtime->atoms.emplace(std::move(content), cell);
  return cell->asString();
}

JSString* AtomizeCell(JSContext* aCx, JSString* aStr) {
  StringCell* cell = StringCell::from(aStr);
  if (cell->tail.isAtom) {
    return aStr;
  }
  return AtomFromV8(aCx, ToV8(aCx, aStr));
}

v8::Local<v8::String> ToV8(JSContext* aCx, JSString* aStr) {
  StringCell* cell = StringCell::from(aStr);
  if (cell->tail.handle.IsEmpty()) {
    v8::Local<v8::String> s;
    if (cell->latin1()) {
      s = v8::String::NewFromOneByte(
              aCx->isolate, static_cast<const uint8_t*>(cell->chars),
              v8::NewStringType::kNormal, int(cell->length()))
              .ToLocalChecked();
    } else {
      s = v8::String::NewFromTwoByte(
              aCx->isolate, static_cast<const uint16_t*>(cell->chars),
              v8::NewStringType::kNormal, int(cell->length()))
              .ToLocalChecked();
    }
    BindCellHandle(aCx, &cell->tail, s, cell->tail.isAtom);
    return s;
  }
  return cell->tail.handle.Get(aCx->isolate).As<v8::String>();
}

JS::Symbol* SymbolFromV8(JSContext* aCx, v8::Local<v8::Symbol> aSymbol) {
  int hash = aSymbol->GetIdentityHash();
  auto range = aCx->runtime->symbols.equal_range(hash);
  for (auto it = range.first; it != range.second; ++it) {
    if (it->second->tail.handle.Get(aCx->isolate) == aSymbol) {
      return it->second->asSymbol();
    }
  }
  SymbolCell* cell = static_cast<SymbolCell*>(AllocCell(sizeof(SymbolCell)));
  cell->tail.kind = CellKind::Symbol;
  cell->code = JS::shadow::Symbol::WellKnownAPILimit;
  BindCellHandle(aCx, &cell->tail, aSymbol, true);
  aCx->runtime->symbols.emplace(hash, cell);
  return cell->asSymbol();
}

v8::Local<v8::Symbol> ToV8(JSContext* aCx, JS::Symbol* aSym) {
  return SymbolCell::from(aSym)->tail.handle.Get(aCx->isolate).As<v8::Symbol>();
}

JS::BigInt* BigIntFromV8(JSContext* aCx, v8::Local<v8::BigInt> aBigInt) {
  BigIntCell* cell = static_cast<BigIntCell*>(AllocCell(sizeof(BigIntCell)));
  cell->tail.kind = CellKind::BigInt;
  BindCellHandle(aCx, &cell->tail, aBigInt, false);
  return cell->asBigInt();
}

v8::Local<v8::BigInt> ToV8(JSContext* aCx, JS::BigInt* aBig) {
  return BigIntCell::from(aBig)->tail.handle.Get(aCx->isolate).As<v8::BigInt>();
}

JS::Value ValueFromV8(JSContext* aCx, v8::Local<v8::Value> aValue) {
  if (aValue.IsEmpty() || aValue->IsUndefined()) {
    return JS::UndefinedValue();
  }
  if (aValue->IsNull()) {
    return JS::NullValue();
  }
  if (aValue->IsBoolean()) {
    return JS::BooleanValue(aValue->IsTrue());
  }
  if (aValue->IsInt32()) {
    return JS::Int32Value(aValue.As<v8::Int32>()->Value());
  }
  if (aValue->IsNumber()) {
    return JS::DoubleValue(aValue.As<v8::Number>()->Value());
  }
  if (aValue->IsString()) {
    return JS::StringValue(StringFromV8(aCx, aValue.As<v8::String>()));
  }
  if (aValue->IsSymbol()) {
    return JS::SymbolValue(SymbolFromV8(aCx, aValue.As<v8::Symbol>()));
  }
  if (aValue->IsBigInt()) {
    return JS::BigIntValue(BigIntFromV8(aCx, aValue.As<v8::BigInt>()));
  }
  return JS::ObjectValue(*ObjectFromV8(aCx, aValue.As<v8::Object>()));
}

bool ValueFromV8(JSContext* aCx, v8::Local<v8::Value> aValue,
                 JS::MutableHandleValue aResult) {
  aResult.set(ValueFromV8(aCx, aValue));
  return true;
}

v8::Local<v8::Value> ToV8(JSContext* aCx, const JS::Value& aValue) {
  v8::Isolate* isolate = aCx->isolate;
  if (aValue.isUndefined() || aValue.isMagic()) {
    return v8::Undefined(isolate);
  }
  if (aValue.isNull()) {
    return v8::Null(isolate);
  }
  if (aValue.isBoolean()) {
    return v8::Boolean::New(isolate, aValue.toBoolean());
  }
  if (aValue.isInt32()) {
    return v8::Integer::New(isolate, aValue.toInt32());
  }
  if (aValue.isDouble()) {
    return v8::Number::New(isolate, aValue.toDouble());
  }
  if (aValue.isString()) {
    return ToV8(aCx, aValue.toString());
  }
  if (aValue.isSymbol()) {
    return ToV8(aCx, aValue.toSymbol());
  }
  if (aValue.isBigInt()) {
    return ToV8(aCx, aValue.toBigInt());
  }
  if (aValue.isObject()) {
    return ToV8(aCx, &aValue.toObject());
  }
  return v8::Undefined(isolate);
}

bool IdFromV8(JSContext* aCx, v8::Local<v8::Value> aKey,
              JS::MutableHandleId aResult) {
  if (aKey->IsSymbol()) {
    aResult.set(JS::PropertyKey::Symbol(SymbolFromV8(aCx, aKey.As<v8::Symbol>())));
    return true;
  }
  if (aKey->IsUint32()) {
    uint32_t index = aKey.As<v8::Uint32>()->Value();
    if (index <= JS::PropertyKey::IntMax) {
      aResult.set(JS::PropertyKey::Int(int32_t(index)));
      return true;
    }
  }
  v8::Local<v8::String> name;
  if (!aKey->ToString(aCx->v8Context()).ToLocal(&name)) {
    return false;
  }
  v8::String::Value chars(aCx->isolate, name);
  uint32_t index = 0;
  bool isIndex = chars.length() > 0 && chars.length() <= 10;
  for (int i = 0; isIndex && i < chars.length(); i++) {
    char16_t c = (*chars)[i];
    if (c < '0' || c > '9' || (i == 0 && c == '0' && chars.length() > 1)) {
      isIndex = false;
      break;
    }
    index = index * 10 + (c - '0');
  }
  if (isIndex && index <= JS::PropertyKey::IntMax) {
    aResult.set(JS::PropertyKey::Int(int32_t(index)));
    return true;
  }
  aResult.set(JS::PropertyKey::NonIntAtom(AtomFromV8(aCx, name)));
  return true;
}

v8::Local<v8::Value> ToV8(JSContext* aCx, jsid aId) {
  if (aId.isInt()) {
    return v8::Integer::New(aCx->isolate, aId.toInt());
  }
  if (aId.isSymbol()) {
    return ToV8(aCx, aId.toSymbol());
  }
  if (aId.isString()) {
    return ToV8(aCx, aId.toString());
  }
  return v8::Undefined(aCx->isolate);
}

}
