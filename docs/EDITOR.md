# Vulkan Editor

## Почему Dear ImGui, а не Nuklear

Редактор использует Dear ImGui `v1.92.8-docking`, закреплённый в CMake.
Nuklear остаётся хорошим вариантом для компактного игрового HUD, но для
полноценного редактора ему пришлось бы самостоятельно реализовать и
поддерживать production-интеграцию SDL3/Vulkan, docking, несколько системных
окон, clipping больших списков и значительную часть editor widgets.

Dear ImGui выбран из-за официальных SDL3/Vulkan backends, docking branch,
multi-viewport и `ImGuiListClipper`. Визуальный стиль намеренно сделан
плоским и компактным, ближе к Nuklear. Ссылки:

- <https://github.com/ocornut/imgui/tree/v1.92.8-docking>
- <https://github.com/Immediate-Mode-UI/Nuklear/tree/4.13.3>

UI не попадает в benchmark/runtime binary: `vulkan_cube` собирается с
`VCR_EDITOR_ENABLED=0`, а `vulkan_editor` — отдельный executable. ImGui
использует Vulkan 1.3 dynamic rendering. Узкий adapter переводит три legacy
submission-вызова upstream backend в `vkQueueSubmit2`, а его barriers — в
synchronization2, используя только фиксированные stack arrays.

## Запуск

```bat
scripts\build_windows.cmd Debug
build\windows-debug\bin\vulkan_editor.exe
```

Release-запуск с автоматической сборкой:

```bat
scripts\run_editor_windows.cmd --no-validation
```

Поддерживаются общие параметры окна, present mode, validation и AA из
`vulkan_cube`. `--benchmark` и `--mcp-stdio` редактор намеренно отклоняет:
для воспроизводимого benchmark и automation используется изолированный
`vulkan_cube`.

## Возможности

- `Scene Hierarchy`: иерархический forest с generational handles,
  drag-and-drop reparenting, защитой от циклов, фильтром и clipped-отрисовкой.
- `Inspector`: имя и visibility для всех nodes; position/rotation/scale для
  geometry и иерархических folders; PBR base color, metallic, roughness и
  ambient occlusion для cubes/ground. Terrain, sky и sun visibility уже
  управляет renderer, а неподключённые component controls намеренно не
  показываются как работающие.
- `Scene View`: offscreen Vulkan target, изменяемый размер и fly camera
  (`RMB + WASD`, `E/Q`, `Shift`).
- `Play/Stop` (`F5`): authoring scene сохраняется перед Play, редактирование
  блокируется, а Stop восстанавливает исходный снимок.
- `Save/Open` (`Ctrl+S`/`Ctrl+O`): versioned JSON в
  `assets/scenes/editor_scene.json`, запись через complete temporary file,
  транзакционная загрузка и защита от потери несохранённых изменений при
  Open/Exit.
- `Game in separate window` (`F6` во время Play): secondary OS window через
  ImGui multi-viewport. Оно использует тот же scene image и отдельный swapchain
  backend; resize окна меняет реальное scene resolution, а fly-camera input
  принимается и в detached viewport.
- `Assets`: sandboxed browser исходного project `assets` (с fallback на
  packaged copy), сортировка, типы, drag payload, ручной refresh, явное
  сообщение о truncation и запрет выхода через `..`/symbolic links.
- `Renderer`: live CPU/GPU timings и управление AA.

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
  Import pipeline, watcher и постоянный background job-system остаются
  отдельным этапом.
- Save/Open уже сохраняет иерархию и authored properties, но Save As, undo/redo,
  prefabs, component inspector и произвольный node graph ещё не реализованы.
  Текущая иерархия намеренно остаётся tree/forest, чтобы ownership и transform
  propagation были однозначными; Delete требует подтверждения.

Эти ограничения нельзя скрывать при performance-отчёте: FPS `vulkan_editor`
включает UI и не сравнивается с baseline `vulkan_cube`.
