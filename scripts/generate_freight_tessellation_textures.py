"""Generate a coherent tileable PBR set for the Freight Yard tessellation demo.

Run with Blender so the texture pipeline has one reproducible implementation:

    blender.exe --background --python scripts/generate_freight_tessellation_textures.py -- \
        --source art/freight_yard/textures/tessellated_concrete/concrete_height_source_imagegen.png \
        --output art/freight_yard/textures/tessellated_concrete

The generated height and tangent-space normal maps come from the same periodic
scalar field. A periodic-plus-smooth decomposition removes boundary jumps
without the obvious repeated symmetry of a mirrored tile.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

import bpy
import numpy as np


def parse_arguments() -> argparse.Namespace:
    arguments = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--size", type=int, default=2048)
    parser.add_argument("--tile-meters", type=float, default=4.0)
    parser.add_argument("--displacement-scale", type=float, default=0.12)
    return parser.parse_args(arguments)


def periodic_gaussian(field: np.ndarray, sigma_pixels: float) -> np.ndarray:
    height, width = field.shape
    frequencies_y = np.fft.fftfreq(height)[:, None]
    frequencies_x = np.fft.rfftfreq(width)[None, :]
    kernel = np.exp(
        -2.0
        * math.pi
        * math.pi
        * sigma_pixels
        * sigma_pixels
        * (frequencies_x * frequencies_x + frequencies_y * frequencies_y)
    )
    return np.fft.irfft2(
        np.fft.rfft2(field) * kernel,
        s=field.shape,
    ).astype(np.float32)


def periodic_component(source: np.ndarray) -> np.ndarray:
    """Return the non-symmetric periodic component of a scalar image.

    This is the periodic-plus-smooth decomposition by Moisan. The smooth
    component carries the opposite-edge mismatch; subtracting it leaves a
    field whose wrapped finite differences are continuous.
    """
    height, width = source.shape
    boundary = np.zeros_like(source, dtype=np.float32)
    boundary[0, :] = source[-1, :] - source[0, :]
    boundary[-1, :] = source[0, :] - source[-1, :]
    boundary[:, 0] += source[:, -1] - source[:, 0]
    boundary[:, -1] += source[:, 0] - source[:, -1]

    frequency_y = np.arange(height, dtype=np.float64)[:, None]
    frequency_x = np.arange(width, dtype=np.float64)[None, :]
    denominator = (
        2.0 * np.cos(2.0 * math.pi * frequency_x / float(width))
        + 2.0 * np.cos(2.0 * math.pi * frequency_y / float(height))
        - 4.0
    )
    denominator[0, 0] = 1.0
    smooth_spectrum = np.fft.fft2(boundary) / denominator
    smooth_spectrum[0, 0] = 0.0
    smooth = np.fft.ifft2(smooth_spectrum).real.astype(np.float32)
    return (source - smooth).astype(np.float32)


def save_rgba(path: Path, rgba: np.ndarray) -> None:
    height, width, channels = rgba.shape
    if channels != 4:
        raise ValueError("RGBA output must have four channels.")
    image = bpy.data.images.new(
        path.stem,
        width=width,
        height=height,
        alpha=True,
        float_buffer=False,
    )
    image.colorspace_settings.name = "Non-Color"
    image.alpha_mode = "STRAIGHT"
    image.pixels.foreach_set(
        np.ascontiguousarray(rgba, dtype=np.float32).reshape(-1)
    )
    image.filepath_raw = str(path.resolve())
    image.file_format = "PNG"
    image.save()
    bpy.data.images.remove(image)


def main() -> None:
    options = parse_arguments()
    if options.size < 256 or options.size > 8192 or options.size % 2 != 0:
        raise ValueError("--size must be an even value from 256 through 8192.")
    if not math.isfinite(options.tile_meters) or options.tile_meters <= 0.0:
        raise ValueError("--tile-meters must be finite and positive.")
    if (
        not math.isfinite(options.displacement_scale)
        or options.displacement_scale <= 0.0
    ):
        raise ValueError("--displacement-scale must be finite and positive.")

    source_path = options.source.resolve()
    if not source_path.is_file():
        raise FileNotFoundError(source_path)
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=True)

    source_image = bpy.data.images.load(str(source_path), check_existing=False)
    source_image.colorspace_settings.name = "Non-Color"
    source_image.scale(options.size, options.size)
    pixels = np.empty(options.size * options.size * 4, dtype=np.float32)
    source_image.pixels.foreach_get(pixels)
    pixels = pixels.reshape((options.size, options.size, 4))
    luminance = (
        pixels[:, :, 0] * 0.2126
        + pixels[:, :, 1] * 0.7152
        + pixels[:, :, 2] * 0.0722
    )
    bpy.data.images.remove(source_image)

    periodic_source = periodic_component(luminance)
    # Remove photographic illumination and retain only interpretable relief.
    smooth = periodic_gaussian(periodic_source, 1.25)
    broad = periodic_gaussian(periodic_source, 96.0)
    relief = smooth - broad
    median = float(np.median(relief))
    negative = max(median - float(np.percentile(relief, 1.0)), 1.0e-6)
    positive = max(float(np.percentile(relief, 99.0)) - median, 1.0e-6)
    normalized = np.where(
        relief < median,
        (relief - median) / negative,
        (relief - median) / positive,
    )
    normalized = np.clip(normalized, -1.0, 1.0).astype(np.float32)
    # Intact concrete stays at the 0.5 midlevel. Cracks/pits have more room
    # below the surface than aggregate has above it.
    height = np.where(
        normalized < 0.0,
        0.5 + normalized * 0.24,
        0.5 + normalized * 0.16,
    ).astype(np.float32)

    normal_height = periodic_gaussian(height, 1.0)
    texel_meters = options.tile_meters / float(options.size)
    slope_x = (
        np.roll(normal_height, -1, axis=1)
        - np.roll(normal_height, 1, axis=1)
    ) * (options.displacement_scale / (2.0 * texel_meters))
    slope_y = (
        np.roll(normal_height, -1, axis=0)
        - np.roll(normal_height, 1, axis=0)
    ) * (options.displacement_scale / (2.0 * texel_meters))
    tangent_normal = np.stack(
        (-slope_x, -slope_y, np.ones_like(height)),
        axis=2,
    )
    tangent_normal /= np.maximum(
        np.linalg.norm(tangent_normal, axis=2, keepdims=True),
        1.0e-8,
    )
    normal_rgb = tangent_normal * 0.5 + 0.5

    concrete = np.array([0.47, 0.445, 0.405], dtype=np.float32)
    brightness = 0.94 + normalized[:, :, None] * 0.12
    dirt = np.clip(-normalized, 0.0, 1.0)[:, :, None]
    base_color = np.clip(
        concrete[None, None, :] * brightness
        + dirt * np.array([0.010, -0.005, -0.018], dtype=np.float32),
        0.05,
        0.92,
    )
    roughness = np.clip(
        0.82 + np.abs(normalized) * 0.10 + dirt[:, :, 0] * 0.04,
        0.72,
        0.98,
    )

    alpha = np.ones_like(height)
    height_rgba = np.stack((height, height, height, alpha), axis=2)
    normal_rgba = np.concatenate((normal_rgb, alpha[:, :, None]), axis=2)
    base_rgba = np.concatenate((base_color, alpha[:, :, None]), axis=2)
    metallic_roughness = np.stack(
        (np.ones_like(height), roughness, np.zeros_like(height), alpha),
        axis=2,
    )

    seam_delta = max(
        float(np.mean(np.abs(height[:, 0] - height[:, -1]))),
        float(np.mean(np.abs(height[0, :] - height[-1, :]))),
    )
    interior_delta = max(
        float(np.mean(np.abs(np.diff(height, axis=1)))),
        float(np.mean(np.abs(np.diff(height, axis=0)))),
        1.0e-8,
    )
    seam_ratio = seam_delta / interior_delta
    if seam_ratio > 1.05:
        raise RuntimeError(
            "Periodic height seam exceeds ordinary neighboring-texel variation: "
            f"ratio={seam_ratio:.4f}"
        )

    save_rgba(output / "tess_concrete_height.png", height_rgba)
    save_rgba(output / "tess_concrete_normal.png", normal_rgba)
    save_rgba(output / "tess_concrete_base_color.png", base_rgba)
    save_rgba(
        output / "tess_concrete_metallic_roughness.png",
        metallic_roughness,
    )
    print(
        "Generated tileable concrete PBR set: "
        f"height=[{height.min():.4f}, {height.max():.4f}], "
        f"size={options.size}, tile={options.tile_meters:.3f}m, "
        f"seam_ratio={seam_ratio:.4f}"
    )


if __name__ == "__main__":
    main()
