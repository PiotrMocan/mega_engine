# Pine forest

Основная локальная сцена: `assets/scenes/editor_scene.json`.
Копия леса: `assets/scenes/pine_forest.json`.
Сохранённая сцена склада: `assets/scenes/freight_yard_scene.json`.

Лес содержит 4096 сосен трёх видов на плоском участке 352 x 352 м.
Шаг 5.5 м с детерминированным разбросом, плотность около 331 дерева/га,
seed 1327. Исходный UE-ландшафт не используется. Модели размещаются инстансами;
цвет, масштаб и поворот сцены не зависят от случайности во время запуска.

Запуск редактора: `scripts\run_editor_windows.cmd`.
Запуск игры: `scripts\run_windows.cmd`.
Режим benchmark использует авторскую камеру без автоматического персонажа:
`scripts\run_windows.cmd --benchmark --warmup 5 --duration 15 --present immediate --no-validation`.

Модели и текстуры находятся в `assets/pine_benchmark/runtime/models`.
Это локальный контент из The Pine Vegetation, он не включается в Git.
Материалы упрощены до стандартного PBR: base color, normal map,
roughness 0.85, metallic 0, alpha mask и двусторонняя хвоя.
UE wind/WPO и Two Sided Foliage subsurface не перенесены.

`scripts/export_pine_ue.py` извлекает модели из установленного UE через
`-ExecutePythonScript`. `scripts/prepare_pine_benchmark.py` подготавливает
текстуры и использует существующий импортёр нашего редактора. Для подготовки
нужен native Windows Python и Pillow (локальная установка в `.tools/pine-benchmark/python`).
Обычному запуску UE и Python не нужны.

Повторная генерация только леса: `python scripts/generate_pine_scene.py`.
Выбор леса основной сценой: `python scripts/generate_pine_scene.py --make-default`.
Исходный склад при этом сохраняется один раз и не заменяется следующими запусками.

В renderer добавлены alpha mask и double-sided shading для импортированных
моделей. Маска совпадает в цвете, тенях и full depth prepass, включая temporal
AA и MSAA. Порог хранится с точностью UNORM16. Alpha mask вместе с tessellation
отклоняется понятной ошибкой, поскольку эта комбинация пока не поддерживается.
Материалы сохраняют прежнее opaque поведение по умолчанию.

4096 деревьев остаются тяжёлой геометрической нагрузкой. Проверка загрузки и
корректности не является заявлением об оптимальной производительности леса.
