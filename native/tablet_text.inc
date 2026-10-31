// Included after the upstream SDL text-cache definition. Labels are measured
// in layout units; the cached glyphs and their ink bounds use drawable pixels.
float textOutputScale = 1.f;
size_t textureCacheBudget() {
    const double scale = std::clamp(double(textOutputScale), 1., 2.5);
    return size_t(8 * 1024 * 1024 * scale * scale);
}
int logicalTextSize(TTF_Font *font, const char *value, int *width, int *height) {
    int w = 0, h = 0;
    const int result = TTF_SizeUTF8(font, value, &w, &h);
    if (width) *width = int(std::ceil(w / textOutputScale));
    if (height) *height = int(std::ceil(h / textOutputScale));
    return result;
}
std::string fitText(TTF_Font *font, const std::string &value, int available) {
    if (available <= 0 || value.empty()) return {};
    int w = 0;
    if (logicalTextSize(font,value.c_str(),&w,nullptr) == 0 && w <= available) return value;
    // Binary search UTF-8 boundaries; never feed a substring ending inside
    // a multibyte Chinese character to SDL_ttf.
    const std::string suffix = "…";
    if (logicalTextSize(font,suffix.c_str(),&w,nullptr) != 0 || w > available) return {};
    std::vector<size_t> ends{0};
    for (size_t i = 1; i < value.size(); ++i)
        if ((static_cast<unsigned char>(value[i]) & 0xc0) != 0x80) ends.push_back(i);
    size_t low = 0, high = ends.size();
    while (low + 1 < high) {
        const size_t mid = low + (high - low) / 2;
        const auto candidate = value.substr(0,ends[mid]) + suffix;
        if (logicalTextSize(font,candidate.c_str(),&w,nullptr) == 0 && w <= available) low = mid;
        else high = mid;
    }
    return value.substr(0,ends[low]) + suffix;
}
void copyGlyphs(SDL_Renderer *renderer, const TextEntry &entry, float x, float y) {
    // Snap to the drawable pixel grid and draw the complete texture at 1:1,
    // including under KDE fractional scaling.
    const float scale = textOutputScale;
    SDL_FRect dst{std::round(x*scale)/scale, std::round(y*scale)/scale,
                  entry.width/scale, entry.height/scale};
    SDL_RenderCopyF(renderer,entry.texture,nullptr,&dst);
}
