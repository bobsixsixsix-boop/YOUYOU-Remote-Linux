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
            if(client && s.streaming && s.textChannelReady && !s.privacyPending && !s.secondScreen){
                const auto mode=a==755?uu::PrivacyMode::Normal:
                    a==756?uu::PrivacyMode::ScreenOff:uu::PrivacyMode::Off;
                client->command("privacy-screen",{{"deviceId",s.activeDeviceId},{"mode",unsigned(mode)}});
            }
            break;
        case 9049:
            // Only this explicit action leaves the incompatible connection mode.
            // Reuse the session-checked desktop-mode command and its pending UI.
            if(s.secondScreen && s.streaming && s.textChannelReady && !s.privacyPending &&
               !s.qualityPending && !s.settingApplyPending)action(9048,s);
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
            std::string stateText=snapshot.privacyNotice;
            if(snapshot.secondScreen){
                const bool switching=desktopModePending.has_value();
                const bool canSwitch=ready && snapshot.displays && snapshot.displays->session &&
                    !snapshot.activeDeviceId.empty() && !snapshot.displays->pending &&
                    !snapshot.qualityPending && !snapshot.settingApplyPending && !switching;
                twoLineLabel(r,f.small,"当前主机的专用桌面模式不支持防窥。",{848,248,376,68},ink);
                add(848,344,376,64,switching?"正在重新连接普通桌面…":"切换普通桌面并重连",
                    9049,canSwitch,false,"",Kind::Button);
                twoLineLabel(r,f.small,"重连会关闭这块专用桌面，窗口可能移动。",{848,430,376,68},label);
                stateText=switching?"正在重新连接，请稍候":!desktopModeRetryNotice.empty()?desktopModeRetryNotice:
                    "重连后请再次打开防窥页，选择要启用的模式。";
            }else{
                row(1,"启用隐私屏保",755,ready,mode==uu::PrivacyMode::Normal);
                row(2,"关闭被控显示屏",756,ready,mode==uu::PrivacyMode::ScreenOff);
                row(3,"恢复正常显示",854,ready,mode==uu::PrivacyMode::Off);
                if(stateText.empty())stateText=!snapshot.streaming?"连接电脑后可设置":
                    !snapshot.textChannelReady?"正在等待电脑控制通道":
                    "隐私屏保遮住电脑画面，平板仍可操作。";
            }
''' + live[end:]
    live = live.replace('page==Page::Bitrate || page==Page::Resolution',
                        'page==Page::ScreenProtection || page==Page::Bitrate || page==Page::Resolution')
    return live
