# Процедурные fixtures для LOD transitions

[Генератор](C:/GDF/vk_test/scripts/generate_lod_fixture_windows.py) создаёт два
небольших набора обычных cooked assets. Они проходят
`load_cooked_model_asset → prepare_authored_model_lods → imported model draws`;
special renderer или новый CLI режим не используются.

## Asset root для изолированного запуска

[find_runtime_asset_root](C:/GDF/vk_test/src/runtime_assets.cpp) сначала читает
`VCR_ASSET_ROOT` через `SDL_getenv`. Значение задаёт **сам каталог assets**, а не
его родителя. Это холодный lookup без дополнительного глобального cache.
Существующий каталог канонизируется и возвращается с `project_owned = false`.
Если переменная задана, но путь пуст, недоступен, не является каталогом или не
преобразуется в filesystem path, возвращается `nullopt`; переход к canonical
сцене в таком случае запрещён. Без переменной сохраняется прежний поиск
project/package assets.

SDL читает свой thread-safe environment snapshot. Устанавливать переменную
следует в окружении нового процесса **до его запуска**, а не менять окружение
во время rendering. На Windows путь интерпретируется как UTF-8; абсолютный путь
исключает зависимость от working directory. Переменная не меняет пути встроенных
shader binaries, sky texture и SMAA assets: они по-прежнему находятся рядом с
исполняемым файлом.

## Генерация

В native Windows PowerShell:

```powershell
python scripts/generate_lod_fixture_windows.py --configuration Debug
```

`--configuration` принимает `Debug`, `Release`, `ASan`. Соответствующий штатный
build tree должен уже существовать; генератор создаёт только вложенный
`lod-fixture`. CMake manifests, fake-project markers, binaries и GPU процессы
не создаются. `--camera-z N` меняет только authored camera в generated scene:
допустим конечный `N > 0.1` и `N <= 400`, default `12`.

```text
build/windows-debug/lod-fixture/
  fixture-manifest.json
  coverage/assets/
    scenes/editor_scene.json
    lod_fixture/
      lod0.vcrasset.json
      lod1.vcrasset.json
      lod2.vcrasset.json
      mesh/lod0.vcrmesh
      mesh/lod1.vcrmesh
      mesh/lod2.vcrmesh
      materials/surface.vcrmaterial.json
  geometry/assets/
    ... та же структура ...
```

Повторная генерация разрешена только в каталоге с собственным
`fixture-manifest.json`; посторонний непустой каталог не перезаписывается.
Файлы не удаляются. Каждый путь проверяется относительно generated root, а
SHA-256 пользовательского `assets/scenes/editor_scene.json` сравнивается
до/после без его изменения. Manifest содержит размеры, SHA-256 и counts.

## Геометрия и authored LOD

Оба набора — плоская поверхность XY с normal `+Z`, CCW triangles и одним
непрозрачным unlit-материалом `(0.25, 0.5, 0.75, 1)` без текстур. Общий material
name `LodFixture` обеспечивает штатное сопоставление между authored LOD.

| LOD | Подразделения | Vertices | Indices | Triangles | Coverage half extent | Geometry half extent |
|---|---:|---:|---:|---:|---:|---:|
| 0 | 8 × 8 | 81 | 384 | 128 | 1 | 1 |
| 1 | 4 × 4 | 25 | 96 | 32 | 1 | 31/32 |
| 2 | 1 × 1 | 4 | 6 | 2 | 1 | 7/8 |

Все координаты и UV — двоичные дроби. Coverage меняет tessellation, сохраняя
силуэт, плоскость и цвет. Geometry дополнительно уменьшает покрываемую область.
Объединённый runtime mesh содержит 110 vertices и 486 indices; index ranges
трёх LOD начинаются с 0, 384, 480. Это counts данных, не результаты GPU запуска.

LOD0 metadata содержит:

```json
"lods": [
  {"model": "lod_fixture/lod1.vcrasset.json", "geometric_error": 0.0625},
  {"model": "lod_fixture/lod2.vcrasset.json", "geometric_error": 0.25}
]
```

LOD references отсчитываются от asset root; mesh/material references — от
каталога metadata. LOD1/2 не содержат дальнейших `lods`. Errors намеренно
одинаковы в обоих наборах, чтобы сравнивать переходы при одинаковом policy:
для coverage реальная surface deviation равна нулю; для geometry она не больше
`sqrt(2)/32 ≈ 0.044195` и `sqrt(2)/8 ≈ 0.176777`. Указанные положительные
возрастающие значения являются консервативными границами и удовлетворяют
контракту authored LOD.

Mesh binary использует штатные little-endian `VCRMESH\0`, version 1,
40-byte header, один 24-byte submesh record, 48-byte vertices
`position/normal/uv/tangent` и `uint32` indices. Runtime material/instance ABI
48/144 bytes сюда не сериализуется: material factors хранятся отдельно в JSON.

## Запуск и acceptance

После сборки runtime с поддержкой override выбрать один generated root:

```powershell
$fixturePreviousRoot = [Environment]::GetEnvironmentVariable('VCR_ASSET_ROOT', 'Process')
try {
    $env:VCR_ASSET_ROOT = (Resolve-Path 'build/windows-debug/lod-fixture/coverage/assets').Path
    & '.\build\windows-debug\bin\vulkan_cube.exe' --validation --width 640 --height 360 --present immediate --lighting legacy --gi off --culling frustum --lod on --depth-prepass off --aa off --visibility-backend cpu
} finally {
    [Environment]::SetEnvironmentVariable('VCR_ASSET_ROOT', $fixturePreviousRoot, 'Process')
}
```

Для автоматизации установить ту же переменную в `ProcessStartInfo.Environment`
дочернего процесса. Generated scene содержит один model и startup camera
`(0, 0, N)`, направленную вдоль `-Z`; terrain, sun и sky не включены. Не передавать
renderer CLI `--camera-z`: он выбирает legacy pose с другим наклоном. Для
параллельного плоскости обзора менять generated camera через параметр генератора
либо двигаться вперёд/назад штатным camera input без поворота.

- Сначала дождаться завершения обычной асинхронной загрузки (`assets_pending = 0`).
- Проверить по runtime counts, что при изменении расстояния выбираются LOD0/1/2.
  Точные расстояния зависят от resolution, pixel-error budget, conservative scale
  и hysteresis; заданный camera distance сам по себе не доказывает выбранный LOD.
- На coverage сравнить settled LOD0 и промежуточные переходы: одинаковый unlit
  цвет и силуэт позволяют искать дырки или двойное покрытие без смены BRDF.
  Побитовое равенство pixels всё равно требует реального capture test.
- На geometry проверить изменение силуэта, завершение old/new records,
  reversal, третий target и reset. Это проверка перехода различной геометрии;
  exact-pixel сравнение с LOD0 для неё некорректно.
- Повторить при depth-prepass off/partial/full и релевантных AA modes, когда
  интеграция fade готова. Результат CPU масок не заменяет GPU coverage test.
- Отдельно проверить invalid `VCR_ASSET_ROOT`: приложение должно сообщить
  невозможность найти root, не загружая пользовательскую canonical scene.

Генератор выполнен нативным Windows Python для Debug: internal binary/header,
finite-value, winding, index-range и declared-error checks пройдены; manifest
подтверждает неизменный canonical SHA-256. C++ loader, environment override,
GPU rendering и визуальные acceptance на момент подготовки fixture ещё не
запускались; новые runtime sources требуют штатных Windows gates.
