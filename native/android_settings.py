"""Give the homepage settings stack a tablet layout while sharing the live actions."""


def style_local_settings(tv, live):
    end = tv.rindex("\n}")
    tv = tv[:end] + '\n#include "android_settings.inc"\n' + tv[end:]
    marker = "    bool usesDrawer() const {"
    live = live.replace(marker, """    bool isLocalSettingsSurface() const {
        return stack.size() >= 2 && stack[0].page == Page::Home && stack[1].page == Page::Display;
    }
""" + marker + " if(isLocalSettingsSurface())return false;", 1)

    draw = live.index("    void draw(")
    head, body = live[:draw], live[draw:]
    body = body.replace("const bool drawer=usesDrawer();", """const bool drawer=usesDrawer();
        const bool localSettings=isLocalSettingsSurface();
        auto drawerText=[&](SDL_Renderer *rr,TTF_Font *font,const std::string &s,SDL_Rect b,SDL_Color c){
            if(!localSettings)aligned(rr,font,s,b,c);
        };
        auto drawerLines=[&](SDL_Renderer *rr,TTF_Font *font,const std::string &s,SDL_Rect b,SDL_Color c){
            if(!localSettings)twoLineLabel(rr,font,s,b,c);
        };""", 1)
    body = body.replace("else if(!snapshot.streaming || !drawer)backdrop(r);",
                        "else if(!localSettings && (!snapshot.streaming || !drawer))backdrop(r);", 1)
    for start, stop in [("Display", "Peripherals"), ("HostScreens", "Gamepad")]:
        i = body.index("        case Page::" + start + ":")
        j = body.index("        case Page::" + stop + ":", i)
        body = body[:i] + body[i:j].replace("aligned(r,", "drawerText(r,").replace(
            "twoLineLabel(r,", "drawerLines(r,") + body[j:]
    body = body.replace("if(!title.empty()) {", "if(!localSettings && !title.empty()) {", 1)
    marker = "        if(frame.focus<0 || frame.focus>=int(buttons.size()))frame.focus=0;"
    position = body.index(marker)
    body = body[:position] + '''        if(localSettings){
            const std::string format=codec==2?"AV1 优先":codec==1?"H.265":"H.264";
            const std::string feedback=!saveError.empty()?saveError:!notice.empty()?notice:snapshot.qualityNotice;
            settingsui::draw(r,f,page,title,videoSize(),std::to_string(fps)+" FPS",format,
                             panelPixels,dimensionError,activeDimension,feedback,!saveError.empty(),
                             buttons,frame.focus,touchDown?touchTarget:-1);
            return;
        }
''' + body[position:]
    return tv, head + body
