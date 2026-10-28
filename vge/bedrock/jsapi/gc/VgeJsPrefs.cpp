#include <stdio.h>

#include "js/Initialization.h"
#include "js/Prefs.h"

JS_PREF_CLASS_FIELDS_INIT;

#ifdef DEBUG
void JS::Prefs::assertCanSetStartupPref() {
  MOZ_ASSERT(JS::detail::libraryInitState ==
                 JS::detail::InitState::Uninitialized,
             "startup prefs must be set before calling JS_Init");
}
#endif

bool JS::Prefs::fuzzingSafe_ = false;
bool JS::Prefs::reportIgnoredFuzzingUnsafePrefs_ = false;

void JS::Prefs::reportIgnoredFuzzingUnsafePref(const char* name) {
  if (reportIgnoredFuzzingUnsafePrefs_) {
    fprintf(stderr, "Warning: ignoring pref %s with --fuzzing-safe\n", name);
  }
}
