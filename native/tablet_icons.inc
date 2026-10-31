// SVGs are rasterized independently at each density during packaging. Cache the
// selected textures together with the text so the same renderer owns their lifetime.
std::vector<std::string> missingArt;

SDL_Texture *assetTexture(SDL_Renderer *r,const std::string &path,int w,int h,bool roundMask=false) {
    auto entry=std::find_if(textCache.begin(),textCache.end(),[&](const TextEntry &e){
        return e.renderer==r && !e.font && e.value==path;
    });
    if(entry!=textCache.end()){
        textCache.splice(textCache.begin(),textCache,entry);
        return textCache.front().texture;
    }
    if(std::find(missingArt.begin(),missingArt.end(),path)!=missingArt.end())return nullptr;
    std::ifstream file(path,std::ios::binary);
    std::vector<Uint8> pixels(size_t(w)*h*4);
    if(!file.read(reinterpret_cast<char*>(pixels.data()),pixels.size()) || file.peek()!=EOF){
        missingArt.push_back(path);return nullptr;
    }
    if(roundMask){
        const float radius=std::min(w,h)*.22f;
        for(int y=0;y<h;++y)for(int x=0;x<w;++x){
            const float dx=std::max(std::abs(x+.5f-w*.5f)-(w*.5f-radius),0.f);
            const float dy=std::max(std::abs(y+.5f-h*.5f)-(h*.5f-radius),0.f);
            auto &alpha=pixels[(size_t(y)*w+x)*4+3];
            alpha=Uint8(std::round(alpha*std::clamp(radius+.5f-std::hypot(dx,dy),0.f,1.f)));
        }
    }
    auto *surface=SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(),w,h,32,w*4,SDL_PIXELFORMAT_RGBA32);
    if(!surface)return nullptr;
    auto *texture=uu::display::textureFromSurface(r,surface);
    SDL_FreeSurface(surface);
    if(!texture)return nullptr;
    SDL_SetTextureScaleMode(texture,SDL_ScaleModeLinear);
    SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);
    const size_t bytes=pixels.size();
    while(!textCache.empty() && (textCache.size()>=192 || textCacheBytes+bytes>textureCacheBudget())){
        textCacheBytes-=textCache.back().bytes;
        uu::display::destroyTexture(textCache.back().texture);textCache.pop_back();
    }
    textCache.push_front({r,nullptr,path,0,texture,w,h,bytes});textCacheBytes+=bytes;
    return texture;
}

int artWidth(const std::string &name,int wanted) {
    if(name=="desktop-link")return wanted<=192?192:wanted<=384?384:576;
    if(name=="device" && wanted>128)return 384;
    return wanted<=32?32:wanted<=64?64:128;
}

bool image(SDL_Renderer *r,const std::string &name,SDL_Rect dst,SDL_Color tint=ink) {
    const bool hero=name=="desktop-link";
    const bool brand=name=="youyou-mark";
    SDL_Texture *tex=nullptr;
    if(brand)
        tex=assetTexture(r,assetRoot+"/ui-tv/youyou-mark-192.rgba",192,192);
    else{
        const int w=artWidth(name,int(std::ceil(dst.w*textOutputScale)));
        const auto path=assetRoot+"/ui-svg/"+name+"-"+std::to_string(w)+".rgba";
        tex=assetTexture(r,path,w,hero?w*2/3:w);
    }
    if(!tex && (brand || name=="mouse" || name=="touch"))
        tex=assetTexture(r,assetRoot+"/ui-tv/"+name+".rgba",64,64,brand);
    if(!tex){
        if(hero){dst.x+=(dst.w-dst.h)/2;dst.w=dst.h;return symbol(r,"device",dst);}
        if(name=="menu")return symbol(r,"more",dst);
        return symbol(r,name=="mute"?"sound":name=="peripherals"?"attachment":name,dst);
    }
    if(brand)SDL_SetTextureColorMod(tex,255,255,255);
    else SDL_SetTextureColorMod(tex,tint.r,tint.g,tint.b);
    SDL_SetTextureAlphaMod(tex,tint.a);
    return SDL_RenderCopy(r,tex,nullptr,&dst)==0;
}
