// AV1 RTP aggregation (AOM AV1 RTP 1.0, sections 4.3 and 5). One instance
// consumes an already reordered, contiguous temporal unit, through the marker.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace uu {
class Av1RtpUnit {
    static constexpr size_t limit=4*1024*1024;
    std::vector<uint8_t> fragment_;
    bool packets_=false,frame_=false,reduced_=false,key_=false;
    std::vector<uint8_t> &sequence_;

    static bool sizeField(const uint8_t *&p,const uint8_t *end,size_t &size) {
        uint64_t value=0;
        for(unsigned i=0;i<8 && p<end;++i){
            uint8_t b=*p++;
            value|=uint64_t(b&127)<<(7*i);
            if(!(b&128)){if(value>limit)return false;size=size_t(value);return true;}
        }
        return false;
    }
    bool obu(const uint8_t *p,size_t n,std::vector<uint8_t> &out) {
        if(!n || (p[0]&0x81))return false;
        const uint8_t header=*p++;--n;
        const bool extension=(header&4)!=0;
        uint8_t ext=0;
        if(extension){if(!n || (*p&7))return false;ext=*p++;--n;}
        if(header&2){
            auto end=p+n;size_t declared=0;
            if(!sizeField(p,end,declared) || declared!=size_t(end-p))return false;
            n=declared;
        }
        const uint8_t type=(header>>3)&15;
        if(type==2 || type==8 || type==15)return true; // delimiter, tile list, padding
        if(!n || n>limit || out.size()+n+10>limit)return false;
        const auto start=out.size();
        if(type==1)reduced_=(p[0]&8)!=0;
        if((type==3 || type==6) && !frame_){
            frame_=true;
            // show_existing_frame=0, frame_type=KEY_FRAME, show_frame=1.
            key_=reduced_ || (p[0]&0xf0)==0x10;
        }
        out.push_back(header|2);
        if(extension)out.push_back(ext);
        for(size_t size=n;;size>>=7){
            out.push_back(uint8_t(size&127)|(size>=128?128:0));
            if(size<128)break;
        }
        out.insert(out.end(),p,p+n);
        if(type==1)sequence_.assign(out.begin()+start,out.end());
        return true;
    }
public:
    Av1RtpUnit(const std::vector<uint8_t> &cached,std::vector<uint8_t> &sequence):sequence_(sequence) {
        if(cached.empty())return;
        const uint8_t *end=cached.data()+cached.size(),*p=cached.data()+1+bool(cached[0]&4);
        size_t n=0;
        if(p<end && sizeField(p,end,n) && n && n==size_t(end-p))reduced_=(p[0]&8)!=0;
    }
    // N marks a new coded video sequence, not every keyframe. A repeated
    // sequence header or a shown KEY_FRAME can also anchor loss recovery.
    static bool startsSequence(const uint8_t *p,size_t n) {
        if(n<2 || (p[0]&128))return false;
        if(p[0]&8)return true;
        const auto count=(*p>>4)&3;
        const auto *end=p+n;++p;
        size_t elem=size_t(end-p);
        if(count!=1 && (!sizeField(p,end,elem) || elem>size_t(end-p)))return false;
        if(!elem)return false;
        end=p+elem;
        uint8_t header=*p++,type=(header>>3)&15;
        if(type==1)return true;
        if(type!=3 && type!=6)return false;
        if(header&4){if(p==end)return false;++p;}
        if((header&2) && !sizeField(p,end,elem))return false;
        return p<end && (p[0]&0xf0)==0x10;
    }
    bool append(const uint8_t *p,size_t n,std::vector<uint8_t> &out) {
        if(n<2)return false;
        const uint8_t header=*p++;
        const bool z=header&128,y=header&64,newSequence=header&8;
        if(z!=!fragment_.empty() || (newSequence && (z || packets_)))return false;
        packets_=true;
        const unsigned count=(header>>4)&3;
        const uint8_t *end=p+n-1;
        unsigned index=0;
        while(p<end){
            ++index;
            size_t bytes=size_t(end-p);
            if(count==0 || index!=count){
                if(!sizeField(p,end,bytes) || bytes>size_t(end-p))return false;
            }
            if(!bytes || (count && index>count))return false;
            const bool unfinished=y && bytes==size_t(end-p);
            if((index==1 && z) || unfinished){
                if(fragment_.size()+bytes>limit)return false;
                fragment_.insert(fragment_.end(),p,p+bytes);
                if(!unfinished){
                    if(!obu(fragment_.data(),fragment_.size(),out))return false;
                    fragment_.clear();
                }
            }else if(!obu(p,bytes,out))return false;
            p+=bytes;
        }
        return !count || count==index;
    }
    bool complete() const {return packets_ && fragment_.empty();}
    bool recovery() const {return frame_ && key_;}
};
}
