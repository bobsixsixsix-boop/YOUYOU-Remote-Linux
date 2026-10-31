// Full-page composition for the settings opened from the local device home.
// The same Button objects drive SDL drawing, touch hit testing and G9 navigation.
namespace settingsui {
using namespace homeui;

std::string description(Page page,std::pair<int,int> panel,int field) {
    switch(page){
        case Page::Display:return "选择要调整的串流项目";
        case Page::Picture:return "调整发送到平板的画面和带宽";
        case Page::Quality:return "选择目标帧率，也可以调整画质和缓冲";
        case Page::QualityPreset:return "使用电脑端预设，或为本次配置指定码率";
        case Page::Codec:return "下次连接时协商，AV1 优先可回退到 H.265 或 H.264";
        case Page::Resolution:return "平板屏幕  "+std::to_string(panel.first)+" × "+std::to_string(panel.second)+"   ·   连接后可查看实际接收尺寸";
        case Page::FixedResolution:return "选择常用 16∶9 尺寸，或输入宽高";
        case Page::CustomResolution:return std::string("正在输入")+(field?"高度":"宽度")+"   ·   短边 180–2160，长边 320–4096";
        case Page::Bitrate:return "选择视频码率上限，单位为 Mbps";
        case Page::Playout:return "较低的缓冲延迟更小，较高的缓冲可容忍网络波动";
        case Page::HostScreens:return "连接电脑后，这里会显示可供切换的屏幕";
        case Page::VirtualDisplay:return "连接后可按串流目标，创建独立的电脑桌面";
        default:return "";
    }
}

const char *detail(int a) {
    switch(a){
        case 30:return "帧率、画质档位与播放缓冲";
        case 31:return "分辨率、视频编码与垂直同步";
        case 9040:return "查看电脑端的物理屏和虚拟屏";
        case 9041:return "使用适合平板比例的专用桌面";
        default:return "";
    }
}

struct Layout {
    const bool narrow=uiCanvasWidth==720;
    const int margin=narrow?32:58;
    const int mainX=narrow?margin:430;
    const int mainW=uiCanvasWidth-mainX-margin;
    const int headingY=narrow?162:134;
    const int listY=headingY+96;
};

void overview(SDL_Renderer *r,Fonts &f,const Layout &g,const std::string &size,
              const std::string &fps,const std::string &codec) {
    if(g.narrow){
        const SDL_Rect summary{g.margin,104,g.mainW,46};
        rounded(r,summary,lightBlue,14);
        aligned(r,f.hint,"当前配置",{g.margin+17,104,85,46},secondary);
        aligned(r,f.small,size+"   ·   "+fps+"   ·   "+codec,
                {g.margin+108,104,g.mainW-126,46},primary);
        return;
    }
    const SDL_Rect box{g.margin,138,328,350};
    card(r,box);
    rounded(r,{box.x+20,box.y+20,box.w-40,106},lightBlue,16);
    image(r,"desktop-link",{box.x+box.w-141,box.y+35,109,73});
    aligned(r,f.hint,"这台平板",{box.x+36,box.y+34,116,28},secondary);
    aligned(r,f.normal,"当前配置",{box.x+36,box.y+62,130,42},primary);
    const char *labels[]={"目标分辨率","目标帧率","编码偏好"};
    const std::string values[]={size,fps,codec};
    for(int i=0;i<3;++i){
        const int y=box.y+146+i*60;
        aligned(r,f.hint,labels[i],{box.x+26,y,96,50},secondary);
        aligned(r,f.small,values[i],{box.x+135,y,175,50},primary);
        if(i<2)rect(r,box.x+24,y+54,box.w-48,1,line);
    }
}

bool radio(Page p,int a) {
    switch(p){
        case Page::Quality:return a==840 || a==40 || a==41;
        case Page::Codec:case Page::Bitrate:case Page::Playout:return true;
        case Page::Resolution:return a!=3030;
        case Page::FixedResolution:return a!=3000;
        case Page::QualityPreset:return a!=514;
        default:return false;
    }
}

void settingRow(SDL_Renderer *r,Fonts &f,const Button &b,Page page,bool focused,bool down){
    const auto box=b.box;
    const bool root=page==Page::Display, editing=page==Page::CustomResolution;
    const bool chosen=b.chosen && b.kind!=Kind::Toggle;
    const auto bg=down?pressed:chosen?lightBlue:surface;
    if(focused)card(r,box,bg,focus,13,2);
    else if(chosen || editing)rounded(r,box,editing && !chosen?canvas:bg,12);
    const SDL_Color fg=b.enabled?primary:muted;
    if(editing){
        const bool apply=b.action==3005;
        if(apply){
            const SDL_Rect inner{box.x+3,box.y+3,box.w-6,box.h-6};
            rounded(r,inner,b.enabled?accent:line,11);
        }
        aligned(r,f.small,b.title,box,apply && b.enabled?surface:chosen?accent:fg,true);
        return;
    }
    int left=box.x+18;
    const int cy=box.y+box.h/2;
    if(root){
        const char *asset=buttonIcon(b.action,b.title);
        if(asset){
            rounded(r,{left,cy-22,44,44},lightBlue,14);
            image(r,asset,{left+10,cy-12,24,24},accent);
        }
        left+=59;
    }
    std::string value=b.value;
    if(b.action==9040 || b.action==9041)value="连接后可用";
    const bool check=radio(page,b.action);
    const int right=box.x+box.w-16;
    int mark=0;
    if(b.kind==Kind::Toggle){
        mark=55;
        homeui::card(r,{right-48,cy-14,48,28},b.chosen?accent:line,b.chosen?accent:line,14);
        rounded(r,{right-(b.chosen?25:45),cy-11,22,22},surface,11);
    }else if(check){
        mark=40;
        image(r,b.chosen?"radio":"radio_empty",{right-26,cy-13,26,26},b.chosen?accent:muted);
    }else if(opensPage(b.action)){
        mark=38;
        image(r,"arrow",{right-22,cy-11,22,22},muted);
    }
    int valueW=0;
    if(!value.empty()){
        logicalTextSize(f.small,value.c_str(),&valueW,nullptr);
        valueW=std::min(valueW+2,(right-left-mark)/2);
        aligned(r,f.small,value,{right-mark-valueW,box.y,valueW,box.h},chosen?accent:secondary);
    }
    const int textW=right-left-mark-valueW-(valueW?18:0);
    const std::string title=b.title+(page==Page::Bitrate?" Mbps":"");
    if(root){
        aligned(r,f.small,title,{left,cy-28,textW,32},fg);
        aligned(r,f.hint,detail(b.action),{left,cy+5,textW,24},secondary);
    }else aligned(r,f.small,title,{left,box.y,textW,box.h},chosen?accent:fg);
}

void draw(SDL_Renderer *r,Fonts &f,Page page,const std::string &title,
          const std::string &size,const std::string &fps,const std::string &codec,
          std::pair<int,int> panel,const std::string &error,int field,
          const std::string &feedback,bool failed,std::vector<Button> &buttons,int &focusIndex,int pressedIndex) {
    const Layout g;
    const int count=int(buttons.size())-1; // Back is appended after the page actions.
    if(count<0)return;
    rect(r,0,0,uiCanvasWidth,uiCanvasHeight,canvas);
    rect(r,0,0,uiCanvasWidth,96,surface);
    rect(r,0,95,uiCanvasWidth,1,line);
    const SDL_Rect back{g.margin,22,54,52};
    buttons.back().box=back;
    if(focusIndex<0 || focusIndex>count || !buttons[focusIndex].enabled){
        const auto it=std::find_if(buttons.begin(),buttons.end(),[](const Button &b){return b.enabled;});
        focusIndex=it==buttons.end()?count:int(it-buttons.begin());
    }
    card(r,back,pressedIndex==count?pressed:surface,focusIndex==count?focus:line,14,focusIndex==count?2:1);
    image(r,"back",{back.x+15,back.y+14,24,24},primary);
    aligned(r,f.brand,"串流设置",{back.x+75,17,230,61},primary);
    chip(r,f,{uiCanvasWidth-g.margin-112,31,112,34},"本地设置",lightBlue,accent);
    overview(r,f,g,size,fps,codec);
    const std::string heading=page==Page::Display?"设置项目":title;
    aligned(r,f.normal,heading,{g.mainX,g.headingY,g.mainW,44},primary);
    const bool invalid=page==Page::CustomResolution && !error.empty();
    aligned(r,f.hint,invalid?error:description(page,panel,field),
            {g.mainX,g.headingY+47,g.mainW,34},invalid?SDL_Color{172,50,42,255}:secondary);

    const int gap=6, inset=8;
    const bool keypad=page==Page::CustomResolution, pair=page==Page::Bitrate;
    const int rows=keypad?6:pair?(count+1)/2:count;
    const int step=page==Page::Display?84:rows>=6?54:64;
    const int height=rows*step+inset*2;
    card(r,{g.mainX,g.listY,g.mainW,height});
    for(int i=0;i<count;++i){
        auto &b=buttons[i];
        int row=i,col=0,columns=1;
        if(pair){row=i/2;col=i%2;columns=2;}
        if(keypad && i<2){row=0;col=i;columns=2;}
        else if(keypad && i<14){row=1+(i-2)/3;col=(i-2)%3;columns=3;}
        else if(keypad){row=5;}
        const int inner=g.mainW-inset*2;
        const int cell=(inner-gap*(columns-1))/columns;
        b.box={g.mainX+inset+col*(cell+gap),g.listY+inset+row*step,cell,step-gap};
        settingRow(r,f,b,page,i==focusIndex,i==pressedIndex);
        if(!keypad && !pair && i+1<count)
            rect(r,g.mainX+24,b.box.y+b.box.h+2,g.mainW-48,1,line);
    }
    if(!feedback.empty()){
        const int y=g.listY+height+9;
        twoLineLabel(r,f.hint,feedback,{g.mainX+6,y,g.mainW-12,50},
                     failed?SDL_Color{172,50,42,255}:secondary);
    }
}
} // namespace settingsui
