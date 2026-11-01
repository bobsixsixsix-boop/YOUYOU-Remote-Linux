// Linux host for the upstream SDL TV interface. Only the platform integration
// is new; the authenticated pages and their command handlers come from upstream.
#include <string>
#include "panel_scale.hpp"
namespace {
std::string assetRoot;
int uiCanvasWidth=1280, uiCanvasHeight=720;
}
#include "overlay_helpers.inc"

uu::Client &yy_ui_client(void *);
std::shared_ptr<uu::Media> yy_ui_media(void *);
namespace {
thread_local std::string uiError;
struct Ttf {
    Ttf() { if (TTF_Init() != 0) throw std::runtime_error(TTF_GetError()); }
    ~Ttf() { TTF_Quit(); }
};
struct DisplayFonts : Fonts {
    unsigned dpi = 72;
    void resizeFor(float scale) {
        const unsigned next = std::max(1L,std::lround(72.f*scale));
        if (next != dpi) {
            clearTextCache();
            for (const auto &face : {std::pair{normal,26},std::pair{small,21},
                                     std::pair{large,48},std::pair{hint,16},std::pair{brand,34}}) {
                if (TTF_SetFontSizeDPI(face.first,face.second,next,next) < 0)
                    throw std::runtime_error(TTF_GetError());
            }
            dpi=next;
        }
        textOutputScale=scale;
    }
    ~DisplayFonts() { clearTextCache(); tvui::missingArt.clear(); }
};
struct View {
    Ttf ttf;
    DisplayFonts fonts;
    uu::Client &client;
    SDL_Renderer *renderer;
    tvui::LiveFrontend ui;
    uu::Snapshot state;
    Uint32 next = 0, began = 0;
    double presentFps = -1;
    uu::StreamStatsSnapshot stats;
    Uint32 statsDue = 0;
    const bool traceFrames = SDL_GetHintBoolean("YOUYOU_STREAM_METRICS", SDL_FALSE);
    bool held = false;
    bool reportMetrics = SDL_GetHintBoolean("YOUYOU_UI_METRICS", SDL_FALSE);
    View(void *core, SDL_Renderer *r):client(yy_ui_client(core)),renderer(r),ui(client,yy_ui_media(core),r) {
        if(uu::display::vsync(r,client.snapshot().vsync?1:0)!=0)
            SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,"Cannot restore vertical sync: %s",SDL_GetError());
    }
};
struct Geometry {
    float scale;
    SDL_Rect pixels;
    int width,height;
    static bool isAppPage(const tvui::LiveFrontend &ui) {
        return ui.isOnPage(tvui::Page::Home)||ui.isOnPage(tvui::Page::Devices)||
               ui.isAccountSurface()||ui.isLocalSettingsSurface();
    }
    static Geometry forPage(SDL_Renderer *r,const tvui::LiveFrontend &ui) {
        int w,h;
        if(SDL_GetRendererOutputSize(r,&w,&h) || w<=0 || h<=0)
            throw std::runtime_error("Cannot determine SDL drawable size");
        const bool fullPage=isAppPage(ui);
        const bool narrow=w<1.08f*h;
        int width=fullPage && narrow?720:1280;
        const int minHeight=fullPage && narrow?760:720;
        // Right-side drawer artwork lives at x=824..1248. On a tall display,
        // frame this column instead of shrinking an entire 16:9 desktop around it.
        const bool narrowDrawer=narrow && ui.usesDrawer();
        float scale=std::min(w/float(narrowDrawer?540:width),h/float(minHeight));
        const int height=fullPage?std::max(minHeight,int(std::round(h/scale))):720;
        int x=narrowDrawer?int(std::round(w*.5f-1036*scale)):int(std::round((w-width*scale)*.5f));
        int y=int(std::round((h-height*scale)*.5f));
        return {scale,{x,y,int(std::round(width*scale)),int(std::round(height*scale))},width,height};
    }
};

struct Layout {
    SDL_Renderer *r; SDL_Rect old; float sx, sy;
    Layout(SDL_Renderer *renderer, DisplayFonts &fonts,const Geometry &g):r(renderer) {
        SDL_RenderGetViewport(r,&old);SDL_RenderGetScale(r,&sx,&sy);
        fonts.resizeFor(g.scale);
        uiCanvasWidth=g.width;uiCanvasHeight=g.height;
        SDL_RenderSetScale(r,1,1); SDL_RenderSetViewport(r,&g.pixels);
        SDL_RenderSetScale(r,g.scale,g.scale); SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
    }
    ~Layout(){SDL_RenderSetScale(r,1,1);SDL_RenderSetViewport(r,&old);SDL_RenderSetScale(r,sx,sy);}
};
int flags(const View &v){return (v.ui.captures()?1:0)|(v.ui.gamepadMode()?2:0)|(v.ui.active()?4:0);}
template<class F>int guard(F f){try{return f();}catch(const std::exception &e){uiError=e.what();return -1;}catch(...){uiError="SDL UI failed";return -1;}}
}
extern "C" const char *yy_ui_error(){return uiError.c_str();}
extern "C" void *yy_ui_open(void *core, SDL_Renderer *r, const char *assets){
    try {assetRoot=assets;return new View(core,r);}
    catch(const std::exception &e){uiError=e.what();return nullptr;}
}
extern "C" void yy_ui_close(void *p){delete static_cast<View*>(p);}
extern "C" void yy_ui_present_fps(void *p,double fps){
    auto &v=*static_cast<View*>(p);
    v.presentFps=fps;
    if(v.traceFrames && fps>=0)std::fprintf(stderr,"YOUYOU_STREAM_METRICS presented_fps=%.2f\n",fps);
}
extern "C" int yy_ui_disconnect(void *p){
    return guard([&]{
        auto &v=*static_cast<View*>(p);
        const auto session=v.client.snapshot();
        if(!session.streaming)return 0;
        v.client.command("release");
        v.client.command("disconnect",{{"restoreSound",v.ui.restoreRemoteSoundOnDisconnect(session)},{"deviceId",session.activeDeviceId}});
        // The worker flushes the optional remote sound request before closing
        // the data channel. Let it process this queued command before yy_close.
        const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(v.client.snapshot().streaming && std::chrono::steady_clock::now()<until)
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        if(v.client.snapshot().streaming)throw std::runtime_error("等待远程会话断开超时");
        return 0;
    });
}
extern "C" int yy_ui_tick(void *p,int action,int x,int y,const char *value){
    return guard([&]{
        auto &v=*static_cast<View*>(p);const Uint32 now=SDL_GetTicks();
        if(!v.next || Sint32(now-v.next)>=0){
            const bool was=v.state.streaming;v.state=v.client.snapshot();v.ui.sync(v.state);
            if(!was && v.state.streaming){v.began=now;v.statsDue=0;}
            v.next=now+100;
        }
        tvui::LiveInput in;
        if(action>=8 && action<=10){
            const auto g=Geometry::forPage(v.renderer,v.ui);
            x=int(std::floor((x-g.pixels.x)/g.scale));
            y=int(std::floor((y-g.pixels.y)/g.scale));
        }
        switch(action){
            case 1:in.toolbar=true;break; case 2:in.confirm=true;break; case 3:in.cancel=true;break;
            case 4:in.up=true;break;case 5:in.down=true;break;case 6:in.left=true;break;case 7:in.right=true;break;
            case 8:v.held=true;v.ui.pointer(true,x,y);break;
            case 9:if(v.held)v.ui.pointer(false,x,y);v.held=false;break;
            case 10:if(v.held)v.ui.movePointer(x,y);break;
            case 11:{SDL_Event e{};e.type=SDL_TEXTINPUT;SDL_strlcpy(e.text.text,value,sizeof e.text.text);v.ui.event(e);break;}
            case 12:{SDL_Event e{};e.type=SDL_KEYDOWN;e.key.keysym.sym=SDLK_BACKSPACE;v.ui.event(e);break;}
            case 13:{SDL_Event e{};e.type=SDL_MOUSEWHEEL;e.wheel.y=y;v.ui.event(e);break;}
            case 14:v.held=false;v.ui.cancelPointer();break;
            case 15:v.ui.controllerName=value;break;
            case 16:case 17:{
                SDL_Event e{};e.type=SDL_KEYDOWN;e.key.keysym.sym=action==17?SDLK_TAB:SDLK_0+x;
                if(v.ui.isOnPage(tvui::Page::CustomResolution))v.ui.event(e);
                break;
            }
        }
        v.ui.input(in,v.state,v.held);
        return flags(v);
    });
}
extern "C" int yy_ui_draw(void *p){
    return guard([&]{
        auto &v=*static_cast<View*>(p);
        if(v.state.streaming && !v.ui.hasStreamOverlay() && !v.reportMetrics)return flags(v);
        if(v.state.streaming && v.ui.usesDrawer()){
            SDL_SetRenderDrawBlendMode(v.renderer,SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(v.renderer,5,12,24,112);
            SDL_RenderFillRect(v.renderer,nullptr);
        }
        Layout layout(v.renderer,v.fonts,Geometry::forPage(v.renderer,v.ui));
        if(v.reportMetrics){
            int w=0,h=0,pw=0,ph=0;
            if(auto *window=SDL_RenderGetWindow(v.renderer))SDL_GetWindowSize(window,&w,&h);
            SDL_GetRendererOutputSize(v.renderer,&pw,&ph);
            SDL_RendererInfo info{};
            SDL_GetRendererInfo(v.renderer,&info);
            const char *driver=SDL_GetCurrentVideoDriver();
            std::fprintf(stderr,
                "YOUYOU_UI_METRICS backend=%s renderer=%s window=%dx%d drawable=%dx%d layout=%dx%d glyph_dpi=%u scale=%.4f\n",
                driver?driver:"?",info.name?info.name:"?",w,h,pw,ph,uiCanvasWidth,uiCanvasHeight,
                v.fonts.dpi,double(textOutputScale));
            v.reportMetrics=false;
        }
        if(!v.state.streaming){
            const auto background=Geometry::isAppPage(v.ui)?tvui::homeui::canvas:tvui::bg;
            SDL_SetRenderDrawColor(v.renderer,background.r,background.g,background.b,255);
            SDL_RenderClear(v.renderer);
        }
        const Uint32 now=SDL_GetTicks();
        if(!v.statsDue || Sint32(now-v.statsDue)>=0){
            v.stats=v.client.streamStats();
            v.statsDue=now+250;
        }
        auto &stats=v.stats;
        stats.presentFps=v.presentFps;
        if(v.ui.networkOverlay && v.state.streaming && !v.ui.active())
            compactNetworkInfo(v.renderer,v.fonts.hint,stats,(SDL_GetTicks()-v.began)/1000);
        v.ui.draw(v.renderer,v.fonts,v.state,stats);
        return flags(v);
    });
}
extern "C" int yy_preview(SDL_Renderer *renderer,const char *assets,int page){
    return guard([&]{
        assetRoot=assets;Ttf ttf;DisplayFonts fonts;
        tvui::LiveFrontend ui(renderer,page);
        Layout layout(renderer,fonts,Geometry::forPage(renderer,ui));
        if(page==0 || page==4 || page==6){
            SDL_SetRenderDrawColor(renderer,tvui::homeui::canvas.r,tvui::homeui::canvas.g,tvui::homeui::canvas.b,255);
            SDL_RenderClear(renderer);
        }
        uu::Snapshot state;state.authenticated=true;state.streaming=page!=0 && page!=4 && page!=6;state.nickname="界面预览";
        state.detail="离线界面预览 · 未连接远程电脑";
        state.devices={{{"alias","演示电脑"},{"device_id","preview"},{"status","CONNECTED"}}};
        ui.draw(renderer,fonts,state,{});clearTextCache();
        return 0;
    });
}

// Verify fit uses layout units while the CJK glyph textures use physical pixels.
static void checkTextRaster(SDL_Renderer *renderer, DisplayFonts &fonts){
    const float previous=textOutputScale;
    fonts.resizeFor(1.f);
    const auto *base=cachedText(renderer,fonts.normal,"画质与帧率",white);
    if(!base)throw std::runtime_error("Cannot cache CJK label");
    const int baseWidth=base->width;
    fonts.resizeFor(2.375f);
    if(!textCache.empty())throw std::runtime_error("Stale glyphs after display scale change");
    const auto *hd=cachedText(renderer,fonts.normal,"画质与帧率",white);
    int logical=0;
    if(!hd || hd->width<baseWidth*2 || logicalTextSize(fonts.normal,"画质与帧率",&logical,nullptr) ||
       std::abs(logical-baseWidth)>4)
        throw std::runtime_error("Incorrect high-density CJK label size");
    int budget=0,outWidth=0;
    logicalTextSize(fonts.normal,"画质…",&budget,nullptr);
    const auto shortText=fitText(fonts.normal,"画质与帧率",budget);
    if(shortText!="画质…" || logicalTextSize(fonts.normal,shortText.c_str(),&outWidth,nullptr) ||
       outWidth>budget)
        throw std::runtime_error("CJK label fitting failed");
    fonts.resizeFor(previous);
}

static void checkIconRaster(SDL_Renderer *r,DisplayFonts &fonts){
    const auto previous=textOutputScale;
    fonts.resizeFor(2.375f);
    const auto check=[&](const char *id,SDL_Rect dst,int w,int h) {
        if(!tvui::image(r,id,dst))throw std::runtime_error(std::string("Cannot draw ")+id);
        const std::string expected=assetRoot+"/ui-svg/"+id+"-"+std::to_string(w)+".rgba";
        // A legacy geometric fallback would also draw successfully. Check the
        // SVG-derived texture was actually uploaded at the selected density.
        const auto entry=std::find_if(textCache.begin(),textCache.end(),[&](const TextEntry &e){
            return e.renderer==r && e.value==expected && e.width==w && e.height==h;
        });
        if(entry==textCache.end())throw std::runtime_error(std::string("Missing SVG artwork: ")+id);
    };
    check("gamepad",{0,0,24,24},64,64);
    check("desktop-link",{0,0,138,92},384,256);
    fonts.resizeFor(previous);
}

// Run the original hit test, release-to-activate and back navigation without
// connecting a client or sending any commands.
extern "C" int yy_ui_selftest(SDL_Renderer *renderer,const char *assets){
    return guard([&]{
        assetRoot=assets;Ttf ttf;DisplayFonts fonts;
        tvui::LiveFrontend homeLayout(renderer,0);
        Layout layout(renderer,fonts,Geometry::forPage(renderer,homeLayout));
        checkTextRaster(renderer,fonts);
        checkIconRaster(renderer,fonts);
        struct Clear { ~Clear(){clearTextCache();} } clear;
        uu::Snapshot state;state.authenticated=true;
        auto tap=[&](tvui::LiveFrontend &ui,int x,int y){
            ui.draw(renderer,fonts,state,{});ui.pointer(true,x,y);ui.pointer(false,x,y);
            ui.input({},state,false);
        };
        tvui::LiveFrontend home(renderer,0);
        tap(home,790,70);
        if(!home.isOnPage(tvui::Page::Devices))throw std::runtime_error("Home to Devices failed");
        tvui::LiveInput back;back.cancel=true;home.input(back,state,false);
        tap(home,1130,70);
        if(!home.isOnPage(tvui::Page::Account))throw std::runtime_error("Home to Account failed");
        home.input(back,state,false);
        tap(home,1100,418);
        if(!home.isOnPage(tvui::Page::Display) || !home.isLocalSettingsSurface())
            throw std::runtime_error("Home settings did not open a local page");
        tap(home,860,354); // Picture in the local settings list
        tap(home,860,326); // Codec
        if(!home.isOnPage(tvui::Page::Codec))throw std::runtime_error("Offline video codec settings unavailable");
        home.input(back,state,false);
        tap(home,860,264);
        if(!home.isOnPage(tvui::Page::Resolution))throw std::runtime_error("Panel scaling unavailable");
        tvui::LiveInput down,confirm;down.down=true;confirm.confirm=true;
        // The available panel sizes depend on the actual display; advance to
        // the last row with the same navigation used by a G9.
        for(size_t i=0;i<=uu::panelScales.size();++i){home.draw(renderer,fonts,state,{});home.input(down,state,false);}
        home.input(confirm,state,false);
        if(!home.isOnPage(tvui::Page::FixedResolution))throw std::runtime_error("Fixed sizes unavailable");
        tap(home,860,456);
        if(!home.isOnPage(tvui::Page::CustomResolution))throw std::runtime_error("Custom resolution editor unavailable");
        auto editKey=[&](SDL_Keycode key){SDL_Event e{};e.type=SDL_KEYDOWN;e.key.keysym.sym=key;home.event(e);};
        for(char key:std::string("1601"))editKey(key);
        editKey(SDLK_TAB);
        for(char key:std::string("900"))editKey(key);
        tap(home,830,534);
        if(!home.isOnPage(tvui::Page::CustomResolution))throw std::runtime_error("Accepted odd video width");
        editKey(SDLK_TAB);
        // Enter 1600 on the local three-column keypad, then apply it.
        tap(home,560,316);tap(home,1090,371);tap(home,830,479);tap(home,830,479);
        tap(home,830,534);
        if(!home.isOnPage(tvui::Page::Resolution))throw std::runtime_error("Valid numeric keypad entry rejected");
        state.streaming=true;tvui::LiveFrontend toolbar(renderer,1);
        tap(toolbar,1000,207);tap(toolbar,1000,207);
        if(!toolbar.isOnPage(tvui::Page::Quality))throw std::runtime_error("Toolbar to Quality failed");
        toolbar.input(back,state,false);toolbar.input(back,state,false);
        tap(toolbar,901,102);
        if(!toolbar.isOnPage(tvui::Page::Disconnect))throw std::runtime_error("Disconnect confirmation missing");
        tap(toolbar,993,358);
        if(!toolbar.isOnPage(tvui::Page::Toolbar))throw std::runtime_error("Cancel disconnect failed");
        tap(toolbar,1000,207);tap(toolbar,1000,207);
        tap(toolbar,830,300); // Sheet padding belongs to the menu.
        toolbar.pointer(true,790,310);toolbar.movePointer(900,310);toolbar.pointer(false,790,310);
        toolbar.input({},state,false);
        if(!toolbar.isOnPage(tvui::Page::Quality))throw std::runtime_error("Drag or padding dismissed the drawer");
        tap(toolbar,420,300);
        if(toolbar.active())throw std::runtime_error("Backdrop tap did not close the whole drawer");
        tvui::LiveFrontend confirmClose(renderer,1);
        tap(confirmClose,901,102); // Disconnect confirmation must be safe to dismiss.
        tap(confirmClose,1204,58);
        if(confirmClose.active())throw std::runtime_error("Close in confirmation did not dismiss the whole drawer");
        return 0;
    });
}
