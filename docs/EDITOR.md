# Vulkan Editor

В `Render` panel доступны frustum/occlusion culling, mesh/terrain LOD,
dynamic object shadows и бюджет ошибки LOD. Здесь же показаны текущий
CPU/GPU backend, visible/rejected/LOD counts и GPU timings.
Runtime-архитектура, ограничения и A/B проверки описаны в
[ENGINE_EVOLUTION.md](ENGINE_EVOLUTION.md).

## Почему Dear ImGui, а не Nuklear

Редактор использует Dear ImGui `v1.92.8-docking`, закреплённый в CMake.
Nuklear остаётся хорошим вариантом для компактного игрового HUD, но для
полноценного редактора ему пришлось бы самостоятельно реализовать и
поддерживать production-интеграцию SDL3/Vulkan, docking, несколько системных
окон, clipping больших списков и значительную часть editor widgets.

Dear ImGui выбран из-за официальных SDL3/Vulkan backends, docking branch,
multi-viewport и `ImGuiListClipper`. Transform gizmo использует закреплённое
ядро ImGuizmo `1.10`; sequencer, graph editor и examples не собираются. Поверх
них редактор использует
собственный `EditorUiKit`, чтобы стандартные immediate-mode widgets выглядели
как единый интерфейс движка. Ссылки:

- <https://github.com/ocornut/imgui/tree/v1.92.8-docking>
- <https://github.com/CedricGuillemet/ImGuizmo/tree/1.10>
- <https://github.com/Immediate-Mode-UI/Nuklear/tree/4.13.3>
- <https://github.com/Binaryify/OneDark-Pro>

`vulkan_cube` и `vulkan_editor` используют общий authored-scene renderer
(`VCR_EDITOR_ENABLED=1`); editor application и editor panels входят только
в `vulkan_editor`. ImGui
использует Vulkan 1.3 dynamic rendering. Узкий adapter переводит три legacy
submission-вызова upstream backend в `vkQueueSubmit2`, а его barriers — в
synchronization2, используя только фиксированные stack arrays.

## Визуальный язык EditorUiKit

`src/editor_ui_kit.*` — единая точка определения editor theme и переиспользуемых
компонентов. Цветовая основа следует One Dark Pro: глубокие графитовые surfaces,
мягкий серый текст, синий action accent, cyan для информации, green для
успешного состояния, yellow для предупреждений, red для ошибок и purple как
вторичный фирменный акцент. Цвета палитры переводятся из sRGB в linear до
упаковки ImGui vertex color, потому что основной swapchain использует
`*_SRGB` format.

На Windows редактор загружает системный Segoe UI с Cyrillic glyph range. Linux
использует первый доступный DejaVu Sans, Liberation Sans или Ubuntu Sans;
встроенный scalable ImGui font остаётся последним fallback. Шрифт ОС не
распространяется с приложением и загружается только при создании UI context.

Узнаваемость не зависит от bitmap icon atlas: cube mark и editor icons
отрисовываются компактными `ImDrawList` primitives. UI kit предоставляет:

- quiet/secondary/primary/danger action buttons и icon buttons;
- badges и semantic status colors;
- hierarchy и asset rows с единым selection/hover language;
- двухколоночный property grid и цветные resettable XYZ controls;
- panel/section headers, empty states, viewport badges и metric cards.

Новые editor panels SHOULD использовать эти компоненты вместо локальных
палитр и случайных размеров. Красный и жёлтый MUST оставаться цветами
диагностики, а яркие accents не должны занимать большие фоновые площади:
интерфейс рассчитан на длительную работу. Любой GPU-backed preview/cache должен
оставаться bounded и следовать frame ownership; UI kit сам не владеет Vulkan
objects и не выполняет per-frame filesystem I/O.

## Запуск

```bat
scripts\build_windows.cmd Debug
build\windows-debug\bin\vulkan_editor.exe
```

Release-запуск с автоматической сборкой:

```bat
scripts\run_editor_windows.cmd --no-validation
```

AI authoring через live editor MCP:

```bat
build\windows-debug\bin\vulkan_editor.exe --mcp-stdio --no-validation
```

Поддерживаются общие параметры окна, present mode, validation и AA из
`vulkan_cube`. `--benchmark` редактор намеренно отклоняет: для воспроизводимого
performance benchmark используется изолированный `vulkan_cube`. `--mcp-stdio`
в редакторе включает live authoring profile, описанный в [MCP.md](MCP.md).

## Возможности

- `Scene Hierarchy`: иерархический forest с generational handles,
  работающими chevrons, `Expand all`/`Collapse all`, drag-and-drop reparenting,
  защитой от циклов, фильтром и clipped-отрисовкой.
- `Inspector`: имя, visibility и position/rotation/scale для всех nodes;
  asset path для model
  nodes; PBR base color, metallic, roughness и ambient occlusion для
  cubes/ground. Terrain, sky и sun visibility уже
  управляет renderer, а неподключённые component controls намеренно не
  показываются как работающие.
- `Scene View`: offscreen Vulkan target, изменяемый размер, ray/AABB picking,
  selection bounds и transform gizmo. `W/E/R` выбирают move/rotate/scale,
  доступны World/Local space, постоянный snap и временный snap через `Ctrl`.
  `F` кадрирует выбранный объект, `Ctrl+D` дублирует весь subtree, `Delete`
  удаляет выбранное с возможностью Undo. Fly camera: `RMB + WASD`, `E/Q`,
  `Shift`.
- `Play/Stop` (`F5`): authoring scene сохраняется перед Play, редактирование
  блокируется, а Stop восстанавливает исходный снимок.
- `Save/Open` (`Ctrl+S`/`Ctrl+O`): versioned JSON в
  `assets/scenes/editor_scene.json`, запись через complete temporary file,
  транзакционная загрузка и защита от потери несохранённых изменений при
  Open/Exit. Это каноническая game scene: редактор автоматически открывает её
  при старте, а `vulkan_cube` загружает тот же документ и referenced assets.
- `Game in separate window` (`F6` во время Play): secondary OS window через
  ImGui multi-viewport. Оно использует тот же scene image и отдельный swapchain
  backend; resize окна меняет реальное scene resolution, а fly-camera input
  принимается и в detached viewport.
- `Assets`: sandboxed browser исходного project `assets` (с fallback на
  packaged copy), сортировка, типы, drag payload, ручной refresh, явное
  сообщение о truncation и запрет выхода через `..`/symbolic links.
- `Asset Import`: FBX, OBJ, glTF/GLB, DAE, PLY, STL, 3DS и изображения
  импортируются через отдельное окно или
  внешний drag-and-drop в `Assets`. Model cook создаёт versioned mesh,
  model manifest, отдельные material/texture assets и сохраняет source.
  Публикация транзакционная и не заменяет существующий asset.
- `Add to Scene`: model manifest добавляется кнопкой, двойным кликом,
  context menu или drag-and-drop на `Scene View`. Renderer безопасно проверяет
  cooked данные, загружает immutable GPU buffers и группирует одинаковые
  model/submesh instances. Начальный transform явно центрирует model на земле
  у камеры и равномерно вписывает его в размер 3 world units.
- `Material Editor`: отдельное dockable окно открывает импортированные
  `*.vcrmaterial.json`, редактирует PBR factors и сохраняет complete temporary
  file перед атомарной заменой.
- `Landscape`: создание плоского authored ground и deterministic scatter
  выбранного imported model с count, seed, area и scale range.
- `Undo/Redo`: bounded history на 32 scene transactions, `Ctrl+Z/Ctrl+Y`;
  structural actions, Inspector edits, целый gizmo drag, duplicate и landscape
  batches входят в историю.
- `Renderer`: live CPU/GPU timings, управление AA, tone mapper и
  `Atmospheric fog` — [дымка по расстоянию и высоте](ATMOSPHERIC_FOG.md) с
  настройками цвета и плотности. Tone mapper и все параметры fog являются
  authored-настройками сцены и сохраняются обычными Save/Undo/Redo.

Первая раскладка создаётся автоматически: hierarchy слева, inspector справа,
assets снизу, toolbar сверху и scene view в центре. После этого ImGui хранит
локальную пользовательскую раскладку в игнорируемом `imgui.ini`.

## Масштабирование и lifetime

Scene storage ограничен `100000` nodes. ID содержит index и generation, поэтому
удалённый handle не начинает указывать на новый объект. Связи parent/child и
sibling хранятся индексами; обход, удаление subtree и построение hierarchy
итеративны и не переполняют CPU stack на глубокой сцене.

World transforms вычисляются как точные affine matrices. Поэтому
`non-uniform parent scale + rotated child` сохраняет shear вместо ошибочного
разложения обратно в TRS; GPU scene instance с PBR material занимает 80 bytes.
Gizmo редактирует world matrix и затем проверенно переводит её в локальный
положительный TRS. World-rotation под non-uniform parent может породить shear,
который authored TRS не хранит; в этом частном случае используется ближайший
ортонормальный rotation basis, а Local mode остаётся точным вариантом.

GPU scene data имеет фиксированный верхний предел и отдельный
device-local buffer на каждый `FrameContext`. Изменённый render view копируется
через persistently mapped per-frame upload buffer только после fence этого
frame. В steady state без изменений сцены CPU upload отсутствует; rename
инвалидирует только hierarchy/search cache и не загружает instance buffer.

Scene target один, потому что main UI и secondary window используют одну
graphics queue. Последовательность submission и synchronization2 barrier
защищают переход `color attachment -> sampled -> color attachment`.
Resize viewport coalesced с задержкой 140 ms и ограничен `3840x2160`.

## Осознанные ограничения текущего этапа

- Play имеет корректную границу authoring/runtime snapshot, но gameplay
  scripting, physics и runtime component systems в renderer пока отсутствуют.
- Отдельное окно — окно текущего процесса с зеркалом одной camera, не отдельный
  процесс и не независимо отрендеренная game camera. Main window сейчас
  present-ится до secondary viewport, поэтому FIFO на main window может
  добавить latency detached preview.
- Secondary OS windows включаются только на Windows/X11 и когда graphics queue
  совпадает с present queue. На Wayland остаётся embedded fallback.
- Resize scene target является редкой реконфигурацией и вызывает
  `vkDeviceWaitIdle`, потому что lifetime fences secondary swapchains принадлежат
  upstream backend. Render hot path не вызывает queue/device idle.
- Изменение transform сейчас перестраивает render snapshot за `O(N)` и по одному
  разу обновляет оба per-frame GPU buffers. Иерархия и steady-state рассчитаны
  на 100k nodes, но следующий performance-этап для постоянно меняющейся большой
  сцены — dirty subtrees, stable render slots и coalesced copy ranges.
- Asset enumeration синхронна, но запускается только явным navigation/refresh,
  ограничена 100000 entries и не выполняет filesystem I/O каждый frame.
  Навигация транзакционна: ошибка не смешивает новый path со старым listing.
  Import выполняет постоянный bounded worker и обновляет browser один раз после
  завершения batch. Watcher и общий engine job-system остаются отдельным этапом.
- Imported `*.vcrmesh` рисуется в editor и game scene с material factors из
  `*.vcrmaterial.json`; base-color и normal textures декодируются при первом
  использовании, получают mip chain и bind-ятся отдельным material descriptor
  set. Cooked UV/tangent передаются в dedicated direct/AA model pipelines.
  Подробности:
  [ASSET_IMPORT.md](ASSET_IMPORT.md).
- Save/Open уже сохраняет иерархию и authored properties. Undo/Redo использует
  bounded full-scene snapshots; для очень больших сцен следующий этап — delta
  history с byte budget. Save As, prefabs, component inspector и произвольный
  node graph и multi-selection ещё не реализованы.
  Текущая иерархия намеренно остаётся tree/forest, чтобы ownership и transform
  propagation были однозначными. Context-menu Delete требует подтверждения,
  а клавиша `Delete` выполняет сразу отменяемую через Undo операцию.

Эти ограничения нельзя скрывать при performance-отчёте: FPS `vulkan_editor`
включает UI и не сравнивается с baseline `vulkan_cube`.

## Единая сцена редактора и игры

Оба executable используют `assets/scenes/editor_scene.json` как единственный
источник локальной authored scene. Этот файл и игровые модели исключены из Git;
при отсутствии файла оба приложения открывают встроенную техническую сцену.
Существующий повреждённый файл остаётся ошибкой. Граница игровых и служебных
ресурсов описана в [GAME_CONTENT.md](GAME_CONTENT.md). Обычный цикл разработки:

1. Изменить сцену в `vulkan_editor` и сохранить через `Ctrl+S`.
2. Запустить `scripts\run_windows.cmd --no-validation`: в checkout game сразу
   читает обновлённый project `assets`.
3. Для переносимой Release-сборки выполнить
   `scripts\build_windows.cmd Release`. Target `vcr_runtime_assets` копирует
   scene, model manifests, meshes, materials и textures в
   `build\windows-release\bin\assets` рядом с `vulkan_cube.exe`.

В packaged окружении без project `CMakeLists.txt` game и editor используют
копию `assets` рядом со своими executable, поэтому сборка содержит именно
сохранённую сцену и её зависимости.

Панель renderer также управляет временем суток, скоростью цикла и voxel GI.
Параметры пока не сохраняются в scene-файл; см. [DYNAMIC_LIGHTING.md](DYNAMIC_LIGHTING.md).
