# Проверка rendering pipeline

Новые функции движка проверяются процедурными нагрузками до появления production
content. Эти сцены проверяют корректность и стоимость конкретных путей; выводы о
скорости допустимы только по повторяемым локальным результатам при сопоставимых
условиях. Числа одной GPU не прогнозируют производительность будущей игры.

## RenderingPipeline: до и после изменения

До сборки изменений сохраните Release executable вместе с его `shaders/`, runtime
DLL (включая `amd_fidelityfx_vk.dll`) и assets. Копия должна находиться внутри
`build/windows-release/`, например в `pipeline-baseline/`; отдельный build tree
не создаётся. Нельзя подменять shaders старого executable новыми SPIR-V.

Из рабочей директории benchmark renderer ищет общую папку project `assets/`
раньше assets, упакованных рядом с executable. Поэтому project assets должны
оставаться одинаковыми на протяжении всего A/B, даже если у baseline есть своя
копия. Скрипт проверяет наличие baseline shaders и FidelityFX DLL; соответствие
их версии executable должно быть обеспечено сохранением целого bundle.

```powershell
scripts/benchmark_evolution_windows.ps1 -Suite RenderingPipeline `
    -BaselineExe build/windows-release/pipeline-baseline/vulkan_cube.exe `
    -Repeats 3 -Warmup 5 -Duration 15 -OutputName pipeline-comparison
```

- Статическая сцена: dynamic lighting, GI off/voxel, AA off/SMAA/FSR3 — шесть пар.
- Stress: 100000 процедурных объектов, движущаяся камера, GPU occlusion culling,
  dynamic lighting, GI off, AA off — одна пара.
- В каждой паре идентичные CLI arguments. Время суток 14, day cycle выключен.
  Baseline остаётся с его исходным Reinhard; текущая сборка должна также иметь
  Reinhard по умолчанию. Новые CLI параметры baseline не передаются.

Скрипт требует отдельный `BaselineExe`. Он проверяет фактически записанные
AA, lighting/GI, время суток и параметры stress, чтобы fallback или неприменённая
настройка не превратились в сравнение разных нагрузок.

## ToneMapping: цена выбора tone mapper

```powershell
scripts/benchmark_evolution_windows.ps1 -Suite ToneMapping `
    -Repeats 3 -Warmup 5 -Duration 15 -OutputName tone-mapping-comparison
```

Один текущий Release executable сравнивается с `--tone-mapper reinhard` и
`--tone-mapper gt7` для AA off/SMAA/FSR3. Dynamic lighting и GI off заданы явно:
legacy path не выполняет нужный HDR tone mapping. Exposure фиксирована движком
на 1; отдельного exposure CLI нет. Камера и сцена статичны, время суток 14,
day cycle выключен. Выбранный mapper проверяется по `tone_mapper` в JSON.

## DepthPrepass: стоимость при контролируемом overdraw

```powershell
scripts/benchmark_evolution_windows.ps1 -Suite DepthPrepass `
    -Repeats 3 -Warmup 5 -Duration 15 -OutputName depth-prepass-comparison
```

Один текущий Release executable сравнивается с `--depth-prepass off|partial|full`
на двух процедурных fixtures: `--render-fixture overdraw-1` и `overdraw-16`.
При трёх повторах это **18 запусков**. Fixtures создают соответственно один и
16 слоёв opaque cubes с совпадающими perspective footprints; слои подаются
back-to-front. Это контролируемые low/high overdraw нагрузки, а не прогноз
стоимости будущего content. Полный набор допустимых fixtures включает также
`overdraw-4`, но он не входит в эту ограниченную benchmark серию.

Камера и geometry неподвижны: `--camera-z 12`, без animation flags.
Фиксированы `--visibility-backend cpu --culling frustum --lod off`: frustum
culling включён, occlusion выключен. Lighting dynamic, GI off, время 14,
day-cycle off, AA off, Reinhard, shadow cache full, D32 atlas 2048. Внутри
одной fixture меняется только prepass mode; resolution, geometry и остальные
настройки остаются одинаковыми. Эти 18 запусков не измеряют FSR/GI voxel или
GPU visibility и не заменяют их отдельную проверку.

Порядок mode внутри каждой fixture вращается между повторами:
off/partial/full, partial/full/off, full/off/partial. Это уменьшает связь
конкретного mode с постоянной позицией в серии, но не заменяет контроль clocks,
temperature и фоновой GPU нагрузки. Запускайте серию после сборки fixtures и
интеграционной проверки; сам этот раздел не означает, что они уже пройдены.

Скрипт требует совпадения `depth_prepass_requested/actual` и всей заданной
metadata. GPU frame, geometry, postprocess и prepass timing обязаны быть
доступны и конечны; `null`/отрицательное значение не принимается за нулевую
стоимость. Для off требуется prepass timing = 0, для partial/full на этих
непустых opaque fixtures — >0. Проверяется также, что prepass не превышает
его enclosing geometry interval с учётом допуска округления 0.000001 ms.

**`average_gpu_geometry_ms` уже включает `average_gpu_depth_prepass_ms`.**
Эти значения нельзя складывать: prepass — subset geometry. Основной результат
сравнения — полный GPU frame и enclosing geometry, а отдельный prepass timing
показывает дополнительную depth работу. FPS, 1% low, CPU breakdown и прочие
timings сохраняются в исходном JSON каждого запуска.

Вместо подстановки номинального числа кубов скрипт сохраняет реальные
`source_instances`, `visible_instances`, `triangles`, frustum/occlusion counters
и LOD counts из renderer JSON. Они обязаны совпадать между всеми modes и
повторами одной fixture; пустая geometry или изменение counters останавливает
серию. Эти counters описывают reported geometry, а не сумму draw invocations:
prepass повторно обрабатывает часть geometry, не удваивая автоматически поле
`triangles` в отчёте. Фактический объём дополнительной работы оценивается
отдельным timing и при необходимости GPU capture.

В `build/windows-release/<OutputName>/` сохраняются:

- `DepthPrepass-environment.json`: cases, args, условия и policy порядка;
- `{overdraw-1,overdraw-16}-{off,partial,full}-{1,2,3}.json` и `.log`;
- `DepthPrepass-metrics.json`: timing availability, ключевые timings/FPS,
  фактические geometry counters каждого завершённого запуска, `expected_runs`
  и `complete`. Незавершённый файл не считается полной серией; median/min/max
  и вывод о скорости рассчитываются после завершения всех повторов.

Обе fixtures используют opaque primitive layers. Они не являются отдельным
измерением различия partial/full на imported models; такой workload нужен для
проверки выгоды исключения imported batches из partial policy.

## Условия и интерпретация

Эти suites используют существующие ограничения: Windows Release, 1920×1080,
IMMEDIATE, validation выключена, минимум три повторения после прогрева. Cases
чередуются по повторениям; JSON, логи и environment metadata сохраняются внутри
`build/windows-release/<OutputName>/`. Для каждого запуска suite нужен новый
`OutputName`, чтобы сохранить предыдущие результаты.

Сравнивайте median и разброс CPU/GPU frame time, FPS и 1% low, а также pass timings.
Изменение свыше примерно 3% требует расследования. Скрипт записывает GPU/driver,
CPU, power scheme и фоновые процессы; clocks и температура не фиксируются и
не измеряются, это ограничение результата. Не запускайте GPU benchmarks
параллельно с другими renderer, сборками или GPU-нагрузкой.

Performance runs не заменяют Debug synchronization validation, ASan и визуальную
проверку reverse-Z, disocclusion и HDR цветов при одинаковой камере. Выбор GT7
оценивается по качеству и измеренной цене; его наличие само по себе не означает
ускорение.

## RenderingPipeline: runtime regression

```powershell
scripts/render_validation_windows.ps1 `
    -ExePath build/windows-debug/bin/vulkan_cube.exe `
    -Scenario rendering-pipeline -Startups 3 -Cycles 2
```

Сценарий использует процедурную fixture `--scene-count 128 --scene-walls off
--camera-z 12`, dynamic lighting, GI off, время суток 14 и выключенный day cycle.
Камера остаётся в исходной позе: mouse movement в этом сценарии отключён.
Через существующий F6 цикл проверяются AA off, SMAA и FSR3, если он доступен.
В каждом режиме выполняется Reinhard → GT7 → Reinhard, проверяется фактический
mapper и `depth_convention=reverse-z`. Неверные значения, отсутствующие и лишние
поля MCP запроса должны отклоняться без изменения настроек.

Проверка наличия геометрии привязана к этой fixture при 16:9. У кубов 63–65
передние грани попадают в patches 3×3 около нормализованных координат
`(.45, 2/3)`, `(.50, 2/3)`, `(.55, 2/3)`. Промежутки между кубами находятся
правее на `.025` ширины кадра. Средний RGB каждой грани должен превосходить
соседний промежуток минимум на `(8,15,24)` в кодах PNG 0–255. У контрольных
Reinhard/GT7 captures контраст около `(20,30,43)` / `(19,28,41)`; FSR оставляет
достаточный запас. Значения patches записываются в `*-image-check.json`.
Это контракт конкретного расположения кубов, камеры и освещения, который ловит
sky-only регрессию; его нельзя переносить на произвольную сцену. Изменение fixture
требует пересмотра координат и диапазонов. Он применяется к scene captures,
а lifecycle кадры instancing/gpu-saturation проверяются отдельно как непустые.

Все PNG сохраняются в `build/windows-debug/render-validation/rendering-pipeline/`.
Reinhard и GT7 должны давать разные непустые изображения. Для off/SMAA restoration
сравнивается SHA256 всех декодированных пикселей и требуется точное совпадение.
Для FSR3 результат сравнения записывается в `*-image-check.json`; различия из-за
jitter/history допустимы. Это проверка переключения controls и порядка обработки
цвета, отдельная численная проверка shader против GT7 reference по-прежнему нужна.

Resize, present-mode и scene/instancing/gpu-saturation переходы, repeated shutdown,
synchronization validation и нулевые live allocations остаются обязательными.
У ASan или Release executable выходная папка находится в соответствующем разрешённом
build tree; успешный сценарий не заменяет GPU profiling и качество будущего контента.

## ShadowCache: стоимость и корректность каскадного cache

```powershell
scripts/benchmark_evolution_windows.ps1 -Suite ShadowCache `
    -Repeats 3 -Warmup 5 -Duration 15 -OutputName shadow-cache-comparison
scripts/render_validation_windows.ps1 `
    -ExePath build/windows-debug/bin/vulkan_cube.exe `
    -Scenario shadow-cache -Startups 3 -Cycles 2
```

Benchmark сравнивает один executable с `--shadow-cache full` и `cascade` в
canonical scene и процедурной сцене из 100000 объектов с GPU visibility.
Обе стороны используют движущуюся камеру, AA off, dynamic lighting, GI off,
фиксированное время 14 и выключенный day cycle. Проверяются `cascade_cache`,
`shadow_passes_total`, `shadow_tiles_total`; full должен обновлять четыре tiles
в каждом shadow pass. Все прежние правила прогрева, повторений и metadata действуют.

Runtime regression использует процедурную lighting room с receivers, камерой
`--camera-z 4`, GI off и временем 14. При неподвижной камере проверяются
full → cascade → full и отклонение некорректных MCP payloads. После небольшого
движения камера останавливается; cascade сравнивается с принудительным full при
той же позе. Для off/SMAA требуются идентичные декодированные пиксели, а FSR
history differences записываются отдельно. Сохраняются PNG, camera pose,
`shadow_passes`, `shadow_tiles` и `last_shadow_mask`.

Отдельная ограниченная серия коротких движений вперёд обязана показать хотя бы
один интервал с `passes_delta > 0` и `tiles_delta < 4 * passes_delta` при включённом
cascade cache. Переключений cache внутри измеряемого интервала нет. Если fixture
не упражняет частичное обновление, сценарий сохраняет `*-partial-work.json` и
завершается ошибкой; отсутствие этой проверки не считается успехом оптимизации.

Также проверяются resize, shadow resolution 1024/2048, скачки времени 6/14,
возврат из instancing/gpu-saturation и доступный `test.set_roof` для удаления и
возврата крыши. Runtime MCP не предоставляет редактирование transform; эта
непокрытая проверка явно записывается в `*-invalidation-coverage.json`, отдельно
от проверенной scene invalidation. Завершающие synchronization, lifetime,
allocation и shutdown gates остаются обязательными. Артефакты находятся в
`build/<windows-tree>/render-validation/shadow-cache/`.

## ShadowDepth: startup profiles D32/D16

`render_validation_windows.ps1` принимает `-ShadowDepth auto|d32|d16`; значение
передаётся в startup CLI и проверяется при каждом `debug.get_state`.
`shadow_depth_requested` должен совпадать с запросом, `shadow_depth_actual`
должен быть d32/d16, а для explicit profile — совпадать с ним. Поле
`shadow_linear_filter` обязано быть boolean; `false` означает допустимый nearest
filter, а не отсутствие проверки.

```powershell
scripts/render_validation_windows.ps1 `
    -ExePath build/windows-debug/bin/vulkan_cube.exe `
    -Scenario shadow-cache -ShadowDepth d32 -Startups 3 -Cycles 2
scripts/render_validation_windows.ps1 `
    -ExePath build/windows-debug/bin/vulkan_cube.exe `
    -Scenario shadow-cache -ShadowDepth d16 -Startups 3 -Cycles 2
scripts/benchmark_evolution_windows.ps1 -Suite ShadowDepth `
    -Repeats 3 -Warmup 5 -Duration 15 -OutputName shadow-depth-comparison
```

При `auto` сохраняется прежняя output directory. Explicit profiles добавляют
`-d32` / `-d16`, например `render-validation/shadow-cache-d16/`, поэтому эти
запуски не перезаписывают прежние auto captures/logs. Для shadow-resolution
суффикс добавляется после lighting profile: `shadow-resolution-off-d16/`.

Shadow-cache fixture перед partial probe открывает крышу и проверяет видимый
контраст между освещённым полом и тенью куба. Эта проверка, partial-work evidence
и сравнения cache/full сохраняются для обоих depth profiles.

Benchmark ShadowDepth сравнивает один executable в четырёх cases:
2048/4096 × d32/d16. Внутри пары неизменны resolution, движущаяся камера,
AA off, dynamic lighting, GI off, время 14, выключенный day cycle и full cache.
Проверяются requested/actual depth, resolution, cache mode, boolean filter metadata
и четыре обновлённых tiles на shadow pass. Сравнивайте timing вместе с precision,
bias и качеством выбранной фильтрации: меньшая стоимость D16 сама по себе
не подтверждает эквивалентное качество теней.

## CascadeRange: procedural fixture по диапазону каскадов

```powershell
scripts/render_validation_windows.ps1 `
    -ExePath build/windows-debug/bin/vulkan_cube.exe `
    -Scenario cascade-range -ShadowDepth d32 -Startups 3 -Cycles 2
scripts/render_validation_windows.ps1 `
    -ExePath build/windows-debug/bin/vulkan_cube.exe `
    -Scenario cascade-range -ShadowDepth d16 -Startups 3 -Cycles 2
```

Сценарий выбирает `--render-fixture cascade-cache --camera-z 12` и проверяет
`render.render_fixture=cascade-cache`. Lighting dynamic, GI off, время 14,
day cycle off и начальный shadow cache full фиксированы. Lighting room и
`test.set_roof` здесь не используются; `roof_scene_invalidation_checked=false`
и отсутствие runtime transform проверки честно записываются в coverage JSON.

Используются те же обязательные cache/full и restoration сравнения, AA цикл,
ограниченные движения с partial-work evidence, resize, resolution 1024/2048,
mode-switch и завершающие validation/lifetime gates. Общий дополнительный mouse
move отключён; движение выполняют только контролируемые shadow-cache проверки.
Скачки времени проходят часы 7 и 17 с восстановлением 14 перед продолжением.

Начальная проверка контраста конкретных shadow pixels пока ожидает reference
captures этой fixture; `*-shadow-contrast.json` явно содержит
`initial_shadow_contrast_checked=false` и причину. Непустые PNG, равенство
cache/full при одной позе и обязательное частичное обновление остаются gates,
но сами по себе не подтверждают качество дальних теней. Контракт lighting room
не меняется. Артефакты сохраняются отдельно в
`build/<windows-tree>/render-validation/cascade-range-d32/` и `cascade-range-d16/`.

## DepthPrepass: runtime regression

```powershell
scripts/validate_depth_prepass_windows.ps1 `
    -ExePath build/windows-debug/bin/vulkan_cube.exe
scripts/validate_depth_prepass_windows.ps1 `
    -ExePath build/windows-debug/bin/vulkan_cube.exe `
    -Fixture canonical -VisibilityBackend gpu -Lighting legacy
scripts/validate_depth_prepass_windows.ps1 `
    -ExePath build/windows-debug/bin/vulkan_cube.exe `
    -Fixture cubes -VisibilityBackend cpu -Lighting dynamic
scripts/validate_depth_prepass_windows.ps1 `
    -ExePath build/windows-debug/bin/vulkan_cube.exe `
    -Fixture stress -VisibilityBackend gpu -Lighting dynamic
scripts/validate_depth_prepass_windows.ps1 `
    -ExePath build/windows-debug/bin/vulkan_cube.exe `
    -Fixture overdraw-16 -VisibilityBackend cpu -Lighting dynamic
```

Для проверки совместимости depth prepass с coarse front-to-back sorting:

```powershell
scripts/validate_depth_prepass_windows.ps1 `
    -ExePath build/windows-debug/bin/vulkan_cube.exe `
    -Fixture canonical -VisibilityBackend cpu -FrontToBack coarse
```

`-FrontToBack off|coarse` по умолчанию равен off. Скрипт проверяет requested
sorting, а actual должен быть coarse только при запросе coarse на CPU scene/
gpu-saturation; в instancing и при GPU visibility требуется off.
Coarse добавляет суффикс `-coarse` к папке профиля, например
`depth-prepass-canonical/cpu-dynamic-coarse/`, сохраняя прежние off-артефакты.
Image equality проверяет совместимость off/partial/full prepass внутри выбранного
sorting profile; сравнение off/coarse между startup выполняется отдельно.

Отдельный скрипт по умолчанию выполняет один startup и один cycle для canonical,
CPU visibility и dynamic lighting. Параметры `-Startups` / `-Cycles` увеличивают
число повторений. `-Fixture cubes` задаёт 128 кубов, `stress` — 100000;
обе процедурные сцены используют `--scene-walls off --camera-z 12`.
Доступны также `overdraw-1`, `overdraw-4`, `overdraw-16`: фиксированный
`--render-fixture`, camera-z 12, только CPU visibility, `--culling frustum`
и `--lod off`, чтобы occlusion culling не устранял измеряемый overdraw.
Запрошенный render fixture проверяется через debug state.
Canonical сохраняет authored camera и требует наличия imported model nodes:
`partial` выполняет prepass terrain и primitive/cube batches, пропуская models,
а `full` включает все opaque batches. Скрипт записывает hash canonical JSON,
ждёт завершения загрузок и проверяет фактический visibility backend.

Для каждой нужной fixture запускайте отдельно CPU/GPU × dynamic/legacy.
Например, у второй команды `-Lighting dynamic` проверит canonical/GPU dynamic,
а у первой `-Lighting legacy` — canonical/CPU legacy. Время 14, GI off и
day cycle off фиксированы для обоих lighting profiles; runtime API не переключает
dynamic/legacy, поэтому они проверяются отдельными startup. Запуски выполняются
последовательно. Release не заменяет Debug validation; ASan использует тот же скрипт.

В AA off/SMAA/доступном FSR3 выполняется off → partial → full → off.
Для off/SMAA restoration off→off и все procedural fixtures требуют совпадения
каждого декодированного пикселя с baseline. Только canonical при сравнении разных
prepass modes допускает инженерный бюджет: RGB channel error ≤1 LSB и не более
0,01% пикселей с любым RGB-различием; число допустимых пикселей не округляется
вверх. Alpha обязана совпадать. JSON отдельно записывает `exact_identical`,
`numeric_equivalent`, число отличающихся RGB/alpha pixels, максимальную ошибку,
порог и факт применения допуска. Причина наблюдавшейся разницы не установлена;
этот бюджет не доказывает её связь с interpolation или color quantization.
Исторические failed runs не становятся passed без полного повторного запуска.
Для FSR3 equality записывается с оговоркой о jitter/history, alpha остаётся exact. Все PNG сохраняются
с JSON state, camera и metrics. Некорректные MCP values/types/fields должны
отклоняться без изменения режима или камеры. После смены prepass скрипт ждёт
минимум шесть кадров и требует `latest.gpu_depth_prepass_ms=0` при off,
положительное значение при partial/full; unavailable не считается успехом.

Дополнительно проверяются resize 640×360 → 1280×720 → 640×360,
scene → instancing → gpu-saturation → scene и точное восстановление baseline.
Стабильный gpu-saturation round trip использует iterations=0. Затем отдельно
проверяется реальный compute workload iterations=8 с full/off: его `frame_seed`
зависит от frame serial и меняет цвет, поэтому pixel equality не требуется и
это отмечено в coverage JSON. Общий nonempty-image guard проверяет вариацию
вне HUD; он не заменяет отдельный контракт видимости геометрии/материалов.

Ошибки synchronization/object lifetime/ASan, ненулевые live GPU allocations,
отсутствующий validation layer и нештатный shutdown завершают сценарий ошибкой.
Артефакты разделены по профилям внутри
`build/<windows-tree>/render-validation/depth-prepass-<fixture>/<cpu|gpu>-<dynamic|legacy>/`.
Проверка предназначена для корректности движка; timings этого Debug-сценария
не являются доказательством ускорения. Performance выводы требуют отдельного
повторяемого Release A/B.

## FrontToBack: стоимость coarse sorting

```powershell
scripts/benchmark_evolution_windows.ps1 -Suite FrontToBack `
    -Repeats 3 -Warmup 5 -Duration 15 -OutputName front-to-back-comparison
```

Один текущий Release executable сравнивается с `--front-to-back off|coarse`
на двух нагрузках: **12 запусков** при трёх повторах. Canonical использует
неподвижную authored camera, CPU visibility, frustum culling и LOD on.
Stress задаёт 100000 объектов, scene-walls off, camera-z 12, animate-camera,
CPU visibility, culling none и LOD off. В обоих случаях фиксированы depth prepass
off, dynamic lighting, GI off, AA off, время 14 и day cycle off. Порядок off/coarse
внутри каждой нагрузки чередуется между повторами.

Проверяются requested/actual sorting и одинаковые source instances, visible
instances и triangles внутри каждой нагрузки, включая все повторы. В stress
отключённые culling/LOD сохраняют объём geometry при движении камеры. Запуски
повторяют одну time-based trajectory, но не гарантируют одинаковую позу последнего
кадра. Эта серия измеряет также CPU visibility, packing и sorting: цену
`average_scene_prepare_ms` нужно рассматривать вместе с CPU/GPU frame и FPS.

`FrontToBack-metrics.json` сохраняет каждый результат и median/min/max по
workload/mode для scene preparation, CPU frame, GPU frame, FPS и 1% low.
Поле `complete` становится true только после всех ожидаемых запусков;
неполная серия не является итоговым A/B. Исходные JSON, логи и environment
metadata сохраняются по общим правилам suite. Эти измерения не заменяют
отдельное сравнение качества изображения между off/coarse.

## CpuHierarchy: linear и hierarchy CPU culling

```powershell
scripts/benchmark_evolution_windows.ps1 -Suite CpuHierarchy `
    -Repeats 3 -Warmup 5 -Duration 15 -OutputName cpu-hierarchy-comparison
```

Один текущий Release executable сравнивает `--cpu-culling linear|hierarchy`
для scene-count 16, 1024, 25000 и 100000: **24 запуска** при трёх повторах.
В каждой паре одинаковы CPU visibility, frustum culling, LOD off, scene-walls
off, camera-z 12 и animate-camera. Фиксированы dynamic lighting, GI off,
AA off, depth prepass off, front-to-back off, Reinhard, время 14, day cycle off,
full shadow cache и D32 atlas 2048. Requested/actual CPU culling и остальные
заданные настройки проверяются строго: actual `off` не принимается за
linear/hierarchy. Порядок linear/hierarchy внутри каждой нагрузки чередуется
между повторами.

Камера повторяет одну time-based trajectory, но конечная поза зависит от времени
завершения запуска. Поэтому final visible/frustum-rejected/triangles **не обязаны
совпадать между runs** и не используются как equality gate. Числа visible и
frustum-rejected описывают последний sampled state, а не среднее по кадрам.
Нулевые visible counts допустимы, когда объекты данной нагрузки вне frustum;
это нужно учитывать при интерпретации GPU стоимости.

`CpuHierarchy-metrics.json` сохраняет каждый run и median/min/max для
`average_scene_prepare_ms`, CPU frame, FPS, 1% low, GPU geometry, GPU frame,
visible instances и frustum rejected, сгруппированные по scene-count/mode.
Raw samples дополнительно содержат source instances и triangles. CPU timings
включают culling и подготовку packet; выигрыш оценивается вместе с полным
CPU/GPU frame. `complete=true` требует всех ожидаемых runs. Действуют прежние
правила последовательных запусков, fresh OutputName, environment metadata
и отдельной проверки корректности; данный раздел не означает выполненный benchmark.

## MaterialABI: старый и новый material pipeline

```powershell
scripts/benchmark_evolution_windows.ps1 -Suite MaterialABI `
    -BaselineExe build/windows-release/rendering-pipeline-20260912/hierarchy-runtime/vcr_hierarchy_profile.exe `
    -Repeats 3 -Warmup 5 -Duration 15 -OutputName material-abi-comparison
```

`-BaselineExe` обязателен: сохранённый executable должен иметь свои matching
`shaders/` и `amd_fidelityfx_vk.dll`. After — текущий
`build/windows-release/bin/vulkan_cube.exe`. Bundle и используемые обеими
сторонами scene/model/material assets должны соответствовать своим сборкам и
оставаться неизменными во время A/B. Общие project assets имеют приоритет
перед packaged assets, как описано в разделе RenderingPipeline. Скрипт
проверяет наличие baseline bundle и сохраняет SHA-256 обоих executable;
наличие файлов само по себе не доказывает совместимость их версий.

Три нагрузки × before/after × три повтора дают **18 запусков**:

| Нагрузка | Visibility / LOD | Сцена и камера |
| --- | --- | --- |
| canonical-cpu | CPU, frustum, LOD on | Неизменная canonical сцена, статичная камера |
| canonical-gpu | GPU, frustum, LOD on | Та же canonical сцена, статичная камера |
| stress-cpu | CPU, frustum, LOD off | 100000 объектов, walls off, camera-z 12, animate-camera |

В каждой before/after паре CLI arguments идентичны. Фиксированы CPU culling
requested `linear` (actual `linear` на CPU и `off` на GPU), front-to-back off,
depth prepass off, dynamic lighting, GI off, AA off, Reinhard, время суток 14,
day-cycle off, dynamic shadows on, D32 atlas 2048 и full shadow refresh
(`cascade_cache=false`). Output/render resolution — 1920×1080, Release,
IMMEDIATE. Порядок внутри каждой нагрузки: before/after, after/before,
before/after; повторы ≥3 обязательны.

Контракт материалов — прежний default **IOR 1.5, standard shading** в обеих
сторонах. Отдельного IOR CLI и поля IOR в benchmark JSON нет: скрипт не
объявляет этот параметр проверенным по metadata. Перед запуском необходимо
убедиться, что unchanged assets используют указанные defaults, без новых IOR
overrides или unlit materials. Серия оценивает весь переход material pipeline,
включая runtime instance stride **128→144 bytes (+12.5%)** и соответствующие
shaders. Это не изолированный benchmark дополнительных 16 bytes, bandwidth
или BVH. Whole-inside hierarchy не участвует в culling, поскольку обе стороны
принудительно используют linear; межсерийные GPU результаты CpuHierarchy с
разным material ABI нельзя приписывать только изменению traversal.

Requested/actual настройки и full-measurement percentile scope проверяются
строго. Canonical CPU и canonical GPU отдельно требуют совпадения source,
visible, triangles, frustum/occlusion и LOD counters между всеми before/after
повторами своей нагрузки. Обе fixture должны сохранять непустую геометрию.
Stress повторяет одну time-based trajectory, но не гарантирует одинаковый
набор sampled poses или финальную позу: его final visibility/triangles не
используются как equality gate. Все counters сохраняются. Full-shadow counters
должны давать четыре tiles на pass; static cached frames могут иметь нулевой
shadow timing.

`MaterialABI-metrics.json` содержит все samples и median/min/max отдельно
для каждой workload/стороны: FPS, 1% low, CPU frame, scene preparation,
fence wait, acquire, command record, queue submit, present, GPU frame,
geometry, workload, shadows, postprocess, depth prepass, voxel geometry/light
и visibility. Невыполняемый gpu-saturation workload может иметь `null` timing;
summary сохраняет `null` и число unavailable samples, не подменяя их нулями.
Остальные timings должны быть конечными и доступными; prepass off требует
нулевого prepass timing. Полные исходные JSON и logs сохраняются отдельно,
`complete=true` появляется только после всех ожидаемых запусков.

Скрипт не меняет clocks: их фиксацию, восстановление и telemetry выполняет
внешний Windows workflow. Требуется fresh `OutputName`; результаты не заменяют
image/material correctness validation. Стоимость новой ABI оценивается по
сопоставимым локальным CPU/GPU timings, с учётом меняющейся камеры, CPU overhead,
shadow work и разброса FPS/1% low. Этот раздел описывает подготовленную suite,
а не уже выполненный benchmark.

## CpuPackets: source и индексы вместо копии видимых instances

```powershell
pwsh -NoProfile -File scripts/benchmark_evolution_windows.ps1 -Suite CpuPackets -Repeats 3 -Warmup 5 -Duration 10 -BaselineExe build/windows-release/rendering-pipeline-20260912/material-runtime/vcr_material_profile.exe -OutputName cpu-packets-ab
```

Сохранённый baseline имеет SHA256
`73763DB94CCB2400EE3D9D163D702A00D18795E199E7496C9A334D99B786EA3A`.
Его matching shaders, assets и runtime DLL должны оставаться рядом с exe.
Before использует CPU packets с копией видимых runtime instances, after —
source buffer и индексы видимых объектов. GPU instance ABI **144 bytes**,
точность материалов и default IOR 1.5 / standard shading одинаковы у обеих
сторон. `canonical-gpu` служит контрольной нагрузкой GPU visibility.

Suite разделяет построение cases, guards, порядок и summary с MaterialABI:
canonical CPU / canonical GPU со статичной камерой и stress CPU 100000 с
animate-camera, before/after × 3 = **18 запусков**. Culling — CPU linear
(actual off для GPU), frustum; LOD on у canonical и off у stress. Sorting,
prepass, GI и AA выключены; dynamic lighting, Reinhard, час 14, day-cycle off,
D32 2048 и полный shadow refresh фиксированы. Порядок before/after чередуется
между повторами, CLI внутри пары совпадает. Canonical counters обязаны
совпадать; stress повторяет time-based trajectory без гарантии одинаковой
финальной позы, поэтому его visible/triangle counters не являются equality gate.

`CpuPackets-environment.json` сохраняет отдельный `CostScope` и SHA256 обеих
программ. `CpuPackets-metrics.json` содержит те же 18 CPU/GPU/FPS метрик,
median/min/max, availability и counters, что MaterialABI; действуют те же
strict metadata, nonempty geometry и four-tiles-per-shadow-pass guards.
Измеряется совокупная стоимость CPU packet preparation, upload и draw path.
Bandwidth-причина возможного изменения timings требует отдельного аппаратного
профиля; результат не следует приписывать смене material ABI или BVH.
Clocks контролирует внешний workflow, output directory должен быть новым.
Этот раздел описывает подготовленную suite; GPU benchmark здесь не запускался.

## LocalLights: forward reference и clustered forward

Принятая серия запускается нативно под Windows:

```powershell
pwsh -NoProfile -File scripts/benchmark_local_lights_windows.ps1 -Repeats 3 -Warmup 2 -Duration 4 -Width 1280 -Height 720 -OutputName local-lights-ab-20260913-v3
```

Suite использует procedural fixture `light-swarm`: 1/16/64/256/4096 одинаковых
point/spot lights в layouts `separated` и `overlap`, каждый case сравнивает
`--local-lighting forward` и `clustered`. Фиксируются Windows Release,
1280x720, IMMEDIATE, AA/GI/prepass/LOD off, CPU frustum, Reinhard, dynamic
lighting в 14:00, day cycle off, D32 shadow atlas 2048 и static built-in camera.
Три повтора для 2 layouts x 5 counts x 2 backends дают 60 runs.

До серии и между runs process guard проверяет отсутствие активных
`vulkan_cube`/`vulkan_editor`. GPU clocks устанавливаются внешним workflow на
RX 6800 core 1825 MHz / memory 1000 MHz и после серии восстанавливаются в Normal.
Fresh output обязателен. Приняты только
[v3 summary](C:/GDF/vk_test/build/windows-release/local-lights-ab-20260913-v3/LocalLights-summary.json)
и [environment](C:/GDF/vk_test/build/windows-release/local-lights-ab-20260913-v3/LocalLights-environment.json):
v1 исключён из-за неверного timestamp, v2 остался прерванным exploratory run.

Основные comparison metrics — median/min/max GPU frame, FPS, 1% low и
`gpu_light_culling_ms`. Metadata обязана подтвердить requested/actual backend,
fixture/layout/count, resolution, present mode и отключённые AA/GI. Cluster
statistics сохраняют assignments, max list length, overflow cluster count и
dropped lights. Любой `dropped != 0`, неверный count/backend либо не полный набор
runs делает серию неприемлемой.

Performance suite дополняется correctness gates. Debug forward/clustered
captures для 256 separated и overlap должны совпадать пиксель-в-пиксель; overlap
обязан реально вызвать overflow. Отдельные 4096-light separated/overlap runs
проверяют bounded capacity и `dropped=0`. Windows Debug/Release/ASan CTest,
shader compile + `spirv-val`, Debug synchronization validation и ASan runtime/
editor authoring выполняются отдельно: benchmark с validation off их не заменяет.

`auto` принимает crossover policy, а не обещание, что clustered быстрее для
любого spatial distribution. Текущий порог 64 сохраняет forward для малых сцен;
production captures могут уточнить policy без изменения reference path.

## DisplayMappingE13: histogram exposure и 3D LUT

Принятая native Windows Release серия запускается так:

```powershell
pwsh -NoProfile -File scripts/benchmark_display_mapping_windows.ps1 -Repeats 3 -Warmup 3 -Duration 5 -Width 1920 -Height 1080 -OutputName display-mapping-e13-v1
```

Suite использует статичный procedural `material-spheres`, dynamic lighting в
14:00, GT7, AA/GI/day-cycle/prepass/LOD off и IMMEDIATE. Fixed exposure +
grading off является reference. Отдельные cases включают trilinear/tetrahedral
identity LUT 32³/64³ и auto exposure без LUT. Case order вращается между тремя
повторами. Перед серией и между каждым run process guard проверяет отсутствие
`vulkan_cube` и `vulkan_editor`; первая попытка была отвергнута guard-ом до
создания output, когда обнаружился параллельный editor.

Приняты [summary](C:/GDF/vk_test/build/windows-release/display-mapping-e13-v1/DisplayMapping-summary.json)
и [environment](C:/GDF/vk_test/build/windows-release/display-mapping-e13-v1/DisplayMapping-environment.json).
GPU clocks suite не фиксирует; power scheme — Windows Balanced. Поэтому цифры
используются как локальная непрерывная rotating-order серия, а не как
fixed-clock comparison с другими этапами. Все 18 source JSON подтвердили
Release, 1920x1080, IMMEDIATE, fixture, requested/actual modes и конечные
timings/state.

| Case | Median FPS | Median GPU frame, ms | `gpu_display_ms` | `gpu_exposure_ms` |
| --- | ---: | ---: | ---: | ---: |
| fixed + off | 3540.25 | 0.14505 | 0.08091 | 0 |
| fixed + trilinear 32³ | 3496.81 | 0.14887 | 0.08534 | 0 |
| fixed + tetrahedral 32³ | 3383.59 | 0.15620 | 0.09263 | 0 |
| fixed + trilinear 64³ | 3475.55 | 0.14945 | 0.08575 | 0 |
| fixed + tetrahedral 64³ | 3411.59 | 0.15701 | 0.09348 | 0 |
| auto + off | 2944.33 | 0.19728 | 0.08022 | 0.05333 |

Относительно fixed/off trilinear 32³ добавил 0.00443 ms к display draw и
0.00381 ms к GPU frame; trilinear 64³ — 0.00484/0.00439 ms. Tetrahedral 32³
добавил 0.01172/0.01115 ms, 64³ — 0.01257/0.01196 ms. Разница размера LUT
намного меньше разницы interpolation. Auto histogram/adaptation добавил
0.05333 ms собственного GPU interval и 0.05223 ms к GPU frame. Падение FPS
−16.83% выглядит крупно из-за reference около 0.145 ms GPU frame; абсолютная
цена остаётся 0.053 ms на RX 6800.

Performance suite дополняется correctness gates: все shaders скомпилированы и
прошли `spirv-val`; explicit GPU histogram reference дал exact 256-bin results
в 8 cases и maximum numeric error `4.77e-7`; LUT reference прошёл 144 cases с
maximum error `3.13e-5` для trilinear и `4.15e-8` для tetrahedral. Debug
identity screenshots для off/trilinear32/tetrahedral64 различались максимум на
1 код 8-bit канала. Debug synchronization validation прошла AA off/SMAA/FSR,
resize и mode changes; ASan runtime FSR и editor authoring прошли отдельно.

## Subsurface E14: full-resolution masked separable SSS

Подготовленная native Windows Release серия запускается так:

```powershell
pwsh -NoProfile -File scripts/benchmark_subsurface_windows.ps1 -Repeats 3 -WarmupSeconds 3 -DurationSeconds 6 -AntiAliasing off
```

Suite сравнивает `--subsurface off` и `full` на procedural fixture
`subsurface-profiles` в rotating order, три повтора и шесть runs. Фиксируются
1920×1080, IMMEDIATE, dynamic lighting в 14:00, GI/day-cycle off, fixed exposure
EV 0 и выбранный одинаковый AA mode. Перед каждым run process guard требует
отсутствия `vulkan_cube` и `vulkan_editor`; параллельный editor делает всю серию
неприемлемой, даже если процесс завершился во время одного из замеров.

Каждый source JSON обязан подтвердить Release, разрешение, fixture,
requested/actual mode и IMMEDIATE. Для `off` обязательны `provisioned=false`,
нулевые resident bytes и `average_gpu_subsurface_ms=0`. Для `full` обязательны
provisioned resources, положительный SSS timestamp и 66 355 200 resident bytes
(63.28125 MiB): два дополнительных full-resolution `RGBA16F` target на каждый
из двух frame slots. Summary сохраняет median FPS, GPU frame, geometry,
postprocess, SSS interval и deltas.

Принят финальный [summary](C:/GDF/vk_test/build/windows-release/subsurface-benchmark/summary.json)
от 12:07 после исправления общего 24-pixel kernel-radius cap и перекомпиляции
SPIR-V: AMD Radeon RX 6800, driver 8388961, native Windows Release, 1920×1080
IMMEDIATE, AA off, три повтора, warmup 3 s и measurement 6 s. Process guard
перед каждым run подтвердил отсутствие параллельных renderer/editor процессов.
Clocks и power telemetry не фиксировались; per-run GPU values стабильны, но
серию нельзя напрямую сравнивать с результатами других этапов.

| Mode | Median FPS | GPU frame, ms | Geometry, ms | Postprocess, ms | SSS interval, ms | Resident bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| off | 2661.80 | 0.16136 | 0.12378 | 0.03445 | 0 | 0 |
| full | 2409.03 | 0.22762 | 0.10174 | 0.12416 | 0.09864 | 66 355 200 |

`full` снизил FPS на 9.50% и добавил 0.06626 ms к полному GPU frame. Geometry
изменилась на −0.02204 ms, postprocess — на +0.08971 ms. SSS interval входит в
postprocess, поэтому эти значения не складываются. Изменение geometry между
чередующимися runs не трактуется как ускорение geometry path; для решения важна
end-to-end delta полного кадра. 63.28125 MiB — фактическая дополнительная
resident storage цена после provisioning при этом разрешении.

Performance результат дополнен correctness gates: Debug/Release/ASan прошли
8/8 CTest; Debug synchronization validation прошла dynamic AA off/SMAA/FSR и
legacy AA off, ASan synchronization validation — FSR runtime. Editor MCP smoke
прошёл с allowlist 22 tools. Material GPU reference проверил 30 материалов,
maximum F0 error `3.774273360513e-08` и ноль live allocations. В fixed
1280×720 capture между off/full изменились 18 136 из 921 600 pixels (1.9679%),
maximum absolute RGB difference — 21/5/1; визуальный просмотр не выявил явного
profile/depth bleeding. Это ограниченная fixture-проверка, не pixel equivalence
и не доказательство качества на production коже.

## Forward MSAA E15: quality и performance

Image gate запускается нативно под Windows:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate_msaa_quality_windows.ps1
```

Он снимает `aa-wires` в 640×360 для Off/SMAA1x/SMAA T2x/FSR Native/MSAA2/MSAA4
и Off-reference в 2560×1440. HUD исключается. Reference декодируется из sRGB,
каждый блок 4×4 усредняется в linear light, edge pixels выбираются по
внутриблочной variance. Acceptance требует улучшения T2x и обоих MSAA режимов
относительно Off, MSAA4 не должен уступать MSAA2 больше чем на 2%, а полностью
покрытые MSAA interiors не должны получить drift.

Исторический E15 результат до equal-luma корректировки fixture: 2350 edge и
80 967 interior pixels; edge MAE Off/SMAA/MSAA2/MSAA4 =
`0.075373/0.053197/0.059043/0.036500`. Последний E16 equal-luma результат:
Off/SMAA1x/T2x/FSR Native/MSAA2/MSAA4 =
`0.07364754/0.05203927/0.04802657/0.03433304/0.05764277/0.03548015`;
interior MAE ≈`0/0.00013566/0.00056777/0.00043068/≈0/≈0`.

Performance suite:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File scripts/benchmark_msaa_windows.ps1 `
  -Repeats 3 -WarmupSeconds 3 -DurationSeconds 6 -RenderFixture aa-wires
pwsh -NoProfile -ExecutionPolicy Bypass -File scripts/benchmark_msaa_windows.ps1 `
  -Repeats 3 -WarmupSeconds 3 -DurationSeconds 6 -RenderFixture overdraw-4
```

Windows Release, 1920×1080, IMMEDIATE, dynamic lighting 14:00, GI/day-cycle off,
fixed EV0, SSS off и full 3+6 second windows фиксированы. Порядок шести modes
разворачивается между повторами. Process guard требует отсутствия
`vulkan_cube`/`vulkan_editor` до и после каждого run. Каждый JSON обязан
подтвердить exact requested/actual samples; fallback делает серию ошибочной.
Summary сохраняет FPS/1% low/CPU record/GPU frame/geometry/postprocess, ranges,
MSAA resident bytes и SMAA mode/history metadata. Для T2x JSON обязан подтвердить
`smaa_mode=t2x`, два temporal samples, ненулевую shared resident capacity,
0 incremental bytes и reuse FSR output. Приняты [aa-wires summary](C:/GDF/vk_test/build/windows-release/msaa-benchmark/aa-wires/summary.json)
и [overdraw-4 summary](C:/GDF/vk_test/build/windows-release/msaa-benchmark/overdraw-4/summary.json).

Correctness дополняют Debug sync MCP cases для MSAA2/4 + full depth prepass,
runtime и editor typed switch, resize и отрицательная SSS/MSAA матрица. ASan
повторяет 2×↔4×↔Off и resize. Shader source/SPIR-V проверяются общей
`compile_shaders_windows.ps1` с Vulkan 1.3 / SPIR-V 1.5.

## SMAA E16: detector и T2x acceptance

Color/luma A/B использует equal-luma chromatic boundary. Критический ROI содержит
74 pixels: color MAE `0.0288994`, luma `0.0536648`, то есть luma хуже примерно
в 1.86 раза. Clean Release delta luma относительно color (`+0.0454%` FPS,
`-0.00194 ms` GPU frame, `-0.00230 ms` postprocess) считается шумом. Поэтому
вариант luma удалён из runtime surface; color остаётся единственным/default
detector. Depth detector не добавляется, поскольку same-depth material,
specular/normal-map и shadow edges в depth отсутствуют, а sampled reverse-Z
depth требует ещё resource и threshold contract.

T2x performance измеряется тем же `benchmark_msaa_windows.ps1`, который запускает
rotating order Off, SMAA1x, T2x, FSR Native, MSAA2 и MSAA4. Принятая clean серия:
AMD Radeon RX 6800, driver 8388961, Windows Release, 1920×1080, IMMEDIATE,
3 repeats, warmup 3 s, duration 6 s, editor closed и process guard.

| Fixture / mode | GPU frame, ms | Postprocess, ms |
| --- | ---: | ---: |
| aa-wires / SMAA1x | 0.1169617 | 0.0877499 |
| aa-wires / T2x | 0.1582523 | 0.1178376 |
| aa-wires / MSAA4 | 0.1234626 | 0.0156178 |
| aa-wires / FSR Native, separate | 0.6530913 | 0.6121606 |
| overdraw-4 / SMAA1x | 0.4446806 | 0.1015921 |
| overdraw-4 / T2x | 0.5124975 | 0.1464892 |
| overdraw-4 / MSAA4 | 0.4598950 | 0.0161829 |
| overdraw-4 / FSR Native, separate | 1.0487279 | 0.6794999 |

T2x acceptance зафиксирован как optional: edge MAE лучше SMAA1x примерно на
7.7%, GPU cost выше на `0.04129 ms` (`aa-wires`) и `0.06782 ms`
(`overdraw-4`), shared history равна 31.64 MiB при 1080p и incremental bytes
равны нулю. Он не заменяет MSAA4, который на текущем fixture качественнее и
быстрее, но требует 189.8 MiB multisample memory. FSR Native quality выше, однако
он существенно дороже и измерен отдельной серией.

Correctness gate включает typed runtime/editor switches, history reset и
moving-camera synchronization/screenshot smoke. Он подтверждает отсутствие
выявленных sync/layout ошибок и работоспособность reprojection path, но не
является production motion-quality proof. Disocclusion, animated geometry,
foliage и glossy highlights остаются content QA. S2x/SMAA4 пока отклоняются:
для них нужны sampled stored 2× source, separation/two chains, standard sample
locations и multisample velocity validation.

## Motion blur E17: correctness и performance

Performance suite запускается только из `windows-release` и использует process
guard для `vulkan_cube`/`vulkan_editor`:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File scripts/benchmark_motion_blur_windows.ps1 `
  -Width 1920 -Height 1080 -Runs 3 -WarmupSeconds 3 -DurationSeconds 6 `
  -MotionFixtureFps 60 -OutputName e17-motion-blur-fixed60-20260913a
```

Скрипт чередует Off, low и high на animated `motion-bars`, AA Off, IMMEDIATE,
GI/day cycle Off, fixed exposure и fixed procedural simulation step 60 Hz. Это
не FPS cap: render loop остаётся uncapped. CLI/JSON
`--motion-fixture-fps 30|60|144` позволяет повторять один motion rate независимо
от render FPS. Каждый JSON обязан подтвердить requested/actual mode, quality,
centered shutter, 180° angle, 32 px radius, нулевой camera fade,
`motion_fixture_fps=60` и ненулевой `average_gpu_motion_blur_ms` только для
active modes. Summary сохраняет FPS/1% low, CPU/GPU timings и blur interval.

Принятые условия: AMD Radeon RX 6800, driver 8388961, Windows Release,
1920×1080, три rotating runs, warmup 3 s, duration 6 s, editor закрыт и process
guard активен. Clock/power telemetry отсутствует.

| Mode | Median FPS | CPU frame, ms | GPU frame, ms | Postprocess, ms | Motion blur, ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Off | 3071.628 | 0.325560 | 0.124407 | 0.039817 | 0 |
| Low, 8 taps | 2866.402 | 0.348869 | 0.192682 | 0.134150 | 0.131721 |
| High, 16 taps | 2757.383 | 0.362663 | 0.224867 | 0.166420 | 0.163991 |

GPU frame delta low/high равна `+0.068275/+0.100460 ms`, FPS delta —
`-6.68%/-10.23%`. Fixture очень лёгкий и CPU-bound: 1% low остаётся шумным, но
измеренный рост CPU frame и падение FPS нельзя целиком списать на noise. Blur
ranges low `0.131629..0.131920 ms`, high `0.163904..0.163994 ms` подтверждают
реальную разницу стоимости 8/16 taps. Motion-blur interval входит в postprocess
и не складывается с ним.
[Машинная сводка](C:/GDF/vk_test/build/windows-release/e17-motion-blur-fixed60-20260913a/MotionBlur-summary.json).

Correctness matrix покрывает AA Off/SMAA1 и FSR Native, runtime/editor typed
`render.set_motion_blur`, pause/cut/settings reset, camera/object/all,
centered/trailing, screenshots и explicit rejects для MSAA/T2x/scaled FSR.
Shader compile + `spirv-val`, Debug/Release/ASan 8/8 CTest, Debug runtime/editor
sync MCP, SMAA1/FSR Native sync и ASan runtime/editor прошли. Runtime/editor
inventory содержит 30/24 tools. Validation также проверяет исправление binding 5:
normal и blurred SMAA используют отдельные SPIR-V variants.

Quality acceptance пока структурный: procedural `motion-bars` содержит две
противоположно движущиеся полосы, а sync/screenshots подтверждают прохождение
ресурсов и дают материал для ограниченной визуальной проверки. Temporal
supersample reference, production transparent/skinned geometry и окончательная
художественная оценка отсутствуют. Visual Debug Off/High capture дополнительно
выполнен с fixed30, ASan — с fixed60.

## E18: финальная матрица реализованных путей

Debug `rendering-pipeline-e18-motion2` и ASan `rendering-pipeline-e18-motion`
проверили Off/SMAA/MSAA2/MSAA4/FSR3, tone mapper roundtrips, resize,
present/render-mode switches, validation и live allocation counters. Финальные
combined cases для Debug SMAA1 и FSR Native и ASan SMAA1 одновременно включали
`subsurface-profiles`, full SSS с 20 active materials, auto exposure,
tetrahedral LUT64 и motion all/high.

После последних C++ изменений выполнен повторный gate:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File scripts/render_validation_windows.ps1 `
  -Startups 1 -Cycles 1 -Scenario rendering-pipeline -RunTag e18-final
```

Debug и ASan получили по 22 captures, все пять AA methods,
`validation_errors=0` и `live_gpu_allocations=0`. Logs:
[Debug](C:/GDF/vk_test/build/windows-debug/render-validation/rendering-pipeline-e18-final-summary.log)
и [ASan](C:/GDF/vk_test/build/windows-asan/render-validation/rendering-pipeline-e18-final-summary.log).

E18 завершён для реализованных E01–E10 и E13–E17 paths. E11 particles, E12 SDF
и eye specialization остаются content-deferred; условный G-buffer/deferred path
пропущен. Production content QA эта матрица не заменяет.
