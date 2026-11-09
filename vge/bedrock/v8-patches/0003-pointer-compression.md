# Pointer compression build of wee8

`build_wee8_compile.cmd` passes `v8_enable_pointer_compression=true` and keeps `v8_enable_sandbox=false`.

Gecko compiles against `dist/Inc`, so the embedder ABI macros must match the library. They are defined right after
the include guard of `dist/Inc/v8config.h`, each under `#ifndef`:

- `V8_COMPRESS_POINTERS`
- `V8_COMPRESS_POINTERS_IN_SHARED_CAGE`
- `V8_31BIT_SMIS_ON_64BIT_ARCH`
- `V8_EXTERNAL_CODE_SPACE`
- `V8_STATIC_ROOTS`
- `V8_SHARED_RO_HEAP`
- `CPPGC_CAGED_HEAP`
- `CPPGC_POINTER_COMPRESSION`
- `CPPGC_SLIM_WRITE_BARRIER`
- `CPPGC_YOUNG_GENERATION`

The list comes from the `defines` line of `out.gn/x64.release/obj/v8_libbase.ninja`. Compare it again after a V8 update.
