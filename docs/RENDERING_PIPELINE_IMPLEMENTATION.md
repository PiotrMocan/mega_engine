# Очередь развития rendering pipeline движка

Дата обновления: 2026-09-13. Основание — исследование
`low_overhead_modern_game_rendering_pipeline_specification.md` версии 1.5.0
и подтверждение пользователя: сначала разрабатывается движок, игровой контент
появится позже.

Отсутствие production assets не блокирует рекомендованные подсистемы.
Их data/API/resource contracts, процедурные fixtures и проверки входят в работу.
Техническая зависимость означает предыдущий реализуемый этап, а не отказ от scope.
Условия производительности определяют default/auto path и quality presets;
сам факт наличия shader или тестовой картинки не доказывает полезность режима.

## Статус и порядок

Этот файл — очередь и критерии приёмки. Измеренные результаты и фактически
запущенные Windows gates описаны в `docs/RENDERING_PIPELINE_RESULTS.md`.
P1 реализован и проверен; независимое обновление каскадов прошло A/B и
sync/ASan проверки; D16 прошёл отдельную проверку и не показал ускорения. E10,
E13, SSS-часть E14, forward MSAA E15, SMAA T2x E16 и motion blur E17 реализованы на
synthetic engine fixtures. E18 завершает интеграционную проверку реализованных
путей; content-deferred ветки не считаются реализованными.

| Этап | Принятая доработка | Зависимости | Статус |
| --- | --- | --- | --- |
| E01 / P1 | Audit load/store и удаление доказанно избыточных clears | Baseline, coverage и read/write inventory | Реализовано: final fullscreen DONT_CARE, необходимые clears сохранены; Windows gates и A/B пройдены |
| E02 / P1 | D32 reverse-Z | Единый camera/depth contract, полный inventory consumers | Реализовано: camera/sky/CPU/GPU culling/FSR, численные tests и Windows gates пройдены |
| E03 / P1 | Переключаемый точный GT7 SDR tone mapper | HDR/colour-space contract, Reinhard baseline | Реализовано: 19 GPU reference samples, AA matrix и 18 A/B runs; Reinhard остаётся default |
| E04 / P2 | Pass dependencies, bounded resources, timings и fixture infrastructure | E01–E03; внедрять вместе с первыми новыми passes | Частично: postprocess timing и CSM resource contract; общие depth/velocity consumers развиваются со следующими passes |
| E05 / P2 | Per-cascade cache и optional D16 shadow profile | E04, действующие CSM, tile/matrix ownership | Реализованы и проверены, оба опциональны; ускорение cache измерено, выигрыш D16 не установлен |
| E06 / P2 | Optional depth prepass и coarse front-to-back | E02, E04, единый transform/coverage contract | Реализованы и проверены; fixed-clock A/B показывает зависимость prepass от overdraw и tail-risk sorting; оба default off |
| E07 / P2 | CPU spatial hierarchy/SIMD и измеренный выбор visibility path | E04, contiguous bounds, runtime ISA detection | Whole-inside hierarchy прошла gates и A/B: 100k FPS +32.27%, маленькая сцена сохраняет linear default. Source/index packets отдельно дали +109.91% FPS на CPU 100k; SIMD/auto policy pending |
| E08 / P2 | IOR и material-model/ID contract | Действующий PBR и asset/runtime material schema | IOR/standard/unlit, import/serialization/editor/MCP, reference и runtime gates пройдены. Измеренная цена ABI: canonical GPU +1–2%, прежний CPU 100k path +10.37%; последующая source/index оптимизация измерена отдельно |
| E09 / P2 | Optional LOD cross-fade | E06, E08, stable IDs, previous/current transforms, shadow LOD contract | Реализован и проверен CPU/GPU Bayer4/8; 3 Windows builds/CTest, numeric/runtime/ASan gates и clean fixed-clock A/B пройдены. Default off; включать для authored LOD после content review |
| E10 / P2 | Local point/spot lights и clustered forward | E04, light API/buffers, простой forward reference | Реализовано и проверено: KHR-style lights, forward reference, bounded clustered path, lossless overflow fallback, Windows gates и fixed-clock A/B пройдены; auto использует clustered от 64 lights |
| E11 / P2 | Full/half-res particles и reactive mask | E02, E04, transparency/depth/upscaler resource contracts | Отложено по решению пользователя до появления подходящего particle-контента |
| E12 / P2 | SDF cook/storage и near CSM/far SDF shadows | E04, E05, mesh cook/streaming/transform contracts | Отложено по решению пользователя до появления SDF/shadow-контента с реальной потребностью |
| E13 / P2 | Histogram auto-exposure и grading LUT | E03, E04, temporal exposure contract | Реализовано и проверено: общий HDR metering, external FSR exposure, identity 32³/64³ LUT, Windows/sync/ASan/numeric gates и чистый A/B пройдены; fixed/off остаются default |
| E14 / P2 | Masked separable SSS, material variants и специализированные eyes | E04, E08, diffuse/specular separation и masks | SSS реализован и проверен: default-off/lazy full-res path, Windows/sync/ASan/MCP/visual gates и clean A/B пройдены. Eye renderer отложен до появления целевого geometry/UV/content contract |
| E15 / P3 | Forward MSAA 2×/4× | E02, E04, format/sample-count detection и depth resolve contract | Реализовано и проверено: 2×/4× coverage/depth, average HDR resolve, Windows/sync/ASan/MCP/image gates и два clean A/B; optional, default не изменён |
| E16 / P3 | SMAA detector A/B, optional T2x, оценка S2x/4x | E15 для S2x/4x; независимые motion/history для T2x | T2x реализован и проверен как optional; color остаётся единственным/default detector; luma/depth пропустить, S2x/4x отложить |
| E17 / P3 | Optional depth-aware velocity-aligned motion blur | E02, E04, независимые velocity/depth, выбранный AA/exposure contract | Реализован и проверен как default-off: Off/SMAA1/FSR Native, runtime/editor MCP, sync/ASan и clean A/B; production content QA pending |
| E18 / P2–P3 | Интеграционная матрица и выбор default/auto policies | Реализованные E01–E10, E13–E17 paths; повторная итоговая проверка | Завершено для реализованных путей: Debug/ASan 22 captures, пять AA, combined SSS/exposure/LUT/motion, validation/live allocations 0. Content-deferred и условные paths исключены |

Ветка G-buffer packing/decals/selective deferred MSAA пропущена для текущего
forward renderer. Вернуться к ней можно только через отдельное техническое
решение, а не ради схемы из видео. Hair/foliage shader variants
получают общий material-model contract; новые BRDF должны иметь собственную
определённую модель и reference до специализации.

## Общие требования к fixtures и измерениям

Приведённые ниже имена `depth-range`, `particles-edges` и другие — **планируемые
идентификаторы тестовых сцен**, не утверждение о существующих CLI-командах.
Каждая сцена должна воспроизводиться из фиксированного seed, параметров и
trajectory. Проверять API и asset pipeline через те же структуры, которыми
будет пользоваться будущий игровой контент, без отдельной неподдерживаемой
логики исключительно для демонстрации.

- Генерация mesh/texture/LUT/particle seeds, cook и allocation выполняются при
  подготовке fixture или асинхронно; steady-state render loop не аллоцирует.
- Fixture сохраняет реальные units, camera near/far, lighting, timings,
  capacities, render/display resolution и выбранные режимы. Временные outputs
  располагаются внутри одного из трёх разрешённых Windows build trees.
- Для каждого нового пути есть reference/off/fallback. Сравнение включает
  uploads, reductions, intermediate targets, masks, composite, sync и память.
- Измерять native Windows Release при одинаковых GPU/driver/resolution/present
  mode/scene/seed/camera/light/AA/GI; прогрев и минимум три прогона каждой стороны.
  Показывать median/разброс, 1% low, CPU breakdown и GPU pass timings.
- Сценарий, который выгоден новой технике, дополняется маленькой или неблагоприятной
  сценой. Условия crossover определяют `auto`; quality feature может стоить
  дороже, но стоимость и улучшение должны быть явно представлены.
- Ресурсы feature имеют bounded capacity, resize/reset/retirement contract и
  статистику. При выключении feature нет её постоянных dispatch/draw/upload
  расходов; допустимое хранение кеша имеет предел и политику освобождения.
- Optional Vulkan formats/features/sample counts и CPU ISA проверяются runtime.
  Fallback сохраняет корректность; overflow не обрезает видимые объекты/lights
  молча и не допускает выхода за границы buffers.

## E01–E03: фундамент кадра

**E01, `attachment-coverage`.** Проверить clear/load/store каждого читаемого
attachment, renderArea/scissor, channel write masks, discard, blending и layers.
Fixture сочетает sky on/off, пустой фон, редкую opaque geometry, HUD и все AA
режимы; затем resize и переключение modes. Удалять clear только при полном
overwrite до первого чтения. SMAA edge clear сохраняется, поскольку edge shader
может discard. Проверка должна выявлять зависимость от старого содержимого
attachment, а не только сравнивать один удачный кадр.

**E02, `depth-range`.** Near/far planes и CPU/GPU projection используют один
контракт: near → 1, far → 0, clear 0, сравнения GREATER/GREATER_EQUAL. Проверить
sky, frustum/occlusion, motion, FSR flags и inputs, reconstruction, будущие
depth reductions. Shadow-map convention учитывается отдельно. Fixture: пары
почти копланарных поверхностей на 0.11, 1, 100, 300 и 499 м, пересечение near
plane, sky/background и moving occluder; gaps меняются относительно глубины.
Численная проверка должна показать precision D32 на дальней глубине; image
comparison проверяет отсутствие новых visibility/sky/FSR ошибок. D24 fallback
не объявлять равноценным по точности.

**E03, `hdr-colour-chart`.** Reinhard остаётся baseline; GT7 реализуется по
официальному SDR ICtCp reference с Rec.709↔Rec.2020 и одним sRGB encode.
Чёрный, нейтральные серые, насыщенные RGB/CMY, HDR gradients, negative undershoot
и максимальные конечные HDR значения проверяются численно и на GPU. Настройки
и эталоны описаны в `docs/GT7_TONE_MAPPING.md`. Проверить AA off/SMAA/FSR на
одинаковом HDR input и fixed exposure. Разница яркости операторов не скрывается
автоматической компенсацией. Замерять display-pass cost отдельно.

## E04: контракты passes и воспроизводимая нагрузка

Для каждого нового pass зафиксировать producers/consumers, read/write layouts,
stage/access, extent/sample count, clear policy, frame ownership и последний
GPU consumer. Общие depth/velocity/mask ресурсы запрашиваются потребителями,
а не случайным состоянием FSR. Обеспечить отдельные timings и bounded storage.
Render graph вводить там, где он устраняет повторение lifetime/barrier логики;
полная замена инфраструктуры не является prerequisite первого небольшого pass.

**Приёмка:** Windows sync/object-lifetime validation на repeated startup/shutdown,
resize, mode switches и комбинациях producers/consumers; ASan/resource counters;
никаких read-before-write или use-after-retirement. Пустая/выключенная feature
не добавляет скрытых producer passes. Seed и настройки fixture записываются
в benchmark metadata без per-frame logging.

## E05: CSM cache и формат shadow maps

Вести dirty state и матрицу каждой cached tile; сохранить нетронутые tiles,
очищать только обновляемые регионы и инвалидировать по caster/light/camera
coverage. Teleport, изменение light direction, transform/asset changes и выход
из coverage margin вызывают нужное немедленное обновление. Срок годности
определяется временем и coverage, а не чётностью номера кадра.

**Fixture `cascade-cache`:** ground и повторяемые box/sphere casters около
границ всех четырёх каскадов; неподвижная сцена, camera pan, teleport, moving
caster и низкое солнце; отдельный offscreen caster. Сравнить с full atlas redraw
при той же матрице/свете, проверить stale shadows и пересечения blend bands.
Показать число обновлённых cascades, GPU cost и стоимость tile clears.

Optional D16 profile проходит format/comparison/filter detection и отдельную
проверку bias/acne/peter-panning на длинном depth range; D32 сохраняется как
reference/fallback. D16 не включается автоматически по одному меньшему размеру.

## E06–E09: depth, visibility, материалы и LOD

**E06, `controlled-overdraw`:** создать 1/4/16 слоёв opaque geometry, крупные
ближние occluders и множество маленьких дальних объектов. Запускать при дешёвом
и дорогом существующем PBR/GI shading. Реализовать `off/partial/full` prepass
и coarse front-to-back внутри batching buckets. Сравнить полный кадр и CPU
sorting cost. Prepassed shading использует согласованный EQUAL/depth-write-off
путь; transformations, jitter и coverage совпадают. Alpha/deformation fixtures
добавляются вместе с соответствующими material/geometry paths, без запрета
необходимых texture reads. Low-overdraw baseline должен сохранять прямой путь.

**E07, `visibility-grid`:** deterministic grids на 16, 1024, 100000 и доступной
максимальной capacity объектов с 0/25/75/100% видимостью; статические bounds
и контролируемые transform changes. Реализовать contiguous bounds/spatial
hierarchy, затем SIMD при подтверждённом CPU bottleneck; ISA fallback обязателен.
Сравнить CPU/GPU/direct selection, preparation/upload/visibility и frame cost.
Reference frustum/occlusion проверки не допускают false occlusion. Worker pool
и thresholds принимаются по полезной работе, маленькая сцена не платит за
координацию больше стоимости прямого пути.

**E08, `material-spheres`:** procedural sphere grid с IOR 1.0/1.33/1.5/2.0,
roughness 0.05/0.25/0.7 и metallic 0/1 под фиксированным HDR освещением.
Dielectric F0 = `((ior - 1) / (ior + 1))²`; default 1.5 сохраняет прежние 0.04.
Проверить input validation, material serialization/import/runtime update и
согласованность CPU/GPU layout. Conductor использует свой F0 contract, без
подмены произвольным IOR multiplier. Shading-model IDs и masks имеют точный
encode/decode и не усредняются как цвет. Packing применяется к реальному
потребителю, не создаёт обязательный G-buffer.

**E09, `lod-silhouettes`:** procedural сфера/ступенчатый mesh с тремя LOD,
контрастный силуэт и решётка тонких элементов. Камера пересекает оба направления
LOD thresholds, затем объект движется при статичной камере. Fade является
опцией; off/hysteresis reference сохраняется. Общие object ID/weight используются
в camera/shadow/motion paths; camera-space и light-space `gl_FragCoord` сами
по себе не считаются согласованным dither. Сравнить Bayer/blue-noise и длительность
fade при AA off/SMAA/FSR, проверить shadow pops, disocclusion и двойную shading
стоимость во время transition. Фиксированный Bayer 2×2 не является требованием.

## E10: local lights и clustered forward

Реализован bounded API до 4096 point/spot lights с KHR-style контрактом:
linear RGB, intensity в candela, конечный range, локальная ось `-Z` для spot,
smooth distance cutoff и squared cone falloff. Scene JSON, editor authoring и
типизированный MCP используют тот же payload. GPU records находятся в per-frame
SSBO и обновляются после fence соответствующего frame. Area/capsule lights не
добавлены без определённого BRDF и integration contract.

Простой forward loop сохранён как reference. Clustered forward строит
`32x18x24` clusters light-centric compute dispatch-ем: одна 64-thread workgroup
на light, до 64 индексов в cluster. Bounded headers/indices и statistics имеют
per-frame ownership. Если cluster переполнен, fragment path losslessly считает
все lights через full-forward fallback; `dropped=0`. Доступны число assignments,
максимальная длина списка, overflow clusters и отдельный
`gpu_light_culling_ms`. Политика `auto` выбирает clustered начиная с 64 lights,
а малые наборы оставляет на прямом forward path.

**Fixture `light-swarm`:** deterministic 1/16/64/256/4096 point/spot lights в
`separated` и `overlap` layouts. Debug validation дал точное равенство PNG между
forward и clustered для 256 lights в обоих layouts, включая 3384 overflow
clusters в overlap. Stress 4096 подтвердил bounded статистику и lossless
fallback: separated — 605211 assignments, max 290, overflow 3835; overlap —
10494648 assignments, max 4096, overflow 3456; в обоих случаях `dropped=0`.
Три Windows build прошли 7/7 CTest, shader compile/`spirv-val`, Debug validation
и ASan runtime/editor authoring gates.

Fixed-clock RX 6800 A/B подтвердил crossover для separated volumes: clustered
уже быстрее при 16 lights и выигрывает 44.89% GPU frame при 64 и 77.63% при
256. Для overlap он дороже до 64 lights и почти равен forward при 256/4096 из-за
overflow fallback. Поэтому реализация рекомендована как engine subsystem с
forward reference и порогом auto 64; выбор можно уточнить по production content.
Local direct lighting пока unshadowed и не инжектируется в voxel GI — это явные
границы E10, а не незавершённость его принятого point/spot contract.

## E11: particles, transparency и reactive mask

Сделать bounded emitter/particle данные, spawn/lifetime/transform contract,
стабильный seed, camera-facing geometry и явную alpha/order policy. Начать с
full-resolution HDR reference, premultiplied-alpha composition и soft depth
intersection. Затем half-resolution путь для низкочастотных effects, min/max
depth reduction с reverse-Z semantics и depth-aware reconstruction. Thin/sharp
effects и неоднозначные depth edges должны иметь full-res выбор/fallback.

Reactive mask — отдельный ресурс/явно извлечённый сигнал по контракту FSR,
сохраняющий alpha/transmittance композиции. Не заменять её одним mobility bit
и не записывать 1 повсюду. Проверить layout, extent, reset и lifetime SDK input.
Чистый particle alpha blending не объявляется полноценным froxel volumetrics.

**Fixture `particles-edges`:** процедурные smoke puffs с мягким radial alpha,
яркие sparks, fire-colour gradient и перекрывающиеся слои без внешних текстур;
32/4096/32768 particles в пределах указанной capacity. Тонкие стойки, наклонная
стенка и дверной проём создают foreground/background depth discontinuities.
Камера и emitters движутся независимо; есть быстрые bright particles, spawn/despawn
и camera cut. Сравнить full/half-res, mask off/on, AA off/SMAA/FSR. Проверять
opacity, sorting policy, clipping, halos, temporal trails и disocclusion.
Стоимость reduction/upscale/mask/composite включается в итог; half-res не
становится default для всех effects только по меньшему render target.

## E12: SDF asset pipeline и дальние тени

Cook принимает замкнутый triangle mesh, а не только встроенные аналитические
примитивы. Нужны sign convention (снаружи положительно, внутри отрицательно),
local-space distance units, voxel-center/grid/bounds metadata, padding,
разрешение/precision/error policy и versioned storage. Проверять overflow
размеров/индексов и некорректные/open/non-manifold inputs. Cook выполняется вне
render hot path; загрузка, streaming, bounded cache, resource retirement и
ошибки assets проходят штатный pipeline.

Runtime задаёт transform/invalidation и консервативную обработку масштаба,
включая non-uniform scale. Для open/thin/deformed/неподдерживаемых meshes
нужен raster fallback или явная диагностируемая политика; их нельзя молча
считать замкнутыми solid casters. GI occupancy/radiance не является SDF.

**Fixture `sdf-cook`:** закрытый box, triangulated sphere и замкнутый вогнутый
ступенчатый mesh, с несколькими сетками, например 32³/64³/128³. Для box/sphere
сверить sign и distances с analytic reference с учётом mesh approximation;
для общего mesh — с независимым nearest-triangle/sign reference. Ошибка
storage+sampling документируется в voxel units; целевой bound на fixtures —
не больше диагонали voxel cell вне оговорённой boundary ambiguity. Отдельно
open plane, thin wall, inverted winding и malformed indices проверяют
validation/fallback. Проверить cook→save→load и roundtrip metadata/distances.

**Fixture `far-sdf-shadows`:** receivers около перехода из CSM в SDF и на
250–450 м при текущем camera far 500 м; box/sphere/concave casters, включая
offscreen caster, низкое солнце, moving rigid transform, scale и asset reload.
Сравнить с raster shadow reference достаточного coverage/resolution при том
же свете; оценить leaks, self-shadow bias, penumbra и seams hybrid transition.
Проверить bounded trace steps, streaming miss fallback и немедленное invalidation.
Показать cook time, resident memory, trace/pass/frame cost. SDF runtime не
объявляется завершённым при наличии только аналитического raymarch demo.

## E13: exposure и grading

Реализован 256-bin log-luminance histogram общего HDR scene color до AA/display
mapping. Linear Rec.709 luminance попадает в диапазон `[-16,+16] EV`; отдельные
краевые bins удерживают black/underflow и highlights. Доступны uniform и
center-weighted metering, percentile rejection, EV compensation/clamps и разные
временные константы осветления/затемнения. Адаптация выполняется в EV space,
не зависит от частоты кадров и сбрасывается при scene/camera discontinuity.
Невалидные HDR samples считаются, но не заражают state; пустой black frame
удерживает последнее корректное exposure.

Один device-lifetime state buffer хранит значение для display mapping, а
`1x1 R32_SFLOAT` image передаёт тот же GPU-generated float в FSR. FSR internal
auto-exposure отключён; SDK и GT7/Reinhard используют одно вычисленное exposure
при `preExposure=1`. Это сохраняет единый HDR domain и исключает две
конкурирующие экспозиции. Cross-submission зависимости явно покрывают
compute/fragment reads;
resize пересоздаёт extent-dependent AA targets, сохраняя exposure и LUT resources.
Fixed exposure остаётся default и полностью пропускает histogram pass при AA
off/SMAA. Runtime diagnostic readback имеет 32 bytes и читается только после
fence соответствующего frame.

Color grading выполняется после GT7/Reinhard в display-linear Rec.709 `[0,1]`
и до единственного hardware sRGB encode. В памяти постоянно находятся identity
`RGBA16F` LUT 32³ (0.25 MiB) и 64³ (2 MiB). Доступны hardware trilinear и
explicit tetrahedral interpolation; `off` не делает texture sample. Поддержка
3D `RGBA16F` sampled image проверяется через точные format/image properties.
Production look не добавлен без контента: identity LUT реализует и проверяет
engine contract, а художественный LUT появится вместе с материалом для оценки.

CLI, editor и типизированные MCP `render.set_exposure`/
`render.set_color_grading` меняют настройки атомарно. Requested/actual mode,
metering, exposure state, LUT metadata и отдельные `gpu_exposure_ms`/
`gpu_display_ms` доступны в debug state и benchmark JSON. Explicit GPU tests
сравнивают все histogram bins и adaptation с независимым double reference, а
144 LUT cases покрывают identity/affine/nonlinear transforms, все tetrahedral
порядки, tie planes, endpoints и clamp. CPU step response проверен при
30/60/144 FPS. Полноэкранные identity captures отличаются от bypass не более
чем на 1 код 8-bit канала, что соответствует промежуточной `RGBA16F` precision.

Чистый Windows Release A/B находится в
`build/windows-release/display-mapping-e13-v1`: 18 runs, 1920x1080 IMMEDIATE,
fixed/off reference и отдельные LUT/auto cases. Median `gpu_display_ms`:
0.08091 off, 0.08534 trilinear 32³, 0.09263 tetrahedral 32³, 0.08575
trilinear 64³ и 0.09348 tetrahedral 64³. Auto histogram/adaptation стоит
0.05333 ms. Fixed/off остаются benchmark defaults; auto/LUT включаются по
требованию camera/content, а 32³ trilinear является дешёвым начальным quality
режимом. Процедурная `exposure-room` и production grading chart отложены до
появления контента и не требуются для готовности engine API/numeric contract.

## E14: SSS и глаза

Реализован `MaterialModel::subsurface` с профилями `none`, `narrow`, `medium`,
`wide` и strength `[0,1]`. Profile занимает точный `uint32_t` в CPU/GPU ABI,
strength — полный binary32. Geometry shader разделяет lighting: specular и
нерассеиваемая доля diffuse попадают в HDR base, а quantized strength × diffuse —
в отдельный target. Его alpha содержит целочисленный control
`profile * 256 + strengthUNorm8`; значения до 1023 точно сохраняются в
`RGBA16F`, а point sampling не смешивает material IDs.

Full-resolution H/V passes используют по семь samples, раздельные read/write
images, reverse-Z-aware linear depth rejection и строгую проверку profile.
Profile задаёт RGB Gaussian sigma в единицах сцены; радиус ограничен 24 pixels.
Оба passes пока растеризуют fullscreen triangle; pixels с `profile == none`
выходят до convolution. Маска предотвращает смешивание материалов, но не
сокращает fragment coverage; stencil/tile compaction требует отдельного
coverage A/B на сцене с персонажами.
Vertical pass композитит filtered diffuse с HDR base, поэтому specular не
размывается. Это измеримый engine/reference baseline на основе принципа двух 1D
convolutions из [Separable Subsurface Scattering](https://www.iryoku.com/separable-sss/),
а не обещание точного совпадения с финальным production kernel статьи. Half-res,
Bayer temporal sampling и in-place соседние reads/writes не входят в реализацию.

Режим `off` является default и не provision-ит SSS targets. Первый rendered
frame после выбора `full` лениво создаёт `subsurface_base` и `subsurface_ping`
`RGBA16F` для каждого из двух frame slots. При 1920×1080 их дополнительный
resident footprint равен 66 355 200 bytes (63.28125 MiB). Выключение прекращает
MRT/SSS passes; уже созданные targets сохраняются до resize/extent recreation или
shutdown. Mode change сбрасывает temporal history, FSR и exposure history.
Instancing workload не поддерживает PBR/SSS и диагностирует actual mode `off`.

CLI использует `--subsurface off|full`. Runtime и editor предоставляют
типизированный MCP `render.set_subsurface`; debug/benchmark state различает
requested/actual, provisioned, active material count, render extent, resident
bytes и `gpu_subsurface_ms`. Editor renderer panel меняет глобальный mode, а node
inspector редактирует `subsurface` model, profile и strength. Scene и cooked
material JSON сохраняют эти поля с точными lowercase tokens и strict range
validation; старые документы остаются `standard`/`none`/`0`. Importer не
угадывает scattering и явно выдаёт `none`/`0`. Material-document loader/saver
сохраняет новые поля, но отдельное imported-submesh authoring UI не добавлено без
соответствующего typed MCP action.

**Fixture `subsurface-profiles`:** 28 сфер в семи рядах по четыре: standard
control, `narrow`/`medium`/`wide` strength 1, `medium`/`wide` strength 0.5,
glossy SSS и unlit control. Дистанция меняется по колонкам; 20 материалов активно
используют SSS. Он проверяет profile/strength/MRT/filter contract до появления
production кожи. Финальный clean 1920×1080 Release IMMEDIATE A/B после
исправления общего 24-pixel kernel-radius cap с тремя повторами дал для `full`
+0.06626 ms GPU frame, −9.50% FPS и 0.09864 ms собственного SSS interval;
после provisioning занято 63.28125 MiB. Поэтому SSS принят как optional quality
subsystem, а `off` сохраняется default.

Debug/Release/ASan прошли 8/8 CTest. Debug synchronization validation покрыла
dynamic AA off/SMAA/FSR и legacy AA off; ASan synchronization validation — FSR
runtime. Editor MCP smoke прошёл с 22 tools. Material GPU reference проверил 30
материалов, exact model/profile/strength bits, maximum F0 error
`3.774273360513e-08` и ноль live allocations. Fixed 1280×720 off/full captures
изменили 1.9679% pixels с maximum RGB difference 21/5/1; визуально явного
profile/depth bleeding не обнаружено. Это fixture evidence, не утверждение о
pixel equivalence или готовности production skin profiles.

Dedicated eye renderer отложен до появления целевой eye geometry, normals,
UV/layout iris и контракта композиции. `MaterialModel::eye` сейчас не добавляется.
После появления content contract сначала нужен непрозрачный sclera/iris baseline;
transparent cornea shell, tear/wetness, refraction, calibrated sclera scattering
и pupil response являются отдельными quality этапами. Документация Unreal также
показывает зависимость eye shader от специальной геометрии и UV:
[Eye Shading Model](https://dev.epicgames.com/documentation/unreal-engine/shading-models-in-unreal-engine)
и [Digital Humans](https://dev.epicgames.com/documentation/unreal-engine/digital-humans?application_version=4.27).

## E15–E16: native AA modes

E15 реализует отдельные `msaa2x` и `msaa4x`. Точный support является
пересечением framebuffer color/depth limits и `sampleCounts` для выбранных
`RGBA16F`/depth formats. Geometry и depth-prepass pipelines заранее создаются
для поддержанных sample counts; `sampleShadingEnable=false`. Каждый frame slot
после собственного fence лениво получает multisampled HDR color и depth.
Dynamic rendering делает `AVERAGE` color resolve в прежний single-sample HDR
target с `storeOp=DONT_CARE` для multisample source. Resolved depth пока никто
не потребляет, поэтому depth resolve отсутствует. Будущий reverse-Z consumer
потребует проверенный `MAX` resolve или shader fallback.

`requested` и `actual` режимы/samples, support, formats, resolve modes, extent
и resident bytes опубликованы в benchmark JSON и runtime/editor MCP. Runtime
переключение и resize не используют `vkDeviceWaitIdle`; targets конкретного
frame slot меняются после его fence. Startup 4× может откатиться на 2×, затем
Off, а 2× — на Off; benchmark отклоняет любой такой fallback. Текущий SSS
кодирует дискретный profile ID вместе со strength в FP16 alpha, поэтому обычный
average resolve разрушил бы mask. Комбинация MSAA+full SSS строго отвергается
в CLI и typed MCP в обоих направлениях.

E16 добавляет к прежнему SMAA1x optional native-resolution `t2x`. Projection
чередует jitter `±(0.25, -0.25)`, blending-weight pass получает reference
subsample indices, а resolve использует motion reprojection, current-neighborhood
clamp и motion attenuation. Previous input — non-recursive neighborhood result
кадра N−1, поэтому две frame textures действительно задают строгую пару N/N−1,
а не скрытую экспоненциальную историю. Validity и phase сбрасываются вместе с
temporal state при resize, cuts и несовместимых изменениях scene/render state.

Для history повторно используются два per-frame `RGBA16F` FSR outputs. При
1920×1080 это 31.64 MiB общей resident capacity, но 0 incremental bytes для
T2x. Runtime/editor typed `render.set_anti_aliasing` принимает optional
`smaa_mode: "1x"|"t2x"`; capabilities, state и benchmark JSON публикуют mode,
temporal samples, validity/phase, extent, resident/incremental bytes и признак
reuse FSR output. SMAA1x остаётся default SMAA mode.

Пока T2x повторно использует общий с FSR temporal geometry path, он записывает
и переводит в sampled layout full-resolution `R32F` depth target, который SMAA
resolve не читает. Измеренная стоимость включает этот overhead. Его следует
убрать отдельным color+motion pipeline variant после профилирования, не меняя
публичный T2x contract.

Color detector остаётся единственным. На isoluminant chroma ROI из 74 pixels
color MAE `0.0288994`, luma `0.0536648` — примерно в 1.86 раза хуже. Разница
luma против color (`+0.0454%` FPS, `-0.00194 ms` GPU frame,
`-0.00230 ms` postprocess) находится в шуме и не компенсирует потерю edges.
Depth detector также пропущен: same-depth material/specular/shadow edges ему не
видны, а sampled reverse-Z depth добавляет resource и threshold contract.

SMAA S2x и комбинированный SMAA4 отложены. Им нужны сохранённый sampled 2×
source, subsample separation, две SMAA chains, standard sample locations и
multisample velocity contract. Уже реализованный MSAA4 проще и на текущем
fixture качественнее/быстрее, хотя требует больше attachment memory. Mandatory
edge-only/narrow-kernel/programmable-sample-location правила из исходного списка
не переносятся.

**Fixture `aa-wires`:** реализованы 17 обычных unlit cube instances: четыре
толщины диагональных wires в двух направлениях, touching red/green material
edge, near cross поверх far panel и interior controls. Исторический E15 fixture
до equal-luma корректировки дал edge MAE Off/SMAA/MSAA2/MSAA4
`0.07537/0.05320/0.05904/0.03650`; он сохранён в results как исходный MSAA gate.

Последний equal-luma gate дал Off/SMAA1x/T2x/FSR Native/MSAA2/MSAA4
`0.07364754/0.05203927/0.04802657/0.03433304/0.05764277/0.03548015`.
Interior MAE: ≈`0/0.00013566/0.00056777/0.00043068/≈0/≈0`. T2x улучшил
SMAA1x edge MAE примерно на 7.7%, добавив `0.04129 ms` GPU frame на `aa-wires`
и `0.06782 ms` на `overdraw-4`. Он медленнее и менее точен, чем MSAA4, но не
добавляет memory; FSR Native точнее, но существенно дороже. Поэтому T2x принят
как optional, defaults не меняются.

Static fixture и moving-camera sync/screenshot smoke не доказывают production
motion quality. Alpha/foliage, glossy normal/specular aliasing, disocclusion и
camera-cut content QA остаются pending; ни E15, ни E16 не объявляют их решёнными.

## E17: motion blur — реализован и проверен как optional default-off

Geometry производит `RG16F` current-to-previous motion и `R32F` reverse-Z.
Режимы `camera`, `objects` и `all` разделяют вклад через inverse current и
current/previous unjittered view-projection matrices. Настройки включают
`centered|trailing` shutter, angle, maximum radius и camera distance fade.
Пауза больше 250 ms, camera cuts и изменение настроек делают snap/reset history.

`16×16` TileMax и `3×3` NeighborMax строят bounded velocity field; low/high
reconstruction использует 8/16 taps вдоль движения с depth-aware selection.
Дополнительный full-resolution target не создаётся. Два маленьких per-frame
`RGBA16F` tile images занимают около 261 120 bytes при 1920×1080 и двух frame
slots. HUD и editor рисуются после blur.

Поддержаны AA Off и SMAA1, где blur выполняется перед SMAA edge detection, а
также FSR Native с blur после upscale. MSAA, SMAA T2x и scaled FSR режимы явно
отклоняются CLI/MCP, поскольку их resolve/history contract пока несовместим.
Validation выявил static binding 5 в обычном SMAA; ошибка исправлена отдельными
normal/blurred SPIR-V variants, чтобы normal path не объявлял неиспользуемый
blurred input.

Typed `render.set_motion_blur` доступен в runtime и editor. State и benchmark
JSON публикуют requested/actual mode, quality, shutter, angle/radius/fade;
отдельный GPU timing — `average_gpu_motion_blur_ms`. Runtime/editor smoke
проверили 30/24 tools.

**Fixture `motion-bars`:** две противоположно движущиеся procedural полосы.
Clean Release серия: AMD Radeon RX 6800, driver 8388961, 1920×1080 IMMEDIATE,
AA Off, GI/day cycle Off, fixed exposure и fixed procedural simulation step
`motion_fixture_fps=60`, который не ограничивает render FPS. Warmup 3 s,
duration 6 s, три rotating runs, editor закрыт и process guard активен.
Off/low/high median FPS `3071.628/2866.402/2757.383`, CPU frame
`0.325560/0.348869/0.362663 ms`, GPU frame
`0.124407/0.192682/0.224867 ms`, postprocess
`0.039817/0.134150/0.166420 ms`, blur interval
`0/0.131721/0.163991 ms`. Delta GPU frame `+0.068275/+0.100460 ms`, FPS
`-6.68%/-10.23%`. Fixture лёгкий и CPU-bound, но CPU/FPS regression нельзя
целиком списать на noise; 1% low остаётся шумным. Blur ranges low
`0.131629..0.131920 ms`, high `0.163904..0.163994 ms` подтверждают разницу
8/16 taps. Clock/power telemetry отсутствует. CLI/JSON
`--motion-fixture-fps 30|60|144` фиксирует simulation step; Debug visual off/high
проверен на fixed30, ASan — на fixed60.

Shader compile + `spirv-val`, Debug/Release/ASan 8/8 CTest, Debug runtime/editor
sync MCP, SMAA1/FSR Native sync, ASan runtime/editor и unsupported CLI rejects
пройдены. Quality validation ограничена procedural structure, sync и screenshots:
temporal supersample reference и production transparent/skinned content пока
отсутствуют. Default остаётся Off; centered/trailing и camera fade являются
настраиваемыми художественными controls, а не обязательными физическими законами.

## E18: интеграция завершена для реализованных путей

Debug `rendering-pipeline-e18-motion2` и ASan `rendering-pipeline-e18-motion`
прошли все пять AA methods (`off`, `smaa`, `msaa2x`, `msaa4x`, `fsr3`), tone
mapper roundtrips, resize/present/mode switches с нулём validation errors и live
GPU allocations. Финальные combined cases прошли в Debug SMAA1 и FSR Native, а
также ASan SMAA1: `subsurface-profiles`, SSS full с 20 active materials, auto
exposure, tetrahedral LUT64 и motion all/high.

После последних C++ изменений Debug и ASan `render_validation_windows.ps1`
`-Startups 1 -Cycles 1 -Scenario rendering-pipeline -RunTag e18-final` повторно
дали по 22 captures, те же пять AA methods, `validation_errors=0` и
`live_gpu_allocations=0`. Summary:
`build/windows-debug/render-validation/rendering-pipeline-e18-final-summary.log`
и `build/windows-asan/render-validation/rendering-pipeline-e18-final-summary.log`.

E18 закрыт для реализованных E01–E10 и E13–E17 paths. E11 particles и E12 SDF
остаются content-deferred по решению пользователя; eye specialization также
content-deferred. Условный G-buffer/deferred path пропущен. Эти ветки не
считаются проверенными текущей матрицей, production content QA не заявляется.

Этап завершён только после реализации API/данных и GPU/CPU пути, procedural
fixtures и reference/fallback, Windows Debug/Release сборок и релевантных CTest,
ASan/resource counters и object-lifetime/synchronization validation. Shader
изменения включают обновлённый SPIR-V Vulkan 1.3 / SPIR-V 1.5 и `spirv-val`.
Perf/default решения имеют сопоставимый A/B, material quality — явно описанный
reference и limitations. `git diff --check` чист; новые build trees, Linux gates,
неавторизованные commit/push не вводятся.

Итоговый статус отделяет **implemented**, **validated**, **default enabled** и
**measured improvement**. Эти состояния не выводятся одно из другого. Настоящие
игровые assets впоследствии расширят coverage и могут изменить budgets/presets,
но движковые подсистемы и их проверяемые contracts разрабатываются сейчас.
