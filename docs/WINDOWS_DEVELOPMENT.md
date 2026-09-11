# Разработка под Windows x64

Команды выполняются из корня репозитория в PowerShell или cmd, нативно под
Windows. Скрипты сами активируют MSVC x64; отдельный Developer Prompt для
сборки не нужен.

## Инструменты

- Visual Studio 2022 с C++ x64, Windows SDK и MSVC AddressSanitizer.
- CMake 3.22 или новее и Git в PATH.
- Ninja: `scripts/configure_windows.cmd` использует `.tools/ninja.exe`
  и скачивает версию 1.12.1, если файла нет.
- Vulkan SDK: `VULKAN_SDK` должен указывать на существующий каталог SDK.
  Нужны `Bin/glslangValidator.exe`, `Bin/spirv-val.exe` и зарегистрированный
  `VK_LAYER_KHRONOS_validation`.
- Драйвер GPU с поддержкой требуемых Vulkan 1.3 features.

SDL, Vulkan Headers, volk, VMA и остальные зависимости получает CMake.
Первичная конфигурация требует доступа к GitHub.

## Сборка и CPU-тесты

```powershell
.\scripts\configure_windows.cmd Debug
.\scripts\configure_windows.cmd Release
.\scripts\configure_windows.cmd ASan
.\scripts\build_windows.cmd Debug
.\scripts\build_windows.cmd Release
.\scripts\build_windows.cmd ASan
```

Каждый `build_windows.cmd` после сборки запускает CTest. Допустимы только
`build/windows-debug`, `build/windows-release` и `build/windows-asan`.
ASan использует `RelWithDebInfo` и `/fsanitize=address` для C++.
Это не означает инструментацию всех сторонних C-библиотек и FidelityFX DLL.

## Запуск и GPU-проверки

```powershell
.\scripts\run_windows.cmd --no-validation
.\scripts\run_editor_windows.cmd
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\mcp_smoke.ps1 -ExePath "$PWD\build\windows-debug\bin\vulkan_cube.exe" -Validation
```

MCP smoke проверяет протокол, runtime self-check, получение PNG и штатный выход.
Для запуска ASan вручную сначала активируйте MSVC через `vcvars64.bat` в cmd,
чтобы ASan runtime DLL была доступна в PATH. Сборочный скрипт уже делает это
для своих тестов.

Для расширенной проверки синхронизации в PowerShell перед запуском:

```powershell
$env:VK_VALIDATION_VALIDATE_SYNC = 'true'
```

Проверяйте stderr на `[Vulkan ERROR]` и synchronization hazards: успешный код
выхода приложения сам по себе не доказывает отсутствие validation errors.
После проверки переменную можно убрать через
`Remove-Item Env:VK_VALIDATION_VALIDATE_SYNC`.

Автоматическая проверка повторных startup/shutdown, resize, FIFO/IMMEDIATE,
off/SMAA/FSR, instancing и compute workload:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\render_validation_windows.ps1 -ExePath "$PWD\build\windows-debug\bin\vulkan_cube.exe" -Startups 3 -Cycles 2
```

Скрипт сам включает synchronization validation, получает завершённые GPU
captures, проверяет stderr и нулевой остаток VMA allocations. Логи находятся
в `render-validation` внутри выбранного build tree. Для ASan используйте
`windows-asan/bin/vulkan_cube.exe` в окружении `vcvars64.bat`. Это ограниченный
автоматический soak; minimize/fullscreen и длительный asset churn в него не входят.

## Benchmark JSON

`schema_version: 2` сохраняет workload mode, build type, число triangles,
фактическое время measurement и подробные CPU/GPU timings. FPS равен числу
завершённых CPU render iterations за measurement seconds. CPU frame
измеряется start-to-start, включая window title/metrics work и scheduler gaps.
Это не display latency и не количество кадров, реально показанных монитором.

`one_percent_low_fps` — обратная величина среднего времени самых медленных
`ceil(N / 100)` кадров всего measurement interval. До прогрева выделяется
массив максимум на 1 000 000 samples (8 MB); переполнение делает benchmark
неуспешным вместо выдачи усечённого percentile. `percentile_scope` и
`percentile_samples` описывают охват. Старые JSON использовали последние 2000
samples, поэтому их 1% low напрямую сравнивать нельзя. UI/F2 сохраняют rolling
window; редактор обновляет snapshot раз в 250 ms.

Недоступные GPU timings записываются как JSON `null`. GPU timestamps читаются
после frame fence без query WAIT и относятся к более раннему submission;
первые samples на границе прогрева могут относиться к предыдущему кадру.

## Шейдеры

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\compile_shaders_windows.ps1
```

Команда обновляет versioned SPIR-V в `shaders/spv`. Source и binary должны
изменяться вместе. Чтобы проверить существующие binaries без перекомпиляции:

```powershell
Get-ChildItem shaders\spv\*.spv | ForEach-Object {
    & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.3 $_.FullName
    if ($LASTEXITCODE -ne 0) { throw "Invalid SPIR-V: $($_.Name)" }
}
```

## Проверка окружения 2026-09-11

На этой машине: VS 2022 Community, MSVC 14.38.33130, CMake 4.3.2,
Ninja 1.12.1, AMD Radeon RX 6800. Vulkan SDK 1.4.313.2 восстановлен:
до установки переменная окружения и реестр ссылались на отсутствующие файлы.

- Debug, Release и ASan собраны; CTest прошёл 4/4 в каждой конфигурации.
- MCP smoke прошёл в Release, Debug и ASan; последние два запускались с validation.
- Все 34 SPIR-V binaries прошли `spirv-val`.
- `vulkaninfoSDK.exe --summary` обнаружил Khronos validation layer.
- Исходная synchronization validation обнаружила `WRITE_AFTER_READ` и
  `PRESENT_AFTER_WRITE` для swapchain. В последующем аудите исправлены acquire
  dependency, present signal scope и depth reuse; Debug/ASan сценарии
  `render_validation_windows.ps1` проходят без ошибок, с нулевыми VMA allocations.
- Проверены graceful failure при отсутствующем shader, а также startup/render/
  shutdown редактора с synchronization validation. Подробности и performance
  результаты находятся в [аудите renderer](RENDERER_AUDIT.md).
- Нативный clang-tidy запускался для renderer, application, metrics, FSR,
  Vulkan context и GPU buffer. Остались общие guideline/style предупреждения;
  это не полностью чистый static-analysis gate всего проекта. Отдельные
  analyzer targets пока не подключены к CMake; MSVC `/analyze` не запускался.

Установка SDK: https://vulkan.lunarg.com/doc/view/1.4.313.0/windows/getting_started.html
