# MCP control plane

## Editor authoring profile

`vulkan_editor` использует тот же bounded stdio transport, но публикует отдельный
набор authoring tools:

```bat
build\windows-debug\bin\vulkan_editor.exe --mcp-stdio --no-validation
```

| Toolset | Инструменты |
| --- | --- |
| `editor` | `get_state` |
| `editor.selection` | `set` |
| `editor.scene` | `list_nodes`, `get_node`, `create_node`, `update_node`, `delete_node`, `reparent_node`, `duplicate_node`, `save` |
| `editor.camera` | `focus_selection` |
| `editor.history` | `undo`, `redo` |
| `editor.assets` | `list`, `import` |
| `editor.landscape` | `create_flat_ground`, `scatter_model` |
| `render` | `set_exposure`, `set_color_grading`, `set_subsurface`, `set_motion_blur`, `set_anti_aliasing` |
| `capture` | `screenshot` |
| `engine` | `quit` |

Hierarchy и asset listing пагинируются (`offset`, `limit`, максимум 1000).
Node handles содержат `index` и `generation`; после delete/undo/redo старый
handle нужно считать недействительным и снова получить через list/get. Import
ставится в background queue, а его состояние возвращает `editor.get_state`.
`create_node`/`update_node` принимают transform и visibility; для cube/ground
также доступны `base_color`, `metallic`, `roughness` и `ambient_occlusion`.
`scatter_model` принимает cooked `*.vcrasset.json`, count до 10 000, seed,
центр/размер XZ-области и scale range. Повторный вызов с теми же параметрами
создаёт те же transforms, но новые nodes.
`selection.set` принимает generational handle либо `null`, раскрывает родителей
выбранного node в UI и отражается в `editor.get_state`. `duplicate_node`
копирует весь subtree одной Undo-транзакцией и выбирает новую root-копию;
`camera.focus_selection` кадрирует текущий выбор без изменения scene document.
Точный эквивалент ручного gizmo для AI — `editor.scene.update_node` с
`translation`, `rotation_degrees` и/или `scale`.

Нативный smoke editor-profile:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File `
  scripts\editor_mcp_smoke.ps1 `
  -ExePath build\windows-debug\bin\vulkan_editor.exe
```

Editor MCP изменяет только текущий authoring document. Запись canonical JSON
происходит исключительно через явный `editor.scene.save`.

## Runtime profile

`vulkan_cube` может работать как локальный MCP stdio server. Режим запускается
явно, поэтому обычный renderer и benchmark не получают фонового потока, JSON
parser, queue polling или screenshot usage flag:

```bat
build\windows-release\bin\vulkan_cube.exe --mcp-stdio --no-validation
```

MCP-клиент должен запускать этот процесс как stdio child process. В
`--mcp-stdio` stdout зарезервирован только для newline-delimited JSON-RPC,
диагностика движка перенаправляется в stderr.

Пример общей конфигурации клиента (точное имя корневого поля зависит от
конкретного MCP host):

```json
{
  "mcpServers": {
    "vulkan_cube": {
      "command": "C:\\GDF\\vk_test\\build\\windows-release\\bin\\vulkan_cube.exe",
      "args": ["--mcp-stdio", "--no-validation"]
    }
  }
}
```

## Toolsets

Имена инструментов сгруппированы стабильными тематическими префиксами:

| Toolset | Инструменты |
| --- | --- |
| `debug` | `get_capabilities`, `get_state`, `get_metrics`, `reset_metrics` |
| `test` | `run_self_check`, `set_roof` (только `--lighting-room`) |
| `lighting` | `set` (hour, cycle, gi) |
| `capture` | `screenshot` |
| `render` | `set_mode`, `set_instances`, `set_gpu_workload`, `set_present_mode`, `set_shadow_resolution`, `set_shadow_cache`, `set_tone_mapper`, `set_exposure`, `set_color_grading`, `set_subsurface`, `set_motion_blur`, `set_anti_aliasing`, `set_depth_prepass`, `set_lod_fade` |
| `window` | `set_fullscreen`, `set_size`, `set_mouse_capture` |
| `input` | `key`, `mouse_move`, `mouse_button`, `release_all` |
| `engine` | `quit` |

Динамические сутки, GI, счётчики и проверочная комната описаны в [DYNAMIC_LIGHTING.md](DYNAMIC_LIGHTING.md).

`tools/list` возвращает JSON Schema и risk annotations для каждого инструмента.
Результаты содержат одновременно текстовый JSON и `structuredContent`.
`capture.screenshot` дополнительно возвращает MCP `ImageContent` с PNG.

### Ввод

MCP input является детерминированным engine-local вводом. Он проходит через тот
же action path, что SDL keyboard/mouse events, но не посылает глобальные события
в Windows/Linux и не может управлять другими приложениями.

Для движения удобно использовать пары вызовов:

```text
input.key {"key":"w","action":"down"}
input.mouse_move {"delta_x":120,"delta_y":-25}
input.key {"key":"w","action":"up"}
```

`press` создаёт однокадровый импульс для movement keys и одно срабатывание для
action keys. MCP-вызов `escape` безопасно отпускает mouse capture; завершение
процесса выполняется явно через `engine.quit`, чтобы JSON-RPC ответ гарантированно
успел уйти клиенту.

Для сглаживания доступны те же engine-local клавиши, что и в окне: `f6`
переключает `off`/SMAA/MSAA2/MSAA4/FSR 3, `f7` понижает, а `f8` повышает
качество SMAA/FSR. Typed `render.set_anti_aliasing` принимает `off`, `smaa`,
`fsr3`, `msaa2x` или `msaa4x`. Для `method: "smaa"` optional поле
`smaa_mode` принимает `"1x"` или `"t2x"`; с другим методом это поле считается
ошибкой. Одинаковая typed schema действует в runtime и editor profiles:

```text
render.set_anti_aliasing {"method":"smaa","smaa_mode":"t2x"}
render.set_anti_aliasing {"method":"smaa","smaa_mode":"1x"}
```

`debug.get_capabilities` публикует `anti_aliasing.smaa_modes`, support FSR/MSAA,
formats и resolve policy. Runtime `debug.get_state` и editor `editor.get_state`
возвращают `smaa_mode`, `smaa_temporal`, `smaa_history_valid`,
`smaa_temporal_phase`, `smaa_history_resident_bytes`, history width/height,
`smaa_history_incremental_bytes` и `smaa_history_reuses_fsr_output`. При 1080p
T2x сообщает 31.64 MiB общей resident history capacity и 0 incremental bytes,
поскольку использует два per-frame `RGBA16F` FSR outputs. Benchmark JSON также
содержит `smaa_mode`, `smaa_temporal_samples`, `smaa_history_resident_bytes`,
`smaa_history_incremental_bytes` и `smaa_history_reuses_fsr_output`.
Full SSS и MSAA взаимоисключающие.

Typed `render.set_motion_blur` атомарно принимает все семь полей:

```text
render.set_motion_blur {"mode":"all","quality":"high","shutter":"centered","angle_degrees":180,"maximum_radius_pixels":32,"camera_fade_start":0,"camera_fade_end":0}
```

`mode` — `off|camera|objects|all`, `quality` — `low|high` (8/16 taps),
`shutter` — `centered|trailing`; angle ограничен 0–360°, radius 1–64 px.
Camera fade выключается двумя нулями либо требует `start < end` в диапазоне
0–10000. Runtime `debug.get_state` и editor `editor.get_state` возвращают
`motion_blur.mode_requested`, `mode_actual`, quality, shutter, angle, radius и
fade. Benchmark JSON дублирует requested/actual settings и публикует отдельный
`average_gpu_motion_blur_ms`.

Active blur поддерживает AA Off/SMAA1 и FSR Native. Для SMAA он выполняется до
edge detection, для FSR Native — после upscale. MSAA, SMAA T2x и scaled FSR
возвращают ошибку совместимости. HUD/editor компонуются после blur. Пауза больше
250 ms, camera cut или смена настроек сбрасывают temporal state. Runtime/editor
smoke inventories после E17 содержат 30/24 tools.

### Screenshot

Захват выполняется без `vkDeviceWaitIdle()`:

1. capability surface/format проверяется до добавления
   `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`;
2. следующий отрисованный swapchain image копируется в host-visible readback
   buffer;
3. CPU читает buffer только после fence соответствующего `FrameContext`;
4. BGRA при необходимости преобразуется в RGBA;
5. PNG и base64 кодируются в MCP I/O thread.

Одновременно разрешён один capture. Максимальный raw readback — 256 MiB.
Minimized/0×0 window и surface без transfer-source support возвращают tool
error, а не пытаются использовать неподдерживаемый Vulkan path.

Нативный Windows smoke проверяет handshake, discovery, runtime self-check,
реальный GPU capture, PNG и graceful shutdown:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\mcp_smoke.ps1 `
  -ExePath build\windows-debug\bin\vulkan_cube.exe -Validation
```

Параметр `-ScreenshotPath capture.png` опционально сохраняет полученный через
MCP PNG для визуальной проверки; такой локальный файл не коммитится.

## Архитектурные гарантии

- MCP выключен по умолчанию; swapchain transfer-source usage и MCP queue polling
  в обычный render path не добавляются.
- JSON parsing, schema generation, PNG и base64 не выполняются в render thread.
- Между I/O и main/render thread находятся bounded SPSC queues на 64 элемента.
- В main loop нет mutex, blocking wait или steady-state allocation от MCP.
- Все Vulkan/window mutations выполняются владельцем — main/render thread.
- Queue operations используют release/acquire ordering и single-writer
  ownership.
- Закрытие stdin штатно завершает engine; `engine.quit` сначала отправляет tool
  result и только затем останавливает main loop.
- Максимальный входящий JSON-RPC message ограничен 1 MiB.

## Совместимость

Сервер реализует стабильную ревизию MCP `2025-11-25`: `initialize`,
`notifications/initialized`, `ping`, `tools/list`, `tools/call` и stdio
transport. На 2026-07-26 ревизия `2026-07-28` остаётся release candidate/draft,
поэтому stateless `server/discover` пока намеренно не объявлен.

## Расширение

Новая операция добавляется как типизированная команда:

1. добавить `McpCommandType` и bounded payload в `mcp_control.hpp`;
2. объявить tool и JSON Schema в `mcp_server.cpp`;
3. валидировать JSON на I/O thread и преобразовать его в `McpCommand`;
4. выполнить команду в `Application::execute_mcp_command`;
5. вернуть typed/structured result и добавить regression test.

Нельзя передавать в render thread произвольный JSON, raw owning pointer или
callback с неочевидным lifetime. Долгие операции должны получить отдельный
bounded async job/task path, а не блокировать кадр.
