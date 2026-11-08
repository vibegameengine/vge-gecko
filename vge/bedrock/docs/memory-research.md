# Память хоста Gecko+V8: что её держит и как уменьшить

Документ пополняется по ходу работы. Здесь только то, что прочитано из отчётов движка, из живого процесса или из кода.
Числа ниже — из запусков 2026-11-09.

## Как снималось

- Частная копия `dist/bin` в scratchpad, чтобы сборка основной сессии её не трогала и файлы не пропадали.
- Отчёт движка: `nsIMemoryInfoDumper.dumpMemoryReportsToNamedFile` из chrome-модуля, зарегистрированного в копии
  (`chrome://vgeprobe`), который потом уводит окно на целевой URL.
- Windows: обход `VirtualQueryEx` + `QueryWorkingSetEx` по каждой странице; private working set по модулям и регионам,
  стеки потоков из TEB, имена потоков и стартовые адреса.
- V8: `VGE_V8_FLAGS=--trace-gc-verbose` (штатная переменная слоя, `jsapi/VgeJsRuntime.cpp:594`), размеры по пространствам.

## Дефект, мешающий измерению: JS-репортёр падает

`dumpMemoryReportsToNamedFile` роняет процесс с 0xc0000005:

```
xpc::XPCJSRuntimeStats::initExtraRealmStats (js/xpconnect/src/XPCJSRuntime.cpp:2229)
JS::CollectRuntimeStats (vge/bedrock/jsapi/gc/VgeJsMemoryMetrics.cpp:225)
xpc::JSReporter::CollectReports (js/xpconnect/src/XPCJSRuntime.cpp:2296)
nsWindowMemoryReporter::CollectReports (dom/base/nsWindowMemoryReporter.cpp:536)
```

Значит about:memory на этом хосте сейчас не работает вообще. Чтобы получить остальные отчёты, в частной копии `xul.dll`
функция `JS::CollectRuntimeStats` заменена на `return false` (в репозитории ничего не менялось). Поэтому в числах ниже нет
дерева `explicit/js-*`; V8 показан из `--trace-gc-verbose`.

## Простая страница (`test/no-script.html`), главный процесс

| Что | Байты | Источник |
|---|---|---|
| Private working set всего | 61 МБ | обход страниц |
| `resident-unique` | 60,0 МБ | репортёр |
| Shared working set | 79 МБ | обход страниц |
| Committed | 747 МБ | обход страниц |
| Reserved private | 14,6 ГБ адресов | `address-space/reserved/private` |
| Потоков | 168, из них 107 без имени | TEB |

Состав private working set (61 МБ):

| Что | МБ | Источник |
|---|---|---|
| Частные данные всего | 54,4 | категория private-data |
| из них mozjemalloc: выделено | 21,7 | `heap-allocated` |
| из них mozjemalloc: служебное | 4,05 | `heap/committed/bookkeeping` |
| из них системная куча Windows | 12,2 | `system-heap-allocated` |
| из них V8, все пространства, committed | 4,3 (занято 3,0) | `--trace-gc-verbose` |
| Стеки потоков | 2,6 | TEB + working set |
| `xul.dll`, частные страницы | 0,7 | обход страниц (shared 34,3) |
| Драйвер NVIDIA `nvgpucomp64.dll` | 0,8 | обход страниц |

Отдельный процесс Socket: `resident-unique` 4,5 МБ, `private` 9,3 МБ, `resident` 18,1 МБ.

Прочие объекты из отчёта: `phc/metadata` 1,34 МБ, `gfx/webrender` 1,5 МБ, `threads/overhead/kernel` 1,4 МБ,
`storage/sqlite` 0,5 МБ, `layout/style-sheet-cache` 0,4 МБ, `icu` 0,4 МБ.

## Размеры файлов

`xul.dll` 186,6 МБ, `icudtl.dat` 10,7 МБ, `dxcompiler.dll` 24,2 МБ, `onnxruntime.dll` 11,9 МБ, `gkcodecs.dll` 11,4 МБ,
`libGLESv2.dll` 4,9 МБ, `mozavcodec.dll` 4,6 МБ, `mozinference.dll` 3,1 МБ. Каталог `dist/bin` целиком 321 МБ.

## Игра `https://claybound-56949.web.app/`, главный процесс, 50 с после старта

| Что | Байты | Источник |
|---|---|---|
| Private working set | 735 МБ | обход страниц |
| `resident-unique` | 739,7 МБ | репортёр |
| Shared working set | 356 МБ | обход страниц |
| Committed | 1691 МБ | обход страниц |
| Потоков | 300 | TEB |
| mozjemalloc выделено | 109,2 МБ | `heap-allocated` |
| Системная куча Windows | 26,0 МБ | `system-heap-allocated` |
| `webgl-texture-memory` | 121,8 МБ | репортёр WebGL |
| Общие секции (shmem) | 277,4 МБ выделено, 199,8 отображено | `shmem-*` |
| Кэш скриптов | 13,2 МБ | `explicit/js-non-window/cache` |
| Картинки | 8,6 МБ | `explicit/images/content/raster` |
| Webaudio | 3,1 МБ | `explicit/webaudio/audiobuffer` |

Плюс процессы Socket (private WS 4 МБ, 10 потоков) и Utility (16 МБ, 25 потоков).

Из 300 потоков 135 — один пул драйвера NVIDIA (стартовый адрес `nvwgf2umx.dll+0xe6ec30`), 64 безымянных с точкой входа
`ucrtbase`, остальные с именами (пулы Style, WebRender и прочие).

## В работе

Страница WebGL, разбор потоков, сравнение с CEF/Chromium, стоимость обёртки, предложения.
