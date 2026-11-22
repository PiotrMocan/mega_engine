# Pine forest

Проект находится в `../projects/pine_forest/`. Пути `assets/` ниже отсчитываются
от этой папки. Основная сцена находится в `assets/scenes/editor_scene.json`,
копия леса — в `assets/scenes/pine_forest.json`.

Лес содержит 4096 сосен трёх видов на плоском участке 352 x 352 м.
Шаг 5.5 м с детерминированным разбросом, плотность около 331 дерева/га,
seed 1327. Исходный UE-ландшафт не используется. Модели размещаются инстансами;
цвет, масштаб и поворот сцены не зависят от случайности во время запуска.

Запуск редактора из корня движка — `scripts\run_editor_windows.cmd --project ..\projects\pine_forest`.
Запуск игры — `..\projects\pine_forest\pine_forest.exe`.
Режим benchmark использует авторскую камеру без автоматического персонажа:
`scripts\run_windows.cmd --project ..\projects\pine_forest --benchmark --warmup 5 --duration 15 --present immediate --no-validation`.

Модели и текстуры находятся в `assets/pine_benchmark/runtime/models`.
Это локальный контент из The Pine Vegetation, он не включается в Git.
Материалы упрощены до стандартного PBR: base color, normal map,
roughness 0.85, metallic 0, alpha mask и двусторонняя хвоя.
UE wind/WPO и Two Sided Foliage subsurface не перенесены.

Скрипты импорта находятся в `../projects/pine_forest/tools/`.
В Unreal-проекте с пакетом `/Game/0_The_Pine_Vegetation` запустите
`export_pine_ue.py` через `-ExecutePythonScript`, затем выполните
`prepare_pine_benchmark.py` в Windows Python с Pillow. Для обоих процессов
задайте одну переменную `VCR_IMPORT_WORK` с путём к новой папке в `%TEMP%`.
После проверки моделей удалите эту папку. Игра читает готовые меши и текстуры
из `assets/pine_benchmark/runtime/models/`.

Повторная генерация только леса — `python ../projects/pine_forest/tools/generate_pine_scene.py`.
Для выбора леса основной сценой добавьте `--make-default`.
Исходный склад при этом сохраняется один раз и не заменяется следующими запусками.

В renderer добавлены alpha mask и double-sided shading для импортированных
моделей. Маска совпадает в цвете, тенях и full depth prepass, включая temporal
AA и MSAA. Порог хранится с точностью UNORM16. Alpha mask вместе с tessellation
отклоняется понятной ошибкой, поскольку эта комбинация пока не поддерживается.
Материалы сохраняют прежнее opaque поведение по умолчанию.

4096 деревьев остаются тяжёлой геометрической нагрузкой. Проверка загрузки и
корректности не является заявлением об оптимальной производительности леса.
