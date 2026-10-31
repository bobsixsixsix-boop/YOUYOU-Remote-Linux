"""Adapt the streaming privacy controls to the tablet drawer."""


def privacy_ui(live):
    start = live.index("            add(848,56,112,88,", live.index("        case Page::Toolbar:"))
    end = live.index("            rect(r,848,160,", start)
    live = live[:start] + '''            add(848,56,88,88,"退出",20,true,false,"",Kind::Shortcut);
            add(944,56,88,88,"键盘",55,true,false,"",Kind::Shortcut);
            add(1040,56,88,88,sound?"声音":"已静音",48,true,false,"",Kind::Shortcut);
            add(1136,56,88,88,"防窥",757,true,false,"",Kind::Shortcut);
''' + live[end:]
    start = live.index("        case 755:case 756:")
    end = live.index("        case 760:", start)
    live = live[:start] + '''        case 755:case 756:case 854:
            if(client && s.streaming && s.textChannelReady && !s.privacyPending){
                const auto mode=a==755?uu::PrivacyMode::Normal:
                    a==756?uu::PrivacyMode::ScreenOff:uu::PrivacyMode::Off;
                client->command("privacy-screen",{{"deviceId",s.activeDeviceId},{"mode",unsigned(mode)}});
            }
            break;
''' + live[end:]
    live = live.replace('row(4,"屏幕保护",757', 'row(4,"防窥模式",757')
    start = live.index('        case Page::ScreenProtection:{')
    end = live.index('            // Status is internal', start)
    live = live[:start] + '''        case Page::ScreenProtection:{title="被控端防窥模式";
            const auto mode=snapshot.privacyKnown?snapshot.privacyMode:uu::PrivacyMode::Unknown;
            const bool ready=snapshot.streaming && snapshot.textChannelReady && !snapshot.privacyPending;
            const std::string current=mode==uu::PrivacyMode::Off?"正常显示":
                mode==uu::PrivacyMode::Normal?"隐私屏保":mode==uu::PrivacyMode::ScreenOff?"显示屏关闭":"未确认";
            info(0,"电脑当前状态",current);
            row(1,"启用隐私屏保",755,ready,mode==uu::PrivacyMode::Normal);
            row(2,"关闭被控显示屏",756,ready,mode==uu::PrivacyMode::ScreenOff);
            row(3,"恢复正常显示",854,ready,mode==uu::PrivacyMode::Off);
            std::string stateText=snapshot.privacyNotice;
            if(stateText.empty())stateText=!snapshot.streaming?"连接电脑后可设置":
                !snapshot.textChannelReady?"正在等待电脑控制通道":
                "隐私屏保遮住电脑画面，平板仍可操作。";
''' + live[end:]
    live = live.replace('page==Page::Bitrate || page==Page::Resolution',
                        'page==Page::ScreenProtection || page==Page::Bitrate || page==Page::Resolution')
    return live
