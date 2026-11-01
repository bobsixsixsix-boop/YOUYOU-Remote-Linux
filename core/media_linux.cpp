// SPDX-License-Identifier: GPL-3.0-or-later
// uu::Media for Linux. Video goes straight to a stateful V4L2 decoder (the
// Qualcomm iris on the Y700) and decoded NV12 buffers are lent to the player
// as DMA-BUFs (bridge.h), so no pixel is copied. On the Y700 this is
// 1.1-2.7x lower latency than VA-API (tools/results/20261102-decode-latency.txt).
// Presentation is "newest picture wins": pacing belongs to the player, which
// knows the display's vblank. Audio is upstream's Opus + SDL scheduler, unchanged.
#include "media.hpp"
#include "bridge.h"
#include "diagnostics.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <map>
#include <opus.h>
#include <poll.h>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {
constexpr uint32_t OUT = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, CAP = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
constexpr unsigned kOutBuffers = 8;
constexpr size_t kOutBytes = 4u << 20; // the strict receiver's AU limit

int64_t mediaClockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
int64_t monoUs() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return int64_t(t.tv_sec) * 1000000 + t.tv_nsec / 1000;
}
int xioctl(int fd, unsigned long request, void *arg) {
    int r;
    do r = ioctl(fd, request, arg);
    while (r < 0 && errno == EINTR);
    return r;
}
void wake(int fd) {
    const uint64_t one = 1;
    if (fd >= 0) (void)!write(fd, &one, sizeof one);
}
// Random-access point, read from the first slice NAL of the access unit
// (every slice of one AU shares it): H.264 IDR; HEVC BLA, IDR or CRA. Upstream
// counts only 16-20, but x265's periodic keyframes are CRA and decode fine.
bool keyframe(const uint8_t *d, size_t n, bool hevc) {
    for (size_t i = 0; i + 3 < n; ++i) {
        if (d[i] || d[i + 1] || d[i + 2] != 1) continue;
        const int type = hevc ? d[i + 3] >> 1 & 63 : d[i + 3] & 31;
        if (hevc ? type < 32 : type >= 1 && type <= 5) return hevc ? type >= 16 && type <= 21 : type == 5;
        i += 2;
    }
    return false;
}
// First memory-to-memory device that takes this bitstream format. Stateless
// decoders use the *_SLICE formats, so a match is always a stateful one.
int openDecoder(uint32_t fourcc) {
    for (int n = 0; n < 64; ++n) {
        const std::string path = "/dev/video" + std::to_string(n);
        const int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        v4l2_capability cap{};
        if (!xioctl(fd, VIDIOC_QUERYCAP, &cap) && (cap.device_caps & V4L2_CAP_VIDEO_M2M_MPLANE)) {
            v4l2_fmtdesc f{};
            f.type = OUT;
            for (; !xioctl(fd, VIDIOC_ENUM_FMT, &f); ++f.index)
                if (f.pixelformat == fourcc) return fd;
        }
        close(fd);
    }
    return -1;
}

// Hand-off between the decoder thread and the player. One Media per process.
// Up to QUEUED pictures wait, oldest first: the frame pacer (YY_PACE) shows each
// of them; without it the player takes the newest and the rest go back.
constexpr size_t QUEUED = 3;
struct Slot {
    std::mutex mutex;
    std::condition_variable cv;
    uint64_t epoch = 0, serial = 0;
    std::deque<yy_frame> ready;    // unclaimed pictures, fd unset
    std::vector<int> exported;     // one DMA-BUF per CAPTURE buffer of this epoch
    std::vector<uint32_t> returned; // released by the player, for the decoder thread to queue
    int wake = -1;
};
// Never destroyed: the player's threads may still wait on it while the process exits.
Slot &slot = *new Slot;

// Lends ready[at] and gives the older pictures back to the decoder. Holds slot.mutex.
int lend(yy_frame *out, size_t at) {
    for (size_t i = 0; i < at; ++i) slot.returned.push_back(slot.ready[i].index);
    *out = slot.ready[at];
    slot.ready.erase(slot.ready.begin(), slot.ready.begin() + at + 1);
    out->fd = out->index < slot.exported.size() ? fcntl(slot.exported[out->index], F_DUPFD_CLOEXEC, 0) : -1;
    if (out->fd < 0) slot.returned.push_back(out->index);
    if (at || out->fd < 0) wake(slot.wake);
    return out->fd >= 0;
}
} // namespace

namespace uu {
struct Media::Impl {
    struct Unit {
        std::vector<uint8_t> data;
        bool hevc;
        std::optional<bool> keyframe;
    };
    std::mutex mutex, audioMutex;
    // Shared under `mutex`.
    std::deque<Unit> queue;
    size_t queueBytes = 0;
    bool stop = false, restart = false, resync = false;
    std::string error;
    std::atomic<uint64_t> generation{1};
    std::atomic<bool> keyframeRequested{false}, hevcStream{false};
    std::atomic<int64_t> lastDecodedMs{0};
    std::atomic<uint64_t> keyframes{0}, decodeErrors{0}, dropped{0}, decoded{0};
    std::atomic<int> width{0}, height{0}, playoutDelayMs{35};
    std::atomic<double> fps{0}, decodeMs{0};
    const int wakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    std::thread thread;

    // Decoder thread only.
    int fd = -1;
    bool hevc = false, waitKey = true, failed = false, capOn = false, pollable = true;
    unsigned capPlanes = 1;
    uint32_t stride = 0, uvOffset = 0, visibleWidth = 0, visibleHeight = 0;
    std::array<void *, kOutBuffers> outMap{};
    std::array<size_t, kOutBuffers> outLength{};
    std::vector<uint32_t> outFree;
    std::array<int64_t, 64> sentUs{};
    uint64_t sequence = 0;
    int64_t fpsWindowUs = 0;
    int fpsFrames = 0;

    // Audio, as upstream media.cpp.
    SDL_AudioDeviceID audioDevice = 0;
    std::atomic<int> volumePercent{100};
    OpusDecoder *opus = nullptr;
    enum class AudioStatus { Ready, Unavailable, QueueFailed };
    std::atomic<AudioStatus> audioStatus{AudioStatus::Ready};
    std::map<int64_t, std::vector<uint8_t>> audioHold;
    bool audioAnchored = false, audioHaveClock = false, audioHavePlay = false;
    int64_t audioAnchorRtp = 0, audioAnchorDueMs = 0, audioExtendedRtp = 0, audioPlayRtp = 0;
    uint32_t audioLastRtp32 = 0;
    int audioDelayMs = 35, audioFrameSamples = 960;
    std::array<int16_t, 5760 * 2> audioPcm{};
    void resetAudioSchedule();
    void queueAudioPcm(int samples);
    void concealAudioUntil(int64_t rtp);
    void releaseAudio();
    void ingestAudio(const uint8_t *data, size_t size, std::optional<uint32_t> timestamp);

    Impl() {
        if (wakeFd < 0) throw std::runtime_error("eventfd failed");
        SDL_AudioSpec spec{};
        spec.freq = 48000;
        spec.format = AUDIO_S16SYS;
        spec.channels = 2;
        spec.samples = 960;
        audioDevice = SDL_OpenAudioDevice(nullptr, 0, &spec, nullptr, 0);
        if (!audioDevice) audioStatus = AudioStatus::Unavailable;
        int code = 0;
        opus = opus_decoder_create(48000, 2, &code);
        if (code != OPUS_OK) {
            if (audioDevice) SDL_CloseAudioDevice(audioDevice);
            close(wakeFd);
            throw std::runtime_error("Opus decoder initialization failed");
        }
        if (audioDevice) SDL_PauseAudioDevice(audioDevice, 0);
        {
            std::lock_guard lock(slot.mutex);
            slot.wake = wakeFd;
        }
        thread = std::thread([this] {
            try {
                run();
            } catch (...) {
                diagnosticException("video-decode-exception", std::current_exception());
            }
            closeDevice();
        });
    }
    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stop = true;
        }
        wake(wakeFd);
        thread.join();
        {
            std::lock_guard lock(slot.mutex);
            slot.wake = -1;
        }
        close(wakeFd);
        if (audioDevice) SDL_CloseAudioDevice(audioDevice);
        opus_decoder_destroy(opus);
    }

    void run() {
        for (;;) {
            bool restartNow, resyncNow;
            std::optional<Unit> unit;
            {
                std::lock_guard lock(mutex);
                if (stop) return;
                restartNow = std::exchange(restart, false);
                resyncNow = std::exchange(resync, false);
                if (!queue.empty() && (restartNow || fd < 0 || !outFree.empty())) {
                    unit = std::move(queue.front());
                    queue.pop_front();
                    queueBytes -= unit->data.size();
                }
            }
            if (restartNow) {
                closeDevice();
                failed = false;
            }
            if (resyncNow) waitKey = true;
            if (unit) {
                submit(*unit);
                continue;
            }
            pollfd p[2] = {{pollable ? fd : -1, POLLIN | POLLOUT | POLLPRI, 0}, {wakeFd, POLLIN, 0}};
            if (poll(p, 2, -1) < 0 && errno != EINTR) throw std::runtime_error("poll failed");
            if (p[1].revents) {
                uint64_t count;
                (void)!read(wakeFd, &count, sizeof count);
            }
            if (p[0].revents) service(p[0].revents);
            std::vector<uint32_t> returned;
            {
                std::lock_guard lock(slot.mutex);
                returned.swap(slot.returned);
            }
            for (const auto index : returned) queueCapture(index);
        }
    }
    bool fail(const char *what) {
        const std::string text = std::string(what) + ": " + std::strerror(errno);
        diagnostic(("v4l2-failed " + text).c_str());
        {
            std::lock_guard lock(mutex);
            error = "视频解码失败（" + text + "）";
        }
        closeDevice();
        failed = true;
        return false;
    }
    bool openDevice(bool wantHevc) {
        const uint32_t fourcc = wantHevc ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264;
        fd = openDecoder(fourcc);
        if (fd < 0) return fail(wantHevc ? "没有支持 H.265 的 V4L2 解码器" : "没有支持 H.264 的 V4L2 解码器");
        hevc = wantHevc;
        v4l2_format f{};
        f.type = OUT;
        f.fmt.pix_mp.pixelformat = fourcc;
        f.fmt.pix_mp.width = 1920; // the real size arrives with SOURCE_CHANGE
        f.fmt.pix_mp.height = 1080;
        f.fmt.pix_mp.num_planes = 1;
        f.fmt.pix_mp.plane_fmt[0].sizeimage = kOutBytes;
        if (xioctl(fd, VIDIOC_S_FMT, &f)) return fail("S_FMT output");
        v4l2_requestbuffers r{};
        r.type = OUT;
        r.memory = V4L2_MEMORY_MMAP;
        r.count = kOutBuffers;
        if (xioctl(fd, VIDIOC_REQBUFS, &r) || r.count < kOutBuffers) return fail("REQBUFS output");
        for (uint32_t i = 0; i < kOutBuffers; ++i) {
            v4l2_plane plane{};
            v4l2_buffer b{};
            b.type = OUT;
            b.memory = V4L2_MEMORY_MMAP;
            b.index = i;
            b.m.planes = &plane;
            b.length = 1;
            if (xioctl(fd, VIDIOC_QUERYBUF, &b)) return fail("QUERYBUF output");
            void *map = mmap(nullptr, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, plane.m.mem_offset);
            if (map == MAP_FAILED) return fail("mmap output");
            outMap[i] = map;
            outLength[i] = plane.length;
            outFree.push_back(i);
        }
        v4l2_event_subscription sub{};
        sub.type = V4L2_EVENT_SOURCE_CHANGE;
        if (xioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub)) return fail("subscribe SOURCE_CHANGE");
        int type = OUT;
        if (xioctl(fd, VIDIOC_STREAMON, &type)) return fail("STREAMON output");
        hevcStream = wantHevc;
        diagnostic(wantHevc ? "v4l2-open-hevc" : "v4l2-open-h264");
        return true;
    }
    // Invalidate every lent picture: the player drops releases from older epochs.
    void retire() {
        std::lock_guard lock(slot.mutex);
        ++slot.epoch;
        slot.ready.clear();
        for (const int e : slot.exported) close(e);
        slot.exported.clear();
        slot.returned.clear();
    }
    void closeDevice() {
        if (fd < 0) return;
        retire(); // the player's dup'ed DMA-BUFs keep their memory alive
        for (uint32_t i = 0; i < kOutBuffers; ++i)
            if (outMap[i]) munmap(outMap[i], outLength[i]);
        outMap = {};
        outFree.clear();
        close(fd);
        fd = -1;
        capOn = false;
        pollable = true;
        waitKey = true;
    }
    void submit(const Unit &u) {
        const bool key = u.keyframe ? *u.keyframe : keyframe(u.data.data(), u.data.size(), u.hevc);
        if (key) ++keyframes;
        if (fd >= 0 && u.hevc != hevc) closeDevice();
        if (failed) return;
        if (waitKey && !key) {
            ++dropped;
            keyframeRequested = true;
            return;
        }
        if (fd < 0 && !openDevice(u.hevc)) return;
        const uint32_t index = outFree.back();
        if (u.data.size() > outLength[index]) {
            ++dropped;
            keyframeRequested = true;
            waitKey = true;
            return;
        }
        std::memcpy(outMap[index], u.data.data(), u.data.size());
        v4l2_plane plane{};
        plane.bytesused = uint32_t(u.data.size());
        v4l2_buffer b{};
        b.type = OUT;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = index;
        b.m.planes = &plane;
        b.length = 1;
        b.timestamp.tv_sec = time_t(sequence); // CAPTURE hands it back: decode latency
        sentUs[sequence++ % sentUs.size()] = monoUs();
        if (xioctl(fd, VIDIOC_QBUF, &b)) {
            fail("QBUF output");
            return;
        }
        outFree.pop_back();
        waitKey = false;
        pollable = true;
    }
    void service(short revents) {
        if (revents & POLLPRI) {
            v4l2_event ev{};
            while (fd >= 0 && !xioctl(fd, VIDIOC_DQEVENT, &ev))
                if (ev.type == V4L2_EVENT_SOURCE_CHANGE && !setupCapture()) return;
        }
        if (fd >= 0 && (revents & POLLOUT))
            for (;;) {
                v4l2_plane plane{};
                v4l2_buffer b{};
                b.type = OUT;
                b.memory = V4L2_MEMORY_MMAP;
                b.m.planes = &plane;
                b.length = 1;
                if (xioctl(fd, VIDIOC_DQBUF, &b)) break;
                outFree.push_back(b.index);
                if (b.flags & V4L2_BUF_FLAG_ERROR) bitstreamError();
            }
        if (fd >= 0 && capOn && (revents & POLLIN))
            for (;;) {
                v4l2_plane planes[VIDEO_MAX_PLANES]{};
                v4l2_buffer b{};
                b.type = CAP;
                b.memory = V4L2_MEMORY_MMAP;
                b.m.planes = planes;
                b.length = capPlanes;
                if (xioctl(fd, VIDIOC_DQBUF, &b)) break;
                if ((b.flags & V4L2_BUF_FLAG_ERROR) || !planes[0].bytesused) {
                    if (b.flags & V4L2_BUF_FLAG_ERROR) bitstreamError();
                    queueCapture(b.index);
                } else
                    publish(b.index, uint64_t(b.timestamp.tv_sec));
            }
        // With nothing queued on either side an m2m device polls as POLLERR
        // forever; leave it out until the next QBUF.
        if (revents == POLLERR) pollable = false;
    }
    void bitstreamError() {
        ++decodeErrors;
        keyframeRequested = true;
    }
    bool setupCapture() {
        int type = CAP;
        if (capOn) xioctl(fd, VIDIOC_STREAMOFF, &type);
        capOn = false;
        retire();
        v4l2_requestbuffers r{};
        r.type = CAP;
        r.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_REQBUFS, &r)) return fail("REQBUFS capture 0");
        v4l2_format f{};
        f.type = CAP;
        if (xioctl(fd, VIDIOC_G_FMT, &f)) return fail("G_FMT capture");
        // The decoder spec's visible rectangle is the CAPTURE crop. Read it before
        // S_FMT, which resets it to the coded size on iris; drivers differ on
        // whether they take the single- or multi-planar type here.
        v4l2_rect visible{};
        for (const uint32_t selType : {uint32_t(V4L2_BUF_TYPE_VIDEO_CAPTURE), CAP}) {
            v4l2_selection sel{};
            sel.type = selType;
            sel.target = V4L2_SEL_TGT_CROP;
            if (!xioctl(fd, VIDIOC_G_SELECTION, &sel) && sel.r.width && sel.r.height) {
                visible = sel.r; // shortcut: assumes the crop starts at 0,0, as every stream so far does
                break;
            }
        }
        f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12; // linear: EGL imports it without a modifier
        if (xioctl(fd, VIDIOC_S_FMT, &f) || f.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12)
            return fail("S_FMT capture NV12");
        capPlanes = f.fmt.pix_mp.num_planes;
        if (capPlanes != 1) {
            errno = EINVAL;
            return fail("NV12 capture is not one plane");
        }
        stride = f.fmt.pix_mp.plane_fmt[0].bytesperline;
        uvOffset = stride * f.fmt.pix_mp.height; // coded height, e.g. 1088 for 1080p
        visibleWidth = std::min(visible.width ? visible.width : UINT32_MAX, f.fmt.pix_mp.width);
        visibleHeight = std::min(visible.height ? visible.height : UINT32_MAX, f.fmt.pix_mp.height);
        v4l2_control minimum{};
        minimum.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;
        // +6: up to three waiting in the slot, two held by the player, one spare.
        r.count = (xioctl(fd, VIDIOC_G_CTRL, &minimum) ? 4 : minimum.value) + QUEUED + 3;
        if (xioctl(fd, VIDIOC_REQBUFS, &r)) return fail("REQBUFS capture");
        std::vector<int> exported;
        for (uint32_t i = 0; i < r.count; ++i) {
            v4l2_exportbuffer e{};
            e.type = CAP;
            e.index = i;
            e.flags = O_RDONLY | O_CLOEXEC;
            if (xioctl(fd, VIDIOC_EXPBUF, &e)) {
                for (const int x : exported) close(x);
                return fail("EXPBUF capture");
            }
            exported.push_back(e.fd);
        }
        {
            std::lock_guard lock(slot.mutex);
            slot.exported = std::move(exported);
        }
        for (uint32_t i = 0; i < r.count; ++i) queueCapture(i);
        if (xioctl(fd, VIDIOC_STREAMON, &type)) return fail("STREAMON capture");
        capOn = true;
        width = int(visibleWidth);
        height = int(visibleHeight);
        diagnostic(("v4l2-capture-" + std::to_string(visibleWidth) + "x" + std::to_string(visibleHeight) +
                    "-stride-" + std::to_string(stride) + "-buffers-" + std::to_string(r.count)).c_str());
        return true;
    }
    void queueCapture(uint32_t index) {
        v4l2_plane planes[VIDEO_MAX_PLANES]{};
        v4l2_buffer b{};
        b.type = CAP;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = index;
        b.m.planes = planes;
        b.length = capPlanes;
        if (fd >= 0 && xioctl(fd, VIDIOC_QBUF, &b)) diagnostic(("v4l2-requeue-failed " + std::to_string(errno)).c_str());
        pollable = true;
    }
    void publish(uint32_t index, uint64_t sent) {
        const int64_t now = monoUs();
        if (sent < sequence && sequence - sent <= sentUs.size()) {
            const double ms = double(now - sentUs[sent % sentUs.size()]) / 1000.0;
            const double average = decodeMs.load();
            decodeMs = average ? average * 0.9 + ms * 0.1 : ms;
        }
        if (now - fpsWindowUs >= 1000000) {
            fps = fpsWindowUs ? fpsFrames * 1e6 / double(now - fpsWindowUs) : 0.0;
            fpsWindowUs = now;
            fpsFrames = 0;
        }
        ++fpsFrames;
        ++decoded;
        lastDecodedMs = mediaClockMs();
        int64_t displaced = -1;
        {
            std::lock_guard lock(slot.mutex);
            slot.ready.push_back({-1, visibleWidth, visibleHeight, stride, uvOffset, index, slot.epoch, ++slot.serial, now});
            if (slot.ready.size() > QUEUED) {
                displaced = slot.ready.front().index;
                slot.ready.pop_front();
            }
        }
        slot.cv.notify_all();
        if (displaced >= 0) queueCapture(uint32_t(displaced));
    }
};

Media::Media() : impl_(std::make_unique<Impl>()) {}
Media::~Media() = default;

void Media::video(const uint8_t *data, size_t size, uint64_t generation, std::optional<uint32_t>, bool hevc,
                  std::optional<bool> validatedKeyframe) {
    if (!data || !size || generation != impl_->generation.load()) return;
    if (size > kOutBytes) {
        ++impl_->dropped;
        std::lock_guard lock(impl_->mutex);
        impl_->resync = true;
        impl_->keyframeRequested = true;
        return;
    }
    Impl::Unit unit{std::vector<uint8_t>(data, data + size), hevc, validatedKeyframe};
    {
        std::lock_guard lock(impl_->mutex);
        if (generation != impl_->generation.load()) return;
        // Decoding fell behind: drop the backlog and restart from the next IDR
        // instead of feeding broken references (upstream's policy and limits).
        if (impl_->queue.size() >= 32 || impl_->queueBytes + size > 8u * 1024u * 1024u) {
            impl_->dropped += impl_->queue.size();
            impl_->queue.clear();
            impl_->queueBytes = 0;
            impl_->resync = true;
            impl_->keyframeRequested = true;
        }
        impl_->queueBytes += size;
        impl_->queue.push_back(std::move(unit));
    }
    wake(impl_->wakeFd);
}

namespace {
int opusFrameSamples(int samples) {
    switch (samples) {
    case 120: case 240: case 480: case 960: case 1920: case 2880: return samples;
    default: return 960;
    }
}
}
void Media::Impl::resetAudioSchedule() {
    audioHold.clear();
    audioAnchored=false;audioHaveClock=false;audioHavePlay=false;
    audioAnchorRtp=audioAnchorDueMs=audioExtendedRtp=audioPlayRtp=0;
    audioLastRtp32=0;audioFrameSamples=960;
}
void Media::Impl::queueAudioPcm(int samples) {
    if(samples<=0 || !audioDevice) return;
    const int volume=volumePercent.load();
    if(volume!=100) for(int i=0;i<samples*2;++i) audioPcm[size_t(i)]=int16_t(int(audioPcm[size_t(i)])*volume/100);
    // A device queue past 200 ms is already late. Drop this frame and leave
    // the samples that are still playing.
    if(SDL_GetQueuedAudioSize(audioDevice) > 48000u * 4u / 5u) return;
    audioStatus=SDL_QueueAudio(audioDevice,audioPcm.data(),Uint32(samples*4))==0 ?
        AudioStatus::Ready : AudioStatus::QueueFailed;
}
void Media::Impl::concealAudioUntil(int64_t rtp) {
    const int frame=opusFrameSamples(audioFrameSamples);
    if(!audioHavePlay) {
        audioPlayRtp=rtp;audioHavePlay=true;return;
    }
    if(rtp<=audioPlayRtp+frame/2) return;
    int concealed=0;
    while(audioPlayRtp+frame/2<rtp && concealed<frame*3) {
        const int samples=opus_decode(opus,nullptr,0,audioPcm.data(),frame,0);
        if(samples<=0) break;
        queueAudioPcm(samples);
        audioPlayRtp+=samples;concealed+=samples;audioFrameSamples=samples;
    }
    if(rtp-audioPlayRtp>opusFrameSamples(audioFrameSamples)) {
        opus_decoder_ctl(opus,OPUS_RESET_STATE);
        audioPlayRtp=rtp;
    }
}
void Media::Impl::releaseAudio() {
    const auto now=mediaClockMs();
    while(!audioHold.empty()) {
        const auto it=audioHold.begin();
        const int64_t due=audioAnchorDueMs+(it->first-audioAnchorRtp)*1000/48000;
        if(due>now) break;
        const int frame=opusFrameSamples(audioFrameSamples);
        if(audioHavePlay && it->first+frame/2<audioPlayRtp) {audioHold.erase(it);continue;}
        concealAudioUntil(it->first);
        const int samples=opus_decode(opus,it->second.data(),int(it->second.size()),audioPcm.data(),5760,0);
        if(samples>0) {
            audioFrameSamples=samples;
            queueAudioPcm(samples);
            audioPlayRtp=it->first+samples;audioHavePlay=true;
        }
        audioHold.erase(it);
    }
    while(audioHold.size()>16) audioHold.erase(audioHold.begin());
}
void Media::Impl::ingestAudio(const uint8_t *data, size_t size, std::optional<uint32_t> timestamp) {
    if(!timestamp) {
        const int samples=opus_decode(opus,data,int(size),audioPcm.data(),5760,0);
        if(samples>0) {audioFrameSamples=samples;queueAudioPcm(samples);}
        return;
    }
    int64_t extended;
    if(!audioHaveClock) {
        audioHaveClock=true;audioLastRtp32=*timestamp;audioExtendedRtp=int64_t(*timestamp);extended=audioExtendedRtp;
    } else {
        const int32_t delta=int32_t(*timestamp-audioLastRtp32);
        audioLastRtp32=*timestamp;audioExtendedRtp+=delta;extended=audioExtendedRtp;
    }
    const int frame=opusFrameSamples(audioFrameSamples);
    if(audioHavePlay && extended+frame/2<audioPlayRtp) return;
    if(!audioAnchored) {
        audioAnchored=true;audioAnchorRtp=extended;audioAnchorDueMs=mediaClockMs()+audioDelayMs;
    } else if(!audioHavePlay && extended<audioAnchorRtp) {
        audioAnchorDueMs-=(audioAnchorRtp-extended)*1000/48000;audioAnchorRtp=extended;
        const auto now=mediaClockMs();
        if(audioAnchorDueMs<now) audioAnchorDueMs=now;
    }
    if(!audioHold.count(extended)) audioHold.emplace(extended,std::vector<uint8_t>(data,data+size));
    releaseAudio();
}
void Media::audio(const uint8_t *data, size_t size, uint64_t generation, std::optional<uint32_t> timestamp) {
    if (!size || size > 4096)
        return;
    std::lock_guard lock(impl_->audioMutex);
    if (generation != impl_->generation.load())
        return;
    impl_->ingestAudio(data, size, timestamp);
}

// The player draws lent DMA-BUFs (yy_frame_acquire); these SDL-picture hooks are for upstream's Switch UI.
Picture Media::picture(uint64_t, int) { return {}; }
void Media::setYuvOutput(bool) {}
void Media::setNativeOutput(bool) {}

void Media::setPlayoutDelay(int milliseconds) {
    // Video always shows the newest picture; the delay only schedules audio.
    milliseconds = std::clamp(milliseconds, 0, 100);
    const int previous = impl_->playoutDelayMs.exchange(milliseconds);
    std::lock_guard lock(impl_->audioMutex);
    impl_->audioDelayMs = milliseconds;
    if (impl_->audioAnchored) impl_->audioAnchorDueMs += milliseconds - previous;
    impl_->releaseAudio();
}
void Media::reset() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->generation.fetch_add(1);
        impl_->queue.clear();
        impl_->queueBytes = 0;
        impl_->restart = true;
        impl_->error.clear();
        impl_->keyframes = 0;
        impl_->decoded = 0;
        impl_->decodeErrors = 0;
        impl_->dropped = 0;
        impl_->keyframeRequested = true;
        impl_->lastDecodedMs = 0;
        impl_->width = 0;
        impl_->height = 0;
        impl_->fps = 0;
        impl_->decodeMs = 0;
    }
    wake(impl_->wakeFd);
    std::lock_guard lock(impl_->audioMutex);
    if (impl_->audioDevice) SDL_ClearQueuedAudio(impl_->audioDevice);
    opus_decoder_ctl(impl_->opus, OPUS_RESET_STATE);
    impl_->resetAudioSchedule();
}
uint64_t Media::generation() const { return impl_->generation.load(); }
void Media::setVolume(int percent) { impl_->volumePercent.store(std::clamp(percent, 0, 100)); }
std::string Media::status() {
    std::lock_guard lock(impl_->mutex);
    std::string text = impl_->error.empty() ? "V4L2 硬件解码" : impl_->error;
    const int width = impl_->width.load(), height = impl_->height.load();
    if (width > 0) text += " · " + std::to_string(width) + "x" + std::to_string(height);
    if (const auto errors = impl_->decodeErrors.load()) text += " · 解码错误 " + std::to_string(errors);
    if (const auto dropped = impl_->dropped.load()) text += " · 丢帧 " + std::to_string(dropped);
    const auto audio = impl_->audioStatus.load();
    if (audio != Impl::AudioStatus::Ready)
        text += audio == Impl::AudioStatus::Unavailable ? " · 音频设备不可用，请退出程序后重试" : " · 音频播放失败，正在重试";
    return text;
}
bool Media::takeKeyframeRequest() { return impl_->keyframeRequested.exchange(false); }
int64_t Media::frameAgeMs() {
    const auto last = impl_->lastDecodedMs.load();
    return last == 0 ? int64_t{1} << 40 : mediaClockMs() - last;
}
MediaStats Media::stats() {
    MediaStats out;
    std::lock_guard lock(impl_->mutex);
    out.codec = impl_->hevcStream ? "H.265" : "H.264";
    out.decoder = "V4L2 硬解";
    out.width = impl_->width.load();
    out.height = impl_->height.load();
    out.fps = impl_->fps.load();
    out.decodeMs = impl_->decodeMs.load();
    out.decodeErrors = impl_->decodeErrors.load();
    out.droppedFrames = impl_->dropped.load();
    out.keyframeCount = impl_->keyframes.load();
    out.decodedTotal = out.publishedTotal = impl_->decoded.load();
    out.epoch = impl_->generation.load();
    out.inputQueued = int(impl_->queue.size());
    out.hardwareFrame = true;
    out.playoutDelayMs = impl_->playoutDelayMs.load();
    const auto last = impl_->lastDecodedMs.load();
    out.lastDecodedAgeMs = last ? mediaClockMs() - last : -1;
    return out;
}
uint64_t Media::keyframeCount() { return impl_->keyframes.load(); }
} // namespace uu

extern "C" int yy_frame_acquire(yy_frame *out) {
    std::lock_guard lock(slot.mutex);
    return out && !slot.ready.empty() ? lend(out, slot.ready.size() - 1) : 0;
}
extern "C" int yy_frame_next(yy_frame *out) {
    std::lock_guard lock(slot.mutex);
    const int waiting = int(slot.ready.size());
    return out && waiting && lend(out, 0) ? waiting : 0;
}
extern "C" void yy_frame_release(const yy_frame *frame) {
    std::lock_guard lock(slot.mutex);
    if (!frame || frame->epoch != slot.epoch) return;
    slot.returned.push_back(frame->index);
    wake(slot.wake);
}
extern "C" uint64_t yy_frame_wait(uint64_t serial, int timeout_ms) {
    std::unique_lock lock(slot.mutex);
    return slot.cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                            [&] { return !slot.ready.empty() && slot.ready.back().serial > serial; })
               ? slot.ready.back().serial
               : 0;
}
// No session needed: the player's clip self-test feeds the decoder directly.
extern "C" void yy_debug_video(const void *data, size_t size, int hevc) {
    static auto &media = *new uu::Media; // never destroyed, as the feeding thread outlives exit()
    media.video(static_cast<const uint8_t *>(data), size, media.generation(), std::nullopt, hevc != 0);
}
