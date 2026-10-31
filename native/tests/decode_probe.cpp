// SPDX-License-Identifier: GPL-3.0-or-later
#include "media.hpp"
#include "strict_video_receiver.hpp"
#include "config_store.hpp"
#include "av1_sdp.hpp"
#include <fstream>
#include <iostream>
#include <thread>
#include <chrono>

int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "usage: youyou_decode_probe <pack> h264|hevc|av1|av1-loss|auto-h264|auto-hevc software|vaapi\n";
        return 2;
    }
    const std::string codec=argv[2];
    const bool loss=codec=="av1-loss",av1=codec=="av1" || loss;
    const bool autoH264=codec=="auto-h264",autoHevc=codec=="auto-hevc",rtp=av1 || autoH264 || autoHevc;
    if (codec!="h264" && codec!="hevc" && !rtp) return 2;
    if (std::string(argv[3]) != "software" && std::string(argv[3]) != "vaapi") return 2;
    const bool hevc = std::string(argv[2]) == "hevc";
    const bool expectHardware = std::string(argv[3]) == "vaapi";
    if (SDL_Init(SDL_INIT_AUDIO) != 0) return 3;
    bool passed = false;
    {
        uu::ConfigStore::instance().reload();
        uu::Media media;
        media.setYuvOutput(true);
        const uint64_t epoch = media.generation();
        std::ifstream file(argv[1], std::ios::binary);
        uint32_t size = 0;
        int fed = 0, red = 0;
        uint64_t serial = 0;
        bool sawHw = false;
        int delivered=0;
        std::unique_ptr<uu::StrictVideoReceiver> receiver;
        if(rtp){
            const std::string formats=autoH264?"100 101\r\na=rtpmap:100 H264/90000":autoHevc?
                "96 97\r\na=rtpmap:96 H265/90000":"98 99\r\na=rtpmap:98 AV1/90000\r\na=fmtp:98 profile=0";
            auto allowed=uu::av1Negotiation("m=video 9 UDP/TLS/RTP/SAVPF "+formats+"\r\n");
            if(allowed.av1!=av1 || allowed.hevc!=autoHevc || allowed.h264!=autoH264)return 7;
            auto h26x=[&](bool h265,const uint8_t *p,size_t n,uint32_t ts,bool key) {
                if((h265 && !autoHevc) || (!h265 && !autoH264) || ts/3000!=uint32_t(delivered++))
                    throw std::runtime_error("Incorrect H.26x fallback dispatch");
                media.video(p,n,epoch,ts,h265,key);
            };
            receiver=std::make_unique<uu::StrictVideoReceiver>(true,100,
                [](auto...){throw std::runtime_error("Wrong payload type dispatched");},true,false,
                [h26x](const uint8_t *p,size_t n,uint32_t ts,bool key){h26x(true,p,n,ts,key);},
                [&](const uint8_t *p,size_t n,uint32_t ts,bool key){
                    const int index=int(ts/3000);
                    const int expected=loss && delivered>=8?delivered+7:delivered;
                    if(index!=expected || ((index%15==0)!=key))
                        throw std::runtime_error("Frame index "+std::to_string(index)+" vs "+std::to_string(expected)+", recovery="+std::to_string(key));
                    media.video(p,n,epoch,ts,false,key,true);
                    ++delivered;
                },[h26x](const uint8_t *p,size_t n,uint32_t ts,bool key){h26x(false,p,n,ts,key);});
            receiver->setSources(123,456);
            receiver->setNegotiatedCodecs(allowed.av1,allowed.hevc,allowed.h264);
        }
        auto check = [&] {
            const auto frame = media.picture(serial);
            if (!frame.serial) return;
            serial = frame.serial;
            if (frame.width == 1280 && frame.height == 720 && frame.pixels &&
                frame.pixelFormat == SDL_PIXELFORMAT_NV12) {
                const auto &buf = *frame.pixels;
                const size_t uv = 1280 * 720;
                if (buf.size() >= uv + uv / 2 && buf[0] >= 70 && buf[0] <= 94 &&
                    buf[uv] < 125 && buf[uv+1] > 190) ++red;
            }
            sawHw |= media.stats().hardwareFrame;
        };
        while (file.read(reinterpret_cast<char*>(&size), sizeof(size))) {
            if (!size || size > 1000000) return 4;
            std::vector<uint8_t> data(size);
            if (!file.read(reinterpret_cast<char*>(data.data()), size)) return 5;
            if(rtp){
                if(size<12)return 4;
                auto packet=std::make_shared<rtc::Message>(size);
                std::memcpy(packet->data(),data.data(),size);
                rtc::message_vector packets{packet};
                receiver->incoming(packets,[](auto){});
                if(!(data[1]&128))continue;
                ++fed;
            }else media.video(data.data(), data.size(), epoch, uint32_t(fed++ * 3000), hevc);
            check();
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
        for (int i=0;i<12;++i) { check(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        const auto m = media.stats();
        std::cout << "codec=" << m.codec << " decoder=" << m.decoder << " hw=" << sawHw
                  << " input=" << fed << " published=" << m.publishedTotal
                  << " uniqueRedNV12=" << red << " transfer=" << m.cpuTransfers
                  << " decodeErrors=" << m.decodeErrors << " status=" << media.status() << std::endl;
        passed = fed >= 30 && red > 10 && m.decodeErrors == 0 &&
                 sawHw == expectHardware && (m.cpuTransfers > 0) == expectHardware;
        if(rtp) {
            const auto r=receiver->snapshot();
            std::cout<<"rtp="<<r.accepted<<" rtx="<<r.rtxRecovered<<" dropped="<<r.dropped<<"\n";
            const std::string expectedCodec=av1?"AV1":autoHevc?"H.265":"H.264";
            passed=passed && m.codec.rfind(expectedCodec,0)==0 && r.accepted==uint64_t(loss?35:42) &&
                   r.rtxRecovered==uint64_t(av1) && (r.dropped>0)==loss && m.publishedTotal==r.accepted;
        }
    }
    SDL_Quit();
    return passed ? 0 : 6;
}
