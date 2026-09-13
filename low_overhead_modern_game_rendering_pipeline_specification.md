# Modern Low-Overhead Game Rendering Pipeline Specification

**Версия:** 1.5.0 — исследование, решения и измеренные этапы развития `vulkan_cube_renderer`
**Дата проверки:** 2026-09-13
**Исходный материал:** Threat Interactive — *Tutorial: How To Optimize Almost Every Step In Modern Game Rendering*; предоставленный пользователем список версии 1.0.0
**Проверенная база:** рабочие исходники `C:\GDF\vk_test`, HEAD `675332e`; имевшиеся изменения `src/editor_ui.cpp` не относятся к этому исследованию.

Это **аннотированный разбор**, а не требование внедрить pipeline из видео целиком. Для нашего forward renderer часть списка полезна, часть уже реализована, часть требует новых подсистем движка. Пользователь подтвердил развитие движка **до появления production content**: рекомендованные engine features реализуются вместе с процедурными тестовыми сценами; отсутствие игровых assets не является причиной их откладывать. Утверждение «лучше существующего» ниже означает либо подтверждённое свойство метода (например, precision reverse-Z), либо инженерную оценку пригодности с указанным условием. **Ускорение на нашем проекте без A/B не установлено.**

Проверены текущие C++/GLSL paths, спецификация Vulkan, работы авторов методов, материалы GPU vendors и разработчиков движков. Исходные тезисы каждого раздела сохранены в раскрываемом блоке **«непроверенный исходный материал»**; нормативное решение задаётся русской аннотацией перед ним. Это предотвращает использование исходных `MUST`/`assert` вопреки результатам проверки.

[Страница исходного видео](https://www.youtube.com/watch?v=SnNm7rSSvlg) найдена, но прямое получение её содержимого было ограничено веб-инструментом. Полная запись/транскрипт не просмотрены; точность цитат и таймкодов исходного списка **не подтверждена**. Поэтому ошибки формулировок относятся к предоставленному документу, а не автоматически приписываются автору видео. Числа 5%, 13–40%, +40% и бюджеты проходов без воспроизводимого эксперимента считаются неподтверждёнными.

### Как читать решения

- **Рекомендовано к имплементации** — хороший кандидат; сначала выполнить указанные prerequisites и criteria, затем решать о включении по умолчанию.
- **Рекомендовано; уже реализовано** — сохранить/проверить существующее решение; повторная реализация не нужна.
- **Рекомендовано к имплементации при условии…** — принятый кандидат с техническими prerequisites или измеряемыми условиями выбора пути. Prerequisites входят в очередь разработки движка; масштаб и свойства будущего контента воспроизводятся процедурными fixtures. Условие производительности определяет выбор default/auto path, а не необходимость ждать production assets.
- **Рекомендовано пропустить** — отвергается указанная формулировка или её применение в нашем проекте; это не утверждение о бесполезности метода во всех движках.

Приоритет P1 — фундамент depth/attachments/display; P2 — подсистемы движка в порядке технических зависимостей; P3 — специальные quality modes. Все принятые пункты входят в scope; P3 не означает отказ от реализации. Производительность и качество определяют включение по умолчанию, crossover и fallback. Полная очередь, synthetic acceptance scenes и текущие статусы приведены в [плане реализации](C:/GDF/vk_test/docs/RENDERING_PIPELINE_IMPLEMENTATION.md). В конце приведены первичные источники; обозначения [S01]–[S28] ведут к ним.

### База на момент исследования, до текущей реализации

| Область | Состояние, подтверждённое чтением кода |
| --- | --- |
| Visibility/LOD | CPU conservative grid/frustum, GPU classification/compaction/indirect, LOD errors/hysteresis, direct bypass |
| Main rendering | Forward PBR/HDR, один sun, environment и optional voxel diffuse GI; полноценного G-buffer нет |
| Depth | Обычный Z, `LESS`, clear 1, perspective 0.1–500; отдельного prepass нет |
| Shadows | Stable 4-cascade CSM, dirty cache, tent PCF; atlas 1024–8192, default 2048 |
| Materials | Metallic-roughness, GGX/Smith/Schlick; skin/eye/DMP material systems нет |
| AA | SMAA 1x и FSR temporal upscaler; MSAA/SMAA 4x отсутствуют |
| Display | Fixed-exposure Reinhard и один sRGB encode; scene metering/3D grading LUT нет |
| Новые эффекты | Отдельных SSS, particles/volumetrics, G-buffer decals, motion blur и SDF shadows в render path нет |

Архитектурные факты проверены по исходникам, а не по старым FPS-ориентирам из документации. Это исходный snapshot, не статус изменяемого сейчас кода; текущие статусы находятся в §15 и `docs/RENDERING_PIPELINE_RESULTS.md`. Основные ссылки на исходную code evidence приведены в соответствующих разделах.

---

## 1. Architectural Principles & System Design Rules

### Результат проверки: развивать существующий гибридный путь

**В проекте:** CPU frustum/консервативная occlusion grid и GPU frustum/occlusion/LOD с compaction уже сосуществуют. GPU получает подготовленную CPU сетку, а не полноценный Hi-Z из depth buffer. Для дешёвых кубов есть автоматический bypass по ограниченной выборке. Это видно в [renderer_scene.cpp](C:/GDF/vk_test/src/renderer_scene.cpp:31), [renderer_gpu_visibility.cpp](C:/GDF/vk_test/src/renderer_gpu_visibility.cpp:312) и [scene_visibility.comp](C:/GDF/vk_test/shaders/scene_visibility.comp:43).

| Техника из списка | Вердикт и сравнение с проектом |
| --- | --- |
| CPU SIMD для coarse culling, BVH и bounding volumes | **Рекомендовано к имплементации при росте CPU cost.** Направление разумное; часть CPU culling уже есть. Начать с contiguous bounds, кеширования и профиля. SIMD/worker pool добавлять при выигрыше над текущим прямым путём. Intel показывает жизнеспособность CPU occlusion, но не доказывает универсальные sub-ms сроки для нашего workload. [S02] |
| Перенести весь occlusion/frustum culling и простые анимации на CPU | **Рекомендовано пропустить как обязательную замену.** GPU culling может экономить raster/vertex работу и CPU submission; CPU-вариант платит за подготовку и передачу результатов. Skinning/анимацию оценивать отдельно по объёму данных. Сохранить выбор CPU/GPU/direct и сравнивать полный кадр. [S03] |
| Compute в начале кадра ≤5% | **Рекомендовано пропустить.** Непонятен даже знаменатель «total frame compute time». Важны критический путь, зависимости и overlap; универсального процента нет. Наличие dispatch само по себе не доказывает cache thrashing или starvation. [S01], [S03] |
| AVX2/AVX-512 как безусловная база | **Рекомендовано пропустить.** Выбирать ISA через runtime detection с fallback. Поколение x86 не гарантирует AVX-512, а упомянутые «десятки тысяч за <1 ms» не являются замером проекта. |

**Условие принятия:** минимум три A/B-прогона CPU/GPU/direct на одинаковых сценах с разной долей видимых объектов; измерять CPU preparation, upload, GPU visibility и общий frame time. Occlusion не должна скрывать видимые объекты. GPU readback ради culling текущего кадра не вводить.

<details>
<summary>Версия 1.0: непроверенный исходный материал — не требования к реализации</summary>

### 1.1 CPU vs. GPU Workload Distribution
* **Problem:** Modern engines frequently offload simple algorithmic tasks to GPU Compute Shaders early in the frame (e.g., micro-culling, simple animations, bounding box calculation) because the engine CPU threads are bloated with unoptimized script logic and poorly scheduled code. This induces GPU frontend starvation, memory cache thrashing, and pipeline bubble stalls.
* **Architecture Directive:**
  * Keep compute shader dispatches at the start of the frame strictly bounded ($\le 5\%$ of total frame compute time).
  * Shift occlusion culling, coarse bounding volume hierarchies (BVH), and frustum tests to native C++ SIMD threads on the CPU. A 9th-generation x86 CPU core executing optimized AVX2/AVX-512 code can easily cull tens of thousands of instances within sub-millisecond budgets.
* **Author Reference:**
  > *"The GPU pipeline needs to process a lot of steps and it can only process so many things. Do not bloat it with actions the CPU can handle instead."* `[00:07:31]`

---

</details>

## 2. Buffer Allocations, Clears & Memory Topology

### Результат проверки: корректное управление load/store полезно, «бесплатные clears» — нет

**В проекте:** dynamic rendering использует `VK_ATTACHMENT_LOAD_OP_CLEAR`; fullscreen-треугольники выполняют реальный post-process. Имеются кандидаты на удаление clear в полностью перезаписывающих final passes, но не доказательство, что любой fullscreen pass безопасен для `DONT_CARE`. См. [renderer.cpp](C:/GDF/vk_test/src/renderer.cpp:5896), [tonemap.frag](C:/GDF/vk_test/shaders/tonemap.frag:7), [composite.frag](C:/GDF/vk_test/shaders/composite.frag:12).

| Техника из списка | Вердикт и исправление |
| --- | --- |
| Hardware clear вместо отдельного draw только для очистки | **Рекомендовано; уже применяется.** Для Vulkan обычно использовать attachment `loadOp=CLEAR`. У fast clear есть ограничения по формату, значению, области и состоянию compression metadata. «Single-cycle» и «всегда быстрее» не являются API-гарантиями. [S01] |
| Пропуск clear перед полным overwrite | **Рекомендовано к имплементации после проверки coverage.** `DONT_CARE` допустим, если все читаемые далее pixels/samples/channels и используемые layers определены записями до чтения; учитывать renderArea, scissor, masks, blending и discard. Сам переход layout данные не инициализирует. [S04] |
| Не очищать G-buffer, потому что очищен depth | **Рекомендовано пропустить эту аргументацию.** Фон, отверстия alpha test и uncovered samples остаются без цветовых записей. Можно не очищать будущий G-buffer только если lighting и все последующие потребители действительно не читают неопределённые области, включая filter footprints. |
| Sparse-only clears для emissive, SSS, motion и coverage | **Рекомендовано как проверка инициализации, но не как список единственных разрешённых clears.** Accumulation с additive blending требует определённого начального значения; motion/background и history тоже требуют контракта. Direct diffuse освещает обычные поверхности, а не только сами источники; объединять его с emissive как заведомо sparse target неверно. Часть выходов можно полностью записывать producer-ом. |
| «Hijack clear»: прицепить auxiliary MRT и писать нули | **Рекомендовано пропустить как стандартную оптимизацию.** При неполном coverage он даже не очищает весь target. При полном — добавляет color exports/traffic; Vulkan требует совместимых attachment formats и pipeline state. «Ноль дополнительных state changes» не гарантирован. Рассматривать только измеренную fusion реальных вычислений. |

**Конкретный контрпример проекта:** [SMAA.h](C:/GDF/vk_test/shaders/third_party/smaa/SMAA.h:760) делает `discard` для pixels без edges. Поэтому clear `smaa_edges` обязателен в текущем алгоритме; удаление даст старые/неопределённые edges. Библиотека также явно требует инициализации рабочих targets. При пересмотре `smaa_weights` сначала проверить все paths, а не массово менять clears.

**Условие принятия:** по каждому target записать producer/consumer, покрытие, neutral value, load/store и barriers; сравнить изображения AA off/SMAA/FSR, пустую сцену, sky off и resize. Проверять frame time и bandwidth: корректное удаление clear ещё не означает измеримого ускорения.

<details>
<summary>Версия 1.0: непроверенный исходный материал — не требования к реализации</summary>

### 2.1 Render Target Clear Invariants
1. **Zero Fullscreen Quad/Triangle Clears:**
   * **Rule:** Never clear a render target using a rasterized fullscreen quad or triangle.
   * **Hardware Reality:** Modern GPU hardware utilizes tiled render cache flags (e.g., AMD DCC / NVIDIA Fast Clear) to clear memory surfaces in single-cycle operations without writing memory blocks. Emitting geometry bypasses hardware clear metadata and forces physical memory writes.
   * **Author Reference:**
     > *"A full screen triangle should never be used to clear a render target... using quads or triangles will always be slower than a hardware clear... you will destroy performance doing this."* `[00:10:45]`

2. **Full-Screen Overwrite Elimination (Skipped Clears):**
   * Do **not** issue hardware clear calls for full-coverage G-Buffer targets (`Albedo`, `Normals`, `Roughness/Metallic`) or Fullscreen Post-Process intermediate buffers.
   * Because the scene depth buffer is pre-cleared, opaque geometry is guaranteed to overwrite all valid on-screen fragments. Fullscreen post-processes (Tone Mapping, Bloom, AO) write $100\%$ of the render target surface area. Hardware clears on these targets are redundant bandwidth waste.

3. **Sparse-Only Clears:**
   * Only issue hardware `ClearRenderTargetView()` calls for render targets that contain partial or sparse geometry:
     * `Emissive / Direct Diffuse Accumulation` (`RGBA16_FLOAT`): only touched by light sources and glowing objects.
     * `Subsurface / Dual-Normal Buffer` (`RGB10A2_UNORM` / `RGBA16_FLOAT`): only touched by skin/foliage.
     * `MSAA Coverage / Motion Vector Overlays`.

4. **The "Hijack Clear" Pattern:**
   * When an active rendering pass requires an empty auxiliary buffer downstream, bind that auxiliary buffer as a secondary Multiple Render Target (MRT) in an existing draw pass that is already executing, writing the default clear value ($0.0$) via pixel shader export with zero additional pipeline state changes.

---

</details>

## 3. Depth Prepass Architecture: Non-Alpha Tested Partial Prepass

### Результат проверки: reverse-Z — хороший кандидат; partial prepass — по overdraw

**В проекте:** отдельного depth prepass нет. Используются обычная perspective `near=0.1, far=500`, `LESS`, depth clear 1; предпочтение D32 с fallback D24S8. См. [renderer.cpp](C:/GDF/vk_test/src/renderer.cpp:3459), [depth state](C:/GDF/vk_test/src/renderer.cpp:3985), [swapchain.cpp](C:/GDF/vk_test/src/swapchain.cpp:309). Это не reverse-Z.

| Техника из списка | Вердикт и сравнение |
| --- | --- |
| Partial prepass крупных ближних occluders | **Рекомендовано к имплементации как отключаемый вариант для сцен с дорогим shading и высоким overdraw.** Может уменьшить повторный PBR/GI shading; добавляет vertex processing, depth traffic и commands. Для текущей простой сцены выигрыш не установлен. Сравнить no/partial/full prepass. |
| Полный запрет alpha-tested geometry в prepass | **Рекомендовано пропустить.** Alpha mask задерживает depth writes, но не означает безусловное отключение всех early/hierarchical rejects. Дешёвый alpha-only prepass иногда позволяет дорогому base pass использовать `EQUAL` без повторного discard. AMD прямо описывает такой вариант. Проценты 13–40% из списка не переносим на проект. [S01], [S05] |
| Ноль texture reads в обоих shader stages | **Рекомендовано пропустить абсолютный запрет.** Position-only путь полезен, но prepass обязан воспроизвести deformation/displacement и coverage основного прохода. Vertex fetch heightmap не эквивалентен alpha-tested fragment shader; исключать необходимую выборку ради правила нельзя. |
| Исключать distant/micro geometry по <16 pixels | **Рекомендовано исключать по измеренному отношению occlusion/стоимость; фиксированный порог пропустить.** Tiny triangles действительно могут плохо использовать pixel quads, но площадь сама по себе не определяет пользу prepass. Depth-only путь без fragment shader не имеет той же стоимости helper lanes, что shading pass. |
| D32 floating-point reverse-Z | **Рекомендовано к имплементации, приоритет P1.** Это обоснованное улучшение точности по сравнению с текущим Z, особенно на дальней глубине; ускорение не обещается. NDC near→1/far→0, clear 0, согласованные `GREATER`/`GREATER_EQUAL`. 1 и 0 здесь — значения глубины, не расстояния до плоскостей. [S06] |
| D24S8 как равноценная замена D32 reverse-Z | **Рекомендовано пропустить утверждение о равноценной точности.** Главный выигрыш reverse-Z связан с float. При необходимости stencil проверить D32S8 и поддержку; D24S8 допустим как явно описанный fallback с иной точностью. [S06] |
| ≤0.30 ms при 1440p/4K | **Рекомендовано пропустить как обязательный budget.** Нет GPU, triangle count, coverage и профиля сцены; это не воспроизводимая цель. |

**Условие принятия reverse-Z:** одновременно обновить projection, sky depth, clears, CPU occlusion grid, compute comparisons и FSR depth-inverted/near/far контракт; сбросить temporal history. Сейчас [scene_visibility.cpp](C:/GDF/vk_test/src/scene_visibility.cpp:119) и [FSR dispatch](C:/GDF/vk_test/src/fsr3_upscaler.cpp:325) исходят из обычного Z. Shadow-map projection можно оставить отдельным контрактом. Проверять дальние близко расположенные поверхности, near-plane clipping и отсутствие false occlusion. [S07]

<details>
<summary>Версия 1.0: непроверенный исходный материал — не требования к реализации</summary>

### 3.1 Design Invariants
* **Total Frame Budget:** $\le 0.30 \text{ ms}$ at 1440p / 4K.
* **Texture Fetch Ban:** Zero texture sampling operations are permitted in the vertex or pixel stages of the Depth Prepass.

### 3.2 Exclusion Rules
1. **Strict Exclusion of Alpha-Tested Geometry (Foliage, Hair, Fences):**
   * Executing `clip()` / `discard` or evaluating alpha channel textures in the Depth Prepass inhibits GPU Early-Z coarse depth culling (e.g., Hierarchical-Z / Z-Cull) and introduces significant pipeline serialization.
   * Rendering alpha-tested foliage directly in the Base Pass with `Z-Write = ON` yields a $13\% \text{ to } 40\%$ net performance increase compared to including them in the Prepass.
2. **Exclusion of Distant and Micro-Geometry (LODs):**
   * Distant triangles ($< 16 \text{ pixels}$ screen-space area) suffer severe **Quad Overdraw** overhead ($2\times 2$ pixel quad lane waste where 3 out of 4 helper threads are discarded). Prepassing distant meshes consumes rasterization bandwidth without preventing overdraw downstream.
3. **Prepass Inclusion Criteria:**
   * Only large, occluding structural geometry (terrain, buildings, major environmental landmarks, close characters) situated near the camera should be rendered into the Depth Prepass.

### 3.3 Target Format & Depth State
* **Format:** `D32_FLOAT` (or `D24_UNORM_S8_UINT` if 8-bit stencil is co-allocated).
* **Projection:** Floating-point **Reverse-Z** (Near Plane = $1.0$, Far Plane = $0.0$) using `GREATER_EQUAL` depth comparison. Eliminates near-plane precision degradation and resolves distant Z-fighting.
* **Author Reference:**
  > *"Ensure your customers that no per-pixel texture sampling is allowed in the prepass, such as alpha testing or per-pixel displacement... Only large/close geometry should be prepassed with pure depth rendering."* `[00:26:16]`

---

</details>

## 4. G-Buffer Layout & Base Pass Execution Order

### Результат проверки: не переводить renderer в deferred ради фиксированного списка MRT

**В проекте:** lighting выполняется в geometry fragment shaders и пишет общий HDR color. Дополнительные motion и R32F depth outputs создаются для temporal AA; SMAA пишет один color target. Полноценного G-buffer нет. PBR уже использует GGX, correlated Smith, Schlick и metallic-roughness с dielectric F0=0.04: [pbr_lighting.glsl](C:/GDF/vk_test/shaders/pbr_lighting.glsl:140), [PbrMaterial](C:/GDF/vk_test/src/pbr_material.hpp:16).

| Техника из списка | Вердикт и обоснование |
| --- | --- |
| Строгий порядок: close alpha → remaining opaque → distant alpha → prepassed opaque | **Рекомендовано пропустить как универсальный порядок.** Ранний alpha может закрыть дорогой opaque, но может и напрасно выполнить alpha tests перед более ближним opaque. На procedural foliage fixture сравнить это расписание с opaque-first и batching + front-to-back. Дальность alpha сама по себе не определяет выгодный порядок. |
| Front-to-back для непрепассированной opaque geometry | **Рекомендовано к имплементации при высоком overdraw**, сохраняя batching. Внутри bucket по pipeline/material допускается грубая depth sorting; стоимость CPU сортировки и ухудшение batching входят в A/B. |
| `EQUAL`, depth writes off для prepassed geometry | **Рекомендовано к имплементации вместе с prepass.** Нужны совпадающие transformations, jitter, rasterization, displacement и coverage. «Zero overdraw» не абсолютно: совпадающие поверхности и дублированная геометрия тоже могут пройти equality. [S05] |
| Фиксированные 4 MRT из §4.2 | **Рекомендовано пропустить как замену текущего forward.** Схема хранит 16 bytes/pixel цвета плюс 4 depth: при 1080p это ≈39.6 MiB, при 4K ≈158.2 MiB на комплект без MSAA/служебных данных; две дополнительные RGBA16F accumulation поверхности — ещё 16 bytes/pixel (≈31.6/126.6 MiB соответственно). Это собственный расчёт, не измерение traffic. Deferred выбирать по реальной задаче многих lights/materials. [S08] |
| Packed normals, flags и material IDs | **Рекомендовано к имплементации для buffers с определённым потребителем.** Контракт IDs/masks развивается вместе с SSS/material variants; полный G-buffer остаётся отдельным архитектурным решением. Octahedral normal обычно кодируется двумя компонентами; запись «Octahedral/World Normal RGB» не задаёт decoder. Два alpha bits дают только четыре значения. Material IDs нельзя линейно фильтровать/усреднять при MSAA resolve; UNORM ID требует точного encode/decode. Выбрать sRGB/linear для albedo, precision и packing по ошибке, не по таблице. |
| Dual Material Profile вместо scalar metallic | **Рекомендовано пропустить обязательную замену.** Metallic между 0 и 1 пригоден для пространственных смесей/фильтрации; это не утверждение, что существует «полуметалл». Два IDs и weight не описывают полноценные layered материалы и не помещаются автоматически в прежний metallic byte. Для ржавчины/краски сначала нужен явный material blending/layering контракт. [S09] |
| IOR-driven specular | **Рекомендовано к имплементации при расширении PBR.** Для dielectric использовать F0=((ior−1)/(ior+1))²; default ior=1.5 сохраняет нынешние 0.04. Цветной conductor F0 и слоистые материалы требуют отдельной модели. Таблица IOR и произвольный multiplier сами по себе energy conservation не гарантируют. [S10] |
| 2×2 Bayer LOD cross-fade | **Рекомендовано и реализовано как опциональное улучшение authored-model LOD transitions; фиксированный 2×2 рекомендовано пропустить.** CPU/GPU Bayer4/8 используют complementary color/depth coverage, stable source token и bounded history. На clean RX 6800 A/B для 1024 объектов цена составила +0.0038…0.0059 ms GPU frame и −0.4…−3.8% FPS; на одном объекте GPU-разница почти неразличима. Устойчивого победителя Bayer4/8 нет, default остаётся `off` до content review. |
| Согласовать dither в color и shadow passes | **Рекомендовано пропустить буквальное копирование camera-space dither в shadow pass.** Одинаковый `gl_FragCoord` в camera и light space создаёт разные holes. Реализованный reference всегда рисует LOD0 в shadow maps и потому не даёт camera-LOD shadow pop ценой дополнительной геометрии. Отдельный shadow-LOD transition имеет смысл исследовать только вместе с реальным shadow LOD контентом, общим object/phase contract и light-space quality A/B. |

**Условие принятия:** сохранять forward baseline; для PBR — material spheres и reference lighting, для packing — normal/ID ошибки, для LOD — видео переходов с AA off/SMAA/FSR и тенями. Наличие production-применения техники не доказывает выигрыш на нашей сцене.

<details>
<summary>Версия 1.0: непроверенный исходный материал — не требования к реализации</summary>

### 4.1 Optimal Draw Call Ordering (Mitigating Non-Prepassed Overdraw)
To maximize GPU Early-Z efficiency after a partial depth prepass, execute the G-Buffer base pass in this strict order:
1. **Close-Range Alpha-Tested Meshes (Foliage, Hair):** Render with active Depth Write enabled (`Z-Write = ON`). Writes early depth for complex overlapping leaf layers.
2. **Non-Prepassed Medium/Distant Opaque Geometry:** Typical props and environment objects sorted front-to-back.
3. **Distant Alpha-Tested Meshes:** Rendered against the nearly-complete scene depth buffer to cull occluded distant foliage.
4. **Prepassed Large Geometry:** Rendered with Depth Test set to `EQUAL` and `Z-Write = OFF`. Pixel shaders run only for the exact visible fragments; zero overdraw occurs.

### 4.2 G-Buffer Render Target Memory Layout

| MRT Slot | Target Name | Format | Channel Layout (R / G / B / A) |
| :--- | :--- | :--- | :--- |
| **MRT 0** | Albedo / Material ID | `RGBA8_UNORM` | Base Color (RGB), Material ID / Stencil Selector (A) |
| **MRT 1** | World Normal / Flags | `RGB10A2_UNORM` | Octahedral/World Normal (RGB), Anisotropy / Shading Model Bits (A) |
| **MRT 2** | Surface Properties | `RGBA8_UNORM` | Roughness (R), Dual Profile Metallic (G), Specular / IOR (B), Cavity (A) |
| **MRT 3** | Motion Vectors | `RG16_FLOAT` | Screen-Space Motion Vector $\Delta X, \Delta Y$ |
| **Depth** | Primary Scene Depth | `D32_FLOAT` | Reverse-Z Scene Depth |

### 4.3 PBR Model Refactoring
* **Dual Material Profile (DMP) vs. Scalar Metallic:**
  * Standard scalar metallic ($[0.0 \dots 1.0]$) incorrectly interpolates intermediate physical states (e.g., oxidized/chipped paint over raw iron).
  * DMP passes two discrete `Material Profile IDs` into the G-Buffer. The single byte previously allocated to metallic acts as a normalized blend weight between two physically valid Bidirectional Reflectance Distribution Function (BRDF) profiles.
* **Index of Refraction (IOR) Driven Specular:**
  * Store a calibrated scalar multiplier against an intrinsic physical IOR table indexed by Material ID, ensuring micro-fresnel reflections remain energy-conserving.
* **GPU-Driven 2x2 Bayer Dither for LOD Cross-Fading:**
  * Discard geometry transitions using a branch-free $2\times 2$ Bayer matrix ALU calculation inside the pixel shader.
  * Synchronize identical Bayer dither equations into shadow map rendering passes to eliminate cross-fade shadow pops.

---

</details>

## 5. MSAA Integration via `SV_Coverage`

### Результат проверки: MSAA полезен как отдельный режим; deferred-схема преждевременна

**В проекте:** после исследования добавлены отдельные forward `msaa2x`/`msaa4x` с multisampled HDR color/depth и hardware average color resolve. Multisampled G-buffer и sample classification по-прежнему отсутствуют, поэтому deferred-схема из видео не описывает реализованный путь. [S11], [S29], [S30]

| Техника из списка | Вердикт и обоснование |
| --- | --- |
| «Forward MSAA заставляет shading всех каналов на каждом sample» | **Рекомендовано пропустить: утверждение неверно.** Coverage/depth sampling и shader invocation frequency различны; Vulkan отдельно задаёт sample shading. Без принудительного per-sample shading один результат может обслуживать несколько covered samples. [S11] |
| `SV_Coverage` / `gl_SampleMaskIn` и stencil interior/edge classification | **Рекомендовано к имплементации только если позже выбран deferred+MSAA.** Это разумный способ ограничить sample-frequency lighting, но coverage mask не заменяет multisample storage и полный тест shading discontinuities. Нельзя после обычного single-sample G-buffer восстановить потерянные subpixel surfaces. |
| Vertex edge/flat-normal attributes для исключения coplanar interior edges | **Рекомендовано пропустить как обязательную базу.** Coplanar triangles могут иметь разные материалы/normal maps; одного geometric normal недостаточно для выбора одной lighting evaluation. Дополнительные attributes и split vertices тоже стоят bandwidth. Сначала сравнить классификацию по multisample depth/normal/material. |
| Отказ от decal staging и прямая запись в G-buffer | **Рекомендовано к имплементации только при выбранном deferred path; для текущего forward эту схему пропустить.** Прямая модификация G-buffer применяется в движках, но D-buffer решает другие задачи. Нужно определить normal blending, masks сохранения IDs, порядок пересечений и read/write hazards; «без дополнительных resolve/barriers» не гарантировано. [S12] |

**Более подходящий путь реализован и рекомендован как опция:** обычный forward MSAA 2×/4× работает без temporal history и без forced sample shading. Exact format/sample support проверяется runtime; color разрешается через `AVERAGE`, depth остаётся multisampled и не resolve, пока нет single-sample depth consumer. На `aa-wires` MSAA4 дал лучший edge MAE (`0.03650` против SMAA `0.05320`), MSAA2 (`0.05904`) оказался хуже SMAA, но не менял полностью покрытые interiors. Цена 1080p на RX 6800: 94.9/189.8 MiB и +0.0922/+0.0961 ms GPU на лёгкой сцене; при `overdraw-4` +0.1334/+0.1546 ms. Поэтому MSAA4 — рекомендуемый quality mode, MSAA2 — memory-saving hardware option, Off остаётся default, а SMAA — более дешёвой spatial альтернативой. Shader/specular aliasing MSAA полностью не решает.

<details>
<summary>Версия 1.0: непроверенный исходный материал — не требования к реализации</summary>

### 5.1 Deferred MSAA Pipeline
* **Limitation:** Forward MSAA forces multi-rate shading across all pixel channels.
* **Optimization:**
  * Sample sub-pixel geometry using native DirectX/Vulkan hardware coverage masks (`SV_Coverage` in HLSL / `gl_SampleMaskIn` in GLSL).
  * Store edge discontinuities and flat triangle normal interfaces into vertex attributes. Prevent unnecessary MSAA multi-rate operations on coplanar interior edges.
  * Generate a **Sample Frequency Stencil Mask**:
    * Stencil Value $0$: Pixel Frequency (Interior fragments; 1 lighting evaluation per pixel).
    * Stencil Value $1$: Sub-Sample Frequency (Geometric silhouette edges; multi-rate MSAA evaluation).
* **Decal Subsystem:**
  * Eliminate dedicated Decal Staging targets. Render projected mesh/box decals directly into G-Buffer MRTs, maintaining existing Shading Model IDs without additional Resolve barriers.

---

</details>

## 6. Shadow Pipeline: Perspective Shadow Maps (PSM) & Approximations

### Результат проверки: оставить stable CSM; улучшать кеширование и качество по измерениям

**В проекте:** четыре orthographic cascades с texel snapping и blend bands; default atlas 2048², доступны 1024/2048/4096/8192. Все cascades используют общий atlas; обновление определяется dirty state, а не слепым запуском каждый кадр. Tent PCF [1,2,1] уже реализован через четыре bilinear comparison samples. См. [shadow_cascades](C:/GDF/vk_test/src/lighting.cpp:128), [cache](C:/GDF/vk_test/src/renderer_lighting.cpp:338), [filter](C:/GDF/vk_test/shaders/lighting_common.glsl:31).

| Техника из списка | Вердикт и сравнение |
| --- | --- |
| PSM perspective warping вместо CSM | **Рекомендовано пропустить как замену.** PSM перераспределяет texels, но качество зависит от взаимного направления света/камеры, включает сложные случаи casters за камерой и spatially varying bias. Это документированные проблемы метода; текущие stable CSM лучше соответствуют свободной fly camera и суткам. Скоростное превосходство CSM здесь не измерялось. [S13], [S14] |
| Warping по camera velocity/focal center | **Рекомендовано пропустить.** Это не полное определение классического PSM. Нужны конкретная формула, conservative caster coverage и доказанная temporal stability; движение проекции по velocity может само усиливать shimmer. |
| Жёсткий cap atlas 2048² | **Рекомендовано пропустить.** Default уже 2048, но это лишь 1024² на cascade. В большом frustum разрешения может не хватать. Сохранить текущие quality options и измерять память/время/качество, а не удалять 4096/8192. [S14] |
| D16 вместо D32 | **Рекомендовано к имплементации как необязательный профиль после A/B.** D16 может сократить footprint относительно D32, но надо проверить precision, bias, sampled-image comparison/filter support и cascade depth ranges. Не менять format вслепую. |
| Маленький PCF и аккуратный bias | **Рекомендовано; уже реализовано.** Сначала улучшать coverage/texel allocation, затем kernel. Малый PCF — дешёвое сглаживание, но не физический contact hardening. Для PCSS нужны blocker search и penumbra estimation, поэтому «PCF ≤3×3 = contact hardening» пропустить. [S15] |
| Near cascades каждый кадр, far через кадр | **Рекомендовано к имплементации адаптивное кеширование отдельных cascades; фиксированное чередование пропустить.** Учитывать invalidation по camera/caster/light, возраст в миллисекундах и coverage margin; при резком движении обновлять немедленно. Не обновлять неподвижные near cascades просто по номеру. |
| Дальние тени заменить screen-space contact shadows после 100 m | **Рекомендовано пропустить как полную замену.** Depth содержит только видимые поверхности; offscreen/скрытый caster исчезает из решения. Contact shadows — возможное дополнение для локальных деталей, не гарантия дальней видимости. «100 m» не универсальная граница. [S16] |
| SDF shadows для дальних объектов | **Рекомендовано к имплементации как гибрид near CSM/far SDF вместе с SDF asset pipeline.** Пререквизиты движка: cook signed distances для замкнутого triangle mesh, формат/storage/streaming, bounds/transforms, invalidation и fallback для неподдерживаемой geometry. Проверять на процедурных закрытых box/sphere/mesh, в том числе с casters за камерой, и синтетической большой сцене. Сейчас GI voxel occupancy/radiance не является signed distance field; thin/open/deformed meshes требуют явной политики, а не выдачи occupancy за SDF. [S17] |

**Критичное условие cascade caching:** текущий [shadow pass](C:/GDF/vk_test/src/renderer.cpp:3113) очищает atlas и рисует все cascades. Для частичного обновления надо сохранять untouched tiles, очищать только dirty regions и хранить матрицу, соответствующую каждой кешированной tile. Нельзя поменять матрицу дальнего каскада, оставив его старую карту. Сравнить выгоду tile clears с потерей full-surface fast clear.

**Условие принятия:** A/B на статичной/движущейся камере, низком солнце, moving casters и границах cascades; проверять shimmer, acne, peter-panning и offscreen shadows, а не только FPS.

<details>
<summary>Версия 1.0: непроверенный исходный материал — не требования к реализации</summary>

### 6.1 Constraints & Layout
* **Atlas Resolution Cap:** Maximum dimension $\le 2048 \times 2048$ pixels (`D16_UNORM` or `D32_FLOAT`).
* **Perspective Warping (PSM):**
  * Modulate shadow projection matrices dynamically according to the camera frustum angle and camera velocity vector.
  * Direct shadow texel density into the screen-space region closest to the camera focal center.

### 6.2 Filtering & Distance Strategy
* **Minimal Softening (Contact Hardening):**
  * Avoid heavy percentage-closer filtering (PCF) radii ($> 3\times 3$ taps) or Gaussian blurs that decouple shadows from contact points. Maintain narrow filter kernels with tight depth bias offsets.
* **Cascade Scheduling:**
  * Cascades 0 and 1: Update every frame.
  * Cascades 2 and 3: Time-slice across alternating frames.
  * Distant Cascades ($> 100 \text{ m}$): Replace shadow map cascades entirely with screen-space directional contact shadows or Signed Distance Field (SDF) ray approximations.

---

</details>

## 7. Lighting Pipeline: Stencil Volume Deferred Lighting

### Результат проверки: stencil volumes — нишевой кандидат; clustered lighting не исключать

**В проекте:** directional sun + sky/environment + voxel diffuse GI. Системы множества point/spot/area lights, deferred accumulation и stencil light volumes не обнаружены в текущем render path. Солнечный BRDF вычисляется непосредственно в [pbr_lighting.glsl](C:/GDF/vk_test/shaders/pbr_lighting.glsl:113); отдельный G-buffer ради одного sun добавит инфраструктуру.

| Техника из списка | Вердикт и обоснование |
| --- | --- |
| Отказ от tiled/clustered из-за heterogeneous light types | **Рекомендовано пропустить.** Divergence/register pressure надо измерять; lights можно группировать по типам и выбирать shader variants. Clustered forward — доказанно работоспособное семейство для многих lights, transparency и MSAA; преимущества зависят от сцены. [S08] |
| Stencil light volumes + отдельный lighting draw на volume | **Рекомендовано пропустить как основную будущую архитектуру проекта.** Оставить сравнительный prototype для небольшого числа крупных/дорогих локальных lights, если будет deferred. Перекрывающиеся volumes увеличивают raster passes, additive traffic и state changes; sun не получает пользы от объёмной локализации. [S18] |
| Запись Light ID в stencil на depth-fail/pass boundary | **Рекомендовано пропустить псевдокод как готовый алгоритм.** В 8-bit stencil нельзя хранить произвольное множество перекрывающихся IDs. Нужен корректный mask на текущий light/группу с reset, front/back-face ops, camera-inside и near-plane handling. Одного `GREATER_EQUAL` недостаточно. [S05] |
| Отдельные RGBA16F diffuse/specular accumulation | **Рекомендовано к имплементации только для конкретного потребителя**, например SSS. Иначе общий HDR color дешевле по числу targets. Разделение не нужно ни для IOR, ни для корректного auto-exposure само по себе. |
| Независимые passes/variants для Skin/PBR/Hair/Foliage | **Рекомендовано к имплементации через явный shading-model contract и специализацию по поддержанным моделям.** Подготовить procedural material fixtures; skin/eye варианты развиваются с соответствующими подсистемами, новые hair/foliage BRDF требуют своего определения. Специализация может убрать неподходящие инструкции и снизить register pressure. Само отсутствие bind LUT не доказывает уменьшение GPR: решает исполняемый shader. Не вводить несколько fullscreen passes для нынешнего одного PBR. [S01] |

**Рекомендовано; реализовано и проверено:** bounded KHR-style point/spot lights до 4096 источников и clustered forward. Реализация использует linear RGB, candela, finite range, local `-Z` spot direction, smooth distance cutoff, squared cone falloff и per-frame SSBO; тот же контракт доступен через scene JSON, editor и типизированный MCP. Cluster grid `32x18x24` хранит до 64 индексов на cluster, строится light-centric dispatch-ем с одной 64-thread workgroup на light и публикует `gpu_light_culling_ms`/statistics. Overflow losslessly переходит на full-forward для этого cluster, поэтому `dropped=0`.

Forward reference и fixture `light-swarm` 1/16/64/256/4096 в separated/overlap layouts прошли Windows Debug/Release/ASan gates, shader validation и pixel-exact forward/clustered comparison для 256 lights. Fixed-clock RX 6800 A/B показывает crossover, зависящий от распределения: separated clustered быстрее от 16 lights и даёт -44.89% GPU frame при 64, -77.63% при 256; overlap дороже до 64 и почти равен reference при overflow на 256/4096. Поэтому `auto` консервативно выбирает clustered от 64 lights, сохраняя forward reference/fallback. E10 принят как engine subsystem без production assets. Local lights пока unshadowed и не инжектируются в GI; это отдельные будущие возможности, а не основание отклонять реализованный direct-lighting path.

<details>
<summary>Версия 1.0: непроверенный исходный материал — не требования к реализации</summary>

### 7.1 Rejection of Global Compute Grid
* Standard Tiled/Clustered compute shaders suffer from branch divergence and maximum-register occupancy degradation when local scenes contain heterogenous light types (Point, Spot, Capsule, Area) with varying attenuation profiles.

### 7.2 Stencil Volume Shading Flow
```
[G-Buffer Complete]
        │
        ▼
[Pass 1: Rasterize Light Volume Convex Geometry]
  ├─ Depth Test: GREATER_EQUAL (Reverse-Z)
  ├─ Depth Write: OFF
  └─ Stencil Operation: Write Light ID on Depth-Fail/Pass Boundary
        │
        ▼
[Pass 2: Fullscreen/Volume Lighting Evaluation]
  ├─ Stencil Test: EQUAL to Light ID
  ├─ Shading Model Filter: Masked to current material profile
  ├─ Blend Mode: Dual-Target Additive
  └─ Targets:
       ├─ MRT 0: Diffuse Accumulation (RGBA16_FLOAT)
       └─ MRT 1: Specular Accumulation (RGBA16_FLOAT)
```

### 7.3 Multi-Material Shading Decomposition
* Execute independent lighting passes partitioned by Shading Model (Skin, Standard PBR, Anisotropic Hair, Translucent Foliage).
* Standard PBR passes do not bind high-bandwidth look-up tables (LUTs) or dual-normal textures, minimizing active GPR (General Purpose Register) register pressure and maximizing GPU wavefront occupancy.

---

</details>

## 8. Subsurface Scattering (SSS) & Eye Rendering

### Результат проверки: masked separable SSS рекомендован и реализован; специализированные глаза отложены до контракта контента

**Текущий статус:** в renderer добавлен непрозрачный `subsurface` material model с
профилями `narrow` / `medium` / `wide`, силой `[0,1]` и full-resolution
depth/profile-aware separable фильтром. Опорные profile distances заданы в
единицах сцены и не объявляются физически откалиброванными параметрами кожи.
Dedicated eye renderer пока не добавлен: корректная реализация зависит от
целевой геометрии глаза, normals, UV/layout iris и правил композиции cornea/tear
слоёв. Добавление пустого `MaterialModel::eye` до этого контракта только закрепило
бы недоказанный API. Требования eye shader к согласованным mesh/UV данным также
видны в production-документации [Unreal Engine Eye Shading Model](https://dev.epicgames.com/documentation/unreal-engine/shading-models-in-unreal-engine)
и [Digital Humans](https://dev.epicgames.com/documentation/unreal-engine/digital-humans?application_version=4.27).

| Техника из списка | Вердикт и обоснование |
| --- | --- |
| Full-resolution SSS только на skin pixels | **Рекомендовано; реализовано как optional engine path.** Diffuse и specular разделяются в geometry pass: только доля diffuse, заданная `subsurface_strength`, проходит два full-resolution 7-tap H/V прохода. `RGBA16F` alpha хранит точно представимый `profile * 256 + strengthUNorm8`; point reads сохраняют дискретную границу profile без stencil. Отдельный `R32F` depth ограничивает перенос между поверхностями. Сейчас оба fullscreen passes запускают fragment на всём extent, а `profile == none` быстро выходит; маска обеспечивает корректность фильтрации, но не ограничивает raster coverage. Stencil/tile compaction имеет смысл добавлять только после измерения реального skin coverage. Это практический reference baseline, а не заявление о точном воспроизведении финального kernel Jimenez. [S19] |
| Абсолютный запрет half-resolution SSS | **Рекомендовано пропустить.** Качество зависит от projected skin area, reconstruction, depth/profile rejection и бюджета. Проверять губы, веки и силуэт в full-res/reference; half-res оставить quality option для дальних/малых персонажей. |
| Только 2 samples с Bayer phase, жёстко привязанной к 60 FPS | **Рекомендовано пропустить.** Это не валидированная модель diffusion profile; два прохода separable SSS не означают две texture samples. Публикация Jimenez использует две 1D convolutions и описывает importance sampling/jitter, а не данное универсальное правило. 60 FPS phase без reprojection/adaptive timing не гарантирует стабильности на 30/144 FPS. [S19] |
| Писать SSS прямо в тот же Diffuse Accumulation | **Рекомендовано пропустить небезопасную in-place трактовку; реализовано безопасное разделение.** Geometry пишет scattered diffuse и unscattered HDR base отдельно; horizontal pass пишет в независимый ping target, vertical pass читает его и композитит в scene color. Barrier не используется как замена независимому read/write storage. |
| Dedicated forward eyes, iris refraction, sclera scattering, tear meniscus/wetness | **Рекомендовано отложить до появления целевой eye geometry/UV/content contract.** Сейчас специализированный material enum и procedural approximation не добавляются. После определения mesh/UV контрактов отдельно проверить непрозрачную sclera/iris основу; прозрачная cornea shell, tear/wetness, refraction и calibrated sclera scattering требуют отдельного compositing и quality решения. [S20] |
| Pupil dilation от средней яркости | **Рекомендовано отложить вместе с eye content.** Будущий управляемый художественный параметр может использовать сглаженный exposure/light signal с limits и manual override, но средняя яркость кадра не равна retinal illuminance: pupil response зависит также от поля и stimulus. [Watson and Yellott, 2012](https://doi.org/10.1167/12.10.12) |
| Eye budget <0.02 ms и SSS budget из графа | **Рекомендовано пропустить как обещание.** Стоимость зависит от screen coverage, числа персонажей, shader и GPU. |

**Реализованный контракт и границы:** `--subsurface off|full`, editor control и
типизированный MCP `render.set_subsurface` управляют режимом; default `off` не
создаёт SSS targets и не запускает passes. Первый кадр `full` лениво создаёт два
дополнительных `RGBA16F` target на каждый frame slot; при 1920×1080 и двух кадрах
это 66 355 200 bytes (63.28125 MiB). После выключения targets остаются resident
до extent recreation/shutdown, но GPU SSS passes прекращаются. Scene и cooked
material JSON сохраняют model/profile/strength; старые документы получают
`standard` + `none` + `0`, а importer явно сохраняет `none`/`0` и не выводит SSS
из ассета эвристикой. Процедурный `subsurface-profiles` fixture содержит 28 сфер,
из них 20 активных SSS материалов, три profile и strength `1`/`0.5`, glossy row,
standard/unlit controls и variation по дистанции. Финальный чистый Release A/B
после исправления общего 24-pixel kernel-radius cap на RX 6800, 1920×1080
IMMEDIATE дал для `full` +0.06626 ms GPU frame, −9.50% FPS и 0.09864 ms SSS
interval; resident storage после provisioning — 63.28125 MiB.
Windows builds/CTest, synchronization validation, ASan, MCP и ограниченная
visual fixture-проверка пройдены. Eye acceptance fixture появится только вместе
с определённым eye content contract.

<details>
<summary>Версия 1.0: непроверенный исходный материал — не требования к реализации</summary>

### 8.1 Screen-Space SSS Rules
* **Avoid Half-Resolution Buffers:** Downscaled SSS passes cause edge-bleeding and high-frequency blurring around character features (lips, eyelids).
* **Implementation:**
  * Execute a full-resolution pixel shader pass masked exclusively to pixels tagged with the `Skin Shading Model ID` using the Stencil Buffer.
  * Apply a 2-sample jittered kernel utilizing a $2\times 2$ Bayer sequence locked to a 60 FPS temporal phase to suppress color banding without wide blurs.
  * Write directly into the `Diffuse Accumulation` (`RGBA16_FLOAT`) buffer.

### 8.2 Dedicated Eye Shading
* Forward-rendered overlay directly following the base pass ($< 0.02 \text{ ms}$ budget).
* Required features:
  * Pupil dilation tied to average scene luminance.
  * Geometric tear duct wetness lines (`Normal + Specular` overlay).
  * Inner iris refraction parallax and localized sclera scattering.

---

</details>

## 9. Volumetrics, Particles & Transparency

### Результат проверки: half-res particles — рекомендованная подсистема движка, min/max недостаточно для идеальных краёв

**В проекте:** нет отдельной системы alpha-blended particles, transparency accumulation, froxel volumetrics и reactive-mask producer. Наличие voxel GI не означает, что атмосферный scattering или прозрачность уже реализованы.

| Техника из списка | Вердикт и обоснование |
| --- | --- |
| Half-resolution дым/туман/крупные мягкие particles | **Рекомендовано к имплементации вместе с bounded emitter/particle data, HDR transparency и процедурным smoke/fire/spark fixture.** Снижение fill-rate разумно для низкочастотных эффектов. Мелкие искры, резкий огонь и читаемые силуэты могут требовать full-res. Нужны depth-aware upsampling, premultiplied-alpha composition и понятный порядок прозрачных слоёв. Full-res reference и half-res путь разрабатываются до production effects. [S21] |
| 2×2 min/max depth в RG32F | **Рекомендовано к имплементации как вспомогательный conservative тест.** Проверять перспективную нелинейность и направление Z; в reverse-Z числовой min — дальняя граница, max — ближняя. Хранить обе границы полезно для распознавания смешанных blocks, но формат и reuse общего depth pyramid выбирать по реальным потребителям. |
| Одна branch по min/max устраняет clipping и depth overdraw | **Рекомендовано пропустить это обещание.** Два экстремума не восстанавливают пространственное расположение четырёх samples. Для смешанного block нужны full-res depth tests/reconstruction либо full-res fallback. Overdraw между прозрачными particles этот тест не устраняет. [S21] |
| Responsive alpha=1 для быстрых/ярких particles | **Рекомендовано к имплементации reactive mask по контракту temporal upscaler.** Это не просто velocity/mobility flag: маска задаёт доверие к реконструкции прозрачности. Фиксированное значение 1 для всех таких pixels рекомендовано пропустить; значение надо подбирать по SDK и качеству. Не перезаписывать opacity/transmittance в accumulation alpha, если она нужна композиции. Передавать выделенный ресурс/правильно извлечённую маску в FSR; alpha сама по себе SDK не подключается. [S07] |
| Применить ту же схему ко всем volumetrics | **Рекомендовано пропустить обобщение.** Здесь описаны offscreen particles, а не volume density, интегрирование scattering/transmittance, lighting и temporal reconstruction объёмного тумана. Для volumetrics потребуется отдельный дизайн. |

**Условие принятия:** сравнить full/half-res на тонких препятствиях, силуэтах, пересечениях и rapid motion; проверять ореолы и ghosting во всех AA режимах. Стоимость depth reduction + upscale + mask + composite входит в сравнение.

<details>
<summary>Версия 1.0: непроверенный исходный материал — не требования к реализации</summary>

### 9.1 Min-Max Depth Low-Resolution Rendering
1. **Depth Downsampling:** Downsample primary scene depth to half-resolution using a $2\times 2$ min-max reduction filter, writing minimum depth to Channel R and maximum depth to Channel G (`RG32_FLOAT`).
2. **Particle Evaluation:**
   * Evaluate unlit and semi-transparent alpha-blended particles (smoke, fog, fire) at half-resolution.
   * Compare fragment depth against both Min and Max depth bounds in a single ALU branch. Eliminates edge intersection clipping and suppresses depth overdraw for fully occluded particle quads.
3. **Responsive Alpha Mask Tagging:**
   * High-velocity or bright transparent particles write a mobility value ($1.0$) to the Alpha channel of the accumulation target to signal downstream temporal anti-aliasing (TAA) to drop historical weight.

---

</details>

## 10. Tone Mapping & Exposure Control

### Результат проверки: улучшить display mapping можно; diffuse-only exposure отвергнуть

**В проекте после E13:** fixed/auto exposure и GT7/Reinhard объединены с одним display transform и одним hardware sRGB encode. Auto mode строит 256-bin log-luminance histogram общего HDR scene color до AA, использует percentile rejection, uniform/center metering и EV-domain temporal adaptation. Compute записывает одно exposure одновременно в state buffer для финального tone mapper и external `1x1 R32F` image для FSR при `preExposure=1`. Identity `RGBA16F` LUT 32³/64³ поддерживает trilinear/tetrahedral sampling после tone mapping. Fixed exposure и grading off остаются default. См. [display_mapping.glsl](C:/GDF/vk_test/shaders/display_mapping.glsl:3) и результаты в `docs/RENDERING_PIPELINE_RESULTS.md`.

| Техника из списка | Вердикт и сравнение |
| --- | --- |
| GT-style toe/linear midtones/shoulder | **Рекомендовано к имплементации как переключаемый quality prototype, P1.** У Reinhard нет управляемого линейного среднего участка; GT-оператор может дать более подходящий contrast/highlight roll-off. Это кандидат на визуальное улучшение, не доказанное ускорение. Нужны фиксированные exposure, display target и тестовые цвета. |
| Назвать произвольную кривую «GT7 formula» и гарантировать white/pale-yellow highlights | **Рекомендовано пропустить.** В исходнике вообще нет формулы. Официальный GT7 reference 2025 использует не только scalar curve, но и управление chroma в perceptual space и определённый luminance/display contract. Нельзя приравнивать его к любой ранней Uchimura-кривой. Нужный hue не задаётся общей фразой про shoulder. [S22] |
| «ACES всегда даёт cyan-burn», «crushed shadows физически правильны» | **Рекомендовано пропустить.** Не указаны версия ACES, input gamut, exposure и display transform. Ни потеря деталей в тенях, ни конкретный художественный contrast не являются универсальной физической нормой. Сравнивать полные colour pipelines на одном линейном HDR входе. |
| Auto-exposure исключительно от Diffuse Accumulation; specular запрещён | **Рекомендовано пропустить; альтернативный общий HDR histogram реализован и проверен.** Он использует percentile rejection, weighting и temporal adaptation. Specular-only металлическая сцена, небо и яркие видимые источники учитываются общим видимым radiance; исключение specular целиком остаётся возможным художественным режимом, а не правилом корректности. [S23] |
| Не измерять exposure по raw albedo | **Рекомендовано.** Albedo — отражательная характеристика, не освещённость/выходная radiance. Однако diffuse radiance всё равно зависит от albedo, поэтому предложенная схема не избавляет metering от тёмных материалов. |
| LUT 64³ RGBA16F + tetrahedral interpolation | **Grading LUT реализован и рекомендован как опция; обязательные 64³/tetrahedral по-прежнему пропустить.** Identity/reference 32³/64³ `RGBA16F`, trilinear/tetrahedral и linear-Rec.709 ordering прошли numeric/visual gates. На RX 6800 при Full HD trilinear добавил 0.0044–0.0048 ms, tetrahedral 0.0117–0.0126 ms к display draw; 64³ не дал измеримого quality/cost основания для default. Начальный режим — 32³ trilinear после появления художественного LUT. [S24] |

**Рекомендуемая система exposure движка реализована:** log-luminance histogram по общей HDR сцене до display mapping, ограничение выбросов, uniform/center metering, раздельные скорости адаптации и manual override. Fixed exposure сохранён для сопоставимых benchmark. Frame-rate response, invalid/black input и histogram math проверены CPU/GPU references; художественная тёмная комната с ярким выходом остаётся content validation после появления подходящей сцены. Pre-exposure не введён: scene colour и history остаются в исходном HDR domain, FSR и display используют одно GPU-generated значение через согласованные image/buffer представления.

**GT7/LUT engine contract принят:** reference test vectors и numeric 3D LUT cases пройдены, порядок linear HDR → exposure → GT7/Reinhard → LUT → один sRGB encode закреплён, отдельный GPU display timing измерен. Skin/metal/neon и production look остаются художественной проверкой контента; они не блокируют готовность identity/interpolation API.

<details>
<summary>Версия 1.0: непроверенный исходный материал — не требования к реализации</summary>

### 10.1 GT / Gran Turismo 7 Curve Formula
The tone mapper must preserve linear midtones while implementing an aggressive human-eye-adapted toe and shoulder:

```
                  ┌────────────────────── [Smooth Shoulder: Saturation preserved]
                  │
                 /
                / ◄────────────────────── [Linear Midtones: Physically correct contrast]
               /
             _/ ◄──────────────────────── [Weber-Fechner Contrast Toe: Crushed shadow detail]
```

* **Color Shifts:** High-intensity specular reflections shift towards white/pale yellow rather than clipping directly into clamped single-channel extremes (avoiding typical ACES cyan-burn and highlight distortion).
* **Exposure Calculation:**
  * Downsample the **Diffuse Lighting Accumulation** target exclusively to compute scene average luminance.
  * **Rule:** Never include the Specular target or raw Albedo in the exposure metering loop. Specular highlights and dark base textures bias auto-exposure incorrectly.
* **Color Grading LUT:** $64\times 64\times 64$ `RGBA16_FLOAT` volumetric texture sampled using tetrahedral interpolation.

---

</details>

## 11. Anti-Aliasing: Two-Frame SMAA 4X (S2X + T2X)

### Результат проверки: optional SMAA T2x принят; S2x/4x и альтернативные detectors сейчас пропустить

**В проекте:** SMAA 1x остаётся default SMAA mode и использует reference
`SMAA.h`, color edge detection, search/area textures и presets. Дополнительно
реализован optional native-resolution SMAA T2x: фазы jitter
`±(0.25, -0.25)`, reference subsample indices, reprojection по motion vectors,
neighborhood clamp и ослабление history по скорости. История строго состоит из
текущего non-recursive neighborhood result и такого же результата предыдущего
кадра; reset срабатывает при cuts/resize/изменениях render state. Два per-frame
`RGBA16F` output FSR повторно используются как history: это 31.64 MiB resident
storage при 1920×1080 и **0 incremental bytes**. FSR Native AA остаётся отдельным
temporal reconstruction path. См. [SMAA integration notes](C:/GDF/vk_test/shaders/third_party/smaa/SMAA.h:176),
[smaa_blend.frag](C:/GDF/vk_test/shaders/smaa_blend.frag:47) и
[FSR dispatch](C:/GDF/vk_test/src/fsr3_upscaler.cpp:290).

| Техника из списка | Вердикт и сравнение |
| --- | --- |
| SMAA T2x | **Рекомендовано; уже реализовано как optional native-resolution mode.** На обновлённом `aa-wires` edge MAE снизился с `0.05203927` у SMAA1x до `0.04802657` (примерно 7.7%), но GPU frame вырос на `0.04129 ms`; на `overdraw-4` цена составила `+0.06782 ms`. Режим полезен без дополнительной памяти, но по текущим quality/performance уступает MSAA4. Default не менять; production motion/disocclusion quality требует content QA. [S25] |
| SMAA S2x и комбинированный SMAA 4x | **Рекомендовано пропустить/отложить сейчас.** Нужны сохранённый sampled 2× source, разделение subsamples, две SMAA chains, проверка standard sample locations и корректные multisample velocity inputs. Текущий forward MSAA4 проще и на fixture одновременно качественнее и быстрее; его недостаток — 189.8 MiB attachment storage. [S25] |
| Luma edges вместо нынешних color edges | **Рекомендовано пропустить.** На isoluminant chroma ROI из 74 pixels color detector дал MAE `0.0288994`, luma — `0.0536648`, то есть luma примерно в 1.86 раза хуже. Его performance-разница с color находится в шуме: `+0.0454%` FPS, `-0.00194 ms` GPU frame, `-0.00230 ms` postprocess. Color остаётся единственным detector и default. |
| Depth edges вместо color edges | **Рекомендовано пропустить.** Они пропускают same-depth material boundaries, specular/normal-map и shadow edges, требуют sampled depth resource и отдельного threshold contract; фиксированный threshold особенно плохо переносится между reverse-Z depth ranges. |
| Orthogonal/diagonal searches дают ещё +40% | **Рекомендовано пропустить непроверенную цифру.** Reference SMAA уже содержит ускоренные searches и diagonal detection; нужны конкретный diff, preset и одинаковое качество. Нельзя второй раз приписать проекту ускорение от уже используемого алгоритма. |
| Закон строго двух кадров; запрет любой более длинной history | **Рекомендовано пропустить как общее требование.** Короткая история быстрее забывает ошибки, но даёт меньше temporal samples для subpixel/shading aliasing. Ghosting определяется также motion, rejection, clipping, exposure и reconstruction; это не просто число кадров. [S26] |
| Resolve radius ≤0.15 px якобы равен UE `0.09` | **Рекомендовано пропустить.** Параметр конкретного фильтра/движка не является универсальным радиусом другого kernel. Слишком узкий reconstruction footprint может усилить shimmer/aliasing; нужны точная формула и motion QA. |
| Temporal blend только на geometric/SMAA edges, interior всегда bypass | **Рекомендовано пропустить как универсальную настройку.** Specular/normal-map/texture aliasing возникает внутри полигонов. Edge-only temporal вариант допустим как отдельный режим с явно принятой потерей стабилизации этих сигналов. |
| Programmable sample locations вместо camera jitter | **Рекомендовано пропустить как обязательную замену.** `VK_EXT_sample_locations` optional и имеет ограничения sample counts/grid/depth compatibility. Перемещение coverage samples не гарантирует эквивалентного смещения shader/texture sampling. Reference jitter и корректные motion vectors остаются fallback. [S27] |
| Все DLSS/TSR/TAA «накапливают 16–64 кадра» | **Рекомендовано пропустить неподтверждённое обобщение.** Длина jitter sequence, число хранимых textures и эффективный вес старых samples — разные величины. Они зависят от алгоритма/режима и меняются на disocclusion. |

**Проверка логики «двух кадров»:** если `Hn = a·Cn + (1−a)·H(n−1)`, то после подстановки предыдущего H остаётся вклад `(1−a)²·C(n−2)` и более старых кадров. Две ping-pong textures не дают двухкадровую историю. Для строго двух исходных кадров нужен previous non-recursively-accumulated input, согласованный resolve и history validity. Это математический вывод, не измерение конкретного SDK.

**Измеренный результат E16:** на equal-luma версии `aa-wires` edge MAE
Off/SMAA1x/T2x/FSR3 Native/MSAA2/MSAA4 равен
`0.07364754/0.05203927/0.04802657/0.03433304/0.05764277/0.03548015`.
Interior MAE соответственно ≈`0/0.00013566/0.00056777/0.00043068/≈0/≈0`.
На RX 6800, driver 8388961, Release 1920×1080 IMMEDIATE, три повтора с
warmup 3 s и measurement 6 s, при закрытом editor и process guard,
GPU frame/postprocess на `aa-wires` составили `0.1169617/0.0877499 ms` для
SMAA1x, `0.1582523/0.1178376 ms` для T2x и `0.1234626/0.0156178 ms` для
MSAA4. FSR Native измерен отдельной серией: `0.6530913/0.6121606 ms`.
На `overdraw-4`: SMAA1x `0.4446806/0.1015921 ms`, T2x
`0.5124975/0.1464892 ms`, MSAA4 `0.4598950/0.0161829 ms`; FSR Native отдельно
`1.0487279/0.6794999 ms`.

**Предел доказательства:** static fixture и moving-camera sync/screenshot smoke
пройдены, но production motion, disocclusion, foliage и glossy-content quality
не доказаны. Эти сцены остаются условием расширения применения T2x. Сравнивать
режимы следует при одинаковом render resolution; FSR upscale дополнительно —
при одинаковом display resolution с полным GPU frame cost. Выбор AA сохраняется
за пользователем.

<details>
<summary>Версия 1.0: непроверенный исходный материал — не требования к реализации</summary>

### 11.1 Spatial Stage (SMAA S2X)
* Execute optimized morphological edge detection using luma-weighted color gradients.
* Constrain line searches to orthogonal search steps with diagonal line detection enhancements (+40% speedup over baseline reference implementation).

### 11.2 Temporal Stage (SMAA T2X) — The Strict 2-Frame Law
```
Frame N-1 Output ──┐
                   ├─► [Velocity-Weighted Narrow Resolve] ──► Final Anti-Aliased Output
Current Frame N  ──┘
```

1. **Two-Frame Only History Window:**
   * **Rule:** Restrict temporal accumulation strictly to the current frame ($N$) and the immediate previous frame ($N-1$).
   * **Absolute Ban:** **Do not use infinite history accumulation buffers.** Modern temporal solutions (DLSS, TSR, standard TAA) accumulate across $16 \dots 64$ frames, introducing ghosting trails, temporal softness, and motion smearing.
2. **Narrow Filter Footprint:**
   * Constrain resolve kernel filter radius to $\le 0.15 \text{ pixels}$ (equivalent to Unreal Engine console parameter `r.TemporalAAFilterSize 0.09`).
3. **Isolated Edge Anti-Aliasing:**
   * Apply temporal blend operations **only** to fragments flagged by the SMAA edge detection mask or `SV_Coverage` geometric boundaries.
   * Internal texture surfaces remain unprocessed, retaining $100\%$ raw sampling clarity.
4. **Programmable Sample Locations (PSL):**
   * Use hardware programmable sample offsets on the GPU instead of wide sub-pixel camera jitter offsets, avoiding image smearing during static shots.

---

</details>

## 12. Physically Placed Motion Blur

### Результат проверки: motion blur — художественная опция; три предложенных «физических закона» отвергнуть

**В проекте:** реализован optional default-off depth-aware velocity-aligned
motion blur. Geometry пишет `RG16F` current-to-previous motion и `R32F`
reverse-Z; camera/object/all разделяются через inverse current и current/previous
unjittered view-projection matrices. Доступны centered/trailing shutter,
shutter angle, maximum radius и camera distance fade. `16×16` TileMax и `3×3`
NeighborMax ограничивают поиск; low/high используют 8/16 reconstruction taps.
Отдельного full-resolution output нет: два per-frame `RGBA16F` tile images стоят
около 261 120 bytes при 1080p и двух frame slots.

| Техника из списка | Вердикт и обоснование |
| --- | --- |
| Blur строго backward, всегда резкий leading edge | **Рекомендовано пропустить как единственно физически верное правило.** Интервал экспозиции может быть центрирован относительно timestamp либо заканчиваться в нём. Trailing shutter — допустимый художественный выбор, но gather-направление само по себе не гарантирует корректной экспозиции/силуэта. Понадобится reconstruction с учётом соседних движений и occlusion. [S28] |
| Всегда использовать −V…0 | **Рекомендовано пропустить буквальную формулу.** В проекте motion записан как previous−current, то есть знак противоположен распространённому current−previous. Перенос знака из чужого pipeline может размывать в другую сторону. Зафиксировать units, jitter removal, shutter fraction и знак. |
| Depth cutoff/U-curve, горизонты без blur | **Рекомендовано к имплементации только как пользовательский camera-blur control**, по умолчанию off. Это художественная/игровая настройка. При чистом вращении камеры дальняя геометрия тоже движется по экрану; физическое исчезновение blur с глубиной не следует из движения. Указанное название U-shape не соответствует описанной монотонной кривой. |
| Обычный separable Horizontal→Vertical вместо velocity-aligned filter | **Рекомендовано пропустить.** Для диагонального движения два осевых размытия образуют площадной след вместо линии вдоль velocity; при пространственно меняющихся скоростях/occlusion задача ещё сложнее. Separable tile reductions допустимы для поиска motion bounds, но не заменяют итоговую реконструкцию. |
| Полностью устранить noise/boiling отказом от stochastic samples | **Рекомендовано пропустить гарантию.** Deterministic sparse taps могут дать полосы/повторные силуэты; jitter меняет структуру ошибки. Сравнивать sampling/reconstruction на движении и disocclusion. |

**Подходящий кандидат для движка:** **рекомендовано; уже реализовано как
опциональный default-off режим.** AA Off и SMAA1x применяют blur до SMAA edge
detection; FSR Native — после upscale. MSAA, SMAA T2x и scaled FSR явно
отклоняются, пока для них нет корректного resolve/history contract. HUD и editor
рисуются после blur. Пауза больше 250 ms, camera cuts и изменения настроек
сбрасывают/snap temporal state. Typed runtime/editor MCP предоставляет
`render.set_motion_blur`; benchmark JSON публикует settings и
`average_gpu_motion_blur_ms`. [S28]

**Измеренный результат:** финальная clean Release серия на RX 6800 driver
8388961, 1920×1080 IMMEDIATE, animated `motion-bars`, AA Off, GI/day cycle Off,
fixed exposure и фиксированный procedural simulation step
`motion_fixture_fps=60` — это не FPS cap. Три rotating runs с warmup 3 s и
duration 6 s дали Off/low/high median FPS `3071.628/2866.402/2757.383`, CPU frame
`0.325560/0.348869/0.362663 ms`, GPU frame
`0.124407/0.192682/0.224867 ms`, postprocess
`0.039817/0.134150/0.166420 ms` и blur interval
`0/0.131721/0.163991 ms`. Полная GPU цена low/high равна
`+0.068275/+0.100460 ms`, FPS delta `-6.68%/-10.23%`. Fixture очень лёгкий и
CPU-bound, но эту CPU/FPS цену нельзя целиком списывать на шум; 1% low остаётся
шумным. Blur ranges low `0.131629..0.131920 ms`, high
`0.163904..0.163994 ms` подтверждают реальную разницу 8/16 taps. Clock/power
telemetry не записывалась.

**Предел доказательства:** procedural `motion-bars` содержит две противоположно
движущиеся полосы; shader/SPIR-V, Windows builds/tests, sync/ASan и screenshots
прошли. CLI/JSON `--motion-fixture-fps 30|60|144` меняет только simulation step;
визуальный Debug off/high проверен при fixed30, ASan — при fixed60. Temporal
supersample reference и production transparent/skinned content не проверялись.
Три отвергнутых выше «физических закона» не становятся
обязательными из-за реализации centered/trailing controls.

<details>
<summary>Версия 1.0: непроверенный исходный материал — не требования к реализации</summary>

### 12.1 Unidirectional Shutter Trailing
* **Problem:** Standard motion blur algorithms sample bidirectionally along the motion vector ($-\vec{V}/2 \dots +\vec{V}/2$), spreading blur forward into regions the object has not yet occupied.
* **Correction:**
  * Sample exclusively **backward** from the current surface fragment along its velocity vector ($-\vec{V} \dots 0$).
  * Preserves razor-sharp leading edges on moving objects while leaving natural motion trails strictly behind them.

### 12.2 Camera Depth Isolation (U-Shape Curve)
* Calculate blur intensity as a function of depth:
  * Foreground / Near-Plane: Full velocity-scaled motion blur.
  * Mid-to-Far Distance: Dampen motion blur rapidly down to zero.
* Keeps environmental horizons and combat sightlines sharp during rapid rotational camera movements.

### 12.3 Separable Reconstruction
* Replace noisy single-pass stochastic sampling with an orthogonal two-pass (Horizontal then Vertical) separable blur filter.
* Eliminates the need for Interleaved Gradient Noise or Blue Noise dithering, completely removing temporal grain and boil artifacts from the velocity blur.

---

</details>

## 13. Complete Frame Execution Pipeline Graph

### Результат проверки: граф и budgets из версии 1.0 не являются планом проекта

**Вердикт: рекомендовано пропустить исходный граф как обязательный pipeline.** Он предполагает несуществующие G-buffer, MSAA и character/transparency subsystems, не отражает наш voxel GI и задаёт одинаковые budgets для неописанных GPU/сцен. Сумма указанных бюджетов — 8.90 ms; это арифметика таблицы, не полученный frame time и не обещание FPS.

Текущий динамический путь в [record_commands](C:/GDF/vk_test/src/renderer.cpp): scene upload/visibility → dirty shadow map → необходимые voxel geometry/light updates → forward HDR geometry/sky → optional SSS H/V composition → exposure → AA/display mapping → HUD. Direct legacy/instancing path может отличаться. Ниже рекомендации объединены с текущим статусом реализованных этапов.

| Стадия | Рекомендация для проекта | Техническая зависимость и synthetic fixture |
| --- | --- | --- |
| CPU preparation + visibility | Сохранить CPU/GPU/direct выбор; SIMD и spatial hierarchy по профилю | Bounded bounds/hierarchy; procedural occluded/open grids с разной visible fraction |
| Shadow/GI producers | Сохранить CSM/GI; per-cascade cache и near CSM/far SDF | Tile/matrix lifetime; SDF cook/storage; moving/offscreen caster fixtures |
| Optional depth prepass | Forward-compatible partial/full/off варианты | Единый depth/coverage contract; controlled overdraw fixture |
| Main shading | Текущий forward HDR; local lights и clustered forward | Point/spot API и bounded light lists; deterministic light swarm |
| SSS/eyes | Full-res masked SSS реализован; eye renderer отложен до geometry/UV/content contract | `subsurface-profiles`, отдельный diffuse consumer и independent H/V targets; eye fixture после определения контента |
| Decals/G-buffer consumers | Только при обоснованном отдельном deferred path | Решение об архитектуре и resource dependencies, а не наличие assets |
| Transparency/particles | HDR composition + depth-aware half-res path + reactive mask | Bounded particle data; smoke/sparks и thin-occluder fixtures |
| Exposure | Из HDR radiance до display mapping; fixed в benchmark | Histogram/adaptation state; dark-room/bright-exit fixture |
| AA/reconstruction | Off default; SMAA1x spatial, optional T2x/MSAA/FSR независимо | T2x реализован на shared FSR outputs; S2x/4x отложены; production motion/gloss/disocclusion QA pending |
| Optional motion blur | Реализован default-off; после scene/SMAA1 input или FSR Native reconstruction, до HUD/editor | RG16F motion + R32F reverse-Z; Tile/NeighborMax; MSAA/T2x/scaled FSR rejected; production motion QA pending |
| Display mapping/grading/UI | Один display transform/encode; HUD последним | Сохранить инвариант |

FSR должен получать предусмотренный SDK вход; не ставить перед ним отдельный SMAA resolve только ради схемы «AA в конце». SMAA имеет собственные требования к edge signal/encoding, поэтому при изменении tone mapping отдельно проверить, что нынешний HDR color edge detection даёт ожидаемый threshold/contrast. Порядок относительно display mapping нельзя механически переносить между AA решениями.

**Рекомендовано к имплементации вместе с новыми passes:** явные producer/consumer declarations и pass timings, затем при необходимости render graph. Эти контракты являются технической основой particles/SSS/exposure/SDF, а не задачей после появления assets. Полный graph framework вводить для устранения реального lifetime/barrier duplication, не ради количества абстракций.

<details>
<summary>Версия 1.0: непроверенный исходный материал — не требования к реализации</summary>

| Phase | Sub-Pass / Technique | Target Format | GPU Budget | Pipeline Invariant |
| :---: | :--- | :--- | :---: | :--- |
| **01** | Depth Prepass | `D32_FLOAT` | $\le 0.30 \text{ ms}$ | Reverse-Z. Zero texture reads. Large structural geometry only. |
| **02** | G-Buffer Base Pass | 4x MRT (`RGB10A2`, `RGBA8`) | $2.50 \text{ ms}$ | Close alpha-tested first; prepassed opaque meshes last with `Depth Equal`. |
| **03** | Coverage Mask Gen | `Stencil` + `R8_UINT` | $\le 0.20 \text{ ms}$ | Read `SV_Coverage`; split interior pixels from silhouette edges. |
| **04** | Shadow Maps | `D16_UNORM` / `D32_FLOAT` | $\le 1.20 \text{ ms}$ | Cap atlas at $2048^2$. PSM matrix warping; distance screen-space shadows. |
| **05** | Direct Lighting | `2x RGBA16_FLOAT` | $2.00 \text{ ms}$ | Stencil volume bounds. Distinct passes per Shading Model ID. |
| **06** | SSS & Eye Forward | `RGBA16_FLOAT` | $\le 0.40 \text{ ms}$ | Full-res SSS via stencil mask. Forward eyes with dynamic pupil scalar. |
| **07** | Min-Max Translucency | Half-Res `RGBA16_FLOAT` | $\le 0.70 \text{ ms}$ | $2\times 2$ depth reduction bounds. Particles write TAA responsive alpha. |
| **08** | Tone Mapping & Exposure | Native `RGBA8` / `R11G11B10F` | $\le 0.30 \text{ ms}$ | GT7 curve. Meter auto-exposure only from Diffuse Accumulation buffer. |
| **09** | SMAA 4X (S2X + T2X) | Swapchain Output | $\le 0.80 \text{ ms}$ | Strict 2-frame window. Isolated edge blend; interior pixels bypassed. |
| **10** | Unidirectional Motion Blur| Swapchain Output | $\le 0.50 \text{ ms}$ | Trailing blur only ($-\vec{V} \dots 0$). Far-plane depth cutoff enabled. |

---

</details>

## 14. AI Agent Implementation Directives

### Результат проверки: заменить категорические asserts проверками корректности и измерений

**Вердикт: рекомендовано пропустить все исходные asserts как требования к реализации.** Некоторые из них запрещают корректные решения, другие допускают баги. Блок версии 1.0 ниже оставлен только как исследуемый материал и **не задаёт требования к изменениям renderer**; утверждённая очередь использует результаты проверки выше.

| Вместо assert из версии 1.0 | Корректный критерий |
| --- | --- |
| Albedo clear всегда равен 0 calls | Ни один consumer не читает неопределённый pixel/sample/channel; clear удаляется только при доказанном overwrite или безопасном исключении чтения |
| Prepass никогда не читает texture/discard | Prepass воспроизводит нужные position/coverage; экономия полного кадра превышает повторную обработку geometry |
| History frame depth ровно 2 | History соответствует выбранному AA; motion, disocclusion, reset и exposure корректны; реальный feedback отдельно от числа textures |
| Exposure только diffuse | Metering соответствует HDR scene signal и заданной weighting/adaptation policy |
| Motion blur только negative velocity | Направление, units и shutter interval соответствуют velocity contract; reconstruction учитывает depth |
| Shadow atlas ≤2048 | Resolution поддержан GPU и budget; каскады покрывают receivers/casters, cache согласован с матрицами |

**Acceptance Criteria для последующих реализаций:**

- Для каждого prototype указаны цель (качество/точность/latency/throughput), baseline, workload и измеримый критерий принятия; качество не подменяется одним FPS.
- Optional features имеют feature/format/sample-count detection и fallback; CPU ISA тоже проверяется.
- Нет steady-state allocations, лишнего CPU↔GPU readback, queue idle или per-frame pipeline creation.
- Изменения depth/attachments/history/caching проверены на clear coverage, image layouts, ownership и lifetime всех frames in flight.
- Performance comparison: native Windows Release, одинаковые GPU/driver/resolution/present mode/camera/light/assets/AA/GI, прогрев и минимум три сопоставимых прогона каждой стороны. Отчёт содержит median/разброс, 1% low, CPU breakdown, GPU passes и память; регрессию >~3% исследовать.
- Depth/AA/shadow/particle tests включают движение и disocclusion. Screenshot одной статичной сцены недостаточен.
- Для новых подсистем procedural fixtures входят в реализацию: фиксированный seed, точные units/parameters, воспроизводимая camera/light trajectory и reference path. Отсутствие production content не является blocker; fixture не заменяет последующую проверку реальных assets.
- Для затронутой реализации выполняются Windows Debug/Release и релевантные CTest, ASan/resource counters, object-lifetime/synchronization validation; для shader source обновляется SPIR-V Vulkan 1.3/SPIR-V 1.5 и запускается `spirv-val`.
- Build trees ограничены `windows-debug`, `windows-release`, `windows-asan`; `git diff --check` чист. Не запускать отложенные Linux gates.

**Definition of Done исследовательского этапа:** каждый пункт исходного списка имеет решение и причину; исходная реализация отделена от принятых доработок; внешние performance claims не названы локальными замерами. Само исследование не включало изменения renderer, Windows builds, GPU capture, runtime validation и новые A/B. **Текущая отдельная реализация продолжается**; её результаты и gates учитываются в [плане реализации](C:/GDF/vk_test/docs/RENDERING_PIPELINE_IMPLEMENTATION.md), а не выводятся из готовности исследования.

<details>
<summary>Версия 1.0: непроверенный исходный материал — не требования к реализации</summary>

When writing engine rendering passes based on this specification, the implementation agent must satisfy the following checks:

```python
# Validation Checkpoint for AI Code Generation:
assert render_target_clear_calls["Albedo_MRT"] == 0, "Albedo clear must be bypassed (Fullscreen Overwrite)."
assert depth_prepass.contains_texture_sampling() == False, "Depth Prepass cannot sample textures or use clip()."
assert depth_prepass.contains_alpha_test() == False, "Alpha-tested geometry must be excluded from Prepass."
assert taa_history_buffer.frame_depth == 2, "TAA accumulation history must not exceed 2 frames."
assert auto_exposure.input_target == "Diffuse_Accumulation", "Auto-exposure must not read Specular/Albedo."
assert motion_blur.sample_direction == "NEGATIVE_VELOCITY_ONLY", "Motion blur must trail behind movement."
assert shadow_atlas.max_dimension <= 2048, "Shadow atlas must remain <= 2048x2048."
```

</details>

## 15. Рекомендованная очередь работ

Эта очередь — результат сопоставления исследования с кодом и подтверждённого пользователем scope разработки движка до появления production content. Пререквизиты и процедурные fixtures реализуются вместе с соответствующими подсистемами. Реализация не означает автоматическое включение по умолчанию или доказанное ускорение.

| Приоритет | Что рекомендуется к имплементации | Зависимость и критерий принятия | Статус |
| --- | --- | --- | --- |
| P1 | Audit load/store с удалением только доказанно лишних clears (§2) | Полный coverage; сохранить SMAA edge clear; никаких undefined reads, frame/bandwidth A/B | Реализовано и проверено; результаты в docs/RENDERING_PIPELINE_RESULTS.md |
| P1 | D32 reverse-Z (§3) | Migration depth consumers, CPU/GPU occlusion, sky и FSR; procedural near/far precision fixture | Реализовано и проверено; результаты в docs/RENDERING_PIPELINE_RESULTS.md |
| P1 | Переключаемый точный GT7 reference (§10) | Reinhard baseline, fixed exposure, RGB references/colour spaces и GPU cost | Реализовано и проверено; результаты в docs/RENDERING_PIPELINE_RESULTS.md |
| P2 | Producer/consumer declarations и pass timings (§13) | Явные lifetime/read/write/clear contracts по мере добавления passes; graph framework только при устраняемой duplication | Частично: pass timings и CSM/depth-prepass contracts; общие consumers развиваются |
| P2 | Per-cascade dirty caching и optional D16 shadow profile (§6) | Tile/matrix ownership, format support; static/moving/offscreen casters и cascade-boundary fixture; A/B определяет default | Реализовано и проверено; cache даёт измеренный выигрыш, D16 оставлен optional без доказанного ускорения |
| P2 | Partial/full/off depth prepass и coarse front-to-back (§3–4) | Единый depth/coverage contract; controlled PBR/GI overdraw fixture; измерение полного кадра | Реализовано и проверено; default off, результаты зависят от overdraw и CPU sorting cost |
| P2 | SIMD/иерархия CPU culling (§1) | Contiguous bounds, bounded hierarchy, runtime ISA fallback; generated scenes с разной visible fraction; измеренный crossover | Иерархия реализована, повторное A/B; SIMD и auto thresholds pending |
| P2 | IOR, material-model/ID contract и optional LOD cross-fade (§4, §7) | Material sphere grid, procedural LOD silhouettes, stable IDs и shadow/motion consistency | Реализовано и проверено. IOR/standard/unlit прошли gates и cost A/B; source/index оптимизация отдельно улучшила CPU 100k path. LOD fade CPU/GPU прошёл numeric/runtime/ASan gates и clean fixed-clock A/B; default off |
| P2 | Простые point/spot lights → clustered forward (§7) | Light API, bounded lists/overflow fallback; deterministic 1–4096-light fixture и измеренный crossover | Реализовано и проверено; forward reference сохранён, lossless overflow даёт dropped=0, auto использует clustered от 64 lights |
| P2 | Full/half-res particles + depth-aware resolve + reactive mask (§9) | Bounded emitters, premultiplied HDR composition, depth reduction и FSR mask contract; smoke/sparks/occluders fixture | Отложено по решению пользователя до появления нужного particle-контента |
| P2 | SDF cook/storage и hybrid near CSM/far SDF (§6) | Closed triangle mesh cook, streaming/transforms/invalidation и thin/open/deformed fallback; box/sphere/concave mesh и far-caster fixtures | Отложено по решению пользователя до появления SDF/shadow-контента с реальной потребностью |
| P2 | Histogram auto-exposure и grading LUT (§10) | Metering/adaptation/manual control, colour encoding/shaper; dark-room/bright-exit и identity/known-transform LUT fixtures | Реализовано и проверено на engine numeric/runtime fixtures; fixed/off остаются default, production look/content validation отложены до появления контента |
| P2 | Full-res masked separable SSS и специализированные eyes (§8) | Profile/mask, diffuse/specular separation, procedural skin/eye geometry, independent intermediates и HDR composition | SSS реализован и проверен: default-off/lazy full-res H/V path, `subsurface-profiles`, material/editor/MCP/persistence, Windows gates и clean A/B. Eyes отложены до целевого geometry/UV/content contract |
| P3 | Forward MSAA и optional SMAA T2x; оценка S2x/4x и detectors (§5, §11) | Format/sample support, strict non-recursive history; generated wires и moving-camera smoke; production gloss/disocclusion QA | MSAA 2×/4× и SMAA T2x реализованы и проверены. T2x optional, Off и SMAA1x defaults сохранены; luma/depth и S2x/4x рекомендовано пропустить/отложить |
| P3 | Optional depth-aware velocity motion blur (§12) | Depth/velocity независимо от AA, shutter/camera/object controls; counter-motion fixture; default off | Реализован и проверен: Off/SMAA1/FSR Native paths, typed MCP, sync/ASan и clean A/B; production temporal reference/content QA pending |
| E18 / P3 | Integration matrix реализованных E01–E10, E13–E17 paths | AA/tone mapping/resize/present/mode roundtrips; combined SSS+exposure+LUT+motion; zero validation/live allocations | Завершено для реализованных путей. E11/E12 и eye specialization остаются content-deferred; условный G-buffer/deferred path пропущен |
| При отдельном архитектурном решении | G-buffer packing/decals, selective deferred MSAA (§4–7) | Требуется новое техническое обоснование deferred path; assets сами по себе не являются prerequisite | Рекомендовано пропустить в текущем forward renderer; возвращаться только отдельным решением |

**E18 evidence:** Debug `rendering-pipeline-e18-motion2` и ASan
`rendering-pipeline-e18-motion` прошли пять AA methods, tone mapper roundtrips,
resize/present/mode switches и combined SSS/exposure/LUT/motion cases. После
финальных C++ изменений Debug и ASan `e18-final` повторили по 22 captures для
Off/SMAA/MSAA2/MSAA4/FSR3 с `validation_errors=0` и
`live_gpu_allocations=0`. Combined Debug SMAA1/FSR Native и ASan SMAA1 включали
20 active SSS materials, auto exposure, tetrahedral LUT64 и motion all/high.
Это integration proof engine paths, а не production content QA.

**Не переносить из исходника:** CPU-only/5% compute doctrine; безусловный no-alpha/no-texture prepass; фиксированный draw order; обязательный 4-MRT G-buffer/DMP; PSM вместо stable CSM; 2048 cap; screen-space замена всех far shadows; запрет clustered lighting; двухсэмпловый SSS на фазе 60 FPS; diffuse-only exposure; «две textures = два кадра»; обязательные narrow/edge-only temporal rules; PSL как универсальная замена jitter; осевой H/V blur и неподтверждённые percentages/budgets.

## 16. Первичные источники и пределы доказательств

Ссылки относятся к проверке от 2026-09-12. Старые публикации используются для алгоритмов и ограничений, **не для переноса FPS со старых GPU на RX 6800**. Vulkan specification объясняет корректность API, но не гарантирует скорость; vendor guides и engine docs показывают рабочие варианты, но не выбирают победителя для нашего workload.

- **S01.** [AMD — RDNA Performance Guide][S01]. Fast-clear ограничения, Vulkan load ops, compute scheduling и shader recommendations.
- **S02.** [Intel — Masked Software Occlusion Culling][S02]. Первичная CPU SIMD occlusion работа; результаты её test scenes не считаются нашими.
- **S03.** [NVIDIA — Advanced API Performance: Command Buffers][S03]. GPU culling/indirect и оговорки о scheduling; рекомендации не означают, что CPU-path следует удалить.
- **S04.** [Khronos — VkAttachmentLoadOp][S04]. Семантика CLEAR/LOAD/DONT_CARE и undefined contents.
- **S05.** [Khronos — Fragment Operations][S05]. Depth/stencil, early/late fragment operations и blending.
- **S06.** [Nathan Reed / NVIDIA — Visualizing Depth Precision][S06]. Floating-point reverse-Z и отличие от fixed-point depth.
- **S07.** [AMD — FSR 3.1.4 Upscaler documentation][S07]. Depth, exposure, jitter, motion и reactive-mask contracts. Наличие этой страницы не означает автоматическую совместимость примера с API wrapper проекта.
- **S08.** [Olsson, Billeter, Assarsson — Tiled and clustered forward shading: supporting transparency and MSAA][S08]. Первичная работа авторов, 2012; контрпример общему запрету clustered forward.
- **S09.** [Khronos — glTF 2.0 Specification][S09]. Metallic-roughness и интерпретация intermediate metalness.
- **S10.** [Khronos — KHR_materials_ior][S10]. IOR/Fresnel и совместимость с базовым material model.
- **S11.** [Khronos — Rasterization, Sample Shading][S11]. Разделение coverage samples и fragment invocation frequency.
- **S12.** [Epic — Decal Materials][S12]. D-buffer и прямые G-buffer decals как разные варианты, а не один универсальный путь.
- **S13.** [Simon Kozlov — Perspective Shadow Maps: Care and Feeding][S13]. Геометрические и bias/stability ограничения PSM; старые API детали не переносить в Vulkan.
- **S14.** [Zhang et al. — Parallel-Split Shadow Maps on Programmable GPUs][S14]. Cascaded/split shadow mapping и распределение разрешения.
- **S15.** [NVIDIA — Integrating Realistic Soft Shadows Into Your Game Engine][S15]. PCSS: blocker search, penumbra, filtering.
- **S16.** [Epic — Contact Shadows][S16]. Screen-space depth marching и область применения.
- **S17.** [Epic — Distance Field Soft Shadows][S17]. Near CSM/far distance-field hybrid и ограничения asset representation.
- **S18.** [Rusty Koonce — Deferred Shading in Tabula Rasa][S18]. Практический deferred/light-volume контекст; не доказательство превосходства над современным forward.
- **S19.** [Jimenez et al. — Separable Subsurface Scattering][S19]. Две 1D convolutions, diffusion profiles, sampling; не две samples всего. Авторы отдельно предупреждают, что старый demo/code отличается от финальной статьи.
- **S20.** [Jimenez — Open Your Eyes, SIGGRAPH 2012][S20]. Первичный production-контекст skin/eye rendering; не источник budget 0.02 ms.
- **S21.** [Iain Cantlay — High-Speed, Off-Screen Particles][S21]. Low/mixed-resolution particles, depth downsampling и reconstruction artefacts.
- **S22.** [Polyphony Digital — GT7 Tone Mapping reference code][S22], опубликованный в [SIGGRAPH 2025 shading course](https://blog.selfshadow.com/publications/s2025-shading-course/). Reference code прочитан напрямую; большой PDF слайдов веб-инструмент не загрузил. Проверены scalar curve, chroma handling и luminance/colour-space contract, а не соответствие всех формулировок исходному видео.
- **S23.** [Epic — Auto Exposure][S23]. Log-luminance, histogram, metering mask и adaptation.
- **S24.** [OpenColorIO — Interpolation enums][S24]. Tetrahedral и trilinear 3D LUT interpolation; размер LUT и runtime cost выбираются отдельно.
- **S25.** [Jimenez et al. — SMAA: Enhanced Subpixel Morphological Antialiasing][S25]. 1x/S2x/T2x/4x и сочетание spatial/temporal samples; сверено также с встроенным `SMAA.h`.
- **S26.** [Yang, Liu, Salvi — A Survey of Temporal Antialiasing Techniques][S26]. Страница авторов подтверждает различие sample accumulation/history validation и наличие unresolved quality issues. Полный PDF в этой сессии не загрузился; формула рекурсивной истории в §11 — самостоятельный вывод.
- **S27.** [Khronos — VK_EXT_sample_locations][S27]. Optional extension и ограничения sample-location/depth contracts.
- **S28.** [McGuire et al. — A Reconstruction Filter for Plausible Motion Blur][S28]. Velocity/depth reconstruction и Tile/NeighborMax как базис, а не универсальный separable H/V blur.
- **S29.** [Khronos — Vulkan multisampling tutorial][S29]. Attachment sample counts и color resolve; tutorial не является performance-гарантией.
- **S30.** [Khronos — MSAA performance sample][S30]. Практические bandwidth/store рекомендации и зависимость стоимости от GPU architecture.

[S01]: https://gpuopen.com/learn/rdna-performance-guide/
[S02]: https://www.intel.com/content/www/us/en/developer/articles/technical/masked-software-occlusion-culling.html
[S03]: https://developer.nvidia.com/blog/advanced-api-performance-command-buffers/
[S04]: https://docs.vulkan.org/refpages/latest/refpages/source/VkAttachmentLoadOp.html
[S05]: https://docs.vulkan.org/spec/latest/chapters/fragops.html
[S06]: https://developer.nvidia.com/blog/?p=32034
[S07]: https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-upscaler/
[S08]: https://research.chalmers.se/publication/161726
[S09]: https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html
[S10]: https://raw.githubusercontent.com/KhronosGroup/glTF/main/extensions/2.0/Khronos/KHR_materials_ior/README.md
[S11]: https://docs.vulkan.org/spec/latest/chapters/primsrast.html#primsrast-sampleshading
[S12]: https://dev.epicgames.com/documentation/unreal-engine/decal-materials-in-unreal-engine
[S13]: https://developer.nvidia.com/gpugems/gpugems/part-ii-lighting-and-shadows/chapter-14-perspective-shadow-maps-care-and-feeding
[S14]: https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-10-parallel-split-shadow-maps-programmable-gpus
[S15]: https://developer.download.nvidia.com/assets/gamedev/docs/PCSS_Integration.pdf
[S16]: https://dev.epicgames.com/documentation/unreal-engine/contact-shadows?application_version=4.27
[S17]: https://dev.epicgames.com/documentation/en-us/unreal-engine/distance-field-soft-shadows-in-unreal-engine
[S18]: https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-19-deferred-shading-tabula-rasa
[S19]: https://www.iryoku.com/separable-sss/
[S20]: https://www.iryoku.com/open-your-eyes/
[S21]: https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-23-high-speed-screen-particles
[S22]: https://blog.selfshadow.com/publications/s2025-shading-course/pdi/supplemental/gt7_tone_mapping.cpp
[S23]: https://dev.epicgames.com/documentation/unreal-engine/auto-exposure-in-unreal-engine
[S24]: https://opencolorio.readthedocs.io/en/v2.5.0/api/enums.html#interpolation
[S25]: https://www.iryoku.com/smaa/downloads/SMAA-Enhanced-Subpixel-Morphological-Antialiasing.pdf
[S26]: https://research.nvidia.com/labs/rtr/publication/yang2020survey/
[S27]: https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_sample_locations.html
[S28]: https://casual-effects.com/research/McGuire2012Blur/index.html
[S29]: https://docs.vulkan.org/tutorial/latest/10_Multisampling.html
[S30]: https://docs.vulkan.org/samples/latest/samples/performance/msaa/README.html
