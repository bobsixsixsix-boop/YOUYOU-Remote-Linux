"""Rasterize the bundled SVG artwork for SDL, using the build host's librsvg."""
import argparse
from array import array
import ctypes as C
import os
from pathlib import Path


def bind(lib, name, result, *args):
    call = getattr(lib, name)
    call.restype, call.argtypes = result, args
    return call


ptr = C.c_void_p
cairo = C.CDLL("libcairo.so.2")
rsvg = C.CDLL("librsvg-2.so.2")
gobject = C.CDLL("libgobject-2.0.so.0")
load_svg = bind(rsvg, "rsvg_handle_new_from_file", ptr, C.c_char_p, ptr)
unref = bind(gobject, "g_object_unref", None, ptr)
surface_create = bind(cairo, "cairo_image_surface_create", ptr, C.c_int, C.c_int, C.c_int)
context_create = bind(cairo, "cairo_create", ptr, ptr)
context_destroy = bind(cairo, "cairo_destroy", None, ptr)
surface_destroy = bind(cairo, "cairo_surface_destroy", None, ptr)
flush = bind(cairo, "cairo_surface_flush", None, ptr)
stride = bind(cairo, "cairo_image_surface_get_stride", C.c_int, ptr)
data = bind(cairo, "cairo_image_surface_get_data", ptr, ptr)
render = bind(rsvg, "rsvg_handle_render_document", C.c_int, ptr, ptr, ptr, ptr)


def rasterize(handle, width, height):
    surface = surface_create(0, width, height)  # CAIRO_FORMAT_ARGB32
    cr = context_create(surface)
    try:
        viewport = (C.c_double * 4)(0, 0, width, height)
        if not render(handle, cr, viewport, None):
            raise ValueError("librsvg could not render the artwork")
        flush(surface)
        pitch = stride(surface)
        raw = C.string_at(data(surface), pitch * height)
    finally:
        context_destroy(cr)
        surface_destroy(surface)
    # Cairo uses native-endian premultiplied ARGB. SDL blends straight RGBA.
    rgba = bytearray()
    for y in range(height):
        for word in array("I", raw[y * pitch:y * pitch + width * 4]):
            alpha = word >> 24
            if alpha:
                rgb = ((word >> n) & 255 for n in (16, 8, 0))
                rgba.extend(min(255, (v * 255 + alpha // 2) // alpha) for v in rgb)
                rgba.append(alpha)
            else:
                # Retain the light edge color when SDL interpolates a white stroke.
                rgba.extend((255, 255, 255, 0))
    return rgba


def build(assets, output):
    output.mkdir(parents=True, exist_ok=True)
    icons = sorted((assets / "icons").glob("*.svg"))
    if not icons:
        raise ValueError("No SVG icons found in " + str(assets / "icons"))
    for source in [*icons, assets / "desktop-link.svg", assets / "android-home-scene.svg"]:
        sizes = [(32, 32), (64, 64), (128, 128)]
        if source.stem == "device":
            sizes.append((384, 384))
        if source.stem == "desktop-link":
            sizes = [(192, 128), (384, 256), (576, 384)]
        if source.stem == "android-home-scene":
            sizes = [(300, 210), (600, 420), (900, 630)]
        handle = load_svg(os.fsencode(source), None)
        if not handle:
            raise ValueError("Cannot open SVG: " + str(source))
        try:
            for width, height in sizes:
                rgba = rasterize(handle, width, height)
                if not any(rgba[3::4]):
                    raise ValueError("Empty SVG: " + str(source))
                (output / f"{source.stem}-{width}.rgba").write_bytes(rgba)
        finally:
            unref(handle)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("assets", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    build(args.assets, args.output)
