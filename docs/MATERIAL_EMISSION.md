# Material emission and scene exposure

Cooked `.vcrmaterial.json` files can define independent emitted radiance:

```json
{
  "pbr": {
    "emissive": [24.0, 8.0, 0.5]
  },
  "textures": {
    "emissive": "textures/neon.vcrtexture.json"
  }
}
```

The three factors are linear HDR values in `[0, 65504]`, with zero as the
default. Emissive textures use sRGB decoding. Their linear RGB is multiplied by
the factor and added to surface radiance before exposure and tone mapping.
Without a texture, the factor emits a constant color. A failed texture load
disables emission for that material instead of illuminating its whole surface.
Emission is independent of albedo, metalness and roughness. This does not
inject emissive surfaces into voxel GI or implement bloom.

The glTF importer preserves emissive color, maps and
`KHR_materials_emissive_strength`. Existing materials without emission retain
their previous appearance. Material records occupy bounded, append-only aligned
uniform-buffer slots; published records remain immutable until GPU retirement.

Scenes can save the existing exposure settings in an optional `exposure` block:

```json
{
  "exposure": {
    "mode": "fixed",
    "compensation_ev": 4.830075
  },
  "lighting": {
    "sky_intensity": 3.0
  }
}
```

The editor saves exposure changes with the scene. Runtime and editor use the
saved block unless an explicit `--exposure*` argument overrides it. Missing
blocks retain the normal defaults. `lighting.sky_intensity` defaults to `1`,
accepts finite values in `[0, 64]`, and scales sky illumination and reflections
without changing the visible sky. It is available as **Sky light intensity**
in the lighting controls.

For MiamiClub, UE's manual physical camera uses ISO 100, f/4, 1/60 s and +15 EV
compensation. Its exposure multiplier is
`2^15 / (1.2 * 4^2 * 60) = 28.444444`, equivalent to native fixed compensation
`log2(28.444444) = 4.830075 EV`. The source SkyLight intensity is `3`.
The native procedural sky is an approximation of UE's captured sky; this
conversion does not reproduce Lumen, captured indoor reflections or area lights.
