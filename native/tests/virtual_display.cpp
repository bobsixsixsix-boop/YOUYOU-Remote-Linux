#include "quality_rpc.hpp"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
using namespace uu;
namespace {
void check(bool ok,const char *why){if(!ok)throw std::runtime_error(why);}
bool ends(const Bytes &v,const Bytes &tail){return v.size()>=tail.size() && std::equal(tail.rbegin(),tail.rend(),v.rbegin());}
using Op=VirtualDisplays::Op;
HostScreen screen(int id,int type){
    HostScreen s;s.id=id;s.type=type;s.name=type?"Remote Monitor":"Display";
    s.width=3040;s.height=1904;s.fps=120;return s;
}
void wire(){
    auto native=screen(19,1);
    const Bytes native1080{0x50,3,0x70,0x80,0x0f,0x78,0xb8,0x08,0x80,1,0xbc,0x0d,0x88,1,0xb8,0x08};
    check(ends(videoScaleRpc(5,3040,1904,1724,1080,120,3,19,0,&native),native1080),
          "native 1080 tier preserves the smaller pixel limit and follows the desktop");
    const Bytes strict1080{0x50,3,0x70,0xbc,0x0d,0x78,0xb8,0x08,0x80,1,0xbc,0x0d,0x88,1,0xb8,0x08};
    check(ends(videoScaleRpc(5,3040,1904,1724,1080,120,3,2,0,&native),strict1080),
          "a different captured screen cannot widen the encoder envelope");
    check(native.encoderBounds(1150,720)==std::make_pair(1280,720),"tablet 720 tier");
    check(native.encoderBounds(2280,1428)==std::make_pair(2280,1428),"non-standard targets retain their bounds");
    check(native.encoderBounds(1600,1080)==std::make_pair(1600,1080),"a different aspect cannot widen the limit");
    std::swap(native.width,native.height);
    check(native.encoderBounds(1080,1724)==std::make_pair(1080,1920),"portrait tablet tier");
    native.type=0;
    check(native.encoderBounds(1080,1724)==std::make_pair(1080,1724),"physical desktop keeps strict bounds");
    check(ends(virtualDisplayRpc(42,Op::Create,-1,3040,1904),
               {0x62,10,0x0a,6,0x08,0xe0,0x17,0x10,0xf0,0x0e,0x10,1}),"create one selected mode");
    check(ends(virtualDisplayRpc(42,Op::Create,-1,3040,1904,0,0,0,{{1520,952},{1724,1080},{3040,1904},{1520,952}}),
               {0x62,26,0x0a,6,0x08,0xe0,0x17,0x10,0xf0,0x0e,0x0a,6,0x08,0xf0,0x0b,0x10,0xb8,0x07,
                0x0a,6,0x08,0xbc,0x0d,0x10,0xb8,0x08,0x10,1}),"one monitor with distinct tablet resolutions");
    check(ends(virtualDisplayRpc(42,Op::Remove,19,3040,1904),{0x6a,3,0x0a,1,19}),"remove exact packed ID");
    check(ends(virtualDisplayRpc(42,Op::Select,19,3040,1904),{0x12,2,0x20,19}),"physical select only");
    check(ends(virtualDisplayRpc(42,Op::Select,19,1520,952,120,6,100),{0x12,2,0x20,19}),
          "selecting an existing owned screen must never change the Windows display mode");
    check(ends(virtualDisplayRpc(42,Op::Initialize,19,3040,1904,120,6,100),
               {0x20,19,0x08,4,0x10,6,0x18,1,0x28,0xe0,0x17,0x30,0xf0,0x0e,0x40,0x80,0xc2,0xd7,0x2f,
                0x50,4,0x60,0,0x70,0xe0,0x17,0x78,0xf0,0x0e,0x80,1,0xe0,0x17,0x88,1,0xf0,0x0e,0x90,1,120}),
               "owned virtual mode preserves selected capture quality, bitrate, scale and pixel dimensions");
    const Bytes ok{0xb2,1,6,0x0a,2,8,42,0x42,0};
    const auto ack=parseVirtualDisplayAck(ok.data(),ok.size());
    check(ack && ack->create && ack->id==42 && !ack->code,"typed create ACK");
    const Bytes bare{0xb2,1,4,0x0a,2,8,42};
    check(!parseVirtualDisplayAck(bare.data(),bare.size()),"header is not creation proof");
    const Bytes topology{0x3a,20,0x0a,0,0x0a,14,0x08,19,0x10,120,0x22,6,0x18,0xe0,0x17,0x20,0xf0,0x0e,0x28,1,0x10,19};
    const auto info=parseScreenSources(topology.data(),topology.size());
    check(info && info->screenId==19 && info->screens.size()==2 && info->screens[0].id==0 &&
          info->screens[1].type==1 && info->screens[1].supports(3040,1904) && info->screens[1].fps==120,"authoritative screens");
}
void lifecycle(){
    auto a=screen(0,0),b=screen(2,0),v=screen(19,1),other=screen(22,1);
    VirtualDisplays d;
    auto begin=[&](Op op,int target){return d.begin(op,42,target,3040,1904,120,0);};
    d.observe({a,b},0);check(d.view.canCreate() && begin(Op::Create,-1),"create from known physical baseline");
    check(!begin(Op::Create,-1),"serialize create");
    d.observe({a,b,v},19);check(d.view.owned<0 && !d.ack(41,Op::Create,0),"list alone or mismatched ACK");
    d.ack(42,Op::Create,0);
    check(d.view.owned==19 && !d.view.canRemove() && !begin(Op::Remove,0),"active/physical protected");
    check(!d.beginInitialMode(43,0),"correct desktop timing needs no correction");
    check(begin(Op::Select,0),"switch physical");
    d.ack(42,Op::Select,0);check(d.view.pending,"switch needs topology confirmation");
    d.observe({a,b,v},0);check(!d.view.pending && d.view.canRemove(),"physical restored");
    check(!begin(Op::Remove,2) && begin(Op::Remove,19),"remove owned only");
    d.ack(42,Op::Remove,0);check(d.view.owned==19 && d.view.pending,"wait for actual removal");
    d.observe({a,b},0);check(d.view.owned<0 && !d.view.pending,"confirmed removal");
    check(begin(Op::Create,-1),"second explicit create");
    d.ack(42,Op::Create,0);d.observe({a,b,v,other},0);
    check(d.view.owned<0 && !d.view.canRemove(),"concurrent additions never claimed");
    d=VirtualDisplays{};d.observe({a,b,v},0);
    check(!d.view.canCreate() && !d.view.canRemove() && !begin(Op::Remove,19),"existing/orphan never claimed");
    d=VirtualDisplays{};d.observe({a,b},0);begin(Op::Create,-1);d.expire(11000);
    d.ack(42,Op::Create,0);d.observe({a,b,v},0);
    check(d.view.owned<0 && d.view.createUncertain && !d.view.canRemove(),"late ACK after timeout");
    d=VirtualDisplays{};d.observe({a,b},0);d.begin(Op::Create,42,-1,1600,900,120,0);
    v.width=2560;v.height=1440;v.fps=144;v.modes.emplace_back(1600,900);
    d.ack(42,Op::Create,0);d.observe({a,b,v},0);
    check(!d.beginInitialMode(43,0),"never change current physical screen upon creation");
    d.observe({a,b,v},19);
    check(!d.beginInitialMode(43,0) && !d.begin(Op::Select,43,19,1600,900,120,0),
          "a later switch to the virtual screen cannot initialize or reconfigure it");
    d=VirtualDisplays{};d.observe({a,b},0);d.begin(Op::Create,42,-1,1600,900,120,0);
    d.ack(42,Op::Create,0);d.observe({a,b,v},19);
    const auto mode=d.beginInitialMode(43,0);
    check(mode && mode->id==19 && mode->w==1600 && mode->h==900 && mode->fps==120 && !d.beginInitialMode(44,0),
          "one automatic native-mode correction on the newly selected virtual screen");
    d.ackSelection(43,0);d.observe({a,b,v},19);check(d.view.pending,"wrong dimensions cannot confirm correction");
    v.width=1600;v.height=900;d.observe({a,b,v},0);
    check(d.view.pending,"correct dimensions cannot confirm the wrong current ID");
    d.observe({a,b,v},19);
    check(d.view.capturesOwnedSize(1600,900) && !d.view.capturesOwnedSize(3040,1904),"confirmed current capture timing");
    check(!d.view.pending && !d.expire(11000) && d.view.find(19)->fps==144,"1600x900/120 FPS confirmed on actual 144 Hz display");
    check(!d.begin(Op::Select,42,19,1600,900,120,0),"refresh mismatch alone cannot retry a matched size");
    check(!d.begin(Op::Select,43,19,1520,952,120,0) && !d.begin(Op::Initialize,43,19,1520,952,120,0),
          "current owned screen rejects any later resize including an unsupported half size");
    d.observe({a,b,v},0);
    check(d.begin(Op::Select,43,19,1520,952,120,0),"selecting an existing virtual screen only changes capture");
    d.ackSelection(43,0);d.observe({a,b,v},19);
    check(!d.view.pending && !d.beginInitialMode(44,0) && d.view.find(19)->hasSize(1600,900),
          "selection succeeds at the previously reported desktop timing");
    d=VirtualDisplays{};d.observe({a,b},0);d.begin(Op::Create,42,-1,1600,900,120,0);
    d.ack(42,Op::Create,0);d.observe({a,b,v},19);
    check(d.view.owned==19 && !d.beginInitialMode(43,0),"matched size avoids automatic retry even at 144 Hz");
    d=VirtualDisplays{};d.observe({a,b},0);d.begin(Op::Create,42,-1,1520,952,120,0);
    d.ack(42,Op::Create,0);d.observe({a,b,v},19);
    check(!d.beginInitialMode(43,0),"must not initialize a desktop mode absent from the host's list");
}
}
int main(){
    try{wire();lifecycle();std::puts("PASS display wire, ownership, mode, acknowledgements, reconnection");}
    catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
