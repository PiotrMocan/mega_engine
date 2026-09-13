# Независимое обновление каскадов и формат shadow atlas

`--shadow-cache full|cascade` выбирает полное или независимое обновление четырёх
CSM tiles. `full` остаётся baseline по умолчанию. В `cascade` неизменная tile
сохраняется только при точном совпадении её стабилизированной матрицы. Новая
матрица записывается в frame uniforms вместе с обновлением соответствующей tile.

Кеш не задаёт частоту обновления по номеру кадра и не задерживает изменения
освещения. Смена сцены, caster transforms/assets, режима освещения, разрешения
атласа или cache policy инвалидирует весь atlas. Движение камеры инвалидирует
только изменившиеся матрицы. Изменение направления солнца обычно затрагивает
все четыре матрицы, поэтому выигрыш при неподвижном солнце нельзя переносить
на любой day-cycle workload.

## Ресурсный контракт

- Atlas один, используется общей graphics queue. Camera depth — reverse-Z;
  shadow depth сохраняет conventional projection, clear 1 и LESS/comparison LE.
- Undefined image и полная invalidation используют CLEAR всего atlas.
  Частичная invalidation использует LOAD и `vkCmdClearAttachments` строго внутри
  dirty tile. Чистые tiles остаются неизменными; STORE сохраняет все четыре.
- Перед изменением atlas barrier упорядочивает предыдущие fragment samples и
  последующие early/late depth accesses. При LOAD destination access включает
  DEPTH_STENCIL_ATTACHMENT_READ вместе с WRITE. После прохода depth writes
  становятся видимыми fragment samples; layout возвращается в DEPTH_READ_ONLY.
- Frame fences защищают command pools, uniforms и descriptors. Замена общего
  atlas при resize его разрешения ждёт оба frame fences до изменения descriptors.
  Steady-state cache не создаёт Vulkan objects, не читает GPU на CPU и не аллоцирует.
- Счётчики `shadow_passes`, `shadow_tiles`, `last_shadow_mask` доступны в
  `debug.get_state.render.lighting`; benchmark содержит totals. Totals включают
  startup/warmup, pass timings усредняются только за измеряемый интервал.

Управление доступно через checkbox в editor и
`render.set_shadow_cache {"mode":"full"|"cascade"}` в runtime MCP.

## Профиль глубины при старте

`--shadow-depth auto|d32|d16` и scene setting `lighting.shadow_depth` задают
профиль создания pipeline. `auto` предпочитает D32 и допускает прежний D16
fallback. Явные `d32`/`d16` требуют выбранный формат; неподдерживаемая комбинация
формата/разрешения завершается понятной ошибкой с предложением `auto`/1024.

Проверяются attachment, sampled-image, depth-comparison features, image-format
usage, sample count и ограничения extent. Linear filtering выбирается только
при поддержке, иначе остаётся nearest fallback. Фактические формат и фильтр
видны в editor, debug state и benchmark JSON. Смена формата требует перезапуска;
runtime setter освещения отклоняет смену profile, не пересоздавая pipeline
неявно. Разрешение и cache policy по-прежнему переключаются во время работы.

D16 сокращает размер хранимого depth texel, но уменьшает точность вдоль больших
ортографических depth intervals. Существующие slope/raster и world-texel receiver
bias сохраняются. D16 не включается автоматически ради меньшего размера:
необходимы отдельные проверки acne, отрыва тени и blend bands при низком солнце.

## Проверки

`scripts/render_validation_windows.ps1 -Scenario shadow-cache` использует
процедурную lighting-room с открытой крышей. Перед движением проверяется контраст
между тенью куба и освещённым полом. Probe обязан зафиксировать меньше четырёх
обновлений tiles на shadow pass; иначе тест завершается ошибкой. Затем cached
кадр сравнивается с full redraw при той же позе: все decoded pixels должны
совпасть при AA off/SMAA. Для FSR различия history/jitter фиксируются отдельно.

Проверяются resize, 1024/2048, часы 6/14, закрытие/открытие крыши, AA и scene /
instancing / compute switches. Roof mutation проверяет scene invalidation;
runtime MCP не предоставляет произвольное изменение caster transform, поэтому
это не доказательство покрытия всех editor transform operations.

Нативные Debug и ASan запуски сохраняют captures, comparisons, partial-work,
sync/lifetime diagnostics в соответствующем build tree. Результаты A/B публикуются
в `docs/RENDERING_PIPELINE_RESULTS.md`; наличие режима не означает доказанного
ускорения на каждом GPU или при любых настройках.

## Процедурная проверка диапазонов CSM

`--render-fixture cascade-cache` создаёт обычный SceneDocument без внешних assets:
пол, 36 box casters в 12 рядах, четыре тонких столбика, высокий боковой caster,
Sun и Sky. Ряды расположены на расстояниях 6 / 10.8 / 12 / 13.2 / 28.8 / 32 /
35.2 / 72 / 80 / 88 / 170 / 188 м от исходного положения камеры. Это даёт
контент для проверки движка до появления production-сцены. Fixture создаётся
однократно и проходит обычную загрузку runtime scene; генерации каждый кадр нет.

`scripts/render_validation_windows.ps1 -Scenario cascade-range -ShadowDepth d32`
или `d16` проверяет тот же cache/full, partial-work и lifecycle контракт на этой
сцене. При старте явно фиксируются camera-z 12, AA off, Reinhard, atlas 2048,
GI off и час 14. Камера находится в `(0,4,12)`, pitch −12°. Последующие проверки
включают часы 7 / 17 / 14 и смену разрешения атласа, но начальный pixel contract
не переносится на изменённые позы или освещение.

До движения камеры проверяются пять пар участков **пола**, тень и освещённый
receiver на одной строке. Каждая проба усредняет RGB в 3×3 pixels, координаты
центров нормализованы относительно reference 640×360. Проверка требует как
минимального контраста во всех трёх каналах, так и освещённого пола с RGB не ниже
125 / 128 / 132. Поэтому пустая сцена, исчезнувшие тени и тёмный фон не проходят
только за счёт непустого PNG. Room-контракт `shadow-cache` сохранён отдельно.

| Проба | Тень x,y | Свет x,y | RGB-контраст reference D32 | Минимум RGB |
|---|---|---|---|---|
| Ближняя, depth ≈6.1 м | 315,308 | 346,308 | 64.00 / 50.00 / 36.00 | 40 / 30 / 20 |
| За первым split, ≈13.0 м | 351,208 | 375,208 | 59.89 / 47.89 / 34.22 | 35 / 25 / 18 |
| Blend около второго split, ≈31.6 м | 336,156 | 352,156 | 23.67 / 19.00 / 13.44 | 12 / 9 / 6 |
| Третий каскад, ≈69.8 м | 425,136 | 440,136 | 18.67 / 15.22 / 11.33 | 9 / 7 / 5 |
| Четвёртый каскад, ≈85.2 м | 425,133 | 439,133 | 11.56 / 9.89 / 7.78 | 6 / 5 / 4 |

Reference взят из `cascade-range-d32/1-partial-start.png` нативного Debug запуска.
Отдельный CPU ray/AABB расчёт для геометрии fixture подтвердил: все девять rays
каждой shadow-пробы видят пол, а не грань caster. Ближние две пробы целиком
перекрыты тенью; последние три содержат три shadowed floor samples из девяти.
Это объясняет меньшие пороги дальних проб при сохранении запаса до измеренного
контраста. Извлечённый из скрипта контракт прошёл на этом существующем PNG и
отклонил модель равномерно освещённого пола без теней. Это CPU-проверка проверки,
а не новый GPU/ASan прогон.

`initial_shadow_contrast_checked=true` записывается только после прохождения
всех пяти проб. Pixel contract устанавливает наличие реальной тени в каждом
диапазоне CSM, но не доказывает отсутствие acne, отрыва контакта или скачка на
каждом пикселе blend band. Ряды 170/188 м присутствуют в captures, однако
отдельного надёжного pixel assertion для границы покрытия 192 м при 640×360 нет.
Для D16 и низкого солнца нужны соответствующие runtime captures; приведённые
reference числа сами по себе не означают, что эти дополнительные прогоны прошли.

После добавления контракта выполнены native Debug D32 и D16 по 1 startup ×
1 cycle (56 captures каждый), ASan D16 1 startup × 2 cycles (108 captures).
Все пять проб прошли; cache/full equality при AA off/SMAA, partial work,
часы 7/17/14 и lifecycle checks прошли без validation errors и живых GPU allocations.
Сравнение исходной позы D32/D16: mean absolute RGB8 error 0.00448, max channel
error 18; 0.11545% pixels отличаются больше чем на 1, 0.00260% — больше чем на 10.
Это локальная проверка данного fixture, а не гарантия одинакового качества
форматов на произвольной геометрии. D32 остаётся первым выбором `auto`.
