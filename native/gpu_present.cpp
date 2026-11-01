/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gpu_present.hpp"
#include <SDL.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
}
#include <cstdint>
#include <cstdio>
#include <deque>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

namespace uu {
namespace {
void require(bool condition,const std::string& message) {
    if(!condition)throw std::runtime_error(message);
}
void sdlCheck(int result,const char* operation) {
    if(result!=0)throw std::runtime_error(std::string(operation)+": "+SDL_GetError());
}
bool extension(const char* extensions,const char* name) {
    return extensions && (std::string(" ")+extensions+" ").find(std::string(" ")+name+" ")!=std::string::npos;
}
std::string eglError(const char* operation) {
    char text[96];std::snprintf(text,sizeof(text),"%s (EGL 0x%x)",operation,eglGetError());return text;
}
struct EglApi {
    EGLDisplay display=EGL_NO_DISPLAY;
    EGLContext context=EGL_NO_CONTEXT;
    PFNEGLCREATEIMAGEKHRPROC createImage=nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage=nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bindImage=nullptr;
    PFNEGLCREATESYNCKHRPROC createSync=nullptr;
    PFNEGLDESTROYSYNCKHRPROC destroySync=nullptr;
    PFNEGLCLIENTWAITSYNCKHRPROC waitSync=nullptr;
    bool modifiers=false;
};
// This is the only owner of the retained decoder reference and exported FDs.
// Destruction is allowed only before use, after its fence, or after reset's
// one-time GPU drain. A texture/image reference alone does not pin a decoder slot.
struct Surface {
    EglApi* api=nullptr;
    AVFrame* frame=nullptr;
    SDL_Texture* texture=nullptr;
    EGLImageKHR image=EGL_NO_IMAGE_KHR;
    EGLSyncKHR fence=EGL_NO_SYNC_KHR;
    VADRMPRIMESurfaceDescriptor desc{};
    SDL_Rect source{};
    bool drawn=false, needsFence=false;
    explicit Surface(EglApi& value):api(&value) {
        for(auto& object:desc.objects)object.fd=-1;
    }
    ~Surface() {
        if(fence!=EGL_NO_SYNC_KHR)api->destroySync(api->display,fence);
        if(texture)SDL_DestroyTexture(texture);
        if(image!=EGL_NO_IMAGE_KHR)api->destroyImage(api->display,image);
        for(unsigned i=0;i<desc.num_objects&&i<4;++i)
            if(desc.objects[i].fd>=0)close(desc.objects[i].fd);
        av_frame_free(&frame);
    }
};
}

struct GpuPresenter::Impl {
    SDL_Renderer* renderer=nullptr;
    SDL_Texture* anchor=nullptr;
    EglApi api;
    std::unique_ptr<Surface> current;
    std::deque<std::unique_ptr<Surface>> retiring;
    const std::thread::id thread=std::this_thread::get_id();
    bool enabled=false;
    std::string failure;
    static constexpr size_t MaxRetiring=2;

    explicit Impl(SDL_Renderer* r):renderer(r) {}
    ~Impl() {
        // Owner guarantees SDL renderer/context still exist on this UI thread.
        if(!drain()) {
            // A broken/lost context must never return an in-use decoder slot.
            // This terminal failure retains at most three surfaces until exit;
            // normal rendering, reset and fallback reclaim every reference.
            current.release();
            for(auto& surface:retiring)surface.release();
            retiring.clear();
        }
        if(anchor)SDL_DestroyTexture(anchor);
    }
    void activate() {
        require(std::this_thread::get_id()==thread,"GPU presenter used from another thread");
        sdlCheck(SDL_RenderFlush(renderer),"GPU renderer flush");
        // SDL's bind hook activates the renderer even when its queue is empty.
        sdlCheck(SDL_GL_BindTexture(anchor,nullptr,nullptr),"GPU context activation");
        sdlCheck(SDL_GL_UnbindTexture(anchor),"GPU context activation end");
        if(api.context!=EGL_NO_CONTEXT)
            require(eglGetCurrentContext()==api.context&&eglGetCurrentDisplay()==api.display,
                    "GPU renderer EGL context changed");
    }
    bool drain() noexcept {
        if(!current&&retiring.empty())return true;
        // Resets, fallback and shutdown may block once. Steady-state upload and
        // presentation never call glFinish or wait for an unsignaled fence.
        try {activate();}catch(...) {}
        bool complete=false;
        if(eglGetCurrentContext()==api.context&&eglGetCurrentDisplay()==api.display) {
            // A failed SDL flush must not skip completion of earlier reads.
            glFinish();complete=glGetError()==GL_NO_ERROR;
        } else {
            // A fence can be queried without its creation context being current.
            // Never call glFinish on an unrelated context as completion proof.
            auto finished=[&](const std::unique_ptr<Surface>& surface) {
                return !surface||(!surface->drawn)||(!surface->needsFence&&
                    surface->fence!=EGL_NO_SYNC_KHR&&api.waitSync(api.display,surface->fence,0,0)==
                        EGL_CONDITION_SATISFIED_KHR);
            };
            complete=finished(current);
            for(const auto& surface:retiring)complete=finished(surface)&&complete;
        }
        if(!complete)return false;
        current.reset();retiring.clear();
        return true;
    }
    int fail(const std::string& reason) {
        failure=reason;enabled=false;drain();return 0;
    }
    void collect() {
        for(auto it=retiring.begin();it!=retiring.end();) {
            auto& surface=**it;
            require(surface.fence!=EGL_NO_SYNC_KHR,"GPU retired surface missing fence");
            const EGLint status=api.waitSync(api.display,surface.fence,0,0);
            if(status==EGL_CONDITION_SATISFIED_KHR)it=retiring.erase(it);
            else {
                if(status!=EGL_TIMEOUT_EXPIRED_KHR)throw std::runtime_error(eglError("GPU fence poll failed"));
                ++it;
            }
        }
    }
    void sealCurrent() {
        if(!current||!current->needsFence)return;
        // flush is needed for both normal post-present and a defensive upload
        // before the caller's presented hook. The fence covers every old read.
        sdlCheck(SDL_RenderFlush(renderer),"GPU submit before fence");
        EGLSyncKHR next=api.createSync(api.display,EGL_SYNC_FENCE_KHR,nullptr);
        if(next==EGL_NO_SYNC_KHR)throw std::runtime_error(eglError("GPU fence creation failed"));
        glFlush();
        if(current->fence!=EGL_NO_SYNC_KHR)api.destroySync(api.display,current->fence);
        current->fence=next;current->needsFence=false;
    }
    std::unique_ptr<Surface> import(const AVFrame* frame) {
        require(frame&&frame->format==AV_PIX_FMT_VAAPI,"GPU input is not a VAAPI frame");
        require(frame->width>0&&frame->height>0&&frame->width<=8192&&frame->height<=8192&&
                !(frame->width&1)&&!(frame->height&1),"GPU frame dimensions are unsupported");
        require(!frame->crop_left&&!frame->crop_top&&!frame->crop_right&&!frame->crop_bottom,
                "GPU explicit frame cropping uses CPU fallback");
        require(frame->hw_frames_ctx,"GPU frame has no hardware context");
        auto frames=reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
        require(frames->format==AV_PIX_FMT_VAAPI&&frames->sw_format==AV_PIX_FMT_NV12&&frames->device_ctx&&
                frames->device_ctx->type==AV_HWDEVICE_TYPE_VAAPI,
                "GPU import requires 8-bit VAAPI NV12");
        require(frame->color_range!=AVCOL_RANGE_JPEG,"GPU full-range NV12 uses CPU fallback");
        const bool bt709=frame->colorspace==AVCOL_SPC_BT709||
            (frame->colorspace==AVCOL_SPC_UNSPECIFIED&&frame->height>576);
        require(frame->colorspace==AVCOL_SPC_UNSPECIFIED||frame->colorspace==AVCOL_SPC_BT709||
                frame->colorspace==AVCOL_SPC_SMPTE170M||frame->colorspace==AVCOL_SPC_BT470BG,
                "GPU colorspace uses CPU fallback");
        auto va=reinterpret_cast<AVVAAPIDeviceContext*>(frames->device_ctx->hwctx);
        require(va&&va->display,"GPU VAAPI display is unavailable");
        auto result=std::make_unique<Surface>(api);
        result->frame=av_frame_clone(frame);
        require(result->frame,"GPU frame reference allocation failed");
        auto id=static_cast<VASurfaceID>(reinterpret_cast<uintptr_t>(frame->data[3]));
        VAStatus status=vaSyncSurface(va->display,id);
        require(status==VA_STATUS_SUCCESS,std::string("GPU surface synchronization: ")+vaErrorStr(status));
        status=vaExportSurfaceHandle(va->display,id,VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            VA_EXPORT_SURFACE_READ_ONLY|VA_EXPORT_SURFACE_COMPOSED_LAYERS,&result->desc);
        require(status==VA_STATUS_SUCCESS,std::string("GPU surface export: ")+vaErrorStr(status));
        const auto& desc=result->desc;
        require(desc.fourcc==VA_FOURCC_NV12&&desc.num_layers==1&&desc.num_objects>0&&desc.num_objects<=4&&
                desc.width>=static_cast<unsigned>(frame->width)&&desc.height>=static_cast<unsigned>(frame->height)&&
                desc.width<=8192&&desc.height<=8192&&!(desc.width&1)&&!(desc.height&1),
                "GPU export format or storage dimensions are unsupported");
        // The exported surface may include coded-size padding (e.g. 1088 for
        // 1080 video). Import its actual allocation geometry and sample only
        // the visible AVFrame rectangle. Pitch/offset remain driver-provided.
        result->source={0,0,frame->width,frame->height};
        const auto& layer=desc.layers[0];
        // DRM_FORMAT_NV12 is the same little-endian fourcc as VA_FOURCC_NV12.
        require(layer.drm_format==VA_FOURCC_NV12&&layer.num_planes==2,"GPU export is not composed NV12");
        std::vector<EGLint> attrs={EGL_WIDTH,static_cast<EGLint>(desc.width),EGL_HEIGHT,static_cast<EGLint>(desc.height),
            EGL_LINUX_DRM_FOURCC_EXT,static_cast<EGLint>(layer.drm_format),
            EGL_YUV_COLOR_SPACE_HINT_EXT,bt709?EGL_ITU_REC709_EXT:EGL_ITU_REC601_EXT,
            EGL_SAMPLE_RANGE_HINT_EXT,EGL_YUV_NARROW_RANGE_EXT};
        constexpr EGLint fdKeys[]={EGL_DMA_BUF_PLANE0_FD_EXT,EGL_DMA_BUF_PLANE1_FD_EXT};
        constexpr EGLint offsetKeys[]={EGL_DMA_BUF_PLANE0_OFFSET_EXT,EGL_DMA_BUF_PLANE1_OFFSET_EXT};
        constexpr EGLint pitchKeys[]={EGL_DMA_BUF_PLANE0_PITCH_EXT,EGL_DMA_BUF_PLANE1_PITCH_EXT};
        constexpr EGLint loKeys[]={EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT};
        constexpr EGLint hiKeys[]={EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT};
        for(unsigned i=0;i<2;++i) {
            require(layer.object_index[i]<desc.num_objects,"GPU export object index is invalid");
            const auto& object=desc.objects[layer.object_index[i]];
            const uint64_t rows=i?desc.height/2:desc.height;
            const uint64_t end=uint64_t(layer.offset[i])+uint64_t(layer.pitch[i])*(rows-1)+desc.width;
            require(object.fd>=0&&layer.pitch[i]>=desc.width&&end<=object.size,
                    "GPU export plane exceeds its allocation");
            require(object.drm_format_modifier!=UINT64_MAX,"GPU export modifier is unspecified");
            require(api.modifiers||object.drm_format_modifier==0,"GPU modifier import is unsupported");
            attrs.insert(attrs.end(),{fdKeys[i],object.fd,offsetKeys[i],static_cast<EGLint>(layer.offset[i]),
                pitchKeys[i],static_cast<EGLint>(layer.pitch[i])});
            if(api.modifiers)attrs.insert(attrs.end(),{loKeys[i],static_cast<EGLint>(object.drm_format_modifier),
                hiKeys[i],static_cast<EGLint>(object.drm_format_modifier>>32)});
        }
        attrs.push_back(EGL_NONE);
        result->image=api.createImage(api.display,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,nullptr,attrs.data());
        if(result->image==EGL_NO_IMAGE_KHR)throw std::runtime_error(eglError("GPU DMA-BUF import failed"));
        result->texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_EXTERNAL_OES,SDL_TEXTUREACCESS_STATIC,
                                           static_cast<int>(desc.width),static_cast<int>(desc.height));
        require(result->texture,std::string("GPU external texture: ")+SDL_GetError());
        sdlCheck(SDL_SetTextureBlendMode(result->texture,SDL_BLENDMODE_NONE),"GPU texture blend mode");
        sdlCheck(SDL_SetTextureScaleMode(result->texture,SDL_ScaleModeLinear),"GPU texture scale mode");
        sdlCheck(SDL_GL_BindTexture(result->texture,nullptr,nullptr),"GPU texture binding");
        GLenum previous=GL_NO_ERROR;
        for(unsigned i=0;i<16;++i) {previous=glGetError();if(previous==GL_NO_ERROR)break;}
        require(previous==GL_NO_ERROR,"GPU context has persistent errors");
        api.bindImage(GL_TEXTURE_EXTERNAL_OES,result->image);
        const GLenum error=glGetError();
        sdlCheck(SDL_GL_UnbindTexture(result->texture),"GPU texture binding end");
        require(error==GL_NO_ERROR,"GPU EGLImage texture binding failed");
        // Never SDL_UpdateTexture this object: its storage belongs to the decoder.
        return result;
    }
};

GpuPresenter::GpuPresenter(std::unique_ptr<Impl> impl):impl_(std::move(impl)) {}
GpuPresenter::~GpuPresenter()=default;
std::unique_ptr<GpuPresenter> GpuPresenter::open(SDL_Renderer* renderer,std::string& error) {
    try {
        require(renderer,"GPU renderer is null");
        SDL_RendererInfo info{};sdlCheck(SDL_GetRendererInfo(renderer,&info),"GPU renderer information");
        require(info.name&&std::string(info.name)=="opengles2","GPU direct presentation requires SDL GLES2");
        bool external=false;
        for(unsigned i=0;i<info.num_texture_formats;++i)external|=info.texture_formats[i]==SDL_PIXELFORMAT_EXTERNAL_OES;
        require(external,"SDL renderer does not support external OES textures");
        auto impl=std::make_unique<Impl>(renderer);
        impl->anchor=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,1,1);
        require(impl->anchor,std::string("GPU context anchor: ")+SDL_GetError());
        impl->activate();
        auto& api=impl->api;
        api.display=eglGetCurrentDisplay();api.context=eglGetCurrentContext();
        require(api.display!=EGL_NO_DISPLAY&&api.context!=EGL_NO_CONTEXT,"SDL GLES2 has no current EGL context");
        const auto eglExtensions=eglQueryString(api.display,EGL_EXTENSIONS);
        const auto glExtensions=reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        require(extension(eglExtensions,"EGL_EXT_image_dma_buf_import"),"EGL DMA-BUF import is unavailable");
        require(extension(eglExtensions,"EGL_KHR_fence_sync"),"EGL fence synchronization is unavailable");
        require(extension(glExtensions,"GL_OES_EGL_image_external"),"GLES external images are unavailable");
        api.modifiers=extension(eglExtensions,"EGL_EXT_image_dma_buf_import_modifiers");
        api.createImage=reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        api.destroyImage=reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
        api.bindImage=reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        api.createSync=reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
        api.destroySync=reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
        api.waitSync=reinterpret_cast<PFNEGLCLIENTWAITSYNCKHRPROC>(eglGetProcAddress("eglClientWaitSyncKHR"));
        require(api.createImage&&api.destroyImage&&api.bindImage&&api.createSync&&api.destroySync&&api.waitSync,
                "GPU EGL entrypoints are unavailable");
        impl->enabled=true;error.clear();
        return std::unique_ptr<GpuPresenter>(new GpuPresenter(std::move(impl)));
    }catch(const std::exception& ex) {error=ex.what();return nullptr;}
}
int GpuPresenter::upload(const AVFrame* frame) {
    auto& p=*impl_;if(!p.enabled)return 0;
    try {
        p.activate();p.collect();p.sealCurrent();
        // No waits: bounded presentation dropping leaves the codec reference
        // chain intact and keeps one visible image while the GPU catches up.
        if(p.current&&p.current->drawn&&p.retiring.size()>=Impl::MaxRetiring)return 2;
        auto next=p.import(frame);
        if(p.current&&p.current->drawn)p.retiring.push_back(std::move(p.current));
        else p.current.reset();
        p.current=std::move(next);return 1;
    }catch(const std::exception& ex) {return p.fail(ex.what());}
}
int GpuPresenter::draw(int x,int y,int width,int height) {
    auto& p=*impl_;if(!p.enabled||!p.current)return 0;
    try {
        require(width>0&&height>0,"GPU draw dimensions are invalid");
        SDL_Rect target{x,y,width,height};
        sdlCheck(SDL_RenderCopy(p.renderer,p.current->texture,&p.current->source,&target),"GPU frame draw");
        p.current->drawn=true;p.current->needsFence=true;return 1;
    }catch(const std::exception& ex) {return p.fail(ex.what());}
}
int GpuPresenter::presented() {
    auto& p=*impl_;if(!p.enabled)return 0;
    try {p.activate();p.sealCurrent();p.collect();return 1;}
    catch(const std::exception& ex) {return p.fail(ex.what());}
}
int GpuPresenter::reset() {
    if(!impl_->drain()) {
        impl_->enabled=false;impl_->failure="GPU context cannot confirm completion; retaining decoder surfaces";
    }
    return impl_->enabled?1:0;
}
bool GpuPresenter::active()const{return impl_->enabled;}
const std::string& GpuPresenter::error()const{return impl_->failure;}
}
