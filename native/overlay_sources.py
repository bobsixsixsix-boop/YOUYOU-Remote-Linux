"""Extract the pinned upstream drawing code without its Switch entry point."""
from pathlib import Path
import re
import sys
sys.dont_write_bytecode = True
from custom_resolution_ui import adapt as custom_resolution_ui

source, out = map(Path, sys.argv[1:])
def read(name):
    return (source / name).read_text(encoding="utf-8")

helpers = read("main.cpp").split('#include "tv_frontend.inc"')[0]
helpers = helpers.replace('#include "qrcodegen.hpp"', '')
start = helpers.index('const std::string root=std::ifstream("romfs/fonts/')
end = helpers.index(';', start) + 1
helpers = helpers[:start] + 'const std::string root=assetRoot + "/fonts/";' + helpers[end:]
tv = read("tv_frontend.inc").split('bool preview(')[0] + '\n}\n'
tv = tv[:tv.index("bool image(")] + '#include "tablet_icons.inc"\n' + tv[tv.index("void centered("):]
tv = tv.replace('{x,y,height,height},64,64);', '{x,y,height,height});')
tv = tv.replace('case 13:case 23:', 'case 23:return "peripherals";case 13:')
tv = tv.replace('case 14:case 21:', 'case 14:return "menu";case 21:')
tv = tv.replace('case 57:return "account"', 'case 57:return "guide"')
tv = tv.replace('case 70:return "device"', 'case 70:return "more"')
tv = tv.replace('case 30:return "fps"', '''
    case 44:case 3000:return "resolution";
    case 757:return "privacy";
    case 9040:case 9060:case 9061:case 9062:case 9063:return "screens";
    case 9041:case 9042:return "virtual-display";
    case 30:return "fps"''')
tv = tv.replace('rect(r,0,0,1280,720,bg);','rect(r,0,0,uiCanvasWidth,uiCanvasHeight,bg);')
tv = tv.replace('Nintendo Switch · 开源远程桌面客户端', 'Linux · 开源远程桌面客户端')
tablet_pages = Path(__file__).with_name("tablet_pages.inc").read_text(encoding="utf-8")
tv = tv[:tv.index("void homeHotkeys(")] + tablet_pages + "\n" + tv[tv.index("bool opensPage("):]
tv = tv.replace('const std::string version="v" UU_VERSION;', 'const std::string version="Linux";')
live = read("live_frontend.inc")
# Preview is read-only and can render without constructing an account, media
# backend, network worker, or config store. Only draw() is used in this mode.
live = live.replace('uu::Client &client;', 'uu::Client *client;')
live = live.replace('client(c),', 'client(&c),').replace('client.', 'client->')
live = live.replace('    ~LiveFrontend()', '''
    LiveFrontend(SDL_Renderer *r,int previewPage):client(nullptr),renderer(r) {
        constexpr Page previews[]={Page::Home,Page::Toolbar,Page::More,Page::Quality,
                                   Page::Devices,Page::Gamepad,Page::Account,Page::QualityPreset};
        push(previews[std::clamp(previewPage,0,7)]);
        device="演示电脑";
        if(previewPage==5)controllerName="GameSir Legion Gaming Controller G9 2026";
    }
    std::string controllerName;
    void cancelPointer(){touchTarget=hit=-1;touchDown=false;}
    bool isOnPage(Page page)const{return active() && stack.back().page==page;}
    bool hasStreamOverlay()const{return active() || networkOverlay || Sint32(modeNoticeUntil-SDL_GetTicks())>0;}
    ~LiveFrontend()''')
# Hardware clocks and Switch-only controller emulation are not Linux features.
live = live.replace('const bool unsupported=a==750;', 'const bool unsupported=a==750 || a==50 || a==59 || a==856 || a==613;')
live = live.replace('touchMode=1,inputMode=0,','touchMode=1,inputMode=1,')
live = live.replace('row(0,"键鼠映射",600,true', 'row(0,"仅键鼠 / 触屏",600,true')
live = live.replace('串流中长按减号键', '可接入 SDL 识别的外接手柄').replace('在手柄和键鼠操作之间切换', '键鼠和触控在两种模式下均可使用')
# Use the persisted multitouch flag, not touch_mode (which upstream always saves as 1).
live = live.replace('if(page==Page::Touch && (a==610 || a==611)){touchMode=1;nativeMultitouch=false;client->command("release");resetTouchZoom=true;draining=true;save();return;}',
                    '''if(page==Page::Touch && (a==610 || a==614)){
            const bool native=a==614;
            if(native!=nativeMultitouch){
                if(client)client->command("release");
                nativeMultitouch=native;
                draining=true;
                if(client)save();
            }
            return;
        }''')
live = live.replace('case 614:nativeMultitouch=true;client->command("release");resetTouchZoom=true;draining=true;save();break;', '')
touch = live.index('        case Page::Touch:title=')
end = live.index('        case Page::Mouse:title=', touch)
live = live[:touch] + '''        case Page::Touch:title="触屏操作";
            row(0,"原生多点触控",614,true,nativeMultitouch);
            row(1,"桌面鼠标",610,true,!nativeMultitouch);
            twoLineLabel(r,f.small,nativeMultitouch?"将手指触点直接发送给电脑，支持双指操作。":"手指落在哪里，指针就到哪里。按住拖动窗口，抬手松开左键。",
                         {848,360,376,92},label);
            break;
''' + live[end:]
live = live.replace('nativeMultitouch?"平板多点触控":"手机鼠标指针"', 'nativeMultitouch?"原生多点触控":"桌面鼠标"')
live = live.replace('SDL_StopTextInput();keyboardEditRequested=true;', 'SDL_StartTextInput();')
live = live.replace('长按 ＋', '关闭窗口').replace('返回启动器', '退出 Linux 客户端')
live = live.replace('"Nintendo Switch"', '"Linux / Y700"').replace('键鼠映射模式','仅键鼠 / 触屏').replace('inputMode?"手柄直连":"键鼠映射"','inputMode?"手柄直连":"仅键鼠 / 触屏"')
live = live.replace('text(r,f.normal,"掌机内置手柄",160,264,ink);',
                    'aligned(r,f.small,controllerName.empty()?"尚未识别到手柄":controllerName,{154,252,425,70},ink);')
# The upstream hit-testing and focus model consume the same button rectangles
# we draw. Set those at layout time rather than moving the pixels afterwards.
home = live.index("        case Page::Home: {")
next_page = live.index("        case Page::StreamPreview:", home)
live = live[:home] + '''        case Page::Home: {
            const bool hasDevice=selectedDevice>=0 && selectedDevice<int(snapshot.devices.size());
            const auto *selected=hasDevice?&snapshot.devices[selectedDevice]:nullptr;
            homeui::home(r,f,add,snapshot,device,hasDevice,selected && deviceState(*selected)==DeviceState::Online,
                       selected && snapshot.stage=="idle" && connectableDevice(*selected));
            break;
        }
''' + live[next_page:]
devices = live.index("        case Page::Devices:",home)
end = live.index("        case Page::Toolbar:",devices)
live = live[:devices] + '''        case Page::Devices: {
            const int x=uiCanvasWidth==720?36:64, width=uiCanvasWidth-2*x;
            aligned(r,f.brand,"我的设备",{122,43,uiCanvasWidth-188,58},homeui::primary);
            const auto online=std::count_if(snapshot.devices.begin(),snapshot.devices.end(),
                [&](const auto &d){return deviceState(d)==DeviceState::Online;});
            const int start=uiCanvasWidth==720?212:166;
            aligned(r,f.hint,"全部设备   "+std::to_string(snapshot.devices.size())+" 台   ·   在线 "+
                    std::to_string(online)+" 台",{x,start-48,width,38},homeui::secondary);
            for(size_t i=size_t(deviceOffset);i<std::min(snapshot.devices.size(),size_t(deviceOffset+5));++i){
                const auto &d=snapshot.devices[i];
                add(x,start+(int(i)-deviceOffset)*85,width,72,d.value("alias",std::string("电脑")),
                    1000+int(i),connectableDevice(d),int(i)==selectedDevice,deviceStateText(deviceState(d)),Kind::Menu);
            }
            if(snapshot.devices.empty()){
                homeui::card(r,{x,start,width,168});
                image(r,"device",{x+(width-36)/2,start+18,36,36},homeui::accent);
                aligned(r,f.normal,"尚未发现电脑",{x,start+57,width,42},homeui::primary,true);
                aligned(r,f.small,"请在电脑上登录同一账号，再返回首页刷新",{x+10,start+103,width-20,44},homeui::secondary,true);
            }
            break;
        }
''' + live[end:]
account = live.index("        case Page::Account:",home)
end = live.index("        case Page::Login:",account)
live = live[:account] + '        case Page::Account: break;\n' + live[end:]
start = live.index("        case Page::Logout:",account)
end = live.index("        case Page::Keyboard:",start)
live = live[:start] + '        case Page::Logout: case Page::About: break;\n' + live[end:]
live = live.replace("if(!snapshot.streaming || !drawer)backdrop(r);",
                    "if(page==Page::Home || page==Page::Devices)homeui::background(r); else if(!snapshot.streaming || !drawer)backdrop(r);")
tile = live.index("            if(b.kind==Kind::Tile)")
end = live.index("if(b.kind==Kind::Shortcut)",tile)
live = live[:tile] + "            " + live[end:]
button = live.index("            const auto fill=",live.index("        if(frame.focus<0",live.index("        if(!title.empty())")))
live = live[:button] + '''            if(page==Page::Home || page==Page::Devices){
                const bool pressed=touchDown && touchTarget==int(i);
                if(b.kind==Kind::Tile)
                    homeui::deviceCard(r,f,b,focused,pressed,!snapshot.devices.empty());
                else if(page==Page::Devices && b.action>=1000)
                    homeui::deviceRow(r,f,b,focused,pressed,b.chosen,b.value=="在线");
                else homeui::plainButton(r,f,b,focused,pressed);
                continue;
            }
''' + live[button:]
notice = live.index('        if(!saveError.empty())aligned')
live = live[:notice]+live[notice:].replace('if(!saveError.empty())aligned',
                  'const bool lightPage=page==Page::Home || page==Page::Devices;\n        if(!saveError.empty())aligned',1).replace(
                  'SDL_Color{255,170,140,255}', 'lightPage?SDL_Color{160,56,36,255}:SDL_Color{255,170,140,255}').replace(
                  '32},label);','32},lightPage?homeui::secondary:label);').replace(
                  'SDL_Color{149,191,255,255}', 'lightPage?homeui::accent:SDL_Color{149,191,255,255}')

live = live.replace('vw=std::min(vw,140);', '''
                int titleWidth=0;TTF_SizeUTF8(f.small,b.title.c_str(),&titleWidth,nullptr);
                const int available=b.box.w-left-right-16;
                vw=std::min(vw,std::max(0,available-std::min(titleWidth,available/2)));''')
live = live.replace('focused?blue:b.chosen?', 'focused?SDL_Color{42,98,216,255}:b.chosen?')
live = live.replace('b.enabled?(focused?ink:label)', 'b.enabled?(focused?ink:SDL_Color{232,234,238,255})')
live = live.replace('const char *asset=buttonIcon(b.action,b.title);', 'const char *asset=b.action==48 && !sound?"mute":buttonIcon(b.action,b.title);')
live = re.sub(r'(image\(r,(?:asset|"arrow"|b.chosen\?"radio":"radio_empty"),\{[^}]+\})\);', r'\1,tint);',live)
live = live.replace('b.box.w-right-vw,b.box.y,vw,b.box.h},tint);',
                    'b.box.w-right-vw,b.box.y,vw,b.box.h},(b.enabled && !focused)?label:tint);')
live = live.replace('aligned(r,f.small,b.title,{b.box.x+left,b.box.y,b.box.w-left-right-(vw?vw+16:0),b.box.h},tint);',
                    'twoLineLabel(r,f.small,b.title,{b.box.x+left,b.box.y+3,b.box.w-left-right-(vw?vw+16:0),b.box.h-6},tint);')
# Keep the entire hardware name and decoder/encoder descriptions readable if
# the actual device returns longer strings than upstream's short sample data.
live = live.replace('aligned(r,f.small,controllerName.empty()?', 'twoLineLabel(r,f.small,controllerName.empty()?')
live = live.replace('aligned(r,f.small,codecValues[i],{x,538,340,34},ink);',
                    'twoLineLabel(r,f.small,codecValues[i],{x,534,340,46},ink);')
live = live.replace('高清及以上建议 CPU 超频','画质档位由电脑端应用').replace('提升流畅度','自定义可设置码率上限')
live = live.replace('xboxLayout?"按字母：Switch A/B/X/Y 对应 Xbox 同名字母":"按位置：Switch B/A/Y/X 对应 Xbox A/B/X/Y"',
                    '"使用系统识别的标准游戏手柄布局"')
live = live.replace('仅影响手柄直连；菜单仍 A 确认、B 返回','菜单内用 A 确认、B 返回')
live = live.replace('已切回键鼠映射','已切换为键鼠与触屏输入')
# Keep the configured preference distinct from the codec actually delivering frames.
live = live.replace('fps=60,resolution=720,codec=0','fps=120,resolution=720,codec=2')
live = live.replace('bool vsync=true,sound=true', 'bool vsync=false,sound=true')
live = live.replace('row(2,"垂直同步",46,true,false,vsync?"开启":"关闭");',
                    'add(848,328,376,64,"垂直同步",46,true,vsync,"",Kind::Toggle);')
live = live.replace('codec=s.hevc;', 'codec=s.av1?2:s.hevc?1:0;')
live = live.replace('{"hevc",bool(codec)}','{"hevc",codec==1},{"av1",codec==2}')
live = live.replace('if(uu::display::vsync(renderer,vsync?1:0)!=0)', 'if(vsync!=s.vsync && uu::display::vsync(renderer,vsync?1:0)!=0)')
live = live.replace('case 40:fps=60;', 'case 840:fps=120;applyQuality(s);break;case 40:fps=60;')
live = live.replace('row(0,"60 帧",40,true,fps==60);row(1,"30 帧",41,true,fps==30);',
                    'row(0,"120 帧",840,true,fps==120);row(1,"60 帧",40,true,fps==60);row(2,"30 帧",41,true,fps==30);')
live = live.replace('row(2,"画质档位"', 'row(3,"画质档位"').replace('row(3,"播放缓冲"', 'row(4,"播放缓冲"')
live = live.replace('row(4,"播放缓冲",47,true,false,std::to_string(bufferMs)+" ms");break;', '''
            row(4,"播放缓冲",47,true,false,std::to_string(bufferMs)+" ms");
            if(snapshot.streaming && stats.presentFps>=0)
                aligned(r,f.small,"当前显示　"+number(stats.presentFps)+" FPS",{848,562,376,48},label);
            break;''')
live = live.replace('(a==40||a==41||a==46', '(a==840||a==40||a==41||a==46')
live = live.replace('(b.action==40 || b.action==41)', '(b.action==840 || b.action==40 || b.action==41)')
live = live.replace('codec?"H.265":"H.264"', 'codec==2?"AV1 优先":codec==1?"H.265":"H.264"')
live = live.replace('row(0,"H.264",500,true,codec==0);row(1,"H.265",501,true,codec==1);break;', '''
            row(0,"AV1 优先",502,true,codec==2);
            row(1,"H.265",501,true,codec==1);
            row(2,"H.264",500,true,codec==0);
            aligned(r,f.small,"兼容编码按 H.265、H.264 顺序选择",{848,416,376,40},label);
            if(snapshot.streaming){
                aligned(r,f.hint,"本次解码",{848,480,376,28},label);
                twoLineLabel(r,f.small,stats.codec.empty() || stats.codec=="—"?"等待视频帧":stats.codec,{848,512,376,64},ink);
            }
            break;''')
# Keep essential notifications in the visible canvas on portrait Home/Devices.
live = live.replace('{65,638,710,32}', '{uiCanvasWidth==720?36:65,uiCanvasWidth==720?uiCanvasHeight-54:638,uiCanvasWidth==720?648:710,32}')
guide = live.index('case Page::Guide:title="操作说明"')
end = live.index('break;',guide)
live = live[:guide]+'''case Page::Guide:title="操作说明";
    aligned(r,f.normal,"Linux 平板",{90,155,1100,56},ink);
    aligned(r,f.small,"触屏　从左侧或右侧向内滑打开工具栏。点面板外或右上角 × 收起",{90,255,1100,48},label);
    aligned(r,f.small,"键盘　F8 打开菜单，F11 全屏，方向键选择，回车确认，Esc 返回",{90,340,1100,48},label);
    aligned(r,f.small,"手柄　选择＋开始打开菜单，方向键或左摇杆选择，A 确认，B 返回",{90,425,1100,48},label);
    '''+live[end:]
# The pinned upstream measures all text in 720p pixels. Keep its layout units
# and hit boxes, but use Linux's drawable-sized font metrics and glyph cache.
tv, live = custom_resolution_ui(tv, live)
helpers = '#include "stream_size.hpp"\n' + helpers
helpers = helpers.replace('TTF_SizeUTF8(', 'logicalTextSize(')
helpers = helpers.replace('TTF_FontHeight(font)+2','int(std::ceil(TTF_FontHeight(font)/textOutputScale))+2')
helpers = helpers.replace('const TextEntry *cachedText(', '#include "tablet_text.inc"\nconst TextEntry *cachedText(')
start = helpers.index('    const auto *entry=cachedText(r,f,s,c);')
end = helpers.index('\n}',start)
helpers = helpers[:start] + '''    const auto *entry=cachedText(r,f,fitText(f,s,maxWidth),c);
    if(entry)copyGlyphs(r,*entry,float(x),float(y));''' + helpers[end:]
helpers = helpers.replace('8u*1024u*1024u','textureCacheBudget()')
tv = tv.replace('8u*1024u*1024u','textureCacheBudget()')
tv = tv.replace('TTF_SizeUTF8(', 'logicalTextSize(')
live = live.replace('TTF_SizeUTF8(', 'logicalTextSize(')
start = tv.index('    int w=0,h=0;',tv.index('std::string fit('))
end = tv.index('\n}',start)
tv = tv[:start] + '    return fitText(font,value,maxWidth);' + tv[end:]
start = tv.index('    SDL_Rect src{',tv.index('void aligned('))
end = tv.index('\n}',start)
tv = tv[:start] + '''    const float scale = textOutputScale;
    const float x = area.x + (middle ? (area.w-entry->width/scale)*.5f : 0.f);
    const float y = area.y + (area.h-entry->inkHeight/scale)*.5f - entry->inkTop/scale;
    copyGlyphs(r,*entry,x,y);''' + tv[end:]
# Reuse the original drawer/page distinction for rendering and pointer mapping.
drawer_start = live.index("const bool drawer=page!=Page::Home")
drawer_end = live.index(";",drawer_start)
drawer_rule = live[drawer_start + len("const bool drawer="):drawer_end]
live = live[:drawer_start] + "const bool drawer=usesDrawer()" + live[drawer_end:]
live = live.replace("    bool isOnPage(", "    bool usesDrawer() const { if (!active()) return false; const auto page=stack.back().page; return " + drawer_rule + "; }\n    bool isOnPage(")
live = live.replace('rect(r,0,0,1280,720,{0,0,0,216});rounded', 'rounded')
from privacy_ui import privacy_ui
live = privacy_ui(live)
from virtual_display_ui import virtual_display_ui
tv,live = virtual_display_ui(tv,live)
# Resolution controls pixel count. Keep a separate video-rate ceiling for
# metered or bandwidth-limited connections, using the existing quality RPC.
live = live.replace("    void loadSettings(", """    static constexpr int bitrateCaps[]={5,10,15,20,30,50,75,100};
    void loadSettings(""", 1)
live = live.replace('if(page==Page::Bitrate && a>=820 && a<=823){',
                    'if(page==Page::Bitrate && a>=820 && a<820+int(std::size(bitrateCaps))){')
live = live.replace('static constexpr int choices[]={30,50,75,100};\n            customMbps=choices[a-820];',
                    'customMbps=bitrateCaps[a-820];')
start = live.index('case Page::Bitrate:title=')
end = live.index('}break;',start)
live = live[:start] + '''case Page::Bitrate:title="视频码率上限";{
            aligned(r,f.small,"单位　Mbps",{848,124,376,42},label);
            for(int i=0;i<int(std::size(bitrateCaps));++i){
                const int cap=bitrateCaps[i];
                add(848+(i%2)*194,182+(i/2)*70,182,58,std::to_string(cap),820+i,true,
                    customQuality && customMbps==cap,"",Kind::Menu);
            }
            twoLineLabel(r,f.small,"在外串流时，按可用带宽选择上限。",{848,484,376,76},label);
            ''' + live[end:]
picture_end = live.index('break;',live.index('case Page::Picture:title='))
live = live[:picture_end] + '''row(4,"码率上限",514,true,false,customQuality?std::to_string(customMbps)+" Mbps":"主机预设");''' + live[picture_end:]
from android_drawer import style_drawer
tv = tv.replace("void twoLineLabel(", '#include "android_drawer.inc"\nvoid twoLineLabel(', 1)
live = style_drawer(live)
live = live.replace("    bool usesDrawer() const {", '''    bool isAccountSurface() const {
        return isOnPage(Page::Account) || isOnPage(Page::About) || isOnPage(Page::Logout);
    }
    bool usesDrawer() const { if(isAccountSurface())return false;''')
live = live.replace("        const bool drawer=usesDrawer();", '''        if(isAccountSurface()){
            const int online=int(std::count_if(snapshot.devices.begin(),snapshot.devices.end(),
                [&](const auto &d){return deviceState(d)==DeviceState::Online;}));
            accountui::draw(r,f,page,snapshot,online,buttons,frame.focus,touchDown?touchTarget:-1);
            return;
        }
        const bool drawer=usesDrawer();''')
from android_settings import style_local_settings
tv, live = style_local_settings(tv, live)
out.write_text(helpers + tv + live + '\n}\n', encoding='utf-8')
