# Дорожная карта превращения движка в юзабельный игровой движок

Этот документ фиксирует полный список подсистем и инструментов, необходимых для
превращения текущего высокопроизводительного Vulkan 1.3 рендерера и редактора
уровней (`vulkan_cube` / `vulkan_editor`) в полноценный игровой движок, а также
порядок их поэтапной реализации в строгом соответствии с инженерным контрактом
[AGENTS.md](../AGENTS.md).

---

## Реализованные этапы

Выполнены все 32 пункта девяти этапов для Windows x64. После сборки регрессионные
тесты каждого дерева можно повторить командой
`ctest --test-dir build/windows-release --output-on-failure`, подставив для
Debug или AddressSanitizer соответственно `build/windows-debug` или `build/windows-asan`.
Работа художника с персонажами и растительностью описана в руководствах
[Character Studio](CHARACTER_SUITE.md) и [Vegetation Studio](VEGETATION_STUDIO.md).

Для сквозной проверки соберите цель `Shipping` в `build/windows-release`,
выполните `vulkan_project_manager.exe --ship <проект> --output <каталог>`
и запустите игру из полученного каталога. На контрольном проекте с экспортами
обеих студий игра загрузила стартовую сцену из 10 узлов и завершилась
с нулём оставшихся GPU-аллокаций.

### Этап 1. Стабильная идентификация и расширяемая компонентная модель (**P0**)
- [x] Персистентные 128-битные `SceneUuid` для всех узлов `SceneDocument`
      (сохранение в `scene_io`, обратная совместимость со старыми JSON-сценами,
      генерация новых UUID при дублировании поддеревьев, `O(1)` поиск по UUID).
- [x] Расширяемая типизированная система компонентов узла сцены без RTTI,
      виртуальных вызовов в hot path и сырого владения памятью (`SparseComponentPool`
      с хранением и инспекцией компонентов отдельно от `SceneNode`).
- [x] Поддержка UUID и компонентов в `vulkan_editor` (`Inspector`) и live `MCP`
      (`editor.scene.find_by_uuid`, чтение/патч компонентов).
- [x] Префабы (`Prefabs`): сохранение поддерева с компонентами в переиспользуемый
      ассет и инстансирование с локальными переопределениями (`overrides`).

### Этап 2. Физика, коллизии и пространственные запросы (**P0**)
- [x] Компонент коллайдера (`ColliderComponent`: `auto_bounds`, `box`, `sphere`,
      `capsule`, флаги `enabled` / `is_trigger`, слои коллизий, local offset/size)
      в `SceneDocument`, `scene_io`, `Inspector`, `Scene View` (3D wireframe) и `MCP`.
- [x] `CollisionWorld`: сборка коллайдеров статической геометрии (`ground`,
      `terrain`, `cube`, `model` bounds и явных `ColliderComponent`) без аллокаций
      в steady-state кадре.
- [x] Пространственные запросы: `raycast`, разрешение контактов капсулы
      (`resolve_capsule`), `sample_supporting_height` и отслеживание триггер-объёмов
      (`TriggerOverlapTracker`: `entered` / `exited`).
- [x] Полноценный `CharacterController`: движение капсулы со скольжением вдоль стен
      (`collide-and-slide`), подъёмом на ступеньки (`step_height`), лимитом угла
      наклона (`max_slope_degrees`) и опорой на произвольные поверхности и рельеф.
- [x] Защита `ThirdPersonCamera` от прохождения сквозь стены и объекты (`spring-arm`
      raycast от точки фокуса к камере).
- [x] Динамические твёрдые тела (`RigidBody`) и кукинг выпуклых/треугольных мешей
      столкновений при импорте моделей.

### Этап 3. Система ввода и звуковой пайплайн (**P0**)
- [x] `Input Action System`: абстракция действий и осей (`Move`, `Jump`, `Run`,
      `Interact`, `Pause`), контексты ввода (`Gameplay`, `UI`, `Cutscene`),
      переназначение клавиш (`config/input_bindings.json`) и нативная поддержка
      геймпадов/вибрации (`XInput`, без нарушения ограничения `SDL3` в `AGENTS.md`).
- [x] Аудио-бэкенд и микшер (`AudioEngine` + `miniaudio` WASAPI): 5 шин громкости
      (`Master`, `Music`, `SFX`, `Voice`, `UI`), brickwall-лимитирование на `Master`,
      плавные двухдековые кроссфейды музыки, синхронизация с `Play / Pause / Stop`
      в редакторе при сохранении отзывчивости `UI` предпрослушивания.
- [x] 3D Spatial Audio: позиционирование источников, equal-power стерео-панорамирование,
      аттенюация по расстоянию (`inverse_clamped`, `linear`, `exponential`), конусы
      направленности, эффект Доплера, сферические зоны реверберации (`Freeverb`) и
      raycast-окклюзия через `CollisionWorld` / `PhysicsWorld`.
- [x] Импорт и потоковый стриминг звуковых ассетов (`.wav`, `.ogg`, `.flac` ->
      `*.vcraudio.json`), компоненты `AudioSource` / `AudioListener` / `ReverbZone`,
      пулы вариаций (`Sound Cues`, `*.vcrcue.json`) с рандомизацией pitch/volume и
      предпрослушивание в `Asset Browser` + панель `Audio & Input`.

### Этап 4. Скелетная анимация и геймплейный фреймворк (**P0**)
- [x] Импорт скелетов, весов вершин и анимационных клипов из glTF/FBX в cooked
      формат (`*.vcrskin`, `*.vcranim` внутри `*.vcrasset.json`); GPU compute
      skinning (`shaders/model_skinning.comp`) с корректными previous/current
      skinning transforms и motion vectors для FSR3 / SMAA / Motion Blur.
- [x] Проигрывание клипов, блендинг (`single_clip`, `1D/2D Blend Spaces`),
      `AnimationStateMachine`, маски слоёв костей (`BoneMask` overrides/additive),
      Root Motion, двухкостный Foot IK, Look-At IK и `Animation Events`
      (маркеры шагов, звуковых cues и хитбоксов через кольцевой лог).
- [x] Жизненный цикл геймплейных систем (`on_start`, `on_fixed_update`,
      `on_update`, `on_trigger_enter`, `on_trigger_exit`, `on_interact`),
      модульный C++ `GameplayBehaviorRegistry` + `GameplayWorld` (поведения
      `patrol`, `collectible`, `door`, `trigger_zone`, `destructible`,
      `health_pack`), редакторский предпросмотр клипов и визуализация скелета
      (`Skeleton`), `Animation & Gameplay` панель, 6 MCP-инструментов и
      сериализация состояния игры (`vcr_game_save` v1, `*.vcrsave.json`, `F5`/`F9`,
      тесты `vcr_animation_gameplay_tests`).

### Этап 5. Внутриигровой UI / HUD и локализация (**P0–P1**)
- [x] Runtime UI-слой для самой игры (отдельно от Dear ImGui редактора): главное
      меню, пауза, настройки графики/звука/управления (включая переназначение
      клавиатуры и геймпада) и интерфейса/языка, HUD, прицел, многовариантные
      диалоги (`*.vcrdlg.json`), субтитры, уведомления и мировые маркеры
      (`UiMarkerComponent`, поведение `dialogue`, `GameUiRuntime`).
- [x] Масштабирование и якоря (`anchors`) под разрешения 720p–4K (`UiScaler`,
      `UiAnchorPreset`, safe area), навигация с клавиатуры и геймпада, таблицы
      локализации (`LocalizationSystem`, `*.vcrloc.json`), SDF-шрифтовые
      атласы (`GameUiFontAtlas`, `*.vcrfont.json` + PNG), панель редактора
      `UI & Localization`, секция Inspector `UI Marker`, 3 MCP-инструмента и
      набор тестов `vcr_game_ui_localization_tests`.

### Этап 6. Редактор кат-сцен (Sequencer / Timeline) и камеры (**P1**)
- [x] Нелинейный `Timeline / Cutscene Sequencer` в `vulkan_editor` со скраббингом (`CutsceneSequence` / `*.vcrseq.json`, `CutsceneRuntimePlayer`, панель `Sequencer` с мультиканальной лентой, редактором кривых и Live Edit-mode превью):
      - дорожки трансформаций, сплайнового движения, света, материалов, параметров аниматора и gameplay-переменных с кривыми интерполяции (`Constant` / `Linear` / `Cubic Hermite` / `Cubic Bezier`);
      - `Camera Cut Track` (смена ракурсов, привязка к `Look-At` цели и рельсовому сплайну, анимация FOV, экспозиции, roll и глубины фокуса `DoF`, letterbox);
      - дорожки запуска скелетных анимаций, реплик/музыки/SFX, локализованных субтитров и событий, CLI `--cutscene` и поведение `cutscene_trigger`.
- [x] 3D-сплайны (`SplineComponent` для `Linear` / `Catmull-Rom` / `Cubic Hermite` с замкнутыми контурами, arc-length параметризацией, поведением `spline_mover`, секцией Inspector `3D Spline` и wireframe/frustum-гизмо в viewport), процедурный `Camera Shake` (`CameraShakeSystem`), плавный блендинг между игровой камерой и кат-сценой (`blend_camera_poses`), 3 MCP-инструмента (`editor.scene.set_spline`, `editor.scene.remove_spline`, `editor.sequencer.control`) и набор тестов `vcr_sequencer_camera_tests`.

### Этап 7. Навигация и искусственный интеллект (**P1**)
- [x] Генерация и запекание `NavMesh` (`*.vcrnav.json`, `bake_navmesh_from_scene` по статической геометрии `ground` / `cube` / `model` / `ColliderComponent` и процедурному `terrain` с учётом уклона, ступеньки, клиренса и радиуса агента), динамические препятствия (`NavObstacleComponent` + динамические/кинематические `RigidBodyComponent`), прыжковые/вертикальные переходы (`OffMeshLinkComponent`: `jump` / `drop` / `climb` / `teleport`) и zero-allocation поиск пути A* + String-Pulling Funnel с локальным рулением (`NavigationRuntime`).
- [x] Управление NPC (`NpcAiComponent`: встроенный тактический FSM `Idle` / `Patrol` / `Investigate` / `Chase` / `Attack` / `Search` / `Return` и сериализуемые деревья поведений `BehaviorTreeAsset` / `*.vcrbt.json` с 20 типами узлов и `AiBlackboard`), система восприятия (`AiPerceptionSystem`: конус зрения с проверкой прямой видимости raycast через `CollisionWorld` / `PhysicsWorld` и слух от звуковых событий с затуханием через препятствия), панель редактора `Navigation & AI`, оверлей вьюпорта `"Nav & AI"`, сериализация в `.vcrsave.json`, валидация `ship_project`, 7 MCP-инструментов (`editor.scene.set_nav_obstacle`, `editor.scene.remove_nav_obstacle`, `editor.scene.set_off_mesh_link`, `editor.scene.remove_off_mesh_link`, `editor.scene.set_npc_ai`, `editor.scene.remove_npc_ai`, `editor.navigation.control`) и набор тестов `vcr_navigation_ai_tests`.
- [x] Автономная студия процедурной растительности `vulkan_vegetation_studio.exe` (6 архетипов и 14 пресетов: трава для `gpu_grass`, кусты, лиственные, хвойные и пальмы; состояния листвы `full`/`sparse`/`bare`; 3D-кисть покраски/обрезки кроны; 5 масштабируемых арен от `hero_single` и `lod_lineup` до `meadow_10000` с живыми метриками LOD/VRAM/overdraw; единый формат экспорта `*.vcrasset.json` / `*.vcrmesh` / `*.vcrmaterial.json` / `*.vcrtexture.json` + sidecar `*.vcrveg.json`; CLI `--cli-generate`, MCP stdio `2025-11-25` с 17 инструментами, smoke `scripts/vegetation_studio_mcp_smoke.ps1` и тесты `vcr_vegetation_studio_tests`).

### Этап 8. Игровые визуальные эффекты и развитие рендера (**P1**)
- [x] В меню `Scene → Add particle emitter` создаётся эмиттер. Inspector задаёт
      цвет, прозрачность, число частиц, время жизни, скорость и размер.
      По умолчанию частицы вычисляются на GPU. Режим `Simulate on CPU`
      использует тот же рисунок движения и отдельный буфер для каждого кадра в полёте.
- [x] Узел `Projected decal` проецирует след попадания, отпечаток обуви, грязь
      или лужу на видимую геометрию по глубине сцены. Проектор можно перемещать,
      вращать и масштабировать. Сцены и префабы сохраняют настройки VFX.
- [x] Опция `--transparency wboit` включает взвешанное смешивание полупрозрачных
      моделей с отдельными буферами цвета и пропускания. Оно доступно при пространственном
      сглаживании в один сэмпл и поддержке независимого смешивания. Для MSAA,
      временной реконструкции, Motion Blur, полного SSS и устройств без независимого смешивания используется
      сортировка от дальних поверхностей к ближним. Её также выбирает `--transparency sorted`.
- [x] Cooked-текстуры принимают нативные блоки BC1–BC7 в двумерном KTX2
      (`storage: "ktx2"`, без supercompression). Shipping сохраняет блоки и mip-уровни
      в пакете. Материал восстанавливает нормаль из двух каналов BC5 UNORM и SNORM.
      Для исходного RGBA доступны кодировщики BC4 и BC7. При подготовке mip-уровней
      листвы для BC7 фильтр учитывает покрытие альфа-маски и разброс нормалей.
      `--texture-budget-mib` задаёт верхнюю границу резидентности. При нехватке
      места загрузчик выбирает меньшие mip-уровни. При смене состава сцены под
      давлением бюджета рендерер освобождает общий кэш и загружает актуальные модели.
- [x] HDR Bloom включается через настройки игры или `--bloom on`. Камера
      Sequencer управляет дистанцией фокуса и диафрагмой для DoF. Погружение
      камеры ниже поверхности океана включает подводное затухание и рефракцию.

### Этап 9. Эргономика миростроения и Shipping-сборка (**P1–P2**)
- [x] Ctrl+щелчок в иерархии или вьюпорте добавляет и снимает выделение.
      Гизмо перемещает группу, `Edit → Snap selection to surface` ставит её узлы
      на коллизионную поверхность. Доступны групповое дублирование и удаление,
      гизмо света, звука и коллайдеров, открытие сцен и `Save As`.
      История Undo / Redo хранит изменения узлов, иерархии и компонентов.
- [x] Панель `Landscape` содержит кисти подъёма и опускания высот, три слоя
      материалов и запекание дорог или рек по выбранному сплайну. Запечённый
      террейн можно открыть для повторной правки. В `Streaming sublevels`
      задаются сцена и радиус загрузки вокруг выбранной точки. В игре и в
      режиме Play загрузка асинхронная. Выгрузка происходит за пределами
      125% радиуса. Поддерживается один уровень вложенности.
- [x] Игровой проект содержит собственный манифест и каталог ресурсов.
      Цель `Shipping` в `windows-release` собирает игровой runtime без Dear ImGui.
      Команда Project Manager `--ship` собирает автономную папку игры с архивом
      cooked-ресурсов. Подготовка ассетов и упаковка используют общий пул Windows.
      Обработчик аварий сохраняет текстовую диагностику и Windows minidump.
