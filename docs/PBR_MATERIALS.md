# PBR materials: IOR, shading models и SSS controls (E08/E14)

Состояние на 2026-09-13: реализованы полный `float` IOR и три режима материала —
`standard`, `unlit` и `subsurface`. Это расширение существующего непрозрачного
metallic-roughness renderer. Оно не добавляет прозрачность, преломление лучей,
объёмную оптику, eye shader или спектральную модель проводника.

## Данные и стоимость ABI

[PbrMaterial](C:/GDF/vk_test/src/pbr_material.hpp) передаётся в GPU без FP16,
упаковки нескольких параметров в один float или округления IOR при сериализации.
Все оптические числовые параметры хранятся как IEEE-754 binary32. Идентификатор
модели — отдельный `uint32_t` на CPU и `uint` в GLSL.

| Смещение, bytes | Поле | Тип / значение по умолчанию |
|---|---|---|
| 0–15 | `base_color` | `vec4`, `(1, 1, 1, 1)` |
| 16 | `metallic` | `float`, `0` |
| 20 | `roughness` | `float`, `0.65` |
| 24 | `ambient_occlusion` | `float`, `1` |
| 28 | `reserved` | `float`, `0` при sanitization; существующий runtime source ID |
| 32 | `ior` | `float`, `1.5` |
| 36 | `model` | `uint32_t`: `standard = 0`, `unlit = 1`, `subsurface = 2` |
| 40 | `subsurface_profile` | `uint32_t`: `none = 0`, `narrow = 1`, `medium = 2`, `wide = 3` |
| 44 | `subsurface_strength` | `float` в `[0,1]`, default `0` |

`PbrMaterial` занимает 48 bytes вместо 32: +16 bytes, или +50% на материал.
[SceneGpuInstance](C:/GDF/vk_test/src/scene_document.hpp) содержит три текущие
строки transform и материал: 96 bytes, материал начинается со смещения 48.
[RuntimeGpuInstance](C:/GDF/vk_test/src/runtime_scene.hpp) дополнительно содержит
три предыдущие строки transform со смещения 96: 144 bytes вместо 128, +12.5%.
Demo `PbrInstanceData` также вырос с 80 до 96 bytes. Million-instance benchmark
сохраняет отдельный 64-byte `InstanceData` и свой Lambert shader; это увеличение
PBR ABI к нему не относится.
Массивы материалов и runtime instances используют соответственно stride 48 и 144
в `std430`. Нельзя использовать старый stride или старые SPIR-V с новым CPU ABI.

Для 100 000 runtime записей добавка составляет 1 600 000 bytes на одну копию
массива. Это арифметика размера данных, а не измерение суммарной памяти renderer:
число CPU/GPU/frame копий учитывается отдельно. MaterialABI A/B выявил рост
стоимости, описанный ниже. Последующее устранение дублирования CPU packets
сохраняет stride 144 и рассматривается отдельно в `CPU_SCENE_PACKETS.md`.

## IOR и Fresnel

Внешний контракт IOR: конечное число в диапазоне `[1, FLT_MAX]`, default `1.5`.
Scene JSON, cooked material JSON, редактор материала и MCP проверяют диапазон до
преобразования JSON number в float. Неверные значения, строки и boolean
отклоняются. Отсутствующее поле сохраняет совместимость со старым материалом.
Внутренний `sanitize_pbr_material` восстанавливает default для невалидного IOR;
это защитный CPU путь, а не разрешение принимать некорректный файл.

Для диэлектрика относительно воздуха используется
`F0 = ((ior - 1) / (ior + 1))²`. Это отражение при нормальном падении, а не
постоянное отражение для всех углов. Связь IOR с F0 и default `1.5` определены в
[KHR_materials_ior](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_materials_ior/README.md).

[CPU helper](C:/GDF/vk_test/src/pbr_material.hpp) и
[GLSL helper](C:/GDF/vk_test/shaders/pbr_material.glsl) вычисляют эквивалентную
форму `r = 1 / ior; F0 = ((1 - r) / (1 + r))²`. Она не переполняет float при
очень большом допустимом IOR. Для `ior == 1.5` возвращается буквальное `0.04F` /
`0.04`, сохраняя прежнюю константу побитово. Для `ior == 1` результат — ноль.
GLSL полагается на проверенный CPU ввод; NaN и бесконечность в shader ABI не
разрешены. F0 и SSS strength передаются из vertex shader как `flat float`,
модель и profile — как `flat uint`.

IOR управляет F0; существующие Schlick Fresnel и split-sum IBL не являются
точным dielectric Fresnel. При IOR `1` F0 равен нулю, но grazing-отражение и
аддитивный IBL bias не обнуляются полностью: например, Schlick при cosine `0.5`
даёт `0.03125`. Это ограничение BRDF-аппроксимации, а не ошибка material layout.

[PBR lighting](C:/GDF/vk_test/shaders/pbr_lighting.glsl) использует
`mix(vec3(dielectricF0), baseColor, metallic)` для нормального отражения в direct
и environment освещении. При `metallic == 1` цвет проводника задаётся base color,
IOR не влияет на BRDF; для промежуточного metallic меняется диэлектрическая часть.
Roughness остаётся в диапазоне `[0.045, 1]`, metallic и AO — `[0, 1]`.
Процедурный terrain пока выдаёт фиксированные `standard` и F0 `0.04`.

## Direct и environment lighting

Исходная BRDF основа сохранена: Lambert diffuse и Cook-Torrance microfacet
specular с GGX distribution, height-correlated Smith masking-shadowing и
Schlick Fresnel по half-vector. Diffuse уменьшается на Fresnel и исчезает при
`metallic = 1`. Artist-facing roughness — perceptual: shader вычисляет
`alpha = roughness²`, затем `alpha²` в GGX/Smith. Основа реализации описана в
[Crash Course in BRDF Implementation](https://boksajak.github.io/files/CrashCourseBRDF.pdf).

Directional sun использует полный BRDF и shadow map. Для статичной панорамы
renderer строит GPU mip chain с linear blit при доступных format features;
иначе выдаёт warning и использует mip 0. Roughness выбирает mip как дешёвую
аппроксимацию low-pass фильтра, а аналитический split-sum fit приближает
интегрированный environment BRDF. Это не GGX-convolution cubemap. AO множит
indirect diffuse и specular, не direct sun.

Imported model path читает base-color texture как sRGB, normal map и
metallic-roughness texture как linear data. В последней G умножает roughness,
B — metallic. Отдельная AO texture, anisotropy, clear coat, transmission и
multiple-scattering compensation этим этапом не реализованы. В dynamic lighting
появляются процедурное небо, CSM и опциональное voxel GI; их собственные
приближения описаны в [DYNAMIC_LIGHTING.md](C:/GDF/vk_test/docs/DYNAMIC_LIGHTING.md).

## Standard, unlit и subsurface

`standard` сохраняет существующие GGX/Smith/Schlick, direct light, тени и ambient
освещение. `unlit` использует линейный base color; у импортированной модели он
умножается на base-color texture. В этой ветке не вычисляются BRDF, normal map,
metallic-roughness texture, освещение, принимаемые тени или AO. IOR, metallic,
roughness и AO разрешено хранить во внутреннем unlit-материале, но они не меняют
его цвет. `model` не выводится из IOR и не кодируется в дробной части параметра.

`subsurface` использует ту же непрозрачную GGX/Smith/Schlick BRDF, но при
активном глобальном режиме `full` отделяет diffuse от specular. Доля
`subsurface_strength × diffuse` проходит screen-space diffusion, а specular и
остаток diffuse сохраняются в неразмываемом HDR base. `none` или нулевая strength
дают обычный непрозрачный результат. Profile и strength не влияют на
`standard`/`unlit` и могут безопасно оставаться `none`/`0`.

Доступны три опорных RGB Gaussian sigma в единицах сцены:

| Profile | RGB scatter distance |
|---|---|
| `narrow` | `(0.02, 0.009, 0.004)` |
| `medium` | `(0.06, 0.028, 0.012)` |
| `wide` | `(0.15, 0.075, 0.032)` |

Эти значения проверяют engine contract и различимость profiles; они не являются
физически откалиброванными skin presets. Geometry output квантует strength в
UNORM8 и записывает exact control `profile * 256 + strengthUNorm8` в alpha
`RGBA16F`. Диапазон до 1023 представим в FP16 точно. Два full-resolution
7-sample passes фильтруют только соседей с тем же profile и близким linearized
reverse-Z depth; horizontal пишет в отдельный ping target, vertical композитит
с HDR base. Радиус ограничен 24 pixels. Общий принцип двух одномерных свёрток
следует [Separable Subsurface Scattering](https://www.iryoku.com/separable-sss/),
но текущие Gaussian profiles являются нейтральным baseline, а не копией
production diffusion kernel статьи.

Unlit проходит обычную геометрию, depth test/write и применимый depth prepass.
Он может перекрывать объекты и отбрасывать тень; это не источник света для GI.
AA, motion/depth outputs и display mapping сохраняются. Поэтому unlit не
гарантирует совпадение экранных sRGB значений с исходным линейным base color.
Синтетическая workload modulation в режиме GPU saturation также остаётся общей
для обеих моделей.

Текущие pipelines непрозрачные: fragment shader выводит alpha, но нет alpha
blending, alpha-mask discard или выбора двухсторонней растеризации по материалу.
Cooked vertex не содержит vertex color. Следовательно, поддерживается только
подмножество поведения
[KHR_materials_unlit](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_materials_unlit/README.md):
полный стандарт также сохраняет alpha coverage, double-sided поведение и
умножение на vertex color. Alpha в файле сама по себе не включает эти функции.

## Импорт glTF/GLB и OBJ

[Importer](C:/GDF/vk_test/src/asset_importer.cpp) проверяет optical extensions в
JSON до Assimp; для GLB читается ограниченный JSON chunk с проверкой заголовка и
границ. `KHR_materials_ior` и `KHR_materials_unlit` должны быть объектами и
перечисляться в `extensionsUsed`. Значение IOR сохраняется как полный float.
Отсутствующее `ior` внутри IOR extension означает `1.5`; unlit импортируется
через соответствующую shading-model семантику Assimp.

glTF допускает специальный `ior = 0` как режим совместимости с отражением 1.
Движок этот режим явно отклоняет с диагностикой `IOR 0`, поскольку его контракт
ограничен конечным IOR не меньше 1. Это намеренное ограничение импорта, а не
утверждение, что такой glTF недействителен.
[Определение специального режима](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_materials_ior/README.md).

В одном glTF материале IOR extension запрещено сочетать с
`KHR_materials_unlit` или `KHR_materials_pbrSpecularGlossiness` согласно
[Exclusions](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_materials_ior/README.md#exclusions).
Этот запрет применяется на границе импорта; внутренний `PbrMaterial` может
содержать unlit и любой допустимый IOR. Проверка исключений покрыта отдельными
отрицательными glTF/GLB tests; положительный test разрешает документ с IOR и
unlit в разных материалах.

OBJ `Ni` намеренно не импортируется. Assimp может выдавать refraction index
`1.0` даже при отсутствии авторского `Ni`; пока происхождение значения не
различается, OBJ сохраняет исторический IOR `1.5`, включая файлы с явным `Ni`.
Поддержка IOR extension не означает поддержку transmission, volume или
произвольных других glTF material extensions.

## Сериализация и управление

Сцена сохраняет `color` рядом с объектом `pbr`; новые поля находятся в
`pbr.ior`, `pbr.model`, `pbr.subsurface_profile` и
`pbr.subsurface_strength`. Фрагмент узла:

```json
{
  "color": [0.45, 0.45, 0.45, 1.0],
  "pbr": {
    "metallic": 0.0,
    "roughness": 0.25,
    "ambient_occlusion": 1.0,
    "ior": 1.33,
    "model": "subsurface",
    "subsurface_profile": "medium",
    "subsurface_strength": 0.75
  }
}
```

Cooked `*.vcrmaterial.json` хранит цвет как `pbr.base_color`; все четыре поля
находятся в том же `pbr`. Старые scene/material документы без них загружаются с
`1.5`, `standard`, `none` и `0`. Допустимы только точные lowercase model tokens
`standard` / `unlit` / `subsurface` и profile tokens `none` / `narrow` /
`medium` / `wide`; неизвестный token, non-finite strength или значение вне
`[0,1]` является ошибкой. Importer явно записывает `none`/`0`: он не назначает
SSS материал по имени или виду исходного ассета.

[Inspector и material editor](C:/GDF/vk_test/src/editor_ui.cpp) имеют `IOR` и
`Shading model`. IOR редактируется шагом `0.01`, с ограничением `[1, FLT_MAX]`;
формат отображения `%.6g` не округляет хранимое значение благодаря
`NoRoundToFormat`. Node inspector также редактирует SSS profile и strength при
модели `subsurface`. Material-document loader/saver сохраняет и проверяет новые
поля, но отдельные SSS controls импортированного submesh material editor пока не
выставляет: для новой authoring операции требуется отдельный typed MCP contract.

[MCP](C:/GDF/vk_test/src/mcp_server.cpp): `editor.scene.create_node` и
`editor.scene.update_node` принимают `ior`, `model`, `subsurface_profile` и
`subsurface_strength` как поля аргументов верхнего уровня. Материальные правки
разрешены только для `cube` и `ground`.
`editor.scene.get_node` / `editor.scene.list_nodes` возвращают их внутри `pbr`.
Это API authored factors; он не патчит submesh materials импортированной модели.
Глобальный `render.set_subsurface` принимает только `{"mode":"off"}` или
`{"mode":"full"}`. CLI эквивалент — `--subsurface off|full`, default `off`.

## Процедурная таблица из 28 сфер

Обычная demo scene сохраняет heightfield `256 × 192` cells / `98 304` triangles
вместо прежней сетки на 10 миллионов triangles и выбирает terrain base color и
roughness по высоте и уклону. Два стандартных ряда по пять сфер показывают
gold-like металлы и синие диэлектрики при roughness
`0.08, 0.28, 0.50, 0.72, 0.94`. Полная обычная demo workload имеет `120 387`
triangles и 12 renderable objects без sky как object. Это описание геометрии,
а не актуальный performance benchmark.

`--render-fixture material-spheres --camera-z 12` создаёт семь рядов по четыре
сферы. Столбцы слева направо: IOR `1`, `1.33`, `1.5`, `2`; radius `0.65`.
Таблица рассчитана на camera `(0, 4, 12)` с наклоном вниз 12°; terrain и обычные
demo cubes в её geometry path не рисуются. Fixture нельзя комбинировать с
`--scene-count` или `--lighting-room`.

| Ряды сверху вниз | Model / metallic | Roughness | Линейный RGB |
|---|---|---|---|
| 1–3 | standard / 0 | 0.05; 0.25; 0.7 | (0.45, 0.45, 0.45) |
| 4–6 | standard / 1 | 0.05; 0.25; 0.7 | (0.8, 0.45, 0.2) |
| 7 | unlit / 0 | 0.05 | (0.25, 0.5, 0.75) |

Это acceptance fixture до появления production content: диэлектрические ряды
должны показывать изменение отражения с IOR; BRDF проводника не должен зависеть
от IOR, unlit — от освещения. Разные позиции сфер меняют view/light directions,
поэтому простое равенство пикселей разных металлических сфер не является
корректным числовым тестом независимости от IOR. Для такого сравнения нужны
одинаковые pose, material и lighting с изменением только IOR.

`--render-fixture subsurface-profiles` использует ту же таблицу 7×4, но меняет
глубину по колонкам. Ряды: standard control; `narrow`, `medium`, `wide` с
strength 1; `medium` и glossy `wide` с strength 0.5; unlit control. Активны 20
SSS материалов. Fixture проверяет material/MRT/profile/depth-aware filter contract
до появления production кожи. Глобальный SSS mode остаётся default `off`; при
первом кадре `full` два дополнительных `RGBA16F` target на каждый из двух frame
slots создаются лениво. Их дополнительный размер при 1920×1080 равен
66 355 200 bytes (63.28125 MiB). Выключение прекращает SSS passes, но уже
созданные targets освобождаются при resize/extent recreation или shutdown.

Финальный чистый Windows Release A/B после исправления общего 24-pixel
kernel-radius cap на RX 6800, 1920×1080 IMMEDIATE, AA off и трёх повторах дал
для `full` 2409.03 FPS и 0.22762 ms GPU frame против 2661.80 FPS и 0.16136 ms
при `off`: −9.50% FPS и +0.06626 ms GPU frame. Измеренный H/V SSS interval —
0.09864 ms. Режим принят как optional quality path; default остаётся
`off`. Debug/Release/ASan прошли 8/8 CTest, релевантная synchronization
validation покрыла dynamic AA off/SMAA/FSR и legacy AA off, ASan — FSR runtime,
editor MCP smoke — 22 tools. Fixed 1280×720 off/full captures изменили 1.9679%
pixels, maximum RGB difference 21/5/1; визуально явного profile/depth bleeding
не найдено. Этот fixture не заменяет будущую проверку откалиброванных skin assets.

## Подтверждённые проверки и оставшиеся gates

До последней правки проверки glTF exclusions нативные Windows Debug, Release и
ASan builds прошли с 6/6 CPU CTest в каждой конфигурации:
[Debug log](C:/GDF/vk_test/build/windows-debug/material-build.log),
[Release log](C:/GDF/vk_test/build/windows-release/material-build.log),
[ASan log](C:/GDF/vk_test/build/windows-asan/material-build.log).
После исправления glTF exclusions Debug повторно собран с 6/6 CPU CTest:
[exclusions log](C:/GDF/vk_test/build/windows-debug/material-exclusions-build.log).

Отдельный target `vcr_material_gpu_tests` проверяет 30 material records со stride
48 и две реальные runtime записи со stride 144. Набор покрывает три модели,
три SSS profile и exact binary32 strength bits.
Максимальная абсолютная ошибка F0 относительно независимой double-формулы —
`3.774273360513e-08`; default F0, uint model, color/AO и transform rows совпали
точно, после завершения live GPU allocations — 0.
[Debug GPU log](C:/GDF/vk_test/build/windows-debug/material-gpu-validation.log),
[ASan GPU log](C:/GDF/vk_test/build/windows-asan/material-gpu-validation.log),
[C++ test](C:/GDF/vk_test/tests/material_gpu_tests.cpp),
[compute shader](C:/GDF/vk_test/shaders/material_reference.comp).

Тест включает IOR `1`, ближайший float выше 1, `1.33`, `1.5`, `2`, `2.42`,
`16`, `65504`, `1e20`, `FLT_MAX` для трёх моделей; полный GPU ABI проверяется
sentinel значениями. Это числовой и layout test, не проверка качества sphere
chart или всех graphics pipelines.

Runtime validation для dynamic lighting с AA off/SMAA/FSR3 завершилась с
`validation_errors = 0`, `live_gpu_allocations = 0`, `exit_code = 0` во всех
перечисленных сериях:

| Конфигурация / fixture | Startup × cycles | Captures | Результат |
|---|---|---:|---|
| Debug / material-spheres | 1 × 1 | 34 | [log](C:/GDF/vk_test/build/windows-debug/material-spheres-validation.log) |
| ASan / material-spheres | 2 × 1 | 68 | [log](C:/GDF/vk_test/build/windows-asan/material-spheres-validation.log) |
| Debug / canonical CPU hierarchy + coarse sort | 1 × 1 | 34 | [log](C:/GDF/vk_test/build/windows-debug/material-canonical-cpu-validation.log) |
| Debug / canonical GPU visibility | 1 × 1 | 34 | [log](C:/GDF/vk_test/build/windows-debug/material-canonical-gpu-validation.log) |
| ASan / canonical CPU hierarchy + coarse sort | 1 × 2 | 68 | [log](C:/GDF/vk_test/build/windows-asan/material-canonical-cpu-validation.log) |
| Debug / stress CPU hierarchy + coarse sort | 1 × 1 | 34 | [log](C:/GDF/vk_test/build/windows-debug/material-stress-validation.log) |

Для default материалов сравнивались декодированные pixels 56 пар PNG до/после:
все пары совпали точно, maximum difference и changed channels равны нулю.
Сравнение покрывает canonical CPU/GPU groups, но исключает FSR3 и active compute
workload; это не утверждение эквивалентности этих исключённых режимов.
[Comparison JSON](C:/GDF/vk_test/build/windows-debug/render-validation/depth-prepass-canonical/material-default-comparison.json),
[helper](C:/GDF/vk_test/build/windows-debug/compare-material-default.py).

Editor MCP в Debug и ASan прошёл по 168 checks, включая отклонение 20 invalid
requests. Временные узлы очищены, исходная canonical scene сохранила SHA-256,
shutdown завершился с нулём live GPU allocations. Эти проверки не являются
попиксельной оценкой освещения editor screenshot:
[Debug summary](C:/GDF/vk_test/build/windows-debug/material-editor-validation.log),
[ASan summary](C:/GDF/vk_test/build/windows-asan/material-editor-validation.log),
[Debug runtime log](C:/GDF/vk_test/build/windows-debug/render-validation/material-editor/editor.log),
[ASan runtime log](C:/GDF/vk_test/build/windows-asan/render-validation/material-editor/editor.log).

После glTF exclusions все три Windows сборки повторно прошли 6/6 CTest
(`material-exclusions-build.log`). Контролируемая MaterialABI серия из 18 runs
завершена; результаты находятся в `RENDERING_PIPELINE_RESULTS.md`.
В исходном варианте с дублируемыми CPU packets расширение материала добавило
примерно 1–2% GPU времени основной сцене и 10% CPU stress с 100k объектов.
Устранение повторного packing/upload измеряется отдельным последующим этапом.
Capture/validation counts сами по себе не доказывают визуальную точность всех
BRDF случаев; добавление quality feature не является ускорением.
