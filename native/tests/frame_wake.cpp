// SPDX-License-Identifier: GPL-3.0-or-later
#include "media.hpp"
#include "config_store.hpp"
#include <SDL.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace {
void check(bool ok,const char* message) {
    if(!ok)throw std::runtime_error(message);
}
struct TemporaryConfig {
    std::filesystem::path previous=std::filesystem::current_path(),directory;
    TemporaryConfig() {
        char name[]="/tmp/youyou-frame-wake-XXXXXX";
        check(mkdtemp(name)!=nullptr,"temporary configuration");
        directory=name;std::filesystem::current_path(directory);
        std::ofstream("config.ini")<<"decoder=software\nframe_pacing=1\nplayout_delay_ms=0\nfps=120\n";
        uu::ConfigStore::instance().reload();
    }
    ~TemporaryConfig() {
        std::error_code ec;
        std::filesystem::current_path(previous,ec);
        std::filesystem::remove_all(directory,ec);
    }
};
struct FilterState {
    uu::Media* media=nullptr;
    Uint32 type=0,window=0;
    std::atomic<int> mode{0},calls{0},reentries{0};
    std::atomic<bool> badPayload{false};
};
int SDLCALL filter(void* opaque,SDL_Event* event) {
    auto& state=*static_cast<FilterState*>(opaque);
    if(event->type!=state.type)return 1;
    ++state.calls;
    if(event->user.windowID!=state.window||event->user.code||event->user.data1||event->user.data2)
        state.badPayload=true;
    if(state.mode==1) {
        // Would deadlock if the decoder invoked SDL_PushEvent under Media.mutex.
        (void)state.media->stats();
        (void)state.media->picture(0);
        ++state.reentries;
    }
    return state.mode==2?0:1;
}
int queued(Uint32 type,Uint32 window) {
    SDL_Event events[4]{};
    const int count=SDL_PeepEvents(events,4,SDL_PEEKEVENT,type,type);
    check(count>=0,"inspect private event queue");
    for(int i=0;i<count;++i)
        check(events[i].type==type&&events[i].user.windowID==window&&!events[i].user.code&&
              !events[i].user.data1&&!events[i].user.data2,"wake event contains an owned/pointer payload");
    return count;
}
void take(Uint32 type) {
    SDL_Event event{};
    check(SDL_PeepEvents(&event,1,SDL_GETEVENT,type,type)==1,"consume one frame-ready event");
}
void publish(uu::Media& media) {
    static uint32_t timestamp=90000;
    constexpr uint8_t packet[]={
#include "../test_frame.inc"
    };
    const auto before=media.stats().publishedTotal;
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    while(std::chrono::steady_clock::now()<until) {
        media.video(packet,sizeof(packet),media.generation(),timestamp,false,true,false);
        timestamp+=750;
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        if(media.stats().publishedTotal>before) {
            // Publication precedes notify; let that same worker finish the hint.
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            return;
        }
    }
    throw std::runtime_error("synthetic publication timed out");
}
struct SdlCleanup {
    ~SdlCleanup(){SDL_SetEventFilter(nullptr,nullptr);SDL_Quit();}
};
struct FilterCleanup {
    ~FilterCleanup(){SDL_SetEventFilter(nullptr,nullptr);}
};
}

int main() {
    alarm(8); // Fail deterministically if a callback regresses into a deadlock.
    try {
        TemporaryConfig temporary;
        setenv("SDL_VIDEODRIVER","dummy",1);setenv("SDL_AUDIODRIVER","dummy",1);
        setenv("UU_DECODER","software",1);
        check(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO)==0,"initialize isolated SDL queue");
        SdlCleanup cleanup;
        const Uint32 type=SDL_RegisterEvents(1),window=71;
        check(type!=Uint32(-1),"allocate private frame-ready event");
        FilterState state;
        auto media=std::make_unique<uu::Media>();
        media->setYuvOutput(true);media->setTargetFps(120);media->setPlayoutDelay(0);
        state.media=media.get();state.type=type;state.window=window;
        FilterCleanup filterCleanup;
        SDL_SetEventFilter(filter,&state);
        media->setFrameWake(type,window);
        publish(*media);
        check(queued(type,window)==1,"publication did not wake SDL");
        // Repeated polling acknowledges the hint, but without event pumping the
        // worker must coalesce notifications into the existing private event.
        for(int i=0;i<6;++i) {
            (void)media->picture(0);publish(*media);
            check(queued(type,window)==1,"unconsumed frame notifications accumulated");
        }
        take(type);(void)media->picture(0);publish(*media);
        check(queued(type,window)==1,"picture acknowledgement did not allow the next wake");
        // Re-registering the same renderer on reset must keep the queued hint.
        media->setFrameWake(type,window);
        check(queued(type,window)==1,"same registration erased a pending wake");
        take(type);(void)media->picture(0);
        state.mode=1;publish(*media);
        check(state.reentries>0&&queued(type,window)==1,"SDL filter could not reenter stats and picture");
        take(type);(void)media->picture(0);
        state.mode=2;
        const int rejectedBefore=state.calls;
        publish(*media);publish(*media);
        check(state.calls>=rejectedBefore+2&&queued(type,window)==0,"filtered event prevented a later publication retry");
        state.mode=0;publish(*media);
        check(queued(type,window)==1,"notification did not recover after filter rejection");
        const auto generation=media->generation();
        media->reset();
        check(media->generation()!=generation,"reset did not advance the media generation");
        check(queued(type,window)==1,"reset erased the existing safe pointerless hint");
        take(type);publish(*media);
        check(queued(type,window)==1,"new generation remained stuck behind an old pending bit");
        media->setFrameWake(0,0);
        check(queued(type,window)==0,"disable did not flush its private event");
        publish(*media);
        check(queued(type,window)==0,"disabled registration published a new event");
        media->setFrameWake(type,window);publish(*media);
        check(queued(type,window)==1,"registration did not resume after disable");
        check(!state.badPayload&&media->stats().decodeErrors==0,"invalid payload or synthetic decode error");
        media.reset();
        check(queued(type,window)==0,"Media destruction left a private event queued");
        SDL_SetEventFilter(nullptr,nullptr);
        alarm(0);
        std::puts("PASS publication wake, bounded queue, ack, callback reentry/retry, generation, disable and destruction");
        return 0;
    }catch(const std::exception& error) {
        std::fprintf(stderr,"%s\n",error.what());return 1;
    }
}
