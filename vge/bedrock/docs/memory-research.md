# Память хоста Gecko+V8: что её держит и как уменьшить

Числа из запусков 2026-11-09, окно 1280x800, машина с 24 логическими процессорами и картой NVIDIA. Только прочитанное из
отчётов движка, живого процесса или кода. Где причина не найдена, так и написано.

## Как снималось

- Отдельная копия `dist/bin` в scratchpad, чтобы сборка основной сессии не мешала. В ней добавлен chrome-модуль
  `vgeprobe`, который из `core.sys.mjs` вызывает `nsIMemoryInfoDumper.dumpMemoryReportsToNamedFile` и уводит окно на URL.
  В репозитории ничего не менялось.
- Windows: обход `VirtualQueryEx` + `QueryWorkingSetEx` по каждой странице: private working set (PWS) по модулям и
  регионам, стеки из TEB, имена потоков, стартовые адреса; у безымянных потоков цепочка адресов со дна стека.
- V8: `VGE_V8_FLAGS=--trace-gc-verbose` (`jsapi/VgeJsRuntime.cpp:594`), `All spaces` по изолятам.
- Chromium: Playwright `chromium-1234` (151.0.7922.34) с `--no-sandbox`. CEF: `E:\Projects\AIGamess\cef-fsapp`, та же
  страница через `--url=`. Для них PWS — сумма по всему дереву процессов. Число JS из CDP (`Runtime.getHeapUsage`,
  `performance.memory`), видеопамять — счётчик `GPU Process Memory\Dedicated Usage`.

## Дефекты, найденные при измерении

1. **`about:memory`/отчёт движка падает на V8.** 0xc0000005:
   `XPCJSRuntimeStats::initExtraRealmStats` (`js/xpconnect/src/XPCJSRuntime.cpp:2229`), вызов из
   `JS::CollectRuntimeStats` (`vge/bedrock/jsapi/gc/VgeJsMemoryMetrics.cpp:225`), из `JSReporter::CollectReports`, из
   `nsWindowMemoryReporter::CollectReports` (`dom/base/nsWindowMemoryReporter.cpp:536`). Чтобы снять остальные отчёты, в
   частной копии `xul.dll` функция `JS::CollectRuntimeStats` заменена на `return false`. Поэтому в числах ниже нет
   дерева `explicit/js-*`. Без исправления этого падения штатного отчёта у хоста нет.
2. **Минимизация памяти не завершается на игре.** Дамп с `minimizeMemoryUsage` на no-script закончился, на игре за 90 с нет:
   изолят воркера делает Mark-Compact по 1,4 мс подряд тысячами (6052 блока `All spaces` за запуск).
3. **Падение процесса с Worker.** Один запуск закончился 0xc0000005 в обработчике `MessageEvent` на потоке воркера
   (`MessageEventRunnable::WorkerRun`, `dom/workers/MessageEventRunnable.cpp:92`, вызов `EventHandlerNonNull::Call`). На
   следующих запусках не повторилось.

## Сводка сравнения

| Страница | Хост | CEF (cef-fsapp) | Chromium 151 |
|---|---|---|---|
| `no-script.html`, PWS всего дерева | 61 + Socket 4,5 = 66 МБ | 74 МБ | 149 МБ |
| Игра, PWS всего дерева | 735-769 + Socket 4 + Utility 16 = 755-789 МБ | 416 МБ | 485 МБ |
| Игра, рендерер PWS | весь главный процесс | 307 МБ | 314 МБ |
| Игра, Dedicated GPU | 3021 МБ | 220 МБ | 331 МБ |
| Игра, JS-куча V8 | 44 МБ занято, 48 МБ committed | 54 МБ занято (performance.memory) | 49 МБ занято |
| Игра, потоков | 291-300 | 8 процессов, 254 потока | 11 процессов, 333 потока |

Вывод из таблицы: простой хост на уровне CEF и меньше Chromium. Лишнее появляется на настоящей странице: хост держит на
300 МБ PWS больше CEF и в десять раз больше видеопамяти. JS-куча сопоставима, поэтому сам V8 здесь не источник.

## Что держит память, по убыванию байт

### 1. Видеопамять 3,0 ГБ против 220-331 МБ у CEF/Chromium

- Держит: процесс хоста, счётчик `GPU Process Memory\Dedicated Usage` (3 021 602 816 байт), не PWS.
- Отчёты движка объясняют только 358 МБ: `gfx/webrender/textures/*` (render-texture-hosts 220,6; render-targets 64;
  atlas 36; depth 17; picture-tiles 10; swap-chains 7,3; standalone 3,8; staging 4,2) и `webgl-texture-memory` 121,8.
- Остальные примерно 2,5 ГБ движок не считает. Причина не найдена.
- Как закрыть: снять список D3D-ресурсов устройства ANGLE/WebRender (`ID3D11Device` с Debug Layer и
  `ReportLiveDeviceObjects`) и прочитать, кто создаёт. Решение пишется только после этого.

### 2. Большие частные блоки без хозяина, около 530 МБ PWS

- Главный процесс игры, `private-data` 720-769 МБ. Из них объяснено: mozjemalloc 109,2 + служебное 4,9, системная куча
  26, V8 48 committed, ячейки 9 МБ.
- Остаток около 530 МБ лежит в отдельных регионах точных размеров: 16 МиБ x7, 32 МиБ x4, 5,4 МиБ x16, 4 МиБ x8 и другие
  (файл регионов запуска `clay3`). В части регионов с начала идут RGBA-пиксели (`0xff00a6ff...`), в остальных нули.
  Размеры 16 и 4 МиБ равны 2048x2048 и 1024x1024 RGBA.
- `V8 External memory reported` = 211,5 МБ. Это сумма `JS::AddAssociatedMemory`
  (`jsapi/gc/VgeJsRuntimeMisc.cpp:194`), её вызывают биндинги (`dom/bindings/BindingUtils.h`, `XPCWrappedNative.cpp`),
  2D-канвас и `ImageBitmap` (`dom/canvas/ImageBitmap.cpp`, `CanvasRenderingContext2D.cpp:2147`). Это учётная величина,
  не адрес блока, и какой объект её набрал, не установлено.
- Причина не найдена, регионы не приписаны. Как закрыть: запись стека на `VirtualAlloc` и крупные CRT-выделения в живом
  процессе, затем чтение того, кто создаёт. Пока стек не прочитан, решение здесь не предлагается.
- Отдельно по коду: `v8::ArrayBuffer::Allocator::NewDefaultAllocator` (`VgeJsRuntime.cpp:629`) в V8 идёт через
  `base::Calloc` (`v8/src/api/api.cc:524`), то есть мимо mozjemalloc, и `ArrayBuffer` не попадают в `heap-allocated`. Для
  сравнения в Chromium `backingStorageSize` 18,5 МБ, в CEF 23,3 МБ, поэтому большой остаток не объясняется одними
  `ArrayBuffer` игры.

### 3. Общие секции: 277-293 МБ выделено, 200 МБ отображено

- Держит: `shmem-allocated`, `shmem-mapped`; shared working set 356 МБ. Это не PWS.
- Состав в отчёте: `gfx/webrender/images/mapped_from_owner/...` — декодированные поверхности 1024x1024 и 2048x2048, по
  сумме сходится с `render-texture-hosts` 220,6 МБ. Хозяин: `SharedSurfacesParent` (`gfx/layers/ipc/SharedSurfacesParent.cpp`).
- Сравнение: у CEF и Chromium таких секций в том же объёме нет, общая часть PWS у них меньше.
- Что сделать: прочитать жизнь `SharedSurfacesParent` и вызовы, откуда поверхность остаётся отображённой после загрузки в
  текстуру. По отчёту она отображена у владельца и у WebRender. Чтение ещё не сделано.

### 4. mozjemalloc: 109,2 МБ + 4,9 служебного

- `heap-allocated`, `heap/committed/bookkeeping`. Распределено по отчётам 48 МБ `explicit`:
  `explicit/js-non-window/cache` 13,2 (`dom/script/SharedScriptCache.cpp:323`, разделяемый кэш скриптов);
  `images/content/raster` 8,6; `webaudio/audiobuffer` 3,1; `gfx/webrender` 5,3; `storage/sqlite` 1,5;
  `window-objects` 1,9; `threads` 5,1 (стеки и kernel).
- Остальные около 60 МБ сейчас идут как heap-unclassified: JS-репортёр выключен (дефект 1), там обычно V8 malloc и DOM-объекты.
- Что сделать: чинить дефект 1, потом читать это дерево. Кэш скриптов: прочитать политику `SharedScriptCache` и
  ограничить по памяти для одного окна.

### 5. Куча V8: 44 МБ занято, 48 МБ committed

- Главный изолят: `All spaces, used: 44234 KB, committed 48060 KB`, Old 32708, Code 3584, Large object 10744.
- Сравнение: CEF 54, Chromium 49 МБ. Паритет.
- Единственный оставшийся на уровне сборки рычаг: `v8_enable_pointer_compression=false` в
  `build_wee8_compile.cmd`. Сжатие указателей уменьшает старые пространства по документации V8 (v8.dev/blog/pointer-compression;
  здесь не проверялось). Оценка 15-20 МБ на этой странице, отдельный замер нужен после сборки. Полное решение: сборка V8 с этим
  флагом, проверка, что `VgeV8Config.h` совпадает со сборкой, и граница 4 ГБ на изолят.
- Настройки heap (`--max-semi-space-size`, `--optimize-for-size` в `src/flags/flag-definitions.h:1267,1538`) снимают
  единицы мегабайт и не использованы; это делается `VGE_V8_FLAGS` без сборки.

### 6. Ячейки JS API: 9 МБ

- Регионы по 1 МиБ со общим `runtime` в `ChunkBase`: 9 штук (запуск `clay3`) на игре.
- Стоимость одной обёртки по коду (`jsapi/VgeJsInternal.h`):
  - `ObjectCell` = 8 (shape) + 8 x 18 (`words`, 2 заголовка + 16 слотов `MAX_FIXED_SLOTS`) + `CellTail` 48 = 200 байт
    (`VgeJsHeap.cpp`, `AllocCell`, кусками по 1 МиБ с `VirtualAlloc`, `MEM_COMMIT` сразу);
  - `v8::Global` — узел `GlobalHandles::Node`, блоками по 256 (`src/handles/global-handles.cc:41`), около 32 байт на узел;
  - `v8::TracedReference` в `CellTail` плюс второй в `Anchor`: узел `TracedNode` до 16 байт каждый
    (`traced-handles.cc:146`);
  - `Anchor` cppgc: 16 + заголовок 8 = 24 байта;
  - запись в `unordered_set` `objectCells` (`VgeJsValues.cpp:119`), узел MSVC около 48 байт.
  - Итого около 350 байт на обёртку плюс сам объект V8. Строка: `StringCell` 80 байт (`VgeJsInternal.h:74`).
- Сравнение с целевым видом: у SpiderMonkey такого нет, и нет `CellTail`, отдельный 16-слотовый резерв на каждую ячейку не
  нужен большинству объектов.
- Что сделать окончательно: 16 слотов вынести в опциональный блок по фактическому `reservedSlots` (`Shape::reservedSlots`
  уже есть): ячейка тогда 56 байт вместо 200, у маленьких классов выигрыш больше; `objectCells` заменить интрузивным списком
  через `CellTail` (минус примерно 48 байт и его бакеты). Выигрыш на этой странице единицы мегабайт из 9, на больших DOM
  пропорционален числу обёрток.

### 7. Потоки: 291-300 в игре, 168-170 на пустой странице

Разбор живого процесса (стартовые адреса и имена):

| Группа | Потоков | Кто / как уменьшить |
|---|---|---|
| Пул драйвера NVIDIA | 135 (игра), 57 (пусто) | `nvwgf2umx.dll+0xe6ec30`. Driver. У Chromium GPU-процесс 109 потоков, у CEF 101 — та же природа |
| Безымянные с `ucrtbase` | 62-64 (игра), 40 (пусто) | Среди них пул V8: `NewDefaultPlatform()` (`VgeJsPlatform.cpp:303`) = min(24-1, 16) = 16 потоков (`default-platform.cc:33-40`). Остальные по цепочке стека не определены |
| WebRender | 12 + 2 builder + RenderBackend | `WRWorker#`, `WRWorkerLP#`, `WRSceneBuilder*`, `WRRenderBackend*`, `Renderer`, `Compositor`, `CanvasRenderer` |
| Style | 5 | `layout.css.stylo-threads`, `modules/libpref/init/StaticPrefList.yaml:11592` |
| TaskController | 8 | `xpcom/threads/TaskController.cpp:56`, `MOZ_TASKCONTROLLER_THREADCOUNT`, иначе от числа ядер |
| DNS | 5 | `network.dns.max_any_priority_threads`, `netwerk/dns/nsHostResolver.cpp:185` |
| Сервисы | по 1 | `URL Classifier`, `Permission`, `Cookie`, `QuotaManager IO`, `glean.dispatcher`, `BHMgr Monitor`, `BHMgr Processor`, `Gamepad`, `osclientcerts`, `JS Watchdog`, `sqldb:cookies.sqlite`, `Link Monitor`, `IPC Launch` |

Вес всего этого: стеки в отчёте `threads/stacks` 2,9 МБ, ядро `threads/overhead/kernel` 2,1 МБ, в PWS категории стеков 6,8 МБ.
Сокращение потоков экономит единицы мегабайт и закрывается настройками числа потоков выше (`NewDefaultPlatform(N)`, env,
префы), тут важен пул V8 и пул NVIDIA не трогается. Потоки — не то место, где лежат сотни мегабайт.

### 8. Сервисы Gecko, не нужные хосту

- Запущены: `nsUrlClassifierDBService` (поток `URL Classifier`, `toolkit/components/url-classifier/nsUrlClassifierDBService.cpp:2070`),
  БД `permissions.sqlite` и `cookies.sqlite`, glean, BHMgr, Quota. Память всей подсистемы SQLite 1,5 МБ на игре.
- `Places` и служба политик в этих прогонах в отчётах не видны (нет `places.sqlite`, нет потока). Хост только шлёт
  `profile-after-change` наблюдателям и не поднимает категории (`runtime/VgeBedrockRuntime.cpp:559`), так что запускались они,
  если запускались, не через категорию. Для полной уверенности нужен список загруженных модулей.
- Что сделать: срез каталогов и категорий в `app.mozbuild` с крейтами gkrust, как записано в `DECISIONS.md`. Вес единицы МБ.

### 9. Отдельные процессы

Socket: PWS 4 МБ, 10 потоков. Utility (аудио) на игре: 16 МБ, 25 потоков. Выбор Gecko, задаётся префами `network.process.enabled`
и аналогичными; в одном процессе они исчезают ценой изоляции. Решает пользователь.

### 10. Файлы

| Файл | Диск | Что в памяти |
|---|---|---|
| `xul.dll` | 186,6 МБ | 0,7-0,9 МБ PWS, 34-65 МБ shared, committed 178,8 МБ |
| `icudtl.dat` | 10,7 МБ | отображается файлом; PWS 0 |
| `onnxruntime.dll`, `mozinference.dll` | 11,9 и 3,1 МБ | на этих страницах не загружены |
| `dxcompiler.dll`, `libGLESv2.dll` | 24,2 и 4,9 МБ | шейдеры ANGLE |
| Каталог `dist/bin` | 321 МБ | devtools, gmp, браузерная часть — только диск |

PWS от бинарников меньше 1 МБ, это вопрос диска и shared, не private. `icudtl.dat` и ICU внутри `xul` (`js_static_lib`,
`DECISIONS.md`) — две копии данных; единая копия уберёт одну из них, 10,7 МБ на диске.

## Обновление 2026-11-09, после исправлений

- Видеопамять: причина роста найдена. Фабрика ANGLE не поддерживает цветовые пространства и создаёт поверхности с `Display`,
  а `SwapChain::Acquire` сравнивал их с запрошенным sRGB и сбрасывал пул каждый кадр. Исправлено (`SurfaceFactory::ResolveColorSpace`).
  Dedicated Usage игры стабильно 469 МБ вместо 3 ГБ и роста.
- Отчёт памяти работает: `VGE_MEMORY_REPORT=<путь с обратными слешами>`, `VGE_MEMORY_REPORT_DELAY=<секунды>`. Зависание было от
  `FindObjectById` на каждую ячейку (полный обход кучи), падение от списка realm, снятого до GC снимка.
- Игра в процессе: private 1082 МБ, resident-unique 817 МБ, heap-allocated 219 МБ, webgl-texture-memory 241 МБ, `explicit` около 180 МБ.
- `shmem-allocated` 565 МБ считает дескрипторы, а не отображения, поэтому в сумму не складывается: отображено 203 МБ.
- Безымянные блоки private-data остаются неприписанными: около 500 МБ PWS, размеры 16; 5,4; 8,7; 32 МиБ.
  Выделений такого размера через `NtAllocateVirtualMemory` со стеками драйвера нет.
- Самый крупный поток больших выделений: `TexUnpackBlob::ConvertIfNeeded`, 336 МБ суммарно за 58 вызовов. Эти буферы освобождаются после загрузки.
- Проверенная и не подтвердившаяся версия: промежуточные текстуры `Image11` в ANGLE. За прогон под отладчиком создано только 3.

## Что не проверено

- Первопричина пунктов 1 и 2: стек выделения и D3D-список ещё не сняты. Без них про остаток 2,5 ГБ и 530 МБ нельзя сказать ничего.
- Какие объекты дают 211,5 МБ associated memory.
- Что игра в хосте дошла до того же состояния, что в CEF/Chromium (`clayBoot.ready` в хосте не прочитан). Косвенный признак: JS-куча та же. На
  этом измерение лучше не считать окончательным.
