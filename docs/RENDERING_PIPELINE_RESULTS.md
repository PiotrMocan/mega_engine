# Rendering pipeline: измеренные результаты P1–E18

Начат 2026-09-12, обновлён результатами E18 2026-09-13. Отчёт разделяет
проверенный снимок P1 (reverse-Z, audit clears,
переключаемый display mapping), E05 cascade cache и последующий D16/D32 snapshot.
Последующие разделы добавляют самостоятельные серии вплоть до E18. Каждая серия
подтверждает только перечисленные engine paths и fixtures; итоговая матрица не
считается production content QA.

В завершённой серии из 42 запусков медиана GPU frame уменьшилась во всех семи
нагрузках на 0.20–3.16%; медиана FPS выросла на 0.11–7.61%. Самые убедительные
сопоставимые данные — статичные пары с одинаковой геометрией. Разброс CPU/present
и 1% low заметен; весь прирост FPS нельзя приписать выполнению shaders.

**Все after-запуски этой серии использовали Reinhard. Это не замер скорости GT7.**
В старом before JSON поле `tone_mapper` отсутствует: baseline использует прежний
фиксированный Reinhard. Его отсутствие сохранено как null в машинной сводке.
Отдельная завершённая серия ToneMapping добавлена ниже и рассматривается
самостоятельно: её GT7 результаты не подмешаны в 42 before/after запуска.

## Условия и воспроизводимость

Источник — ровно 42 файла `before|after-<case>-1|2|3.json` в
`build/windows-release/rendering-pipeline-ab-20260912`; служебный
`benchmark.json` исключён. Все JSON прочитаны и проверены на три повтора каждой
стороны. [Машинная сводка](C:/GDF/vk_test/build/windows-release/rendering-pipeline-ab-20260912/RenderingPipeline-summary.json)
содержит исходные значения, median/min/max, абсолютные/процентные изменения,
metadata и SHA-256 входных файлов.
[Environment snapshot](C:/GDF/vk_test/build/windows-release/rendering-pipeline-ab-20260912/RenderingPipeline-environment.json)
зафиксирован в 18:00:36 UTC.

| Параметр | Значение |
| --- | --- |
| CPU / GPU | Ryzen 7 9700X / Radeon RX 6800 |
| Windows driver / Vulkan driver ID | 32.0.21045.5002 / 8388961 |
| Build / present | Native Windows Release / IMMEDIATE во всех 42 JSON |
| Display resolution | 1920×1080 |
| Render resolution | 1920×1080 для off/SMAA; 1280×720 для FSR 3.1.4 quality |
| Повторы | 3 before + 3 after на нагрузку |
| Прогрев / измерение | 5 секунд / около 10 секунд на запуск |
| Lighting | Dynamic, time-of-day 14, day-cycle off, shadow atlas 2048 |
| Static cases | Камера и объекты неподвижны; GI off/voxel × AA off/SMAA/FSR |
| Stress case | GI off, AA off, 100000 source instances, animated camera, GPU occlusion |
| Power / clocks | Balanced; driver-controlled clocks и temperature не фиксировались/не семплировались |

Фоновые desktop processes записаны в environment, но не изолированы. Benchmarks
не измеряли hardware bandwidth counters, ETW/PresentMon, display latency или
фиксированные частоты. FPS здесь характеризует render-loop throughput, а не
частоту реально показанных монитором кадров.

## Сводка изменений медиан

`Δ = 100 × (after / before − 1)`. Для FPS положительное изменение лучше,
для времени отрицательное лучше. Проценты рассчитываются до округления.
Нулевая исходная метрика даёт «—», а не деление на ноль.

| Нагрузка | FPS Δ | 1% low Δ | CPU frame Δ | GPU frame Δ | GPU geometry Δ | CPU present Δ |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| GI off / AA off | +5.49% | +6.74% | -5.20% | -3.16% | -0.12% | -3.55% |
| GI off / SMAA high | +0.56% | +13.77% | -0.55% | -0.51% | -0.16% | +1.48% |
| GI off / FSR quality | +1.73% | +4.82% | -1.70% | -0.95% | -0.18% | -3.07% |
| Voxel GI / AA off | +1.60% | +0.44% | -1.57% | -0.90% | -0.05% | -0.03% |
| Voxel GI / SMAA high | +0.11% | -1.56% | -0.11% | -0.20% | -0.05% | -0.20% |
| Voxel GI / FSR quality | +0.36% | +1.95% | -0.36% | -0.89% | -0.31% | -1.20% |
| 100k / GPU culling / camera motion | +7.61% | +1.83% | -7.07% | -2.95% | +0.71% | -4.48% |

## Интерпретация и ограничения

В статичных AA-off парах GPU frame сократился примерно на 0.0090 ms: GI off
0.28631 → 0.27726 ms, voxel GI 0.99965 → 0.99065 ms. Диапазоны трёх GPU frame
средних в этих парах не пересекаются, а GPU geometry почти неизменен
(−0.00031 и −0.00047 ms). Это согласуется с уменьшением стоимости вне geometry,
но отдельного timestamp вокруг clear/display pass и hardware counters нет:
**изолированная стоимость удалённого clear или reverse-Z не установлена**.

Для SMAA изменения GPU frame меньше: −0.00176 ms без GI и −0.00210 ms с GI.
В FSR: −0.00680 и −0.00929 ms. В некоторых режимах диапазоны перекрываются;
три повтора без контроля clocks недостаточны для универсального обещания
такого улучшения на другом запуске или GPU.

CPU present занимает большую часть CPU frame. Например, GI-off/AA-off:
present 0.34935 → 0.33693 ms, fence wait 0.02197 → 0.00493 ms; FPS при этом
+5.49%, а GPU frame −3.16%. Stress: present 0.34420 → 0.32879 ms и fence
0.01370 → 0.00538 ms. Это CPU-side waits/API timings, которые могут зависеть
от GPU back-pressure, presentation scheduling, driver и desktop compositor.
**Причина «именно DWM» не доказана** без presentation trace. Нельзя складывать
CPU present с GPU frame как независимые последовательные этапы или суммировать
независимые медианы breakdown для восстановления медианы полного кадра.

1% low имеет широкий разброс: например, GI-off/SMAA before 668.9–1334.0 FPS,
after 817.1–1436.1 FPS. Показанные изменения медиан сохраняют этот разброс,
не являются доверительным интервалом и не доказывают исчезновение stutter.

Статичные pairs совпадают по source/visible/frustum/occlusion counters:
324 / 252 / 72 / 0. Triangles тоже совпадают внутри пары: 234868 для off/SMAA,
233834 для FSR с его меньшим render resolution и соответствующим LOD. Поэтому
сравнение FSR с native AA — другая quality/workload задача, а не этот before/after.

**У stress case геометрия отличается.** Final visible instances:
before 21632–21634, after 21993–21994; triangles: 259585–259609 → 263917–263929
(около +1.67%). Frustum rejects одинаковы (18678), occlusion rejects:
59688–59690 → 59328–59329. GPU geometry вырос на 0.71%, хотя полный GPU frame
сократился на 2.95%. After не ускорился за счёт меньшего показанного числа
triangles, однако эти final counters не описывают весь кадр-за-кадром workload.
Camera motion зависит от elapsed time; новые conservative проверки обоих
clip planes также могут сохранять дополнительные bounds. Причина разницы без
fixed-pose эксперимента не установлена. Stress результат не объявляется
изолированным сравнением одинаковой геометрии. Shadow timing −7.18% здесь
также не доказывает новую per-cascade cache оптимизацию: E05 в серию не входил.

## Детальные значения

Каждая ячейка ниже: **median [min–max]** трёх per-run средних; для 1% low —
трёх рассчитанных внутри отдельных прогонов значений 1% low. Все timing метрики
в ms. Это не min/max отдельных кадров и не общий percentile объединённых кадров.
`percentile_scope=full_measurement` подтверждён во всех входных JSON.

### GI off / AA off — `static-off-off`

| Метрика | Before | After | Δ |
| --- | ---: | ---: | ---: |
| FPS | 2249.1 [2140.6–2276.4] | 2372.5 [2214.7–2410.3] | +5.49% |
| 1% low FPS | 1163.4 [1101.0–1606.1] | 1241.9 [1096.3–1613.2] | +6.74% |
| CPU frame | 0.44463 [0.43928–0.46716] | 0.42149 [0.41489–0.45153] | -5.20% |
| GPU frame | 0.28631 [0.28599–0.28756] | 0.27726 [0.27698–0.27794] | -3.16% |
| CPU fence wait | 0.02197 [0.01816–0.02278] | 0.00493 [0.00482–0.00516] | -77.54% |
| CPU acquire | 0.01742 [0.01724–0.02296] | 0.01785 [0.01721–0.02208] | +2.45% |
| CPU command record | 0.01736 [0.01680–0.02210] | 0.01552 [0.01524–0.02068] | -10.63% |
| CPU queue submit | 0.01360 [0.01327–0.02931] | 0.01281 [0.01256–0.02423] | -5.85% |
| CPU present | 0.34935 [0.34549–0.34987] | 0.33693 [0.33060–0.34014] | -3.55% |
| CPU scene prepare | 0.00072 [0.00070–0.00118] | 0.00066 [0.00064–0.00134] | -8.11% |
| GPU geometry | 0.25512 [0.25504–0.25520] | 0.25481 [0.25429–0.25482] | -0.12% |
| GPU shadow | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU voxel geometry | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU voxel light | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU visibility | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |

### GI off / SMAA high — `static-off-smaa`

| Метрика | Before | After | Δ |
| --- | ---: | ---: | ---: |
| FPS | 1886.7 [1875.8–2050.4] | 1897.2 [1896.0–1936.7] | +0.56% |
| 1% low FPS | 1183.5 [668.9–1334.0] | 1346.4 [817.1–1436.1] | +13.77% |
| CPU frame | 0.53003 [0.48771–0.53311] | 0.52710 [0.51634–0.52743] | -0.55% |
| GPU frame | 0.34389 [0.34282–0.35265] | 0.34213 [0.34206–0.34697] | -0.51% |
| CPU fence wait | 0.02204 [0.02001–0.02293] | 0.02049 [0.01944–0.02229] | -7.06% |
| CPU acquire | 0.01907 [0.01718–0.02475] | 0.02127 [0.01920–0.02318] | +11.57% |
| CPU command record | 0.02165 [0.01968–0.02698] | 0.02076 [0.01988–0.02365] | -4.15% |
| CPU queue submit | 0.01367 [0.01280–0.02940] | 0.02999 [0.01372–0.03114] | +119.37% |
| CPU present | 0.40386 [0.38876–0.43165] | 0.40985 [0.40529–0.41688] | +1.48% |
| CPU scene prepare | 0.00080 [0.00073–0.00119] | 0.00084 [0.00069–0.00103] | +5.20% |
| GPU geometry | 0.25537 [0.25408–0.25624] | 0.25496 [0.25490–0.25524] | -0.16% |
| GPU shadow | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU voxel geometry | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU voxel light | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU visibility | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |

### GI off / FSR quality — `static-off-fsr3`

| Метрика | Before | After | Δ |
| --- | ---: | ---: | ---: |
| FPS | 1098.2 [1097.0–1099.3] | 1117.1 [1111.9–1133.7] | +1.73% |
| 1% low FPS | 853.7 [578.4–916.5] | 894.8 [725.0–910.1] | +4.82% |
| CPU frame | 0.91060 [0.90965–0.91159] | 0.89515 [0.88207–0.89937] | -1.70% |
| GPU frame | 0.71247 [0.71197–0.72322] | 0.70568 [0.70486–0.70582] | -0.95% |
| CPU fence wait | 0.02077 [0.01823–0.02178] | 0.01352 [0.00952–0.01673] | -34.92% |
| CPU acquire | 0.02382 [0.02266–0.03310] | 0.02355 [0.02320–0.03053] | -1.14% |
| CPU command record | 0.08439 [0.07825–0.09230] | 0.08186 [0.07972–0.09090] | -3.00% |
| CPU queue submit | 0.01612 [0.01430–0.01706] | 0.01609 [0.01357–0.01774] | -0.20% |
| CPU present | 0.74216 [0.72086–0.74257] | 0.71937 [0.70988–0.73287] | -3.07% |
| CPU scene prepare | 0.00131 [0.00127–0.00225] | 0.00140 [0.00134–0.00217] | +7.29% |
| GPU geometry | 0.17572 [0.17572–0.17615] | 0.17541 [0.17526–0.17562] | -0.18% |
| GPU shadow | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU voxel geometry | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU voxel light | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU visibility | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |

### Voxel GI / AA off — `static-voxel-off`

| Метрика | Before | After | Δ |
| --- | ---: | ---: | ---: |
| FPS | 832.6 [830.8–843.4] | 845.9 [844.4–858.4] | +1.60% |
| 1% low FPS | 721.9 [588.8–732.7] | 725.1 [601.6–734.4] | +0.44% |
| CPU frame | 1.20111 [1.18570–1.20371] | 1.18221 [1.16495–1.18427] | -1.57% |
| GPU frame | 0.99965 [0.99907–0.99989] | 0.99065 [0.99002–0.99092] | -0.90% |
| CPU fence wait | 0.01815 [0.01543–0.02016] | 0.00807 [0.00587–0.00846] | -55.54% |
| CPU acquire | 0.02901 [0.02311–0.02937] | 0.02676 [0.02510–0.02678] | -7.76% |
| CPU command record | 0.02876 [0.02247–0.02971] | 0.02199 [0.02052–0.02474] | -23.55% |
| CPU queue submit | 0.02749 [0.01627–0.02956] | 0.02660 [0.01522–0.02740] | -3.24% |
| CPU present | 1.06803 [1.06771–1.08504] | 1.06767 [1.05828–1.06885] | -0.03% |
| CPU scene prepare | 0.00180 [0.00137–0.00184] | 0.00143 [0.00130–0.00150] | -20.74% |
| GPU geometry | 0.96749 [0.96741–0.96777] | 0.96702 [0.96694–0.96714] | -0.05% |
| GPU shadow | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU voxel geometry | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU voxel light | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU visibility | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |

### Voxel GI / SMAA high — `static-voxel-smaa`

| Метрика | Before | After | Δ |
| --- | ---: | ---: | ---: |
| FPS | 793.5 [791.2–801.9] | 794.3 [793.4–806.2] | +0.11% |
| 1% low FPS | 635.4 [557.6–683.1] | 625.4 [559.7–691.9] | -1.56% |
| CPU frame | 1.26031 [1.24703–1.26395] | 1.25895 [1.24044–1.26034] | -0.11% |
| GPU frame | 1.05509 [1.05439–1.05554] | 1.05300 [1.05229–1.05338] | -0.20% |
| CPU fence wait | 0.02349 [0.01938–0.02361] | 0.02365 [0.01792–0.02379] | +0.70% |
| CPU acquire | 0.02456 [0.02302–0.03111] | 0.02538 [0.02411–0.03042] | +3.37% |
| CPU command record | 0.02581 [0.02420–0.03408] | 0.02461 [0.02335–0.03115] | -4.65% |
| CPU queue submit | 0.02862 [0.01624–0.02972] | 0.02943 [0.01624–0.03032] | +2.84% |
| CPU present | 1.13303 [1.11963–1.13852] | 1.13075 [1.11783–1.13235] | -0.20% |
| CPU scene prepare | 0.00124 [0.00124–0.00177] | 0.00117 [0.00110–0.00166] | -5.67% |
| GPU geometry | 0.96765 [0.96742–0.96781] | 0.96717 [0.96715–0.96727] | -0.05% |
| GPU shadow | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU voxel geometry | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU voxel light | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU visibility | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |

### Voxel GI / FSR quality — `static-voxel-fsr3`

| Метрика | Before | After | Δ |
| --- | ---: | ---: | ---: |
| FPS | 803.1 [798.8–807.7] | 806.0 [786.6–814.7] | +0.36% |
| 1% low FPS | 562.0 [459.1–695.3] | 573.0 [503.2–690.9] | +1.95% |
| CPU frame | 1.24524 [1.23808–1.25186] | 1.24076 [1.22749–1.27134] | -0.36% |
| GPU frame | 1.04669 [1.04389–1.07360] | 1.03740 [1.03725–1.08966] | -0.89% |
| CPU fence wait | 0.02098 [0.01601–0.02145] | 0.01546 [0.01262–0.02108] | -26.31% |
| CPU acquire | 0.02484 [0.02322–0.03022] | 0.02792 [0.02371–0.02986] | +12.37% |
| CPU command record | 0.08228 [0.08212–0.09654] | 0.08625 [0.08150–0.09451] | +4.83% |
| CPU queue submit | 0.01498 [0.01377–0.01889] | 0.01508 [0.01414–0.01913] | +0.69% |
| CPU present | 1.07717 [1.05311–1.07907] | 1.06425 [1.04451–1.10422] | -1.20% |
| CPU scene prepare | 0.00185 [0.00162–0.00231] | 0.00190 [0.00147–0.00212] | +2.75% |
| GPU geometry | 0.50466 [0.50253–0.50739] | 0.50312 [0.50233–0.50523] | -0.31% |
| GPU shadow | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU voxel geometry | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU voxel light | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU visibility | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |

### 100k / GPU culling / camera motion — `stress-gpu`

| Метрика | Before | After | Δ |
| --- | ---: | ---: | ---: |
| FPS | 2074.5 [2063.8–2155.8] | 2232.3 [2138.3–2282.1] | +7.61% |
| 1% low FPS | 905.0 [879.3–912.5] | 921.6 [834.6–923.2] | +1.83% |
| CPU frame | 0.48205 [0.46386–0.48454] | 0.44797 [0.43820–0.46767] | -7.07% |
| GPU frame | 0.30155 [0.29995–0.30171] | 0.29264 [0.29189–0.29352] | -2.95% |
| CPU fence wait | 0.01370 [0.01290–0.01450] | 0.00538 [0.00485–0.00593] | -60.71% |
| CPU acquire | 0.02157 [0.01930–0.02420] | 0.01904 [0.01720–0.02375] | -11.75% |
| CPU command record | 0.01886 [0.01751–0.01894] | 0.01663 [0.01479–0.01711] | -11.80% |
| CPU queue submit | 0.02397 [0.01252–0.02638] | 0.01245 [0.01239–0.02208] | -48.06% |
| CPU present | 0.34420 [0.34029–0.34655] | 0.32879 [0.32551–0.33188] | -4.48% |
| CPU scene prepare | 0.02730 [0.02596–0.02943] | 0.02704 [0.02541–0.03117] | -0.95% |
| GPU geometry | 0.20814 [0.20810–0.20815] | 0.20962 [0.20954–0.20963] | +0.71% |
| GPU shadow | 0.03329 [0.03185–0.03344] | 0.03090 [0.03044–0.03224] | -7.18% |
| GPU voxel geometry | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU voxel light | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU visibility | 0.03010 [0.03006–0.03011] | 0.03008 [0.03004–0.03012] | -0.05% |

Нулевые shadow/voxel-update timings в static cases относятся к steady-state
после прогрева: содержимое кешей используется при shading, но update passes
не выполнялись в измеряемом интервале. Это не «бесплатные тени/GI» и не замер
startup/cook/update latency. Synthetic compute workload в этих cases выключен.

## Подтверждённые проверки P1

Наличие перечисленных логов и содержащиеся в них результаты проверены при
подготовке отчёта. Они относятся к снимку P1, собранному до текущих последующих
правок; изменённый после этого исходник не наследует статус пройденной проверки.

| Проверка | Подтверждение / ограничение |
| --- | --- |
| Native Debug build + CTest | 6/6, [лог](C:/GDF/vk_test/build/windows-debug/rendering-pipeline-build.log), 17:53 UTC |
| Native Release build + CTest | 6/6, [лог](C:/GDF/vk_test/build/windows-release/rendering-pipeline-build.log), 17:56 UTC |
| Native ASan build + CTest | 6/6, [лог](C:/GDF/vk_test/build/windows-asan/rendering-pipeline-build.log), 17:56 UTC |
| Final Debug rendering-pipeline validation | `Startups=1, Cycles=2`, 28 captures текущего startup; [startup-1.log](C:/GDF/vk_test/build/windows-debug/render-validation/rendering-pipeline/startup-1.log), 17:58 UTC |
| Final ASan rendering-pipeline validation | `Startups=1, Cycles=2`, 28 captures; [startup-1.log](C:/GDF/vk_test/build/windows-asan/render-validation/rendering-pipeline/startup-1.log), 18:00 UTC |
| GPU GT7 reference | 19 samples passed, absolute tolerance ≤5e-4; [лог](C:/GDF/vk_test/build/windows-debug/tone-reference-runtime.log) |
| Narrow clang-tidy | `fsr3_upscaler.cpp` и `scene_visibility.cpp`; [лог](C:/GDF/vk_test/build/windows-debug/rendering-pipeline-tidy.log), есть bounds/easily-swappable warnings; это не warning-clean full-project analysis |
| Baseline/current geometry и images | Четыре PNG и соответствующие `*-state.json` в `build/windows-release/rendering-pipeline-20260912`; наличие проверено, визуальная сверка выполнена основным исполнителем |

Последние Debug/ASan validation логи завершаются `Live GPU allocations: 0
(0 bytes)`. В них есть FidelityFX warning о `frameTimeDelta < 1 ms`; при
sub-millisecond кадрах это heuristic warning SDK, само по себе не свидетельство
передачи секунд вместо миллисекунд. В том же Debug output каталоге остались
артефакты более ранних startup-2/3; их 84 суммарных PNG не выдаются за 84 captures
финального запуска после исправления sky binary.

Ранний P1 лог печатал `max absolute error=0.00` с недостаточной точностью;
этот вывод подтверждал лишь прохождение порога ≤5e-4. После пересборки и
повторной GPU-проверки в снимке E05 обновлённый лог показывает **19 samples,
max absolute error = 1.817941666e-05**. Нулевая численная ошибка не утверждается.

Read-only review reverse-Z не обнаружил рассогласования CPU/GPU occlusion,
camera/sky/FSR depth contract или final-attachment coverage. Само чтение кода
не заменяет runtime validation. Исправление stale `sky.vert.spv` включено в
этот P1 снимок; результаты узкого аудита ещё трёх перекомпилированных SPIR-V
приведены ниже. Проверки более поздних изменений учитываются отдельно.

## Отдельная серия: Reinhard → GT7

Серия ToneMapping завершена: 18 JSON, три запуска каждого оператора для
AA off, SMAA high и FSR quality. Это **тот же P1 executable**, GI off,
неподвижная сцена/камера, fixed exposure, time-of-day 14, day-cycle off,
1920×1080 display, 5 секунд прогрева и 10 секунд измерения. Environment snapshot:
18:15:34 UTC. [Машинная сводка ToneMapping](C:/GDF/vk_test/build/windows-release/tone-mapping-ab-20260912/ToneMapping-summary.json)
сохраняет все исходные значения и hashes. Служебный `benchmark.json` исключён.

GT7 ожидаемо **дороже Reinhard**: медиана полного GPU frame выросла на
0.03625–0.04360 ms, или 5.62–15.73% в этих очень коротких кадрах. Медиана FPS
снизилась на 4.71–11.18%. GPU geometry практически неизменен; в каждом AA mode
совпадают source/visible/triangle counters. Раздельного postprocess timestamp
эти P1 JSON не имеют: приведена разница **полного GPU frame** при переключении
оператора, не прямой timing отдельного GT7 shader.

| AA mode | GPU frame Reinhard, ms | GPU frame GT7, ms | GPU Δ, ms | GPU Δ | FPS Δ |
| --- | ---: | ---: | ---: | ---: | ---: |
| off | 0.27718 [0.27707–0.27727] | 0.32079 [0.32060–0.32105] | +0.04360 | +15.73% | -11.18% |
| smaa | 0.34196 [0.34179–0.34221] | 0.37822 [0.37803–0.37836] | +0.03625 | +10.60% | -5.95% |
| fsr3 | 0.70599 [0.70585–0.70639] | 0.74565 [0.74565–0.74629] | +0.03966 | +5.62% | -4.71% |

GT7 сохранён как явная quality option с официальным SDR/chroma contract.
Этот замер не является обоснованием менять default Reinhard, обещать экономию
GPU или сравнивать визуальное качество только по FPS. Разница яркости при
одинаковом exposure является свойством операторов и не скрывалась адаптацией.

### ToneMapping / off

| Метрика | Reinhard: median [min–max] | GT7: median [min–max] | Δ |
| --- | ---: | ---: | ---: |
| FPS | 2286.4 [2274.5–2302.2] | 2030.8 [2030.1–2047.9] | -11.18% |
| 1% low FPS | 1217.0 [1175.5–1237.6] | 1061.7 [1043.1–1093.6] | -12.76% |
| CPU frame | 0.43738 [0.43437–0.43965] | 0.49243 [0.48830–0.49258] | +12.59% |
| GPU frame | 0.27718 [0.27707–0.27727] | 0.32079 [0.32060–0.32105] | +15.73% |
| CPU fence wait | 0.00600 [0.00515–0.00605] | 0.01607 [0.01591–0.01735] | +167.79% |
| CPU acquire | 0.01931 [0.01887–0.01966] | 0.02232 [0.02015–0.02246] | +15.61% |
| CPU command record | 0.01785 [0.01766–0.01906] | 0.02050 [0.01814–0.02057] | +14.85% |
| CPU queue submit | 0.01324 [0.01311–0.01335] | 0.01421 [0.01358–0.01431] | +7.36% |
| CPU present | 0.35135 [0.34366–0.35238] | 0.39518 [0.39459–0.39557] | +12.47% |
| CPU scene prepare | 0.00076 [0.00074–0.00083] | 0.00090 [0.00075–0.00091] | +18.55% |
| GPU geometry | 0.25519 [0.25509–0.25526] | 0.25525 [0.25508–0.25545] | +0.02% |

### ToneMapping / smaa

| Метрика | Reinhard: median [min–max] | GT7: median [min–max] | Δ |
| --- | ---: | ---: | ---: |
| FPS | 1941.7 [1938.7–1955.9] | 1826.1 [1819.7–1830.5] | -5.95% |
| 1% low FPS | 1049.5 [1026.0–1087.1] | 1015.1 [990.3–1049.6] | -3.28% |
| CPU frame | 0.51502 [0.51128–0.51582] | 0.54760 [0.54631–0.54953] | +6.33% |
| GPU frame | 0.34196 [0.34179–0.34221] | 0.37822 [0.37803–0.37836] | +10.60% |
| CPU fence wait | 0.01840 [0.01800–0.02014] | 0.01685 [0.01584–0.01756] | -8.39% |
| CPU acquire | 0.02264 [0.01984–0.02302] | 0.02255 [0.02067–0.02323] | -0.39% |
| CPU command record | 0.02398 [0.02078–0.02432] | 0.02367 [0.02172–0.02455] | -1.32% |
| CPU queue submit | 0.01435 [0.01339–0.01450] | 0.01420 [0.01328–0.01459] | -1.02% |
| CPU present | 0.41271 [0.41126–0.41496] | 0.44648 [0.44620–0.44978] | +8.18% |
| CPU scene prepare | 0.00091 [0.00074–0.00095] | 0.00092 [0.00078–0.00095] | +0.93% |
| GPU geometry | 0.25515 [0.25503–0.25532] | 0.25516 [0.25507–0.25523] | +0.00% |

### ToneMapping / fsr3

| Метрика | Reinhard: median [min–max] | GT7: median [min–max] | Δ |
| --- | ---: | ---: | ---: |
| FPS | 1131.1 [1126.0–1132.1] | 1077.8 [1071.4–1080.6] | -4.71% |
| 1% low FPS | 685.3 [673.1–712.5] | 653.2 [653.0–677.9] | -4.68% |
| CPU frame | 0.88410 [0.88335–0.88811] | 0.92782 [0.92543–0.93340] | +4.94% |
| GPU frame | 0.70599 [0.70585–0.70639] | 0.74565 [0.74565–0.74629] | +5.62% |
| CPU fence wait | 0.01561 [0.01525–0.01751] | 0.01758 [0.01674–0.01827] | +12.57% |
| CPU acquire | 0.02634 [0.02316–0.02695] | 0.02687 [0.02502–0.02802] | +2.01% |
| CPU command record | 0.08855 [0.08166–0.08903] | 0.08829 [0.08601–0.09068] | -0.30% |
| CPU queue submit | 0.01415 [0.01350–0.01426] | 0.01456 [0.01410–0.01457] | +2.90% |
| CPU present | 0.71113 [0.70897–0.72511] | 0.75378 [0.74782–0.76305] | +6.00% |
| CPU scene prepare | 0.00175 [0.00137–0.00176] | 0.00170 [0.00147–0.00176] | -3.16% |
| GPU geometry | 0.17566 [0.17562–0.17579] | 0.17590 [0.17586–0.17594] | +0.13% |

Все shadow/voxel-update/GPU-visibility timings в этой статичной серии равны
0; они сохранены в машинной сводке. Ограничения контроля clocks/desktop
scheduling и трактовки CPU present из предыдущего раздела остаются в силе.

## Узкий аудит перекомпилированных SPIR-V

Native `spirv-dis` сопоставил HEAD и полученные полной перекомпиляцией binaries `cube.vert.spv`,
`cube.frag.spv`, `workload.comp.spv`. Read-only артефакты находятся в
`build/windows-release/rendering-pipeline-20260912/spv-audit`; tracked shader
файлы этим аудитом не изменялись.

- Для cube vertex/fragment тела инструкций совпали. Разница — glslang generator
  10 → 11, порядок decorations и добавленный `NonWritable` на storage-buffer
  variable при уже существовавшем readonly member. Новая математическая работа
  в этих двух shaders не обнаружена.
- Для workload compute изменилось оптимизированное представление: раньше
  `mixBits` был inline с SSA/`OpPhi`, теперь остались function calls и локальные
  `OpLoad/OpStore`. Константы, hash/iteration формулы и output expression по
  дизассемблированному потоку соответствуют исходному алгоритму, но это
  **не только изменение имён**. Поэтому прежняя optimization policy была
  восстановлена отдельно, без смены flags остальных shaders.
- Все 42 RenderingPipeline и 18 ToneMapping JSON имеют
  `gpu_workload_iterations=0`: этот compute workload в анализируемых сериях
  не исполнялся. Его IR-разница не объясняет измеренные здесь GPU deltas.

Дополнительная native компиляция только `workload.comp` с `-Os` тем же
`C:/VulkanSDK/1.4.313.2/Bin/glslangValidator.exe` восстановила **точно совпадающий
с HEAD поток types/constants/functions**, начиная с `%void = OpTypeVoid` до
конца disassembly. Остались только generator 10 → 11 и порядок decorations.
Проверены Vulkan 1.3 / SPIR-V 1.5 и `spirv-val`; binary теперь 3316 bytes.
`scripts/compile_shaders_windows.ps1` применяет `-Os` только к workload compute,
соответствующий versioned SPIR-V обновлён. Остальные shaders и benchmark flags
этим исправлением не менялись. GPU-сравнение saturation в этом аудите не
выполнялось; parent build/copy после исправления учитывается отдельно.

Новая отдельная GPU postprocess timing метрика появилась в последующем E05
build; в завершённых P1 RenderingPipeline/ToneMapping JSON её нет. Более поздние
ShadowCache timings и отдельные ToneMapping RGP captures описаны ниже.

## E05: отдельная серия full → cascade cache

Источник — ровно 12 файлов `{canonical,stress-gpu}-{full,cascade}-{1,2,3}.json` в
`build/windows-release/shadow-cache-ab-20260912`; служебный `benchmark.json`
исключён. [Машинная сводка](C:/GDF/vk_test/build/windows-release/shadow-cache-ab-20260912/ShadowCache-summary.json)
содержит исходные значения, median/min/max, изменения, metadata и SHA-256 всех
12 результатов и environment. [Environment snapshot](C:/GDF/vk_test/build/windows-release/shadow-cache-ab-20260912/ShadowCache-environment.json)
зафиксирован в 18:31:11 UTC. Это сравнение двух runtime policies **одного E05
Release executable**, отдельно от предыдущих before/after снимков P1.

Условия: RX 6800 / Ryzen 7 9700X, тот же driver, 1920×1080 render/display,
IMMEDIATE, 5 секунд прогрева и около 10 секунд измерения, по три повтора каждой
policy в двух нагрузках. GI off, AA off, Reinhard, dynamic lighting, время 14:00,
day-cycle off, atlas 2048. В обеих нагрузках камера анимирована; canonical
содержит 324 source instances и CPU visibility, stress — 100000 и GPU visibility.
Balanced, clocks/temperature не фиксировались и не семплировались.

**Default остаётся `full`; per-cascade cache включается явно через
`--shadow-cache cascade`.** `full` обновляет весь atlas при его invalidation,
а не обязательно каждый кадр. `cascade` позволяет сохранять неизменившиеся
tiles и соответствующие matrices при движении камеры; общая invalidation
scene/resources по-прежнему требует полного обновления.

В canonical медиана GPU frame уменьшилась на **0.00339 ms (−1.27%)**, из них
GPU shadow — на **0.00303 ms (−66.50%)**. В 100k stress уменьшение GPU frame
составило **0.02498 ms (−8.49%)**, GPU shadow — **0.02486 ms (−76.32%)**.
Диапазоны GPU frame и GPU shadow по policies здесь не перекрываются;
почти весь GPU delta совпадает с сокращением shadow timing. Geometry и
postprocess меняются мало. Это поддерживает пользу частичного обновления
в этих двух нагрузках с неподвижными casters и движущейся камерой.

FPS меняется на +0.43% / +3.98%; 1% low — на +3.77% / +24.38%. При этом
CPU present занимает около 0.33–0.35 ms и уменьшается на 0.00207 / 0.02137 ms.
Это CPU API/wait timing: без ETW/PresentMon нельзя отделить вклад GPU queue,
driver и DWM scheduling и приписать весь FPS/1% low эффект shadow shader.
В canonical диапазоны FPS и 1% low заметно перекрываются.

CPU command record вырос примерно на 0.00082 / 0.00078 ms (+4.27% / +4.47%),
scene prepare — на 0.00042 / 0.00087 ms (+3.67% / +3.21%); диапазоны этих
метрик перекрываются. В stress queue submit вырос на 0.00046 ms (+3.56%).
Эти небольшие CPU изменения не скрыты округлением до процентов; без CPU
профиля нельзя уверенно приписать их cache bookkeeping. Измеренный выигрыш
GPU в stress существенно больше, но universal default enable этим не доказан.

Все timings в следующих таблицах — **ms**, кроме FPS и счётчиков. Это median
и min/max **трёх per-run средних**, а для 1% low — трёх per-run percentile
результатов; min/max не являются экстремумами отдельных кадров. Формула
`100 × (cascade median / full median − 1)`; для нулевого baseline — «—».

### ShadowCache / canonical

| Метрика | full: median [min–max] | cascade: median [min–max] | Δ |
| --- | ---: | ---: | ---: |
| FPS | 2232.0 [2228.3–2342.7] | 2241.6 [2240.2–2372.7] | +0.43% |
| 1% low FPS | 1172.7 [1157.8–1214.8] | 1216.9 [1110.6–1258.6] | +3.77% |
| CPU frame | 0.44803 [0.42686–0.44877] | 0.44611 [0.42146–0.44639] | -0.43% |
| GPU frame | 0.26685 [0.26587–0.26723] | 0.26346 [0.26311–0.26409] | -1.27% |
| CPU fence wait | 0.00547 [0.00443–0.00554] | 0.00443 [0.00436–0.00537] | -19.02% |
| CPU acquire | 0.01951 [0.01870–0.01963] | 0.01937 [0.01854–0.02007] | -0.72% |
| CPU command record | 0.01927 [0.01760–0.02045] | 0.02010 [0.01717–0.02035] | +4.27% |
| CPU queue submit | 0.01347 [0.01300–0.01386] | 0.01351 [0.01306–0.01363] | +0.28% |
| CPU present | 0.34376 [0.32691–0.34602] | 0.34170 [0.32362–0.34172] | -0.60% |
| CPU scene prepare | 0.01137 [0.01107–0.01193] | 0.01179 [0.01093–0.01185] | +3.67% |
| GPU geometry | 0.24058 [0.23997–0.24096] | 0.24022 [0.24007–0.24083] | -0.15% |
| GPU shadow | 0.00456 [0.00435–0.00457] | 0.00153 [0.00144–0.00153] | -66.50% |
| GPU postprocess | 0.01990 [0.01977–0.01991] | 0.01992 [0.01981–0.01992] | +0.07% |
| GPU voxel geometry | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU voxel light | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU visibility | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| Shadow passes total | 1823 [1822–1826] | 1820 [1817–1820] | -0.16% |
| Shadow tiles total | 7292 [7288–7304] | 1846 [1846–1848] | -74.68% |
| Tiles / shadow pass | 4.00000 [4.00000–4.00000] | 1.01538 [1.01429–1.01596] | — |

### ShadowCache / stress-gpu

| Метрика | full: median [min–max] | cascade: median [min–max] | Δ |
| --- | ---: | ---: | ---: |
| FPS | 2118.1 [2112.8–2119.5] | 2202.4 [2193.1–2229.2] | +3.98% |
| 1% low FPS | 881.7 [852.3–901.0] | 1096.7 [1096.0–1222.7] | +24.38% |
| CPU frame | 0.47212 [0.47181–0.47331] | 0.45405 [0.44860–0.45598] | -3.83% |
| GPU frame | 0.29428 [0.29378–0.29439] | 0.26931 [0.26917–0.26934] | -8.49% |
| CPU fence wait | 0.00593 [0.00497–0.00594] | 0.00542 [0.00513–0.00614] | -8.60% |
| CPU acquire | 0.01931 [0.01918–0.02017] | 0.01955 [0.01875–0.02021] | +1.26% |
| CPU command record | 0.01736 [0.01723–0.01865] | 0.01814 [0.01713–0.01892] | +4.47% |
| CPU queue submit | 0.01299 [0.01291–0.01321] | 0.01345 [0.01320–0.01364] | +3.56% |
| CPU present | 0.35177 [0.34864–0.35247] | 0.33040 [0.32923–0.33423] | -6.08% |
| CPU scene prepare | 0.02713 [0.02710–0.02944] | 0.02800 [0.02726–0.02957] | +3.21% |
| GPU geometry | 0.20992 [0.20984–0.21019] | 0.20992 [0.20988–0.20996] | +0.00% |
| GPU shadow | 0.03257 [0.03237–0.03268] | 0.00771 [0.00762–0.00773] | -76.32% |
| GPU postprocess | 0.01962 [0.01961–0.01964] | 0.01966 [0.01964–0.01967] | +0.22% |
| GPU voxel geometry | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU voxel light | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU visibility | 0.03020 [0.03014–0.03023] | 0.03022 [0.03017–0.03022] | +0.05% |
| Shadow passes total | 1812 [1811–1826] | 1822 [1816–1822] | +0.55% |
| Shadow tiles total | 7248 [7244–7304] | 1848 [1847–1848] | -74.50% |
| Tiles / shadow pass | 4.00000 [4.00000–4.00000] | 1.01427 [1.01427–1.01707] | — |

Количество shadow passes почти не изменилось, а tiles уменьшилось на
74.68% / 74.50%: на pass приходится около 1.015 tile вместо четырёх.
`shadow_passes_total` и `shadow_tiles_total` — накопленные counters lifetime
Renderer, включая startup/warmup; они не сбрасываются на начало измерительного
окна. Поэтому totals нельзя делить на `frames_rendered` и объявлять точной
частотой обновлений в измеренных десяти секундах. Tiles/pass в таблице — median
и min/max отношений внутри каждого запуска.

Конечная canonical геометрия совпадает во всех шести JSON: 182 visible,
142 frustum-rejected, 0 occlusion-rejected, 160118 triangles. В stress visible:
full 21995/21993/21994, cascade 21995/21994/21994; triangles:
full 263941/263917/263929, cascade 263941/263929/263929. Камера зависит от
elapsed wall time, поэтому строгая идентичность каждого кадра и конечной позы
между отдельными benchmark-запусками не заявляется. GPU geometry medians
совпадают с точностью приведённых пяти знаков, но это не заменяет replay
фиксированных поз для дальнейших изолированных экспериментов.

### Проверки E05 cascade cache

Перечисленные файлы прочитаны; это проверки собранного cache snapshot до D16.
Проверки следующего D16/D32 snapshot приведены в отдельном разделе ниже.

| Проверка | Результат / артефакт |
| --- | --- |
| Native Debug build + CTest | 6/6; [лог](C:/GDF/vk_test/build/windows-debug/shadow-cache-build.log) |
| Native Release build + CTest | 6/6; [лог](C:/GDF/vk_test/build/windows-release/shadow-cache-build.log) |
| Native ASan build + CTest | 6/6; [лог](C:/GDF/vk_test/build/windows-asan/shadow-cache-build.log) |
| Debug shadow-cache validation | 1 startup × 1 cycle, 61 captures; [каталог](C:/GDF/vk_test/build/windows-debug/render-validation/shadow-cache) |
| ASan shadow-cache validation | 2 startups × 2 cycles, 118 captures в каждом startup, 236 всего; [каталог](C:/GDF/vk_test/build/windows-asan/render-validation/shadow-cache) |
| GT7 GPU reference после пересборки | 19 samples; max absolute error 1.817941666e-05 при допуске 5e-4; [лог](C:/GDF/vk_test/build/windows-debug/tone-reference-runtime.log) |

В startup logs нет Validation Error/VUID или AddressSanitizer error;
shutdown сообщает `Live GPU allocations: 0 (0 bytes)`. Warning FidelityFX
о `frameTimeDelta < 1 ms` остаётся. Это пройденные узкие native gates,
не полный анализ всех lifetime paths или всех будущих видов casters.

Процедурный partial-update probe выполнен с открытой крышей и проверяемым
контрастом освещённого пола и тени от коробки. RGB shadow patch = (80,97,116),
lit patch = (140,144,149): разницы R/G = 60/47 превышают требуемые 20.
Сравнение cache/full поэтому проверяет изображение с реальной видимой тенью.

| Свежий partial-work JSON | Δ shadow passes | Δ tiles | Cache/full в той же позе |
| --- | ---: | ---: | --- |
| Debug startup 1 | 11 | 14 | Все decoded pixels идентичны |
| ASan startup 1 | 13 | 17 | Все decoded pixels идентичны |
| ASan startup 2 | 9 | 10 | Все decoded pixels идентичны |

Источники — `*-partial-work.json`, `*-partial-image-check.json` и
`*-shadow-contrast.json` в указанных validation каталогах. Во всех трёх
partial probes `partial_observed=true`, `identical=true`,
`exact_required=true`: число tiles положительно и меньше четырёх на pass.
Это counters за интервал перемещения, а не число tiles одного кадра.

Round-trip/invalidations дополнительно проверяют cache/full switching,
движение камеры, AA modes, shadow resolution, время суток и изменение roof
scene. Во всех пяти `*-invalidation-coverage.json`
`roof_scene_invalidation_checked=true`. Проверка произвольного runtime
transform edit явно **false**: текущий runtime MCP profile такого command
не предоставляет. Roof scene invalidation не объявляется заменой проверки
движущихся/деформируемых casters. Для FSR temporal history/jitter exact pixel
equality не требуется скриптом; это ограничение не распространяется на
приведённые AA-off partial probes.

## Отдельные ToneMapping RGP captures: пределы hardware evidence

После исправления отсутствовавших bundled assets повторные captures успешно
сохранены: [Reinhard](C:/GDF/vk_test/build/windows-release/rendering-pipeline-20260912/tone-reinhard.rgp)
и [GT7](C:/GDF/vk_test/build/windows-release/rendering-pipeline-20260912/tone-gt7.rgp).
Соответствующие `profile-tone-{reinhard,gt7}-rdp.log` подтверждают завершение
и сохранение trace; первая неудачная попытка не считается валидным capture.
Это отдельные ToneMapping captures с AA/GI off, **не hardware counters серии
ShadowCache**.

[Извлечённые counters и SHA-256](C:/GDF/vk_test/build/windows-release/rendering-pipeline-20260912/tone-rgp-counters.json)
содержат по 43 CodeObject, 3 SqttData, 398 SpmCounterData и 22 DerivedSpmCtr
chunks. RDF index/header layout описан в
[первичной спецификации AMD RDF](https://github.com/GPUOpen-Drivers/libamdrdf/blob/main/docs/specification.md);
она описывает контейнер и offsets/compression, а не доказывает семантику
внутренних RGP counters или их привязку к render pass.

Ниже — арифметические средние derived cache-hit percentage **по всему capture**:
262 samples для Reinhard и 309 для GT7. Это не `total hits / total requests`,
не взвешенные по активности показатели и не согласованные по времени окна.

| Capture-wide counter | Reinhard sample mean | GT7 sample mean |
| --- | ---: | ---: |
| Instruction cache hit | 55.18% | 53.86% |
| Scalar cache hit | 54.92% | 53.41% |
| L0 cache hit | 46.68% | 45.19% |
| L1 cache hit | 25.98% | 21.99% |
| L2 cache hit | 37.42% | 34.22% |

У всех перечисленных counters minimum = 0; activity/idle samples и различная
длина capture смешаны. Эти данные подтверждают наличие hardware capture и
извлечённых cache counters, но **не являются per-pass анализом, доказательством
ALU bottleneck, cache bottleneck или причиной измеренного GT7 delta**.
Для таких выводов нужны привязка SQTT/SPM к postprocess interval, одинаковые
окна и релевантные execution/occupancy/stall counters. Измеренная стоимость
GT7 выше остаётся выводом отдельной серии из 18 completed JSON, а не этих
capture-wide средних.

## D16/D32: отдельная серия ShadowDepth

Прочитаны все 12 завершённых
`{2048,4096}-{d32,d16}-{1,2,3}.json` из
`build/windows-release/shadow-depth-ab-20260912`, без служебного
`benchmark.json`. [Машинная сводка](C:/GDF/vk_test/build/windows-release/shadow-depth-ab-20260912/ShadowDepth-summary.json)
содержит полные исходные JSON, порядок запусков, median/min/max, deltas,
признак пересечения диапазонов и 13 SHA-256 inputs.
[Environment](C:/GDF/vk_test/build/windows-release/shadow-depth-ab-20260912/ShadowDepth-environment.json)
записан в 18:44:29 UTC; серия завершена в 18:47:49 UTC.

Это D32/D16 переключения **одного Release executable**; сравнение выполняется
внутри каждого разрешения atlas. Условия: RX 6800 / Ryzen 7 9700X, driver
32.0.21045.5002, IMMEDIATE, 1920×1080, 5 секунд прогрева + около 10 секунд
измерения, три повтора. Canonical 324 source instances, animated camera,
dynamic lighting 14:00, day-cycle off, GI off, AA off, Reinhard,
`--shadow-cache full`. Меняются только явно запрошенный depth format и
указанное в группе разрешение atlas. Все 12 JSON подтверждают requested =
actual для соответствующего D16/D32, `shadow_linear_filter=true`.

**Ускорение D16 этой серией не установлено.** Медиана GPU frame при 2048
изменилась на −0.00034 ms (−0.11%), при 4096 — на +0.00137 ms (+0.43%).
GPU shadow: +0.000067 ms (+1.15%) и −0.000033 ms (−0.22%).
Диапазоны D16/D32 пересекаются для GPU frame, shadow, geometry, postprocess,
FPS и 1% low в обоих разрешениях. Также не установлено причинное замедление:
знак median delta на фоне такого разброса сам по себе его не доказывает.
`auto` остаётся D32 на этой GPU; D16 выбирается явно.

Clocks и temperature не фиксировались и не семплировались. Дополнительный
контроль ниже показывает существенный временной дрейф GPU timings у обеих
сборок даже с одинаковыми SPIR-V. Поэтому абсолютные цифры этой серии нельзя
напрямую сравнивать с более ранним ShadowCache A/B и объявлять разницу ценой
добавления D16. Три повтора здесь не дают надёжной оценки небольших изменений.

Как выше: timings в ms, median/min/max по трём per-run средним, 1% low по
трём per-run результатам; Δ = `100 × (d16 median / d32 median − 1)`.

### ShadowDepth / 2048

| Метрика | d32: median [min–max] | d16: median [min–max] | Δ |
| --- | ---: | ---: | ---: |
| FPS | 2087.3 [2041.1–2223.5] | 2069.2 [2000.2–2074.1] | -0.87% |
| 1% low FPS | 857.9 [768.4–863.5] | 781.2 [645.0–877.9] | -8.94% |
| CPU frame | 0.47909 [0.44975–0.48992] | 0.48329 [0.48214–0.49995] | +0.88% |
| GPU frame | 0.31770 [0.30080–0.32078] | 0.31736 [0.31299–0.31838] | -0.11% |
| CPU fence wait | 0.01143 [0.00813–0.01292] | 0.01206 [0.01098–0.01243] | +5.51% |
| CPU acquire | 0.01712 [0.01707–0.01874] | 0.01797 [0.01756–0.01978] | +5.00% |
| CPU command record | 0.01577 [0.01537–0.01837] | 0.01665 [0.01611–0.01804] | +5.60% |
| CPU queue submit | 0.01286 [0.01220–0.01437] | 0.01345 [0.01306–0.01372] | +4.59% |
| CPU present | 0.38230 [0.35509–0.38424] | 0.38428 [0.38341–0.39653] | +0.52% |
| CPU scene prepare | 0.01021 [0.01018–0.01238] | 0.01068 [0.01031–0.01077] | +4.64% |
| GPU geometry | 0.28425 [0.26946–0.28727] | 0.28403 [0.27747–0.28444] | -0.08% |
| GPU shadow | 0.00579 [0.00508–0.00598] | 0.00585 [0.00583–0.00586] | +1.15% |
| GPU postprocess | 0.02532 [0.02420–0.02546] | 0.02588 [0.02534–0.02753] | +2.20% |
| Shadow passes total | 1818 [1817–1825] | 1818 [1812–1822] | +0.00% |
| Shadow tiles total | 7272 [7268–7300] | 7272 [7248–7288] | +0.00% |

### ShadowDepth / 4096

| Метрика | d32: median [min–max] | d16: median [min–max] | Δ |
| --- | ---: | ---: | ---: |
| FPS | 2092.9 [2061.1–2184.7] | 2082.2 [2059.5–2191.2] | -0.51% |
| 1% low FPS | 842.9 [686.3–860.7] | 829.0 [818.1–834.5] | -1.64% |
| CPU frame | 0.47781 [0.45772–0.48519] | 0.48027 [0.45637–0.48555] | +0.51% |
| GPU frame | 0.31924 [0.28157–0.32315] | 0.32061 [0.30669–0.32261] | +0.43% |
| CPU fence wait | 0.00995 [0.00570–0.01104] | 0.01131 [0.00715–0.01163] | +13.67% |
| CPU acquire | 0.01726 [0.01692–0.02005] | 0.01725 [0.01720–0.01738] | -0.06% |
| CPU command record | 0.01643 [0.01621–0.01894] | 0.01635 [0.01606–0.01641] | -0.46% |
| CPU queue submit | 0.01313 [0.01293–0.01389] | 0.01272 [0.01251–0.01309] | -3.11% |
| CPU present | 0.38167 [0.35689–0.38708] | 0.38220 [0.36135–0.38760] | +0.14% |
| CPU scene prepare | 0.01039 [0.01022–0.01099] | 0.01015 [0.01013–0.01019] | -2.24% |
| GPU geometry | 0.27673 [0.24333–0.27996] | 0.27870 [0.26693–0.27964] | +0.71% |
| GPU shadow | 0.01492 [0.01265–0.01543] | 0.01488 [0.01364–0.01526] | -0.22% |
| GPU postprocess | 0.02548 [0.02375–0.02560] | 0.02490 [0.02407–0.02557] | -2.25% |
| Shadow passes total | 3528 [3486–3530] | 3520 [3515–3532] | -0.23% |
| Shadow tiles total | 14112 [13944–14120] | 14080 [14060–14128] | -0.23% |

Все GPU voxel geometry/light и visibility timings равны нулю и сохранены
в JSON summary. Конечная геометрия одинакова во всех 12 JSON: 182 visible,
142 frustum-rejected, 0 occlusion-rejected, 160118 triangles. Полная траектория
elapsed-time camera между запусками не гарантирована. Shadow totals включают
startup/warmup, по четыре tiles на pass; это не per-frame rate измерительного
окна. Рост количества invalidations при другом atlas resolution также нельзя
выдавать за изменение стоимости одного и того же shadow pass.

### Контроль дрейфа: предыдущий cache build → новый build, auto D32

В `build/windows-release/rendering-pipeline-20260912/depth-baseline-check`
завершены **все шесть** `before|after-{1,2,3}.json`. Before — сохранённый
`cache-runtime/vulkan_cube.exe`, after — D16-capable build с default
`auto → d32`, atlas 2048, cache full. Остальные arguments одинаковы:
native Release, 1920×1080 IMMEDIATE, canonical animated camera, dynamic 14:00,
GI/AA/day-cycle off, Reinhard, warmup 5 s / measurement 10 s.

[Машинная сводка контроля](C:/GDF/vk_test/build/windows-release/rendering-pipeline-20260912/depth-baseline-check/DepthBaselineCheck-summary.json)
содержит все исходные значения и 7 SHA-256 inputs;
[environment](C:/GDF/vk_test/build/windows-release/rendering-pipeline-20260912/depth-baseline-check/environment.json)
фиксирует arguments и hashes двух executable. При подготовке отчёта их hashes
по-прежнему совпали с environment; все **40 runtime SPIR-V filenames/hashes
совпадают** между before и after. Старый JSON не содержит depth-format/filter
полей: в summary они сохранены как null. Новый подтверждает requested auto,
actual d32, linear filter true.

Ни один запуск не исключён. Полный временной ряд ниже использует UTC времени
записи JSON, а не точные границы GPU measurement interval.

| Запуск | JSON записан, UTC | GPU frame, ms | GPU shadow, ms | GPU geometry, ms | GPU postprocess, ms |
| --- | --- | ---: | ---: | ---: | ---: |
| before-1.json | 18:50:21 | 0.281545 | 0.004733 | 0.241297 | 0.033628 |
| after-1.json | 18:50:38 | 0.281548 | 0.004684 | 0.241212 | 0.033775 |
| before-2.json | 18:50:55 | 0.287905 | 0.005182 | 0.249984 | 0.030820 |
| after-2.json | 18:51:11 | 0.325269 | 0.006086 | 0.285422 | 0.031533 |
| before-3.json | 18:51:28 | 0.327443 | 0.006112 | 0.286539 | 0.032556 |
| after-3.json | 18:51:44 | 0.327919 | 0.006147 | 0.287842 | 0.031675 |

Первая пара GPU frame почти совпадает (+0.0012%), во второй after выше
на 12.98%, а третья пара снова близка (+0.15%) уже при существенно большей
стоимости у обеих сборок. Выбирать только первую/третью пару как доказательство
отсутствия регрессии или только вторую как доказательство регрессии нельзя.

Формальные медианы всех трёх повторов before → after дают GPU frame +12.98%,
CPU frame +4.74%, FPS −4.52%. Эти значения оставлены в таблице без удаления
неудобных результатов. Но GPU frame внутри одной старой сборки меняется от
0.28154 до 0.32744 ms, внутри новой — от 0.28155 до 0.32792 ms; диапазоны
перекрываются. Временной дрейф виден и в geometry, а не только в shadow pass.
Это не позволяет отделить стоимость изменения кода от изменявшегося состояния
GPU/среды. **Ни причинная регрессия свыше 3%, ни гарантированное отсутствие
такой регрессии этой серией не доказаны.**

Конкретная причина дрейфа не установлена: clock/power state, temperature и
фоновые GPU consumers не измерялись синхронно. Thermal throttling не
диагностирован. CPU present/fence изменяются вместе с GPU временем, но без
ETW/PresentMon нельзя разделить scheduling, driver, DWM и очередь GPU.
Это ограничение контроля среды относится и к точности малых эффектов в
остальных локальных сериях, где clocks не были закреплены.

| Метрика | before: median [min–max] | after: median [min–max] | Δ |
| --- | ---: | ---: | ---: |
| FPS | 2114.8 [2009.8–2177.5] | 2019.1 [1997.7–2203.4] | -4.52% |
| 1% low FPS | 862.7 [640.3–875.7] | 830.8 [776.0–910.1] | -3.70% |
| CPU frame | 0.47286 [0.45925–0.49757] | 0.49526 [0.45384–0.50057] | +4.74% |
| GPU frame | 0.28791 [0.28154–0.32744] | 0.32527 [0.28155–0.32792] | +12.98% |
| CPU fence wait | 0.01266 [0.01138–0.01921] | 0.01847 [0.01138–0.01864] | +45.86% |
| CPU acquire | 0.01950 [0.01700–0.01994] | 0.01763 [0.01737–0.01957] | -9.58% |
| CPU command record | 0.01786 [0.01552–0.01825] | 0.01649 [0.01604–0.01788] | -7.71% |
| CPU queue submit | 0.01370 [0.01267–0.01374] | 0.01296 [0.01286–0.01373] | -5.46% |
| CPU present | 0.36898 [0.35501–0.39516] | 0.39155 [0.35000–0.39635] | +6.12% |
| CPU scene prepare | 0.01065 [0.00985–0.01069] | 0.01057 [0.01047–0.01069] | -0.76% |
| GPU geometry | 0.24998 [0.24130–0.28654] | 0.28542 [0.24121–0.28784] | +14.18% |
| GPU shadow | 0.00518 [0.00473–0.00611] | 0.00609 [0.00468–0.00615] | +17.45% |
| GPU postprocess | 0.03256 [0.03082–0.03363] | 0.03167 [0.03153–0.03377] | -2.71% |
| Shadow passes total | 1813 [1812–1817] | 1822 [1821–1823] | +0.50% |
| Shadow tiles total | 7252 [7248–7268] | 7288 [7284–7292] | +0.50% |

### Проверки D16/D32 и пределы проверки изображения

| Проверка | Подтверждение |
| --- | --- |
| Native Debug build + CTest | 6/6; [лог](C:/GDF/vk_test/build/windows-debug/shadow-depth-build.log) |
| Native Release build + CTest | 6/6; [лог](C:/GDF/vk_test/build/windows-release/shadow-depth-build.log) |
| Native ASan build + CTest | 6/6; [лог](C:/GDF/vk_test/build/windows-asan/shadow-depth-build.log) |
| Debug D32 shadow-cache validation | 1 startup × 1 cycle, 61 captures; [каталог](C:/GDF/vk_test/build/windows-debug/render-validation/shadow-cache-d32) |
| Debug D16 shadow-cache validation | 1 startup × 1 cycle, 61 captures; [каталог](C:/GDF/vk_test/build/windows-debug/render-validation/shadow-cache-d16) |
| ASan D16 shadow-cache validation | 1 startup × 2 cycles, 118 captures; [каталог](C:/GDF/vk_test/build/windows-asan/render-validation/shadow-cache-d16) |

Логи прочитаны: Validation Error/VUID и AddressSanitizer errors не найдены;
shutdown всех трёх runtime запусков сообщает `Live GPU allocations: 0
(0 bytes)`. Эти gates относятся к собранному D16/D32 snapshot.

[Ближнее сравнение D32/D16](C:/GDF/vk_test/build/windows-debug/render-validation/shadow-depth-near-comparison.json)
сопоставляет 640×360 `1-partial-start.png`: mean absolute 8-bit RGB difference
**0.06705**, максимальное отклонение канала **5**, доля pixels с отклонением
хотя бы одного канала >1 — **2.36285%**, >10 — **0%**. Это численная проверка
одной ближней сцены, а не точное совпадение изображений или доказательство
достаточной D16 precision на дальних cascades. Она не покрывает границы
каскадов, дальние/тонкие casters, наклонные receivers и temporal shimmer.

Затем собрана и проверена процедурная `cascade-range` fixture: Debug D32 и D16
по 56 captures (1 startup × 1 cycle), ASan D16 — 108 captures (1 × 2).
Пять начальных shadow/lit floor probes покрывают четыре каскада; все прошли.
Проверены частичные обновления, exact cache/full images при AA off/SMAA,
FSR lifecycle, часы 7/17/14, resize и 1024/2048. Validation errors: 0,
live GPU allocations: 0. [Контракт и ограничения](SHADOW_CASCADE_CACHE.md).

[Сравнение диапазонов D32/D16](C:/GDF/vk_test/build/windows-debug/render-validation/shadow-depth-range-comparison.json):
mean absolute RGB8 error 0.00448, max channel error 18, pixels >1: 0.11545%,
pixels >10: 0.00260%. Длинные тени при часе 7 дополнительно просмотрены.
Начальная pixel-проверка не доказывает отсутствие shimmer при каждом движении
или всех bias artefacts на произвольной геометрии. Профиль D16 остаётся optional,
поскольку A/B не установил ускорения.

## E06: depth prepass off / partial / full

Depth prepass остаётся **optional, default off**. В отдельной Stable-серии
на RX 6800 он увеличил GPU frame малой `overdraw-1` сцены на **3.53% / 3.65%**
для partial/full, но уменьшил GPU frame `overdraw-16` на **81.76% / 81.74%**.
Это измеренный компромисс для двух процедурных нагрузок, а не основание
включать prepass для любой сцены. В этих fixtures partial/full обрабатывают
одни и те же primitive batches; различие режимов на imported models ими
не измеряется. Контракт и ограничения coverage описаны в
[DEPTH_PREPASS.md](C:/GDF/vk_test/docs/DEPTH_PREPASS.md).

### Сопоставимость и два режима clocks

В каждой серии завершены **18 JSON**: две fixtures × off/partial/full × три
повтора. Все requested/actual и ожидаемые settings совпали. Использован один
native Windows Release executable внутри серии; CPU AMD Ryzen 7 9700X,
GPU AMD Radeon RX 6800, Windows driver 32.0.21045.5002, 1920×1080 IMMEDIATE,
warmup 5 s и measurement 10 s. Visibility CPU, frustum on, occlusion/LOD off,
camera-z 12, AA/GI off, Reinhard, dynamic lighting 14:00, day-cycle off,
shadow atlas 2048 D32, shadow-cache full, front-to-back off. Animation и
animated camera выключены. Порядок off/partial/full циклически менялся между
повторами; сцена между вариантами не изменялась.

| Fixture | Source / visible instances | Logical triangles | Frustum / occlusion rejected | LOD0 / LOD1 / LOD2 |
| --- | ---: | ---: | ---: | ---: |
| overdraw-1 | 128 / 128 | 1537 | 0 / 0 | 128 / 0 / 0 |
| overdraw-16 | 2048 / 2048 | 24577 | 0 / 0 | 2048 / 0 / 0 |

Counts идентичны во всех девяти запусках каждой fixture и между Normal/Stable.
Это логическая геометрия сцены, не Vulkan pipeline-statistics invocations:
дополнительный depth draw не увеличивает scene triangle counter. Shadow totals
во всех запусках — 1 pass / 4 tiles с учётом startup/warmup; измерительный
shadow timing равен 0. GPU visibility timing также 0 при CPU visibility.

[Normal environment](C:/GDF/vk_test/build/windows-release/depth-prepass-ab-20260912/DepthPrepass-environment.json)
фиксирует обычное управление clocks драйвером без синхронного измерения
частот/температуры. Эта серия сохранена как самостоятельный baseline;
оставшееся после RGP состояние peak не установлено: последняя завершённая
профилировка подтвердила восстановление clocks, а отдельная CLI-проверка
подтвердила Normal.

[Stable environment](C:/GDF/vk_test/build/windows-release/depth-prepass-stable-ab-20260912/DepthPrepass-environment.json)
содержит `stable_accepted=true`, `normal_restored=true`. Проверены и первичные
логи: [Normal → Stable](C:/GDF/vk_test/build/windows-release/rendering-pipeline-20260912/depth-prepass-stable-ab-20260912-stable.log)
и [Stable → Normal](C:/GDF/vk_test/build/windows-release/rendering-pipeline-20260912/depth-prepass-stable-ab-20260912-normal.log).
Драйвер сообщает Stable **1825 MHz GPU / 1000 MHz memory**. Это подтверждение
принятого режима, не per-frame telemetry частот и температуры. CPU clocks,
scheduling и фоновые процессы не зафиксированы. В CLI есть диагностическое
сообщение о query настроек DirectX 9, после которого Capture API и оба
переключения режима явно завершились успешно.

Normal и Stable **не объединяются**: сравнение ниже выполняется только между
off/partial/full внутри своей серии. Median/min–max вычислены по трём
per-run averages; 1% low — по трём отдельным full-measurement percentiles,
не по объединённому набору frames. Ни один запуск не исключён.

`gpu_geometry_ms` уже включает `gpu_depth_prepass_ms`; складывать их второй
раз нельзя. Начало интервала общее, end markers упорядочены на одной queue.
Это completion intervals с расходами соответствующих границ, не изолированная
стоимость vertex/fragment instructions при исключённом overlap.

### Normal: измеренные значения

#### Normal: overdraw-1

| Метрика | off: median [min–max] | partial: median [min–max] | full: median [min–max] |
| --- | ---: | ---: | ---: |
| GPU frame, ms | 0.17029 [0.12853–0.17216] | 0.16374 [0.13230–0.17157] | 0.15543 [0.13178–0.17027] |
| GPU geometry, ms | 0.13468 [0.09744–0.13631] | 0.12842 [0.10066–0.13642] | 0.12080 [0.09997–0.13522] |
| GPU prepass, ms | 0.00000 [0.00000–0.00000] | 0.01160 [0.00917–0.01232] | 0.01090 [0.00911–0.01221] |
| GPU postprocess, ms | 0.03296 [0.02918–0.03317] | 0.03259 [0.02977–0.03292] | 0.03237 [0.02994–0.03251] |
| CPU frame, ms | 0.36700 [0.26727–0.37140] | 0.35276 [0.26397–0.36562] | 0.33305 [0.26671–0.36395] |
| FPS | 2724.8 [2692.5–3741.5] | 2834.8 [2735.1–3788.3] | 3002.5 [2747.6–3749.3] |
| 1% low FPS | 1649.9 [1047.3–1711.6] | 1231.6 [860.5–1698.7] | 1252.3 [761.7–1759.8] |

GPU frame median формально ниже на 3.85% / 8.72%, но все диапазоны
пересекаются. Off по хронологии: 0.12853 → 0.17216 → 0.17029 ms; внутри
одного режима дрейф больше предполагаемого выигрыша. **Ускорение малой сцены
по Normal-серии не подтверждено.** 1% low median ниже на 25.36% / 24.10%,
но и эти диапазоны пересекаются; причинную регрессию prepass эта серия
не устанавливает. Конкретная причина дрейфа не измерена.

#### Normal: overdraw-16

| Метрика | off: median [min–max] | partial: median [min–max] | full: median [min–max] |
| --- | ---: | ---: | ---: |
| GPU frame, ms | 1.23007 [1.21118–1.24222] | 0.22700 [0.22642–0.22818] | 0.22684 [0.22656–0.22866] |
| GPU geometry, ms | 1.18018 [1.17891–1.18319] | 0.19597 [0.19543–0.19658] | 0.19578 [0.19540–0.19642] |
| GPU prepass, ms | 0.00000 [0.00000–0.00000] | 0.06437 [0.06418–0.06452] | 0.06433 [0.06417–0.06440] |
| GPU postprocess, ms | 0.04506 [0.03046–0.06025] | 0.02924 [0.02920–0.02980] | 0.02936 [0.02926–0.03043] |
| CPU frame, ms | 1.42065 [1.39562–1.43081] | 0.37798 [0.37672–0.40759] | 0.38814 [0.38290–0.40816] |
| FPS | 703.9 [698.9–716.5] | 2645.6 [2453.5–2654.5] | 2576.4 [2450.0–2611.6] |
| 1% low FPS | 393.6 [380.5–590.2] | 1105.5 [948.5–1690.0] | 985.8 [737.8–1672.5] |

GPU frame ниже на **81.55% / 81.56%**, geometry — на **83.39% / 83.41%**.
Диапазоны off и обоих prepass modes хорошо разделены. FPS median выше
на 275.85% / 266.01%; это наблюдаемый throughput всего приложения, не
доказательство соответствующего ускорения CPU preparation. У off postprocess
меняется 0.06025 → 0.04506 → 0.03046 ms: отдельно ускорение postprocess
за счёт prepass не установлено.

### Stable: измеренные значения

#### Stable: overdraw-1

| Метрика | off: median [min–max] | partial: median [min–max] | full: median [min–max] |
| --- | ---: | ---: | ---: |
| GPU frame, ms | 0.14531 [0.14453–0.14543] | 0.15044 [0.15018–0.15073] | 0.15062 [0.15022–0.15067] |
| GPU geometry, ms | 0.11283 [0.11212–0.11290] | 0.11783 [0.11743–0.11805] | 0.11795 [0.11746–0.11805] |
| GPU prepass, ms | 0.00000 [0.00000–0.00000] | 0.01058 [0.01050–0.01066] | 0.01063 [0.01050–0.01065] |
| GPU postprocess, ms | 0.03027 [0.03022–0.03033] | 0.03048 [0.03042–0.03057] | 0.03048 [0.03043–0.03058] |
| CPU frame, ms | 0.30976 [0.25835–0.31283] | 0.30650 [0.27499–0.31545] | 0.31327 [0.28998–0.31725] |
| FPS | 3228.3 [3196.7–3870.7] | 3262.6 [3170.1–3636.4] | 3192.2 [3152.1–3448.5] |
| 1% low FPS | 1984.4 [1452.1–2031.4] | 1972.8 [1052.3–2036.3] | 1968.0 [709.3–2028.7] |

GPU frame выше на **3.53% / 3.65%** (+0.00513 / +0.00531 ms), geometry —
на **4.43% / 4.54%**. Диапазоны GPU frame и geometry не пересекаются с off:
при малом overdraw дополнительная geometry/depth работа не окупилась.
Это основание сохранить default off и выбирать prepass по измеренной сцене.

FPS median +1.06% / −1.12% и CPU frame −1.05% / +1.13% не интерпретируются
как причинный эффект prepass: диапазоны пересекаются, а CPU drift сохраняется
и при Stable GPU. Например, off CPU frame по повторам 0.25835 → 0.30976 →
0.31283 ms, тогда как GPU frame 0.14453 → 0.14543 → 0.14531 ms.
Без CPU profiler/ETW/PresentMon нельзя разделить scheduling, driver, DWM и
другие host effects. 1% low также имеет широкий разброс.

#### Stable: overdraw-16

| Метрика | off: median [min–max] | partial: median [min–max] | full: median [min–max] |
| --- | ---: | ---: | ---: |
| GPU frame, ms | 1.49494 [1.49484–1.53250] | 0.27264 [0.27234–0.27271] | 0.27292 [0.27273–0.28102] |
| GPU geometry, ms | 1.46099 [1.46071–1.46216] | 0.24003 [0.23974–0.24006] | 0.24019 [0.24010–0.24288] |
| GPU prepass, ms | 0.00000 [0.00000–0.00000] | 0.07907 [0.07893–0.07907] | 0.07908 [0.07890–0.07911] |
| GPU postprocess, ms | 0.03190 [0.03173–0.06813] | 0.03039 [0.03039–0.03049] | 0.03054 [0.03043–0.03594] |
| CPU frame, ms | 1.70826 [1.70238–1.72859] | 0.44637 [0.43422–0.44761] | 0.44541 [0.44482–0.45117] |
| FPS | 585.4 [578.5–587.4] | 2240.3 [2234.1–2303.0] | 2245.1 [2216.5–2248.1] |
| 1% low FPS | 461.9 [458.5–466.8] | 1567.0 [1503.7–1638.2] | 1534.0 [1274.4–1542.6] |

GPU frame ниже на **81.76% / 81.74%**, geometry — на **83.57% / 83.56%**;
off и prepass диапазоны не пересекаются. Prepass interval около 0.07907 ms
уже входит в приведённые 0.24003 / 0.24019 ms geometry. FPS median выше
на 282.70% / 283.52%, 1% low — на 239.26% / 232.11%; это результаты всего
приложения в данной серии. Они не доказывают ускорения отдельно взятого CPU
алгоритма. Первый off postprocess sample 0.06813 ms остаётся в диапазоне;
в остальных повторах около 0.0317–0.0319 ms. Самостоятельный postprocess
выигрыш из этого не выводится.

[Полная Normal-сводка](C:/GDF/vk_test/build/windows-release/depth-prepass-ab-20260912/DepthPrepass-summary.json)
и [полная Stable-сводка](C:/GDF/vk_test/build/windows-release/depth-prepass-stable-ab-20260912/DepthPrepass-summary.json)
содержат **15 метрик**: семь из таблиц плюс CPU fence/acquire/record/submit/
present/scene-prepare и GPU shadow/visibility. Для каждой сохранены все три
значения, median/min/max, изменения относительно off, доступность и пересечение
диапазонов; также есть последовательность запусков, counts/settings и SHA-256
18 исходных JSON. Диапазон min–max не является тестом статистической значимости.

### Выполненные E06 checks и пределы pixel comparison

| Проверка | Фактический результат и артефакт |
| --- | --- |
| Native Debug build + CTest | 6/6; [лог](C:/GDF/vk_test/build/windows-debug/depth-prepass-build.log) |
| Native Release build + CTest | 6/6; [лог](C:/GDF/vk_test/build/windows-release/depth-prepass-build.log) |
| Native ASan build + CTest | 6/6; [лог](C:/GDF/vk_test/build/windows-asan/depth-prepass-build.log) |
| GLSL compile + spirv-val | Завершены для Vulkan 1.3 / SPIR-V 1.5; [лог](C:/GDF/vk_test/build/windows-debug/depth-prepass-shaders.log) |
| Clang-tidy | Запущен для renderer.cpp, renderer_scene.cpp, render_fixture_scene.cpp; [лог](C:/GDF/vk_test/build/windows-debug/depth-prepass-tidy.log) содержит bounds, easily-swappable, implicit-widening и enum-size warnings, не является warning-free результатом; compiler errors не найдены |
| Debug canonical, CPU / dynamic | 1 startup × 1 cycle, 34 captures; [summary](C:/GDF/vk_test/build/windows-debug/render-validation/depth-prepass-canonical/cpu-dynamic/1-summary.json) |
| Debug canonical, GPU / legacy | 1 × 1, 34 captures; [summary](C:/GDF/vk_test/build/windows-debug/render-validation/depth-prepass-canonical/gpu-legacy/1-summary.json) |
| Debug overdraw-16, CPU / dynamic | 1 × 1, 34 captures; [summary](C:/GDF/vk_test/build/windows-debug/render-validation/depth-prepass-overdraw-16/cpu-dynamic/1-summary.json) |
| Debug stress, GPU / dynamic | 1 × 1, 34 captures; [summary](C:/GDF/vk_test/build/windows-debug/render-validation/depth-prepass-stress/gpu-dynamic/1-summary.json) |
| ASan canonical, CPU / dynamic | 2 startups × 2 cycles, 68 captures каждый, 136 всего; [startup 1](C:/GDF/vk_test/build/windows-asan/render-validation/depth-prepass-canonical/cpu-dynamic/1-summary.json), [startup 2](C:/GDF/vk_test/build/windows-asan/render-validation/depth-prepass-canonical/cpu-dynamic/2-summary.json) |
| ASan canonical, CPU / legacy | 1 × 1, 34 captures; [summary](C:/GDF/vk_test/build/windows-asan/render-validation/depth-prepass-canonical/cpu-legacy/1-summary.json) |

Это **136 Debug + 170 ASan captures** в перечисленных prepass runs с
front-to-back off. Все семь startup summaries имеют validation_errors 0,
live_gpu_allocations 0 и exit_code 0; соответствующие runtime logs прочитаны,
Validation Error/VUID/AddressSanitizer errors не найдены, shutdown сообщает
`Live GPU allocations: 0 (0 bytes)`. Скрипт включает synchronization validation
через `VK_VALIDATION_VALIDATE_SYNC=true`. Ранний Debug canonical CPU/legacy
каталог без completed summary не считается пройденным gate.

Проверены off → partial → full → off, invalid controls, одинаковая pose,
AA off/SMAA/FSR, resize, scene/instancing/gpu-saturation switches, delayed
timing availability и shutdown. FSR history/jitter и активный saturation с
меняющимся frame seed не объявляются exact-pixel совпадением; для активного
workload отдельно сохранён
[coverage record](C:/GDF/vk_test/build/windows-debug/render-validation/depth-prepass-canonical/cpu-dynamic/1-0-active-workload-coverage.json).
Для статичной procedural geometry и off→off restoration требуется exact
comparison; пример
[overdraw-16 full](C:/GDF/vk_test/build/windows-debug/render-validation/depth-prepass-overdraw-16/cpu-dynamic/1-0-off-scene-full-check.json)
подтверждает одинаковые decoded pixels.

В canonical imported scene при смене prepass принят явный численный допуск:
**не более 1 RGB8 LSB, не более 0.01% изменённых pixels, alpha совпадает точно**.
Фактически в legacy lighting full отличается от off в одном пикселе
из 230400, максимум 1 LSB на канал; это **0.000434% pixels**. Подтверждения:
[Debug GPU legacy](C:/GDF/vk_test/build/windows-debug/render-validation/depth-prepass-canonical/gpu-legacy/1-0-off-scene-full-check.json)
и [ASan CPU legacy](C:/GDF/vk_test/build/windows-asan/render-validation/depth-prepass-canonical/cpu-legacy/1-0-off-scene-full-check.json).
Причина этого единичного отличия не установлена; результат проходит инженерный
допуск, **не exact equality**. Допуск не распространяется на restoration и
procedural fixtures. Совпадающие coplanar surfaces с разными материалами,
будущий alpha-discard/deformation coverage и произвольный новый content этими
сценами не проверены.

### RGP: подтверждённое завершение и ограниченный анализ counters

[Обновлённая offline extraction](C:/GDF/vk_test/build/windows-release/rendering-pipeline-20260912/prepass-profiles/prepass-complete-rgp-counters.json)
сравнивает новый `prepass-off-complete.rgp` с прежним `prepass-full.rgp` и
сохранена отдельно от A/B. Оба RDF capture содержат 398 raw SPM и 22 derived
counters; завершённый off — **903 samples**, full — **213**. RDF container
проверен по [первичной спецификации AMD](https://github.com/GPUOpen-Drivers/libamdrdf/blob/main/docs/specification.md);
имена и описания derived counters сохранены из capture. Это capture-wide
sample aggregates, не timings/throughput отдельного render pass.

| Cache hit: arithmetic sample mean | off complete | full |
| --- | ---: | ---: |
| Instruction | 75.451% | 36.396% |
| Scalar | 73.324% | 31.840% |
| L0 | 78.973% | 30.483% |
| L1 | 24.283% | 2.909% |
| L2 | 34.064% | 24.936% |

Проверенные application settings совпадают, кроме requested/actual prepass:
overdraw-16, 2048 source/visible instances, 24577 logical triangles, CPU
visibility, AA/GI off, Reinhard, 1920×1080 IMMEDIATE. TraceConfig совпадает:
один capture frame, четыре prep frames, одинаковые SPM events и sample
frequency 4096. Новый off profiling process работал около 45 s, full — 15 s;
эти application measurement intervals не являются сопоставимым performance
экспериментом и не используются для расчёта выигрыша.

Эти числа **не доказывают cache regression**: доля нулевых samples различается
(например, instruction 167/903 против 131/213), а средние процентов не взвешены
по числу requests. Все minima равны 0. Без подтверждённого raw SPM layout и
ASIC event/stage mapping PS/VS invocations/waves не декодировались. Из этих
cache counters не выводятся ALU/bandwidth/early-Z bottleneck или причинная
экономия fragment invocations. Размер файла capture и профилируемые application
FPS/timings не используются как A/B измерения.

Оба текущих capture имеют явное `completed successfully` и `restored clocks`:
[новый off log](C:/GDF/vk_test/build/windows-release/rendering-pipeline-20260912/prepass-profiles/profile-off-complete-rdp.log)
и [прежний full log](C:/GDF/vk_test/build/windows-release/rendering-pipeline-20260912/prepass-profiles/profile-full-rdp.log).
Дополнительный [off Normal check](C:/GDF/vk_test/build/windows-release/rendering-pipeline-20260912/prepass-profiles/off-complete-normal.log)
подтверждает `Current clock mode: Normal` и успешный `Clock mode set to: Normal`.
Оговорка о незавершённом cleanup снята для этой новой пары. Исходный off capture
и [старый JSON с прежней оговоркой](C:/GDF/vk_test/build/windows-release/rendering-pipeline-20260912/prepass-profiles/prepass-rgp-counters.json)
сохранены без перезаписи; SHA-256 старого JSON проверен helper до и после
создания нового output. Успешный cleanup не превращает cache sample means
в доказательство причины измеренного prepass speedup.

## E06: coarse front-to-back, исправление CPU стоимости

`front-to-back` остаётся **default off**. Исправленный coarse ordering уменьшил
GPU frame статичной canonical scene с 0.34980 до 0.33649 ms (**−3.81%**),
но на stress 100000 instances GPU/FPS преимущества не установлено. Исходная
реализация имела тяжёлую CPU регрессию и была исправлена: её stress результат
около 55 FPS не принимается как пригодная реализация. В исправленной серии
остаётся отдельный сигнал ухудшения stress 1% low, описанный ниже.

### Условия и неизменность видимой геометрии

Каждая из двух серий содержит 12 завершённых JSON: canonical/stress-cpu ×
off/coarse × три повтора. Внутри серии один native Windows Release executable,
GPU/CPU/driver и 1920×1080 IMMEDIATE те же, что у prepass A/B; warmup 5 s,
measurement 10 s, CPU visibility, AA/GI/depth-prepass off, Reinhard, dynamic
14:00 без day-cycle, atlas 2048 D32, shadow-cache full. Off/coarse порядок
чередовался по повторам. Все Expected и requested/actual совпали. Серии с
исходной и исправленной реализацией не объединяются в одну статистику.

[Исходная environment](C:/GDF/vk_test/build/windows-release/front-to-back-stable-ab-20260912/FrontToBack-environment.json)
и [исправленная environment](C:/GDF/vk_test/build/windows-release/front-to-back-keys-stable-ab-20260912/FrontToBack-environment.json)
фиксируют Stable 1825 MHz GPU / 1000 MHz memory. Первичные CLI logs обеих
серий подтверждают принятие Stable и последующее восстановление Normal:
[исходная restoration](C:/GDF/vk_test/build/windows-release/rendering-pipeline-20260912/front-to-back-stable-ab-20260912-normal.log),
[исправленная restoration](C:/GDF/vk_test/build/windows-release/rendering-pipeline-20260912/front-to-back-keys-stable-ab-20260912-normal.log).
CPU clocks, temperature, scheduling и фоновые процессы не закреплены.

| Workload | Source / visible instances | Logical triangles | Frustum / occlusion rejected | LOD0 / LOD1 / LOD2 |
| --- | ---: | ---: | ---: | ---: |
| canonical | 324 / 252 | 234868 | 72 / 0 | 244 / 5 / 3 |
| stress-cpu | 100000 / 100000 | 1200001 | 0 / 0 | 100000 / 0 / 0 |

Эти counts идентичны во всех вариантах и повторах обеих серий. Canonical
camera статична, frustum и LOD включены. Stress использует camera-z 12,
scene-walls off, frustum/occlusion/LOD off и time-based camera animation.
Одинаковая траектория не означает одинаковые terminal frame poses или набор
поз в каждом измерительном окне при разной частоте кадров. Отключение culling/
LOD фиксирует membership и объём основной геометрии, но не screen coverage
и не частоту обновления shadows. Shadow totals в stress действительно
различаются; это не полностью одинаковая последовательность GPU работы.

### Исходная реализация: выявленная регрессия

В таблицах median [min–max] рассчитаны по трём per-run значениям. Полные
[исходные summaries/runs](C:/GDF/vk_test/build/windows-release/front-to-back-stable-ab-20260912/FrontToBack-metrics.json)
содержат FPS, CPU/GPU frame, 1% low и scene preparation; дополнительные GPU
pass timings взяты из перечисленных там исходных JSON. Проценты — изменение
coarse median относительно off median; ни один запуск не исключён.

#### Исходная сортировка: canonical

| Метрика | off: median [min–max] | coarse: median [min–max] | Δ |
| --- | ---: | ---: | ---: |
| CPU scene prepare, ms | 0.00080 [0.00073–0.00091] | 0.00103 [0.00098–0.00106] | +29.81% |
| CPU frame, ms | 0.51880 [0.51173–0.53805] | 0.52210 [0.51426–0.53391] | +0.64% |
| GPU frame, ms | 0.35000 [0.34950–0.35040] | 0.33746 [0.33697–0.33761] | −3.58% |
| GPU geometry, ms | 0.31799 [0.31753–0.31845] | 0.30534 [0.30505–0.30570] | −3.98% |
| GPU shadow, ms | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU postprocess, ms | 0.02977 [0.02974–0.02981] | 0.02969 [0.02969–0.02991] | −0.28% |
| FPS | 1927.5 [1858.6–1954.2] | 1915.3 [1873.0–1944.5] | −0.63% |
| 1% low FPS | 1327.0 [1303.9–1392.0] | 1250.5 [978.9–1267.8] | −5.76% |

#### Исходная сортировка: stress-cpu

| Метрика | off: median [min–max] | coarse: median [min–max] | Δ |
| --- | ---: | ---: | ---: |
| CPU scene prepare, ms | 0.00149 [0.00113–0.00150] | 17.63746 [17.61926–18.11918] | +17.63596 ms |
| CPU frame, ms | 0.63025 [0.62627–0.63843] | 18.00752 [17.98078–18.52028] | +2757.20% |
| GPU frame, ms | 0.44947 [0.44903–0.45021] | 0.98662 [0.98165–0.99127] | +119.51% |
| GPU geometry, ms | 0.36552 [0.36541–0.36561] | 0.36638 [0.36609–0.36660] | +0.24% |
| GPU shadow, ms | 0.05325 [0.05310–0.05400] | 0.58577 [0.58125–0.59172] | +1000.01% |
| GPU postprocess, ms | 0.02838 [0.02830–0.02848] | 0.03173 [0.03115–0.03192] | +11.81% |
| FPS | 1586.7 [1566.3–1596.8] | 55.5 [54.0–55.6] | −96.50% |
| 1% low FPS | 698.7 [679.0–711.5] | 45.4 [45.4–48.9] | −93.50% |

CPU scene preparation около 17.6–18.1 ms объясняет, почему первоначальный
вариант непригоден для этой нагрузки. GPU geometry почти не изменилась,
поэтому рост GPU frame нельзя целиком приписывать худшему opaque ordering:
shadow interval вырос примерно в 11 раз при иной частоте кадров/поз камеры.
Shadow pass totals с startup/warmup: off 1796–1803, coarse 662–682; четыре
tiles на pass. Это не изолированный одинаковый shadow workload и не доказанная
GPU регрессия самого comparator.

### Исправление ключей и безопасное повторное использование packet

Ключ сохраняет прежний смысл: ближайшая camera-space depth AABB, bucket 8 м,
затем source index для устойчивого порядка. При необходимости пересортировки
`coarse_depth_key` теперь вычисляется **один раз на visible instance**, а
comparator сравнивает сохранённую пару uint key/source index. Double arithmetic,
finite/overflow checks и проекция bounds не повторяются на каждом сравнении.
Ground/cube/model-LOD ranges, stable IDs, previous transforms и исходный
shadow packet не меняются; порядок обновляется только внутри готового visible
range. См. [ключ](C:/GDF/vk_test/src/coarse_depth_order.hpp:14) и
[sort_range](C:/GDF/vk_test/src/renderer_scene.cpp:167).

Дополнительный `std::vector<uint32_t>` заранее резервируется на 100001 элемент:
**400004 bytes CPU capacity** для ключей, без allocator bookkeeping. В пределах
этого лимита resize/reuse не добавляет per-frame allocation. Это постоянная
стоимость памяти renderer, а не измеренный GPU-memory overhead.

Повторное использование frame packet допускается только при неизменных
scene/transform revisions и отсутствии зависящего от камеры изменения
membership/LOD: frustum выключен, occlusion неактивен, LOD неактивен либо нет
соответствующей model/terrain geometry. Для coarse дополнительно требуется
**точное равенство всех четырёх коэффициентов строки clip.w** текущей и
сохранённой view-projection. Сравнение не использует epsilon или допуск bucket.
При фиксированных bounds одинаковая строка даёт те же depth keys; lateral
motion и AA jitter могут тогда сохранять порядок. Изменение строки заставляет
пересчитать keys/packet. См.
[условия reuse](C:/GDF/vk_test/src/renderer_scene.cpp:55).

В stress camera движется поперёк направления взгляда, а culling/LOD выключены,
поэтому этот exact-reuse path особенно важен. Исправленная серия проверяет
совместный эффект cached keys и reuse; она **не измеряет отдельно стоимость
полной пересортировки 100000 объектов каждый кадр** при вращении/движении
вглубь сцены. Такие изменения clip.w, а также постоянно меняющиеся transforms,
требуют отдельного scaling benchmark.

### Исправленная реализация: результаты

Полные [исправленные summaries/runs](C:/GDF/vk_test/build/windows-release/front-to-back-keys-stable-ab-20260912/FrontToBack-metrics.json)
сохраняют все повторы и geometry/settings проверки. Сравнение ниже — off/coarse
одной исправленной сборки; большая разница с первым экспериментом показывает
устранение обнаруженного CPU провала, но не заменяет эти matched comparisons.

#### Исправленная сортировка: canonical

| Метрика | off: median [min–max] | coarse: median [min–max] | Δ |
| --- | ---: | ---: | ---: |
| CPU scene prepare, ms | 0.00094 [0.00079–0.00096] | 0.00082 [0.00079–0.00088] | −12.40% |
| CPU frame, ms | 0.51683 [0.51609–0.51790] | 0.50196 [0.50079–0.51562] | −2.88% |
| GPU frame, ms | 0.34980 [0.34968–0.34984] | 0.33649 [0.33646–0.33662] | −3.81% |
| GPU geometry, ms | 0.31782 [0.31777–0.31788] | 0.30461 [0.30456–0.30473] | −4.16% |
| GPU shadow, ms | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU postprocess, ms | 0.02976 [0.02971–0.02977] | 0.02968 [0.02968–0.02969] | −0.27% |
| FPS | 1934.9 [1930.9–1937.6] | 1992.2 [1939.4–1996.9] | +2.96% |
| 1% low FPS | 1319.4 [1317.1–1374.8] | 1311.8 [873.1–1410.0] | −0.58% |

Canonical GPU frame/geometry диапазоны разделены: экономия порядка
**0.01332 ms GPU frame** воспроизводится на этой статичной позе. Scene-prepare
диапазоны пересекаются, поэтому его меньшая median не доказывает ускорение CPU
алгоритма. FPS +2.96% — наблюдаемый throughput этой серии при незакреплённом
CPU; 1% low имеет широкий разброс. Универсальный выигрыш по FPS не заявляется.

#### Исправленная сортировка: stress-cpu

| Метрика | off: median [min–max] | coarse: median [min–max] | Δ |
| --- | ---: | ---: | ---: |
| CPU scene prepare, ms | 0.00117 [0.00117–0.00147] | 0.00432 [0.00419–0.00457] | +268.99% |
| CPU frame, ms | 0.61914 [0.61831–0.63513] | 0.62389 [0.61947–0.64943] | +0.77% |
| GPU frame, ms | 0.44865 [0.44806–0.44968] | 0.44899 [0.44846–0.45142] | +0.08% |
| GPU geometry, ms | 0.36543 [0.36533–0.36554] | 0.36557 [0.36549–0.36581] | +0.04% |
| GPU shadow, ms | 0.05237 [0.05215–0.05370] | 0.05257 [0.05252–0.05484] | +0.39% |
| GPU postprocess, ms | 0.02836 [0.02833–0.02851] | 0.02855 [0.02823–0.02863] | +0.69% |
| FPS | 1615.1 [1574.5–1617.3] | 1602.8 [1539.8–1614.3] | −0.76% |
| 1% low FPS | 725.5 [679.3–727.1] | 653.1 [624.7–659.2] | −9.98% |

CPU preparation выросла примерно на **0.00315 ms**, а не на 17.6 ms как в
исходной реализации. Диапазоны CPU frame, GPU frame/geometry и FPS
пересекаются; преимущество coarse на этом stress workload не доказано.
Shadow pass totals теперь близки: off 1794–1798, coarse 1793–1797, но всё ещё
относятся к разным time-based frame sequences и включают startup/warmup.

**Нерешённый сигнал 1% low:** median ниже на 9.98%, диапазоны off/coarse не
пересекаются. Нельзя объявить отсутствие регрессии на основании близких
средних CPU/GPU/FPS. Причина по этим JSON не установлена: нет CPU/presentation
telemetry и детерминированного совпадения frame poses. Это основание для
отдельной проверки tail latency; coarse остаётся opt-in. Ни три повтора,
ни min–max сами по себе не дают статистического доказательства причины.

## E07 initial: CPU bounds hierarchy — измеренный вариант не рекомендован к включению

Первый вариант hierarchy, который отвергает невидимые ветви, **ухудшил CPU
стоимость и throughput на 25000 и 100000 объектов**. На 100000 median scene
prepare выросла на 124.19%, CPU frame на 82.83%, FPS снизился на 45.30%,
1% low на 37.51%. Такой вариант нельзя повышать до default или представлять
как подтверждённую оптимизацию. Linear остаётся контрольным путём.

В исходники уже добавлено принятие целой ветви, полностью находящейся внутри
frustum, без дальнейшей проверки каждого дочернего bounds. **Этот следующий
вариант на момент отчёта ещё не собран и не измерен. Итоговое решение по E07
остаётся открытым.** Приведённые ниже результаты и gates относятся к исходному
варианту; автоматически переносить их на изменённый traversal нельзя.

### Условия серии и проверка исходных данных

Источник — ровно 24 файла `<scene-count>-<linear|hierarchy>-<1|2|3>.json` в
`build/windows-release/cpu-hierarchy-stable-ab-20260912`. Служебный
`benchmark.json` исключён; ни один измерительный запуск не исключён.
[Исходные runs и summaries](C:/GDF/vk_test/build/windows-release/cpu-hierarchy-stable-ab-20260912/CpuHierarchy-metrics.json)
помечены `complete=true`. Новая
[проверенная сводка E07 initial](C:/GDF/vk_test/build/windows-release/cpu-hierarchy-stable-ab-20260912/CpuHierarchy-initial-summary.json)
сохраняет все raw JSON, SHA-256 входных файлов, пересчитанные median/min/max,
изменения медиан и ссылки с хешами на gates. Все 8 полей каждого из 8 summary
совпали с сохранённым benchmark summary без округления.

| Параметр | Подтверждённое значение |
| --- | --- |
| CPU / GPU | Ryzen 7 9700X / Radeon RX 6800 |
| Driver Windows / Vulkan ID | 32.0.21045.5002 / 8388961 |
| Build / present / resolution | Native Windows Release / IMMEDIATE / 1920×1080 render и output |
| Source instances | 16, 1024, 25000, 100000; walls off, camera-z 12 |
| Visibility | CPU forced; frustum on, occlusion off, LOD off |
| Варианты | `--cpu-culling linear|hierarchy`; requested и actual совпадают во всех JSON |
| Остальной pipeline | Dynamic lighting, GI off, AA off, depth prepass off, front-to-back off, Reinhard |
| Shadows / sun | D32 requested и actual, atlas 2048, full cache, hour 14, day-cycle off |
| Motion | Объекты статичны, камера движется по elapsed time |
| Повторы / время | 3 на вариант и размер; warmup 5 s, measurement около 10 s |
| Порядок | linear/hierarchy, hierarchy/linear, linear/hierarchy внутри каждого размера |
| Clocks | AMD RDP Stable принят: GPU 1825 MHz, memory 1000 MHz; Normal восстановлен |

Metadata сверена с `Expected` в
[environment snapshot](C:/GDF/vk_test/build/windows-release/cpu-hierarchy-stable-ab-20260912/CpuHierarchy-environment.json),
включая оба значения cpu_culling, shadow depth, prepass и sorting. Все source
counts соответствуют CLI, `visible + frustum_rejected = source`, occlusion
отключён, shadow tiles равны четырём на shadow pass. Последние counters не
использовались как требование равенства между отдельными запусками.

[Stable log](C:/GDF/vk_test/build/windows-release/rendering-pipeline-20260912/cpu-hierarchy-stable-ab-20260912-stable.log)
и [Normal restore log](C:/GDF/vk_test/build/windows-release/rendering-pipeline-20260912/cpu-hierarchy-stable-ab-20260912-normal.log)
подтверждают смену режима. Их сообщение о невозможности прочитать DirectX 9
settings blob не помешало успешному clock-mode API. Это не телеметрия частот
за каждый кадр: CPU clocks и temperature не закреплены/не семплировались,
power scheme Balanced, фоновые desktop processes не изолированы.

Оба варианта повторяют одну заданную траекторию камеры, но набор измеренных
поз зависит от времени и скорости кадров. **Это не paired-frame сравнение.**
Финальные visible/frustum counters в этой серии совпали, однако совпадение
последнего sample не доказывает одинаковое распределение geometry/shadow work
за весь прогон. Они приведены как фактические значения, а не усреднённая
видимость. У 16 объектов в последнем sample все 16 отвергнуты; это не доказательство
пустой сцены в течение всего измерения и не отдельный test визуальной корректности.

### Детальные результаты E07 initial

Ячейки — **median [min–max] трёх per-run значений**, время в ms. Для 1% low
использованы три отдельных значения с `percentile_scope=full_measurement`;
для visibility — три финальных sample. Это не min/max кадров и не confidence
interval. `Δ = 100 × (hierarchy / linear − 1)` по неокруглённым медианам.

#### 16 source instances

| Метрика | linear: median [min–max] | hierarchy: median [min–max] | Δ |
| --- | ---: | ---: | ---: |
| CPU scene prepare, ms | 0.00154 [0.00148–0.00155] | 0.00156 [0.00147–0.00157] | +1.12% |
| CPU frame, ms | 0.25278 [0.24604–0.26776] | 0.25424 [0.25105–0.26053] | +0.58% |
| FPS | 3955.9 [3734.7–4064.3] | 3933.3 [3838.3–3983.2] | -0.57% |
| 1% low FPS | 2140.3 [2136.3–2161.7] | 2165.7 [2074.9–2205.1] | +1.19% |
| GPU geometry, ms | 0.05663 [0.05635–0.05690] | 0.05663 [0.05638–0.05692] | -0.01% |
| GPU frame, ms | 0.08849 [0.08809–0.08878] | 0.08837 [0.08819–0.08873] | -0.14% |
| Final visible instances | 0 [0–0] | 0 [0–0] | — |
| Final frustum rejected | 16 [16–16] | 16 [16–16] | 0.00% |

#### 1024 source instances

| Метрика | linear: median [min–max] | hierarchy: median [min–max] | Δ |
| --- | ---: | ---: | ---: |
| CPU scene prepare, ms | 0.00727 [0.00717–0.00807] | 0.00676 [0.00655–0.00835] | -7.04% |
| CPU frame, ms | 0.28241 [0.27989–0.30590] | 0.28800 [0.27900–0.30975] | +1.98% |
| FPS | 3540.9 [3269.0–3572.9] | 3472.2 [3228.4–3584.2] | -1.94% |
| 1% low FPS | 2053.8 [2013.0–2121.8] | 2059.3 [1953.4–2095.5] | +0.27% |
| GPU geometry, ms | 0.08980 [0.08951–0.09042] | 0.08977 [0.08946–0.09057] | -0.03% |
| GPU frame, ms | 0.12078 [0.12055–0.12158] | 0.12081 [0.12049–0.12183] | +0.03% |
| Final visible instances | 133 [133–133] | 133 [133–133] | 0.00% |
| Final frustum rejected | 891 [891–891] | 891 [891–891] | 0.00% |

#### 25000 source instances

| Метрика | linear: median [min–max] | hierarchy: median [min–max] | Δ |
| --- | ---: | ---: | ---: |
| CPU scene prepare, ms | 0.17055 [0.16961–0.17130] | 0.24044 [0.23882–0.24398] | +40.98% |
| CPU frame, ms | 0.40333 [0.40257–0.42722] | 0.46110 [0.46025–0.46624] | +14.32% |
| FPS | 2479.4 [2340.7–2484.0] | 2168.7 [2144.8–2172.7] | -12.53% |
| 1% low FPS | 1205.7 [1172.0–1210.6] | 1126.9 [1080.9–1141.3] | -6.54% |
| GPU geometry, ms | 0.14912 [0.14904–0.14997] | 0.14910 [0.14907–0.14958] | -0.02% |
| GPU frame, ms | 0.20775 [0.20758–0.21032] | 0.21233 [0.21163–0.21237] | +2.20% |
| Final visible instances | 10015 [10015–10015] | 10015 [10015–10015] | 0.00% |
| Final frustum rejected | 14985 [14985–14985] | 14985 [14985–14985] | 0.00% |

#### 100000 source instances

| Метрика | linear: median [min–max] | hierarchy: median [min–max] | Δ |
| --- | ---: | ---: | ---: |
| CPU scene prepare, ms | 1.12336 [1.12220–1.12754] | 2.51846 [2.47631–2.57244] | +124.19% |
| CPU frame, ms | 1.88648 [1.88434–1.89373] | 3.44897 [3.40443–3.51353] | +82.83% |
| FPS | 530.1 [528.1–530.7] | 289.9 [284.6–293.7] | -45.30% |
| 1% low FPS | 278.5 [276.0–279.7] | 174.1 [147.2–177.7] | -37.51% |
| GPU geometry, ms | 0.35362 [0.35348–0.35368] | 0.35397 [0.35383–0.35406] | +0.10% |
| GPU frame, ms | 0.95871 [0.95318–0.96159] | 1.20475 [1.19630–1.20679] | +25.66% |
| Final visible instances | 81314 [81314–81314] | 81314 [81314–81314] | 0.00% |
| Final frustum rejected | 18686 [18686–18686] | 18686 [18686–18686] | 0.00% |

На 16 и 1024 диапазоны основных CPU/FPS метрик перекрываются; меньшая median
scene preparation на 1024 сама по себе не доказывает выигрыш. На 25000 и
100000 диапазоны CPU scene preparation, CPU frame, FPS и 1% low разделены
в сторону ухудшения hierarchy. Для 100000 это **+1.39510 ms scene preparation**
и **+1.56249 ms CPU frame**. Scene preparation включает culling и подготовку
packet; отдельного профиля traversal или CPU hardware counters здесь нет.

GPU geometry у 100000 изменяется лишь на +0.10%, а GPU frame на +25.66%.
Shadow averages также различаются: linear 0.14082–0.14452 ms, hierarchy
0.23409–0.24338 ms; shadow passes с startup/warmup 1581–1606 и 1460–1473.
Поэтому рост полного GPU interval нельзя объявлять изолированной стоимостью
CPU-culling алгоритма или худшей fragment efficiency. Причина без timeline/
hardware counters не установлена; парные per-frame workload и scheduling не
зафиксированы. Эти ограничения не отменяют измеренный провал CPU стоимости
исходной реализации на данной процедурной нагрузке.

### Проверки корректности исходного E07 snapshot

Проверены сохранённые логи, новых запусков при подготовке этой сводки не было.
Native Windows
[Debug](C:/GDF/vk_test/build/windows-debug/hierarchy-build.log),
[Release](C:/GDF/vk_test/build/windows-release/hierarchy-build.log) и
[ASan](C:/GDF/vk_test/build/windows-asan/hierarchy-build.log)
завершили сборку и **6/6 CTest каждый**.

| Native regression | Captures | Результат |
| --- | ---: | --- |
| [Debug canonical / CPU / coarse / hierarchy](C:/GDF/vk_test/build/windows-debug/render-validation/depth-prepass-canonical/cpu-dynamic-coarse-hierarchy/1-summary.json) | 34 | 1 cycle; hierarchy actual; validation errors 0, live GPU allocations 0, exit 0 |
| [ASan canonical / CPU / coarse / hierarchy](C:/GDF/vk_test/build/windows-asan/render-validation/depth-prepass-canonical/cpu-dynamic-coarse-hierarchy/1-summary.json) | 68 | 2 cycles; hierarchy actual; validation errors 0, live GPU allocations 0, exit 0 |
| [Debug stress 100k / CPU / coarse / hierarchy](C:/GDF/vk_test/build/windows-debug/render-validation/depth-prepass-stress/cpu-dynamic-coarse-hierarchy/1-summary.json) | 34 | 1 cycle; hierarchy actual; validation errors 0, live GPU allocations 0, exit 0 |
| [Debug canonical / GPU / hierarchy requested](C:/GDF/vk_test/build/windows-debug/render-validation/depth-prepass-canonical/gpu-dynamic-hierarchy/1-summary.json) | 34 | 1 cycle; CPU culling actual off; validation errors 0, live GPU allocations 0, exit 0 |

Каждый сценарий охватывает off/SMAA/FSR, prepass off/partial/full/restoration,
resize и mode-switch. Сохранённые startup logs подтверждают shutdown с
`Live GPU allocations: 0 (0 bytes)`; ASan error и sync/validation hazards не
найдены. Capture count не означает, что все FSR-кадры обязаны быть exact:
для temporal history действует ранее описанный контракт prepass validation.

Отдельный
[linear-image-comparison.json](C:/GDF/vk_test/build/windows-debug/render-validation/depth-prepass-canonical/cpu-dynamic-coarse-hierarchy/linear-image-comparison.json)
содержит **28/28 exact совпадений decoded pixel hashes** между linear и
hierarchy для canonical: AA off/SMAA, prepass и lifecycle captures. Все
`before_hash == after_hash` и `exact_pixels=true`; FSR в эти 28 пар не входит.
Часть mode-switch кадров проверяет fallback. Это отдельное доказательство
сохранения изображения на фиксированных позах, не доказательство равенства
всех кадров движущейся benchmark-камеры.

Узкие [clang-tidy](C:/GDF/vk_test/build/windows-debug/hierarchy-ClangTidy.log) и
[MSVC /analyze](C:/GDF/vk_test/build/windows-debug/hierarchy-MSVC.log) разобраны,
**но warnings не равны нулю**. В tidy показаны 7 новых diagnostics для
`bounds_hierarchy.cpp` (exception escape и 6 array-index checks); 4 LOD-index
diagnostics `renderer_scene.cpp` существовали ранее. Значительная часть MSVC
вывода относится к STL/SDK и общим Core Guidelines checks.

Новые bounds/exception сигналы проверены вручную: median split с leaf size 8
даёт глубину не выше 29 даже в uint32 domain, DFS pending stack укладывается
в 30 элементов при capacity 33; размеры/индексы ограничены входным capacity.
Контейнеры build используют заранее зарезервированное место, scalar copies и
изученный путь `nth_element` нативного MSVC 14.38. Это техническое обоснование
triage текущего снимка, а не общее обещание для другой STL или нового алгоритма.
Подтверждённых новых leak/lifetime/bounds/overflow blockers при этой проверке
не найдено; анализаторы не объявляются полностью чистыми.

Новый renderer-функционал проверяется процедурными нагрузками до появления
production content. Результат здесь локален для указанного RX 6800/9700X,
исходного hierarchy и завершённой серии; изменение whole-inside acceptance
требует собственных Windows correctness gates и нового сопоставимого A/B.

## E08: IOR, material models и проверка GPU ABI

Новые материалы сохраняют full float precision: `PbrMaterial` 32 → 48 bytes,
`RuntimeGpuInstance` 128 → 144 bytes. IOR default 1.5 сохраняет F0 `0.04`
побитово; `standard=0`/`unlit=1` передаются как flat uint. Подробный contract,
ограничения opaque glTF subset и controls — в `PBR_MATERIALS.md`.

Проверки ниже выполнены 2026-09-12 на RX 6800, native Windows; они относятся
к материалам и варианту BVH с whole-inside acceptance. Производительность
в этих validation runs не измерялась.

| Gate | Фактический результат | Артефакт |
| --- | --- | --- |
| Shader compile/spirv-val | Vulkan 1.3 / SPIR-V 1.5, успешный полный shader inventory | `build/windows-release/material-shaders.log` |
| Debug/Release/ASan build + CTest | Все три конфигурации, 6/6 CTest каждая | `build/windows-{debug,release,asan}/material-build.log` |
| GPU numerical reference, Debug + ASan | 20 материалов со stride 48 + 2 runtime records со stride 144; max F0 absolute error 3.774273360513e-08; default F0, uint IDs, color/AO/transform rows exact | `build/windows-{debug,asan}/material-gpu-validation.log` |
| Sphere chart, Debug | 34 captures, AA off/SMAA/FSR, prepass off/partial/full, resize и mode switches | `build/windows-debug/material-spheres-validation.log` |
| Sphere chart, ASan | Два startup/shutdown, 68 captures суммарно | `build/windows-asan/material-spheres-validation.log` |
| Canonical CPU, Debug + ASan | Coarse sorting + hierarchy; 34 и 68 captures | `build/windows-{debug,asan}/material-canonical-cpu-validation.log` |
| Canonical GPU, Debug | Requested hierarchy корректно сообщает actual off; 34 captures | `build/windows-debug/material-canonical-gpu-validation.log` |
| Stress CPU, Debug | 100k sources, coarse sorting + hierarchy, 34 captures | `build/windows-debug/material-stress-validation.log` |
| Default-material regression | 56/56 decoded PNG pairs exact до/после; исключены FSR history и active workload modulation | `build/windows-debug/render-validation/depth-prepass-canonical/material-default-comparison.json` |
| Editor MCP, Debug + ASan | 168 проверок, 20 rejected invalid create/update requests в каждом; defaults, partial update, exact float values, undo/redo/no-op; два временных узла удалены, canonical scene hash unchanged | `build/windows-{debug,asan}/render-validation/material-editor/summary.json` |

Все перечисленные runtime/GPU gates использовали validation layer и
`VK_VALIDATION_VALIDATE_SYNC=true`, завершились с exit code 0, без ошибок
Vulkan/ASan и с `Live GPU allocations: 0 (0 bytes)`. Sphere chart содержит
28 сфер: IOR 1/1.33/1.5/2, три roughness для dielectric/conductor и ряд unlit.
Изображение проверено визуально; default regression сравнивает декодированные
пиксели, не сжатые PNG bytes. Это не тест каждого будущего material model.

Для E07 дополнительно предпринята попытка системного CPU sampling:
`wpr.exe -start CPU -filemode` вернул `0xc5585011`,
`Failed to enable the policy to profile system performance`.
`wpr.exe -status` после ошибки подтвердил `WPR is not recording`.
CPU call stacks не получены; frame/prepare timings не заменяют этот gate.

Последующее исправление glTF Exclusions проверено повторной сборкой всех трёх
конфигураций и 6/6 CTest в каждой: `material-exclusions-build.log` в каждом
Windows build tree. IOR+unlit и IOR+specular-glossiness теперь отклоняются
с диагностикой до публикации asset; отдельные IOR/unlit материалы одного
документа разрешены. glTF и GLB проходят positive/negative/rollback tests.

### E08 MaterialABI: измеренная стоимость нового material pipeline

Завершённая серия из **18 запусков** выявила регрессию на stress 100k:
median GPU frame **+10.37%**, CPU frame **+8.64%**, FPS **−7.95%**, 1% low
**−7.92%**. Диапазоны before/after всех четырёх метрик не пересекаются.
Это явный сигнал выше инженерного порога 3%; его нельзя скрывать за
успешными correctness gates или близкими результатами маленькой сцены.
На статичных canonical нагрузках GPU frame вырос на 1.27–1.70%.
**Окончательное решение о принятии E08 этой сводкой не выносится.**

#### Исходные данные, хеши и условия

Прочитаны ровно 18 файлов `before|after-<workload>-<1|2|3>.json` в
`build/windows-release/material-abi-stable-ab-20260912`; служебный
`benchmark.json` исключён. Все три повтора каждой стороны сохранены без
исключений. [Исходный metrics JSON](C:/GDF/vk_test/build/windows-release/material-abi-stable-ab-20260912/MaterialABI-metrics.json)
имеет `complete=true`, `expected_runs=18`.
[Проверенная машинная сводка](C:/GDF/vk_test/build/windows-release/material-abi-stable-ab-20260912/MaterialABI-summary.json)
содержит raw результаты, SHA-256 всех run JSON/logs, environment/metrics/clock
logs, пересчитанные statistics и classification изменений. Все 108 наборов
median/min/max и availability counts совпали с исходной сводкой без округления.

SHA-256 сохранённого before executable и текущего after executable повторно
сверены с [environment snapshot](C:/GDF/vk_test/build/windows-release/material-abi-stable-ab-20260912/MaterialABI-environment.json):
оба совпали. Проверка executable hash не доказывает сама по себе matching
версии SPIR-V/assets; эта серия использует сохранённый baseline bundle и
неизменные assets, как требует MaterialABI suite.

| Параметр | Значение |
| --- | --- |
| Before | [vcr_hierarchy_profile.exe](C:/GDF/vk_test/build/windows-release/rendering-pipeline-20260912/hierarchy-runtime/vcr_hierarchy_profile.exe), прежний material ABI |
| After | [vulkan_cube.exe](C:/GDF/vk_test/build/windows-release/bin/vulkan_cube.exe), новый IOR/model contract |
| CPU / GPU | Ryzen 7 9700X / Radeon RX 6800 |
| Driver Windows / Vulkan ID | 32.0.21045.5002 / 8388961 |
| Build / present / resolution | Native Windows Release / IMMEDIATE / 1920×1080 render и output |
| Повторы / время | 3 before + 3 after на workload; warmup 5 s, measurement около 10 s |
| Порядок в каждой нагрузке | before/after, after/before, before/after |
| Canonical CPU / GPU | Статичная сцена и камера; соответствующий forced backend, frustum on, LOD on |
| Stress CPU | 100000 sources, walls off, camera-z 12, animate-camera, frustum on, LOD off |
| Общие настройки | Dynamic lighting, GI off, AA off, Reinhard, depth prepass off, front-to-back off |
| CPU culling | Requested linear на обеих сторонах; actual linear на CPU, off на GPU |
| Shadows / time | Dynamic shadows on, full refresh, D32 atlas 2048, linear filter, hour 14, day-cycle off |
| Clocks | AMD RDP Stable принят: GPU 1825 MHz / memory 1000 MHz; Normal восстановлен |

Каждый raw JSON проверен против всех полей `Expected`, включая requested/actual
настройки, `percentile_scope=full_measurement` и нулевой prepass timing.
Внутри before/after пары CLI arguments совпадают. Непустая geometry, суммы
visibility/LOD counters и четыре shadow tiles на pass подтверждены.
[Stable log](C:/GDF/vk_test/build/windows-release/rendering-pipeline-20260912/material-abi-stable-ab-20260912-stable.log)
и [Normal restore log](C:/GDF/vk_test/build/windows-release/rendering-pipeline-20260912/material-abi-stable-ab-20260912-normal.log)
содержат успешную смену clock mode; их DirectX 9 settings-blob diagnostic не
помешал этой операции. CPU clocks/temperature не закреплены и не семплировались,
power scheme Balanced, desktop processes не изолированы. Hardware bandwidth,
cache/occupancy counters и CPU call-stack profile в этой серии отсутствуют.

IOR 1.5 / standard — контракт unchanged defaults/assets. Сам benchmark JSON
не содержит значения IOR и не проверяет его как runtime metadata. Здесь не
измеряется более сложное освещение новых материалов: обе стороны рисуют
default-material workload. Release benchmark использует `--no-validation`;
его timings и предыдущие Debug/ASan correctness gates — разные проверки.

#### Полный breakdown: canonical CPU

Все времена ниже — ms; каждая ячейка **median [min–max] трёх per-run значений**.
Для 1% low это три рассчитанных внутри отдельных runs значения, не общий
percentile объединённых кадров. `Δ = 100 × (after / before − 1)` вычислена
до округления. **Жирным выделено ухудшение медианы более чем на 3%**;
это инженерный сигнал, а не статистическая значимость или доказанная причина.

| Метрика | Before: median [min–max] | After: median [min–max] | Δ |
| --- | ---: | ---: | ---: |
| FPS | 1873.9 [1789.4–1923.1] | 1893.1 [1872.2–1937.4] | +1.02% |
| 1% low FPS | 1270.8 [1122.6–1412.1] | 1292.1 [1154.0–1375.6] | +1.68% |
| CPU frame | 0.53364 [0.51998–0.55886] | 0.52823 [0.51616–0.53413] | -1.01% |
| CPU scene prepare | 0.00079 [0.00078–0.00090] | 0.00077 [0.00075–0.00091] | -3.41% |
| CPU fence wait | 0.00607 [0.00445–0.00625] | 0.00579 [0.00443–0.00589] | -4.70% |
| CPU acquire | 0.02101 [0.02010–0.02225] | 0.02232 [0.02169–0.02310] | **+6.24%** |
| CPU command record | 0.01826 [0.01788–0.02424] | 0.01744 [0.01742–0.02226] | -4.51% |
| CPU queue submit | 0.02508 [0.02407–0.02566] | 0.02582 [0.02438–0.02610] | +2.97% |
| CPU present | 0.43352 [0.41798–0.45569] | 0.42490 [0.41482–0.42727] | -1.99% |
| GPU frame | 0.35031 [0.35018–0.35156] | 0.35477 [0.35445–0.35548] | +1.27% |
| GPU geometry | 0.31830 [0.31820–0.31945] | 0.32292 [0.32252–0.32349] | +1.45% |
| GPU workload | null [3/3 unavailable] | null [3/3 unavailable] | — |
| GPU shadow | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU postprocess | 0.02981 [0.02978–0.02990] | 0.02972 [0.02965–0.02977] | -0.31% |
| GPU depth prepass | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU voxel geometry | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU voxel light | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU visibility | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |

#### Полный breakdown: canonical GPU

| Метрика | Before: median [min–max] | After: median [min–max] | Δ |
| --- | ---: | ---: | ---: |
| FPS | 1878.7 [1816.5–1941.8] | 1837.1 [1835.1–1925.9] | -2.21% |
| 1% low FPS | 1262.5 [1063.8–1438.3] | 1338.6 [1326.3–1474.7] | +6.03% |
| CPU frame | 0.53229 [0.51499–0.55050] | 0.54433 [0.51924–0.54494] | +2.26% |
| CPU scene prepare | 0.00093 [0.00081–0.00095] | 0.00085 [0.00083–0.00095] | -9.36% |
| CPU fence wait | 0.00435 [0.00398–0.00578] | 0.00602 [0.00427–0.00604] | **+38.41%** |
| CPU acquire | 0.02110 [0.02100–0.02228] | 0.02241 [0.02084–0.02275] | **+6.24%** |
| CPU command record | 0.02462 [0.01848–0.02687] | 0.02047 [0.01894–0.02579] | -16.87% |
| CPU queue submit | 0.02576 [0.02500–0.02670] | 0.02513 [0.02361–0.02524] | -2.44% |
| CPU present | 0.42200 [0.41133–0.44026] | 0.43237 [0.41620–0.44287] | +2.46% |
| GPU frame | 0.35225 [0.35137–0.35286] | 0.35824 [0.35696–0.35852] | +1.70% |
| GPU geometry | 0.32032 [0.31961–0.32093] | 0.32623 [0.32512–0.32654] | +1.84% |
| GPU workload | null [3/3 unavailable] | null [3/3 unavailable] | — |
| GPU shadow | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU postprocess | 0.02973 [0.02956–0.02974] | 0.02977 [0.02964–0.02981] | +0.16% |
| GPU depth prepass | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU voxel geometry | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU voxel light | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU visibility | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |

#### Полный breakdown: stress CPU 100k

| Метрика | Before: median [min–max] | After: median [min–max] | Δ |
| --- | ---: | ---: | ---: |
| FPS | 535.0 [531.7–538.1] | 492.5 [491.9–497.3] | **-7.95%** |
| 1% low FPS | 283.1 [280.7–290.8] | 260.7 [241.4–261.7] | **-7.92%** |
| CPU frame | 1.86910 [1.85834–1.88072] | 2.03057 [2.01078–2.03273] | **+8.64%** |
| CPU scene prepare | 1.11322 [1.11233–1.12008] | 1.18580 [1.17688–1.19108] | **+6.52%** |
| CPU fence wait | 0.00229 [0.00223–0.00232] | 0.00230 [0.00225–0.00237] | +0.47% |
| CPU acquire | 0.02199 [0.02124–0.02236] | 0.02267 [0.02139–0.02277] | **+3.08%** |
| CPU command record | 0.47697 [0.46987–0.48157] | 0.54883 [0.54523–0.54997] | **+15.06%** |
| CPU queue submit | 0.02072 [0.02055–0.02086] | 0.02173 [0.02104–0.02187] | **+4.91%** |
| CPU present | 0.20252 [0.20142–0.20263] | 0.21352 [0.21188–0.21768] | **+5.43%** |
| GPU frame | 0.95424 [0.95016–0.96158] | 1.05323 [1.04844–1.05402] | **+10.37%** |
| GPU geometry | 0.35311 [0.35280–0.35341] | 0.37501 [0.37479–0.37522] | **+6.20%** |
| GPU workload | null [3/3 unavailable] | null [3/3 unavailable] | — |
| GPU shadow | 0.14053 [0.13972–0.14423] | 0.15038 [0.14995–0.15194] | **+7.01%** |
| GPU postprocess | 0.02902 [0.02887–0.02930] | 0.02909 [0.02876–0.02920] | +0.25% |
| GPU depth prepass | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU voxel geometry | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU voxel light | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU visibility | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |

`null` workload timing сохранён: gpu-saturation compute в этих scene cases
не выполняется. Ноль prepass/GI относится к отключённым passes. В статичных
canonical случаях shadow totals равны одному pass/четырём tiles на весь run,
а измеренный после прогрева shadow timing нулевой. У canonical GPU также
нулевой measured visibility timing: эта статичная серия не измеряет цену
повторного GPU culling изменяющейся сцены. GPU geometry является enclosing
interval для prepass; CPU/GPU timings и независимые медианы breakdown нельзя
складывать для восстановления времени полного кадра.

#### Геометрия и сопоставимость camera/shadow work

| Нагрузка, обе стороны / все повторы | Source | Final visible | Final frustum rejected | Triangles | LOD 0 / 1 / 2 |
| --- | ---: | ---: | ---: | ---: | --- |
| canonical CPU | 324 | 252 | 72 | 234868 | 244 / 5 / 3 |
| canonical GPU | 324 | 252 | 72 | 234868 | 244 / 5 / 3 |
| stress CPU | 100000 | 81314 | 18686 | 975769 | 81314 / 0 / 0 |

Occlusion rejected везде 0. Статичные canonical pairs прошли строгий equality
gate всех geometry counters. Stress counters тоже фактически совпали, но
совпадение конечного sample **не доказывает совпадение каждого frame pose**.
Камера движется по elapsed time; за measurement before отрисовано 5317–5382
кадра, after 4920–4973. Это разные последовательности sampled poses.

Stress shadow totals включают startup/warmup: before 1587–1613 passes
(6348–6452 tiles), after 1562–1576 passes (6248–6304 tiles). Полный refresh
сохраняет четыре tiles на pass. Разные totals не дают выделить чистую цену
одинакового shadow submission; рост average shadow timing +7.01% нельзя
автоматически приписать только увеличенному material stride.

#### Классификация сигналов и пределы вывода

- **Stress: подтверждённое локальное ухудшение выше 3%** по полному CPU/GPU
  frame, FPS и 1% low. CPU preparation +6.52%, command record +15.06%, GPU
  geometry +6.20% и shadow +7.01% также имеют разделённые диапазоны трёх
  per-run средних. CPU queue submit +4.91% и present +5.43% превышают порог с
  разделёнными диапазонами; acquire +3.08% — сигнал с перекрытием диапазонов.
- **Canonical: основные CPU frame/FPS и GPU frame/geometry изменения в пределах
  3%**, но GPU frame и geometry диапазоны before/after разделены. Это небольшой
  локальный рост GPU стоимости, а не доказательство нулевой цены ABI. CPU
  acquire +6.24% на обоих backends и fence wait +38.41% на GPU — отдельные
  сигналы; диапазоны перекрываются, абсолютные добавки примерно 0.00131 и
  0.00167 ms. По ним причина не установлена.
- **Благоприятные изменения более 3% на canonical не объявляются доказанным
  ускорением.** CPU scene preparation, command record, CPU fence wait и GPU-case
  1% low имеют перекрывающиеся диапазоны. Например, preparation −3.41% на CPU
  соответствует лишь −0.000027 ms. Машинная сводка сохраняет обе стороны
  порога и признак пересечения диапазонов, без отбора только удобных сигналов.

У stress разница медиан составляет **+0.16147 ms CPU frame**, в том числе
наблюдаются +0.07258 ms preparation и +0.07185 ms command record; полный GPU
frame вырос на **0.09899 ms**, geometry на 0.02190 ms. Эти значения описывают
всю новую сборку при фиксированных настройках, а не независимые аддитивные
причины её стоимости.

Full-precision контракт расширил `PbrMaterial` 32→48 bytes и runtime instance
128→144 bytes: **+12.5% на запись**, или +1600000 bytes на массив из 100000
записей. Суммарная цена CPU/frame/device копий зависит от реального числа
массивов. ABI сохраняет точные AO/IOR и отдельный uint model, давая новые
возможности материала; на default workload визуальная выгода не ожидается,
а совместимость изображения проверена отдельно. Измеренные затраты — данные
для оценки этого компромисса, не автоматическое основание принять регрессию.

Дополнительный объём CPU packing/upload и GPU чтений **может** способствовать
росту стоимости. Однако bandwidth/cache pressure — лишь гипотеза: нет hardware
counters, bytes-transferred telemetry или эксперимента, отделяющего дополнительные
16 bytes от новых shader inputs/F0/model branch и прочих изменений сборки.
Нельзя писать, что обнаружен именно bandwidth bottleneck. Hierarchy traversal
здесь не включён: CPU принудительно linear, GPU сообщает CPU culling off.
Межсерийное сравнение GPU результатов CpuHierarchy с разным ABI также не
изолирует BVH. Финальная оценка E08 должна учитывать выявленные регрессии и
отдельно сформулированный выигрыш нового material contract.

## E07 follow-up: whole-inside hierarchy — выигрыш на 100000 объектов

Принятие целиком видимой ветви меняет результат E07: в новой серии на 100000
объектов median CPU scene preparation снизилась на **40.29%**, CPU frame
на **24.40%**, FPS вырос на **32.27%**. Диапазоны трёх повторов этих метрик
разделены. На 25000 экономия scene preparation также повторяется, но общий
FPS/CPU-frame выигрыш неоднозначен из-за разброса. На 16 объектах есть
измеренный overhead; эти данные не обосновывают включение hierarchy для
любой сцены.

### Snapshot и проверка серии

Источник — 24 runs в
[CpuHierarchy-inside-metrics](C:/GDF/vk_test/build/windows-release/cpu-hierarchy-inside-stable-ab-20260912/CpuHierarchy-metrics.json).
[Пересчитанная сводка](C:/GDF/vk_test/build/windows-release/cpu-hierarchy-inside-stable-ab-20260912/CpuHierarchy-inside-summary.json)
сохраняет raw JSON, SHA-256 каждого JSON/log, environment/clock/build evidence,
порядок запусков, median/min/max и изменения медиан. Ни один из 24 запусков
не исключён, служебный `benchmark.json` повторно не учитывается.

Сборка обеих сторон одна: whole-inside traversal, **runtime instance ABI
144 bytes и прежние CPU packets с копиями source и visible instances**.
Сохранённый
[exe snapshot](C:/GDF/vk_test/build/windows-release/rendering-pipeline-20260912/material-runtime/vcr_material_profile.exe)
имеет проверенный SHA-256
`73763DB94CCB2400EE3D9D163D702A00D18795E199E7496C9A334D99B786EA3A`;
он совпал с `ExecutableEvidence` environment. Новый source+indices CPU
packet path ещё не входит в эту измеренную сборку.

| Параметр | Условия |
| --- | --- |
| Hardware / driver | Ryzen 7 9700X, Radeon RX 6800; Windows driver 32.0.21045.5002, Vulkan driver ID 8388961 |
| Build / output | Native Windows Release, IMMEDIATE, 1920×1080 render/output |
| Workloads | 16 / 1024 / 25000 / 100000 объектов; scene walls off, camera-z 12, animate-camera |
| Visibility | CPU forced, frustum on, occlusion off, LOD off; requested/actual linear или hierarchy |
| Pipeline | Dynamic lighting, GI/AA/prepass/front-to-back off, Reinhard; hour 14, day-cycle off |
| Shadows | Dynamic shadows, D32 requested/actual, atlas 2048, full refresh; cascade cache off |
| Timing / order | 3 повтора, warmup 5 s, measurement около 10 s; linear/hierarchy, hierarchy/linear, linear/hierarchy |
| Clocks | AMD Stable принят: GPU 1825 MHz, memory 1000 MHz; Normal восстановлен |

Проверены все `Expected` из
[environment](C:/GDF/vk_test/build/windows-release/cpu-hierarchy-inside-stable-ab-20260912/CpuHierarchy-environment.json),
общие Release/present/resolution gates и 24 пары raw/log JSON. Все recorded
samples совпали с raw; все **8 полей × 8 summary groups** пересчитаны точно,
без сравнения округлённых строк. Порядок в metrics совпал с ожидаемым
чередованием и хронологией файлов. Source counts соответствуют workload,
`visible + frustum_rejected = source`, occlusion и LOD1/2 равны нулю,
LOD0 равен visible; prepass timing нулевой. Shadow tiles равны четырём
на pass; percentile scope — full measurement, sample count равен measured
frames. Финальные geometry counters между запусками не использовались
как equality gate.

[Stable log](C:/GDF/vk_test/build/windows-release/rendering-pipeline-20260912/cpu-hierarchy-inside-stable-ab-20260912-stable.log)
и [Normal log](C:/GDF/vk_test/build/windows-release/rendering-pipeline-20260912/cpu-hierarchy-inside-stable-ab-20260912-normal.log)
подтверждают clock-mode API. Сообщение об ошибке чтения DirectX 9 settings blob
сохраняется, но API принял оба переключения. CPU clocks/temperature
не закреплены и не семплировались; Balanced power plan и фоновые desktop
processes не изолированы. Это локальный результат данного hardware/workload.

### Median и разброс whole-inside

Ячейки — **median [min–max] трёх per-run значений**, время в ms. Для 1% low
сведены три full-measurement percentile; для visibility — три финальных
sample. Диапазоны не являются confidence interval. `Δ = 100 ×
(hierarchy / linear − 1)` по неокруглённым медианам. Жирным выделены
изменения медиан больше 3% в любую сторону; это порог внимания,
а не самостоятельное доказательство устойчивого выигрыша.

#### 16 source instances

| Метрика | linear: median [min–max] | hierarchy: median [min–max] | Δ |
| --- | ---: | ---: | ---: |
| CPU scene prepare, ms | 0.00141 [0.00139–0.00144] | 0.00152 [0.00145–0.00155] | **+7.97%** |
| CPU frame, ms | 0.24688 [0.24540–0.24910] | 0.25441 [0.24594–0.26532] | **+3.05%** |
| FPS | 4050.5 [4014.4–4075.0] | 3930.6 [3769.0–4066.1] | -2.96% |
| 1% low FPS | 2223.6 [2197.4–2241.2] | 2141.2 [2108.9–2170.2] | **-3.71%** |
| GPU geometry, ms | 0.05643 [0.05637–0.05654] | 0.05662 [0.05641–0.05685] | +0.33% |
| GPU frame, ms | 0.08823 [0.08816–0.08829] | 0.08847 [0.08810–0.08872] | +0.27% |
| Final visible instances | 0 [0–0] | 0 [0–0] | — |
| Final frustum rejected | 16 [16–16] | 16 [16–16] | 0.00% |

#### 1024 source instances

| Метрика | linear: median [min–max] | hierarchy: median [min–max] | Δ |
| --- | ---: | ---: | ---: |
| CPU scene prepare, ms | 0.00763 [0.00755–0.00815] | 0.00636 [0.00610–0.00651] | **-16.71%** |
| CPU frame, ms | 0.28724 [0.27896–0.30808] | 0.28826 [0.28108–0.30041] | +0.35% |
| FPS | 3481.4 [3245.9–3584.8] | 3469.1 [3328.8–3557.8] | -0.35% |
| 1% low FPS | 2059.4 [2054.4–2095.9] | 2056.4 [2045.2–2057.2] | -0.15% |
| GPU geometry, ms | 0.09023 [0.08996–0.09089] | 0.09031 [0.08998–0.09081] | +0.09% |
| GPU frame, ms | 0.12132 [0.12097–0.12210] | 0.12143 [0.12100–0.12196] | +0.09% |
| Final visible instances | 133 [133–133] | 133 [133–133] | 0.00% |
| Final frustum rejected | 891 [891–891] | 891 [891–891] | 0.00% |

#### 25000 source instances

| Метрика | linear: median [min–max] | hierarchy: median [min–max] | Δ |
| --- | ---: | ---: | ---: |
| CPU scene prepare, ms | 0.16891 [0.16621–0.17141] | 0.11457 [0.10977–0.11466] | **-32.17%** |
| CPU frame, ms | 0.41887 [0.40256–0.42374] | 0.39983 [0.39147–0.42281] | **-4.55%** |
| FPS | 2387.4 [2359.9–2484.1] | 2501.1 [2365.1–2554.5] | **+4.76%** |
| 1% low FPS | 1128.9 [1095.3–1168.7] | 1193.7 [1131.9–1222.9] | **+5.74%** |
| GPU geometry, ms | 0.15223 [0.15200–0.15265] | 0.15242 [0.15205–0.15299] | +0.12% |
| GPU frame, ms | 0.21455 [0.21309–0.21536] | 0.21345 [0.21229–0.21573] | -0.51% |
| Final visible instances | 10015 [10015–10015] | 10015 [10015–10015] | 0.00% |
| Final frustum rejected | 14985 [14985–14985] | 14985 [14985–14985] | 0.00% |

#### 100000 source instances

| Метрика | linear: median [min–max] | hierarchy: median [min–max] | Δ |
| --- | ---: | ---: | ---: |
| CPU scene prepare, ms | 1.17584 [1.17373–1.18234] | 0.70208 [0.70141–0.71563] | **-40.29%** |
| CPU frame, ms | 2.01169 [2.00148–2.02044] | 1.52088 [1.51025–1.54186] | **-24.40%** |
| FPS | 497.1 [494.9–499.6] | 657.5 [648.6–662.1] | **+32.27%** |
| 1% low FPS | 256.7 [252.9–264.2] | 279.5 [264.1–284.6] | **+8.89%** |
| GPU geometry, ms | 0.37557 [0.37511–0.37569] | 0.37496 [0.37496–0.37532] | -0.16% |
| GPU frame, ms | 1.05037 [1.04720–1.05228] | 0.92127 [0.91546–0.92561] | **-12.29%** |
| Final visible instances | 81314 [81314–81314] | 81314 [81314–81314] | 0.00% |
| Final frustum rejected | 18686 [18686–18686] | 18686 [18686–18686] | 0.00% |

### Интерпретация и ограничения

- **16 объектов:** prepare **+7.97%**, но абсолютная прибавка лишь
  **0.000112 ms**; диапазоны разделены. 1% low **−3.71%**, также с
  разделёнными диапазонами. CPU frame +3.05% имеет перекрытие диапазонов;
  FPS −2.96%. Выгода от hierarchy на этой нагрузке не подтверждена.
- **1024:** prepare **−16.71%** (−0.001275 ms), диапазоны разделены.
  CPU frame +0.35%, FPS −0.35%, 1% low −0.15% с перекрытием;
  улучшение отдельного этапа не превратилось в наблюдаемый throughput gain.
- **25000:** prepare **−32.17%** (−0.054338 ms), диапазоны разделены.
  CPU frame −4.55%, FPS +4.76%, 1% low +5.74%, но все три диапазона
  перекрываются. В третьем повторе FPS — 2359.9 linear и 2365.1 hierarchy,
  всего **+0.22%**; повтор сохранён полностью. Устойчивую величину общего
  выигрыша по этой серии назначать нельзя.
- **100000:** prepare **−40.29%** (−0.473762 ms), CPU frame **−24.40%**
  (−0.490810 ms), FPS **+32.27%** с разделёнными диапазонами — ясный
  локальный сигнал. 1% low +8.89%, однако диапазоны 252.9–264.2 и
  264.1–284.6 едва перекрываются; улучшение tail менее определённо.

Scene preparation включает culling и packet preparation, отдельного CPU
профиля traversal/allocation/cache counters здесь нет. На 25000 median
present выросла с 0.14853 до 0.18400 ms (+23.88%), а на 100000 —
с 0.21385 до 0.28794 ms (+34.64%). На 100000 command record, напротив,
уменьшилась с 0.54509 до 0.45090 ms (−17.28%). Эти изменения сохранены в
additional metrics сводки; они не доказывают одну причину изменения CPU frame.

На 100000 GPU geometry практически прежняя (−0.16%, диапазоны перекрываются),
а GPU frame **−12.29%** с разделёнными диапазонами. GPU shadow median
снизилась с 0.14985 до 0.11565 ms (−22.82%); shadow passes с startup/warmup
составили 1564–1576 и 1591–1595, measured frames — 4950–4997 и 6485–6621.
Сочетание разной частоты кадров, распределения shadow work и scheduling
не позволяет объявить весь GPU-frame выигрыш изолированной стоимостью
traversal. Hardware timeline/counters не снимались; причина не установлена.

Камера задана elapsed time, поэтому одинаковые флаги не обеспечивают одинаковые
sampled poses и per-frame workload. Финальные visible/frustum counts
случайно совпали: 0/16, 133/891, 10015/14985, 81314/18686.
Это не paired-frame доказательство; у 16 объектов нулевая финальная visibility
не означает отсутствие geometry на всём измерительном интервале.

**Первая reject-only серия использовала ABI128, эта — ABI144.**
Межсерийные CPU/GPU изменения нельзя приписывать только whole-inside traversal:
material layout/shaders и стоимость packets также изменились. Исторический
initial report остаётся корректным для прежнего snapshot; приведённый здесь
выигрыш вычислен исключительно как linear/hierarchy внутри новой серии.
Результат ещё не измеряет последующий CPU source+indices path и не заменяет
его отдельный CpuPackets A/B.

### Сохранённые gates этого этапа

[Debug](C:/GDF/vk_test/build/windows-debug/material-build.log),
[Release](C:/GDF/vk_test/build/windows-release/material-build.log) и
[ASan](C:/GDF/vk_test/build/windows-asan/material-build.log) material-build logs
с whole-inside snapshot подтверждают **6/6 CTest каждый**. Их SHA-256 записаны
в сводке. При составлении отчёта сборки и GPU validation не запускались;
прежние captures reject-only варианта не объявляются новыми image gates.

По отдельному разбору
[material clang-tidy](C:/GDF/vk_test/build/windows-debug/material-ClangTidy.log)
показал 124 warnings (123 project + 1 STL);
[material MSVC /analyze](C:/GDF/vk_test/build/windows-debug/material-MSVC.log) —
26872 occurrences (737 project, 632 unique project; 26135 dependencies),
errors 0. Review не нашёл новых подтверждённых high-confidence blockers:
BVH stack capacity 33 при max pending 30, bounded GLB header offsets 0–16
при 20 bytes и fixture count 28 при capacity 29 разобраны как false positives.
Это **не warning-free результат**: численная дельта относительно полного
warning baseline не измерена, а static analysis не доказывает Vulkan
synchronization correctness.

## CpuPackets: изолированный A/B source/index при неизменном ABI144

На процедурной CPU-нагрузке 100000 объектов с движущейся камерой новый packet
path поднял median FPS с **485.2 до 1018.6 (+109.91%)**, 1% low с **236.8
до 671.7 (+183.67%)**. CPU frame снизился с **2.06088 до 0.98178 ms
(−52.36%)**, GPU frame — с **1.05491 до 0.49245 ms (−53.32%)**.
Диапазоны всех этих метрик разделены. Статичная canonical CPU сцена и
canonical GPU контроль не показывают существенного изменения throughput.

Здесь сравнивается именно **прежний CPU packet с копиями полных 144-byte
source/visible records и новый source buffer + uint visible indices**.
В обеих сторонах одинаковы material/runtime ABI48/144, default IOR 1.5,
standard shading, shaders и assets. Это отдельный эксперимент после
MaterialABI; стоимость расширения 128→144 bytes повторно не меняется.
CPU hierarchy принудительно linear, sorting/prepass выключены. Измеряется
packet path целиком — подготовка, upload/reuse и draw indirection, —
а не одна из этих операций независимо.

### Данные, snapshots и условия

Источник — завершённая 13 сентября по локальному времени серия
[CpuPackets metrics](C:/GDF/vk_test/build/windows-release/cpu-packets-stable-ab-20260913/CpuPackets-metrics.json),
ровно 18 before/after runs: три workload × две стороны × три повтора.
Ни один измерительный запуск не исключён; служебный `benchmark.json`
не учитывается повторно.
[Сводка с проверенными хэшами](C:/GDF/vk_test/build/windows-release/cpu-packets-stable-ab-20260913/CpuPackets-summary.json)
содержит raw data, median/min/max всех 18 метрик, counters, изменения медиан
и признак перекрытия диапазонов.
[Локальный helper](C:/GDF/vk_test/build/windows-release/cpu-packets-stable-ab-20260913/summarize_cpu_packets.ps1)
повторяет анализ без запуска renderer.

| Сторона | Сохранённая сборка / SHA-256 |
| --- | --- |
| Before | [material-runtime/vcr_material_profile.exe](C:/GDF/vk_test/build/windows-release/rendering-pipeline-20260912/material-runtime/vcr_material_profile.exe) — `73763DB94CCB2400EE3D9D163D702A00D18795E199E7496C9A334D99B786EA3A` |
| After | [cpu-packets-runtime/vcr_cpu_packets_profile.exe](C:/GDF/vk_test/build/windows-release/rendering-pipeline-20260912/cpu-packets-runtime/vcr_cpu_packets_profile.exe) — `7E777A44A72B0FE05A3615299A601B3A76281D46264273CE6C7DCE4C574DE0D7` |

Оба exe hash совпали с
[environment](C:/GDF/vk_test/build/windows-release/cpu-packets-stable-ab-20260913/CpuPackets-environment.json).
Сравнение сохранённых runtime bundles подтвердило одинаковые sets и byte
hashes **41 SPIR-V, 294 asset-файлов и FidelityFX DLL**. Canonical node и
material JSON не содержат nondefault IOR/model; отсутствующие поля используют
авторские defaults. Проверены именно сохранённые pre-E09 файлы: рабочие
SPIR-V изменились после окончания всех 18 запусков и восстановления Normal,
когда началась следующая доработка. E09 в этой серии не измеряется.

| Параметр | Подтверждённые условия |
| --- | --- |
| CPU / GPU / driver | Ryzen 7 9700X / Radeon RX 6800; Windows 32.0.21045.5002, Vulkan driver ID 8388961 |
| Build / output / present | Native Windows Release, render/output 1920×1080, IMMEDIATE |
| canonical-cpu | CPU visibility, frustum on, LOD on, статичная canonical сцена и камера |
| canonical-gpu | Та же сцена, GPU visibility, frustum/LOD on; CPU culling requested linear, actual off |
| stress-cpu | CPU visibility, 100000 объектов, walls off, camera-z 12, animate-camera, frustum on, LOD off |
| Common controls | CPU culling linear на CPU, front-to-back off, depth prepass off, dynamic lighting, GI off, AA off, Reinhard |
| Sun / shadows | Hour 14, day-cycle off, dynamic shadows on, D32 requested/actual, atlas 2048, full refresh / cascade cache false |
| Повторы и порядок | Warmup 5 s, measurement около 10 s; before/after, after/before, before/after внутри каждой workload |
| Clocks | AMD Stable принят: GPU 1825 MHz, memory 1000 MHz; Normal восстановлен |

CLI arguments внутри каждой before/after пары идентичны. Все `Expected`,
requested/actual controls, Release/present/resolution и full-measurement
percentile scope прошли проверку. Все 18 raw JSON совпали с JSON из своих
console logs; recorded samples и **6 groups × 18 summary fields**
пересчитаны точно, включая availability. Сохранённый порядок совпал
с ожидаемым чередованием и хронологией файлов.

[Stable log](C:/GDF/vk_test/build/windows-release/rendering-pipeline-20260912/cpu-packets-stable-ab-20260913-stable.log)
и [Normal restore log](C:/GDF/vk_test/build/windows-release/rendering-pipeline-20260912/cpu-packets-stable-ab-20260913-normal.log)
подтверждают успешные переключения. Ошибка чтения DirectX 9 settings blob
не помешала clock-mode API. Это не per-frame clock telemetry: CPU clocks
и температуры не закреплены/не семплировались, Balanced power plan и фоновые
desktop processes не изолированы.

### Полный CPU/GPU breakdown

Ячейки — **median [min–max] трёх per-run значений**, время в ms. Для 1% low
используются три full-measurement percentile; диапазоны не являются min/max
отдельных кадров или confidence interval. `Δ = 100 × (after / before − 1)`
считалась до округления. Жирным отмечено абсолютное изменение медианы >3%;
само по себе оно не доказывает устойчивый сигнал. `N/A` сохраняет исходный
`null`, нули означают доступные нулевые timings. GPU geometry включает
prepass, который здесь выключен; GPU интервалы не складываются с CPU как
независимые последовательные затраты.

#### canonical-cpu

| Метрика | before: median [min–max] | after: median [min–max] | Δ |
| --- | ---: | ---: | ---: |
| FPS | 1922.5 [1908.1–1932.2] | 1923.2 [1921.7–1927.2] | +0.04% |
| 1% low FPS | 1365.8 [1359.2–1435.4] | 1327.4 [1326.7–1389.9] | -2.82% |
| CPU frame, ms | 0.52016 [0.51754–0.52407] | 0.51998 [0.51890–0.52037] | -0.04% |
| CPU scene prepare, ms | 0.00094 [0.00081–0.00094] | 0.00089 [0.00075–0.00094] | **-5.75%** |
| CPU fence wait, ms | 0.00502 [0.00426–0.00532] | 0.00444 [0.00439–0.00523] | **-11.59%** |
| CPU acquire, ms | 0.02367 [0.02234–0.02411] | 0.02287 [0.02159–0.02427] | **-3.38%** |
| CPU command record, ms | 0.01938 [0.01763–0.02038] | 0.01929 [0.01697–0.02033] | -0.46% |
| CPU queue submit, ms | 0.02503 [0.02401–0.02552] | 0.02517 [0.02474–0.02692] | +0.55% |
| CPU present, ms | 0.41364 [0.40850–0.42284] | 0.41258 [0.40871–0.41785] | -0.26% |
| GPU frame, ms | 0.35481 [0.35449–0.35502] | 0.35548 [0.35541–0.35570] | +0.19% |
| GPU geometry, ms | 0.32292 [0.32263–0.32294] | 0.32358 [0.32352–0.32383] | +0.20% |
| GPU workload, ms | N/A (3/3 unavailable) | N/A (3/3 unavailable) | — |
| GPU shadows, ms | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU postprocess, ms | 0.02968 [0.02966–0.02988] | 0.02968 [0.02967–0.02969] | +0.03% |
| GPU depth prepass, ms | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU voxel geometry, ms | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU voxel light, ms | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU visibility, ms | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |

#### canonical-gpu

| Метрика | before: median [min–max] | after: median [min–max] | Δ |
| --- | ---: | ---: | ---: |
| FPS | 1924.8 [1922.5–1932.3] | 1906.0 [1904.1–1925.3] | -0.98% |
| 1% low FPS | 1362.7 [1338.2–1407.1] | 1364.8 [1338.3–1381.5] | +0.15% |
| CPU frame, ms | 0.51954 [0.51752–0.52015] | 0.52467 [0.51941–0.52519] | +0.99% |
| CPU scene prepare, ms | 0.00096 [0.00078–0.00098] | 0.00078 [0.00077–0.00092] | **-18.93%** |
| CPU fence wait, ms | 0.00425 [0.00417–0.00524] | 0.00527 [0.00430–0.00534] | **+23.94%** |
| CPU acquire, ms | 0.02383 [0.02265–0.02456] | 0.02209 [0.02156–0.02428] | **-7.28%** |
| CPU command record, ms | 0.02163 [0.01815–0.02186] | 0.01888 [0.01867–0.02243] | **-12.70%** |
| CPU queue submit, ms | 0.02518 [0.02466–0.02595] | 0.02602 [0.02530–0.02612] | **+3.33%** |
| CPU present, ms | 0.40840 [0.40656–0.41341] | 0.41544 [0.41221–0.42225] | +1.72% |
| GPU frame, ms | 0.35702 [0.35661–0.35753] | 0.35703 [0.35662–0.35785] | +0.00% |
| GPU geometry, ms | 0.32522 [0.32498–0.32563] | 0.32523 [0.32488–0.32592] | +0.01% |
| GPU workload, ms | N/A (3/3 unavailable) | N/A (3/3 unavailable) | — |
| GPU shadows, ms | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU postprocess, ms | 0.02960 [0.02944–0.02969] | 0.02958 [0.02955–0.02972] | -0.08% |
| GPU depth prepass, ms | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU voxel geometry, ms | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU voxel light, ms | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU visibility, ms | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |

#### stress-cpu

| Метрика | before: median [min–max] | after: median [min–max] | Δ |
| --- | ---: | ---: | ---: |
| FPS | 485.2 [470.1–494.8] | 1018.6 [998.4–1028.3] | **+109.91%** |
| 1% low FPS | 236.8 [200.5–242.0] | 671.7 [633.2–715.2] | **+183.67%** |
| CPU frame, ms | 2.06088 [2.02110–2.12715] | 0.98178 [0.97244–1.00161] | **-52.36%** |
| CPU scene prepare, ms | 1.25373 [1.19832–1.30150] | 0.76024 [0.75390–0.77448] | **-39.36%** |
| CPU fence wait, ms | 0.00227 [0.00199–0.00228] | 0.00183 [0.00178–0.00187] | **-19.59%** |
| CPU acquire, ms | 0.02114 [0.02101–0.02115] | 0.01494 [0.01458–0.01598] | **-29.35%** |
| CPU command record, ms | 0.53896 [0.52726–0.55033] | 0.01853 [0.01755–0.02065] | **-96.56%** |
| CPU queue submit, ms | 0.02055 [0.02046–0.02131] | 0.01443 [0.01386–0.01450] | **-29.75%** |
| CPU present, ms | 0.20538 [0.20211–0.20921] | 0.14221 [0.14095–0.14376] | **-30.76%** |
| GPU frame, ms | 1.05491 [1.04958–1.06814] | 0.49245 [0.49122–0.49352] | **-53.32%** |
| GPU geometry, ms | 0.37508 [0.37473–0.37512] | 0.37402 [0.37399–0.37415] | -0.28% |
| GPU workload, ms | N/A (3/3 unavailable) | N/A (3/3 unavailable) | — |
| GPU shadows, ms | 0.15134 [0.14977–0.15700] | 0.08166 [0.08035–0.08278] | **-46.04%** |
| GPU postprocess, ms | 0.02908 [0.02886–0.02973] | 0.02852 [0.02847–0.02857] | -1.91% |
| GPU depth prepass, ms | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU voxel geometry, ms | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU voxel light, ms | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |
| GPU visibility, ms | 0.00000 [0.00000–0.00000] | 0.00000 [0.00000–0.00000] | — |

### Geometry и shadow work

| Workload | Source / visible / frustum rejected | Triangles | LOD0 / LOD1 / LOD2 |
| --- | --- | ---: | --- |
| canonical-cpu | 324 / 252 / 72 | 234868 | 244 / 5 / 3 |
| canonical-gpu | 324 / 252 / 72 | 234868 | 244 / 5 / 3 |
| stress-cpu | 100000 / 81314 / 18686 | 975769 | 81314 / 0 / 0 |

Эти финальные counters совпали во всех повторах обеих сторон. Для каждой
canonical workload равенство geometry — обязательный gate; также проверены
непустая geometry, `visible + frustum_rejected = source`, нулевой occlusion
и сумма LOD counters. У stress совпадение финального sample **не является
доказательством одинаковых поз и workload в каждом кадре**: траектория
задана elapsed time, частота кадров отличается. Проверка не навязывала
равенство его final counters.

| Workload / counter | Before: median [min–max] | After: median [min–max] |
| --- | ---: | ---: |
| canonical-cpu shadow passes / tiles | 1 / 4 во всех runs | 1 / 4 во всех runs |
| canonical-gpu shadow passes / tiles | 1 / 4 во всех runs | 1 / 4 во всех runs |
| stress-cpu shadow passes | 1562 [1556–1572] | 1763 [1759–1765] |
| stress-cpu shadow tiles | 6248 [6224–6288] | 7052 [7036–7060] |
| stress-cpu measured frames | 4852 [4702–4948] | 10186 [9984–10283] |

Shadow passes/tiles включают startup/warmup; frames — только measurement.
Во всех raw выполняется `tiles = 4 × passes`, filter metadata доступна.
Static canonical shadow pass выполнен до измерительного окна, поэтому
его measured GPU shadow timing нулевой. GPU visibility timing также нулевой
в статичном GPU control: серия не измеряет стоимость повторного dispatch
на движущейся GPU-камере. Compute workload не запускался, поэтому его timing
честно остаётся unavailable.

### Что подтверждено, а что остаётся гипотезой

**Stress CPU 100k:** scene preparation **−39.36%**, command record
**−96.56%**, CPU frame **−52.36%**, GPU frame **−53.32%**, FPS **+109.91%**,
1% low **+183.67%**. Все изменения значительно больше 3%, диапазоны разделены.
Fence wait, acquire, queue submit и present также снизились с разделёнными
диапазонами. Это повторяемый локальный выигрыш нового packet path при
частых изменениях видимости статических объектов; он не требует уменьшения
material precision или числа финальных видимых объектов.

**Canonical CPU:** FPS +0.04%, CPU frame −0.04%, 1% low −2.82%;
диапазоны перекрываются. Prepare −5.75%, fence wait −11.59%, acquire −3.38%
также имеют перекрытие и малые абсолютные значения — не объявляем их
отдельными подтверждёнными оптимизациями. GPU geometry +0.20% и frame +0.19%
имеют разделённые узкие диапазоны, но значительно ниже порога 3%.

**Canonical GPU control:** FPS −0.98%, CPU frame +0.99%, 1% low +0.15%,
GPU frame +0.003% и geometry +0.006%; диапазоны перекрываются. Изменения
отдельных CPU этапов >3% (prepare, fence wait, acquire, command record,
queue submit) также перекрываются. Контроль не показывает сопоставимого
со stress выигрыша или существенной регрессии; на статичной сцене нет
аналогичной массовой повторной загрузки CPU-visible packets.

На stress GPU geometry изменилась всего на **−0.28%**, postprocess на −1.91%,
а shadow average на **−46.04%**. Все три диапазона разделены, но именно
shadow среднее зависит от частоты обновлений и числа измеренных кадров:
после изменения кадров примерно в 2.10 раза больше, total shadow passes
выросли лишь примерно на 12.87%. Это не доказательство двукратного ускорения
одного shadow pass. Полный GPU frame включает работу вне geometry/shadow
интервалов, поэтому разность этих метрик не является отдельным измеренным
transfer timing.

[Контракт CPU packets](C:/GDF/vk_test/docs/CPU_SCENE_PACKETS.md) объясняет
изменение объёма работы: при неизменных source records и изменении camera
visibility передаются `4 × V` bytes индексов вместо `144 × (N + V)`
bytes повторного полного packet. При изменении source records добавляется
их загрузка. Это аналитический объём по структурам и update contract,
а не измеренный hardware bytes-transferred counter. Снижение CPU packing,
повторных transfers и сопутствующих stalls согласуется с результатом,
но вклад bandwidth, caches и scheduling отдельно не установлен.
RGP/PIX hardware counters и парный per-frame trace не снимались.

Результат относится к процедурной engine-нагрузке до появления production
content. Статичные source records и движущаяся камера — сильный случай
для reuse; массово меняющиеся transforms/materials требуют своего замера.
Эта performance-серия с validation off не заменяет отдельные image,
ASan, lifetime и Vulkan synchronization gates. При составлении сводки
сборки и GPU-проверки не запускались.

## E09: optional authored-model LOD fade — реализовано и проверено

CPU/GPU Bayer4/8 transitions подключены к authored model draw path с общей
complementary depth/color coverage. Управление доступно через CLI, editor и
типизированный MCP `render.set_lod_fade`; requested/actual состояние разделено.
Shadow reference сохраняет полную LOD0 геометрию, terrain остаётся discrete.
Default `off`. Контракт и ограничения:
[LOD_TRANSITIONS.md](C:/GDF/vk_test/docs/LOD_TRANSITIONS.md).

Три native Windows build/CTest завершились успешно, по 7/7:
[Debug](C:/GDF/vk_test/build/windows-debug/lod-fade-build.log),
[Release](C:/GDF/vk_test/build/windows-release/lod-fade-build.log),
[ASan](C:/GDF/vk_test/build/windows-asan/lod-fade-build.log).
[Повторная shader compile и spirv-val](C:/GDF/vk_test/build/windows-debug/lod-fade-shaders-recheck.log)
прошли Vulkan 1.3 / SPIR-V 1.5. Explicit GPU test собран в Debug и ASan;
он не добавлен в ordinary CPU CTest.

Оба GPU numeric запуска с synchronization validation завершились успешно:
[Debug](C:/GDF/vk_test/build/windows-debug/lod-fade-gpu-validation-recheck.log),
[ASan](C:/GDF/vk_test/build/windows-asan/lod-fade-gpu-validation-recheck.log).
Проверены 4194304 complementary pixel pairs (обе роли), 131072 quantized
encoder roundtrips, 49620 strict finite encoder cases и столько же source-mask
cases, 1024 plain-off pixels. Reference использует явные Bayer matrices и
integer-ceiling thresholds; encoder cases включают соседние float значения
вокруг всех 4095 half-steps. Ошибок Vulkan/ASan нет, live GPU allocations 0.
GPU buffers этого теста — 1592288 bytes, readback — 96 bytes; это размеры
test fixture, не runtime storage всего renderer.

Procedural coverage использует три coplanar LOD с одинаковым силуэтом и
128/32/2 triangles. Debug CPU и GPU получили по 76 captures и 38/38 успешных
сравнений: 26/26 exact для AA off/SMAA, prepass off/partial/full, resize и
mode restore; 12/12 FSR temporal smoke. Каждый прогон отверг 20 invalid MCP
payloads без изменения состояния:
[CPU summary](C:/GDF/vk_test/build/windows-debug/render-validation/lod-fade-coverage/cpu-coverage-v2/1-summary.json),
[GPU summary](C:/GDF/vk_test/build/windows-debug/render-validation/lod-fade-coverage/gpu-coverage-v2/1-summary.json).
GPU validation сначала обнаружил реальную ошибку: после переключения fade в
`off` быстрый путь обновлял один lane, но emission читал остальные stale history
lanes и оставлял двойной LOD draw. После запрета full-history read для `off`
тот же неизменённый сценарий прошёл полностью.

Полный Windows ASan GPU-runtime прогон повторил 76 captures, 38/38 сравнений и
20 invalid requests; ошибок ASan/Vulkan и ненулевых live allocations нет,
canonical scene осталась неизменной:
[ASan summary](C:/GDF/vk_test/build/windows-asan/render-validation/lod-fade-coverage/gpu-asan-coverage-v2/1-summary.json).
Предыдущий ASan запуск выполнил сравнения, но его итоговый canonical guard
сработал из-за параллельного сохранения сцены пользователем; как завершённый
gate он не учитывается.

### Clean fixed-clock A/B

Принята только серия
[v2 summary](C:/GDF/vk_test/build/windows-release/lod-fade-ab-20260913-v2/LodFade-summary.json)
с [environment](C:/GDF/vk_test/build/windows-release/lod-fade-ab-20260913-v2/LodFade-environment.json):
RX 6800 stable 1825 MHz / memory 1000 MHz, Ryzen 7 9700X, Windows Release,
1920×1080, IMMEDIATE, validation off, AA off, legacy lighting, GI/shadows/
prepass/sort off, animated camera, warmup 5 s + measurement 10 s, 3 повтора,
rotating case order. Все 36/36 source JSON прошли проверку конфигурации и SHA
executable; после серии clocks восстановлены в Normal. Добавленный process
guard запрещает активные `vulkan_cube`/`vulkan_editor` до серии и между runs.

| Sources | Visibility | Pattern | Median FPS | Δ FPS | Median GPU frame, ms | Δ GPU frame |
| ---: | --- | --- | ---: | ---: | ---: | ---: |
| 1 | CPU | off | 5247.31 | reference | 0.030354 | reference |
| 1 | CPU | Bayer4 | 5250.18 | +0.05% | 0.030441 | +0.000087 ms / +0.29% |
| 1 | CPU | Bayer8 | 5282.88 | +0.68% | 0.030404 | +0.000050 ms / +0.16% |
| 1 | GPU | off | 5118.55 | reference | 0.037851 | reference |
| 1 | GPU | Bayer4 | 5164.57 | +0.90% | 0.038135 | +0.000285 ms / +0.75% |
| 1 | GPU | Bayer8 | 5156.29 | +0.74% | 0.038936 | +0.001085 ms / +2.87% |
| 1024 | CPU | off | 4190.65 | reference | 0.097550 | reference |
| 1024 | CPU | Bayer4 | 4150.25 | −0.96% | 0.102803 | +0.005253 ms / +5.38% |
| 1024 | CPU | Bayer8 | 4173.61 | −0.41% | 0.102653 | +0.005103 ms / +5.23% |
| 1024 | GPU | off | 4515.15 | reference | 0.102846 | reference |
| 1024 | GPU | Bayer4 | 4344.02 | −3.79% | 0.108709 | +0.005863 ms / +5.70% |
| 1024 | GPU | Bayer8 | 4384.81 | −2.89% | 0.106600 | +0.003754 ms / +3.65% |

Один объект ниже надёжного разрешения полного FPS-измерения: GPU delta там
0.00005–0.00109 ms, а FPS меняется в обе стороны. На синтетических 1024
перекрывающихся объектах дополнительная geometry coverage даёт устойчивые
0.0038–0.0059 ms GPU frame. CPU/GPU frame и 1% low сильнее шумят; большой
выигрыш не заявляется. Устойчивого преимущества Bayer4 над Bayer8 нет.

Серия `lod-fade-ab-20260913-v1` исключена: пользователь сообщил, что одновременно
работал `vulkan_editor`; чистый повтор заметно изменил baseline. Артефакты v1
сохранены только как отрицательное свидетельство contaminated measurement.

**Решение:** сохранить реализацию как рекомендуемую опцию для authored models,
когда на реальном контенте виден LOD popping. Default оставить `off`; включение,
паттерн и длительность выбирать после content capture. Буквальное копирование
camera-space dither в shadow map пропустить: текущий LOD0 shadow reference
устраняет camera-LOD shadow pops без несогласованных light-space holes.

## E10: point/spot lights и clustered forward — реализовано и рекомендовано

Добавлен единый KHR-style контракт до 4096 point/spot lights: linear RGB,
candela, finite range, local `-Z` spot direction, smooth distance cutoff и
squared cone falloff. Он проходит через scene JSON, editor и типизированный MCP.
GPU получает per-frame SSBO после fence своего frame. Прямой forward loop
сохранён как эталон; local direct lighting пока не имеет shadows и не
инжектируется в voxel GI.

Clustered path использует сетку `32x18x24`, 64 indices на cluster и
light-centric dispatch с одной 64-thread workgroup на light. Переполненный
cluster не теряет lighting: fragment shader переключается на bounded full-forward
loop по всем lights, поэтому `dropped=0`. Runtime публикует assignments,
max list length, overflow cluster count и `gpu_light_culling_ms`.

Три native Windows конфигурации прошли по 7/7 CTest. Shader sources повторно
скомпилированы для Vulkan 1.3 / SPIR-V 1.5 и прошли `spirv-val`. Debug validation
получил точное PNG equality forward/clustered при 256 separated и overlap lights;
overlap при этом реально переполнил 3384 clusters. Stress 4096 дал:

| Layout | Assignments | Max list | Overflow clusters | Dropped |
| --- | ---: | ---: | ---: | ---: |
| separated | 605211 | 290 | 3835 | 0 |
| overlap | 10494648 | 4096 | 3456 | 0 |

ASan validation успешно прошла overlap 4096 runtime и editor authoring. Ошибок
Vulkan/ASan и потерь lights в принятых gates не обнаружено.

### Fixed-clock LocalLights v3

Принята только
[v3 summary](C:/GDF/vk_test/build/windows-release/local-lights-ab-20260913-v3/LocalLights-summary.json)
с [environment](C:/GDF/vk_test/build/windows-release/local-lights-ab-20260913-v3/LocalLights-environment.json):
AMD Radeon RX 6800 core 1825 MHz / memory 1000 MHz, Ryzen 7 9700X, native
Windows Release, 1280x720 IMMEDIATE, AA/GI off, warmup 2 s, measurement 4 s,
3 повтора. Всего завершено 60 runs: layouts separated/overlap, counts
1/16/64/256/4096 и forward/clustered. Process guard подтвердил отсутствие
параллельных renderer/editor процессов; после серии clocks восстановлены в
Normal. v1 исключён из-за неверного timestamp, v2 был прерванным exploratory
run и в решение не входит.

| Layout | Lights | Forward GPU frame, ms | Clustered GPU frame, ms | Delta clustered | Cluster culling, ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| separated | 1 | 0.0730 | 0.0811 | +11.12% | 0.0047 |
| separated | 16 | 0.0995 | 0.0864 | -13.14% | 0.0087 |
| separated | 64 | 0.1880 | 0.1035 | -44.89% | 0.0214 |
| separated | 256 | 0.5466 | 0.1221 | -77.63% | 0.0246 |
| separated | 4096 | 9.3669 | 9.4571 | +0.96% | 0.0438 |
| overlap | 1 | 0.0759 | 0.1053 | +38.39% | 0.0235 |
| overlap | 16 | 0.1400 | 0.1663 | +18.81% | 0.0211 |
| overlap | 64 | 0.3420 | 0.3723 | +8.86% | 0.0280 |
| overlap | 256 | 1.1886 | 1.1711 | -1.47% | 0.0316 |
| overlap | 4096 | 19.1712 | 19.1342 | -0.19% | 0.1407 |

Separated volumes показывают полезный crossover уже между 1 и 16 lights и
крупный выигрыш clustered при 64/256. Полностью перекрывающиеся volumes —
неблагоприятный случай: до 64 lights dispatch/list overhead дороже reference,
а при 256/4096 overflow fallback делает пути почти равными. Значения 4096 также
включают lossless fallback и не характеризуют чистый clustered shading.

**Решение:** E10 принят и рекомендован как engine subsystem. Простой forward
остаётся reference/fallback, `auto` выбирает clustered начиная с 64 lights.
Порог консервативен для overlap и даёт существенный выигрыш для обычного
separated stress case. Production content validation позднее может уточнить
policy, но не требуется для приёмки bounded point/spot и clustered contracts.

## E13: histogram auto-exposure и 3D grading LUT — реализовано и рекомендовано

Display mapping теперь использует единый HDR contract: 256-bin log-luminance
histogram читает общий scene radiance до AA, а не отдельный diffuse target или
raw albedo. Percentile rejection, uniform/center metering, EV limits,
compensation и независимые brighten/darken constants выполняются на GPU.
Fixed/manual override остаётся default и при AA off/SMAA не запускает compute.
FSR internal auto-exposure отключён: compute записывает один float одновременно
в state buffer для GT7/Reinhard и external `1x1 R32F` image для upscaler при
`preExposure=1`.

Grading LUT имеет явно заданный domain: display-linear Rec.709 `[0,1]` после
tone mapping и до единственного sRGB encode. Реализованы identity `RGBA16F`
32³/64³, hardware trilinear, explicit tetrahedral и bypass без sampling.
Identity занимает суммарно 2.25 MiB resident memory; ресурсы живут на device
и не пересоздаются при resize. Production artistic LUT не создавался без
контента, но весь engine API, interpolation и validation contract готов.

Три native Windows сборки прошли по 8/8 CTest. Shader compile/`spirv-val`,
Debug synchronization validation для AA off/SMAA/FSR, resize/mode switch,
ASan runtime FSR и editor MCP authoring завершились без ошибок. Explicit GPU
reference tests подтвердили 8 exposure cases со всеми 256 exact bins и
maximum adaptation error `4.77e-7`, а также 144 LUT cases с maximum errors
`3.13e-5` trilinear и `4.15e-8` tetrahedral. Live GPU allocations после тестов
равны нулю. Identity fullscreen captures отличаются от bypass максимум на
1 LSB 8-bit output.

Чистая 18-run серия
[DisplayMappingE13](C:/GDF/vk_test/build/windows-release/display-mapping-e13-v1/DisplayMapping-summary.json)
на RX 6800, Ryzen 7 9700X, Windows Release, 1920x1080 IMMEDIATE дала:

| Mode | Display, ms | Exposure, ms | GPU frame, ms | FPS |
| --- | ---: | ---: | ---: | ---: |
| fixed/off | 0.08091 | 0 | 0.14505 | 3540.25 |
| fixed/trilinear 32³ | 0.08534 | 0 | 0.14887 | 3496.81 |
| fixed/tetrahedral 32³ | 0.09263 | 0 | 0.15620 | 3383.59 |
| fixed/trilinear 64³ | 0.08575 | 0 | 0.14945 | 3475.55 |
| fixed/tetrahedral 64³ | 0.09348 | 0 | 0.15701 | 3411.59 |
| auto/off | 0.08022 | 0.05333 | 0.19728 | 2944.33 |

Process guard исключил параллельный editor до начала первой попытки; принятая
серия не имела конкурирующих renderer/editor процессов. Clocks не фиксировались,
поэтому вывод ограничен одной непрерывной rotating-order серией. Измерение
показывает, что 64³ почти не меняет shader interval относительно 32³, а
tetrahedral примерно на 0.007–0.008 ms дороже trilinear. Без production LUT нет
quality-основания платить эту цену.

**Решение:** общий HDR histogram auto-exposure рекомендован как опциональный
camera/display режим; absolute GPU cost 0.053 ms приемлем, fixed остаётся
benchmark default. Grading infrastructure рекомендована; первый content-backed
режим должен использовать 32³ trilinear, пока визуальная ошибка реального LUT
не докажет пользу 64³ или tetrahedral. Требование видео использовать только
diffuse exposure и всегда 64³ tetrahedral отклонено.

## E14: full-resolution masked separable SSS — реализовано и рекомендовано

Добавлен непрозрачный `subsurface` material model с профилями `narrow`,
`medium`, `wide` и strength `[0,1]`. Geometry path отделяет рассеиваемую долю
diffuse от specular/HDR base. Exact `profile * 256 + strengthUNorm8` control в
`RGBA16F` сохраняет дискретную границу profile без stencil; reverse-Z `R32F`
depth ограничивает перенос между поверхностями. Два full-resolution 7-tap H/V
прохода используют независимый ping target, проверку profile/depth и затем
композитят filtered diffuse с неразмытым base. Режим `off` остаётся default;
targets создаются лениво при первом `full` frame.

Текущий mask ограничивает данные convolution по profile, но оба passes всё ещё
растеризуют fullscreen triangle и делают ранний выход на `profile == none`.
Поэтому измеренный interval включает full-extent fragment overhead. Stencil или
tile compaction отложены до content-backed измерения фактического skin coverage.

CLI, editor и типизированный MCP `render.set_subsurface` управляют режимом.
Scene/cooked JSON, node inspector и MCP authoring сохраняют model/profile/
strength; importer явно оставляет `none`/`0` и не угадывает SSS. Процедурный
`subsurface-profiles` содержит 28 сфер, включая 20 активных SSS материалов,
три profile, две strength, glossy SSS и standard/unlit controls.

Принята финальная чистая rotating-order серия
[summary](C:/GDF/vk_test/build/windows-release/subsurface-benchmark/summary.json)
от 12:07 после исправления общего 24-pixel kernel-radius cap и перекомпиляции
SPIR-V: AMD Radeon RX 6800, driver 8388961, Windows Release, 1920×1080
IMMEDIATE, AA off, dynamic lighting, GI/day-cycle off, fixed exposure EV 0,
три повтора, warmup 3 s и measurement 6 s. Process guard выполнялся перед каждым
run; per-run GPU values стабильны.

| Mode | Median FPS | GPU frame, ms | Geometry, ms | Postprocess, ms | SSS interval, ms | Resident memory |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| off | 2661.80 | 0.16136 | 0.12378 | 0.03445 | 0 | 0 |
| full | 2409.03 | 0.22762 | 0.10174 | 0.12416 | 0.09864 | 63.28125 MiB |

`full` дал −9.50% FPS и +0.06626 ms GPU frame. Postprocess вырос на 0.08971 ms,
geometry снизился на 0.02204 ms между чередующимися runs. Отдельный SSS interval
входит в postprocess и не складывается с ним. Снижение geometry не считается
ускорением geometry path; решение опирается на end-to-end стоимость полного
GPU frame. Clock state не сохранён в summary, поэтому цифры относятся к этой
непрерывной серии и не сравниваются напрямую с результатами других этапов.

Debug/Release/ASan прошли 8/8 CTest. Debug synchronization validation прошла
dynamic AA off/SMAA/FSR и legacy AA off; ASan synchronization validation — FSR
runtime. Editor MCP smoke прошёл с allowlist 22 tools. Material GPU reference
проверил 30 материалов, exact model/profile/strength bits, maximum F0 error
`3.774273360513e-08` и ноль live allocations. В fixed 1280×720 off/full captures
изменились 18 136 из 921 600 pixels (1.9679%), maximum absolute RGB difference
21/5/1. Визуальный просмотр не выявил явного profile/depth bleeding, но это не
pixel equivalence и не проверка production кожи.

**Решение:** E14 SSS принят как рекомендуемая опциональная engine subsystem;
измеренная цена мала в абсолютном времени, но заметна относительно короткого
baseline frame, а память зависит от полного output extent. Default остаётся
`off`; включение оправдано для материалов, которым нужен screen-space diffusion.
Dedicated eye renderer, `MaterialModel::eye`, transparent cornea/tear layers,
calibrated sclera scattering и pupil response отложены до определения целевой
eye geometry/UV/content contract.

## E15: forward MSAA 2×/4× — реализовано и рекомендовано как quality option

Добавлены отдельные `msaa2x`/`msaa4x`, exact format/sample-count detection,
prewarmed geometry/sky/depth-prepass/EQUAL variants и per-frame multisampled
`RGBA16F` color/depth targets. Color разрешается inline через
`VK_RESOLVE_MODE_AVERAGE_BIT`, multisample source не хранится; sample shading и
depth resolve выключены. Targets создаются или заменяются после fence своего
frame slot, поэтому runtime switch/resize не вызывает device idle и steady
state не аллоцирует. Typed runtime/editor MCP и benchmark JSON сообщают
requested/actual mode/samples, support, formats, resolve, extent и bytes.

Full SSS с MSAA строго несовместим: packed FP16 alpha хранит категориальный
profile ID вместе со strength, и average resolve испортил бы material mask.
Startup CLI и runtime/editor MCP отклоняют комбинацию в обоих направлениях.
Particles и SDF этим этапом не затрагивались согласно решению пользователя.

Процедурный `aa-wires` содержит 17 unlit cube instances. Quality gate сравнил
640×360 captures с Off-reference 2560×1440, усреднённым 4×4 в linear light.
В 2350 reference edge pixels получены MAE:

| Mode | Edge MAE | Interior MAE, 80 967 pixels |
| --- | ---: | ---: |
| Off | 0.075373 | ≈0 |
| SMAA high | 0.053197 | 0.000164 |
| MSAA 2× | 0.059043 | ≈0 |
| MSAA 4× | **0.036500** | ≈0 |

MSAA4 лучше всего совпал с supersampled reference; MSAA2 улучшил Off, но на этом
fixture уступил SMAA. Полностью покрытые MSAA controls не изменились.

Обе clean rotating-order Release серии выполнены на AMD Radeon RX 6800,
driver 8388961, 1920×1080 IMMEDIATE, fixed EV0, 3 повтора, warmup 3 s,
measurement 6 s. Process guard проверял отсутствие renderer/editor до и после
каждого run. Clocks/power telemetry не фиксировались.

| Fixture / mode | Median FPS | GPU frame, ms | Geometry, ms | Postprocess, ms | Resident memory |
| --- | ---: | ---: | ---: | ---: | ---: |
| aa-wires / Off | 3128.33 | 0.11408 | 0.06501 | 0.04392 | 0 |
| aa-wires / SMAA | 3164.68 | 0.14772 | 0.03632 | 0.10982 | 0 |
| aa-wires / MSAA2 | 2413.64 | 0.20623 | 0.17611 | 0.02725 | 94.9 MiB |
| aa-wires / MSAA4 | 2383.15 | 0.21019 | 0.17973 | 0.02776 | 189.8 MiB |
| overdraw-4 / Off | 1971.00 | 0.37877 | 0.35213 | 0.02486 | 0 |
| overdraw-4 / SMAA | 1464.13 | 0.48803 | 0.35462 | 0.13214 | 0 |
| overdraw-4 / MSAA2 | 1426.54 | 0.51217 | 0.48443 | 0.02592 | 94.9 MiB |
| overdraw-4 / MSAA4 | 1372.95 | 0.53340 | 0.50174 | 0.03000 | 189.8 MiB |

На `aa-wires` MSAA2/4 добавили `0.09215/0.09611 ms` GPU frame; на
`overdraw-4` — `0.13339/0.15463 ms`. Процент FPS велик из-за очень коротких
baseline frames и CPU/fence interaction; решение основано на absolute GPU cost,
quality и memory. Off-run у `overdraw-4` имел широкий диапазон
1697.45–1972.40 FPS, поэтому median сохранён, а результат не выдаётся за
fixed-clock универсальную оценку.

Debug/Release/ASan прошли 8/8 CTest. Все shaders скомпилированы и прошли
`spirv-val`. Debug sync validation прошла MSAA2/4 с full depth prepass и
`aa-wires`; editor MCP smoke прошёл 23 tools. ASan+sync smoke прошёл
2×↔4×↔Off, resize 640×360→704×396, screenshots и отрицательные SSS/MSAA cases
без validation/ASan/resource ошибок.

**Решение:** MSAA4 рекомендован как отдельный native-resolution quality mode
для геометрических coverage edges, если допустимы ~189.8 MiB и измеренная GPU
цена. MSAA2 сохраняется как hardware option с половиной attachment memory, но
не заменяет SMAA по качеству на текущем fixture. Default остаётся Off; SMAA
остаётся более дешёвой spatial альтернативой. Deferred coverage classification
из видео по-прежнему рекомендуется пропустить без отдельного решения о G-buffer.

## E16: SMAA T2x и detector A/B — optional T2x принят

SMAA1x сохранён без изменения default. Новый `smaa_mode=t2x` работает в
native resolution и хранит строго previous non-recursive neighborhood result,
а не предыдущий temporal resolve. Две фазы используют jitter
`±(0.25, -0.25)` и reference subsample indices; resolve выполняет motion
reprojection, current-neighborhood clamp и motion attenuation. History validity
и phase сбрасываются при resize, camera cut и несовместимых изменениях состояния.

Два уже существующих per-frame `RGBA16F` FSR outputs переиспользуются для
current/previous history. При 1920×1080 это 33 177 600 bytes, или 31.64 MiB,
resident capacity; incremental allocation T2x равна 0 bytes. Runtime/editor
typed `render.set_anti_aliasing` принимает optional `smaa_mode` со значениями
`1x|t2x`. State и benchmark JSON публикуют mode, temporal sample count,
history validity/phase, extent, resident/incremental bytes и reuse flag.

### Detector A/B

Equal-luma red/green boundary отделяет chromatic sensitivity от обычного
contrast. На isoluminant ROI из 74 pixels получены:

| Detector | MAE |
| --- | ---: |
| Color | **0.0288994** |
| Luma | 0.0536648 |

Luma хуже примерно в 1.86 раза. Его clean performance delta против color
составила `+0.0454%` FPS, `-0.00194 ms` GPU frame и `-0.00230 ms`
postprocess, то есть находится в шуме. Color остаётся единственным/default
detector; luma рекомендуется пропустить. Depth detector также рекомендуется
пропустить: он не видит same-depth material boundaries, specular/normal-map и
shadow edges, требует sampled depth и отдельного threshold contract, особенно
неустойчивого между reverse-Z depth ranges.

### Equal-luma quality gate

Последняя версия `aa-wires` сравнивалась с 4× linear-resolution Off reference.
Это новый результат после equal-luma изменения fixture; цифры E15 выше остаются
историческим исходным MSAA gate.

| Mode | Edge MAE | Interior MAE |
| --- | ---: | ---: |
| Off | 0.07364754 | ≈0 |
| SMAA 1x | 0.05203927 | 0.00013566 |
| SMAA T2x | 0.04802657 | 0.00056777 |
| FSR 3 Native AA | **0.03433304** | 0.00043068 |
| MSAA 2× | 0.05764277 | ≈0 |
| MSAA 4× | 0.03548015 | ≈0 |

T2x улучшил edge MAE относительно SMAA1x примерно на 7.7%. Он остаётся хуже
MSAA4 и FSR Native на этом static fixture и даёт больший interior drift, хотя
его абсолютное значение мало.

### Clean performance A/B

Измерения выполнены на AMD Radeon RX 6800, driver 8388961, Windows Release,
1920×1080, IMMEDIATE, три повтора, warmup 3 s и measurement 6 s. Editor был
закрыт; process guard проверял отсутствие renderer/editor до и после runs.
FSR Native измерялся отдельной серией, поэтому его значения не используются как
чередующаяся пара для вычисления delta остальных режимов.

| Fixture / mode | GPU frame, ms | Postprocess, ms |
| --- | ---: | ---: |
| aa-wires / SMAA 1x | 0.1169617 | 0.0877499 |
| aa-wires / SMAA T2x | 0.1582523 | 0.1178376 |
| aa-wires / MSAA 4× | 0.1234626 | 0.0156178 |
| aa-wires / FSR 3 Native AA, separate | 0.6530913 | 0.6121606 |
| overdraw-4 / SMAA 1x | 0.4446806 | 0.1015921 |
| overdraw-4 / SMAA T2x | 0.5124975 | 0.1464892 |
| overdraw-4 / MSAA 4× | 0.4598950 | 0.0161829 |
| overdraw-4 / FSR 3 Native AA, separate | 1.0487279 | 0.6794999 |

T2x добавил `0.04129 ms` GPU frame на `aa-wires` и `0.06782 ms` на
`overdraw-4` относительно SMAA1x. На текущем fixture MSAA4 одновременно
качественнее и быстрее T2x, но требует 189.8 MiB multisample attachments; T2x
не добавляет memory. FSR Native дал лучшее edge MAE, но стоил существенно больше.
Текущие T2x-цифры включают запись и transition общего с FSR full-resolution
`R32F` depth target, который SMAA resolve пока не читает; отдельный color+motion
geometry variant может снизить эту стоимость и требует нового A/B.

**Решение:** T2x сохраняется как optional native-resolution AA без изменения
defaults. Luma и depth detectors рекомендуется пропустить. SMAA S2x и
комбинированный SMAA4 отложены: нужны stored sampled 2× source, subsample
separation, две SMAA chains, standard sample locations и multisample velocity
contract, тогда как существующий MSAA4 сейчас проще и доминирует по
quality/performance, кроме memory.

Static fixture дополнен moving-camera synchronization/screenshot smoke, но это
не доказывает production motion quality. Disocclusion, animated objects,
foliage/thin alpha geometry и glossy specular content требуют отдельного QA после
появления соответствующего контента.

## E17: optional motion blur — реализован и проверен

Default-off path использует `RG16F` current-to-previous motion и `R32F`
reverse-Z. Camera/object/all вклад восстанавливается через inverse current и
current/previous unjittered view-projection matrices. Centered/trailing shutter,
angle, maximum radius и camera fade задаются независимо. Пауза больше 250 ms,
camera cuts и изменения настроек сбрасывают history и дают snap вместо длинного
ложного следа.

`16×16` TileMax и `3×3` NeighborMax ограничивают область поиска; low/high
используют 8/16 taps. Дополнительного full-resolution target нет. Два per-frame
`RGBA16F` tile images занимают около 261 120 bytes при 1080p и двух frame slots.
AA Off и SMAA1 поддержаны, причём blur выполняется до SMAA edge detection; FSR
Native обрабатывается после upscale. MSAA, SMAA T2x и scaled FSR явно отвергаются.
HUD и editor компонуются после blur.

Validation обнаружил, что обычный SMAA shader статически объявлял binding 5,
предназначенный для blurred input. Это исправлено отдельными normal/blurred
SPIR-V variants; normal SMAA больше не требует фиктивного blurred descriptor.

Typed `render.set_motion_blur` доступен в runtime и editor MCP; smoke inventory
содержит 30/24 tools. State и benchmark JSON возвращают requested/actual mode,
quality, shutter, angle/radius/camera fade и `average_gpu_motion_blur_ms`.

### Clean Release A/B

AMD Radeon RX 6800, driver 8388961, Windows Release, 1920×1080, IMMEDIATE,
animated `motion-bars`, AA Off, GI/day cycle Off, fixed exposure и fixed
procedural simulation step `motion_fixture_fps=60`. Это не FPS cap: render loop
остаётся uncapped. Warmup 3 s, duration 6 s, три rotating runs. Editor был
закрыт; process guard проверял отсутствие renderer/editor. Clock/power telemetry
не записывалась.

| Mode | Median FPS | CPU frame, ms | GPU frame, ms | Postprocess, ms | Motion blur, ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Off | 3071.628 | 0.325560 | 0.124407 | 0.039817 | 0 |
| Low, 8 taps | 2866.402 | 0.348869 | 0.192682 | 0.134150 | 0.131721 |
| High, 16 taps | 2757.383 | 0.362663 | 0.224867 | 0.166420 | 0.163991 |

Полный GPU frame вырос на `0.068275 ms` для low и `0.100460 ms` для high;
FPS снизился на `6.68%/10.23%`. Fixture очень лёгкий и CPU-bound, однако
измеренный рост CPU frame и падение FPS нельзя целиком списывать на шум. 1% low
остаётся шумным. Blur timing стабилен и разделяет presets: low
`0.131629..0.131920 ms`, high `0.163904..0.163994 ms`, поэтому 8/16 taps реально
дают разную стоимость.
Motion-blur interval входит в postprocess и потому не складывается с ним.
Машинная сводка:
[MotionBlur-summary.json](C:/GDF/vk_test/build/windows-release/e17-motion-blur-fixed60-20260913a/MotionBlur-summary.json).

Shader compile + `spirv-val`, Debug/Release/ASan 8/8 CTest, Debug runtime/editor
sync MCP, отдельные SMAA1/FSR Native sync, ASan runtime/editor и unsupported CLI
rejects прошли. Procedural `motion-bars` содержит две противоположно движущиеся
полосы; screenshots и sync подтверждают структуру и корректность resource path.
CLI/JSON `--motion-fixture-fps 30|60|144` задаёт fixed simulation step. Visual
Debug off/high capture выполнен при fixed30, ASan — при fixed60.
Temporal supersample reference и production transparent/skinned content не
проверялись, поэтому production motion quality не заявляется.

**Решение:** E17 принят как optional default-off engine feature с малой
абсолютной GPU ценой на текущем fixture. Centered/trailing shutter и camera fade
остаются художественными controls. Три отвергнутых в спецификации физических
закона не приняты как обязательные. Завершённый scope E18 integration matrix
приведён ниже.

## E18: интеграционная матрица реализованных путей — завершена

Debug `rendering-pipeline-e18-motion2` и ASan `rendering-pipeline-e18-motion`
прошли пять AA methods: Off, SMAA, MSAA2, MSAA4 и FSR3. Матрица включает tone
mapper roundtrips, resize, present и render-mode switches; validation errors и
live GPU allocations равны нулю.

Финальные combined cases прошли для Debug SMAA1 и FSR Native, а также ASan
SMAA1: одновременно активны `subsurface-profiles`, full SSS с 20 active
materials, auto exposure, tetrahedral LUT64 и motion `all/high`. Это проверяет
совместное владение HDR/depth/motion/history resources и порядок passes.

После последних C++ изменений Debug и ASan validation повторены с параметрами
`-Startups 1 -Cycles 1 -Scenario rendering-pipeline -RunTag e18-final`. Оба
результата содержат 22 captures, все пять AA methods,
`validation_errors=0` и `live_gpu_allocations=0`:
[Debug summary](C:/GDF/vk_test/build/windows-debug/render-validation/rendering-pipeline-e18-final-summary.log)
и [ASan summary](C:/GDF/vk_test/build/windows-asan/render-validation/rendering-pipeline-e18-final-summary.log).

E18 закрыт для реализованных E01–E10 и E13–E17 paths. E11 particles, E12 SDF и
eye specialization остаются content-deferred по решению пользователя. Условный
G-buffer/deferred path пропущен. Эти ветки и production content quality текущей
матрицей не подтверждены.
