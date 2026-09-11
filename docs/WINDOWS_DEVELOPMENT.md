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
- Расширенная synchronization validation обнаружила `WRITE_AFTER_READ` и
  `PRESENT_AFTER_WRITE` для swapchain. Это незакрытый blocker корректности
  renderer; обычный smoke его не выявил. Лог текущего прогона:
  `build/windows-debug/environment-validation.log` (генерируемый, не в Git).
- Static analyzer targets пока не подключены к CMake; `/analyze` и clang-tidy
  не запускались. Полный resize/mode-switch soak не выполнялся.

Установка SDK: https://vulkan.lunarg.com/doc/view/1.4.313.0/windows/getting_started.html
