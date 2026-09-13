# CPU source records и индексы видимости

Реализация подготовлена после MaterialABI A/B: расширение материала усилило
стоимость пути, который при каждом изменении видимости заново создавал
`[N source records][V visible copies]` и передавал весь пакет GPU.
Повторная передача неизменных source records подтверждается кодом; приписывать
весь измеренный GPU regression только bandwidth без hardware counters нельзя.

Новый CPU путь использует тот же vertex indirection, что GPU visibility:

- `scene_packet_` содержит N исходных `RuntimeGpuInstance` со stride 144.
  Их revision меняется при изменении snapshot, материалов, current/previous
  transforms, включая settling previous=current после последнего движения.
- `visible_indices_` содержит V uint source indices в прежнем порядке
  ground/cube/model/material/LOD buckets; coarse sorting меняет этот список.
- Каждый frame slot отдельно хранит source revision и общий packet revision.
  При смене камеры передаются только изменившиеся индексы; при изменении данных
  обновляются source records. Полная frame invalidation принуждает обе загрузки.
- Camera draws используют firstInstance как offset в index list. Shadow draws
  продолжают читать original records напрямую, независимо от camera visibility.
  Stable object ID, full material factors и previous transforms сохраняются.

## GPU storage и synchronization

`frame.visibility.visible` содержит прежний 32-byte header и три слоя индексов
для GPU LOD. CPU пишет только entries с offset 32; header ему не нужен.
Buffer создаётся для authored scene до optional GPU-culling feature checks:
CPU vertex indirection не требует compute или indirect-first-instance features.
Размер и descriptor binding 5 остаются совместимыми с GPU visibility.

Frame fence ожидается до mapped writes. Source data занимает начало staging,
индексы — следующий непересекающийся диапазон. Bounds проверяются до copy.
Обе transfer writes переходят к vertex shader storage reads через точные
buffer barriers. CPU index write инвалидирует GPU view cache этого frame.
GPU dirty-record upload сбрасывает CPU source stamp; существующие переходы
CPU/GPU инвалидируют source packets и соответствующие frame slots.

Instancing использует отдельные buffers: authored upload в этом режиме
пропускается, чтобы не перезаписывать cached GPU index list перед возвращением
в scene. Resize не меняет lifetime этих buffers; recreated descriptors
привязывают те же frame-owned allocations.

## Capacity и стоимость

Для Nmax=100001 source records:

| Хранение | До, bytes | После, bytes |
| --- | ---: | ---: |
| CPU source packet capacity | 28800288 | 14400144 |
| Два device-preferred instance buffers | 57600576 | 28800288 |
| Два mapped upload buffers | 57600576 | 29600296 |

Суммарное уменьшение запрошенной capacity — 71200712 bytes (около 67.9 MiB),
без allocator bookkeeping/alignment. На устройствах без GPU-culling support
новый общий index storage дополнительно требует 2400088 bytes; на RX 6800
эти два buffers уже существовали. Фактический heap размещения выбирает VMA.

При V видимых records и неизменных данных index update передаёт `4*V` bytes
вместо `144*(N+V)`; при изменении source records добавляются `144*N` bytes.
Source-only storage также позволяет последующим LOD transitions передавать
несколько индексов одного source без копирования материала/transform payload.
Этот документ не утверждает, что LOD cross-fade уже подключён.

## Проверка

Исходная source/index реализация прошла Windows Debug/Release/ASan build и
CTest 7/7. Debug canonical CPU/GPU и stress: по 34 captures; ASan canonical:
68; animated CPU: 5 Debug и 20 ASan captures. Все завершились без validation
errors, ASan diagnostics и live GPU allocations. 56/56 сравниваемых decoded
PNG совпали побитово с предыдущей material реализацией. Editor MCP в Debug
и ASan: по 168 checks/20 invalid payload rejects, исходная сцена неизменна.
Clang-Tidy/MSVC завершились без ошибок; новые подтверждённые lifetime/bounds
проблемы не найдены, общие advisory warnings остаются.

`CpuPackets` A/B: 18 прогонов, RX 6800, 1920×1080, IMMEDIATE, Release,
AMD Stable, 5 s warmup + 10 s measurement, три повтора. На CPU stress 100k
median FPS вырос на 109.91%, CPU frame уменьшился на 52.36%, GPU frame —
на 53.32%; geometry interval изменился на −0.28%. Canonical CPU FPS +0.04%,
canonical GPU control −0.98%. Это выигрыш данной нагрузки и пути подготовки/
передачи данных, а не универсальное ускорение shading. Normal clocks восстановлены.

Baseline — сохранённый
`material-runtime/vcr_material_profile.exe` со старым CPU packing и теми же
48/144-byte material/runtime ABI. Поэтому это отдельный замер, не повторная
оценка стоимости расширения IOR. Подробные диапазоны и ограничения находятся в
`RENDERING_PIPELINE_RESULTS.md`. Снимок после оптимизации:
`cpu-packets-runtime/vcr_cpu_packets_profile.exe`, SHA256
`7E777A44A72B0FE05A3615299A601B3A76281D46264273CE6C7DCE4C574DE0D7`.

Эти gates и capacity относятся к снимку до E09. В текущей разработке LOD fade
расширяет индексную capacity до 2N и добавляет transition state; его стоимость
и проверки учитываются отдельно.
