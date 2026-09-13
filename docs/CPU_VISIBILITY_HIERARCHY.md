# CPU-иерархия видимости

`--cpu-culling linear|hierarchy` выбирает реализацию CPU frustum test.
По умолчанию остаётся `linear`. Опция действует вместе с CPU visibility и
включённым frustum culling; GPU visibility, instancing и отключённый frustum
сообщают actual `off`. Requested/actual записываются в debug state и benchmark.
Fallback при недоступной или некорректной иерархии — прежний линейный обход.

## Данные и lifetime

`BoundsHierarchy` владеет contiguous arrays узлов, source indices и копией
world-space Bounds. В дереве нет владеющих pointers или сохранённых внешних
spans. Единственная allocating операция — `prepare_capacity` при инициализации.
Build/refit/query используют подготовленную capacity и локальный стек из
33 элементов. Для renderer capacity 100001 требуется 4897116 bytes хранения
узлов/индексов/bounds, без учёта allocator bookkeeping и небольшого объекта.
Память освобождается с renderer; верхняя граница и `reserved_bytes()` явные.

Build делит source indices по медиане вдоль самой длинной оси центров,
детерминированно разрешая равенство через source index. Лист содержит до
8 объектов. Исходные instance packets, model/LOD batches и stable IDs не
переставляются. Структура сбалансирована по числу объектов; это начальный
вариант без SAH, а не утверждение об оптимальном дереве для каждой сцены.
Общее устройство и компромиссы методов построения описаны в первоисточнике
[PBRT: Bounding Volume Hierarchies](https://pbr-book.org/4ed/Primitives_and_Intersection_Acceleration/Bounding_Volume_Hierarchies).

Cold scene snapshot требует rebuild, включая повторную загрузку assets при
неизменном document revision. Runtime transform changes обновляют world bounds
и делают refit без изменения топологии. Обновление предшествует запросу
видимости. При возвращении с GPU path CPU packet invalidation обновляет bounds.
В steady state без изменений камеры/данных сохраняется прежний packet cache.

Build имеет ожидаемую стоимость O(N log N); refit — O(N). Query очищает всю
visibility mask, поэтому остаётся O(N) даже при почти пустом frustum. Иерархия
сокращает число plane tests, позволяя пропустить целую отклонённую ветвь или
принять целиком ветвь, доказанно находящуюся внутри всех шести плоскостей.
В худшем случае тестируются все листья и добавляются проверки родителей.

## Численная корректность

В пересекающихся с границей leaves вызывается существующий `ViewFrustum::intersects` без изменения
его float-арифметики и допуска −1e−4. Parent enclosure хранит double min/max
с округлением наружу. Parent rejection добавляет запас на округление scalar
distance/radius: одного геометрического containment недостаточно, чтобы
гарантировать совпадение с float reference у самой плоскости. Для полного
принятия используется нижняя support bound каждой плоскости с вычитанием
того же rounding guard. Только когда все шесть нижних границ проходят scalar
допуск, source indices всей ветви получают visibility 1 без leaf plane tests.

При риске переполнения или nonfinite plane parent сохраняет ветвь для leaf
проверки. Invalid bounds, превышение capacity или несовпадающий размер mask
возвращают failure. Query при failure полностью заполняет mask единицами,
после чего renderer выполняет scalar reference. Исправленные данные могут
заново построить ранее invalid tree. Stale bounds не используются.

CPU tests сравнивают полную mask со scalar reference для deterministic random
bounds, нескольких reverse-Z камер, near clip, overlap, shear, refit, огромных
float values, cancellation, invalid input, empty tree, capacity и move.
Проверяется также неизменность prepared capacity. MSVC `nth_element` использует
in-place partition/insertion без heap scratch; алгоритм зависит от выбранной
реализации STL, поэтому это утверждение относится к нативному Windows toolchain.

## Измерения и ограничения

Suite `CpuHierarchy` сравнивает linear/hierarchy на 16, 1024, 25000 и 100000
объектов с движущейся камерой. Измеряется scene preparation вместе с packing,
полный CPU/GPU frame, FPS и 1% low. Траектория задаётся wall time: конечные
позиции и final visibility counts разных прогонов могут отличаться. Совпадение
алгоритмов доказывается тестами и статичными image comparisons, а не равенством
этих конечных counters. Результаты находятся в `RENDERING_PIPELINE_RESULTS.md`.

Explicit SIMD, parallel jobs, SAH и изменение automatic CPU/GPU threshold
требуют следующих измерений. Сейчас не используется optional CPU ISA и не
нужен ISA-dependent fallback; вариант доступен на целевой Windows x64.
