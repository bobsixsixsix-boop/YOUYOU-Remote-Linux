// A dark companion to the Android app's light device pages. The Android TV
// client also uses a right-hand sheet with quick actions above its settings.
namespace drawerTheme {
constexpr SDL_Color surface{24,35,55,255}, row{32,46,68,255}, ink{239,245,255,255},
    secondary{184,198,217,255}, line{61,78,100,255}, blue{58,123,252,255},
    paleBlue{189,225,255,255}, selected{31,64,112,255}, pressed{38,79,137,255};
constexpr SDL_Rect sheetBounds{824,32,424,624};
constexpr int closeAction=9900;

void sheet(SDL_Renderer *r) {
    rounded(r,{824,36,424,624},{5,12,24,80},22);
    rounded(r,sheetBounds,line,22);
    rounded(r,{825,33,422,622},surface,21);
}

void button(SDL_Renderer *r,const Button &b,bool focused,bool down) {
    const auto fill=!b.enabled?surface:down?pressed:(focused || b.chosen)?selected:row;
    if(focused && b.enabled){
        rounded(r,b.box,paleBlue,12);
        rounded(r,{b.box.x+2,b.box.y+2,b.box.w-4,b.box.h-4},fill,10);
    }else rounded(r,b.box,fill,12);
}

void toolbarHeader(SDL_Renderer *r,Fonts &f,bool streaming) {
    aligned(r,f.normal,"串流工具栏",{850,40,235,38},ink);
    const SDL_Rect badge{1070,47,96,26};
    rounded(r,badge,row,13);
    rounded(r,{badge.x+12,57,7,7},streaming?SDL_Color{93,214,163,255}:secondary,4);
    aligned(r,f.hint,streaming?"已连接":"本地",{badge.x+26,47,64,26},paleBlue);
    aligned(r,f.hint,"设置",{850,161,70,30},secondary);
    rect(r,898,176,326,1,line);
}

void context(SDL_Renderer *r,Fonts &f,Page page) {
    const char *caption=nullptr;
    switch(page){
    case Page::Display:caption="画质、视频尺寸与显示器";break;
    case Page::Picture:caption="视频尺寸、编码与带宽";break;
    case Page::Quality:caption="目标帧率与传输画质";break;
    case Page::Codec:caption="重新连接后协商编码格式";break;
    default:return;
    }
    aligned(r,f.hint,caption,{849,96,374,32},secondary);
}
}
