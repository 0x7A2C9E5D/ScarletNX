/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/splash.h"
#include <lunasvg.h>
#include <plutovg.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

struct SnxSplashDrawing {
    std::unique_ptr<lunasvg::Document> document;
};

SnxSplashDrawing* snx_splash_open(const SnxSplashSvg* svg) {
    if (!svg) return nullptr;
    std::unique_ptr<lunasvg::Document> document = lunasvg::Document::loadFromData(svg->text, svg->size);
    if (!document || document->width() <= 0 || document->height() <= 0) return nullptr;
    return new (std::nothrow) SnxSplashDrawing{std::move(document)};
}

int snx_splash_draw(const SnxSplashDrawing* drawing, int side, void* pixels, int stride) {
    if (!drawing || side <= 0 || !pixels || stride < side * 4) return 1;
    const lunasvg::Document& document = *drawing->document;
    lunasvg::Bitmap bitmap(static_cast<uint8_t*>(pixels), side, side, stride);
    bitmap.clear(0);
    /* as renderToBitmap scales it, into our own pixels */
    document.render(bitmap, lunasvg::Matrix(side / document.width(), 0, 0, side / document.height(), 0, 0));
    return 0;
}

void snx_splash_close(SnxSplashDrawing* drawing) { delete drawing; }

int snx_splash_jpeg(const char* name, int side, int quality, SnxSplashWrite write, void* closure) {
    const SnxSplashSvg* svg = nullptr;
    for (int i = 0; name && i < snx_splash_svg_count; i++)
        if (!std::strcmp(snx_splash_svgs[i].name, name)) svg = &snx_splash_svgs[i];
    std::unique_ptr<SnxSplashDrawing> drawing(snx_splash_open(svg));
    if (!drawing || side <= 0 || !write) return 1;
    plutovg_surface_t* surface = plutovg_surface_create(side, side);
    if (!surface) return 1;
    /* a JPEG has no alpha: over white, as a picture with none is shown */
    const plutovg_color_t white = {1, 1, 1, 1};
    plutovg_surface_clear(surface, &white);
    const lunasvg::Document& document = *drawing->document;
    lunasvg::Bitmap bitmap(plutovg_surface_get_data(surface), side, side, plutovg_surface_get_stride(surface));
    document.render(bitmap, lunasvg::Matrix(side / document.width(), 0, 0, side / document.height(), 0, 0));
    const bool written = plutovg_surface_write_to_jpg_stream(surface, write, closure, quality);
    plutovg_surface_destroy(surface);
    return written ? 0 : 1;
}
