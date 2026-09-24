# Сглаживание

Renderer поддерживает `off`, spatial SMAA 1x, optional native-resolution SMAA
T2x, forward MSAA 2×/4× и temporal upscaler FSR 3. Переключение не требует
пересоздания swapchain. `off` и SMAA 1x остаются прежними default choices;
T2x включается явно.

## Управление

| Клавиша | Действие |
| --- | --- |
| `F6` | `off` → SMAA → MSAA 2× → MSAA 4× → FSR 3 → `off`; unsupported/incompatible режимы пропускаются |
| `F7` | понизить качество активного метода |
| `F8` | повысить качество активного метода |

Текущий метод, его качество и эти клавиши всегда выводятся в левом верхнем углу
render window. HUD рисуется после post-process, поэтому сам текст не попадает в
temporal history FSR и остаётся резким.

Те же настройки доступны при запуске:

```bat
vulkan_cube.exe --aa off
vulkan_cube.exe --aa smaa --smaa-quality ultra
vulkan_cube.exe --aa smaa --smaa-mode t2x --smaa-quality ultra
vulkan_cube.exe --aa msaa2x
vulkan_cube.exe --aa msaa4x
vulkan_cube.exe --aa fsr3 --fsr-quality quality
```

`--smaa-mode` принимает строго `1x` или `t2x`; без него используется `1x`.
Оба режима SMAA имеют уровни `low`, `medium`, `high`, `ultra`. FSR 3 имеет
`native-aa`, `quality`, `balanced`, `performance`, `ultra-performance`.
Последние соответствуют масштабам 1.0×, 1.5×, 1.7×, 2.0× и 3.0× между
display и внутренним render resolution. Оба разрешения вместе с выбранным
методом и качеством записываются в `benchmark.json`.

Если FSR недоступен, запрос `--aa fsr3` безопасно откатывается на SMAA, а `F6`
пропускает FSR. Startup MSAA4 откатывается на MSAA2 или Off, MSAA2 — на Off;
requested и actual mode/samples записываются раздельно. Runtime setter отклоняет
unsupported count вместо тихого fallback.

## Render path

При динамическом освещении (в том числе с AA off) и при включённом сглаживании
geometry, sky и уже рассчитанное shadowed lighting
сначала записываются в общий HDR color target. Поэтому post-process покрывает
одновременно силуэты объектов, границы материалов и видимые края теней:

- SMAA 1x пишет только HDR color и обычный depth attachment, затем выполняет
  color edge detection, blending-weight и neighborhood passes; motion vectors
  и отдельный R32F depth output этому spatial методу не нужны;
- SMAA T2x добавляет чередующийся jitter `±(0.25, -0.25)`, reference subsample
  indices и motion vectors. Neighborhood result текущего кадра сохраняется до
  следующего кадра без рекурсивного накопления; temporal resolve репроецирует
  его, ограничивает предыдущий sample текущим neighborhood и снижает history
  weight при быстром движении;
- MSAA пишет geometry/sky в `RGBA16F` color и выбранный depth format с 2/4
  coverage/depth samples. Dynamic rendering делает `AVERAGE` resolve в прежний
  single-sample HDR target; source имеет `storeOp=DONT_CARE`. Sample shading
  выключен. При включённой атмосферной дымке depth разрешается через
  `SAMPLE_ZERO` для её прохода; без дымки depth resolve не выполняется;
- FSR получает HDR color, non-jittered motion vectors и depth, выполняет
  temporal upscale/AA, затем результат компонуется в swapchain;
- динамическое освещение проходит fixed-exposure Reinhard после AA/FSR,
  перед единственным sRGB encode; AA off использует отдельный display pass;
- HUD добавляется последним и не влияет на изображение, которое анализирует
  алгоритм сглаживания.

Shadow map остаётся отдельным 2048×2048 depth pass с hardware comparison
filtering. Сглаживание применяется к итоговым видимым теням, а не меняет
геометрию shadow map.

AA targets принадлежат конкретному `FrameContext`, поэтому CPU не перезаписывает
изображение, пока GPU использует его. Images, samplers, descriptors, pipelines и
FSR context создаются вне steady-state frame loop и освобождаются после
`device idle` при изменении scene extent/formats или shutdown. Смена только
present mode сохраняет offscreen targets и FSR context. Обе geometry pipeline
вариации заранее создаются из существующих SPIR-V. MSAA targets конкретного
frame slot создаются/заменяются после его fence; steady state не аллоцирует и
runtime switch не вызывает device idle.

T2x использует два уже выделенных per-frame `RGBA16F` FSR output как current и
previous history. При 1920×1080 они занимают 31.64 MiB resident storage, но
добавочный расход T2x равен 0 bytes. History validity и фаза сбрасываются при
resize, camera cut и изменениях состояния, нарушающих соответствие кадров.
Previous input всегда является самостоятельным neighborhood result кадра N−1,
а не ранее resolved history, поэтому двухкадровый контракт не превращается в
бесконечную экспоненциальную историю.

Текущая реализация делит geometry temporal path с FSR и поэтому при T2x также
пишет full-resolution `R32F` depth output, хотя temporal resolve SMAA его не
читает. Приведённые ниже замеры уже включают этот лишний bandwidth; отдельный
color+motion geometry variant остаётся локальной оптимизацией без изменения
качества или публичного контракта.

Единственный detector SMAA — color. Luma A/B на 74-pixel isoluminant chroma ROI
дал MAE `0.0288994` для color и `0.0536648` для luma (примерно в 1.86 раза хуже),
а разница производительности была шумом: `+0.0454%` FPS, `-0.00194 ms` GPU frame
и `-0.00230 ms` postprocess. Depth detector также не используется: он теряет
same-depth material/specular/shadow edges, требует sampled depth и отдельного
reverse-Z threshold contract.

MSAA и current full-resolution SSS взаимоисключающие. SSS alpha содержит
дискретный packed profile ID и strength; обычный average resolve смешал бы mask
на coverage/material edges. CLI и typed runtime/editor MCP отклоняют комбинацию
в обоих направлениях. Полноценное сочетание потребовало бы отдельного
per-sample SSS design.

Temporal shaders записывают `previousNdc - currentNdc`. В Vulkan backend
SDK 1.1.4 это переводится в пиксели через `motionVectorScale = renderSize / 2`:
внутри SDK scale делится на render size для получения UV offsets. Jitter,
передаваемый FSR, соответствует фактическому смещению пикселя после projection
и положительного Vulkan viewport; контракт проверяется CPU-тестом.
Runtime instances теперь хранят current/previous object transforms (128 bytes).
Previous обновляется только после успешного submission; одноразовое движение
имеет отдельный settling frame. GPU compaction сохраняет исходный instance ID.
Структурное редактирование сцены
сбрасывает историю. Движение rigid objects через runtime transform API
формирует object motion vectors без такого сброса. Skeletal animation не реализована.

На equal-luma `aa-wires` T2x улучшил edge MAE SMAA1x с `0.05203927` до
`0.04802657` (примерно 7.7%). Цена составила `+0.04129 ms` GPU frame на
`aa-wires` и `+0.06782 ms` на `overdraw-4`. MSAA4 на этих измерениях оказался
качественнее и быстрее T2x, но требует 189.8 MiB multisample attachments;
FSR Native качественнее обоих SMAA, но существенно дороже. Поэтому T2x оставлен
optional без изменения defaults. Проверены static fixture и moving-camera
sync/screenshot smoke; production motion, disocclusion, foliage и glossy-content
quality пока не доказаны. SMAA S2x/4x отложены: для них нужны сохранённый sampled
2× source, разбор subsamples, две SMAA chains, standard sample locations и
multisample velocity contract.

## Версия FSR и платформы

В Vulkan-сборке используется официальный FidelityFX SDK `v1.1.4` и FSR
upscaler `3.1.4`. Это последняя официальная ревизия FSR 3 с Vulkan backend.
Более новый FidelityFX SDK 2.x содержит FSR 3.1.5, но его официальный runtime
на данный момент не поддерживает Vulkan. Frame Generation намеренно не
включён: эта настройка отвечает только за сглаживание/upscale.

На Windows CMake получает SDK из официального репозитория, собирает Vulkan
runtime из исходников и кладёт `amd_fidelityfx_vk.dll` рядом с executable.
В SDK `v1.1.4` исправляется известное несоответствие Vulkan GLSL: luma history
создаётся как RGBA16F, но storage image ошибочно объявлен как `rgba8`. Локальная
однострочная коррекция на `rgba16f` проверяет исходную строку перед изменением и
убирает undefined load/store, обнаруживаемые validation layer. Текущий MVP
собирается и проверяется только нативно под Windows x64.

Исходный SMAA `v2.8`, lookup textures и лицензия находятся в
`shaders/third_party/smaa`, `assets/smaa` и `LICENSES/SMAA.txt`. Лицензия
FidelityFX находится в `LICENSES/FidelityFX-SDK.txt`.

После изменения shader source следует пересобрать и проверить все SPIR-V:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts\compile_shaders_windows.ps1
```

Детали суток, CSM и GI: [DYNAMIC_LIGHTING.md](DYNAMIC_LIGHTING.md).
