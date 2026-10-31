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
    void action(int a,const uu::Snapshot &s){
        if(a==9040){hostPage=0;push(Page::HostScreens);return;}
        if(a==9041){push(Page::VirtualDisplay);return;}
        if(a==9044){
            if(!s.displays || !s.displays->canRemove())return;
            removeTarget=*s.displays->find(s.displays->owned);removeSession=s.displays->session;
            push(Page::RemoveDisplay);return;
        }
        if(a==9046 || a==9047){hostPage+=a==9046?-1:1;stack.back().focus=0;return;}
        if(a==9042 || a==9043 || a==9045 || (a>=9060 && a<=9062)){
            if(!client || !s.displays || !s.streaming)return;
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
    start=live.index('case Page::Display:title="显示"')
    end=live.index("break;",start)
    live=live[:end]+'''row(2,"主机显示器",9040);
            row(3,"平板专用桌面",9041,true,false,snapshot.streaming?"虚拟屏":"连接后可用");'''+live[end:]
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
            title="专用虚拟屏";
            const uu::DisplayView empty;const auto &v=snapshot.displays?*snapshot.displays:empty;
            const bool ready=snapshot.streaming && snapshot.textChannelReady;
            const auto *owned=v.find(v.owned);
            const std::string mode=std::to_string(snapshot.width)+"×"+std::to_string(snapshot.height)+" · "+std::to_string(snapshot.fps)+" FPS";
            aligned(r,f.small,"串流目标　"+mode,{848,124,376,40},label);
            row(0,"创建虚拟屏",9042,ready && v.canCreate());
            row(1,"切换到虚拟屏",9043,ready && owned && !v.pending && v.current!=v.owned,false,
                owned && v.current==v.owned?"当前":"");
            row(2,"移除本次创建的屏幕",9044,ready && v.canRemove());
            std::string hint="创建后，先切回其他屏幕再移除";
            if(!v.known)hint="等待电脑发送屏幕列表";
            else if(v.createUncertain)hint="上次请求结果不明，请在电脑上核对";
            else if(!owned && !v.canCreate() && !v.pending)hint="电脑已有虚拟屏，可在主机显示器中选择";
            if(owned)hint="虚拟桌面　"+std::to_string(owned->width)+"×"+std::to_string(owned->height)+
                (owned->fps>0?" · "+std::to_string(owned->fps)+" Hz":"");
            twoLineLabel(r,f.small,hint,{848,428,376,73},label);
            twoLineLabel(r,f.small,v.notice.empty()?"断线后保留屏幕，由电脑端管理":v.notice,{848,507,376,74},label);
            break;
        }
        case Page::RemoveDisplay: {
            title="移除专用虚拟屏";
            const auto *v=snapshot.displays.get();
            const auto *target=v?v->find(v->owned):nullptr;
            const bool same=target && removeTarget && removeSession==v->session && removeTarget->identity(*target);
            twoLineLabel(r,f.small,same?"移除本次创建的虚拟屏 "+std::to_string(target->id)+"？":"屏幕已变化",
                         {848,172,376,82},ink);
            row(2,"返回",99);row(3,"确认移除",9045,same && v->canRemove() && snapshot.textChannelReady);
            break;
        }
'''+marker,1)
    return tv,live
