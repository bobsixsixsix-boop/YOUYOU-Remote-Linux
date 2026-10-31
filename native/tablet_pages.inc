// Included inside tvui, after the upstream text and image helpers.
void twoLineLabel(SDL_Renderer *r, TTF_Font *font, const std::string &value, SDL_Rect box, SDL_Color color) {
    int width=0;
    if(TTF_SizeUTF8(font,value.c_str(),&width,nullptr)!=0 || width<=box.w) {
        aligned(r,font,value,box,color);
        return;
    }
    // fit() preserves UTF-8 boundaries. Prefer a word boundary for a model
    // name or mixed Latin/CJK device alias; keep a long, unbroken address intact.
    const auto first=fit(font,value,box.w);
    size_t cut=0;
    while(cut<first.size() && cut<value.size() && first[cut]==value[cut])++cut;
    while(cut<value.size() && cut>0 && (static_cast<unsigned char>(value[cut])&0xc0)==0x80)--cut;
    if(!cut){aligned(r,font,value,box,color);return;}
    const auto space=value.rfind(' ',cut);
    if(space!=std::string::npos && space>cut/2)cut=space;
    aligned(r,font,value.substr(0,cut),{box.x,box.y,box.w,box.h/2},color);
    const auto rest=value.find_first_not_of(' ',cut);
    if(rest!=std::string::npos)
        aligned(r,font,value.substr(rest),{box.x,box.y+box.h/2,box.w,box.h-box.h/2},color);
}

#include "android_home_pages.inc"
#include "android_account.inc"
