"""Extend the pinned menu with the tablet's local numeric resolution editor."""
from pathlib import Path


def adapt(tv, live):
    # Append an enum member to keep the upstream IDs stable.
    enum_end = tv.index("};", tv.index("enum class Page"))
    tv = tv[:enum_end] + ",CustomResolution,FixedResolution" + tv[enum_end:]
    tv = tv.replace("return (a>=10 && a<=14)", "return a==3000 || a==3030 || (a>=10 && a<=14)")
    editor = Path(__file__).with_name("resolution_editor.inc").read_text(encoding="utf-8")
    live = live.replace("    void loadSettings(", editor + "\n    void loadSettings(")
    live = live.replace("resolution=s.height;", "resolutionWidth=s.width;resolution=s.height;")
    live = live.replace('const auto data=preferences.dump(2);', '''if(lastCustomSize.first){
                preferences["custom_video_width"]=lastCustomSize.first;
                preferences["custom_video_height"]=lastCustomSize.second;
            }
            const auto data=preferences.dump(2);''')
    live = live.replace('"volume","mouse_speed","touch_mode","scheme","xbox_layout"', '"volume","mouse_speed","touch_mode","scheme","xbox_layout","custom_video_width","custom_video_height"')
    live = live.replace('deviceSelection.restore(p.value(', '''const Dimensions custom{p.value("custom_video_width",0),p.value("custom_video_height",0)};
            if(uu::streamSize(custom.first,custom.second))lastCustomSize=custom;
            deviceSelection.restore(p.value(''')
    live = live.replace("void applyQuality(const uu::Snapshot &s){", "void applyQuality(const uu::Snapshot &s){\n        if(!client)return; // Read-only preview.")
    live = live.replace('{"width",resolution==540?960:resolution==1080?1920:1280}', '{"width",resolutionWidth}')
    live = live.replace('std::to_string(resolution)+"p"', 'videoSize()')
    live = live.replace("case 44:push(Page::Resolution);", "case 44:openScalePage();")
    start = live.index('case Page::Picture:title="画面设置"')
    end = live.index("break;", start)
    live = live[:end] + 'row(3,"平板专用桌面",9041);' + live[end:]
    start = live.index("        if(page==Page::Resolution && a!=99)")
    end = live.index("        if(page==Page::Codec",start)
    live = live[:start] + '''        if(page==Page::Resolution && a==3030){push(Page::FixedResolution);return;}
        if(page==Page::Resolution && a>=3040 && a<3040+int(scaleTargets.size())){
            applyDimensions(scaleTargets[a-3040],s);return;
        }
        if(page==Page::FixedResolution && a==3000){openResolutionEditor();return;}
        if(page==Page::CustomResolution && a!=99){resolutionAction(a,s);return;}
        if(page==Page::FixedResolution && (a==940 || a==1120 || a==1480)){
            const int height=a-400;
            applyDimensions({height*16/9,height},s);back();return;
        }
''' + live[end:]
    live = live.replace('if(e.type==SDL_MOUSEBUTTONDOWN &&', '''if(stack.back().page==Page::CustomResolution && e.type==SDL_KEYDOWN){
            dimensionKey(e.key.keysym.sym);return;
        }
        if(e.type==SDL_MOUSEBUTTONDOWN &&''')
    start = live.index("        case Page::Resolution:title=")
    end = live.index("        case Page::Codec:",start)
    live = live[:start] + '''        case Page::Resolution:title="按屏幕比例缩放";
            drawScalePage(r,f,add,snapshot,stats);break;
        case Page::FixedResolution:{title="固定 / 自定义";
            aligned(r,f.hint,"固定宽高　·　16∶9 常用尺寸",{848,97,376,31},label);
            row(0,"960 × 540",940,true,resolutionWidth==960 && resolution==540);
            row(1,"1280 × 720",1120,true,resolutionWidth==1280 && resolution==720);
            row(2,"1920 × 1080",1480,true,resolutionWidth==1920 && resolution==1080);
            row(3,"自定义宽高",3000,true,false,lastCustomSize.first?pixels(lastCustomSize):"");
            aligned(r,f.small,"串流目标　"+videoSize(),{848,507,376,32},ink);
            break;}
        case Page::CustomResolution:title="自定义视频尺寸";drawResolutionEditor(r,f,add,snapshot.qualityPending);break;
''' + live[end:]
    live = live.replace('page==Page::Bitrate || page==Page::Resolution || page==Page::Codec',
                        'page==Page::Bitrate || page==Page::Resolution && b.action!=3030 || page==Page::FixedResolution && b.action!=3000 || page==Page::Codec')
    live = live.replace('(page==Page::Resolution&&a!=99)', '((page==Page::Resolution&&a>=3040&&a<=3045)||(page==Page::FixedResolution&&a!=99&&a!=3000))')
    return tv, live
