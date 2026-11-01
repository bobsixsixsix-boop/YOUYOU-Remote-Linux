// Light device surfaces follow the Android client's Brand and divider colors.
// Home and Devices leave the video tool drawer in its separate dark theme.
const char *buttonIcon(int action,const std::string &title);
namespace homeui {
constexpr SDL_Color canvas{248,248,253,255}, surface{255,255,255,255}, primary{14,25,45,255},
    secondary{76,93,113,255}, muted{87,104,124,255}, line{225,232,241,255},
    accent{11,102,221,255}, focus{58,123,252,255}, lightBlue{222,240,255,255},
    pressed{204,228,255,255}, onlineBg{218,246,235,255}, onlineFg{19,111,82,255};

void card(SDL_Renderer *r,SDL_Rect b,SDL_Color fill=surface,SDL_Color border=line,int radius=20,int edge=1) {
    rounded(r,b,border,radius);
    rounded(r,{b.x+edge,b.y+edge,b.w-edge*2,b.h-edge*2},fill,std::max(0,radius-edge));
}

void chip(SDL_Renderer *r,Fonts &f,SDL_Rect b,const std::string &s,SDL_Color fill,SDL_Color textColor) {
    rounded(r,b,fill,b.h/2);
    aligned(r,f.hint,s,b,textColor,true);
}

void background(SDL_Renderer *r){
    rect(r,0,0,uiCanvasWidth,uiCanvasHeight,canvas);
    const int h=uiCanvasWidth==720?168:118;
    rect(r,0,0,uiCanvasWidth,h,surface);
    rect(r,0,h,uiCanvasWidth,1,line);
}

void brandHeader(SDL_Renderer *r,Fonts &f,int left,int y) {
    image(r,"youyou-mark",{left,y,52,52},{255,255,255,255});
    const int nameX=left+68;
    aligned(r,f.brand,"UU 远程",{nameX,y,166,52},primary);
    int nameW=0;
    logicalTextSize(f.brand,"UU 远程",&nameW,nullptr);
    chip(r,f,{nameX+nameW+14,y+12,72,28},"Linux",{240,244,247,255},secondary);
}

void scene(SDL_Renderer *r,SDL_Rect dst) {
    const float px=dst.w*textOutputScale;
    const int w=px<=300?300:px<=600?600:900;
    SDL_Texture *tex=assetTexture(r,assetRoot+"/ui-svg/android-home-scene-"+std::to_string(w)+".rgba",w,w*7/10);
    if(tex)SDL_RenderCopy(r,tex,nullptr,&dst);
    else image(r,"desktop-link",dst);
}

void status(SDL_Renderer *r,Fonts &f,int x,int y,const std::string &s,bool online) {
    const SDL_Rect b{x,y,88,34};
    rounded(r,b,online?onlineBg:SDL_Color{232,237,243,255},17);
    rounded(r,{x+12,y+13,8,8},online?onlineFg:muted,4);
    aligned(r,f.hint,s,{x+26,y,55,34},online?onlineFg:secondary,true);
}

void deviceCard(SDL_Renderer *r,Fonts &f,const Button &b,bool active,bool isPressed,bool hasDevice) {
    const auto box=b.box;
    const int edge=active && b.enabled?3:1;
    card(r,box,isPressed?pressed:surface,edge==3?focus:line,22,edge);
    const int inset=8, footerH=76, body=box.h-footerH;
    const SDL_Rect art{box.x+inset,box.y+inset,box.w-2*inset,body-inset};
    rounded(r,art,isPressed?pressed:SDL_Color{231,242,255,255},16);
    const int artW=std::min(art.w*45/100,(art.h-12)*10/7);
    scene(r,{art.x+art.w-artW-12,art.y+(art.h-artW*7/10)/2,artW,artW*7/10});
    const int tx=box.x+30,twidth=art.w-artW-48;
    const int contentH=std::min(art.h-10,146), top=art.y+(art.h-contentH)/2;
    if(art.h>=156)aligned(r,f.hint,"当前设备",{tx,top,twidth,26},secondary);
    const int titleY=top+(art.h>=156?34:0);
    const int titleH=std::max(42,std::min(70,art.h-70));
    twoLineLabel(r,f.normal,b.value,{tx,titleY,twidth,titleH},primary);
    if(hasDevice)status(r,f,tx,titleY+titleH+5,b.chosen?"在线":"离线",b.chosen);
    const int fy=box.y+body;
    if(!hasDevice)aligned(r,f.hint,"在电脑上登录同一账号后刷新",{tx,fy,box.w-215,footerH},secondary);
    else aligned(r,f.hint,b.chosen?"远程桌面":"请在电脑上打开 UU 远程",{tx,fy,box.w-215,footerH},secondary);
    SDL_Rect enter{box.x+box.w-178,fy+(footerH-48)/2,152,48};
    rounded(r,enter,b.enabled?accent:SDL_Color{224,230,238,255},24);
    aligned(r,f.small,"进入桌面",{enter.x+8,enter.y,104,enter.h},b.enabled?surface:muted,true);
    image(r,"arrow",{enter.x+118,enter.y+15,18,18},b.enabled?surface:muted);
}

void sectionHeading(SDL_Renderer *r,Fonts &f,int x,int y,const std::string &name) {
    aligned(r,f.normal,name,{x,y,280,42},primary);
}

void preferences(SDL_Renderer *r,Fonts &f,SDL_Rect b,const uu::Snapshot &s) {
    card(r,b);
    aligned(r,f.normal,"串流偏好",{b.x+24,b.y+12,b.w-48,40},primary);
    const char *labels[]={"目标分辨率","目标帧率","视频编码"};
    const std::string values[]={
        std::to_string(s.width)+"×"+std::to_string(s.height),
        std::to_string(s.fps)+" FPS",s.av1?"AV1 优先":s.hevc?"H.265":"H.264"
    };
    const int col=(b.w-48)/3;
    for(int i=0;i<3;++i){
        const int x=b.x+24+i*col;
        aligned(r,f.hint,labels[i],{x,b.y+68,col-4,26},secondary);
        aligned(r,f.small,values[i],{x,b.y+99,col-4,32},primary);
    }
    rect(r,b.x+24,b.y+b.h-62,b.w-48,1,line);
}

void shortcuts(SDL_Renderer *r,Fonts &f,SDL_Rect b) {
    card(r,b);
    aligned(r,f.small,"快捷操作",{b.x+24,b.y+10,b.w-48,32},primary);
    auto row=[&](int y,const std::string &key,const char *value){
        SDL_Rect cap{b.x+24,y,118,28};
        rounded(r,cap,{240,244,247,255},8);
        aligned(r,f.hint,key,cap,secondary,true);
        aligned(r,f.hint,value,{cap.x+132,y,b.w-186,28},secondary);
    };
    row(b.y+47,"触屏","从左侧或右侧向内滑，打开工具栏");
    row(b.y+79,"Select + Start","手柄工具栏");
    row(b.y+111,"F8  /  F11","工具栏  /  全屏");
}

template<class Add>
void home(SDL_Renderer *r,Fonts &f,Add add,const uu::Snapshot &s,const std::string &name,
          bool hasDevice,bool online,bool canConnect) {
    const bool narrow=uiCanvasWidth==720;
    const int left=narrow?36:64;
    brandHeader(r,f,left,narrow?25:42);
    if(narrow){
        add(left,100,236,50,"切换设备",10);add(290,100,180,50,"刷新",90);add(488,100,196,50,"账号",11);
    }else{
        add(736,46,176,52,"切换设备",10);add(928,46,128,52,"刷新",90);add(1072,46,144,52,"账号",11);
    }
    const int head=narrow?181:144;
    sectionHeading(r,f,left,head,"我的设备");
    chip(r,f,{left+126,head+7,78,28},std::to_string(s.devices.size())+" 台",{231,237,245,255},secondary);
    const int cardY=narrow?238:208;
    const int cardH=narrow?std::clamp(uiCanvasHeight-536,218,338):338;
    const int cardW=narrow?648:652;
    add(left,cardY,cardW,cardH,"进入桌面",12,canConnect,online,hasDevice?name:"尚未发现电脑",Kind::Tile);
    const SDL_Rect pref=narrow?SDL_Rect{left,cardY+cardH+16,648,202}:SDL_Rect{740,cardY,476,240};
    preferences(r,f,pref,s);
    add(pref.x+12,pref.y+pref.h-55,pref.w-24,48,"串流设置",21);
    const SDL_Rect keys{pref.x,pref.y+pref.h+16,pref.w,154};
    if(!narrow || keys.y+keys.h+62<uiCanvasHeight)shortcuts(r,f,keys);
    if(!narrow)aligned(r,f.hint,"方向键选择     A 确认     B 返回",{left,cardY+cardH+18,cardW,36},secondary);
}

void plainButton(SDL_Renderer *r,Fonts &f,const Button &b,bool active,bool isPressed) {
    const SDL_Color fg=active?accent:secondary;
    const bool pref=b.action==21;
    card(r,b.box,isPressed?pressed:active?lightBlue:surface,active?focus:pref?surface:line,
         pref?12:b.box.h/2,active?2:1);
    const char *asset=b.action==21?"picture":buttonIcon(b.action,b.title);
    if(pref){
        image(r,asset,{b.box.x+12,b.box.y+12,24,24},accent);
        aligned(r,f.small,b.title,{b.box.x+50,b.box.y,b.box.w-100,b.box.h},active?accent:primary);
        image(r,"arrow",{b.box.x+b.box.w-35,b.box.y+15,18,18},fg);return;
    }
    int tw=0;
    if(!b.title.empty())logicalTextSize(f.small,b.title.c_str(),&tw,nullptr);
    const int iconW=b.title.empty()?22:24, gap=asset && tw?10:0;
    const int start=b.box.x+(b.box.w-tw-(asset?iconW:0)-gap)/2;
    if(asset)image(r,asset,{start,b.box.y+(b.box.h-iconW)/2,iconW,iconW},fg);
    if(tw)aligned(r,f.small,b.title,{start+(asset?iconW+gap:0),b.box.y,tw+1,b.box.h},active?accent:primary);
}

void deviceRow(SDL_Renderer *r,Fonts &f,const Button &b,bool active,bool isPressed,bool current,bool online) {
    const auto box=b.box;
    card(r,box,isPressed?pressed:surface,active&&b.enabled?focus:line,15,active&&b.enabled?2:1);
    const int cy=box.y+box.h/2;
    rounded(r,{box.x+16,cy-22,44,44},online?lightBlue:canvas,13);
    image(r,"device",{box.x+25,cy-13,26,26},online?accent:muted);
    const int statusX=box.x+box.w-134;
    int titleEnd=statusX-16;
    if(current){
        titleEnd-=84;
        chip(r,f,{titleEnd,cy-14,76,28},"当前设备",{232,239,249,255},secondary);
    }
    twoLineLabel(r,f.small,b.title,{box.x+78,box.y+5,titleEnd-box.x-90,box.h-10},online?primary:secondary);
    status(r,f,statusX,cy-17,b.value,online);
    if(b.enabled)image(r,"arrow",{box.x+box.w-33,cy-9,18,18},muted);
}
} // namespace homeui
