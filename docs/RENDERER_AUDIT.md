# Аудит renderer и план развития движка

Продолжение реализации — [runtime, visibility, LOD, shadows и assets](ENGINE_EVOLUTION.md).
Ниже сохранены находки и измерения исходного аудита; это историческая контрольная точка.

Дата: 2026-09-11. Исходная ревизия: `405f2c3`. Проверяется нативный Windows x64,
RX 6800, Ryzen 7 9700X. Исходное рабочее дерево было чистым.

## Вывод

Это работоспособный прототип renderer, но пока не доказанная основа движка
с минимальным overhead при произвольном росте сцены. Два frame context,
device-local static data, persistently mapped per-frame uploads, instancing,
dynamic rendering и synchronization2 — подходящая основа. Переписывать всё
или автоматически добавлять threads/async compute/bindless оснований нет.

3000 FPS означает примерно 0.333 ms на кадр в конкретном режиме. Если добавить
0.5 ms последовательной работы на критический путь такого кадра, получится
около 1200 FPS. Наличие технологий и высокий FPS простой сцены не доказывают
низкую стоимость каждой технологии или масштабируемость.

Аудит охватывает application loop, renderer, frame ownership, swapchain,
Vulkan device selection, GPU buffers/textures, FSR integration, shaders,
metrics, scene submission и build/test automation. Asset importer и редактор
проверены в точках взаимодействия с renderer; это не исчерпывающий аудит
всего стороннего кода или всех форматов импортируемых файлов.

## Подтверждённые проблемы исходного кода

1. **P0: WSI synchronization.** В `Renderer::record_commands` переход
   acquired swapchain image из UNDEFINED использует src stage NONE, тогда как
   submission ждёт acquire на COLOR_ATTACHMENT_OUTPUT. Нет execution chain
   от acquire wait к layout transition. Present signal с ALL_GRAPHICS также
   не покрывает завершающий переход с dst NONE. В существующем validation log
   присутствуют WRITE_AFTER_READ и PRESENT_AFTER_WRITE. Depth targets
   привязаны к image index, а fence — к frame index; повторный depth clear
   также требует зависимости от предыдущих depth accesses.
2. **P1: error lifetime.** `fatal` и `check_vk` вызывают `std::exit`, обходя
   destructors автоматических объектов. Поэтому основной RAII cleanup не
   работает при ошибке инициализации/Vulkan. Простая замена exit на throw
   недостаточна: временные shader modules, ImmediateSubmission constructor
   и частично инициализированный ImGui backend требуют отдельного rollback.
3. **P1: SMAA выполняет temporal работу.** Spatial SMAA использует три color
   attachments и temporal vertex/fragment shaders: HDR color, RG16F motion
   vectors, R32F depth, плюс обычный depth attachment. Два дополнительных
   color outputs SMAA не читает. При 1920×1080 это 16.59 MB лишних логических
   записей за кадр до аппаратного сжатия/кэширования, плюс varying/ALU/barriers.
4. **P1: temporal ABI требует исправления.** AA shaders возвращают
   `previousNdc - currentNdc`; Vulkan viewport переводит эту разность в UV
   с множителем 0.5. В SDK 1.1.4 host делит motionVectorScale на render size,
   shader использует результат непосредственно как UV offset. Текущий scale
   равен полному render size. Знаки pixel jitter также должны следовать
   фактическому смещению projection при положительной высоте Vulkan viewport.
   Предыдущие transforms объектов не хранятся; этот путь рассчитан на
   статические объекты и движение камеры, а не полноценную анимацию.
5. **P2: fullscreen HUD.** Маленькая панель 468×44 в `hud.frag` рисуется
   fullscreen triangle с discard за её пределами. Scissor может отсечь эту
   работу до fragment shading, сохранив изображение.
6. **P1: benchmark не является полным регрессионным gate.** CPU frame
   заканчивается до window-title update; 1% low вычисляется по последним
   2000 samples, а не всему measurement interval. JSON теряет mode,
   triangles и большую часть уже собираемых timings. Нельзя сопоставлять
   такой 1% low с полным прогоном или путать отсутствующий GPU timing с нулём.
7. **P2: дорогой metrics snapshot в editor frame.** Percentile selection
   выполняет до 2000×20 сравнений и вызывается редактором каждый кадр.
   Отображение latest/average timings не требует пересчёта percentile.
8. **P1: smoke может пропустить synchronization errors.** `mcp_smoke.ps1`
   ищет `Validation Error|VUID-`, но сообщения sync validation могут содержать
   только `[Vulkan ERROR]` и `hazard`. Успешный exit code недостаточен.
9. **P2: device selection неполон.** Renderer всегда создаёт compute pipeline
   и пишет compute timestamps, но выбирает первую graphics queue без
   требования COMPUTE. Проверка compute только для FSR не защищает workload
   path. Required queue/limits нужно проверять до использования.

## Ограничения роста сцены

- CPU submission группирует одинаковые models/submeshes и cube instances,
  но frustum/occlusion culling и LOD отсутствуют. GPU обрабатывает все
  submitted instances. Million-instance benchmark использует иной ABI и
  простой Lambert shader, поэтому не доказывает производительность PBR scene.
- `render_static_shadow_map` вызывается при startup, рисует terrain и в demo
  sample spheres. Authored cubes/imported models в shadow pass не попадают;
  изменения сцены не инвалидируют эту карту. Dynamic shadows — отдельная
  реальная работа, отсутствующая в текущем steady-state benchmark.
- Model/texture load и ImmediateSubmission выполняются синхронно на render
  thread. Добавление нового asset может вызвать filesystem/decode/allocation
  и submission+fence wait. Это непригодный путь для streaming во время игры.
- Scene revision инвалидирует snapshot целиком. Динамическому runtime нужны
  dirty transforms, устойчивые handles, previous transforms и bounded
  frame packets; editor document не должен становиться game hot path.
- AA targets всех методов, FSR context и benchmark instances создаются
  заранее даже для AA off. Это цена startup/VRAM за переключение без stalls,
  а не автоматически per-frame FPS loss. Нужны профили runtime capabilities
  и подготовка ресурсов вне frame, прежде чем заменять это lazy allocation.
- Resize пересоздаёт AA targets и FSR context с device idle. При изменении
  только present mode и неизменных extent/formats это лишняя работа.
- Asset caches ограничены числом и байтами, но не имеют eviction/retirement
  и полной high-water статистики. Static analyzer targets отсутствуют.
- CPU path в renderer не содержит явного per-frame heap allocation при
  неизменной сцене. Это наблюдение по application code, а не доказательство
  отсутствия allocation в SDL, драйвере, FidelityFX и ImGui.

## План изменений

### Этап 1 — реализован в этой задаче

- Исправить WSI/depth synchronization, проверить off/SMAA/FSR/workload,
  capture и resize/mode-switch с synchronization validation.
- Дать ошибкам пройти через cleanup; закрыть временные Vulkan lifetimes,
  проверить missing-resource failure и штатный shutdown с VMA statistics.
- Разделить spatial SMAA и temporal FSR geometry targets/pipelines;
  ограничить HUD scissor. Не менять геометрию, материалы, AA quality или
  разрешение ради увеличения FPS.
- Исправить temporal coordinate contract с CPU regression tests.
- Сделать benchmark samples полными и ограниченными по памяти; сохранить
  mode/workload/timings и явно описать semantics. Убрать percentile work
  из UI hot path. Добавить regression tests на ранний hitch и overflow.
- Усилить Windows smoke, прогнать Release/Debug/ASan и сравнимый baseline.

Реализация: acquire transition получает execution dependency от semaphore
wait; сигнал для present охватывает весь submission. Depth reuse учитывает
предыдущие depth accesses. Это исправляет корректность, а не сокращает работу
ради цифры FPS. Frame command pools сбрасываются целиком без ненужного флага
individual command-buffer reset.

Shader modules, временный upload command pool/fence и geometry AA pipelines
теперь move-only RAII. Fatal Vulkan/initialization errors доходят до catch в
`main` через unwind; shader creation failure не оставляет созданные ранее
ресурсы. Cleanup не пытается выделять память для FSR fallback strings, а
диагностика ошибок в затронутых `noexcept` путях не использует throwing streams.
Это постепенное устранение ручного владения: остальные старые resource groups
ещё используют существующий `initialize/shutdown` lifetime.

SMAA использует один HDR color output и direct shader variants; FSR сохраняет
три outputs. Пять дополнительных SMAA pipelines создаются заранее — есть цена
startup/pipeline memory, зато переключение не компилирует pipeline внутри кадра.
HUD ограничен scissor. При неизменных scene extent/formats пересоздание
swapchain сохраняет AA targets и FSR context.

FSR получает корректные motion scale/pixel jitter и фактический frame time
ниже 1 ms. Изменение сцены или workload сбрасывает temporal history до внедрения
previous object transforms. Это корректный fallback для редактирования,
но не полноценная temporal обработка движущихся объектов.

Benchmark использует заранее выделенный bounded массив, полный measurement
interval и JSON schema 2. Подробная семантика описана в
[Windows development](WINDOWS_DEVELOPMENT.md#benchmark-json).

### Этап 2 — корректная динамическая сцена

- Ввести runtime frame packet: contiguous transforms, previous transforms,
  mesh/material handles, bounded draw batches. Game systems — C++, editor
  document преобразуется в runtime данные на явной границе.
- Добавить dirty transform propagation и bounds; CPU frustum culling
  с прямым путём для маленьких сцен. Измерить порог parallel jobs.
- Сделать shadow pass частью scene dependencies; сначала корректные casters,
  invalidation и pass timings, затем caching/LOD/culling теней.
- Acceptance: движущийся объект имеет корректные motion vectors и тень;
  1/1000/100000 объектов и offscreen objects проверяются отдельно;
  steady-state allocations отсутствуют по счётчику.

### Этап 3 — загрузка и память

- Bounded persistent worker pool для IO/decode/подготовки ресурсов; render
  thread остаётся единственным submission owner.
- Пакетные uploads через persistently mapped ring, retirement по fence/value,
  limits и high-water counters. Отдельная transfer queue — только после
  измерения выигрыша с ownership transfers.
- Acceptance: streaming не вызывает device idle/per-asset render-thread
  waits, все временные ресурсы освобождаются после последнего GPU use;
  startup/shutdown/resize/asset churn проходят soak.

### Этап 4 — новые графические технологии

- Явные pass inputs/outputs и per-pass CPU/GPU timings. Начать с небольшого
  статического списка проходов; полноценный render graph нужен при реальных
  producer/consumer и transient-memory задачах.
- GPU culling/indirect count — при измеренном CPU submission bottleneck;
  bindless — при росте resource/material count; async compute — при доказанном
  overlap. Для optional features обязательны detection и fallback.
- Для каждого эффекта хранить cost budget в ms и bytes, качество, разрешение,
  CPU/GPU bottleneck, median/разброс минимум трёх A/B прогонов. Проверять также
  тяжёлую сцену, а не только пустой/маленький benchmark.

## Проверки и измерения

Генерируемые логи и JSON находятся только внутри трёх разрешённых build tree,
в подкаталогах `audit-20260911` и `render-validation`. Коммиты и push не выполнялись.

### Корректность

| Проверка | Результат |
| --- | --- |
| Native MSVC Release / Debug / ASan | Сборки успешны, CTest 4/4 в каждой |
| Новые CPU regression cases | Ранний hitch вне rolling window, overflow/invalid samples, JSON semantics, NDC motion и projection jitter |
| Synchronization validation до исправления | WRITE_AFTER_READ / PRESENT_AFTER_WRITE воспроизведены в off, SMAA и FSR при exit code 0 |
| Debug и ASan после исправления | Для каждой конфигурации 3 startup/shutdown × 2 цикла; 30 GPU captures, resize, FIFO/IMMEDIATE, 3 AA метода, 100k instances, compute workload |
| Результат этих сценариев | 0 Vulkan/synchronization/ASan errors, при каждом shutdown 0 VMA allocations / 0 bytes |
| Ошибка инициализации | Отсутствующие model shader и поздний SMAA shader: exit 1, cleanup завершён, 0 VMA allocations, 0 validation errors |
| Editor Debug | Startup, SMAA rendering и штатный shutdown с sync validation: 0 ошибок / 0 VMA allocations |
| Скриншоты 640×360 | PNG до/после для off и SMAA совпадают побайтно; FSR изменяется вслед за исправлением temporal coordinates |
| SPIR-V | Все 34 versioned binaries прошли native `spirv-val --target-env vulkan1.3`; исходники шейдеров и binaries не менялись |
| Static analysis | Нативный clang-tidy: renderer, application, metrics, FSR, Vulkan context, GPU buffer; подтверждённые проблемы затронутого cleanup устранены |

В clang-tidy включены `clang-analyzer-*`, `bugprone-*`, `performance-*`,
`concurrency-*`, owning-memory и pro-bounds checks. Остались общие предупреждения
о pointer arithmetic на проверенной C API границе, индексировании bounded
массивов, style/API shapes. Два widening warnings относятся к compile-time
`max_frames_in_flight * 2` при значении 2, а не к переполнению входного размера.
Проект не объявляется полностью чистым по static analysis. MSVC `/analyze`
и отдельные analyzer CMake targets остаются инфраструктурным долгом.

VMA counters не доказывают отсутствие всех CPU/OS leaks. CPU allocation
profiler, hardware counters RGP/PIX, длительный soak с asset churn,
minimize/fullscreen, device-loss fault injection, exhaustive ImGui partial-init
failure и объективная FSR temporal image-quality оценка в движении не выполнялись.
ASan не инструментирует все сторонние DLL. Проверено только это Windows/RX 6800
окружение; Linux не входит в MVP gate.

FSR debug checking может предупреждать о `frameTimeDelta < 1 ms` при высоком
FPS. Это фактическая длительность кадра в данном benchmark; прежнее округление
вверх до 1 ms искажало вход SDK. Предупреждение не скрывается и не считается
synchronization error; в Release debug checking выключен.

### Условия A/B

RX 6800, Ryzen 7 9700X, Windows driver `32.0.21045.5002` (Vulkan driver
`8388961`), MSVC Release, 1920×1080, IMMEDIATE, validation off. Power scheme
«Сбалансированная», clocks/power не фиксировались. Фоновые приложения оставлены
как были; компиляция, анализаторы и другие renderer tests во время benchmark
не запускались. Частоты/температуры аппаратным профайлером не записывались,
поэтому малые изменения нельзя уверенно отделить от шума.

Для каждой стороны и режима — минимум 3 прогона, 5 s warmup + 15 s measurement.
Scene не изменялась: 18 authored objects, 139533 triangles. Это уже другая сцена
и другое разрешение, чем исторический ориентир AGENTS.md на 720p / 170 triangles.
Instancing отдельно использует 100000 экземпляров. SMAA High — 1920×1080;
FSR Quality — render 1280×720 / output 1920×1080.

Исходный executable сохранён как `windows-release/bin/vulkan_cube.audit-before.exe`.
Артефакты `before-*.json`, `after-*.json`, `.log` содержат первичные результаты.
Исходный 1% low имеет другой охват и с новым полным 1% low не сравнивается.
FPS до исправления слегка завышен из-за пропущенного CPU bookkeeping;
число frames / длительность используется как дополнительная сверка throughput.

### Результаты

Значения — медианы трёх прогонов. CPU/GPU — в ms на кадр.

| Режим | FPS до → после | Изменение FPS | CPU до → после | GPU до → после |
| --- | ---: | ---: | ---: | ---: |
| Scene, AA off | 2860.6 → 2843.9 | −0.6% | 0.350 → 0.352 | 0.187 → 0.187 |
| Scene, SMAA High | 2229.9 → 2575.7 | +15.5% | 0.448 → 0.388 | 0.273 → 0.226 |
| Scene, FSR Quality | 1227.3 → 1276.2 | +4.0% | 0.815 → 0.784 | 0.638 → 0.615 |
| Instancing 100k, AA off | 2744.2 → 2799.6 | +2.0% | 0.364 → 0.357 | 0.203 → 0.196 |

| Режим | Диапазон FPS до | Диапазон FPS после | Полный 1% low после, диапазон |
| --- | ---: | ---: | ---: |
| Scene, AA off | 2843.3–3178.8 | 2843.5–2844.9 | 1978.1–2040.2 |
| Scene, SMAA High | 2215.6–2301.8 | 2564.9–2588.3 | 1895.3–1930.6 |
| Scene, FSR Quality | 1208.3–1248.2 | 1272.2–1276.5 | 1073.7–1075.1 |
| Instancing 100k, AA off | 2716.6–2751.0 | 2771.4–2827.1 | 1967.1–2022.9 |

Наиболее уверенный performance результат — SMAA: GPU frame −17.3%,
при побайтно одинаковом контрольном PNG. Для off изменения не отделяются
от шума исходной серии; ускорение этого режима не заявляется. FSR и instancing
показывают умеренный выигрыш в этой серии, без обещания воспроизводимости
этих процентов на другом железе. Все четыре режима сохранили workload и
качество. Регрессии медианы свыше 3% не обнаружено.

Сверка через реальный throughput даёт тот же вывод: у SMAA было 33422 frames
за номинальные 15 s (медиана), стало 38636 за фактические ≈15 s. Выигрыш не
создан сменой формулы FPS. Новая JSON-метрика во всех 12 after runs содержит
`frames_rendered == percentile_samples` и `full_measurement`.

После изменения off тратит около 0.352 ms CPU frame, из которых наблюдаемый
`vkQueuePresentKHR` занимает 0.264 ms; record — 0.010, submit — 0.019,
acquire — 0.014, fence wait — 0.021 ms (медианы). Это указывает на важность
present/driver scheduling для текущего маленького workload. API time не
является чистой стоимостью CPU-кода: в нём могут находиться ожидания.
Без CPU/GPU profiler это не окончательная атрибуция bottleneck и не основание
добавлять threads, менять present semantics или уменьшать обязательную sync.

FSR после исправления имеет GPU frame ≈0.615 ms при geometry ≈0.104 ms;
на такой лёгкой сцене upscaling/post-process существенно дороже самой geometry.
Новый графический эффект обязан иметь собственный измеренный бюджет.
Следующий инженерный приоритет — этап 2: корректные dynamic transforms,
motion vectors/shadows и culling с отдельным benchmark роста сцены.

## Основания для Vulkan/FSR выводов

- [Khronos synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html)
- [Khronos synchronization2 semaphore scope](https://docs.vulkan.org/guide/latest/extensions/VK_KHR_synchronization2.html)
- [Khronos present semaphore lifetime](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)
- [AMD FSR integration](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-upscaler/)

Для motion vectors проверен именно локальный SDK 1.1.4:
`ffx_fsr3upscaler.cpp` normalizes scale, `ffx_fsr3upscaler_callbacks_glsl.h`
возвращает UV offset. Пример NDC в веб-руководстве AMD сам по себе недостаточен
для проверки множителя: решающим является фактический producer/consumer ABI.
