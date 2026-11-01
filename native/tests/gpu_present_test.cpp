// SPDX-License-Identifier: GPL-3.0-or-later
// Default: software-only fallback regression. --gpu FIXTURE.h264 explicitly
// requires a VAAPI/EGL device and a generated 192x122 or 192x128 quadrant clip.
// No login, connection, user input or window focus is needed by either mode.
#include "bridge.h"
#include "media.hpp"
#include <SDL.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Existing production UI accessor; no additional test hook in the library.
std::shared_ptr<uu::Media> yy_ui_media(void *core);

namespace {
bool forceFenceTimeout=false;
PFNEGLCLIENTWAITSYNCKHRPROC realFenceWait=nullptr;
EGLint heldFenceWait(EGLDisplay display,EGLSyncKHR fence,EGLint flags,EGLTimeKHR timeout) {
    if(forceFenceTimeout)return EGL_TIMEOUT_EXPIRED_KHR;
    return realFenceWait(display,fence,flags,timeout);
}
}
// Process-local mock only: production import, queueing and AVFrame references
// stay real. Other EGL entrypoints and all normal fence polls remain unchanged.
extern "C" __eglMustCastToProperFunctionPointerType eglGetProcAddress(const char* name) {
    const auto real=reinterpret_cast<decltype(&eglGetProcAddress)>(dlsym(RTLD_NEXT,"eglGetProcAddress"));
    if(!real)return nullptr;
    const auto result=real(name);
    if(result&&std::string(name)=="eglClientWaitSyncKHR") {
        realFenceWait=reinterpret_cast<PFNEGLCLIENTWAITSYNCKHRPROC>(result);
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(&heldFenceWait);
    }
    return result;
}

namespace {
void check(bool ok,const std::string& message) {
    if(!ok)throw std::runtime_error(message);
}
void sdl(int result,const char* operation) {
    check(result==0,std::string(operation)+": "+SDL_GetError());
}
struct TemporaryConfig {
    std::filesystem::path previous=std::filesystem::current_path(),directory;
    explicit TemporaryConfig(bool gpu) {
        char path[]="/tmp/youyou-gpu-regression-XXXXXX";
        check(mkdtemp(path)!=nullptr,"create temporary configuration");
        directory=path;
        std::filesystem::current_path(directory);
        std::ofstream("config.ini")<<"frame_pacing=1\nplayout_delay_ms=0\nfps=120\n"
            <<"width=1280\nheight=720\nvsync=0\ndecoder="<<(gpu?"hardware":"software")<<'\n';
    }
    ~TemporaryConfig() {
        std::error_code ec;
        std::filesystem::current_path(previous,ec);
        std::filesystem::remove_all(directory,ec);
    }
};
struct Resources {
    SDL_Window* window=nullptr;
    SDL_Renderer* renderer=nullptr;
    SDL_Texture* target=nullptr;
    void* core=nullptr;
    void* gpu=nullptr;
    ~Resources() {
        if(gpu)yy_gpu_close(gpu);
        if(core)yy_close(core);
        if(target)SDL_DestroyTexture(target);
        if(renderer)SDL_DestroyRenderer(renderer);
        if(window)SDL_DestroyWindow(window);
        SDL_Quit();
    }
    void reopenGpu() {
        if(gpu)yy_gpu_close(gpu);
        gpu=yy_gpu_open(core,renderer);
        check(gpu,std::string("open GPU presenter: ")+yy_error());
    }
};
struct FrameFree {void operator()(AVFrame* p)const{av_frame_free(&p);}};
using FrameRef=std::unique_ptr<AVFrame,FrameFree>;
FrameRef clone(const YYFrame& frame) {
    check(frame.format==3&&frame.size==0&&frame.data,"yy_frame did not return an opaque hardware frame");
    FrameRef result(av_frame_clone(reinterpret_cast<const AVFrame*>(frame.data)));
    check(bool(result),"retain hardware frame");
    return result;
}
std::vector<uint8_t> firstAu(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary);
    check(bool(input),"open synthetic H.264 fixture");
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});
    check(!bytes.empty()&&bytes.size()<1024*1024,"invalid synthetic fixture size");
    const size_t size=bytes.size();
    bytes.resize(size+AV_INPUT_BUFFER_PADDING_SIZE,0);
    AVCodecParserContext* parser=av_parser_init(AV_CODEC_ID_H264);
    AVCodecContext* context=avcodec_alloc_context3(nullptr);
    check(parser&&context,"allocate fixture parser");
    uint8_t* out=nullptr;int length=0;
    const int consumed=av_parser_parse2(parser,context,&out,&length,bytes.data(),int(size),0,0,0);
    std::vector<uint8_t> packet;
    if(consumed>=0&&length>0)packet.assign(out,out+length);
    av_parser_close(parser);avcodec_free_context(&context);
    check(!packet.empty(),"fixture must contain at least two complete access units");
    return packet;
}
YYFrame receive(Resources& r,const std::vector<uint8_t>& packet,uint64_t& serial,
                bool wantGpu,std::chrono::steady_clock::time_point deadline) {
    auto media=yy_ui_media(r.core);
    static uint32_t timestamp=90000;
    while(std::chrono::steady_clock::now()<deadline) {
        media->video(packet.data(),packet.size(),media->generation(),timestamp,false,true,false);
        timestamp+=750;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        YYFrame frame{};
        const int result=yy_frame(r.core,serial,&frame);
        check(result>=0,std::string("yy_frame: ")+yy_error());
        if(result==1) {
            check(frame.serial>serial,"frame identity did not advance");
            serial=frame.serial;
            if((frame.format==3)==wantGpu)return frame;
        }
    }
    throw std::runtime_error(wantGpu?"normal yy_frame never produced a GPU frame":"CPU fallback frame timed out");
}
void pixels(Resources& r,const AVFrame* frame,bool bt709) {
    const int width=frame->width,height=frame->height;
    sdl(SDL_SetRenderDrawColor(r.renderer,0,0,0,255),"clear color");
    sdl(SDL_RenderClear(r.renderer),"clear");
    check(yy_gpu_upload(r.gpu,frame)==1,std::string("import via production bridge: ")+yy_error());
    check(yy_gpu_draw(r.gpu,0,0,width,height)==1,std::string("draw: ")+yy_error());
    // Read only the synthetic offscreen target. Readback belongs to this test,
    // never to the production presenter or the user's streamed desktop.
    std::vector<uint8_t> rgba(size_t(width)*height*4);
    SDL_Rect rect{0,0,width,height};
    sdl(SDL_RenderReadPixels(r.renderer,&rect,SDL_PIXELFORMAT_RGBA32,rgba.data(),width*4),"synthetic readback");
    constexpr int yuv[4][3]={{81,90,240},{145,54,34},{41,240,110},{235,128,128}};
    for(int quadrant=0;quadrant<4;++quadrant) {
        const int x=(quadrant%2?3:1)*width/4,y=(quadrant/2?3:1)*height/4;
        const auto* actual=rgba.data()+(size_t(y)*width+x)*4;
        const double luma=(yuv[quadrant][0]-16)*255.0/219.0;
        const double u=yuv[quadrant][1]-128,v=yuv[quadrant][2]-128;
        const double values[3]={luma+(bt709?1.792741:1.596027)*v,
            luma-(bt709?0.213249:0.391762)*u-(bt709?0.532909:0.812968)*v,
            luma+(bt709?2.112402:2.017232)*u};
        for(int channel=0;channel<3;++channel) {
            const int expected=std::clamp(int(std::lround(values[channel])),0,255);
            check(std::abs(int(actual[channel])-expected)<=14,
                  "NV12 quadrant / color-matrix / padding mismatch at quadrant "+std::to_string(quadrant));
        }
        check(actual[3]>=250,"GPU image unexpectedly transparent");
    }
    SDL_RenderPresent(r.renderer);
    check(yy_gpu_presented(r.gpu)==1,std::string("seal presentation fence: ")+yy_error());
}
}

int main(int argc,char** argv) {
    try {
        const bool gpu=argc==3&&std::string(argv[1])=="--gpu";
        check(argc==1||gpu,"usage: gpu_present_test [--gpu synthetic.h264]");
        std::vector<uint8_t> packet;
        if(gpu)packet=firstAu(std::filesystem::absolute(argv[2]));
        else {
            const uint8_t red[]={
#include "../test_frame.inc"
            };
            packet.assign(std::begin(red),std::end(red));
        }
        TemporaryConfig temporary(gpu);
        setenv("SDL_AUDIODRIVER","dummy",1);
        setenv("UU_DECODER",gpu?"hardware":"software",1);
        if(!gpu)setenv("SDL_VIDEODRIVER","dummy",1);
        Resources r;
        sdl(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO),"SDL init");
        r.window=SDL_CreateWindow("YOUYOU synthetic GPU regression",0,0,192,128,SDL_WINDOW_HIDDEN);
        check(r.window,"hidden test window");
        int driver=-1;
        if(gpu)for(int i=0;i<SDL_GetNumRenderDrivers();++i) {
            SDL_RendererInfo info{};
            if(SDL_GetRenderDriverInfo(i,&info)==0&&info.name&&std::string(info.name)=="opengles2")driver=i;
        }
        if(gpu)check(driver>=0,"SDL GLES2 renderer is unavailable");
        r.renderer=SDL_CreateRenderer(r.window,driver,gpu?SDL_RENDERER_ACCELERATED|SDL_RENDERER_TARGETTEXTURE:SDL_RENDERER_SOFTWARE);
        check(r.renderer,std::string("create renderer: ")+SDL_GetError());
        r.core=yy_open("aaaaaaaaaaaaaaaaaaaaaaaa","11111111-2222-3333-4444-555555555555");
        check(r.core,std::string("create isolated core: ")+yy_error());
        auto media=yy_ui_media(r.core);
        media->setTargetFps(120);media->setPlayoutDelay(0);
        uint64_t serial=0;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(gpu?4:3);
        if(!gpu) {
            check(!yy_gpu_open(r.core,r.renderer),"software renderer unexpectedly enabled native output");
            const auto frame=receive(r,packet,serial,false,deadline);
            check(frame.data&&frame.size&&frame.format>=0&&frame.format<=2,"software fallback lost pixel storage");
            check(media->stats().nativePublished==0,"failed GPU open enabled native publication");
            std::puts("PASS software SDL rejection and normal yy_frame CPU publication");
            return 0;
        }
        r.target=SDL_CreateTexture(r.renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_TARGET,192,128);
        check(r.target,"create offscreen validation target");
        sdl(SDL_SetRenderTarget(r.renderer,r.target),"select offscreen target");
        r.reopenGpu();
        auto frame=clone(receive(r,packet,serial,true,deadline));
        check(frame->width==192&&(frame->height==122||frame->height==128),"unexpected synthetic dimensions");
        check(media->stats().cpuTransfers==0,"native yy_frame downloaded pixels before GPU import");
        SDL_Event wakes[4]{};
        check(SDL_PeepEvents(wakes,4,SDL_PEEKEVENT,SDL_USEREVENT,SDL_LASTEVENT)==1,
              "production GPU open did not produce one coalesced frame-ready hint");
        const Uint32 wakeType=wakes[0].type;
        check(wakes[0].user.windowID==SDL_GetWindowID(r.window)&&!wakes[0].user.code&&
              !wakes[0].user.data1&&!wakes[0].user.data2,"GPU frame-ready hint has an unsafe payload");
        // Same exact decoded pixels, independent matrix hints.
        frame->color_range=AVCOL_RANGE_MPEG;frame->colorspace=AVCOL_SPC_SMPTE170M;
        pixels(r,frame.get(),false);
        frame->colorspace=AVCOL_SPC_BT709;
        pixels(r,frame.get(),true);
        // Force delayed fences only in this process. Three retained surfaces
        // are the bounded limit; a fourth upload must neither import nor clone.
        check(yy_gpu_reset(r.gpu)==1,"reset before bounded-backlog test failed");
        forceFenceTimeout=true;
        for(int i=0;i<3;++i) {
            check(yy_gpu_upload(r.gpu,frame.get())==1,"bounded queue rejected a permitted surface");
            check(yy_gpu_draw(r.gpu,0,0,frame->width,frame->height)==1,"bounded queue draw failed");
            SDL_RenderPresent(r.renderer);
            check(yy_gpu_presented(r.gpu)==1,"bounded queue fence failed");
        }
        check(frame->buf[0],"VAAPI frame has no reference-counted surface");
        const int busyReferences=av_buffer_get_ref_count(frame->buf[0]);
        check(yy_gpu_upload(r.gpu,frame.get())==2,"unsignaled GPU backlog was not bounded");
        check(av_buffer_get_ref_count(frame->buf[0])==busyReferences,"busy upload retained another decoder surface");
        forceFenceTimeout=false;
        // A disconnect may clear Media and the bridge's borrowed picture while
        // the renderer still owns the frame. Its independent AVBuffer reference
        // must remain pinned until the renderer drains its outstanding reads.
        media->reset();
        YYFrame empty{};
        check(yy_frame(r.core,serial,&empty)==0,"reset left an old frame visible to the bridge");
        const int references=av_buffer_get_ref_count(frame->buf[0]);
        check(references>=2,"GPU import did not retain its own decoder reference");
        // Renderer reset releases decoder references but allows a new upload.
        check(yy_gpu_reset(r.gpu)==1,"active reset failed");
        check(av_buffer_get_ref_count(frame->buf[0])<references,"GPU reset did not release retained decoder references");
        check(yy_gpu_draw(r.gpu,0,0,192,128)==0,"reset retained an old GPU picture");
        pixels(r,frame.get(),true);
        // Full-range must fall back, never use a limited-range GPU conversion.
        frame->color_range=AVCOL_RANGE_JPEG;
        check(yy_gpu_upload(r.gpu,frame.get())==0,"full-range NV12 did not use CPU fallback");
        check(!SDL_HasEvent(wakeType),"GPU fallback left its frame-ready registration queued");
        check(yy_gpu_draw(r.gpu,0,0,192,128)==0,"failed import kept drawing the old GPU frame");
        const auto cpu=receive(r,packet,serial,false,deadline);
        check(cpu.data&&cpu.size&&media->stats().cpuTransfers>0,"failed import did not restore CPU downloads");
        check(yy_gpu_reset(r.gpu)==0,"reset unexpectedly reactivated failed GPU presenter");
        // Reopening is an explicit new presenter, then unsupported crop rejects.
        r.reopenGpu();
        frame->color_range=AVCOL_RANGE_MPEG;frame->crop_top=2;
        check(yy_gpu_upload(r.gpu,frame.get())==0,"nonzero crop was silently displayed uncropped");
        check(media->stats().decodeErrors==0,"synthetic stream introduced decoder reference errors");
        check(std::chrono::steady_clock::now()<deadline,"offscreen regression exceeded four-second work budget");
        std::printf("PASS yy_frame VAAPI, NV12 601/709, %dx%d visible padding, reset, full-range/crop fallback\n",
                    frame->width,frame->height);
        return 0;
    }catch(const std::exception& error) {
        std::fprintf(stderr,"%s\n",error.what());return 1;
    }
}
