"""Style the pinned SDL drawer while keeping its live actions and hit tests."""


def style_drawer(live):
    def replace(old, new):
        nonlocal live
        if live.count(old) != 1:
            raise ValueError("Unexpected upstream drawer code: " + old[:70])
        live = live.replace(old, new)

    replace("rounded(r,{824,32,424,624},panel,13);", "drawerTheme::sheet(r);")
    start = live.index("        case Page::Toolbar:", live.index("    void draw("))
    end = live.index("        case Page::Display:", start)
    live = live[:start] + '''        case Page::Toolbar: {
            drawerTheme::toolbarHeader(r,f,snapshot.streaming);
            add(848,84,88,72,"退出",20,true,false,"",Kind::Shortcut);
            add(944,84,88,72,"键盘",55,true,false,"",Kind::Shortcut);
            add(1040,84,88,72,sound?"声音":"已静音",48,true,false,"",Kind::Shortcut);
            add(1136,84,88,72,"防窥",757,true,false,"",Kind::Shortcut);
            const char *names[]={"显示","安全","外设","更多","Windows 快捷操作"};
            for(int i=0;i<5;++i)add(848,196+66*i,376,58,names[i],21+i,true,false,"",Kind::Menu);
            aligned(r,f.hint,"F8  /  Select + Start",{848,530,376,45},drawerTheme::secondary,true);
            break;}
''' + live[end:]
    replace('add(848,584,376,56,"返回",99)', 'add(848,584,376,56,"收起工具栏",99)')
    replace('if(drawer)rect(r,848,136,376,1,{73,78,85,255});',
            'if(drawer){drawerTheme::context(r,f,page);rect(r,848,136,376,1,drawerTheme::line);}')
    replace("rounded(r,b.box,fill,10);const char *asset=",
            "if(drawer)drawerTheme::button(r,b,focused,touchDown && touchTarget==int(i));\n"
            "            else rounded(r,b.box,fill,10);\n            const char *asset=")
    replace("const auto tint=b.enabled?", """const auto tint=drawer?(b.enabled?drawerTheme::ink:drawerTheme::secondary):b.enabled?""")
    replace("b.box.y+13,30,30", "b.box.y+(drawer?9:13),30,30")
    replace("b.box.y+51,b.box.w-16,30", "b.box.y+(drawer?41:51),b.box.w-16,30")
    replace("b.chosen?SDL_Color{41,185,184,255}:SDL_Color{99,104,113,255}",
            "b.chosen?(drawer?drawerTheme::blue:SDL_Color{41,185,184,255}):SDL_Color{99,104,113,255}")
    replace('if(radio)image(r,b.chosen?"radio":"radio_empty",{b.box.x+b.box.w-38,b.box.y+(b.box.h-26)/2,26,26},tint);',
            'if(radio)image(r,b.chosen?"radio":"radio_empty",{b.box.x+b.box.w-38,b.box.y+(b.box.h-26)/2,26,26},'
            'drawer?(b.chosen?drawerTheme::paleBlue:drawerTheme::secondary):tint);')
    return live
