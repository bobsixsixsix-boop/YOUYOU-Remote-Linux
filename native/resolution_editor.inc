// Members of the native menu. Every key is a normal SDL menu button, so its
// touch hitbox, keyboard focus and the G9 d-pad agree in both orientations.
int resolutionWidth=1280;
std::string dimensions[2], dimensionError;
int activeDimension=0;
bool replaceDimension=true;

using Dimensions=std::pair<int,int>;
Dimensions panelPixels{},lastCustomSize{};
std::array<Dimensions,uu::panelScales.size()> scaleTargets{};
bool focusSavedScale=false;
static std::string pixels(Dimensions size) {
    return std::to_string(size.first)+"×"+std::to_string(size.second);
}
Dimensions displayPixels() const {
    int w=0,h=0;
    SDL_GetRendererOutputSize(renderer,&w,&h);
    if(auto *window=SDL_RenderGetWindow(renderer)){
        // The fullscreen drawable is already in native, rotation-corrected
        // pixels. A smaller floating window must still refer to its monitor.
        if(!(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN)){
            SDL_DisplayMode mode{};
            const int display=SDL_GetWindowDisplayIndex(window);
            if(display>=0 && SDL_GetDesktopDisplayMode(display,&mode)==0 && mode.w>0 && mode.h>0)
                return {mode.w,mode.h};
        }
    }
    return {w,h};
}
void refreshScaleTargets() {
    const auto size=displayPixels();
    if(size==panelPixels)return;
    panelPixels=size;
    // For example 720p and 100% coincide on a 720p display. In that case
    // the percentage category carries the native entry and its single mark.
    for(int i=int(scaleTargets.size())-1;i>=0;--i){
        const auto target=uu::panelVideoSize(size.first,size.second,uu::panelScales[i]);
        const auto next=scaleTargets.begin()+i+1;
        scaleTargets[i]=std::find(next,scaleTargets.end(),target)==scaleTargets.end()?target:Dimensions{};
    }
}
void openScalePage() {
    push(Page::Resolution);
    focusSavedScale=true;
}
void applyDimensions(Dimensions size,const uu::Snapshot &s) {
    if(s.qualityPending || !uu::streamSize(size.first,size.second))return;
    const Dimensions old{resolutionWidth,resolution};
    // Migrate the sole custom width/height kept by v0.1.5 before a new
    // selection replaces them. Old standard choices remain standard choices.
    if(!lastCustomSize.first && uu::streamSize(old.first,old.second) &&
       old!=Dimensions{960,540} && old!=Dimensions{1280,720} && old!=Dimensions{1920,1080} &&
       std::find(scaleTargets.begin(),scaleTargets.end(),old)==scaleTargets.end()){
        lastCustomSize=old;if(client)save();
    }
    resolutionWidth=size.first;resolution=size.second;applyQuality(s);
}
template<class Add> void drawScalePage(SDL_Renderer *r,Fonts &f,Add add,const uu::Snapshot &s,
                                         const uu::StreamStatsSnapshot &stats) {
    refreshScaleTargets();
    const Dimensions selected{resolutionWidth,resolution};
    if(!isLocalSettingsSurface())
        aligned(r,f.hint,panelPixels.first?"本机屏幕　"+pixels(panelPixels):"尚未取得屏幕像素",{848,97,376,31},label);
    int y=146;
    int savedRow=-1;
    for(size_t i=0;i<scaleTargets.size();++i){
        const auto target=scaleTargets[i];
        if(!target.first)continue;
        if(target==selected)savedRow=int(buttons.size());
        add(848,y,376,46,uu::panelScales[i].label,3040+int(i),!s.qualityPending,target==selected,pixels(target),Kind::Menu);
        y+=50;
    }
    // The previous page's buttons still exist during the first input tick.
    // Set the initial G9 focus only after constructing this page's rows.
    if(focusSavedScale){stack.back().focus=savedRow>=0?savedRow:int(buttons.size());focusSavedScale=false;}
    add(848,y,376,46,"固定 / 自定义",3030,true,savedRow<0,savedRow<0?"已选":"",Kind::Menu);
    if(isLocalSettingsSurface())return;
    const bool hasVideo=s.streaming && stats.width>0 && stats.height>0;
    const std::string received=hasVideo?pixels({stats.width,stats.height}):s.streaming?"等待视频帧":"连接后显示";
    rounded(r,{848,501,376,78},drawerTheme::row,11);
    rect(r,1035,515,1,49,drawerTheme::line);
    aligned(r,f.hint,"串流目标",{864,506,157,26},drawerTheme::secondary);
    aligned(r,f.hint,"实际收到",{1052,506,156,26},drawerTheme::secondary);
    aligned(r,f.small,pixels(selected),{864,535,158,34},drawerTheme::ink);
    aligned(r,f.small,received,{1052,535,156,34},drawerTheme::paleBlue);
    const auto *screen=s.displays?s.displays->find(s.displays->current):nullptr;
    if(s.streaming && screen){
        const std::string source=screen->type==0?"电脑物理屏":screen->type==1?"电脑虚拟屏":"电脑屏幕";
        aligned(r,f.hint,source+"　"+pixels({screen->width,screen->height}),{849,583,375,25},drawerTheme::secondary);
    }
    if(s.qualityPending)
        aligned(r,f.hint,"正在等待电脑应用设置…",{849,610,375,26},drawerTheme::secondary);
    else if(hasVideo && Dimensions{stats.width,stats.height}!=selected)
        aligned(r,f.hint,"电脑当前按可用尺寸发送画面",{849,610,375,26},drawerTheme::secondary);
}
std::string videoSize() const {
    return std::to_string(resolutionWidth)+" × "+std::to_string(resolution);
}
void selectDimension(int index) {
    activeDimension=index;
    replaceDimension=true;
    dimensionError.clear();
}
void dimensionKey(SDL_Keycode key) {
    if(key==SDLK_TAB){
        selectDimension(1-activeDimension);
        stack.back().focus=activeDimension;
        return;
    }
    auto &field=dimensions[activeDimension];
    if(key==SDLK_BACKSPACE){
        if(replaceDimension)field.clear();
        else if(!field.empty())field.pop_back();
    }else if(key>=SDLK_0 && key<=SDLK_9){
        if(replaceDimension)field.clear();
        if(field.size()<5)field+=char('0'+key-SDLK_0);
    }else return;
    replaceDimension=false;
    dimensionError=field.size()>4?"每项最多四位数":"";
}
void openResolutionEditor() {
    const auto initial=lastCustomSize.first?lastCustomSize:Dimensions{resolutionWidth,resolution};
    dimensions[0]=std::to_string(initial.first);
    dimensions[1]=std::to_string(initial.second);
    selectDimension(0);
    push(Page::CustomResolution);
}
void resolutionAction(int action,const uu::Snapshot &s) {
    if(action>=3010 && action<=3019)dimensionKey(SDLK_0+action-3010);
    else if(action==3001 || action==3002)selectDimension(action-3001);
    else if(action==3003)dimensionKey(SDLK_BACKSPACE);
    else if(action==3004)dimensionKey(SDLK_TAB);
    else if(action==3005){
        const int w=dimensions[0].empty()?0:std::stoi(dimensions[0]);
        const int h=dimensions[1].empty()?0:std::stoi(dimensions[1]);
        if(dimensions[0].size()>4 || dimensions[1].size()>4 || !uu::streamSize(w,h)){
            dimensionError="宽高须为偶数，范围见下方";
            return;
        }
        lastCustomSize={w,h};if(client)save();
        applyDimensions(lastCustomSize,s);back();back();
    }
}
template<class Add> void drawResolutionEditor(SDL_Renderer *r,Fonts &f,Add add,bool pending) {
    for(int i=0;i<2;++i)
        add(848+i*194,154,182,52,std::string(i?"高  ":"宽  ")+(dimensions[i].empty()?"—":dimensions[i]),
            3001+i,true,activeDimension==i,"",Kind::Tab);
    static const char *keys[]={"1","2","3","4","5","6","7","8","9","退格","0","切换"};
    static constexpr int actions[]={3011,3012,3013,3014,3015,3016,3017,3018,3019,3003,3010,3004};
    for(int i=0;i<12;++i)add(848+(i%3)*128,222+(i/3)*58,120,50,keys[i],actions[i]);
    add(848,465,376,54,"应用视频尺寸",3005,!pending);
    if(isLocalSettingsSurface())return;
    const std::string status=dimensionError.empty()?std::string("正在输入")+(activeDimension?"高度":"宽度")+"　·　Tab 切换":dimensionError;
    aligned(r,f.small,status,{848,528,376,37},dimensionError.empty()?label:SDL_Color{255,189,151,255});
    aligned(r,f.hint,"短边 180–2160，长边 320–4096",{848,572,376,33},label);
}
