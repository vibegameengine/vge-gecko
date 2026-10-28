#include "VgeJsObjectsInternal.h"

#include <cmath>
#include <string>
#include <vector>

#include "double-conversion/double-conversion.h"
#include "js/JSON.h"
#include "js/friend/ErrorMessages.h"
#include "mozilla/FloatingPoint.h"

using namespace vge::jsapi;
using namespace vge::jsapi::objects;

namespace {

enum class Tok {
  String,
  Number,
  True,
  False,
  Null,
  ArrayOpen,
  ArrayClose,
  ObjectOpen,
  ObjectClose,
  Comma,
  Colon,
  Error,
  OOM
};

bool IsJsonWhitespace(char16_t c) {
  return c == '\t' || c == '\r' || c == '\n' || c == ' ';
}

bool IsDigit(char16_t c) { return c >= '0' && c <= '9'; }

bool IsHexDigit(char16_t c) {
  return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

uint32_t HexValue(char16_t c) {
  if (IsDigit(c)) {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return c - 'A' + 10;
}

template <typename CharT, typename Sink>
class JsonParser {
 public:
  JsonParser(const CharT* aChars, size_t aLength, Sink& aSink)
      : mBegin(aChars), mCur(aChars), mEnd(aChars + aLength), mSink(aSink) {}

  bool Parse() {
    enum class Step {
      Value,
      ValueSwitch,
      Member,
      FinishObject,
      FinishArray,
      Done
    };
    std::vector<Step> stack;
    Step step = Step::Value;
    Tok token = Tok::Error;
    for (;;) {
      switch (step) {
        case Step::FinishObject:
          token = AdvanceAfterProperty();
          if (token == Tok::ObjectClose) {
            stack.pop_back();
            if (!mSink.endObject()) {
              return false;
            }
            step = Step::Done;
            break;
          }
          if (token != Tok::Comma) {
            if (token == Tok::OOM) {
              return false;
            }
            if (token != Tok::Error) {
              Error(
                  "expected ',' or '}' after property-value pair in object "
                  "literal");
            }
            return false;
          }
          token = AdvancePropertyName();
          step = Step::Member;
          break;
        case Step::Member:
          if (token == Tok::String) {
            token = AdvancePropertyColon();
            if (token != Tok::Colon) {
              return false;
            }
            step = Step::Value;
            break;
          }
          if (token == Tok::OOM) {
            return false;
          }
          if (token != Tok::Error) {
            Error("property names must be double-quoted strings");
          }
          return false;
        case Step::FinishArray:
          token = AdvanceAfterArrayElement();
          if (token == Tok::Comma) {
            step = Step::Value;
            break;
          }
          if (token == Tok::ArrayClose) {
            stack.pop_back();
            if (!mSink.endArray()) {
              return false;
            }
            step = Step::Done;
            break;
          }
          return false;
        case Step::Value:
          token = Advance();
          step = Step::ValueSwitch;
          break;
        case Step::ValueSwitch:
          switch (token) {
            case Tok::String:
            case Tok::Number:
            case Tok::True:
            case Tok::False:
            case Tok::Null:
              step = Step::Done;
              break;
            case Tok::ArrayOpen:
              stack.push_back(Step::FinishArray);
              if (!mSink.startArray()) {
                return false;
              }
              token = Advance();
              if (token == Tok::ArrayClose) {
                stack.pop_back();
                if (!mSink.endArray()) {
                  return false;
                }
                step = Step::Done;
              }
              break;
            case Tok::ObjectOpen:
              stack.push_back(Step::FinishObject);
              if (!mSink.startObject()) {
                return false;
              }
              token = AdvanceAfterObjectOpen();
              if (token == Tok::ObjectClose) {
                stack.pop_back();
                if (!mSink.endObject()) {
                  return false;
                }
                step = Step::Done;
              } else {
                step = Step::Member;
              }
              break;
            case Tok::ArrayClose:
            case Tok::ObjectClose:
            case Tok::Colon:
            case Tok::Comma:
              mCur--;
              Error("unexpected character");
              return false;
            case Tok::OOM:
            case Tok::Error:
              return false;
          }
          break;
        case Step::Done:
          if (stack.empty()) {
            for (; mCur < mEnd; mCur++) {
              if (!IsJsonWhitespace(*mCur)) {
                Error("unexpected non-whitespace character after JSON data");
                return false;
              }
            }
            return true;
          }
          step = stack.back();
          break;
      }
    }
  }

 private:
  void Error(const char* aMsg) {
    uint32_t col = 1;
    uint32_t row = 1;
    for (const CharT* p = mBegin; p < mCur; p++) {
      if (*p == '\n' || *p == '\r') {
        ++row;
        col = 1;
        if (p + 1 < mCur && *p == '\r' && *(p + 1) == '\n') {
          ++p;
        }
      } else {
        ++col;
      }
    }
    mSink.error(aMsg, row, col);
  }

  void SkipWhitespace() {
    while (mCur < mEnd && IsJsonWhitespace(*mCur)) {
      mCur++;
    }
  }

  Tok Advance() {
    SkipWhitespace();
    if (mCur >= mEnd) {
      Error("unexpected end of data");
      return Tok::Error;
    }
    switch (*mCur) {
      case '"':
        return ReadString(false);
      case '-':
      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9':
        return ReadNumber();
      case 't':
        if (mEnd - mCur < 4 || mCur[1] != 'r' || mCur[2] != 'u' ||
            mCur[3] != 'e') {
          Error("unexpected keyword");
          return Tok::Error;
        }
        mCur += 4;
        return mSink.booleanValue(true) ? Tok::True : Tok::OOM;
      case 'f':
        if (mEnd - mCur < 5 || mCur[1] != 'a' || mCur[2] != 'l' ||
            mCur[3] != 's' || mCur[4] != 'e') {
          Error("unexpected keyword");
          return Tok::Error;
        }
        mCur += 5;
        return mSink.booleanValue(false) ? Tok::False : Tok::OOM;
      case 'n':
        if (mEnd - mCur < 4 || mCur[1] != 'u' || mCur[2] != 'l' ||
            mCur[3] != 'l') {
          Error("unexpected keyword");
          return Tok::Error;
        }
        mCur += 4;
        return mSink.nullValue() ? Tok::Null : Tok::OOM;
      case '[':
        mCur++;
        return Tok::ArrayOpen;
      case ']':
        mCur++;
        return Tok::ArrayClose;
      case '{':
        mCur++;
        return Tok::ObjectOpen;
      case '}':
        mCur++;
        return Tok::ObjectClose;
      case ',':
        mCur++;
        return Tok::Comma;
      case ':':
        mCur++;
        return Tok::Colon;
      default:
        Error("unexpected character");
        return Tok::Error;
    }
  }

  Tok AdvancePropertyName() {
    SkipWhitespace();
    if (mCur >= mEnd) {
      Error("end of data when property name was expected");
      return Tok::Error;
    }
    if (*mCur == '"') {
      return ReadString(true);
    }
    Error("expected double-quoted property name");
    return Tok::Error;
  }

  Tok AdvancePropertyColon() {
    SkipWhitespace();
    if (mCur >= mEnd) {
      Error("end of data after property name when ':' was expected");
      return Tok::Error;
    }
    if (*mCur == ':') {
      mCur++;
      return Tok::Colon;
    }
    Error("expected ':' after property name in object");
    return Tok::Error;
  }

  Tok AdvanceAfterProperty() {
    SkipWhitespace();
    if (mCur >= mEnd) {
      Error("end of data after property value in object");
      return Tok::Error;
    }
    if (*mCur == ',') {
      mCur++;
      return Tok::Comma;
    }
    if (*mCur == '}') {
      mCur++;
      return Tok::ObjectClose;
    }
    Error("expected ',' or '}' after property value in object");
    return Tok::Error;
  }

  Tok AdvanceAfterObjectOpen() {
    SkipWhitespace();
    if (mCur >= mEnd) {
      Error("end of data while reading object contents");
      return Tok::Error;
    }
    if (*mCur == '"') {
      return ReadString(true);
    }
    if (*mCur == '}') {
      mCur++;
      return Tok::ObjectClose;
    }
    Error("expected property name or '}'");
    return Tok::Error;
  }

  Tok AdvanceAfterArrayElement() {
    SkipWhitespace();
    if (mCur >= mEnd) {
      Error("end of data when ',' or ']' was expected");
      return Tok::Error;
    }
    if (*mCur == ',') {
      mCur++;
      return Tok::Comma;
    }
    if (*mCur == ']') {
      mCur++;
      return Tok::ArrayClose;
    }
    Error("expected ',' or ']' after array element");
    return Tok::Error;
  }

  Tok EmitSource(bool aPropertyName, const CharT* aStart, size_t aLength) {
    bool ok = aPropertyName ? mSink.propertyName(aStart, aLength)
                            : mSink.stringValue(aStart, aLength);
    return ok ? Tok::String : Tok::OOM;
  }

  Tok EmitBuilt(bool aPropertyName, const std::u16string& aChars) {
    bool latin1 = true;
    for (char16_t c : aChars) {
      if (c > 0xFF) {
        latin1 = false;
        break;
      }
    }
    bool ok;
    if (latin1) {
      std::vector<JS::Latin1Char> narrow(aChars.begin(), aChars.end());
      ok = aPropertyName ? mSink.propertyName(narrow.data(), narrow.size())
                         : mSink.stringValue(narrow.data(), narrow.size());
    } else {
      ok = aPropertyName ? mSink.propertyName(aChars.data(), aChars.size())
                         : mSink.stringValue(aChars.data(), aChars.size());
    }
    return ok ? Tok::String : Tok::OOM;
  }

  Tok ReadString(bool aPropertyName) {
    if (++mCur == mEnd) {
      Error("unterminated string literal");
      return Tok::Error;
    }
    const CharT* start = mCur;
    for (; mCur < mEnd; mCur++) {
      if (*mCur == '"') {
        size_t length = mCur - start;
        mCur++;
        return EmitSource(aPropertyName, start, length);
      }
      if (*mCur == '\\') {
        break;
      }
      if (*mCur <= 0x001F) {
        Error("bad control character in string literal");
        return Tok::Error;
      }
    }
    std::u16string builder;
    do {
      for (const CharT* p = start; p < mCur; p++) {
        builder.push_back(char16_t(*p));
      }
      if (mCur >= mEnd) {
        break;
      }
      char16_t c = *mCur++;
      if (c == '"') {
        return EmitBuilt(aPropertyName, builder);
      }
      if (c != '\\') {
        --mCur;
        Error("bad character in string literal");
        return Tok::Error;
      }
      if (mCur >= mEnd) {
        break;
      }
      switch (*mCur++) {
        case '"':
          c = '"';
          break;
        case '/':
          c = '/';
          break;
        case '\\':
          c = '\\';
          break;
        case 'b':
          c = '\b';
          break;
        case 'f':
          c = '\f';
          break;
        case 'n':
          c = '\n';
          break;
        case 'r':
          c = '\r';
          break;
        case 't':
          c = '\t';
          break;
        case 'u':
          if (mEnd - mCur < 4 ||
              !(IsHexDigit(mCur[0]) && IsHexDigit(mCur[1]) &&
                IsHexDigit(mCur[2]) && IsHexDigit(mCur[3]))) {
            if (mCur == mEnd || !IsHexDigit(mCur[0])) {
            } else if (mCur + 1 == mEnd || !IsHexDigit(mCur[1])) {
              mCur += 1;
            } else if (mCur + 2 == mEnd || !IsHexDigit(mCur[2])) {
              mCur += 2;
            } else {
              mCur += 3;
            }
            Error("bad Unicode escape");
            return Tok::Error;
          }
          c = char16_t((HexValue(mCur[0]) << 12) | (HexValue(mCur[1]) << 8) |
                       (HexValue(mCur[2]) << 4) | HexValue(mCur[3]));
          mCur += 4;
          break;
        default:
          mCur--;
          Error("bad escaped character");
          return Tok::Error;
      }
      builder.push_back(c);
      start = mCur;
      for (; mCur < mEnd; mCur++) {
        if (*mCur == '"' || *mCur == '\\' || *mCur <= 0x001F) {
          break;
        }
      }
    } while (mCur < mEnd);
    Error("unterminated string");
    return Tok::Error;
  }

  Tok ReadNumber() {
    bool negative = *mCur == '-';
    if (negative && ++mCur == mEnd) {
      Error("no number after minus sign");
      return Tok::Error;
    }
    const CharT* digitStart = mCur;
    if (!IsDigit(*mCur)) {
      Error("unexpected non-digit");
      return Tok::Error;
    }
    if (*mCur++ != '0') {
      for (; mCur < mEnd; mCur++) {
        if (!IsDigit(*mCur)) {
          break;
        }
      }
    }
    if (mCur < mEnd && *mCur == '.') {
      if (++mCur == mEnd) {
        Error("missing digits after decimal point");
        return Tok::Error;
      }
      if (!IsDigit(*mCur)) {
        Error("unterminated fractional number");
        return Tok::Error;
      }
      while (++mCur < mEnd) {
        if (!IsDigit(*mCur)) {
          break;
        }
      }
    }
    if (mCur < mEnd && (*mCur == 'e' || *mCur == 'E')) {
      if (++mCur == mEnd) {
        Error("missing digits after exponent indicator");
        return Tok::Error;
      }
      if (*mCur == '+' || *mCur == '-') {
        if (++mCur == mEnd) {
          Error("missing digits after exponent sign");
          return Tok::Error;
        }
      }
      if (!IsDigit(*mCur)) {
        Error("exponent part is missing a number");
        return Tok::Error;
      }
      while (++mCur < mEnd) {
        if (!IsDigit(*mCur)) {
          break;
        }
      }
    }
    std::string ascii;
    for (const CharT* p = digitStart; p < mCur; p++) {
      ascii.push_back(char(*p));
    }
    double_conversion::StringToDoubleConverter converter(
        double_conversion::StringToDoubleConverter::NO_FLAGS, 0.0, 0.0,
        nullptr, nullptr);
    int processed = 0;
    double d =
        converter.StringToDouble(ascii.data(), int(ascii.size()), &processed);
    return mSink.numberValue(negative ? -d : d) ? Tok::Number : Tok::OOM;
  }

  const CharT* mBegin;
  const CharT* mCur;
  const CharT* mEnd;
  Sink& mSink;
};

struct ValidationSink {
  bool startObject() { return true; }
  bool endObject() { return true; }
  bool startArray() { return true; }
  bool endArray() { return true; }
  bool propertyName(const JS::Latin1Char*, size_t) { return true; }
  bool propertyName(const char16_t*, size_t) { return true; }
  bool stringValue(const JS::Latin1Char*, size_t) { return true; }
  bool stringValue(const char16_t*, size_t) { return true; }
  bool numberValue(double) { return true; }
  bool booleanValue(bool) { return true; }
  bool nullValue() { return true; }
  void error(const char*, uint32_t, uint32_t) {}
};

struct DelegateSink {
  explicit DelegateSink(JS::JSONParseHandler* aHandler) : mHandler(aHandler) {}
  bool startObject() { return mHandler->startObject(); }
  bool endObject() { return mHandler->endObject(); }
  bool startArray() { return mHandler->startArray(); }
  bool endArray() { return mHandler->endArray(); }
  bool propertyName(const JS::Latin1Char* aChars, size_t aLength) {
    return mHandler->propertyName(aChars, aLength);
  }
  bool propertyName(const char16_t* aChars, size_t aLength) {
    return mHandler->propertyName(aChars, aLength);
  }
  bool stringValue(const JS::Latin1Char* aChars, size_t aLength) {
    return mHandler->stringValue(aChars, aLength);
  }
  bool stringValue(const char16_t* aChars, size_t aLength) {
    return mHandler->stringValue(aChars, aLength);
  }
  bool numberValue(double aValue) { return mHandler->numberValue(aValue); }
  bool booleanValue(bool aValue) { return mHandler->booleanValue(aValue); }
  bool nullValue() { return mHandler->nullValue(); }
  void error(const char* aMsg, uint32_t aLine, uint32_t aColumn) {
    mHandler->error(aMsg, aLine, aColumn);
  }
  JS::JSONParseHandler* mHandler;
};

bool ParseV8(JSContext* cx, v8::Local<v8::String> aSource,
             JS::MutableHandleValue vp) {
  v8::Local<v8::Context> ctx = cx->isolate->GetCurrentContext();
  v8::TryCatch tc(cx->isolate);
  v8::Local<v8::Value> out;
  if (!v8::JSON::Parse(ctx, aSource).ToLocal(&out)) {
    if (tc.HasCaught() && !tc.HasTerminated()) {
      cx->setPending(tc.Exception());
    }
    return false;
  }
  vp.set(ValueFromV8(cx, out));
  return true;
}

bool StringifyV8(JSContext* cx, JS::HandleValue value,
                 JS::HandleObject replacer, JS::HandleValue space,
                 v8::Local<v8::Value>* aOut) {
  v8::Isolate* isolate = cx->isolate;
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  v8::TryCatch tc(isolate);
  v8::Local<v8::Value> stringify = Intrinsic(cx, IntrinsicId::JsonStringify);
  if (!stringify->IsFunction()) {
    isolate->ThrowException(v8::Exception::TypeError(
        v8::String::NewFromUtf8Literal(isolate, "JSON.stringify is missing")));
  } else {
    v8::Local<v8::Value> args[] = {
        ToV8(cx, value.get()),
        replacer ? v8::Local<v8::Value>(ToV8(cx, replacer.get()))
                 : v8::Local<v8::Value>(v8::Null(isolate)),
        ToV8(cx, space.get())};
    if (stringify.As<v8::Function>()
            ->Call(ctx, v8::Undefined(isolate), 3, args)
            .ToLocal(aOut)) {
      return true;
    }
  }
  if (tc.HasCaught() && !tc.HasTerminated()) {
    cx->setPending(tc.Exception());
  }
  return false;
}

bool EmitV8String(JSContext* cx, v8::Local<v8::Value> aString,
                  JSONWriteCallback callback, void* data) {
  v8::String::Value chars(cx->isolate, aString);
  return callback(reinterpret_cast<const char16_t*>(*chars),
                  uint32_t(chars.length()), data);
}

void AppendAscii(std::u16string& aOut, const char* aAscii) {
  for (const char* p = aAscii; *p; p++) {
    aOut.push_back(char16_t(*p));
  }
}

void AppendNumber(std::u16string& aOut, double aValue) {
  char buffer[32];
  int32_t i;
  if (mozilla::NumberEqualsInt32(aValue, &i)) {
    snprintf(buffer, sizeof(buffer), "%d", i);
  } else {
    double_conversion::StringBuilder builder(buffer, sizeof(buffer));
    double_conversion::DoubleToStringConverter::EcmaScriptConverter()
        .ToShortest(aValue, &builder);
    builder.Finalize();
  }
  AppendAscii(aOut, buffer);
}

void AppendQuoted(std::u16string& aOut, const uint16_t* aChars,
                  size_t aLength) {
  static const char kHex[] = "0123456789abcdef";
  aOut.push_back(u'"');
  for (size_t i = 0; i < aLength; i++) {
    char16_t c = aChars[i];
    switch (c) {
      case '"':
        AppendAscii(aOut, "\\\"");
        continue;
      case '\\':
        AppendAscii(aOut, "\\\\");
        continue;
      case '\b':
        AppendAscii(aOut, "\\b");
        continue;
      case '\f':
        AppendAscii(aOut, "\\f");
        continue;
      case '\n':
        AppendAscii(aOut, "\\n");
        continue;
      case '\r':
        AppendAscii(aOut, "\\r");
        continue;
      case '\t':
        AppendAscii(aOut, "\\t");
        continue;
      default:
        break;
    }
    bool lone = false;
    if (c >= 0xD800 && c <= 0xDBFF) {
      lone = !(i + 1 < aLength && aChars[i + 1] >= 0xDC00 &&
               aChars[i + 1] <= 0xDFFF);
      if (!lone) {
        aOut.push_back(c);
        aOut.push_back(char16_t(aChars[++i]));
        continue;
      }
    } else if (c >= 0xDC00 && c <= 0xDFFF) {
      lone = true;
    }
    if (c < 0x20 || lone) {
      aOut.push_back(u'\\');
      aOut.push_back(u'u');
      aOut.push_back(char16_t(kHex[(c >> 12) & 0xF]));
      aOut.push_back(char16_t(kHex[(c >> 8) & 0xF]));
      aOut.push_back(char16_t(kHex[(c >> 4) & 0xF]));
      aOut.push_back(char16_t(kHex[c & 0xF]));
      continue;
    }
    aOut.push_back(c);
  }
  aOut.push_back(u'"');
}

class SafeSerializer {
 public:
  SafeSerializer(v8::Isolate* aIsolate, v8::Local<v8::Context> aCtx)
      : mIsolate(aIsolate), mCtx(aCtx) {}

  enum class Result { Ok, Filtered, Failed };

  Result Serialize(v8::Local<v8::Value> aValue, std::u16string& aOut) {
    if (aValue->IsNull()) {
      AppendAscii(aOut, "null");
      return Result::Ok;
    }
    if (aValue->IsUndefined() || aValue->IsSymbol() || aValue->IsFunction()) {
      return Result::Filtered;
    }
    if (aValue->IsBoolean()) {
      AppendAscii(aOut, aValue->IsTrue() ? "true" : "false");
      return Result::Ok;
    }
    if (aValue->IsNumber()) {
      double d = aValue.As<v8::Number>()->Value();
      if (!std::isfinite(d)) {
        AppendAscii(aOut, "null");
      } else {
        AppendNumber(aOut, d);
      }
      return Result::Ok;
    }
    if (aValue->IsString()) {
      v8::String::Value chars(mIsolate, aValue);
      AppendQuoted(aOut, *chars, size_t(chars.length()));
      return Result::Ok;
    }
    if (aValue->IsBigInt()) {
      mBigInt = true;
      return Result::Failed;
    }
    v8::Local<v8::Object> obj = aValue.As<v8::Object>();
    for (v8::Local<v8::Object> seen : mStack) {
      if (seen == obj) {
        mCyclic = true;
        return Result::Failed;
      }
    }
    mStack.push_back(obj);
    Result result = obj->IsArray() ? SerializeArray(obj.As<v8::Array>(), aOut)
                                   : SerializeObject(obj, aOut);
    mStack.pop_back();
    return result;
  }

  bool mBigInt = false;
  bool mCyclic = false;

 private:
  Result SerializeArray(v8::Local<v8::Array> aArray, std::u16string& aOut) {
    aOut.push_back(u'[');
    uint32_t length = aArray->Length();
    for (uint32_t i = 0; i < length; i++) {
      if (i) {
        aOut.push_back(u',');
      }
      v8::Local<v8::Value> element;
      if (!aArray->Get(mCtx, i).ToLocal(&element)) {
        return Result::Failed;
      }
      Result r = Serialize(element, aOut);
      if (r == Result::Failed) {
        return r;
      }
      if (r == Result::Filtered) {
        AppendAscii(aOut, "null");
      }
    }
    aOut.push_back(u']');
    return Result::Ok;
  }

  Result SerializeObject(v8::Local<v8::Object> aObject, std::u16string& aOut) {
    v8::Local<v8::Array> keys;
    if (!aObject
             ->GetOwnPropertyNames(
                 mCtx,
                 v8::PropertyFilter(v8::PropertyFilter::ONLY_ENUMERABLE |
                                    v8::PropertyFilter::SKIP_SYMBOLS),
                 v8::KeyConversionMode::kConvertToString)
             .ToLocal(&keys)) {
      return Result::Failed;
    }
    aOut.push_back(u'{');
    bool first = true;
    for (uint32_t i = 0; i < keys->Length(); i++) {
      v8::Local<v8::Value> key;
      v8::Local<v8::Value> value;
      if (!keys->Get(mCtx, i).ToLocal(&key) ||
          !aObject->Get(mCtx, key).ToLocal(&value)) {
        return Result::Failed;
      }
      std::u16string member;
      Result r = Serialize(value, member);
      if (r == Result::Failed) {
        return r;
      }
      if (r == Result::Filtered) {
        continue;
      }
      if (!first) {
        aOut.push_back(u',');
      }
      first = false;
      v8::String::Value keyChars(mIsolate, key);
      AppendQuoted(aOut, *keyChars, size_t(keyChars.length()));
      aOut.push_back(u':');
      aOut += member;
    }
    aOut.push_back(u'}');
    return Result::Ok;
  }

  v8::Isolate* mIsolate;
  v8::Local<v8::Context> mCtx;
  std::vector<v8::Local<v8::Object>> mStack;
};

}

JS_PUBLIC_API bool JS::IsValidJSON(const JS::Latin1Char* chars, uint32_t len) {
  ValidationSink sink;
  return JsonParser<JS::Latin1Char, ValidationSink>(chars, len, sink).Parse();
}

JS_PUBLIC_API bool JS::IsValidJSON(const char16_t* chars, uint32_t len) {
  ValidationSink sink;
  return JsonParser<char16_t, ValidationSink>(chars, len, sink).Parse();
}

JS_PUBLIC_API bool JS::ParseJSONWithHandler(const JS::Latin1Char* chars,
                                            uint32_t len,
                                            JS::JSONParseHandler* handler) {
  DelegateSink sink(handler);
  return JsonParser<JS::Latin1Char, DelegateSink>(chars, len, sink).Parse();
}

JS_PUBLIC_API bool JS::ParseJSONWithHandler(const char16_t* chars,
                                            uint32_t len,
                                            JS::JSONParseHandler* handler) {
  DelegateSink sink(handler);
  return JsonParser<char16_t, DelegateSink>(chars, len, sink).Parse();
}

JS_PUBLIC_API bool JS_ParseJSON(JSContext* cx, const char16_t* chars,
                                uint32_t len, JS::MutableHandle<JS::Value> vp) {
  AutoV8 v8(cx);
  v8::Local<v8::String> source;
  if (!v8::String::NewFromTwoByte(v8.isolate(),
                                  reinterpret_cast<const uint16_t*>(chars),
                                  v8::NewStringType::kNormal, int(len))
           .ToLocal(&source)) {
    JS_ReportOutOfMemory(cx);
    return false;
  }
  return ParseV8(cx, source, vp);
}

JS_PUBLIC_API bool JS_ParseJSON(JSContext* cx, const JS::Latin1Char* chars,
                                uint32_t len, JS::MutableHandle<JS::Value> vp) {
  AutoV8 v8(cx);
  v8::Local<v8::String> source;
  if (!v8::String::NewFromOneByte(v8.isolate(), chars,
                                  v8::NewStringType::kNormal, int(len))
           .ToLocal(&source)) {
    JS_ReportOutOfMemory(cx);
    return false;
  }
  return ParseV8(cx, source, vp);
}

JS_PUBLIC_API bool JS_ParseJSON(JSContext* cx, JS::Handle<JSString*> str,
                                JS::MutableHandle<JS::Value> vp) {
  AutoV8 v8(cx);
  return ParseV8(cx, ToV8(cx, str.get()), vp);
}

JS_PUBLIC_API bool JS_StringifyWithLengthHint(
    JSContext* cx, JS::MutableHandle<JS::Value> vp,
    JS::Handle<JSObject*> replacer, JS::Handle<JS::Value> space,
    JSONWriteCallback callback, void* data, size_t lengthHint) {
  AutoV8 v8(cx);
  v8::Local<v8::Value> out;
  if (!StringifyV8(cx, vp, replacer, space, &out)) {
    return false;
  }
  if (!out->IsString()) {
    static const char16_t kNull[] = u"null";
    return callback(kNull, 4, data);
  }
  return EmitV8String(cx, out, callback, data);
}

JS_PUBLIC_API bool JS_Stringify(JSContext* cx, JS::MutableHandle<JS::Value> vp,
                                JS::Handle<JSObject*> replacer,
                                JS::Handle<JS::Value> space,
                                JSONWriteCallback callback, void* data) {
  return JS_StringifyWithLengthHint(cx, vp, replacer, space, callback, data, 0);
}

JS_PUBLIC_API bool JS::ToJSON(JSContext* cx, JS::Handle<JS::Value> value,
                              JS::Handle<JSObject*> replacer,
                              JS::Handle<JS::Value> space,
                              JSONWriteCallback callback, void* data) {
  AutoV8 v8(cx);
  v8::Local<v8::Value> out;
  if (!StringifyV8(cx, value, replacer, space, &out)) {
    return false;
  }
  if (!out->IsString() || out.As<v8::String>()->Length() == 0) {
    return true;
  }
  return EmitV8String(cx, out, callback, data);
}

JS_PUBLIC_API bool JS::ToJSONMaybeSafely(JSContext* cx,
                                         JS::Handle<JSObject*> input,
                                         JSONWriteCallback callback,
                                         void* data) {
  std::u16string out;
  bool bigInt = false;
  bool cyclic = false;
  {
    AutoV8 v8(cx);
    v8::TryCatch tc(v8.isolate());
    SafeSerializer serializer(v8.isolate(), v8.context());
    SafeSerializer::Result result =
        serializer.Serialize(ToV8(cx, input.get()), out);
    if (result == SafeSerializer::Result::Failed) {
      if (tc.HasCaught()) {
        return v8.Check(tc);
      }
      bigInt = serializer.mBigInt;
      cyclic = serializer.mCyclic;
    } else if (result == SafeSerializer::Result::Filtered) {
      out = u"null";
    }
  }
  if (bigInt) {
    JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                              JSMSG_BIGINT_NOT_SERIALIZABLE);
    return false;
  }
  if (cyclic) {
    JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                              JSMSG_JSON_CYCLIC_VALUE);
    return false;
  }
  return callback(out.data(), uint32_t(out.size()), data);
}
