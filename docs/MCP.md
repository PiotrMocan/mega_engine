# MCP control plane

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
| `test` | `run_self_check` |
| `capture` | `screenshot` |
| `render` | `set_mode`, `set_instances`, `set_gpu_workload`, `set_present_mode` |
| `window` | `set_fullscreen`, `set_size`, `set_mouse_capture` |
| `input` | `key`, `mouse_move`, `mouse_button`, `release_all` |
| `engine` | `quit` |

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
