// Included in LiveFrontend. Pointer coordinates use the same logical viewport as draw().
bool outsidePress=false;

void closeStreamOverlay() {
    stopKeyboard();
    if(client)client->command("release");
    cancelPointer();
    stack.clear();
    buttons.clear();
    draining=true;
}

void pointer(bool down,int x,int y) {
    if(!active() || draining || (!down && !touchDown))return;
    const SDL_Point point{x,y};
    const SDL_Rect surface=usesDrawer()?drawerTheme::sheetBounds:SDL_Rect{0,0,1280,720};
    const bool outside=wasStreaming && !SDL_PointInRect(&point,&surface);
    if(down && !touchDown){
        outsidePress=outside;
        pointerX=x;pointerY=y;
        touchStartY=y;touchTarget=-1;
        for(size_t i=0;i<buttons.size();++i){
            if(buttons[i].enabled && SDL_PointInRect(&point,&buttons[i].box)){
                touchTarget=int(i);stack.back().focus=int(i);
            }
        }
    }
    // A drag into or out of the sheet must never turn the trailing lift into a dismiss.
    if(!outside || std::abs(x-pointerX)>12 || std::abs(y-pointerY)>12)outsidePress=false;
    if(down && touchTarget>=0 &&
       (touchTarget>=int(buttons.size()) || !SDL_PointInRect(&point,&buttons[touchTarget].box)))
        touchTarget=-1;
    if(down && stack.back().page==Page::Devices && std::abs(y-touchStartY)>40){
        deviceOffset=std::max(0,deviceOffset+(y<touchStartY?1:-1));touchTarget=-1;touchStartY=y;
    }
    if(!down && outsidePress){closeStreamOverlay();return;}
    if(!down && touchTarget>=0 && touchTarget<int(buttons.size()) && SDL_PointInRect(&point,&buttons[touchTarget].box))
        hit=touchTarget;
    touchDown=down;
    if(!down)touchTarget=-1;
}

void movePointer(int x,int y) {
    // A page transition, Escape or a second finger may have cancelled this press.
    if(touchDown)pointer(true,x,y);
}
