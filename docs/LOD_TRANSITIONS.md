# Переходы между LOD моделей

Статус: E09 реализован и проверен. CPU/GPU пути подключены; Windows
Debug/Release/ASan и CTest 7/7 пройдены. GPU numeric gate прошёл Debug/ASan:
4194304 complementary pixel pairs, 131072 quantized roundtrips, 49620 strict
encoder и source-mask cases, 1024 plain-off pixels; ошибок validation/ASan и
live GPU allocations нет. Debug CPU/GPU runtime coverage дал по 38/38 успешных
сравнений: 26 exact и 12 FSR smoke. Default `off` сохраняется до появления
подходящего authored LOD контента и выбора quality preset.

`--lod-fade off|bayer4|bayer8` и `--lod-fade-seconds 0.05..2` управляют
переходами обычных authored model batches. Начальная длительность 0.25 s.
Те же настройки доступны в editor и MCP `render.set_lod_fade`; пропущенный
`seconds` сохраняет текущую длительность. Некорректный запрос не изменяет
состояние, повторная установка тех же значений не перезапускает переход.
Benchmark/debug различают requested и actual. Instancing, неавторская сцена
и выключенный LOD дают actual `off`.

## Геометрия и coverage

Существующий screen-error/hysteresis выбирает target LOD. Во время перехода
рисуются не более двух его уровней с одинаковыми current/previous object
transforms, material и source ID. Старый уровень получает противоположную
маску нового. Bayer4/8 — упорядоченные матрицы, не blue noise и не temporal
randomization. Пространственный AA не гарантирует скрытия видимого узора.

Маска зависит от render pixel и source index, который стабилен до изменения
scene snapshot. Source index и постоянный authored object ID не смешиваются:
при перестроении/замене assets состояние переходов сбрасывается. Кодирование
в uint: 17 bits source, 12 bits phase, previous role, Bayer8 и active flags.
100001 source records помещаются в 17 bits. Фаза округляется к 4095 уровням;
integer threshold даёт строго дополнительные роли, включая фазы 0 и 1.
FMA residual устраняет неоднозначность округления float product около половин
шага без optional float64/int64. Это проверяется отдельным GPU reference.

Одинаковый GLSL helper используется в color и depth fragment shader. Prepass
сохраняет тот же vertex transform, jitter и `invariant gl_Position`; принудительные
early fragment tests не включены. Для выключенного fade остаётся прежний
vertex-only model prepass; два дополнительных coverage pipelines создаются
заранее и уничтожаются вместе с остальными swapchain pipelines.

Material UV gradients вычисляются до `discard`, затем base/normal/MR maps
используют `textureGrad`; shadow/IBL/GI sampling задаёт explicit LOD.
Это сохраняет определённые derivatives при разном coverage соседних
invocations по [контракту GLSL](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.60.html).

Дополнительность масок гарантирует отсутствие дыр на общей проекции двух
поверхностей. Она не делает разные силуэты одинаковыми и не превращает
переход в geomorph. На области, присутствующей только в одном LOD, покрытие
меняется постепенно. Velocity использует прежний rigid transform текущих
вершин каждого LOD; соответствие вершин между meshes не предполагается.

## Состояние и ownership

CPU хранит bounded `LodFadeState` для каждого source. При неизменной камере
незавершённый переход продолжает обновляться. GPU хранит 16-byte history на
source: previous/current/progress/initialized. Каждым source владеет одна
compute invocation; barrier упорядочивает history между submissions. При
включённом fade и наличии models GPU classification выполняется каждый frame,
чтобы продвигать фазу без readback текущего кадра. Этот расход включается в A/B.

CPU source payload остаётся единственным: максимум 2N visibility tokens и
`144N + 8N` staging bytes. GPU использует существующие три N-sized LOD ranges;
один source может попасть не более одного раза в каждый range. Базовые
draw-group reservations сохранены, уникальный visible counter отделён от
суммы LOD draw emissions. Сумма visible + frustum rejected + occlusion rejected
по-прежнему равна submitted.

На старте переход имеет phase 0. Продолжение использует elapsed seconds.
При развороте previous/current меняются местами, phase становится `1-phase`;
при выборе третьего уровня переход сбрасывается в новый target, поэтому
возможен pop при быстрых трёхсторонних изменениях. Невидимый объект сбрасывает
состояние. Snapshot/assets, смена backend/mode и fade settings тоже сбрасывают
history. Дополнительная camera-cut эвристика: более 25 m за frame, более 45°
поворота или пауза более 250 ms. Это ограниченная эвристика, не универсальный
детектор монтажных склеек.

## Тени и границы реализации

Shadow draws используют полную исходную геометрию LOD0 независимо от camera
visibility; camera dither в light space не переносится. Это устойчивый shadow
reference и явный расход геометрии; отдельный shadow LOD пока не реализован.
Terrain сохраняет дискретный LOD. Blue-noise сравнение, deforming mesh velocity
и camera/shadow geometry blending не входят в текущую реализацию.

Подход к дополнительным маскам и двойной стоимости геометрии согласуется с
[описанием Cesium](https://cesium.com/blog/2022/10/20/smoother-lod-transitions-in-cesium-for-unreal/).
Параметры и результаты другого renderer не считаются доказательством нашей
производительности. Процедурные inputs и штатная загрузка описаны в
`LOD_TRANSITION_FIXTURES.md`; измерения — в `RENDERING_PIPELINE_RESULTS.md`.

На RX 6800 clean fixed-clock A/B для 1024 перекрывающихся authored models
измерил дополнительные 0.0038–0.0059 ms GPU frame. FPS-медиана изменилась
от −0.4% до −3.8% в зависимости от CPU/GPU visibility и паттерна. Это
приемлемая опциональная цена устранения popping, но не основание включать fade
без контента, где проблема видима. Между Bayer4 и Bayer8 устойчивого победителя
не установлено.
