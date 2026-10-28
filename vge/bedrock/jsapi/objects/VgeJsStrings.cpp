#include "VgeJsObjectsInternal.h"

#include <cwchar>
#include <mutex>
#include <string>
#include <unordered_map>

#include "double-conversion/double-conversion.h"
#include "js/CharacterEncoding.h"
#include "js/Conversions.h"
#include "js/Equality.h"
#include "js/Id.h"
#include "js/LocaleSensitive.h"
#include "js/Printer.h"
#include "js/Printf.h"
#include "js/String.h"
#include "js/Symbol.h"
#include "js/Utility.h"
#include "js/friend/ErrorMessages.h"
#include "mozilla/Latin1.h"
#include "mozilla/Printf.h"
#include "mozilla/StringBuffer.h"
#include "mozilla/Utf8.h"
#include "mozilla/intl/Locale.h"
#include "mozmemory.h"

using namespace vge::jsapi;
using namespace vge::jsapi::objects;

namespace {

arena_id_t CreateArena(const char* aLabel) {
  arena_params_t params;
  params.mMaxDirtyIncreaseOverride = 5;
  params.mFlags |= ARENA_FLAG_RANDOMIZE_SMALL_ENABLED;
  params.mLabel = aLabel;
  return moz_create_arena_with_params(&params);
}

JSString* NewFromV8OrReport(JSContext* aCx, v8::MaybeLocal<v8::String> aMaybe) {
  v8::Local<v8::String> str;
  if (!aMaybe.ToLocal(&str)) {
    JS_ReportOutOfMemory(aCx);
    return nullptr;
  }
  return StringFromV8(aCx, str);
}

JSString* NewLatin1String(JSContext* aCx, const uint8_t* aChars,
                          size_t aLength) {
  if (aLength > size_t(v8::String::kMaxLength)) {
    JS_ReportOutOfMemory(aCx);
    return nullptr;
  }
  v8::HandleScope scope(aCx->isolate);
  return NewFromV8OrReport(
      aCx, v8::String::NewFromOneByte(aCx->isolate, aChars,
                                      v8::NewStringType::kNormal, int(aLength)));
}

JSString* NewTwoByteString(JSContext* aCx, const char16_t* aChars,
                           size_t aLength) {
  if (aLength > size_t(v8::String::kMaxLength)) {
    JS_ReportOutOfMemory(aCx);
    return nullptr;
  }
  v8::HandleScope scope(aCx->isolate);
  return NewFromV8OrReport(
      aCx, v8::String::NewFromTwoByte(
               aCx->isolate, reinterpret_cast<const uint16_t*>(aChars),
               v8::NewStringType::kNormal, int(aLength)));
}

bool IsInvalidSecondByte(uint32_t aFirst, uint8_t aSecond) {
  return (aFirst == 0xE0 && (aSecond & 0xE0) != 0xA0) ||
         (aFirst == 0xED && (aSecond & 0xE0) != 0x80) ||
         (aFirst == 0xF0 && (aSecond & 0xF0) == 0x80) ||
         (aFirst == 0xF4 && (aSecond & 0xF0) != 0x80);
}

void ReportInvalidUtf8(JSContext* aCx, size_t aOffset) {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%zu", aOffset);
  JS_ReportErrorNumberASCII(aCx, js::GetErrorMessage, nullptr,
                            JSMSG_MALFORMED_UTF8_CHAR, buffer);
}

bool InflateUtf8(JSContext* aCx, const unsigned char* aSrc, size_t aLength,
                 std::u16string* aOut) {
  aOut->reserve(aLength);
  for (size_t i = 0; i < aLength; i++) {
    uint32_t v = aSrc[i];
    if (!(v & 0x80)) {
      aOut->push_back(char16_t(v));
      continue;
    }
    uint32_t n = 0;
    while (n < 8 && (v & (0x80 >> n))) {
      n++;
    }
    if (n < 2 || n > 4) {
      ReportInvalidUtf8(aCx, i);
      return false;
    }
    if (i + n > aLength) {
      if (i + 2 > aLength) {
        JS_ReportErrorNumberASCII(aCx, js::GetErrorMessage, nullptr,
                                  JSMSG_BUFFER_TOO_SMALL);
        return false;
      }
      if (n == 4 && i + 3 > aLength && !IsInvalidSecondByte(v, aSrc[i + 1]) &&
          (aSrc[i + 1] & 0xC0) == 0x80) {
        JS_ReportErrorNumberASCII(aCx, js::GetErrorMessage, nullptr,
                                  JSMSG_BUFFER_TOO_SMALL);
        return false;
      }
      ReportInvalidUtf8(aCx, i);
      return false;
    }
    if (IsInvalidSecondByte(v, aSrc[i + 1])) {
      ReportInvalidUtf8(aCx, i);
      return false;
    }
    for (uint32_t m = 1; m < n; m++) {
      if ((aSrc[i + m] & 0xC0) != 0x80) {
        ReportInvalidUtf8(aCx, i);
        return false;
      }
    }
    static const uint32_t kMinUcs4[] = {0, 0, 0x80, 0x800, 0x10000};
    uint32_t ucs4 = v & ((1u << (7 - n)) - 1);
    for (uint32_t m = 1; m < n; m++) {
      ucs4 = (ucs4 << 6) | (aSrc[i + m] & 0x3F);
    }
    if (ucs4 < kMinUcs4[n] || (ucs4 >= 0xD800 && ucs4 <= 0xDFFF)) {
      ReportInvalidUtf8(aCx, i);
      return false;
    }
    if (ucs4 < 0x10000) {
      aOut->push_back(char16_t(ucs4));
    } else if (ucs4 <= 0x10FFFF) {
      ucs4 -= 0x10000;
      aOut->push_back(char16_t(0xD800 + (ucs4 >> 10)));
      aOut->push_back(char16_t(0xDC00 + (ucs4 & 0x3FF)));
    } else {
      char buffer[16];
      snprintf(buffer, sizeof(buffer), "0x%x", ucs4);
      JS_ReportErrorNumberASCII(aCx, js::GetErrorMessage, nullptr,
                                JSMSG_UTF8_CHAR_TOO_LARGE, buffer);
      return false;
    }
    i += n - 1;
  }
  return true;
}

JSString* NewUtf8String(JSContext* aCx, const unsigned char* aChars,
                        size_t aLength) {
  bool ascii = true;
  for (size_t i = 0; i < aLength; i++) {
    if (aChars[i] & 0x80) {
      ascii = false;
      break;
    }
  }
  if (ascii) {
    return NewLatin1String(aCx, aChars, aLength);
  }
  std::u16string utf16;
  if (!InflateUtf8(aCx, aChars, aLength, &utf16)) {
    return nullptr;
  }
  return NewTwoByteString(aCx, utf16.data(), utf16.size());
}

template <typename CharT>
size_t PutEscaped(char* aBuffer, size_t aBufferSize, const CharT* aChars,
                  size_t aLength, uint32_t aQuote) {
  static const char kEscapeMap[] = {'\b', 'b', '\f', 'f',  'n', 'n', '\r',
                                    'r',  '\t', 't', '\v', 'v', '"', '"',
                                    '\'', '\'', '\\', '\\', '\0'};
  enum { STOP, FIRST_QUOTE, LAST_QUOTE, CHARS, ESCAPE_START, ESCAPE_MORE } state;
  if (aBufferSize == 0) {
    aBuffer = nullptr;
  } else {
    aBufferSize--;
  }
  const CharT* charsEnd = aChars + aLength;
  size_t n = 0;
  state = FIRST_QUOTE;
  unsigned shift = 0;
  unsigned hex = 0;
  unsigned u = 0;
  char c = 0;
  for (;;) {
    switch (state) {
      case STOP:
        goto stop;
      case FIRST_QUOTE:
        state = CHARS;
        goto do_quote;
      case LAST_QUOTE:
        state = STOP;
      do_quote:
        if (aQuote == 0) {
          continue;
        }
        c = char(aQuote);
        break;
      case CHARS:
        if (aChars == charsEnd) {
          state = LAST_QUOTE;
          continue;
        }
        u = *aChars++;
        if (u < ' ') {
          if (u != 0) {
            const char* escape = nullptr;
            for (const char* p = kEscapeMap; *p; p += 2) {
              if (unsigned(uint8_t(*p)) == u) {
                escape = p;
                break;
              }
            }
            if (escape) {
              u = escape[1];
              goto do_escape;
            }
          }
          goto do_hex_escape;
        }
        if (u < 127) {
          if (u == aQuote || u == '\\') {
            goto do_escape;
          }
          c = char(u);
        } else if (u < 0x100) {
          goto do_hex_escape;
        } else {
          shift = 16;
          hex = u;
          u = 'u';
          goto do_escape;
        }
        break;
      do_hex_escape:
        shift = 8;
        hex = u;
        u = 'x';
      do_escape:
        c = '\\';
        state = ESCAPE_START;
        break;
      case ESCAPE_START:
        c = char(u);
        state = ESCAPE_MORE;
        break;
      case ESCAPE_MORE:
        if (shift == 0) {
          state = CHARS;
          continue;
        }
        shift -= 4;
        u = 0xF & (hex >> shift);
        c = char(u + (u < 10 ? '0' : 'A' - 10));
        break;
    }
    if (aBuffer) {
      if (n != aBufferSize) {
        aBuffer[n] = c;
      } else {
        aBuffer[n] = '\0';
        aBuffer = nullptr;
      }
    }
    n++;
  }
stop:
  if (aBuffer) {
    aBuffer[n] = '\0';
  }
  return n;
}

struct LocaleState {
  std::string defaultLocale = "und";
  const JSLocaleCallbacks* callbacks = nullptr;
};

std::mutex sLocaleLock;

std::unordered_map<JSRuntime*, LocaleState>& LocaleStates() {
  static std::unordered_map<JSRuntime*, LocaleState> states;
  return states;
}

std::string CanonicalBaseName(std::string_view aLocaleId) {
  std::string id(aLocaleId);
  size_t dot = id.find('.');
  if (dot != std::string::npos) {
    id.resize(dot);
  }
  size_t at = id.find('@');
  if (at != std::string::npos) {
    id.resize(at);
  }
  for (char& ch : id) {
    if (ch == '_') {
      ch = '-';
    }
  }
  mozilla::intl::Locale locale;
  if (mozilla::intl::LocaleParser::TryParse(
          mozilla::Span<const char>(id.data(), id.size()), locale)
          .isErr()) {
    return "und";
  }
  locale.ClearVariants();
  if (locale.CanonicalizeBaseName().isErr() ||
      locale.Language().Length() > 3) {
    return "und";
  }
  std::string out(locale.Language().Span().data(),
                  locale.Language().Span().size());
  if (locale.Script().Present()) {
    out += '-';
    out.append(locale.Script().Span().data(), locale.Script().Span().size());
  }
  if (locale.Region().Present()) {
    out += '-';
    out.append(locale.Region().Span().data(), locale.Region().Span().size());
  }
  return out;
}

JS::UniqueChars DuplicateChars(const char* aChars, size_t aLength) {
  char* out = js_pod_malloc<char>(aLength + 1);
  if (!out) {
    return nullptr;
  }
  memcpy(out, aChars, aLength);
  out[aLength] = '\0';
  return JS::UniqueChars(out);
}

bool NumberOf(JSContext* aCx, JS::HandleValue aValue, double* aOut) {
  AutoV8 v8(aCx);
  v8::TryCatch tc(v8.isolate());
  v8::Maybe<double> d = ToV8(aCx, aValue.get())->NumberValue(v8.context());
  if (d.IsNothing()) {
    return v8.Check(tc);
  }
  *aOut = d.FromJust();
  return true;
}

bool ToPrimitiveHintString(JSContext* aCx, v8::Local<v8::Context> aCtx,
                           v8::Local<v8::Object> aObject,
                           v8::Local<v8::Value>* aOut) {
  v8::Isolate* isolate = aCx->isolate;
  v8::Local<v8::Value> exotic;
  if (!aObject->Get(aCtx, v8::Symbol::GetToPrimitive(isolate))
           .ToLocal(&exotic)) {
    return false;
  }
  if (!exotic->IsNullOrUndefined()) {
    if (!exotic->IsFunction()) {
      isolate->ThrowException(v8::Exception::TypeError(
          v8::String::NewFromUtf8Literal(isolate,
                                         "Symbol.toPrimitive is not a function")));
      return false;
    }
    v8::Local<v8::Value> hint = v8::String::NewFromUtf8Literal(isolate, "string");
    v8::Local<v8::Value> result;
    if (!exotic.As<v8::Function>()->Call(aCtx, aObject, 1, &hint).ToLocal(&result)) {
      return false;
    }
    if (result->IsObject()) {
      isolate->ThrowException(v8::Exception::TypeError(
          v8::String::NewFromUtf8Literal(
              isolate, "can't convert object to primitive type")));
      return false;
    }
    *aOut = result;
    return true;
  }
  const char* order[] = {"toString", "valueOf"};
  for (const char* name : order) {
    v8::Local<v8::Value> method;
    if (!aObject->Get(aCtx, v8::String::NewFromUtf8(isolate, name).ToLocalChecked())
             .ToLocal(&method)) {
      return false;
    }
    if (method->IsFunction()) {
      v8::Local<v8::Value> result;
      if (!method.As<v8::Function>()->Call(aCtx, aObject, 0, nullptr).ToLocal(&result)) {
        return false;
      }
      if (!result->IsObject()) {
        *aOut = result;
        return true;
      }
    }
  }
  isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(
      isolate, "can't convert object to string")));
  return false;
}

}

arena_id_t js::ArrayBufferContentsArena = CreateArena("Array buffer contents");
arena_id_t js::StringBufferArena = CreateArena("String buffer contents");

JS_PUBLIC_API bool JS_CopyStringChars(JSContext* cx,
                                      const mozilla::Range<char16_t>& dest,
                                      JSString* str) {
  StringCell* cell = StringCell::from(str);
  uint32_t length = cell->length();
  if (cell->latin1()) {
    const uint8_t* chars = static_cast<const uint8_t*>(cell->chars);
    for (uint32_t i = 0; i < length; i++) {
      dest[i] = chars[i];
    }
  } else if (length) {
    memcpy(dest.begin().get(), cell->chars, length * sizeof(char16_t));
  }
  return true;
}

JS_PUBLIC_API bool JS_EncodeStringToBuffer(JSContext* cx, JSString* str,
                                           char* buffer, size_t length) {
  StringCell* cell = StringCell::from(str);
  size_t writeLength = std::min(size_t(cell->length()), length);
  if (cell->latin1()) {
    memcpy(buffer, cell->chars, writeLength);
  } else {
    const char16_t* src = static_cast<const char16_t*>(cell->chars);
    for (size_t i = 0; i < writeLength; i++) {
      buffer[i] = char(src[i]);
    }
  }
  return true;
}

JS_PUBLIC_API bool JS_GetStringCharAt(JSContext* cx, JSString* str,
                                      size_t index, char16_t* res) {
  StringCell* cell = StringCell::from(str);
  *res = cell->latin1() ? static_cast<const uint8_t*>(cell->chars)[index]
                        : static_cast<const char16_t*>(cell->chars)[index];
  return true;
}

JS_PUBLIC_API bool JS_LinearStringEqualsAscii(JSLinearString* str,
                                              const char* asciiBytes,
                                              size_t length) {
  StringCell* cell = StringCell::from(reinterpret_cast<JSString*>(str));
  if (cell->length() != length) {
    return false;
  }
  if (cell->latin1()) {
    return memcmp(cell->chars, asciiBytes, length) == 0;
  }
  const char16_t* chars = static_cast<const char16_t*>(cell->chars);
  for (size_t i = 0; i < length; i++) {
    if (chars[i] != char16_t(uint8_t(asciiBytes[i]))) {
      return false;
    }
  }
  return true;
}

JS_PUBLIC_API bool JS_LinearStringEqualsAscii(JSLinearString* str,
                                              const char* asciiBytes) {
  return JS_LinearStringEqualsAscii(str, asciiBytes, strlen(asciiBytes));
}

JS_PUBLIC_API bool JS_StringEqualsAscii(JSContext* cx, JSString* str,
                                        const char* asciiBytes, size_t length,
                                        bool* match) {
  *match = JS_LinearStringEqualsAscii(reinterpret_cast<JSLinearString*>(str),
                                      asciiBytes, length);
  return true;
}

JS_PUBLIC_API const char16_t* JS_GetTwoByteStringCharsAndLength(
    JSContext* cx, const JS::AutoRequireNoGC& nogc, JSString* str,
    size_t* length) {
  StringCell* cell = StringCell::from(str);
  *length = cell->length();
  return static_cast<const char16_t*>(cell->chars);
}

JS_PUBLIC_API const JS::Latin1Char* JS_GetLatin1StringCharsAndLength(
    JSContext* cx, const JS::AutoRequireNoGC& nogc, JSString* str,
    size_t* length) {
  StringCell* cell = StringCell::from(str);
  *length = cell->length();
  return static_cast<const JS::Latin1Char*>(cell->chars);
}

JS_PUBLIC_API JSString* JS_GetEmptyString(JSContext* cx) {
  v8::HandleScope scope(cx->isolate);
  return AtomFromV8(cx, v8::String::Empty(cx->isolate));
}

JS_PUBLIC_API JS::Value JS_GetEmptyStringValue(JSContext* cx) {
  return JS::StringValue(JS_GetEmptyString(cx));
}

JS_PUBLIC_API JSLinearString* JS::detail::StringToLinearStringSlow(
    JSContext* cx, JSString* str) {
  return reinterpret_cast<JSLinearString*>(str);
}

JS_PUBLIC_API JSLinearString* JS_EnsureLinearString(JSContext* cx,
                                                    JSString* str) {
  return reinterpret_cast<JSLinearString*>(str);
}

JS_PUBLIC_API JSString* JS::NewStringFromKnownLiveLatin1Buffer(
    JSContext* cx, mozilla::StringBuffer* buffer, size_t length) {
  return NewLatin1String(cx, static_cast<const uint8_t*>(buffer->Data()),
                         length);
}

JS_PUBLIC_API JSString* JS::NewStringFromKnownLiveTwoByteBuffer(
    JSContext* cx, mozilla::StringBuffer* buffer, size_t length) {
  return NewTwoByteString(cx, static_cast<const char16_t*>(buffer->Data()),
                          length);
}

JS_PUBLIC_API JSString* JS::NewStringFromKnownLiveUTF8Buffer(
    JSContext* cx, mozilla::StringBuffer* buffer, size_t length) {
  return NewUtf8String(cx, static_cast<const unsigned char*>(buffer->Data()),
                       length);
}

JS_PUBLIC_API JSString* JS_AtomizeStringN(JSContext* cx, const char* s,
                                          size_t length) {
  return AtomizeLatin1(cx, s, length);
}

JS_PUBLIC_API JSString* JS_AtomizeString(JSContext* cx, const char* s) {
  return AtomizeLatin1(cx, s, strlen(s));
}

JS_PUBLIC_API JSString* JS_AtomizeUCString(JSContext* cx, const char16_t* s) {
  return AtomizeChars(cx, s, std::char_traits<char16_t>::length(s));
}

JS_PUBLIC_API JSString* JS_AtomizeAndPinString(JSContext* cx, const char* s) {
  return AtomizeLatin1(cx, s, strlen(s));
}

JS_PUBLIC_API JSString* JS_ConcatStrings(JSContext* cx,
                                         JS::Handle<JSString*> left,
                                         JS::Handle<JSString*> right) {
  if (size_t(StringCell::from(left)->length()) +
          StringCell::from(right)->length() >
      size_t(v8::String::kMaxLength)) {
    JS_ReportOutOfMemory(cx);
    return nullptr;
  }
  v8::HandleScope scope(cx->isolate);
  v8::Local<v8::String> joined =
      v8::String::Concat(cx->isolate, ToV8(cx, left.get()), ToV8(cx, right.get()));
  return StringFromV8(cx, joined);
}

JS_PUBLIC_API JSString* JS_NewStringCopyN(JSContext* cx, const char* s,
                                          size_t n) {
  return NewLatin1String(cx, reinterpret_cast<const uint8_t*>(s), n);
}

JS_PUBLIC_API JSString* JS_NewStringCopyZ(JSContext* cx, const char* s) {
  if (!s) {
    return JS_GetEmptyString(cx);
  }
  return NewLatin1String(cx, reinterpret_cast<const uint8_t*>(s), strlen(s));
}

JS_PUBLIC_API JSString* JS_NewStringCopyUTF8N(JSContext* cx,
                                              const JS::UTF8Chars& s) {
  return NewUtf8String(cx, s.begin().get(), s.length());
}

JS_PUBLIC_API JSString* JS_NewUCStringCopyN(JSContext* cx, const char16_t* s,
                                            size_t n) {
  if (!n) {
    return JS_GetEmptyString(cx);
  }
  return NewTwoByteString(cx, s, n);
}

JS_PUBLIC_API JSString* JS_NewUCStringCopyZ(JSContext* cx, const char16_t* s) {
  if (!s) {
    return JS_GetEmptyString(cx);
  }
  return NewTwoByteString(cx, s, std::char_traits<char16_t>::length(s));
}

JS_PUBLIC_API JSString* JS_NewUCStringDontDeflate(JSContext* cx,
                                                  JS::UniqueTwoByteChars chars,
                                                  size_t length) {
  return NewTwoByteString(cx, chars.get(), length);
}

JS_PUBLIC_API size_t JS_GetStringEncodingLength(JSContext* cx, JSString* str) {
  return StringCell::from(str)->length();
}

JS_PUBLIC_API size_t JS_PutEscapedLinearString(char* buffer, size_t size,
                                               JSLinearString* str,
                                               char quote) {
  StringCell* cell = StringCell::from(reinterpret_cast<JSString*>(str));
  if (cell->latin1()) {
    return PutEscaped(buffer, size, static_cast<const uint8_t*>(cell->chars),
                      cell->length(), uint32_t(uint8_t(quote)));
  }
  return PutEscaped(buffer, size, static_cast<const char16_t*>(cell->chars),
                    cell->length(), uint32_t(uint8_t(quote)));
}

JS_PUBLIC_API size_t JS_GetStringLength(JSString* str) {
  return StringCell::from(str)->length();
}

JS_PUBLIC_API JS::UniqueChars JS_EncodeStringToLatin1(JSContext* cx,
                                                      JSString* str) {
  StringCell* cell = StringCell::from(str);
  size_t length = cell->length();
  char* out = js_pod_malloc<char>(length + 1);
  if (!out) {
    JS_ReportOutOfMemory(cx);
    return nullptr;
  }
  if (cell->latin1()) {
    memcpy(out, cell->chars, length);
  } else {
    const char16_t* chars = static_cast<const char16_t*>(cell->chars);
    for (size_t i = 0; i < length; i++) {
      out[i] = char(chars[i]);
    }
  }
  out[length] = '\0';
  return JS::UniqueChars(out);
}

JS_PUBLIC_API JS::UniqueChars JS_EncodeStringToUTF8(
    JSContext* cx, JS::Handle<JSString*> str) {
  std::u16string content = StringContent(str);
  std::string utf8 = Utf16ToUtf8(content.data(), content.size());
  JS::UniqueChars out = DuplicateChars(utf8.data(), utf8.size());
  if (!out) {
    JS_ReportOutOfMemory(cx);
  }
  return out;
}

JS_PUBLIC_API mozilla::Maybe<std::tuple<size_t, size_t>>
JS_EncodeStringToUTF8BufferPartial(JSContext* cx, JSString* str,
                                   mozilla::Span<char> buffer) {
  StringCell* cell = StringCell::from(str);
  size_t length = cell->length();
  if (cell->latin1()) {
    auto src = mozilla::Span(static_cast<const char*>(cell->chars), length);
    return mozilla::Some(mozilla::ConvertLatin1toUtf8Partial(src, buffer));
  }
  auto src = mozilla::Span(static_cast<const char16_t*>(cell->chars), length);
  char16_t pendingLead = 0;
  if (!src.IsEmpty()) {
    char16_t last = src[src.Length() - 1];
    if (last >= 0xD800 && last <= 0xDBFF) {
      src = src.To(src.Length() - 1);
      pendingLead = last;
    }
  }
  size_t read;
  size_t written;
  std::tie(read, written) = mozilla::ConvertUtf16toUtf8Partial(src, buffer);
  if (read < src.Length() || !pendingLead) {
    return mozilla::Some(std::make_tuple(read, written));
  }
  if (buffer.Length() - written < 3) {
    return mozilla::Some(std::make_tuple(read, written));
  }
  buffer[written] = '\xEF';
  buffer[written + 1] = '\xBF';
  buffer[written + 2] = '\xBD';
  return mozilla::Some(std::make_tuple(read + 1, written + 3));
}

JS_PUBLIC_API JS::UniqueChars JS::EncodeNarrowToUtf8(JSContext* cx,
                                                     const char* chars) {
  std::mbstate_t mb{};
  const char* tmpChars = chars;
  size_t wideLen = std::mbsrtowcs(nullptr, &tmpChars, 0, &mb);
  if (wideLen == size_t(-1)) {
    JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                              JSMSG_CANT_CONVERT_TO_WIDE);
    return nullptr;
  }
  std::wstring wide(wideLen, L'\0');
  std::mbstate_t mb2{};
  const char* src = chars;
  std::mbsrtowcs(wide.data(), &src, wideLen + 1, &mb2);
  std::string utf8 =
      Utf16ToUtf8(reinterpret_cast<const char16_t*>(wide.data()), wide.size());
  JS::UniqueChars out = DuplicateChars(utf8.data(), utf8.size());
  if (!out) {
    JS_ReportOutOfMemory(cx);
  }
  return out;
}

JS_PUBLIC_API bool JS::ToGetterId(JSContext* cx, JS::Handle<JS::PropertyKey> id,
                                  JS::MutableHandle<JS::PropertyKey> getterId) {
  JS::Rooted<JSString*> name(cx, IdToFunctionName(cx, id, "get "));
  if (!name) {
    return false;
  }
  getterId.set(JS::PropertyKey::NonIntAtom(name));
  return true;
}

JS_PUBLIC_API bool JS::ToSetterId(JSContext* cx, JS::Handle<JS::PropertyKey> id,
                                  JS::MutableHandle<JS::PropertyKey> setterId) {
  JS::Rooted<JSString*> name(cx, IdToFunctionName(cx, id, "set "));
  if (!name) {
    return false;
  }
  setterId.set(JS::PropertyKey::NonIntAtom(name));
  return true;
}

JS_PUBLIC_API bool JS_IdToValue(JSContext* cx, jsid id,
                                JS::MutableHandle<JS::Value> vp) {
  if (id.isInt()) {
    vp.setInt32(id.toInt());
  } else if (id.isString()) {
    vp.setString(id.toString());
  } else if (id.isSymbol()) {
    vp.setSymbol(id.toSymbol());
  } else {
    vp.setUndefined();
  }
  return true;
}

JS_PUBLIC_API bool JS_StringToId(JSContext* cx, JS::Handle<JSString*> string,
                                 JS::MutableHandle<jsid> idp) {
  v8::HandleScope scope(cx->isolate);
  return IdFromV8(cx, ToV8(cx, string.get()), idp);
}

JS_PUBLIC_API bool JS_ValueToId(JSContext* cx, JS::Handle<JS::Value> value,
                                JS::MutableHandle<jsid> idp) {
  if (value.isInt32() && value.toInt32() >= 0) {
    idp.set(JS::PropertyKey::Int(value.toInt32()));
    return true;
  }
  if (value.isSymbol()) {
    idp.set(JS::PropertyKey::Symbol(value.toSymbol()));
    return true;
  }
  AutoV8 v8(cx);
  v8::TryCatch tc(v8.isolate());
  v8::Local<v8::Value> key = ToV8(cx, value.get());
  if (key->IsObject() && !ToPrimitiveHintString(cx, v8.context(),
                                                key.As<v8::Object>(), &key)) {
    return v8.Check(tc);
  }
  if (!IdFromV8(cx, key, idp)) {
    return v8.Check(tc);
  }
  return true;
}

JS_PUBLIC_API JS::Symbol* JS::GetWellKnownSymbol(JSContext* cx,
                                                 JS::SymbolCode which) {
  return WellKnownSymbol(cx, which);
}

JS_PUBLIC_API JS::PropertyKey JS::GetWellKnownSymbolKey(JSContext* cx,
                                                        JS::SymbolCode which) {
  return JS::PropertyKey::Symbol(WellKnownSymbol(cx, which));
}

JS_PUBLIC_API bool js::ToBooleanSlow(JS::HandleValue v) {
  if (v.isString()) {
    return StringCell::from(v.toString())->length() != 0;
  }
  JSContext* cx = CurrentContext();
  v8::HandleScope scope(cx->isolate);
  return ToV8(cx, v.get())->BooleanValue(cx->isolate);
}

JS_PUBLIC_API bool js::ToNumberSlow(JSContext* cx, JS::HandleValue v,
                                    double* dp) {
  return NumberOf(cx, v, dp);
}

JS_PUBLIC_API bool js::ToInt32Slow(JSContext* cx, JS::HandleValue v,
                                   int32_t* out) {
  double d;
  if (v.isDouble()) {
    d = v.toDouble();
  } else if (!NumberOf(cx, v, &d)) {
    return false;
  }
  *out = JS::ToInt32(d);
  return true;
}

JS_PUBLIC_API bool js::ToUint32Slow(JSContext* cx, JS::HandleValue v,
                                    uint32_t* out) {
  double d;
  if (v.isDouble()) {
    d = v.toDouble();
  } else if (!NumberOf(cx, v, &d)) {
    return false;
  }
  *out = JS::ToUint32(d);
  return true;
}

JS_PUBLIC_API bool js::ToUint16Slow(JSContext* cx, JS::HandleValue v,
                                    uint16_t* out) {
  double d;
  if (v.isDouble()) {
    d = v.toDouble();
  } else if (!NumberOf(cx, v, &d)) {
    return false;
  }
  *out = JS::ToUint16(d);
  return true;
}

JS_PUBLIC_API bool js::ToInt64Slow(JSContext* cx, JS::HandleValue v,
                                   int64_t* out) {
  double d;
  if (v.isDouble()) {
    d = v.toDouble();
  } else if (!NumberOf(cx, v, &d)) {
    return false;
  }
  *out = JS::ToInt64(d);
  return true;
}

JS_PUBLIC_API bool js::ToUint64Slow(JSContext* cx, JS::HandleValue v,
                                    uint64_t* out) {
  double d;
  if (v.isDouble()) {
    d = v.toDouble();
  } else if (!NumberOf(cx, v, &d)) {
    return false;
  }
  *out = JS::ToUint64(d);
  return true;
}

JS_PUBLIC_API JSString* js::ToStringSlow(JSContext* cx, JS::HandleValue v) {
  AutoV8 v8(cx);
  v8::TryCatch tc(v8.isolate());
  v8::Local<v8::String> str;
  if (!ToV8(cx, v.get())->ToString(v8.context()).ToLocal(&str)) {
    v8.Check(tc);
    return nullptr;
  }
  return StringFromV8(cx, str);
}

JS_PUBLIC_API JSObject* js::ToObjectSlow(JSContext* cx, JS::HandleValue v,
                                         bool reportScanStack) {
  AutoV8 v8(cx);
  v8::TryCatch tc(v8.isolate());
  v8::Local<v8::Object> obj;
  if (!ToV8(cx, v.get())->ToObject(v8.context()).ToLocal(&obj)) {
    v8.Check(tc);
    return nullptr;
  }
  return ObjectFromV8(cx, obj);
}

void JS::NumberToString(double d, char (&out)[MaximumNumberToStringLength]) {
  int32_t i;
  if (mozilla::NumberEqualsInt32(d, &i)) {
    snprintf(out, sizeof(out), "%d", i);
    return;
  }
  const double_conversion::DoubleToStringConverter& converter =
      double_conversion::DoubleToStringConverter::EcmaScriptConverter();
  double_conversion::StringBuilder builder(out, sizeof(out));
  converter.ToShortest(d, &builder);
  builder.Finalize();
}

JS_PUBLIC_API bool JS::StrictlyEqual(JSContext* cx, JS::Handle<JS::Value> value1,
                                     JS::Handle<JS::Value> value2,
                                     bool* equal) {
  v8::HandleScope scope(cx->isolate);
  *equal = ToV8(cx, value1.get())->StrictEquals(ToV8(cx, value2.get()));
  return true;
}

JS_PUBLIC_API JS::UniqueChars JS_smprintf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  mozilla::SmprintfPolicyPointer<js::SystemAllocPolicy> result =
      mozilla::Vsmprintf<js::SystemAllocPolicy>(fmt, ap);
  va_end(ap);
  return JS::UniqueChars(result.release());
}

JS_PUBLIC_API JS::UniqueChars JS_sprintf_append(JS::UniqueChars&& last,
                                                const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  mozilla::SmprintfPolicyPointer<js::SystemAllocPolicy> lastPtr(last.release());
  mozilla::SmprintfPolicyPointer<js::SystemAllocPolicy> result =
      mozilla::VsmprintfAppend<js::SystemAllocPolicy>(std::move(lastPtr), fmt,
                                                      ap);
  va_end(ap);
  return JS::UniqueChars(result.release());
}

JS_PUBLIC_API bool JS_SetDefaultLocale(JSRuntime* rt, const char* locale) {
  if (!locale) {
    return false;
  }
  std::string canonical = CanonicalBaseName(locale);
  {
    std::lock_guard<std::mutex> lock(sLocaleLock);
    LocaleStates()[rt].defaultLocale = canonical;
  }
  if (rt->isolate) {
    rt->isolate->LocaleConfigurationChangeNotification();
  }
  return true;
}

JS_PUBLIC_API JS::UniqueChars JS_GetDefaultLocale(JSContext* cx) {
  std::string locale;
  {
    std::lock_guard<std::mutex> lock(sLocaleLock);
    LocaleState& state = LocaleStates()[cx->runtime];
    if (state.defaultLocale == "und") {
      state.defaultLocale =
          CanonicalBaseName(mozilla::intl::Locale::GetDefaultLocale());
    }
    locale = state.defaultLocale;
  }
  JS::UniqueChars out = DuplicateChars(locale.data(), locale.size());
  if (!out) {
    JS_ReportOutOfMemory(cx);
  }
  return out;
}

JS_PUBLIC_API void JS_SetLocaleCallbacks(JSRuntime* rt,
                                         const JSLocaleCallbacks* callbacks) {
  std::lock_guard<std::mutex> lock(sLocaleLock);
  LocaleStates()[rt].callbacks = callbacks;
}

JS_PUBLIC_API const JSLocaleCallbacks* JS_GetLocaleCallbacks(JSRuntime* rt) {
  std::lock_guard<std::mutex> lock(sLocaleLock);
  auto it = LocaleStates().find(rt);
  return it == LocaleStates().end() ? nullptr : it->second.callbacks;
}
