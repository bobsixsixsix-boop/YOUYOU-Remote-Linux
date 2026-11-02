#include "quality_rpc.hpp"
#include "stream_limits.hpp"
#include <limits>
#include <cstring>
#include <cmath>
#include <set>
#include <stdexcept>
namespace uu {
namespace {
void var(Bytes &b,uint64_t value) {while(value>127){b.push_back(uint8_t(value)|128);value>>=7;}b.push_back(uint8_t(value));}
void integer(Bytes &b,unsigned key,uint64_t value){var(b,key*8);var(b,value);}
void message(Bytes &b,unsigned key,const Bytes &value){var(b,key*8+2);var(b,value.size());b.insert(b.end(),value.begin(),value.end());}
struct Field {unsigned key=0,wire=0;uint64_t value=0;const uint8_t *data=nullptr;size_t size=0;};
class Reader {
    const uint8_t *p,*end;unsigned fields=0;bool valid=true;
    bool varint(uint64_t &value){value=0;for(unsigned i=0;i<10;++i){if(p==end)return valid=false;const auto byte=*p++;if(i==9 && byte>1)return valid=false;value|=uint64_t(byte&127)<<(i*7);if(!(byte&128))return true;}return valid=false;}
  public:
    Reader(const uint8_t *data,size_t size):p(data),end(data+size){}
    bool next(Field &f){
        if(!valid || p==end)return false;
        if(++fields>128)return valid=false;
        uint64_t tag=0;if(!varint(tag) || !(tag>>3) || (tag>>3)>0x1fffffff)return valid=false;
        f={};f.key=unsigned(tag>>3);f.wire=unsigned(tag&7);
        if(f.wire==0)return varint(f.value);
        uint64_t length=0;
        if(f.wire==2){if(!varint(length))return false;}else if(f.wire==1)length=8;else if(f.wire==5)length=4;else return valid=false;
        if(length>uint64_t(end-p))return valid=false;
        f.data=p;f.size=size_t(length);p+=length;return true;
    }
    bool good()const{return valid && p==end;}
};
}
// APK Main.Message field 3 / General.SimpleAction field 1; enum values
// recovered from cg.d1. These are one-way commands, not acknowledged RPCs.
// Verified mobile Wire adapters K3/C3/X6/b7: Main 20 -> Input 1 -> Touch.
Bytes nativeTouchMessage(uint64_t seq,int type,const Json &points){
    if(!seq || type<1 || type>4 || !points.is_array() || points.size()>10 || ((type<=2)==points.empty()))
        throw std::invalid_argument("Invalid native touch snapshot");
    Bytes touch,input,out;integer(touch,1,type);std::set<int> ids;
    auto real=[&](Bytes &dst,unsigned tag,float value){uint32_t bits;std::memcpy(&bits,&value,4);var(dst,tag*8+5);for(int i=0;i<4;++i)dst.push_back(uint8_t(bits>>(i*8)));};
    for(const auto &p:points){
        const int id=p.at("id").get<int>();const float x=p.at("x").get<float>(),y=p.at("y").get<float>();
        if(id<0 || id>9 || !ids.insert(id).second || !std::isfinite(x)||!std::isfinite(y)||x<0||x>1||y<0||y>1)
            throw std::invalid_argument("Invalid native touch point");
        Bytes point;integer(point,1,id);real(point,2,x);real(point,3,y);real(point,4,2.f);real(point,5,2.f);
        message(touch,2,point);
    }
    message(input,1,touch);integer(out,1,seq);integer(out,2,nowMs());message(out,20,input);return out;
}
Bytes simpleActionMessage(uint64_t seq,unsigned action,std::optional<bool> enabled) {
    if(!seq || !(action==2 || action==9 || action==10 || action==12 || action==13 ||
        action==14 || action==16 || action==17 || action==18 || action==19) || (action==19)!=enabled.has_value())
        throw std::invalid_argument("Invalid system action");
    Bytes body,out;integer(body,1,action);
    // Mobile AutoLockScreen.h: true takes const 2, false branches to const 1.
    // Confirmed independently by the Linux host's SET_AUTO_LOCK decoder.
    if(enabled)integer(body,5,*enabled?2:1);
    integer(out,1,seq);integer(out,2,nowMs());message(out,3,body);return out;
}
// Mobile 4.44: Main21 -> RpcRequest7 -> PrivacyScreenSetting.
// PS_NOT_SET=1, PS_NORMAL_PRIVACY=2, PS_ENHANCED_PRIVACY=3 (mobile screen off).
Bytes privacyScreenRpc(uint64_t id,PrivacyMode mode){
    const auto value=unsigned(mode);
    if(!id || value<1 || value>3)throw std::invalid_argument("Invalid privacy request");
    Bytes body,header,rpc,out;integer(body,1,mode!=PrivacyMode::Off);integer(body,2,value);
    integer(header,1,id);message(rpc,1,header);message(rpc,7,body);
    integer(out,1,id);integer(out,2,nowMs());message(out,21,rpc);return out;
}
std::optional<PrivacyRpcAck> parsePrivacyRpcAck(const uint8_t *data,size_t size) noexcept{
    if(!data || !size || size>65536)return {};
    PrivacyRpcAck out;bool response=false,header=false,id=false,body=false,code=false;unsigned modeFields=0;
    Reader outer(data,size);Field f;
    while(outer.next(f))if(f.key==22){
        if(response || f.wire!=2)return {};response=true;
        Reader rpc(f.data,f.size);Field r;
        while(rpc.next(r)){
            if(r.key==1){
                if(header || r.wire!=2)return {};header=true;
                Reader h(r.data,r.size);Field v;
                while(h.next(v))if(v.key==1){if(id || v.wire!=0 || !v.value)return {};id=true;out.requestId=v.value;}
                if(!h.good())return {};
            }else if(r.key==3){
                if(body || r.wire!=2)return {};body=true;
                Reader b(r.data,r.size);Field v;
                while(b.next(v)){
                    if(v.key==1){if(code || v.wire!=0)return {};code=true;out.errorCode=int(int32_t(v.value));}
                    else if(v.key==2 || v.key==3){
                        const unsigned bit=1u<<v.key;
                        if((modeFields&bit) || v.wire!=0 || v.value>3)return {};
                        modeFields|=bit;
                        if(v.key==2)out.requestedMode=static_cast<PrivacyMode>(v.value);
                        else out.currentMode=static_cast<PrivacyMode>(v.value);
                    }
                }
                if(!b.good())return {};
            }
        }
        if(!rpc.good())return {};
    }
    // An empty privacy body has protobuf's default error_code=0. A bare header
    // or another RPC's response must never confirm this operation.
    if(!outer.good() || !response || !header || !id || !body)return {};
    return out;
}
Bytes clipboardTextRpc(uint64_t id,const std::string &text){
    if(!id || text.empty() || text.size()>2048 || text.find('\0')!=std::string::npos)
        throw std::invalid_argument("Invalid text input size");
    // Validate UTF-8 before encoding a protobuf string; do not log its contents.
    for(size_t i=0;i<text.size();){
        const unsigned c=uint8_t(text[i++]);if(c<0x80)continue;
        unsigned n=0,cp=0,min=0;
        if(c>=0xc2 && c<=0xdf){n=1;cp=c&31;min=0x80;}
        else if(c>=0xe0 && c<=0xef){n=2;cp=c&15;min=0x800;}
        else if(c>=0xf0 && c<=0xf4){n=3;cp=c&7;min=0x10000;}
        else throw std::invalid_argument("Invalid text encoding");
        if(i+n>text.size())throw std::invalid_argument("Truncated text encoding");
        while(n--){const unsigned b=uint8_t(text[i++]);if((b&0xc0)!=0x80)throw std::invalid_argument("Invalid text encoding");cp=(cp<<6)|(b&63);}
        if(cp<min || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff))throw std::invalid_argument("Invalid text codepoint");
    }
    // Mobile ClipboardTextChangeReq: Main21 -> RpcRequest10 -> {type1=1,text2}.
    Bytes change,header,rpc,out;integer(change,1,1);
    message(change,2,Bytes(text.begin(),text.end()));integer(header,1,id);
    message(rpc,1,header);message(rpc,10,change);
    integer(out,1,id);integer(out,2,nowMs());message(out,21,rpc);return out;
}
std::optional<TextRpcAck> parseTextRpcAck(const uint8_t *data,size_t size)noexcept{
    if(!data || !size || size>65536)return {};
    TextRpcAck out;bool response=false,header=false,id=false,body=false,status=false;
    Reader outer(data,size);Field f;
    while(outer.next(f))if(f.key==22){
        if(response || f.wire!=2)return {};response=true;
        Reader rpc(f.data,f.size);Field r;
        while(rpc.next(r)){
            if(r.key==1){
                if(header || r.wire!=2)return {};header=true;
                Reader h(r.data,r.size);Field v;
                while(h.next(v))if(v.key==1){if(id || v.wire!=0 || !v.value)return {};id=true;out.requestId=v.value;}
                if(!h.good())return {};
            }else if(r.key==6){
                if(body || r.wire!=2)return {};body=true;
                Reader b(r.data,r.size);Field v;
                while(b.next(v))if(v.key==1){if(status || v.wire!=0 || v.value>2)return {};status=true;out.status=unsigned(v.value);}
                if(!b.good())return {};
            }
        }
        if(!rpc.good())return {};
    }
    // CB_UNSPECIFIED=0 is never treated as success. CB_SUCCEEDED=1, FAILED=2.
    if(!outer.good() || !response || !header || !id || !body)return {};
    return out;
}
Bytes fpsSettingRpc(uint64_t id,int fps){
    if(!id || (fps!=30&&fps!=60))throw std::invalid_argument("Invalid fps setting");
    Bytes body,header,rpc,out;
    integer(body,1,fps==30?1:2); // gv_pb.FPS: FPS_30=1, FPS_60=2
    integer(header,1,id);message(rpc,1,header);message(rpc,3,body);
    integer(out,1,id);integer(out,2,nowMs());message(out,21,rpc);return out;
}
Bytes videoQualitySettingRpc(uint64_t id,int quality){
    if(!id || quality<1 || quality>6)throw std::invalid_argument("Invalid quality setting");
    Bytes body,header,rpc,out;
    integer(body,1,quality); // gv_pb.VideoQuality
    integer(header,1,id);message(rpc,1,header);message(rpc,4,body);
    integer(out,1,id);integer(out,2,nowMs());message(out,21,rpc);return out;
}
Bytes captureSettingsRpc(uint64_t id,int w,int h,int fps,int quality,int screenId,int customMbps){
    if(!id || screenId<0 || !streamSize(w,h) ||
       !supportedCaptureRate(fps) || quality<1 || quality>6 || customMbps<0 || customMbps>100 ||
       (quality==6 && customMbps==0) || (quality!=6 && customMbps!=0))throw std::invalid_argument("Invalid capture settings");
    Bytes capture,header,rpc,out;
    integer(capture,1,captureRateTier(fps));integer(capture,2,quality);integer(capture,3,1);
    // Preserve the 0.3.25 capture pacing fields; these keep the host frame
    // cadence regular and avoid batching in the decoder queue.
    integer(capture,12,0);
    integer(capture,18,uint64_t(fps));
    // Connect auto-selection (-1) is not a valid online target. Use the
    // current ID advertised in Main.screens and validated against its list.
    integer(capture,4,unsigned(screenId));integer(capture,5,w);integer(capture,6,h);
    // TV wire field 8; mobile custom-state -> capture uses bits/second.
    // Keep chroma settings unchanged and do not treat this request as a
    // guarantee that a static scene will generate the requested rate.
    if(quality==6)integer(capture,8,uint64_t(customMbps)*1000000u);
    integer(header,1,id);message(rpc,1,header);message(rpc,2,capture);
    integer(out,1,id);integer(out,2,nowMs());message(out,21,rpc);return out;
}

Bytes desktopModeRpc(uint64_t id,int screenId,int w,int h,int fps,int quality,int customMbps){
    if(!id || screenId<0 || !streamSize(w,h) || !supportedCaptureRate(fps) || quality<1 || quality>6 ||
       (quality==6 ? customMbps<1 || customMbps>100 : customMbps!=0))throw std::invalid_argument("Invalid desktop mode");
    Bytes capture,header,rpc,out;
    integer(capture,1,captureRateTier(fps));integer(capture,2,quality);integer(capture,3,1);
    integer(capture,4,unsigned(screenId));integer(capture,5,w);integer(capture,6,h); // desktop timing
    if(quality==6)integer(capture,8,uint64_t(customMbps)*1000000u);
    integer(capture,10,4);integer(capture,12,0); // ChooseType_RESOLUTION, manual quality
    integer(capture,14,w);integer(capture,15,h); // encoder limits
    integer(capture,16,w);integer(capture,17,h);integer(capture,18,uint64_t(fps));
    integer(header,1,id);message(rpc,1,header);message(rpc,2,capture);
    integer(out,1,id);integer(out,2,nowMs());message(out,21,rpc);return out;
}

Bytes selectScreenRpc(uint64_t id,int screenId,int fps,int quality,bool hevc,int maxWidth,int maxHeight,int customMbps){
    if(!id || screenId<0 || !supportedCaptureRate(fps) || quality<1 || quality>6 || !streamSize(maxWidth,maxHeight) ||
       (quality==6 ? customMbps<1 || customMbps>100 : customMbps!=0))throw std::invalid_argument("Invalid screen selection");
    Bytes capture,header,rpc,out;
    integer(capture,1,captureRateTier(fps));integer(capture,2,quality);integer(capture,3,1);
    integer(capture,4,unsigned(screenId));
    integer(capture,5,uint64_t(-1));integer(capture,6,uint64_t(-1)); // let the host size it
    if(quality==6)integer(capture,8,uint64_t(customMbps)*1000000u);
    integer(capture,10,1);integer(capture,12,0); // ChooseType_DEFAULT, manual quality
    integer(capture,13,hevc?2:1); // our negotiated track's codec, as the official app sends
    integer(capture,14,maxWidth);integer(capture,15,maxHeight);integer(capture,18,uint64_t(fps));
    integer(header,1,id);message(rpc,1,header);message(rpc,2,capture);
    integer(out,1,id);integer(out,2,nowMs());message(out,21,rpc);return out;
}

Bytes quitSuperScreenRpc(uint64_t id){
    if(!id)throw std::invalid_argument("Invalid request id");
    Bytes header,rpc,out;
    integer(header,1,id);message(rpc,1,header);message(rpc,14,{});
    integer(out,1,id);integer(out,2,nowMs());message(out,21,rpc);return out;
}

Bytes enterSuperScreenRpc(uint64_t id,int w,int h){
    if(!id || !streamSize(w,h))throw std::invalid_argument("Invalid super screen");
    Bytes size,body,header,rpc,out;
    integer(size,1,w);integer(size,2,h);
    integer(body,1,1);message(body,2,size); // REASON_MANUAL
    integer(header,1,id);message(rpc,1,header);message(rpc,26,body);
    integer(out,1,id);integer(out,2,nowMs());message(out,21,rpc);return out;
}

Bytes qualityUpdateRpc(uint64_t id,int fps,int quality,int screenId,int customMbps,int dpiScale){
    if(!id || screenId<0 || !supportedCaptureRate(fps) || quality<1 || quality>6 ||
       customMbps<0 || customMbps>100 || (quality==6&&customMbps==0) ||
       (quality!=6&&customMbps!=0) || dpiScale<0 || dpiScale>1000)throw std::invalid_argument("Invalid quality update");
    Bytes capture,header,rpc,out;
    integer(capture,1,captureRateTier(fps));integer(capture,2,quality);integer(capture,3,1);
    integer(capture,12,0);
    integer(capture,18,uint64_t(fps));
    integer(capture,4,unsigned(screenId));
    // No resolution/max-scale/pixel fields: this request must only update the
    // encoder controls and leave the host capture resolution unchanged.
    if(quality==6)integer(capture,8,uint64_t(customMbps)*1000000u);
    if(dpiScale)integer(capture,9,unsigned(dpiScale));
    integer(header,1,id);message(rpc,1,header);message(rpc,2,capture);
    integer(out,1,id);integer(out,2,nowMs());message(out,21,rpc);return out;
}

Bytes videoScaleRpc(uint64_t id,int sourceWidth,int sourceHeight,
                    int outputWidth,int outputHeight,int fps,int quality,
                    int screenId,int customMbps){
    const bool outputSupported=streamSize(outputWidth,outputHeight);
    if(!id || sourceWidth<1 || sourceHeight<1 || sourceWidth>16384 || sourceHeight>16384 ||
       !outputSupported || screenId<0 || !supportedCaptureRate(fps) || quality<1 || quality>6 ||
       customMbps<0 || customMbps>100 || (quality==6&&customMbps==0) ||
       (quality!=6&&customMbps!=0))
        throw std::invalid_argument("Invalid video scale request");
    Bytes capture,header,rpc,out;
    integer(capture,1,captureRateTier(fps));integer(capture,2,quality);integer(capture,3,1);
    integer(capture,12,0);
    integer(capture,18,uint64_t(fps));
    integer(capture,4,unsigned(screenId));
    // Source/desktop dimensions are intentionally the observed media size, not
    // the selected stream size. The pixel pair is the mobile-style encoded
    // output bound; max_scale is kept as the same bound for hosts that use it.
    integer(capture,5,unsigned(sourceWidth));integer(capture,6,unsigned(sourceHeight));
    if(quality==6)integer(capture,8,uint64_t(customMbps)*1000000u);
    integer(capture,10,3);
    integer(capture,14,unsigned(outputWidth));integer(capture,15,unsigned(outputHeight));
    integer(capture,16,unsigned(outputWidth));integer(capture,17,unsigned(outputHeight));
    integer(header,1,id);message(rpc,1,header);message(rpc,2,capture);
    integer(out,1,id);integer(out,2,nowMs());message(out,21,rpc);return out;
}

namespace {
std::string wireSummary(const uint8_t *data,size_t size,unsigned depth) {
    Reader reader(data,size);Field field;std::string out;unsigned seen=0;
    while(reader.next(field) && ++seen<=128 && out.size()<600) {
        out+=std::to_string(field.key);
        if(field.wire==0) out+="="+std::to_string(field.value);
        else if(field.wire==2) {
            out+="["+std::to_string(field.size)+"]";
            if(depth && field.size && field.size<=8192) {
                const auto nested=wireSummary(field.data,field.size,depth-1);
                if(!nested.empty()) out+="{"+nested+"}";
            }
        } else out+="?";
        out+=",";
    }
    return out;
}
}
std::string screenSourceWireSummary(const uint8_t *data,size_t size) noexcept {
    if(!data || !size || size>65536) return {};
    try { return wireSummary(data,size,2); } catch(...) { return {}; }
}
// Screen.mode / Screen.current: fields 3/4 are the desktop size, 5/6 the
// pixel size when the host scales the desktop.
static bool displayDimensions(const Field &prop,int &width,int &height){
    if(prop.wire!=2)return false;
    Reader r(prop.data,prop.size);Field f;int dims[4]={};
    while(r.next(f))if(f.key>=3 && f.key<=6){
        if(f.wire!=0 || f.value>65535)return false;
        dims[f.key-3]=int(f.value);
    }
    width=dims[2] && dims[3]?dims[2]:dims[0];height=dims[2] && dims[3]?dims[3]:dims[1];
    return r.good();
}
// Screen.dpi_scale (GameViewer gv_pb.DpiScale): 1 current, 2 recommended, 3 the
// scales offered, packed or not.
static bool displayScales(const Field &prop,ScreenSourceInfo &info){
    if(prop.wire!=2)return false;
    Reader r(prop.data,prop.size);Field f;ScreenSourceInfo s;
    auto offer=[&](uint64_t v){if(v && v<=1000 && s.scales.size()<16)s.scales.push_back(int(v));};
    while(r.next(f)){
        if(f.key==1 && f.wire==0 && f.value<=1000)s.scale=int(f.value);
        else if(f.key==2 && f.wire==0 && f.value<=1000)s.recommendedScale=int(f.value);
        else if(f.key==3 && f.wire==0)offer(f.value);
        else if(f.key==3 && f.wire==2){
            uint64_t v=0;unsigned shift=0;
            for(size_t i=0;i<f.size;++i){
                if(shift>63)return false;
                v|=uint64_t(f.data[i]&127)<<shift;shift+=7;
                if(!(f.data[i]&128)){offer(v);v=0;shift=0;}
            }
            if(shift)return false;
        }
    }
    if(!r.good())return false;
    info.scale=s.scale;info.recommendedScale=s.recommendedScale;info.scales=std::move(s.scales);
    return true;
}
std::optional<ScreenSourceInfo> parseScreenSources(const uint8_t *data,size_t size)noexcept{    if(!data || !size || size>65536)return {};
    bool found=false;ScreenSourceInfo result;
    Reader outer(data,size);Field f;
    while(outer.next(f))if(f.key==7){
        if(found || f.wire!=2)return {};found=true;
        std::array<int,32> ids{};unsigned count=0;std::vector<ScreenSourceInfo> screens;std::vector<ScreenEntry> all;
        int current=0;bool currentPresent=false;
        Reader sources(f.data,f.size);Field source;
        while(sources.next(source)){
            if(source.key==1){
                if(source.wire!=2 || count==ids.size())return {};
                int id=0,fps=0;bool idPresent=false;ScreenSourceInfo info;Reader screen(source.data,source.size);Field prop;
                while(screen.next(prop))if(prop.key==1){
                    if(idPresent || prop.wire!=0 || prop.value>uint64_t(INT32_MAX))return {};
                    idPresent=true;id=int(prop.value);
                }else if(prop.key==2 && prop.wire==0 && prop.value<=1000){
                    fps=int(prop.value);
                }else if(prop.key==5 && prop.wire==0 && prop.value<=uint64_t(INT32_MAX)){
                    info.type=int(prop.value);
                }else if(prop.key==3 || prop.key==4){
                    // Only the virtual display mode request reads these: a malformed
                    // size must not cost the screen id the quality RPCs need.
                    int w=0,h=0;
                    if(!displayDimensions(prop,w,h))continue;
                    if(prop.key==4){info.width=w;info.height=h;}
                    else if(w && h && info.modes.size()<64)info.modes.emplace_back(w,h);
                }else if(prop.key==9){
                    // Like the sizes: a bad scale list only hides the scale menu.
                    if(!displayScales(prop,info)){info.scale=info.recommendedScale=0;info.scales.clear();}
                }
                if(!screen.good())return {};
                for(unsigned i=0;i<count;++i)if(ids[i]==id)return {};
                ids[count++]=id;info.screenId=id;all.push_back({id,info.type,info.width,info.height,fps});
                screens.push_back(std::move(info));
            }else if(source.key==2){
                if(currentPresent || source.wire!=0)return {};
                currentPresent=true;
                current=source.value<=uint64_t(INT32_MAX)?int(source.value):-1;
            }
        }
        if(!sources.good())return {};
        result.count=count;
        // Protobuf's omitted int32 ID is zero; it is accepted only when the
        // complete advertised list contains that same ID. Never guess a screen.
        for(auto &s:screens)if(current>=0 && s.screenId==current){result=std::move(s);result.count=count;}
        result.all=std::move(all);
    }
    if(!outer.good() || !found)return {};
    return result;
}

std::optional<CaptureSettingSyncInfo> parseCaptureSettingSync(const uint8_t *data,size_t size)noexcept{
    if(!data || !size || size>65536)return {};
    bool found=false;CaptureSettingSyncInfo result;
    Reader outer(data,size);Field f;
    while(outer.next(f))if(f.key==25){
        if(found || f.wire!=2)return {};found=true;
        Reader body(f.data,f.size);Field v;
        while(body.next(v)){
            if(v.wire!=0)continue;
            switch(v.key){
                case 1: if(v.value>4)return {}; result.fps=int(v.value);result.fields|=1u;break;
                case 2: if(v.value>6)return {}; result.quality=int(v.value);result.fields|=2u;break;
                case 3: if(v.value>4)return {}; result.chroma=int(v.value);break;
                case 4: if(v.value>uint64_t(std::numeric_limits<int64_t>::max()))return {}; result.maxCustomBitrate=int64_t(v.value);result.fields|=16u;break;
                case 5: if(v.value>16384)return {}; result.width=int(v.value);result.fields|=4u;break;
                case 6: if(v.value>16384)return {}; result.height=int(v.value);result.fields|=8u;break;
                case 7: if(v.value>4)return {}; result.resolutionType=int(v.value);break;
                case 8: result.dpiScale=int(v.value);break;
                case 9: if(v.value>16384)return {}; result.pixelWidth=int(v.value);break;
                case 10: if(v.value>16384)return {}; result.pixelHeight=int(v.value);break;
                case 11: result.orientation=int(v.value);break;
                default: break;
            }
        }
        if(!body.good())return {};
    }
    if(!outer.good() || !found)return {};
    if(!result.fields)return {};
    return result;
}

std::optional<CaptureRpcAck> parseCaptureRpcAck(const uint8_t *data,size_t size)noexcept{
    if(!data || !size || size>65536)return {};
    CaptureRpcAck result;bool response=false,header=false,id=false;
    Reader outer(data,size);Field f;
    while(outer.next(f))if(f.key==22){
        if(response || f.wire!=2)return {};response=true;
        Reader rpc(f.data,f.size);Field r;
        while(rpc.next(r)){
            if(r.key==1){
                if(header || r.wire!=2)return {};header=true;
                Reader head(r.data,r.size);Field h;
                while(head.next(h)){if(h.key==1){if(id || h.wire!=0)return {};id=true;result.requestId=h.value;}else ++result.otherFields;}
                if(!head.good())return {};
            }else if(r.key==2){
                if(result.capture || r.wire!=2)return {};result.capture=true;
                Reader capture(r.data,r.size);Field e;
                while(capture.next(e))if(e.key==1){
                    if(e.wire!=2 || ++result.entries>64)return {};
                    Reader error(e.data,e.size);Field v;bool code=false;int entryCode=0;
                    while(error.next(v))if(v.key==1){
                        if(code || v.wire!=0)return {};code=true;
                        entryCode=int(int32_t(uint32_t(v.value)));
                    }
                    if(!error.good())return {};
                    // The repeated Error container also carries code-zero
                    // result records. Check every record, not container size
                    // or only the first record. Proto3's omitted int32 is zero.
                    if(entryCode!=0){
                        if(!result.errors)result.errorCode=entryCode;
                        ++result.errors;
                    }
                }
                if(!capture.good())return {};
            }else if((r.key==10 || r.key==22) && r.wire==2){
                // Quit/EnterSuperScreenResponse{error_code=1}
                result.superScreen=true;
                Reader body(r.data,r.size);Field v;
                while(body.next(v))if(v.key==1 && v.wire==0 && v.value){
                    result.errorCode=int(int32_t(uint32_t(v.value)));++result.errors;
                }
                if(!body.good())return {};
            }else ++result.otherFields;
        }
        if(!rpc.good())return {};
    }
    if(!outer.good() || !response || !header || !id || !result.requestId)return {};
    return result;
}
}
