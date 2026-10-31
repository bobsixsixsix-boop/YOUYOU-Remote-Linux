#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace uu {
// Values reported by GameViewer's ScreenSources. Windows display IDs are
// connection-scoped and may be reused after a disconnect or topology change.
struct HostScreen {
    int id=0,type=0,width=0,height=0,fps=0;
    std::string name;
    std::vector<std::pair<int,int>> modes;
    bool identity(const HostScreen &s) const {return id==s.id && type==s.type && name==s.name;}
    bool hasSize(int w,int h) const {return width==w && height==h;}
    // Windows UU selects a standard encoder tier from max_scale. Widen that
    // envelope only when fitting this desktop into it yields the exact target
    // after rounding to even video pixels. The pixel limit stays unchanged.
    std::pair<int,int> encoderBounds(int w,int h) const {
        const std::pair<int,int> exact{w,h};
        if(type!=1 || w<=0 || h<=0 || width<=w || height<=h || (width<height)!=(w<h))return exact;
        const int shortEdge=std::min(w,h),longEdge=std::max(w,h);
        const int tier=shortEdge==720?1280:shortEdge==1080?1920:shortEdge==1440?2560:0;
        if(longEdge>=tier)return exact;
        const int shortSource=std::min(width,height),longSource=std::max(width,height);
        const auto fittedLong=2*((int64_t(longSource)*shortEdge+shortSource)/(2*int64_t(shortSource)));
        if(fittedLong!=longEdge)return exact;
        return w<h?std::make_pair(shortEdge,tier):std::make_pair(tier,shortEdge);
    }
    bool supports(int w,int h) const {
        return hasSize(w,h) || std::find(modes.begin(),modes.end(),std::make_pair(w,h))!=modes.end();
    }
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(HostScreen,id,type,width,height,fps,name,modes)
struct DisplayView {
    std::vector<HostScreen> screens;
    int current=-1,owned=-1;
    uint64_t session=0;
    bool known=false,pending=false,createUncertain=false;
    std::string notice;
    const HostScreen *find(int id) const {
        const auto it=std::find_if(screens.begin(),screens.end(),[id](const auto &s){return s.id==id;});
        return it==screens.end()?nullptr:&*it;
    }
    bool capturesOwnedSize(int w,int h) const {
        const auto *screen=find(owned);
        return !pending && current==owned && screen && screen->type==1 && screen->hasSize(w,h);
    }
    bool canCreate() const {
        return known && find(current) && !pending && !createUncertain && owned<0 &&
               std::none_of(screens.begin(),screens.end(),[](const auto &s){return s.type!=0;});
    }
    bool canRemove() const {
        const auto *s=find(owned);
        return !pending && s && s->type==1 && find(current) && current!=owned;
    }
};

class VirtualDisplays {
public:
    enum class Op {Create,Select,Remove,Initialize};
    DisplayView view;
    struct Pending {
        Op op;uint64_t id;int target,w,h,fps;int64_t until;
        std::vector<HostScreen> before;
        bool ack=false,updated=false,ambiguous=false;
        std::optional<HostScreen> added;
    };
    struct InitialMode {int id,w,h,fps;};
    // A create RPC can move capture onto the newly attached Windows monitor
    // without honoring its requested desktop size. Consume the correction
    // once, only when the same monitor is still owned and currently captured.
    std::optional<InitialMode> beginInitialMode(uint64_t id,int64_t now) {
        const auto mode=initialMode_;
        if(!mode)return {};
        if(!begin(Op::Initialize,id,mode->id,mode->w,mode->h,mode->fps,now)){
            initialMode_.reset();return {};
        }
        return mode;
    }
    bool begin(Op op,uint64_t id,int target,int w,int h,int fps,int64_t now) {
        if(request_ || !view.known || !view.find(view.current))return false;
        if(op==Op::Create && !view.canCreate())return false;
        if(op==Op::Select && (!view.find(target) || target==view.current))return false;
        if(op==Op::Initialize){
            const auto *s=view.find(target);
            if(!initialMode_ || initialMode_->id!=target || initialMode_->w!=w || initialMode_->h!=h ||
               initialMode_->fps!=fps || target!=view.current || target!=view.owned || !s || s->type!=1 ||
               !s->supports(w,h) || s->hasSize(w,h))return false;
        }
        if(op==Op::Remove && (!view.canRemove() || target!=view.owned))return false;
        initialMode_.reset();
        request_=Pending{op,id,target,w,h,fps,now+10000,view.screens};
        view.pending=true;view.notice="等待电脑确认";
        return true;
    }
    bool matching(uint64_t id,Op op) const {return request_ && request_->id==id && request_->op==op;}
    bool ackSelection(uint64_t id,int error){
        if(!request_ || (request_->op!=Op::Select && request_->op!=Op::Initialize))return false;
        return ack(id,request_->op,error);
    }
    bool ack(uint64_t id,Op op,int error) {
        if(!matching(id,op))return false;
        if(error)finish("电脑拒绝请求（"+std::to_string(error)+"）");
        else {request_->ack=true;reconcile();}
        return true;
    }
    void observe(std::vector<HostScreen> screens,int current) {
        std::optional<HostScreen> old;
        if(const auto *owned=view.find(view.owned))old=*owned;
        view.known=true;view.current=current;view.screens=std::move(screens);
        if(old && (!view.find(old->id) || !old->identity(*view.find(old->id)))){
            initialMode_.reset();view.owned=-1;view.notice="已失去原虚拟屏标识";
        }
        if(!request_)return;
        auto &p=*request_;p.updated=true;
        if(p.op==Op::Create){
            const auto preserves=[&](const HostScreen &s){auto *v=view.find(s.id);return v && s.identity(*v);};
            if(!std::all_of(p.before.begin(),p.before.end(),preserves) || screensChangedAmbiguously(p))p.ambiguous=true;
        }
        reconcile();
    }
    bool expire(int64_t now) {
        if(!request_ || now<request_->until)return false;
        if(request_->op==Op::Create)view.createUncertain=true;
        finish("电脑未确认，请核对屏幕列表");return true;
    }
    void sendFailed(){
        if(request_ && request_->op==Op::Create)view.createUncertain=true;
        finish("发送失败，请核对电脑状态");
    }
private:
    std::optional<InitialMode> initialMode_;
    std::optional<Pending> request_;
    void finish(std::string notice){request_.reset();view.pending=false;view.notice=std::move(notice);}
    bool screensChangedAmbiguously(Pending &p) {
        if(view.screens.size()==p.before.size())return p.added.has_value();
        if(view.screens.size()!=p.before.size()+1)return true;
        for(const auto &s:view.screens){
            if(std::any_of(p.before.begin(),p.before.end(),[&](const auto &old){return old.id==s.id;}))continue;
            if(s.type!=1 || (p.added && !s.identity(*p.added)))return true;
            p.added=s;return false;
        }
        return true;
    }
    void reconcile() {
        auto &p=*request_;
        if(!p.ack || !p.updated)return;
        if(p.op==Op::Create){
            if(p.ambiguous){view.createUncertain=true;finish("屏幕列表有其他变化，未认领虚拟屏");return;}
            if(!p.added || !p.added->supports(p.w,p.h))return;
            view.owned=p.added->id;
            if(view.current==view.owned && !p.added->hasSize(p.w,p.h))
                initialMode_=InitialMode{view.owned,p.w,p.h,p.fps};
            finish("电脑已确认新增虚拟屏");
        }else if(p.op==Op::Select && view.current==p.target)finish("已切换到所选屏幕");
        else if(p.op==Op::Initialize && view.current==p.target){
            const auto *s=view.find(p.target);
            if(p.target==view.owned && s && s->hasSize(p.w,p.h))finish("电脑已确认虚拟屏分辨率");
        }
        else if(p.op==Op::Remove && !view.find(p.target))finish("电脑已确认移除虚拟屏");
    }
};
}
