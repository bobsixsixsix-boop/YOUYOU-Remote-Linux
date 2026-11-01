/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <memory>
#include <string>
struct SDL_Renderer;
struct AVFrame;
namespace uu {
// Owns only presentation resources; the bridge controls Media::nativeOutput.
class GpuPresenter {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit GpuPresenter(std::unique_ptr<Impl> impl);
public:
    static std::unique_ptr<GpuPresenter> open(SDL_Renderer*, std::string& error);
    ~GpuPresenter();
    GpuPresenter(const GpuPresenter&)=delete;
    GpuPresenter& operator=(const GpuPresenter&)=delete;
    int upload(const AVFrame*); // 1 imported, 2 bounded backlog, 0 CPU fallback
    int draw(int x,int y,int width,int height);
    int presented();
    int reset();
    bool active() const;
    const std::string& error() const;
};
}
