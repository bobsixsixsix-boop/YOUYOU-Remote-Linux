#pragma once
#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>

namespace uu {
struct Av1Negotiation { bool av1=false,hevc=false,h264=false; };
inline Av1Negotiation av1Negotiation(const std::string &sdp) {
    bool video=false,av1Listed=false,hevcListed=false,h264Listed=false;
    bool av1Map=false,hevcMap=false,h264Map=false,av1Params=true,hevcParams=true,h264Params=true;
    std::istringstream lines(sdp);std::string line;
    while(std::getline(lines,line)){
        if(!line.empty() && line.back()=='\r')line.pop_back();
        if(line.rfind("m=",0)==0){
            if(video)break;
            std::istringstream m(line);std::string kind,port,proto;int pt;
            m>>kind>>port>>proto;video=kind=="m=video" && port!="0";
            if(video)while(m>>pt){av1Listed|=pt==98;hevcListed|=pt==96;h264Listed|=pt==100;}
        }
        if(!video)continue;
        std::transform(line.begin(),line.end(),line.begin(),[](unsigned char c){return char(std::tolower(c));});
        if(line=="a=rtpmap:98 av1/90000")av1Map=true;
        if(line=="a=rtpmap:96 h265/90000")hevcMap=true;
        if(line=="a=rtpmap:100 h264/90000")h264Map=true;
        const bool av1=line.rfind("a=fmtp:98 ",0)==0;
        const bool hevc=line.rfind("a=fmtp:96 ",0)==0,h264=line.rfind("a=fmtp:100 ",0)==0;
        if(!av1 && !hevc && !h264)continue;
        std::istringstream params(line.substr(line.find(' ')+1));std::string kv;
        while(std::getline(params,kv,';')){
            kv.erase(std::remove_if(kv.begin(),kv.end(),[](unsigned char c){return std::isspace(c);}),kv.end());
            auto eq=kv.find('=');
            if(eq==std::string::npos)continue;
            auto k=kv.substr(0,eq),v=kv.substr(eq+1);
            if(av1 && k=="profile" && v!="0")av1Params=false;
            if(hevc && ((k=="profile-id" && v!="1") || (k=="tx-mode" && v!="srst") ||
                        (k=="sprop-max-don-diff" && v!="0")))hevcParams=false;
            if(h264 && k=="packetization-mode" && v!="0" && v!="1")h264Params=false;
        }
    }
    return {av1Listed && av1Map && av1Params,hevcListed && hevcMap && hevcParams,h264Listed && h264Map && h264Params};
}
}
