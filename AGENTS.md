# AGENTS.md — инженерный контракт Vulkan Cube Renderer

Этот файл действует для всего репозитория. Любой агент или разработчик обязан
прочитать его до изменения исходников, шейдеров, сборки или benchmark-кода.
**MUST** означает обязательное действие, **MUST NOT** — запрет, **SHOULD** —
правило по умолчанию, от которого можно отступить только с измеренным или
технически доказанным обоснованием.

## 1. Контекст проекта

`vulkan_cube_renderer` — минимальный, но расширяемый высокопроизводительный
real-time renderer и benchmark на C++23/Vulkan 1.3. Визуально сцена проста,
потому что главная цель — измеримый максимум производительности доступного
CPU/GPU, низкая задержка, корректная синхронизация и архитектура, которую можно
развить в полноценный renderer.

Основной стек:

- C++23 без RTTI в основном приложении;
- Vulkan 1.3, dynamic rendering, synchronization2 и `vkQueueSubmit2`;
- SDL3 только для окна, событий и Vulkan surface;
- volk для загрузки Vulkan API;
- Vulkan Memory Allocator для GPU-памяти;
- GLM только для математических типов и операций;
- GLSL, скомпилированный в SPIR-V;
- CMake + Ninja;
- Windows — основная проверенная платформа, Linux — обязательная совместимость.

Текущие режимы:

- `scene`: ground plane, статические block instances и управляемая fly camera;
- `instancing`: до 1 000 000 экземпляров одним indexed instanced draw;
- `gpu-saturation`: регулируемая compute-нагрузка, потребляемая fragment shader.

Текущая архитектура уже содержит:

- два `FrameContext` в полёте;
- per-frame command pool/buffer, fence, semaphores, buffers и descriptors;
- device-local vertex/index/instance buffers со staging upload;
- depth buffer, resize, fullscreen и переключение present mode;
- CPU timings, GPU timestamp queries, FPS, 1% low и JSON benchmark;
- pipeline cache;
- нативные Windows-скрипты в `scripts/`;
- исходники шейдеров в `shaders/` и runtime SPIR-V в `shaders/spv/`.

Локальная контрольная точка от 2026-07-26 на AMD Radeon RX 6800, Release,
1280×720, `scene`, `IMMEDIATE`: около 2941 FPS, CPU frame около 0.340 ms,
GPU frame около 0.078 ms. Сцена содержит два draw calls, 15 instances и
170 triangles. Это не универсальная цель для другого железа, а ориентир для
локального поиска регрессий при неизменных условиях.

## 2. Непереговорные законы

1. **Корректность прежде скорости.** Undefined behavior, data race, use-after-free,
   утечка, неверный barrier или нарушение Vulkan lifetime не могут быть приняты
   даже ради большого прироста FPS.
2. **Ноль утечек.** Подтверждённая утечка CPU-памяти, GPU allocation, Vulkan/SDL
   object, OS handle или thread является release blocker.
3. **Результат измеряется.** «Быстрее», «современнее», «лучше масштабируется» и
   «загружает GPU» нельзя утверждать без повторяемого профилирования.
4. **Максимум производительности, а не максимум сложности.** Используем все
   полезные ядра и современные GPU-возможности, когда выигрыш больше overhead.
5. **Hot path не аллоцирует.** В steady-state render loop запрещены динамические
   аллокации, создание Vulkan objects, filesystem I/O и per-frame logging.
6. **Нет искусственного FPS cap.** По умолчанию выбирается `IMMEDIATE`, затем
   `MAILBOX`, затем гарантированный `FIFO`. Sleep допустим только при 0×0/minimized.
7. **Нет лишней CPU↔GPU синхронизации.** Readback, fence wait и queue idle должны
   быть необходимы, точно измерены и как можно позже потреблены.
8. **Feature detection обязателен.** Любая optional extension/feature имеет
   runtime-проверку и корректный fallback либо понятную диагностическую ошибку.
9. **Каждый шаг остаётся собираемым.** Нельзя оставлять основную ветку в состоянии,
   где Windows Release/Debug не собираются или базовые тесты не проходят.
10. **Отчётность честная.** Отделять измеренные оптимизации от гипотез, указывать
    конфигурацию, bottleneck, компромиссы и непроверенные платформы.

При конфликте требований приоритет таков:

1. безопасность памяти и корректность Vulkan;
2. корректность результата и измерений;
3. отсутствие лишней синхронизации;
4. CPU latency и CPU overhead;
5. GPU utilization и throughput;
6. масштабируемость по ядрам;
7. простота сопровождения;
8. визуальное качество.

## 3. Владение памятью и lifetime

### 3.1 Запрет сырого владения

- MUST NOT использовать `new`, `delete`, `new[]`, `delete[]`, `malloc`,
  `calloc`, `realloc` или `free` в application code.
- MUST NOT хранить владеющий raw pointer. Владение выражается типом.
- CPU-ресурсы MUST использовать автоматический lifetime, стандартные контейнеры,
  `std::unique_ptr` с корректным deleter либо специализированный move-only RAII.
- Vulkan, VMA, SDL и OS resources MUST быть заключены в компактные RAII-типы,
  запрещающие копирование и поддерживающие безопасный move.
- `std::shared_ptr` запрещён в render hot path. Вне hot path он допустим только
  при реальном совместном владении, документированном lifetime и измеренном
  отсутствии неприемлемого atomic overhead.
- Передача владения между потоками выполняется move-only объектом или устойчивым
  handle/index в централизованном registry, а не raw pointer.

### 3.2 Допустимая граница C API

Vulkan и SDL являются C API, поэтому указатели неизбежны в сигнатурах API.
Исключение ограничено границей FFI:

- raw pointer допустим только как краткоживущий аргумент вызова C API;
- non-owning pointer внутри класса допустим временно лишь когда ссылка может быть
  nullable/reseatable, lifetime владельца доказан, а поле явно помечено observer;
- для обязательной зависимости предпочитать reference или
  `std::reference_wrapper`;
- C handle или allocation должен быть обёрнут сразу после успешного создания;
- функции не должны возвращать наружу владеющий raw pointer;
- mapped `void*` инкапсулируется buffer/ring wrapper и не распространяется по
  renderer;
- при изменении существующего класса старые неоднозначные pointer lifetimes
  SHOULD постепенно заменяться явным RAII/observer-представлением.

### 3.3 Правила безопасной памяти

- Все handles и pointers инициализируются null/empty состоянием.
- Частично успешная инициализация обязана корректно откатываться через RAII.
- Порядок уничтожения следует графу зависимостей Vulkan: потребитель до владельца
  памяти/device/instance.
- Размеры buffers, offsets, strides, alignments и `count * sizeof(T)` проверяются
  на integer overflow до allocation/copy/dispatch.
- `std::span`, `std::string_view` и другие views не продлевают lifetime. Их
  владелец должен переживать всю операцию, включая asynchronous job.
- Перед доступом по индексу проверяется инвариант размера на внешней границе;
  Debug-сборка содержит assert для внутренних инвариантов.
- Persistent mapping используется через ограниченный API. Flush/invalidate,
  non-coherent atom size и диапазоны обязаны быть корректны.
- CPU write не может пересекаться с чтением GPU того же региона без frame/ring
  ownership и явной синхронизации.
- GPU resource нельзя уничтожать до завершения последнего submission, который
  может его читать. Предпочитать deferred destruction по fence/timeline value.
- Выход из ошибки, resize, device loss и shutdown проходят тот же leak-free путь,
  что и штатное завершение.
- Любой cache/arena/pool имеет верхнюю границу, reset point и статистику high-water
  mark; «вечный cache» не является оправданием утечки.

## 4. Обязательные анализаторы и поиск утечек

Инструменты дополняют RAII, но не заменяют доказанный lifetime.

### 4.1 Static analysis

Build system MUST предоставить отдельные opt-in presets/targets для:

- `clang-tidy` как минимум с `clang-analyzer-*`, `bugprone-*`,
  `performance-*`, `concurrency-*`, `cppcoreguidelines-owning-memory`,
  `cppcoreguidelines-pro-bounds-*` и lifetime-релевантными checks;
- Clang Static Analyzer через `scan-build`;
- MSVC `/analyze` и C++ Core Check на Windows;
- warnings уровня `/W4` для MSVC и текущий строгий набор GCC/Clang.

На момент фиксации этого контракта sanitizer/analyzer presets ещё не подключены
к CMake. Это известный инфраструктурный долг и обязательная следующая задача,
а не повод считать такие проверки выполненными.

Новые high-confidence предупреждения об утечках, lifetime, bounds, integer
overflow, uninitialized memory и data race блокируют завершение задачи.
Suppression допустим только рядом с кодом, с причиной и ссылкой на false positive.

### 4.2 Dynamic analysis

- Linux memory/lifetime gate: ASan + UBSan + LeakSanitizer.
- Windows memory gate: MSVC или Clang AddressSanitizer; недоступные leak checks
  компенсируются отдельным Linux LSan run и resource counters.
- Изменения многопоточности: отдельный ThreadSanitizer run на Linux. TSan не
  смешивается с ASan в одном бинарнике.
- Для сложных CPU lifetime-проблем допустимы Dr. Memory/Valgrind, но они не
  заменяют sanitizer build.
- Должен существовать repeated startup/shutdown и resize/mode-switch soak test.

### 4.3 Vulkan и GPU memory

- Debug запускается с `VK_LAYER_KHRONOS_validation`, debug utils и именами
  объектов, если слой установлен.
- Включать object-lifetime и synchronization validation для соответствующих
  изменений; performance warnings анализировать, а не бездумно подавлять.
- VMA Debug/recording/statistics режим SHOULD быть доступен отдельной сборкой.
- На shutdown Debug-сборка SHOULD проверять live allocation/object counters и
  печатать VMA statistics при ненулевом остатке.
- Ошибка validation, живой GPU allocation или неуничтоженный Vulkan object
  является blocker.
- Если инструмент отсутствует в окружении, это явно указывается в отчёте.
  Нельзя писать «проверено», если проверка фактически не запускалась.

## 5. Многоядерная и многопоточная архитектура

Цель — максимальный полезный parallelism при минимальной координационной цене.
Количество потоков само по себе не является метрикой успеха.

### 5.1 Базовая модель

- Использовать постоянный worker pool/job system, а не создавать thread на task
  или frame.
- Размер pool выводится из доступной CPU topology и оставляет ресурсы main/render
  submission thread; oversubscription запрещён без измеренного выигрыша.
- Работу дробить на достаточно крупные jobs. Порог параллельного пути выбирается
  benchmark-ом, чтобы маленькая scene не платила за dispatch больше самой работы.
- Frame data передавать immutable frame packet или явно разделённые per-frame
  regions. Изменяемое глобальное состояние запрещено.
- Для временных данных использовать bounded per-thread/per-frame arenas,
  сбрасываемые после подтверждённого завершения frame.
- Горячие структуры проектировать data-oriented: contiguous storage, SoA там,
  где он улучшает SIMD/cache behavior, минимальные pointer chasing и virtual calls.
- Предотвращать false sharing: независимые часто изменяемые counters/state
  разделять по cache line.

### 5.2 Синхронизация потоков

- Data race недопустим. Ownership каждого mutable объекта и момент передачи
  между потоками должны быть очевидны из кода.
- Глобальный mutex в render loop запрещён.
- Предпочитать single-writer ownership, message passing, SPSC/MPSC bounded queues
  и frame-local data вместо общего mutable graph.
- Lock-free структура допускается только с доказанной корректностью, описанной
  memory ordering и измеренным преимуществом. `memory_order_seq_cst` не ставится
  автоматически, более слабый ordering также не выбирается наугад.
- Blocking wait worker-а должен использовать condition variable/semaphore, а не
  spin loop. Короткий bounded spin допустим только после профилирования.
- Vulkan queues требуют external synchronization: у каждой queue один submission
  owner либо узкая явная сериализация. Нельзя одновременно вызывать queue API из
  разных worker threads без этой гарантии.

### 5.3 Параллельная запись команд

- Command pool не разделяется между одновременно работающими threads.
- Масштабируемый путь использует per-thread, per-frame command pools и secondary
  command buffers либо параллельно подготовленные draw packets.
- Primary command buffer собирает готовые chunks в стабильном порядке.
- Для одного куба остаётся прямой single-thread path: параллельная запись здесь
  увеличивает overhead.
- Количество chunks адаптируется к числу объектов и ядер; нельзя создавать тысячи
  микроскопических secondary command buffers.
- Pipeline compilation, shader processing, asset decode/upload planning и scene
  preparation SHOULD выполняться асинхронно, не вызывая first-use stalls.

## 6. Современная renderer-архитектура

### 6.1 Обязательная основа

- Vulkan 1.3 dynamic rendering;
- synchronization2 и точные barriers;
- `vkQueueSubmit2`;
- pipeline cache и prewarming;
- persistently mapped upload rings;
- batched/instanced rendering;
- timestamp queries без forced wait;
- feature/property/limit queries до использования возможности.

Legacy render pass и старый `vkQueueSubmit` не добавлять.

### 6.2 Масштабируемый GPU-driven путь

При росте сцены архитектура SHOULD двигаться к:

- GPU culling, LOD selection и compaction видимых объектов;
- indirect draw buffers и `vkCmdDraw*IndirectCount`;
- минимальному числу state changes и submissions;
- descriptor indexing/bindless только при реальном росте material/resource count;
- buffer device address для подходящих GPU-driven structures;
- async compute/transfer только при наличии подходящих queues и доказанном
  overlap без проигрыша на ownership transfers;
- render/task graph, который выводит lifetime, aliasing и barriers из явных
  producer/consumer зависимостей;
- background pipeline compilation и устойчивому pipeline cache.

Optional современные возможности — mesh shaders, descriptor buffer, graphics
pipeline library, shader objects, device-generated commands — применяются только
после проверки поддержки, с fallback и A/B benchmark. Наличие extension не
является основанием включать её автоматически.

Ray tracing, сложный material system, ECS и editor не добавляются в простой
benchmark без отдельной задачи: они не помогают текущей метрике.

## 7. Законы hot path и GPU throughput

- Ни одной постоянной heap allocation в steady-state frame.
- Не создавать/destroy buffers, images, views, pipelines, descriptors,
  semaphores, fences, command pools или query pools каждый frame.
- Command buffers и descriptor sets переиспользуются безопасно по frame ownership.
- Per-frame данные пишутся в persistently mapped ring/frame region.
- Static geometry и instance data размещаются device-local и загружаются staging.
- Draw calls группируются; один draw на объект запрещён при возможности
  instancing/indirect batching.
- Barriers используют минимальные корректные stage/access masks. `ALL_COMMANDS`
  и queue idle требуют отдельного обоснования.
- CPU не читает query/readback до сигнала соответствующего fence/timeline.
- `vkDeviceWaitIdle()` допустим только на shutdown или при редкой полной
  реконфигурации, где более точная синхронизация действительно невозможна.
- Resource retirement SHOULD перейти на deferred deletion вместо device-wide idle.
- Resize/minimize не делает busy loop и не пересоздаёт весь device.
- Release не содержит validation/debug logging overhead по умолчанию.
- Benchmark не использует sleep; фактический present mode всегда выводится.
- Настройки драйвера, DWM, thermal/power state и resolution учитываются при
  сравнении результатов.

## 8. Методика производительности

Для performance change:

1. Зафиксировать GPU, driver, build type, resolution, present mode, workload,
   clocks/power profile и фоновые процессы.
2. Сделать прогрев.
3. Выполнить минимум три сопоставимых прогона до и после.
4. Сравнить median/разброс, average FPS, 1% low, CPU frame, fence/acquire/record/
   submit/present и GPU pass timings.
5. Для GPU bottleneck проверить hardware counters в RGP, PIX или Nsight.
6. Для CPU bottleneck использовать profiler и allocation counters.
7. Регрессию свыше примерно 3% исследовать; принимать только с явным объяснением
   выигрыша в корректности, latency, памяти или другом целевом режиме.

Нельзя:

- сравнивать Debug с Release;
- сравнивать FIFO с IMMEDIATE;
- менять одновременно workload и оптимизируемый код;
- считать загрузку GPU доказательством эффективной работы;
- переносить результат одной GPU на все устройства;
- оптимизировать только отчёт, пропуская реальную работу.

## 9. Vulkan correctness и synchronization

- Каждый `VkResult`, который может сообщить ошибку, проверяется.
- Required features проверяются до `vkCreateDevice`.
- Descriptor нельзя обновлять, пока GPU может его читать.
- Image layout и buffer ownership выводятся из реального producer/consumer.
- Swapchain корректно обрабатывает resize, minimize, `OUT_OF_DATE` и `SUBOPTIMAL`.
- Depth format/aspect и winding/culling должны быть согласованы с projection.
- Queue family ownership transfer обязателен при exclusive resource между
  разными families; concurrent sharing выбирается осознанно.
- Per-frame fence защищает повторное использование всех ресурсов этого frame.
- Timeline semaphore применять для cross-queue/deferred-lifetime графа, когда он
  упрощает систему; не добавлять его ради самого API.
- Debug object names обязательны для основных Vulkan resources.
- Fatal error печатает операцию и `VkResult`, затем выполняет безопасный cleanup
  настолько, насколько позволяет состояние device.

## 10. Стиль C++ и API

- Предпочитать конкретные типы, композицию и value semantics.
- Владеющие классы move-only; destructors `noexcept`; release идемпотентен.
- Не использовать virtual dispatch, RTTI или exceptions в hot path.
- Recoverable initialization error SHOULD выражаться через `std::expected` либо
  явный result type, не через частично инициализированный object.
- `noexcept` ставится только когда контракт реально гарантирован.
- Не скрывать дорогостоящую работу в accessor/operator/destructor.
- Не использовать global mutable singleton.
- Комментарий объясняет причину, invariant или synchronization, а не повторяет код.
- Не копировать legacy Vulkan tutorial patterns без адаптации к Vulkan 1.3.
- Новая абстракция должна устранять реальную ошибку/lifetime duplication или
  позволять измеримое масштабирование; «на будущее» недостаточно.

## 11. Структура и важные файлы

- `src/application.*` — SDL lifecycle, event loop, CLI orchestration;
- `src/vulkan_context.*` — instance/device/surface/queues/VMA/debug;
- `src/swapchain.*` — swapchain images, views и depth targets;
- `src/renderer.*` — frame submission, resources, pipelines, modes и timings;
- `src/frame_context.hpp` — per-frame owned state;
- `src/gpu_buffer.*` — VMA buffer RAII и mapped writes;
- `src/performance_metrics.*` — bounded metrics и benchmark JSON;
- `shaders/` — shader sources;
- `shaders/spv/` — versioned runtime binaries;
- `scripts/` — нативная Windows configure/build/run/smoke automation;
- `tests/` — CPU-side unit tests.

`build/`, `.tools/`, `benchmark.json` и `pipeline_cache.bin` являются
генерируемыми и не коммитятся.

При изменении shader source обновить соответствующий SPIR-V, выполнить
`spirv-val` и закоммитить source и binary вместе. Целевой environment:
Vulkan 1.3 / SPIR-V 1.5.

## 12. Сборка и проверки

Основная Windows-проверка выполняется нативным MSVC, CMake и Ninja:

```bat
scripts\configure_windows.cmd Debug
scripts\build_windows.cmd Debug
scripts\build_windows.cmd Release
scripts\run_windows.cmd --no-validation
```

Benchmark без искусственного ограничения:

```bat
scripts\run_windows.cmd --benchmark --warmup 5 --duration 15 --present immediate --no-validation
```

Из WSL Windows-скрипт вызывается через `cmd.exe /d /c`; результатом считается
Windows `.exe`, а не случайно собранный Linux-бинарник.

Linux остаётся обязательной целью:

```bash
cmake -S . -B build/linux-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux-release --parallel
ctest --test-dir build/linux-release --output-on-failure
```

Если Vulkan SDK/validation layer, sanitizer или analyzer недоступен, выполнить
остальные проверки и явно назвать пропущенный gate.

## 13. Рабочий процесс агента

1. Проверить `git status` и не затронуть чужие изменения.
2. Найти реальный bottleneck/bug и записать короткий план.
3. Сделать минимально целостное изменение.
4. Проверить lifetime, thread ownership и synchronization вручную.
5. Собрать затронутые Debug и Release конфигурации.
6. Запустить unit tests, relevant validation/analyzers/sanitizers.
7. Для shader change выполнить shader compile + `spirv-val`.
8. Для performance change провести контролируемый A/B benchmark.
9. Выполнить `git diff --check` и просмотреть полный diff.
10. Сообщить результат, цифры, ограничения и непроверенные gates.

Не коммитить и не push-ить без явной просьбы пользователя. Не включать в commit
build outputs, caches, local tools, screenshots или случайные benchmark results.

## 14. Definition of Done

Изменение готово только когда:

- корректно владеет всеми CPU/GPU/OS resources и не оставляет утечек;
- не вносит raw owning pointers или неясный cross-thread lifetime;
- не создаёт data race и не нарушает Vulkan external synchronization;
- собирается соответствующей Windows-конфигурацией;
- проходит CTest и релевантные regression tests;
- проходит доступные static analysis/sanitizer/validation gates;
- не добавляет steady-state allocations или скрытый global lock;
- не ухудшает performance без измеренного и объяснённого компромисса;
- optional GPU path имеет feature check и fallback;
- benchmark утверждения подтверждены сопоставимыми числами;
- documentation/controls/CLI обновлены при изменении поведения;
- `git diff --check` чист;
- отчёт отделяет подтверждённое от предполагаемого.

Любая подтверждённая утечка, use-after-free, out-of-bounds, data race,
double-destroy, dangling view/pointer или validation lifetime error автоматически
означает, что задача не завершена.
