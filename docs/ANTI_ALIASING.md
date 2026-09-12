# Сглаживание

Renderer поддерживает три runtime-режима: `off`, SMAA 1x и temporal upscaler
FSR 3. Переключение не требует пересоздания swapchain.

## Управление

| Клавиша | Действие |
| --- | --- |
| `F6` | `off` → SMAA → FSR 3 → `off` |
| `F7` | понизить качество активного метода |
| `F8` | повысить качество активного метода |

Текущий метод, его качество и эти клавиши всегда выводятся в левом верхнем углу
render window. HUD рисуется после post-process, поэтому сам текст не попадает в
temporal history FSR и остаётся резким.

Те же настройки доступны при запуске:

```bat
vulkan_cube.exe --aa off
vulkan_cube.exe --aa smaa --smaa-quality ultra
vulkan_cube.exe --aa fsr3 --fsr-quality quality
```

SMAA имеет уровни `low`, `medium`, `high`, `ultra`. FSR 3 имеет
`native-aa`, `quality`, `balanced`, `performance`, `ultra-performance`.
Последние соответствуют масштабам 1.0×, 1.5×, 1.7×, 2.0× и 3.0× между
display и внутренним render resolution. Оба разрешения вместе с выбранным
методом и качеством записываются в `benchmark.json`.

Если FSR недоступен, запрос `--aa fsr3` безопасно откатывается на SMAA, а `F6`
пропускает FSR. SMAA и `off` при этом продолжают работать.

## Render path

При динамическом освещении (в том числе с AA off) и при включённом сглаживании
geometry, sky и уже рассчитанное shadowed lighting
сначала записываются в общий HDR color target. Поэтому post-process покрывает
одновременно силуэты объектов, границы материалов и видимые края теней:

- SMAA пишет только HDR color и обычный depth attachment, затем выполняет
  color edge detection, blending-weight и neighborhood passes; motion vectors
  и отдельный R32F depth output этому spatial методу не нужны;
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
вариации заранее создаются из существующих SPIR-V.

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
убирает undefined load/store, обнаруживаемые validation layer. На Linux
собираются SMAA и `off`; FSR сообщает о недоступности и использует fallback.

Исходный SMAA `v2.8`, lookup textures и лицензия находятся в
`shaders/third_party/smaa`, `assets/smaa` и `LICENSES/SMAA.txt`. Лицензия
FidelityFX находится в `LICENSES/FidelityFX-SDK.txt`.

После изменения shader source следует пересобрать и проверить все SPIR-V:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts\compile_shaders_windows.ps1
```

Детали суток, CSM и GI: [DYNAMIC_LIGHTING.md](DYNAMIC_LIGHTING.md).
