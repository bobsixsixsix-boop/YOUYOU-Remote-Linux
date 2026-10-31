// Account pages use the same full-window viewport and focus model as Home.
namespace accountui {
using namespace homeui;
constexpr SDL_Color danger{166,53,48,255}, dangerLight{254,237,235,255};

std::string displayName(const std::string &nickname) {
    // Some accounts have a phone number or an email as their display name.
    int digits=0,run=0;
    bool phone=true;
    for(unsigned char c:nickname){
        if(c>='0' && c<='9'){++digits;if(++run>=7)return "UU 用户";}
        else {
            run=0;
            if(c=='@' || c<' ')return "UU 用户";
            if(c!='+' && c!='-' && c!=' ' && c!='(' && c!=')')phone=false;
        }
    }
    return nickname.empty() || (phone && digits>=7)?"UU 用户":nickname;
}

void header(SDL_Renderer *r,Fonts &f,int margin,const char *title) {
    rect(r,0,0,uiCanvasWidth,uiCanvasHeight,canvas);
    rect(r,0,0,uiCanvasWidth,102,surface);
    rect(r,0,101,uiCanvasWidth,1,line);
    aligned(r,f.brand,title,{margin+68,24,uiCanvasWidth-margin*2-160,54},primary);
    image(r,"youyou-mark",{uiCanvasWidth-margin-44,29,44,44},surface);
}

void profile(SDL_Renderer *r,Fonts &f,SDL_Rect b,const uu::Snapshot &s,int online) {
    const bool compact=b.h<240;
    const int bottom=compact?68:82,hero=b.h-bottom;
    card(r,b,surface,line,24);
    SDL_Rect tint{b.x+1,b.y+1,b.w-2,hero};
    rounded(r,tint,{226,240,255,255},23);
    rect(r,tint.x,tint.y+hero-23,tint.w,23,{226,240,255,255});
    const int d=compact?72:84, y=b.y+(hero-d)/2;
    rounded(r,{b.x+24,y,d,d},{197,223,255,255},d/2);
    rounded(r,{b.x+30,y+6,d-12,d-12},{62,139,255,255},d/2);
    image(r,"account",{b.x+24+d/4,y+d/4,d/2,d/2},surface);
    const int x=b.x+d+44, w=b.w-d-68;
    aligned(r,f.hint,"当前账号",{x,y-1,w,24},secondary);
    if(s.authenticated)chip(r,f,{b.x+b.w-94,b.y+12,76,26},"已登录",onlineBg,onlineFg);
    const auto name=displayName(s.nickname);
    int nameW=0;
    logicalTextSize(f.brand,name.c_str(),&nameW,nullptr);
    if(nameW<=w)aligned(r,f.brand,name,{x,y+25,w,d-20},primary);
    else twoLineLabel(r,f.normal,name,{x,y+25,w,d-20},primary);
    const int mid=b.x+b.w/2, foot=b.y+hero;
    rect(r,mid,foot+16,1,bottom-32,line);
    const std::string values[]={std::to_string(s.devices.size())+" 台",std::to_string(online)+" 台"};
    for(int i=0;i<2;++i){
        const int xx=b.x+24+i*(b.w/2);
        aligned(r,f.hint,i?"当前在线":"账号设备",{xx,foot+6,b.w/2-44,24},secondary);
        aligned(r,f.small,values[i],{xx,foot+28,b.w/2-44,bottom-32},primary);
    }
}

void actionRow(SDL_Renderer *r,Fonts &f,const Button &b,bool focusNow,bool down) {
    const bool logout=b.action==58;
    const auto color=logout?danger:primary;
    card(r,b.box,down?pressed:focusNow?SDL_Color{240,247,255,255}:surface,
         focusNow?focus:line,16,focusNow?2:1);
    const int x=b.box.x+18,cy=b.box.y+b.box.h/2;
    rounded(r,{x,cy-22,44,44},logout?dangerLight:lightBlue,14);
    const char *art=b.action==57?"guide":logout?"exit":"refresh";
    image(r,art,{x+9,cy-13,26,26},logout?danger:accent);
    const int tx=x+61,w=b.box.w-139;
    aligned(r,f.small,b.title,{tx,cy-29,w,33},color);
    aligned(r,f.hint,b.value,{tx,cy+6,w,24},secondary);
    image(r,"arrow",{b.box.x+b.box.w-38,cy-10,20,20},muted);
}

void about(SDL_Renderer *r,Fonts &f,SDL_Rect cardBox) {
    const int x=cardBox.x+28,y=cardBox.y,w=cardBox.w-56;
    card(r,cardBox);
    image(r,"youyou-mark",{x,y+20,64,64},surface);
    aligned(r,f.normal,"YOUYOU 远程",{x+84,y+18,w-84,40},primary);
    std::string build=UU_VERSION;
    const std::string prefix="YOUYOU-Linux-";
    if(build.rfind(prefix,0)==0)build.erase(0,prefix.size());
    aligned(r,f.hint,"Linux 客户端  ·  "+build,{x+84,y+56,w-84,28},secondary);
    rect(r,x,y+102,w,1,line);
    aligned(r,f.small,"感谢上游作者",{x,y+114,w,38},primary);
    aligned(r,f.hint,"Antman2023   ·   原始 Switch 客户端、登录与串流",{x,y+156,w,27},secondary);
    aligned(r,f.hint,"doke69   ·   YOUYOU 界面、手柄适配与画质改进",{x,y+190,w,27},secondary);
    rect(r,x,y+234,w,1,line);
    aligned(r,f.small,"许可与说明",{x,y+244,w,38},primary);
    twoLineLabel(r,f.hint,"社区维护的非官方移植，未获网易官方背书。",{x,y+284,w,43},secondary);
    twoLineLabel(r,f.hint,"代码遵循 GPL-3.0-or-later；第三方组件、资源和商标归各自权利人。",{x,y+330,w,50},secondary);
    twoLineLabel(r,f.hint,"仅连接本人或获授权的设备，并遵守相关服务条款。",{x,y+378,w,45},secondary);
}

void draw(SDL_Renderer *r,Fonts &f,Page page,const uu::Snapshot &s,int online,
          std::vector<Button> &buttons,int &focusAt,int held) {
    const bool compact=uiCanvasWidth<=720;
    const int m=compact?36:64, width=uiCanvasWidth-2*m;
    auto add=[&](SDL_Rect box,const char *name,int id,const char *description=""){
        buttons.push_back({box,name,id,true,false,description});
    };
    header(r,f,m,page==Page::About?"关于客户端":"我的账号");
    if(page==Page::Account){
        const int leftW=compact?width:(width*43/100);
        const int begin=compact?134:156+std::clamp((uiCanvasHeight-680)/3,0,80);
        if(!compact)aligned(r,f.normal,"个人资料",{m,begin,leftW,43},primary);
        profile(r,f,{m,begin+(compact?0:55),leftW,compact?173:282},s,online);
        const int x=compact?m:m+leftW+28,y=compact?begin+195:begin;
        const int rightW=compact?width:width-leftW-28;
        aligned(r,f.normal,"账号管理",{x,y,rightW,43},primary);
        add({x,y+55,rightW,78},"重新登录",52,"重新验证本机上的 UU 账号");
        add({x,y+145,rightW,78},"关于客户端",57,"版本信息与开源项目");
        add({x,y+235,rightW,78},"退出登录",58,"退出本机账号");
    }else if(page==Page::About){
        const int w=std::min(width,890),x=(uiCanvasWidth-w)/2,y=compact?132:142;
        about(r,f,{x,y,w,449});
        add({x,y+465,w,75},"重新登录",52,"重新验证本机上的 UU 账号");
    }else {
        const int w=std::min(570,width),x=(uiCanvasWidth-w)/2,y=(uiCanvasHeight-300)/2;
        card(r,{x,y,w,300},surface,line,24);
        rounded(r,{x+28,y+25,52,52},dangerLight,18);
        image(r,"exit",{x+40,y+37,28,28},danger);
        aligned(r,f.brand,"退出这台平板上的账号？",{x+28,y+91,w-56,52},primary);
        twoLineLabel(r,f.small,"再次连接时，请扫码或用短信验证码登录。",{x+28,y+150,w-56,59},secondary);
        const int half=(w-72)/2;
        add({x+28,y+224,half,52},"取消",80);
        add({x+44+half,y+224,half,52},"退出登录",770);
    }
    if(page!=Page::Logout)add({m,24,54,54},"",99);
    focusAt=std::clamp(focusAt,0,int(buttons.size())-1);
    for(int i=0;i<int(buttons.size());++i){
        const Button &b=buttons[i];
        if(b.action==99)plainButton(r,f,b,i==focusAt,i==held);
        else if(page!=Page::Logout)actionRow(r,f,b,i==focusAt,i==held);
        else {
            const bool exit=b.action==770;
            const auto fill=exit?(i==held?SDL_Color{132,43,39,255}:danger):(i==held?pressed:lightBlue);
            card(r,b.box,fill,i==focusAt?focus:fill,16,i==focusAt?3:1);
            aligned(r,f.small,b.title,b.box,exit?surface:accent,true);
        }
    }
}
} // namespace accountui
