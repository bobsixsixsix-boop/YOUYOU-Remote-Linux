"""Add host-reported monitors to the upstream display menu."""
def virtual_display_ui(tv,live):
    # Explicit values avoid renumbering pages persisted by existing builds.
    end=tv.index("};",tv.index("enum class Page {"))
    tv=tv[:end]+",HostScreens=150,VirtualDisplay,RemoveDisplay"+tv[end:]
    marker="bool opensPage(int a) {"
    tv=tv.replace(marker, marker+'\n    if(a==9040 || a==9041 || a==9044)return true;',1)
    live=live.replace("    void action(int a,const uu::Snapshot &s){",'''    int hostPage=0;
    std::array<std::optional<uu::HostScreen>,3> hostRows;
    std::optional<uu::HostScreen> removeTarget;
    uint64_t removeSession=0;
    std::optional<bool> desktopModePending;
    uint64_t desktopModeSession=0;
    uint32_t desktopModeStarted=0;
    std::string desktopModeDevice,desktopModePreviousNotice,desktopModeRetryNotice;
    void refreshDesktopMode(const uu::Snapshot &s){
        if(!desktopModePending && desktopModeRetryNotice.empty())return;
        if(!s.streaming || s.activeDeviceId!=desktopModeDevice || !s.displays ||
           s.displays->session!=desktopModeSession){
            desktopModePending.reset();desktopModeRetryNotice.clear();return;
        }
        // Core publishes a notice when it rejects this command. Success starts
        // a new session without publishing an intermediate success notice.
        if(desktopModePending && !s.displays->notice.empty() && s.displays->notice!=desktopModePreviousNotice){
            desktopModePending.reset();desktopModeRetryNotice=s.displays->notice;
        }else if(desktopModePending && uint32_t(SDL_GetTicks()-desktopModeStarted)>=5000){
            desktopModePending.reset();desktopModeRetryNotice="切换尚未开始，请重试";
        }
    }
    void action(int a,const uu::Snapshot &s){
        refreshDesktopMode(s);
        if(a==9040){hostPage=0;push(Page::HostScreens);return;}
        if(a==9041){push(Page::VirtualDisplay);return;}
        if(a==9048){
            if(!client || !s.streaming || !s.displays || !s.displays->session ||
               s.activeDeviceId.empty() || s.displays->pending || desktopModePending)return;
            desktopModePending=!s.secondScreen;
            desktopModeSession=s.displays->session;desktopModeDevice=s.activeDeviceId;
            desktopModeStarted=SDL_GetTicks();desktopModePreviousNotice=s.displays->notice;
            desktopModeRetryNotice.clear();
            client->command("desktop-mode",{{"deviceId",s.activeDeviceId},{"session",desktopModeSession},
                {"enabled",*desktopModePending}});
            return;
        }
        if(a==9044){
            if(s.secondScreen || desktopModePending || !s.displays || !s.displays->canRemove())return;
            removeTarget=*s.displays->find(s.displays->owned);removeSession=s.displays->session;
            push(Page::RemoveDisplay);return;
        }
        if(a==9046 || a==9047){hostPage+=a==9046?-1:1;stack.back().focus=0;return;}
        if(a==9042 || a==9043 || a==9045 || (a>=9060 && a<=9062)){
            if(!client || !s.displays || !s.streaming)return;
            if(desktopModePending || (s.secondScreen && a<9060))return;
            const auto &v=*s.displays;
            const uu::HostScreen *screen=nullptr;
            if(a>=9060){auto &row=hostRows[a-9060];if(!row)return;screen=&*row;}
            else if(a!=9042){
                screen=v.find(v.owned);if(!screen)return;
                if(a==9045 && (!removeTarget || removeSession!=v.session || !removeTarget->identity(*screen)))return;
            }
            std::vector<std::pair<int,int>> modes;
            if(a==9042){
                const auto panel=displayPixels();
                for(const auto &scale:uu::panelScales){
                    const auto size=uu::panelVideoSize(panel.first,panel.second,scale);
                    if(size.first)modes.push_back(size);
                }
            }
            client->command("virtual-display",{{"op",a==9042?"create":a==9045?"remove":"select"},{"modes",modes},
                {"session",v.session},{"deviceId",s.activeDeviceId},{"target",screen?screen->id:-1},
                {"type",screen?screen->type:-1},{"name",screen?screen->name:std::string{}}});
            if(a==9045)back();
            return;
        }''',1)
    live=live.replace('void draw(SDL_Renderer *r,Fonts &f,const uu::Snapshot &snapshot,const uu::StreamStatsSnapshot &stats){',
        'void draw(SDL_Renderer *r,Fonts &f,const uu::Snapshot &snapshot,const uu::StreamStatsSnapshot &stats){\n        refreshDesktopMode(snapshot);',1)
    start=live.index('case Page::Display:title="显示"')
    end=live.index("break;",start)
    live=live[:end]+'''row(2,"主机显示器",9040);
            row(3,"平板专用桌面",9041,true,false,snapshot.streaming?(snapshot.secondScreen?"记忆桌面":"虚拟屏"):"连接后可用");'''+live[end:]
    marker='        case Page::Gamepad:'
    live=live.replace(marker,'''        case Page::HostScreens: {
            title="主机显示器";
            const uu::DisplayView empty;const auto &v=snapshot.displays?*snapshot.displays:empty;
            const bool ready=snapshot.streaming && snapshot.textChannelReady && !v.pending;
            add(848,148,376,54,"专用虚拟屏",9041,true,false,"",Kind::Menu);
            const int pages=std::max(1,(int(v.screens.size())+2)/3);
            hostPage=std::clamp(hostPage,0,pages-1);hostRows={};
            for(int i=0;i<3 && hostPage*3+i<int(v.screens.size());++i){
                const auto &screen=v.screens[hostPage*3+i];hostRows[i]=screen;
                const std::string kind=screen.type==0?"物理屏":screen.type==1?"虚拟屏":"其他屏幕";
                add(848,214+i*68,376,58,kind+" "+std::to_string(screen.id),9060+i,
                    ready && v.current>=0 && screen.id!=v.current,screen.id==v.current,
                    screen.id==v.current?"当前":screen.width?std::to_string(screen.width)+"×"+std::to_string(screen.height):"",Kind::Menu);
            }
            if(pages>1){
                add(848,426,178,50,"上一页",9046,hostPage>0);
                add(1046,426,178,50,"下一页",9047,hostPage+1<pages);
            }
            twoLineLabel(r,f.small,v.notice.empty()?(v.known?"选择屏幕以切换串流":"等待电脑发送屏幕列表"):v.notice,
                         {848,510,376,70},label);
            break;
        }
        case Page::VirtualDisplay: {
            title="平板专用桌面";
            const uu::DisplayView empty;const auto &v=snapshot.displays?*snapshot.displays:empty;
            const bool ready=snapshot.streaming && snapshot.textChannelReady;
            const auto *owned=v.find(v.owned);
            const auto *managed=v.find(v.sessionManaged);
            const bool switching=desktopModePending.has_value();
            const std::string mode=std::to_string(snapshot.width)+"×"+std::to_string(snapshot.height)+" · "+std::to_string(snapshot.fps)+" FPS";
            aligned(r,f.small,"串流目标　"+mode,{848,124,376,40},label);
            add(848,174,376,58,switching?"正在切换专用桌面…":"记忆专用桌面",9048,
                ready && v.session && !v.pending && !switching,snapshot.secondScreen,"",Kind::Toggle);
            twoLineLabel(r,f.small,switching?"正在重新连接，请稍候":!desktopModeRetryNotice.empty()?desktopModeRetryNotice:
                "位置由电脑保存，连接时自动恢复。切换会重连",
                         {848,240,376,56},label);
            if(snapshot.secondScreen){
                const std::string actual=managed?"专用桌面　"+std::to_string(managed->width)+"×"+std::to_string(managed->height)+
                    (managed->fps>0?" · "+std::to_string(managed->fps)+" Hz":""):"等待电脑确认专用桌面";
                twoLineLabel(r,f.small,actual,{848,312,376,65},ink);
                add(848,394,376,58,"查看主机显示器",9040,!switching,false,"",Kind::Menu);
                twoLineLabel(r,f.small,v.notice.empty()?"关闭上方开关，重新连接普通桌面":v.notice,
                             {848,486,376,80},label);
                break;
            }
            add(848,306,376,58,"创建临时虚拟屏",9042,ready && !switching && v.canCreate(),false,"",Kind::Menu);
            add(848,374,376,58,"切换到临时虚拟屏",9043,
                ready && !switching && owned && !v.pending && v.current!=v.owned,false,
                owned && v.current==v.owned?"当前":"",Kind::Menu);
            add(848,442,376,58,"移除本次创建的屏幕",9044,ready && !switching && v.canRemove(),false,"",Kind::Menu);
            std::string hint="创建后，先切回其他屏幕再移除";
            if(!v.known)hint="等待电脑发送屏幕列表";
            else if(v.createUncertain)hint="上次请求结果不明，请在电脑上核对";
            else if(!owned && !v.canCreate() && !v.pending)hint="电脑已有虚拟屏，可在主机显示器中选择";
            if(owned)hint="虚拟桌面　"+std::to_string(owned->width)+"×"+std::to_string(owned->height)+
                (owned->fps>0?" · "+std::to_string(owned->fps)+" Hz":"");
            twoLineLabel(r,f.small,v.notice.empty()?hint:v.notice,{848,516,376,60},label);
            break;
        }
        case Page::RemoveDisplay: {
            title="移除专用虚拟屏";
            const auto *v=snapshot.displays.get();
            const auto *target=v?v->find(v->owned):nullptr;
            const bool same=!snapshot.secondScreen && !desktopModePending && target && removeTarget &&
                removeSession==v->session && removeTarget->identity(*target);
            twoLineLabel(r,f.small,same?"移除本次创建的虚拟屏 "+std::to_string(target->id)+"？":"屏幕已变化",
                         {848,172,376,82},ink);
            row(2,"返回",99);row(3,"确认移除",9045,same && v->canRemove() && snapshot.textChannelReady);
            break;
        }
'''+marker,1)
    return tv,live
