# Asset import

Редактор импортирует внешние ресурсы в project-owned каталог `assets`.
Импорт доступен только в `Edit` mode:

- `File -> Import assets...` или `Ctrl+I` открывает окно импорта;
- внешний файл можно перетащить в область списка окна `Assets`;
- несколько файлов образуют один bounded batch, но каждый source публикуется
  собственной транзакцией.

Поддерживаемые source formats:

- models: FBX, OBJ, glTF, GLB, Collada DAE, PLY, STL и 3DS;
- images: PNG, JPEG, TGA, HDR, BMP, DDS и EXR.

Импорт выполняет постоянный editor worker. Render thread только ставит bounded
request в очередь и принимает готовый результат на границе frame. Обычный
steady-state frame не читает filesystem и не запускает parser.

## Model asset

Model sources нормализуются через статически связанную, закреплённую версию
Assimp. Для OBJ дополнительно сохраняются безопасно разрешённые MTL dependencies;
для glTF внешние buffers сначала копируются в sandboxed snapshot. GLB, DAE, PLY,
STL и 3DS проходят тот же bounded cook в единый runtime mesh.
Для каждого source создаётся отдельный каталог:

```text
<name>/
  <name>.vcrasset.json
  mesh/<name>.vcrmesh
  materials/<index>_<name>.vcrmaterial.json
  textures/<index>_<usage>.vcrtexture.json
  textures/<imported texture data>
  source/<original model>
```

`*.vcrasset.json` хранит source format, geometry counts, vertex layout,
submeshes и ссылки на material assets. `*.vcrmesh` имеет little-endian
versioned layout:

- magic `VCRMESH`;
- format version и vertex stride;
- vertex/index/submesh counts;
- submesh ranges и material indices;
- vertices `position float3, normal float3, uv0 float2, tangent float4`;
- 32-bit triangle indices.

Model manifest можно добавить в сцену кнопкой `Add to Scene`, двойным кликом,
context menu или перетаскиванием на `Scene View`. Перед изменением сцены editor
повторно проверяет containment/symlinks. Постоянный worker загружает manifest,
mesh, материалы и textures, готовит LOD. Submission thread отправляет единый
upload batch и публикует модель после сигнала fence; UI сохраняет pending
добавление. Начальный transform
записывает в scene JSON явные center/ground/fit translation и uniform scale,
поэтому source-space offset и единицы большого FBX не прячут model от камеры.

Scene Assimp pre-transform применяется при cook, поэтому normalized mesh —
статический snapshot. Исходный FBX/OBJ сохраняется отдельно. Animation и
skinning не попадают в текущий runtime mesh; importer сообщает об этом warning.

Каждый материал FBX/OBJ создаётся отдельным
`*.vcrmaterial.json`. Это относится и к материалам, записанным непосредственно
в FBX. Embedded и доступные external textures извлекаются/копируются отдельно,
а материал ссылается на их `*.vcrtexture.json`.

## Image и material assets

Самостоятельное изображение сохраняется без перекодирования: source bytes
копируются точно, рядом создаётся `*.vcrtexture.json`. Decode и GPU upload
намеренно deferred до первого использования в scene, поэтому import не меняет
исходное качество. Runtime loader поддерживает JPEG/PNG, OpenEXR (включая DWAA)
и embedded RGBA8 payload с проверкой dimensions и bounded working memory.
Cooked mesh сохраняет исходные FBX/OBJ UV с origin снизу слева. Model shader
преобразует `V` при sampling, потому что декодированные строки JPEG/PNG/OpenEXR
загружаются сверху вниз; это позволяет не менять tangent basis normal map.

Material Editor открывает `*.vcrmaterial.json` из окна `Assets`. Он редактирует
имя и безопасно ограниченные metallic-roughness factors:

- base color RGBA;
- metallic;
- perceptual roughness;
- ambient occlusion.

Ссылки на импортированные texture assets сохраняются в material JSON. Save
пишет complete temporary file и только затем заменяет material asset; Revert
возвращает последнюю сохранённую версию.

## Transaction и границы

Destination всегда остаётся внутри canonical project `assets`. Импорт
запрещает `..`, symbolic-link components и молчаливую замену существующего
asset. Все файлы сначала создаются в sibling staging directory. Готовый каталог
публикуется атомарной no-replace операцией; ошибка удаляет staging и не оставляет
частично импортированный asset.

Importer имеет явные limits на source bytes, итоговый объём, mesh/vertex/index,
material и texture counts. Размеры binary output проверяются на integer
overflow до allocation и записи.

Packaged fallback `assets` доступен только для просмотра. Import разрешён лишь
когда найден исходный project root с `CMakeLists.txt` и writable `assets`.

## Runtime rendering

Editor renderer рисует imported model geometry и применяет PBR factors каждого
submesh material. Cooked position/normal/UV/tangent поступают в отдельные
direct/AA model pipelines. Base-color texture использует sRGB view, normal map
— linear view и TBN; отсутствующая, неподдерживаемая или semantic-mismatched
texture заменяется белой/flat-normal fallback с warning. Texture/material cache
общий для editor/game renderer, ограничен по числу assets, descriptors и GPU bytes и живёт до
shutdown, чтобы не уничтожать resources, которые ещё могут использовать два
frames in flight. Metallic/roughness/AO texture slots пока используют factors.
Imported models, cubes и terrain участвуют в shadow caster pass. Карта тени
переиспользуется, пока transforms/состав сцены/настройки не изменились.
Подробности LOD, visibility и стоимости dynamic shadows: [runtime renderer](ENGINE_EVOLUTION.md).
