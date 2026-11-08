#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "mozilla/StackWalk.h"
#include "mozilla/StackWalkThread.h"
#include "nsWindowsWMain.cpp"
#include "vge/bedrock/VgeBedrockRuntime.h"

namespace {

void PrintFrame(uint32_t aFrameNumber, void* aPC, void* aSP, void* aClosure) {
  MozCodeAddressDetails details;
  MozDescribeCodeAddress(aPC, &details);
  char line[1024];
  MozFormatCodeAddressDetails(line, sizeof(line), aFrameNumber, aPC, &details);
  fprintf(stderr, "%s\n", line);
}

LONG WINAPI PrintCrashStack(EXCEPTION_POINTERS* aInfo) {
  fprintf(stderr, "vge crash: exception 0x%08lx at %p\n",
          aInfo->ExceptionRecord->ExceptionCode,
          aInfo->ExceptionRecord->ExceptionAddress);
  CONTEXT context = *aInfo->ContextRecord;
  MozStackWalkThread(PrintFrame, 64, nullptr, GetCurrentThread(), &context);
  fflush(stderr);
  return EXCEPTION_CONTINUE_SEARCH;
}

}

int main(int argc, char** argv) {
  _putenv("MOZ_FORCE_DISABLE_E10S=1");
  SetUnhandledExceptionFilter(PrintCrashStack);
  return VgeBedrock_RunLayoutTest(argc, argv);
}
