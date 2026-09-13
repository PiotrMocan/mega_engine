# Depth prepass и порядок opaque geometry

`--depth-prepass off|partial|full` выбирает предварительное заполнение основной
reverse-Z глубины. По умолчанию `off`. Runtime MCP `render.set_depth_prepass`
принимает строго `{ "mode": "off|partial|full" }`; editor имеет тот же выбор.
Режимы используют заранее созданные pipelines и переключаются без allocation,
пересоздания descriptors или ожидания GPU. На RX 6800 при фиксированных частотах
prepass увеличил GPU-время на 3.53–3.65% для одного слоя и уменьшил примерно
на 81.75% для 16 слоёв. Это результаты процедурной нагрузки, не общий прогноз.

`partial` включает terrain и primitive batches (ground, cubes, встроенные
PBR samples), пропуская imported model batches. Это явно заданный выбор классов
opaque geometry, а не автоматическое определение крупных occluders. `full`
включает все opaque batches. Sky не участвует в prepass. Visibility/LOD selection,
indirect arguments, instance transforms и jitter одинаковы в обоих проходах.

## Depth и lifetime contract

- Prepass использует тот же vertex SPIR-V, vertex layout, rasterization,
  viewport/scissor, index/instance ranges и frame uniforms, что и его color path.
  `invariant gl_Position` обеспечивает согласованный clip depth; для FSR
  используются соответствующие temporal vertex shaders с тем же jitter.
- Depth-only dynamic rendering: clear 0, GREATER, depth writes, STORE. Color
  attachments и fragment shader отсутствуют. В текущих opaque shaders нет
  alpha test/discard/deformation с отличающимся coverage. Будущие материалы с
  такими свойствами должны получить согласованный coverage path до включения.
- После prepass image barrier depth-attachment WRITE в EARLY/LATE tests →
  READ/WRITE в EARLY/LATE tests. Следующий rendering instance делает LOAD.
  Prepassed color batches используют EQUAL и depth-write-off; imported models
  в `partial` сохраняют GREATER с depth writes. Финальный depth STORE остаётся
  DONT_CARE, пока у attachment нет следующего consumer.
- Off path сохраняет один geometry rendering instance с CLEAR. Новых images,
  buffers и CPU readbacks нет. Pipelines имеют RAII ownership и уничтожаются
  после retirement вместе с остальными swapchain pipelines. Timestamp queries
  принадлежат FrameContext и читаются после fence без WAIT flag.

Правила основаны на Vulkan
[invariance](https://docs.vulkan.org/spec/latest/appendices/invariance.html),
[GLSL invariant](https://docs.vulkan.org/glsl/latest/chapters/variables.html)
и [dynamic rendering load/store](https://docs.vulkan.org/spec/latest/chapters/renderpass.html).
Разные материалы на точно совпадающих поверхностях требуют отдельного порядка
композиции: EQUAL допускает все равные depth значения, поэтому такой конфликт
нельзя использовать как эталон уникальной видимой поверхности.

## Измерения и fixtures

`gpu_depth_prepass_ms` / `average_gpu_depth_prepass_ms`: 0 при выключенном
проходе, unavailable (-1 в runtime / null в benchmark JSON) при недоступных
timestamps, иначе отдельный интервал GPU. `gpu_geometry_ms` включает оба
geometry passes; складывать его с prepass timing второй раз нельзя. Полный
кадр включает и дополнительную работу vertex/raster/depth, и экономию shading.
Начало prepass и geometry — одна метка. Prepass/geometry/postprocess end
используют ALL_COMMANDS, чтобы включить предыдущие timestamp writes в first
synchronization scope и гарантировать их порядок, без дополнительных barriers.
Это интервалы completion на одной queue; они не являются изолированной
стоимостью shaders при исключённом overlap. [Vulkan timestamp scope](https://docs.vulkan.org/spec/latest/chapters/queries.html#queries-timestamps).

Процедурные `--render-fixture overdraw-1|overdraw-4|overdraw-16` создают по
128 opaque boxes на слой через обычный SceneDocument/upload path. Слои
располагаются от дальнего к ближнему и имеют одинаковый projected footprint,
без coplanar depth ties. `--camera-z 12 --culling frustum --lod off` сохраняет
видимые перекрывающиеся слои для замера, без occlusion culling. Генерация
выполняется только при startup, production assets не требуются.

Скрипт `scripts/validate_depth_prepass_windows.ps1` сравнивает off/partial/full/off
при одинаковой позе, проверяет invalid controls, delayed metrics, resize, AA,
mode switches и shutdown. Точные decoded pixels сравниваются для статичных
AA off/SMAA paths; FSR history/jitter и меняющийся GPU-saturation workload
фиксируются отдельно. Результаты фактически выполненных gates и A/B находятся
в `docs/RENDERING_PIPELINE_RESULTS.md`.

Для canonical imported geometry при сравнении разных prepass modes введён
численный предел: не более 1 RGB8 LSB и не более 0.01% изменённых pixels;
alpha совпадает точно. Основание — воспроизводимое отличие одного канала
одного пикселя в legacy lighting на CPU и GPU visibility. Причина отличия
не установлена; это явный инженерный допуск, не утверждение об одинаковых
пикселях. Procedural fixtures и off→off restoration сохраняют exact comparison.

## Coarse front-to-back

`--front-to-back off|coarse` (default off) сортирует только CPU visible indices
внутри прежних ground/cube/model-LOD ranges. Ключ — ближайшая camera-space
depth AABB, квантованная с шагом 8 м, затем исходный source index для устойчивого
порядка. Batching boundaries, LOD, stable IDs, previous transforms и исходный
shadow packet не меняются. `std::sort` использует существующий заранее
выделенный scratch vector; новых per-frame allocations нет.

GPU visibility и instancing сохраняют свой порядок. Requested/actual значения
пишутся отдельно в debug state и benchmark. Sorting зависит от камеры и может
увеличить CPU preparation cost. Ключи вычисляются один раз на видимый instance
и хранятся в bounded scratch buffer (400004 bytes при максимальной capacity).
Если visibility/LOD не зависят от камеры и данные не изменились, точное
совпадение строки clip.w позволяет переиспользовать прежний порядок. Для
остальных случаев сохраняется проверка всей матрицы и extent. Сравнения с
epsilon здесь нет. Результаты и ограничения 1% low описаны в отчёте E06.
