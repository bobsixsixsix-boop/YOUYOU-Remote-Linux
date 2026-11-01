// Included in namespace uu in client.cpp. All mutable topology and ownership
// state belongs to the generation-scoped Client worker.
void Client::publishDisplays() {
    displays_.view.session=generation_;
    auto view=std::make_shared<const DisplayView>(displays_.view);
    std::shared_ptr<const DisplayView> previous;
    {
        std::lock_guard lock(stateMutex_);previous=state_.displays;state_.displays=view;
    }
    if(const auto *s=view->find(view->owned)){
        const auto *old=previous?previous->find(previous->owned):nullptr;
        if(!old || !old->identity(*s) || old->width!=s->width || old->height!=s->height || old->fps!=s->fps ||
           previous->current!=view->current)
            diagnostic(("virtual-display-owned-"+std::to_string(s->id)+"-current-"+std::to_string(view->current)+
                "-desktop-"+std::to_string(s->width)+"x"+std::to_string(s->height)+"-hz-"+std::to_string(s->fps)).c_str());
    }
    // An owned display can keep the same ID when Windows switches from its
    // startup timing to the requested size. Discard the earlier scale ACK.
    if(view->capturesOwnedSize(streamWidth_,streamHeight_) &&
       (!previous || previous->current!=view->current || !previous->capturesOwnedSize(streamWidth_,streamHeight_))){
        pendingVideoScale_.reset();videoScaleAttempted_=false;videoScaleVerified_=false;nextVideoScaleCheck_=0;
        diagnostic(("virtual-display-refresh-video-screen-"+std::to_string(view->current)+"-q"+std::to_string(streamQuality_)+
            "-cap-"+std::to_string(maxBitrateMbps_)+"-target-"+std::to_string(streamWidth_)+"x"+std::to_string(streamHeight_)).c_str());
    }
    if(auto mode=displays_.beginInitialMode(nextQualityRpc_,monotonicMs())){
        sendDisplayRpc(nextQualityRpc_++,VirtualDisplays::Op::Initialize,mode->id,mode->w,mode->h,mode->fps,{});
        publishDisplays();
    }
}
void Client::sendDisplayRpc(uint64_t id,VirtualDisplays::Op op,int target,int w,int h,int fps,
                            const std::vector<std::pair<int,int>> &panelModes) {
    const auto ch=channels_.find("TEXT_DATA_CHANNEL");
    if(pendingQuality_ || ch==channels_.end() || !ch->second->isOpen() || ch->second->bufferedAmount()>65536){
        displays_.sendFailed();return;
    }
    try{
        const auto bytes=virtualDisplayRpc(id,op,target,w,h,fps,streamQuality_,streamQuality_==6?maxBitrateMbps_:0,panelModes);
        ch->second->send(reinterpret_cast<const std::byte*>(bytes.data()),bytes.size());
        const char *action=op==VirtualDisplays::Op::Initialize?"initial-mode":op==VirtualDisplays::Op::Select?"select":
            op==VirtualDisplays::Op::Create?"create":"remove";
        diagnostic((std::string("virtual-display-")+action+"-rpc-"+std::to_string(id)+"-screen-"+std::to_string(target)+
            "-request-"+std::to_string(w)+"x"+std::to_string(h)+"-capture-fps-"+std::to_string(fps)).c_str());
    }catch(const std::exception &){displays_.sendFailed();}
}
void Client::displayCommand(const Json &data) {
    const auto state=snapshot();
    if(!state.streaming || data.value("deviceId",std::string{})!=connectedDeviceId_ ||
       data.value("session",uint64_t(0))!=generation_)return;
    auto reject=[&](const char *notice){displays_.view.notice=notice;publishDisplays();};
    auto ch=channels_.find("TEXT_DATA_CHANNEL");
    if(ch==channels_.end() || !ch->second->isOpen() || ch->second->bufferedAmount()>65536){
        reject("电脑控制通道未就绪");return;
    }
    if(pendingQuality_){reject("画质设置仍在应用，请稍后再试");return;}
    const auto action=data.value("op",std::string{});
    if(action!="create" && action!="select" && action!="remove")return;
    const auto op=action=="create"?VirtualDisplays::Op::Create:action=="remove"?VirtualDisplays::Op::Remove:VirtualDisplays::Op::Select;
    const int target=data.value("target",-1);
    if(op!=VirtualDisplays::Op::Create){
        auto screen=displays_.view.find(target);
        if(!screen || screen->type!=data.value("type",-1) || screen->name!=data.value("name",std::string{})){
            reject("电脑屏幕已变化，请重新选择");return;
        }
    }
    const auto id=nextQualityRpc_++;
    const int w=streamWidth_,h=streamHeight_,fps=streamFps_;
    const auto modes=data.value("modes",std::vector<std::pair<int,int>>{});
    if(!displays_.begin(op,id,target,w,h,fps,monotonicMs())){
        reject("屏幕状态已变化，请核对列表");return;
    }
    sendDisplayRpc(id,op,target,w,h,0,modes);
    publishDisplays();
}

void Client::desktopModeCommand(const Json &data) {
    const auto state=snapshot();
    auto reject=[&](const char *message){displays_.view.notice=message;publishDisplays();};
    const auto enabled=data.find("enabled");
    const auto device=data.find("deviceId"),session=data.find("session");
    if(enabled==data.end() || !enabled->is_boolean() || device==data.end() || !device->is_string() ||
       session==data.end() || !session->is_number_unsigned()){
        reject("连接模式请求无效");return;
    }
    const bool busy=pendingQuality_ || pendingQualityApply_ || pendingPrivacy_ ||
        (pendingVideoScale_ && !pendingVideoScale_->acknowledged);
    if(!desktopModeChangeAllowed(displays_.view,state.streaming,api_->authenticated(),busy,
                                 connectedDeviceId_,device->get<std::string>(),session->get<uint64_t>()) ||
       state.activeDeviceId!=connectedDeviceId_ || !validSecondScreenDeviceId(api_->device())){
        reject("连接或屏幕状态已变化，请等待设置完成后重试");return;
    }
    const bool target=enabled->get<bool>();
    if(target==secondScreen_){reject("当前已使用所选连接模式");return;}
    try {saveSecondScreenForDevice(connectedDeviceId_,target);}
    catch(const std::exception &error){reject(error.what());return;}
    if(target)secondScreenBlockedDevice_.clear();
    const auto id=connectedDeviceId_;
    disconnect();
    setState("idle",target?"正在重新连接专用桌面…":"正在重新连接普通桌面…");
    post("connect",{{"deviceId",id}},generation_);
}
