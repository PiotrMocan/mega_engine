# PBR materials

Scene renderer использует metallic-roughness workflow с непрозрачными
материалами. Реализация ориентирована на
[Crash Course in BRDF Implementation](https://boksajak.github.io/files/CrashCourseBRDF.pdf):

- Lambert diffuse;
- Cook-Torrance microfacet specular;
- GGX normal distribution;
- height-correlated Smith masking-shadowing;
- Schlick Fresnel, вычисляемый по half-vector;
- `F0 = mix(0.04, baseColor, metallic)`;
- diffuse energy уменьшается на Fresnel и полностью исчезает при
  `metallic = 1`.

Artist-facing roughness является perceptual roughness. Shader преобразует её в
`alpha = roughness * roughness`, затем использует `alpha^2` в GGX `D` и Smith
`G`. Минимум `0.045` не даёт сингулярного specular peak. Все material factors
дополнительно ограничиваются на CPU и GPU границах.

## Material ABI

`PbrMaterial` занимает 32 bytes:

1. `base_color` - линейный RGBA factor;
2. `metallic` - `[0, 1]`;
3. `roughness` - `[0.045, 1]`;
4. `ambient_occlusion` - `[0, 1]`, применяется только к indirect lighting;
5. один зарезервированный float.

Scene/editor instance имеет три transform lanes и `PbrMaterial`, всего
80 bytes. Million-instance benchmark сохраняет отдельный прежний 64-byte ABI и
Lambert shader, поэтому демонстрационный PBR path не добавляет ему лишние
SSBO bandwidth и fragment cost.

Editor Inspector показывает Base color, Metallic, Roughness и Ambient
occlusion для cubes/ground. Scene JSON сохраняет новый объект `pbr`; старые
version 1 сцены без него остаются совместимыми и получают безопасные defaults.

Editor asset pipeline дополнительно хранит отдельные versioned
`*.vcrmaterial.json`. Material Editor изменяет те же bounded factors, а importer
FBX/OBJ создаёт отдельный material asset для каждого материала source model.
Imported submeshes используют factors этих material assets в editor scene.
Editor model shader дополнительно читает base-color texture как sRGB и normal
map как linear data через отдельный immutable material descriptor set.

## Direct и environment lighting

Directional sun вычисляется полным BRDF и использует существующую shadow map.
Панорама не читается обратно на CPU: при поддержке linear blit для
`R8G8B8A8_SRGB` Vulkan создаёт обычную linear-blit mip chain, roughness выбирает
LOD как дешёвую low-pass аппроксимацию, а аналитический split-sum fit
аппроксимирует интегрированный environment BRDF. Если формат не поддерживает
transfer-source/blit, renderer явно сообщает warning и безопасно использует
mip 0.

Это практичный real-time IBL approximation, а не полноценная офлайн
GGX-convolution cubemap. Imported model path поддерживает base-color и normal
textures; metallic/roughness/AO textures, anisotropy, clear-coat, transmission
и multiple-scattering compensation пока не входят в этот этап.

## Demo scene

- Terrain уменьшен с `2500 x 2000` до `256 x 192` cells:
  `10 000 000 -> 98 304` triangles.
- Terrain остаётся тем же heightfield по размеру и силуэту, но теперь
  процедурно выбирает PBR base color и roughness по высоте и уклону.
- Две строки UV spheres показывают одинаковые уровни roughness
  `0.08, 0.28, 0.50, 0.72, 0.94`: первая - gold-like metals, вторая -
  blue dielectrics.
- Полная scene workload содержит `120 387` triangles и 12 renderable objects
  без учёта sky как object.

Shader source и соответствующие SPIR-V binaries обновляются вместе:

```bat
powershell -NoProfile -ExecutionPolicy Bypass ^
  -File scripts\compile_shaders_windows.ps1
scripts\build_windows.cmd Debug
scripts\build_windows.cmd Release
```
