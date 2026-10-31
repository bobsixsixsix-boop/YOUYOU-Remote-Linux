// Included inside namespace uu in the pinned quality_rpc.cpp, sharing its
// bounded protobuf reader, integer/message encoders and sequence convention.
Bytes virtualDisplayRpc(uint64_t id,VirtualDisplays::Op op,int target,int w,int h,int ownedModeFps,int quality,int customMbps,
                        const std::vector<std::pair<int,int>> &panelModes) {
    if(!id || (op!=VirtualDisplays::Op::Create && target<0))throw std::invalid_argument("Invalid display request");
    Bytes body,header,rpc,out;unsigned method;
    if(op==VirtualDisplays::Op::Create){
        if(!streamSize(w,h) || panelModes.size()>6)throw std::invalid_argument("Invalid display modes");
        std::vector<std::pair<int,int>> sizes{{w,h}};
        for(const auto &size:panelModes)
            if(streamSize(size.first,size.second) && std::find(sizes.begin(),sizes.end(),size)==sizes.end())
                sizes.push_back(size);
        for(const auto &size:sizes){
            Bytes mode;integer(mode,1,size.first);integer(mode,2,size.second);message(body,1,mode);
        }
        integer(body,2,1);method=12;
    }else if(op==VirtualDisplays::Op::Remove){
        Bytes ids;var(ids,uint64_t(target));message(body,1,ids);method=13; // packed repeated Screen.id
    }else{
        // Ordinary screen selection carries only screen_id. The native mode
        // change is reserved for a positively identified session-owned monitor.
        integer(body,4,uint64_t(target));method=2;
        if(op==VirtualDisplays::Op::Initialize){
            if(!streamSize(w,h) || !supportedCaptureRate(ownedModeFps) || quality<1 || quality>6 ||
               (quality==6 ? customMbps<1 || customMbps>100 : customMbps!=0))
                throw std::invalid_argument("Invalid virtual display capture mode");
            integer(body,1,captureRateTier(ownedModeFps));integer(body,2,quality);integer(body,3,1);
            integer(body,5,w);integer(body,6,h); // desktop timing, session-owned monitor only
            if(quality==6)integer(body,8,uint64_t(customMbps)*1000000u);
            integer(body,10,4);integer(body,12,0); // ChooseType_RESOLUTION, manual quality
            integer(body,14,w);integer(body,15,h); // encoder limits
            integer(body,16,w);integer(body,17,h);integer(body,18,ownedModeFps);
        }
    }
    integer(header,1,id);message(rpc,1,header);message(rpc,method,body);
    integer(out,1,id);integer(out,2,nowMs());message(out,21,rpc);return out;
}

std::optional<DisplayRpcAck> parseVirtualDisplayAck(const uint8_t *data,size_t size) noexcept {
    if(!data || !size || size>65536)return {};
    DisplayRpcAck a;bool response=false,header=false,body=false;
    Reader outer(data,size);Field f;
    while(outer.next(f))if(f.key==22){
        if(response || f.wire!=2)return {};response=true;
        Reader rpc(f.data,f.size);Field r;
        while(rpc.next(r)){
            if(r.key==1){
                if(header || r.wire!=2)return {};header=true;
                Reader h(r.data,r.size);Field id;
                while(h.next(id))if(id.key==1){if(a.id || id.wire!=0 || !id.value)return {};a.id=id.value;}
                if(!h.good())return {};
            }else if(r.key==8 || r.key==9){
                if(body || r.wire!=2)return {};body=true;a.create=r.key==8;
                Reader b(r.data,r.size);Field v;bool code=false;
                while(b.next(v))if(v.key==1){
                    if(code || v.wire!=0)return {};code=true;a.code=int32_t(uint32_t(v.value));
                }
                if(!b.good())return {};
            }else return {}; // another response oneof cannot confirm our request
        }
        if(!rpc.good())return {};
    }
    if(!outer.good() || !response || !header || !a.id || !body)return {};
    return a;
}

bool displayDimensions(const Field &prop,int &width,int &height) {
    if(prop.wire!=2)return false;
    Reader r(prop.data,prop.size);Field f;int dims[4]={};
    while(r.next(f))if(f.key>=3 && f.key<=6){
        if(f.wire!=0 || f.value>65535)return false;
        dims[f.key-3]=int(f.value);
    }
    width=dims[2] && dims[3]?dims[2]:dims[0];height=dims[2] && dims[3]?dims[3]:dims[1];
    return r.good();
}
