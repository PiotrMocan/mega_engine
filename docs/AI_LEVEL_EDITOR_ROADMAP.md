# AI-first редактор статических игровых уровней

## Цель

Редактор должен позволять человеку и AI-агенту собрать воспроизводимый
статический уровень из импортированных ресурсов, расположить и сгруппировать
объекты, создать базовый ландшафт, распределить растительность, сохранить сцену
и визуально проверить результат. Runtime renderer и gameplay systems не входят
в этот план: editor UI и MCP работают поверх одного authored scene document.

## Что уже было в проекте

- Отдельный `vulkan_editor` с docking UI, hierarchy, inspector, asset browser,
  material editor, Scene View и Play snapshot.
- Versioned JSON-сцена с generational handles, иерархией, TRS, visibility,
  PBR factors и ссылками на cooked model assets.
- Транзакционный importer FBX/OBJ и изображений, persistent background worker,
  bounded queues и sandbox внутри project `assets`.
- Cooked meshes/materials/textures, asynchronous CPU preparation и безопасная
  GPU-публикация модели.
- Runtime MCP для диагностики renderer, ввода и screenshots.

Главный исходный разрыв: MCP не умел читать или изменять authored scene, а UI
не имел undo/redo, массового размещения и специализированного landscape flow.
AI мог только эмулировать ввод в game executable, что не является надёжным API
редактирования уровня.

## Реализованный первый вертикальный срез

- `vulkan_editor --mcp-stdio` запускает отдельный editor-profile MCP. Runtime
  и editor toolsets разделены, чтобы authoring-команды случайно не попали в
  benchmark/game process.
- MCP предоставляет чтение состояния, пагинацию hierarchy и assets, get/create/
  patch/delete/reparent nodes, transform/visibility/PBR factors, save, undo/redo,
  background import, flat ground, deterministic model scatter и PNG screenshot.
- Все изменения сцены выполняются main/editor thread. JSON parsing и response
  serialization остаются на MCP I/O thread, между ними — bounded SPSC queues.
- Появилась `Landscape` panel: создание плоской площадки и распределение
  выбранного imported model по XZ-области с seed, count и scale range.
- Scatter использует фиксированный integer PRNG, поэтому одинаковые параметры
  дают одинаковые transforms независимо от реализации STL. Batch ограничен
  10 000 instances и публикуется в scene как одна undoable операция.
- Undo/Redo ограничен 32 scene snapshots, доступен из `Edit`, `Ctrl+Z/Ctrl+Y`
  и MCP. Это безопасный первый вариант; долгосрочно большие сцены должны перейти
  на command/delta history с memory budget.
- Model source import расширен до FBX, OBJ, glTF, GLB, Collada DAE, PLY, STL и
  3DS. Для glTF внешний buffer копируется в sandboxed import snapshot; этот путь
  покрыт настоящим cook fixture. Image import сохраняет PNG, JPEG, TGA, HDR, BMP,
  DDS и EXR.

## Реализованный второй ручной срез

- Hierarchy chevrons исправлены, добавлены Expand/Collapse All и reveal
  выбранного через UI/MCP.
- Scene View поддерживает picking renderable bounds, single selection,
  selection outline и translate/rotate/scale gizmo в World/Local space со snap.
- `W/E/R`, `F`, `Ctrl+D` и `Delete` дают базовый keyboard workflow; duplicate
  копирует subtree, а gizmo drag coalesced в одну bounded Undo-запись.
- MCP получил `editor.selection.set`, `editor.scene.duplicate_node` и
  `editor.camera.focus_selection`; точный TRS по-прежнему меняется через
  `editor.scene.update_node`.

## Полный набор, необходимый современному level editor

### P0 — надёжное AI authoring

- Стабильная адресация: persistent UUID в scene-файле поверх session-local
  generational handles; поиск по UUID, path, tag и component query.
- Транзакции: `begin/apply/commit/rollback`, optimistic revision precondition,
  idempotency key и dry-run/validate. Один агентский запрос должен давать одну
  запись history независимо от числа изменённых nodes.
- Batch API: create/update/delete/reparent для массива nodes, duplicate,
  replace-asset, align, distribute, snap-to-grid и snap-to-surface.
- Scene validation: отсутствующие assets, invalid transforms, cycles, overlaps,
  out-of-bounds, duplicate singleton components, triangle/texture budgets.
- MCP resources: компактный scene summary, paged hierarchy, asset catalog,
  import jobs, validation report и thumbnails; change notifications вместо
  постоянного polling.
- Защита совместной работы: revision conflicts между UI и агентом, понятный
  журнал операций и возможность preview diff до commit.
- Отдельный headless editor/cooker MCP для CI и массовых офлайн-операций без
  Vulkan window; live editor MCP остаётся для визуальной проверки.

### P0 — базовое ручное редактирование

- Полный transform gizmo: translate/rotate/scale, local/world, pivot modes,
  multi-select, box select, duplicate, focus selected и numeric precision.
- Command/delta Undo/Redo с coalescing drag, memory budget, named transactions и
  сохранением selection.
- Save As, recent scenes, autosave/recovery, dirty diff и crash-safe backups.
- Collections/layers, lock/hide/isolate, tags, search query и hierarchy virtual
  list для 100k+ nodes.
- Asset thumbnails, favorites, tags, dependency view, reimport, move/rename с
  reference fix-up и missing-reference repair.

### P1 — импорт и asset pipeline

- Presets единиц/оси/scale, normals/tangents, material policy, mesh merge/split,
  LOD generation и collision proxy; preview до публикации.
- Reimport по source fingerprint с сохранением authored references и явным
  diff результата cook.
- Расширение форматов по доказанным сценариям: USD/USDZ, Alembic и CAD только
  отдельными optional import modules; animation/skinning — когда runtime начнёт
  их потреблять. До объявления формата стабильным нужны representative cook
  fixtures, включая external dependencies и повреждённые файлы.
- Texture pipeline: WebP/KTX2/BasisU, channel packing, color-space override,
  normal convention, resize/mips/compression presets и environment maps.
- Async thumbnails и metadata index. Filesystem watcher только через bounded
  event coalescing, без I/O в steady-state frame.
- Prefabs/variants с overrides, nested prefabs, unpack и broken-link repair.

### P1 — landscape и vegetation

- Heightfield asset и terrain component вместо одного global procedural terrain:
  create/import/export heightmap, resolution/extent, tiled storage и LOD metadata.
- Sculpt brushes: raise/lower, smooth, flatten, terrace, noise и erosion; stroke
  является одной undoable transaction.
- Paintable terrain layers, splat/weight maps, slope/height rules и material
  preview. Editor data отделяется от runtime cooked terrain.
- Surface queries/raycast для snap и scatter. Placement учитывает реальную
  высоту и normal, ограничения slope/height, exclusion masks и minimum spacing.
- Vegetation scatter layers с несколькими weighted assets, density map, seed,
  scale/yaw/tilt variation, erase/reseed и bake/unbake. Сначала authored nodes,
  затем отдельный compact instance asset для сотен тысяч растений.
- Roads/splines/water volumes относятся к следующему уровню landscape tools и
  не требуют изменения renderer до появления согласованного authored schema.

### P1 — визуальная проверка агентом

- Screenshot не только всего окна, но и чистого Scene View, asset thumbnail и
  selection mask; deterministic camera bookmarks и frame-settled signal.
- Picking API: screen ray, object under cursor, bounds и world position.
- Diagnostics overlay и MCP report: missing assets, hidden/culled counts,
  streaming state, scene bounds и screenshot metadata.
- Golden-scene smoke tests с image comparison tolerance, отдельно от renderer
  benchmark и без ложных заявлений о performance.

### P2 — production ergonomics

- Multi-scene/sublevel composition, streaming cells и world partition metadata.
- Navigation/collision authoring, light probes/reflection captures и audio zones
  как editor components; runtime подключается отдельными задачами.
- Plugin/import extension API, custom inspectors и schema migration tools.
- Role-based dangerous-operation policy для внешних MCP clients, audit log и
  workspace allowlist для source imports.
- Localization, accessibility, DPI/multi-monitor polish и keyboard-only flows.

## Рекомендуемая последовательность разработки

1. Persistent UUID, batch transactions, revision preconditions и delta history.
2. Multi-select + gizmo + snap/raycast; те же операции сразу публиковать в MCP.
3. Asset index, thumbnails, reimport jobs и dependency-safe rename/move.
4. Heightfield schema и flat/imported terrain tools без изменения runtime path.
5. Brush transactions и scatter layers с surface constraints.
6. Headless editor MCP, deterministic validation и golden-scene tests.
7. Prefabs, sublevels и production-scale instance baking.

Каждый этап сохраняет правило: UI и MCP вызывают один editor-core command API,
а не реализуют две разные версии операции. Renderer получает только готовый
immutable scene snapshot и не становится владельцем editor state.
