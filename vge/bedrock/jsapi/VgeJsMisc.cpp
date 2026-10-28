#include "VgeJsInternal.h"

#include <stdarg.h>
#include <stdio.h>

#include "js/Printf.h"
#include "js/RealmOptions.h"
#include "js/String.h"
#include "js/Utility.h"

using namespace vge::jsapi;

JS::UniqueChars JS_vsmprintf(const char* aFormat, va_list aArgs) {
  va_list copy;
  va_copy(copy, aArgs);
  int length = vsnprintf(nullptr, 0, aFormat, copy);
  va_end(copy);
  if (length < 0) {
    return nullptr;
  }
  JS::UniqueChars out(js_pod_malloc<char>(size_t(length) + 1));
  if (!out) {
    return nullptr;
  }
  vsnprintf(out.get(), size_t(length) + 1, aFormat, aArgs);
  return out;
}

JS::UniqueTwoByteChars JS_CopyStringCharsZ(JSContext* aCx, JSString* aStr) {
  StringCell* cell = StringCell::from(aStr);
  uint32_t length = cell->length();
  JS::UniqueTwoByteChars out(js_pod_malloc<char16_t>(size_t(length) + 1));
  if (!out) {
    JS_ReportOutOfMemory(aCx);
    return nullptr;
  }
  if (cell->latin1()) {
    auto* chars = static_cast<const JS::Latin1Char*>(cell->chars);
    for (uint32_t i = 0; i < length; i++) {
      out[i] = chars[i];
    }
  } else {
    memcpy(out.get(), cell->chars, length * sizeof(char16_t));
  }
  out[length] = 0;
  return out;
}

JSString* JS_NewStringCopyUTF8Z(JSContext* aCx, const JS::ConstUTF8CharsZ aChars) {
  const char* chars = aChars.c_str();
  return JS_NewStringCopyUTF8N(aCx, JS::UTF8Chars(chars, strlen(chars)));
}

JSString* JS_AtomizeUCStringN(JSContext* aCx, const char16_t* aChars,
                              size_t aLength) {
  JSString* str = JS_NewUCStringCopyN(aCx, aChars, aLength);
  if (!str) {
    return nullptr;
  }
  return AtomizeCell(aCx, str);
}

bool JS::RealmCreationOptions::getCoopAndCoepEnabled() const {
  return coopAndCoep_;
}

JS::GCCellPtr::GCCellPtr(const JS::Value& aValue)
    : GCCellPtr(aValue.toGCThing(), aValue.traceKind()) {}

void JS::AutoGCRooter::trace(JSTracer* aTracer) {
  switch (kind_) {
    case Kind::Custom:
      static_cast<JS::CustomAutoRooter*>(this)->trace(aTracer);
      break;
    default:
      MOZ_CRASH("Bad AutoGCRooter::Kind");
  }
}
