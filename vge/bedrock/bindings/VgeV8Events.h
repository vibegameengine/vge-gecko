#ifndef vge_bedrock_VgeV8Events_h
#define vge_bedrock_VgeV8Events_h

#include "nsString.h"
#include "v8.h"

namespace mozilla::dom {
class EventTarget;
}

namespace vge::bedrock::v8b {

void EventTarget_addEventListener(
    const v8::FunctionCallbackInfo<v8::Value>& aInfo);
void EventTarget_removeEventListener(
    const v8::FunctionCallbackInfo<v8::Value>& aInfo);

v8::Local<v8::Value> GetEventHandler(v8::Isolate* aIsolate,
                                     mozilla::dom::EventTarget* aTarget,
                                     const nsAString& aType);
void SetEventHandler(v8::Isolate* aIsolate, mozilla::dom::EventTarget* aTarget,
                     const nsAString& aType, v8::Local<v8::Value> aValue);

void Window_setTimeout(const v8::FunctionCallbackInfo<v8::Value>& aInfo);
void Window_setInterval(const v8::FunctionCallbackInfo<v8::Value>& aInfo);
void Window_clearTimeout(const v8::FunctionCallbackInfo<v8::Value>& aInfo);
void Window_clearInterval(const v8::FunctionCallbackInfo<v8::Value>& aInfo);
void Window_requestAnimationFrame(
    const v8::FunctionCallbackInfo<v8::Value>& aInfo);
void Window_cancelAnimationFrame(
    const v8::FunctionCallbackInfo<v8::Value>& aInfo);
void Window_queueMicrotask(const v8::FunctionCallbackInfo<v8::Value>& aInfo);

void ReleaseEventState();

}

#endif
