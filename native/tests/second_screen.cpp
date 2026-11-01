// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_store.hpp"
#include "protocol.hpp"
#include "quality_rpc.hpp"
#include "virtual_display.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace uu;

void check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

struct Field {
    unsigned number, wire;
    uint64_t value = 0;
    Bytes bytes;
    bool operator==(const Field &other) const {
        return number == other.number && wire == other.wire &&
               value == other.value && bytes == other.bytes;
    }
};

uint64_t varint(const Bytes &bytes, size_t &position) {
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 70; shift += 7) {
        check(position < bytes.size(), "truncated protobuf varint");
        const auto byte = bytes[position++];
        check(shift != 63 || byte <= 1, "overflowed protobuf varint");
        value |= uint64_t(byte & 127) << shift;
        if (!(byte & 128)) return value;
    }
    throw std::runtime_error("unterminated protobuf varint");
}

std::vector<Field> fields(const Bytes &bytes) {
    std::vector<Field> result;
    size_t position = 0;
    while (position < bytes.size()) {
        const auto tag = varint(bytes, position);
        Field field{unsigned(tag >> 3), unsigned(tag & 7)};
        check(field.number != 0, "zero protobuf field");
        if (field.wire == 0) {
            field.value = varint(bytes, position);
        } else {
            check(field.wire == 2, "unexpected connect-options wire type");
            const auto length = varint(bytes, position);
            check(length <= bytes.size() - position, "truncated protobuf message");
            field.bytes.assign(bytes.begin() + position, bytes.begin() + position + length);
            position += size_t(length);
        }
        result.push_back(std::move(field));
    }
    return result;
}

Field only(const std::vector<Field> &message, unsigned number) {
    std::vector<Field> found;
    for (const auto &field : message) if (field.number == number) found.push_back(field);
    check(found.size() == 1, "required protobuf field missing or repeated");
    return found.front();
}

void wire() {
    const std::string client = "3a5fa985-97ce-4d2b-8414-864f0939b9cf";
    for (const auto size : {std::make_pair(3040,1904), std::make_pair(1904,3040), std::make_pair(1724,1080)}) {
        for (int codec = 0; codec < 3; ++codec) {
            const auto normal = connectOptions(size.first,size.second,120,3,0,codec == 1,codec == 2);
            check(normal == connectOptions(size.first,size.second,120,3,0,codec == 1,codec == 2,false,client),
                  "normal desktop changed when a stable client ID was supplied");
            auto extended = fields(connectOptions(size.first,size.second,120,3,0,codec == 1,codec == 2,true,client));
            check(only(extended,1).wire == 0 && only(extended,1).value == 6, "CT_SECOND_SCREEN must be 6");
            const auto id = only(extended,9);
            check(id.wire == 2 && std::string(id.bytes.begin(),id.bytes.end()) == client, "stable client ID is missing");
            const auto modes = fields(only(extended,6).bytes);
            check(modes.size() == 3 && only(modes,1).value == unsigned(size.first) &&
                  only(modes,2).value == unsigned(size.second) && only(modes,3).value == 120,
                  "second-screen mode must contain width, height and requested cadence");
            const auto initial = fields(only(extended,7).bytes);
            check(initial.size() == 2 && only(initial,1).value == unsigned(size.first) &&
                  only(initial,2).value == unsigned(size.second), "incorrect initial second-screen resolution");
            std::vector<Field> normalized;
            for (auto field : extended) {
                check(field.number != 5, "unproven force_recreate option was added");
                if (field.number == 9) continue;
                if (field.number == 1) field.value = 1;
                normalized.push_back(std::move(field));
            }
            check(normalized == fields(normal), "second-screen request changed unrelated legacy wire fields");
            check(extended == fields(connectOptions(size.first,size.second,120,3,0,codec == 1,codec == 2,true,client)),
                  "reconnection changed the second-screen client identity");
        }
    }
    bool rejected = false;
    try { connectOptions(3040,1904,120,3,0,false,false,true,""); }
    catch (const std::invalid_argument &) { rejected = true; }
    check(rejected, "second-screen without a stable ID must be rejected");
}

struct TemporaryConfig {
    std::filesystem::path previous = std::filesystem::current_path(), directory;
    TemporaryConfig() {
        char pattern[] = "/tmp/youyou-second-screen-XXXXXX";
        check(mkdtemp(pattern) != nullptr, "cannot create isolated test configuration");
        directory = pattern;
        std::filesystem::current_path(directory);
    }
    ~TemporaryConfig() {
        std::error_code error;
        std::filesystem::current_path(previous,error);
        std::filesystem::remove_all(directory,error);
    }
};

std::string configuration() {
    std::ifstream input("config.ini");
    return std::string(std::istreambuf_iterator<char>(input), {});
}

void writeConfiguration(const std::string &text) {
    std::ofstream output("config.ini",std::ios::trunc);
    output << text;
    output.close();
    check(bool(output), "cannot write test configuration");
    ConfigStore::instance().reload();
}

void persistence() {
    TemporaryConfig temporary;
    writeConfiguration("# retained setting\nvsync=0\ncustom_setting=keep\n");
    check(!secondScreenForDevice("host-a") && !secondScreenForDevice("host-b"), "old config must use ordinary desktop");
    saveSecondScreenForDevice("host-a",true);
    check(secondScreenForDevice("host-a") && !secondScreenForDevice("host-b"), "host preferences leaked");
    ConfigStore::instance().reload();
    check(secondScreenForDevice("host-a") && !secondScreenForDevice("host-b"), "reloaded connection lost its mode");
    saveSecondScreenForDevice("host-b",true);
    saveSecondScreenForDevice("host-a",false);
    ConfigStore::instance().reload();
    check(!secondScreenForDevice("host-a") && secondScreenForDevice("host-b"), "disabling one host altered another");
    const auto saved = configuration();
    check(saved.find("# retained setting") != std::string::npos &&
          saved.find("vsync=0") != std::string::npos &&
          saved.find("custom_setting=keep") != std::string::npos, "mode save erased existing settings");
    const auto preference = Json::parse(ConfigStore::instance().value("second_screen_devices"));
    check(!preference.contains("host-a") && preference.at("host-b") == true, "disabled host must release its entry");
    saveQualityConfig(3040,1904,120,3,false,0,0,true);
    ConfigStore::instance().reload();
    check(secondScreenForDevice("host-b") && !secondScreenForDevice("host-a"), "quality save erased per-host mode preferences");
    const auto afterQuality = configuration();
    for (const std::string &id : {std::string{}, std::string("bad host"), std::string("bad\nhost"), std::string(257,'a')}) {
        check(!validSecondScreenDeviceId(id), "invalid host ID accepted");
        bool rejected = false;
        try { saveSecondScreenForDevice(id,true); } catch (const std::invalid_argument &) { rejected = true; }
        check(rejected && configuration() == afterQuality, "invalid host ID modified persisted configuration");
    }
    check(validSecondScreenDeviceId("host_1.test:2-id") && validSecondScreenDeviceId(std::string(256,'a')),
          "documented host ID boundary rejected");
    for (const auto malformed : {"{broken", "[]", "{\"host-a\":\"true\"}", "{\"host-a\":true,\"host-a\":false}"}) {
        writeConfiguration(std::string("second_screen_devices=") + malformed + "\nvsync=0\n");
        const auto before = configuration();
        bool readRejected = false, writeRejected = false;
        try { secondScreenForDevice("host-a"); } catch (const std::runtime_error &) { readRejected = true; }
        try { saveSecondScreenForDevice("host-a",true); } catch (const std::runtime_error &) { writeRejected = true; }
        check(readRejected && writeRejected && configuration() == before, "bad preference JSON was silently replaced");
    }
    writeConfiguration("second_screen_devices={}\n");
    for (int i = 0; i < 64; ++i) saveSecondScreenForDevice("host-" + std::to_string(i),true);
    const auto full = configuration();
    bool rejected = false;
    try { saveSecondScreenForDevice("host-extra",true); } catch (const std::exception &) { rejected = true; }
    check(rejected && configuration() == full, "host limit failed or corrupted existing preferences");
    saveSecondScreenForDevice("host-0",false);
    saveSecondScreenForDevice("host-extra",true);
    ConfigStore::instance().reload();
    check(secondScreenForDevice("host-extra") && !secondScreenForDevice("host-0"), "disabled host did not free capacity");
}

HostScreen screen(int id, int type) {
    HostScreen value;
    value.id = id;value.type = type;value.name = type ? "Session display" : "Physical display";
    value.width = 3040;value.height = 1904;value.fps = 144;
    value.modes = {{1600,900},{3040,1904}};
    return value;
}

void lifecycle() {
    using Op = VirtualDisplays::Op;
    const auto physical = screen(0,0), session = screen(19,1), other = screen(22,1);
    VirtualDisplays normal;
    normal.observe({physical,session},19);
    check(normal.view.sessionManaged < 0 && !normal.view.captureDesktop(), "ordinary session claimed an existing virtual screen");
    VirtualDisplays displays;
    displays.beginSecondScreenSession(3040,1904,120);
    displays.observe({physical,session},19);
    check(displays.view.sessionManaged == 19 && displays.view.owned < 0, "official session display was treated as manual ownership");
    check(displays.view.captureDesktop() && displays.view.capturesOwnedSize(3040,1904),
          "session display cannot drive confirmed capture bounds");
    check(displays.view.captureDesktop()->encoderBounds(1724,1080) == std::make_pair(1920,1080),
          "session desktop cannot retain aspect-correct encoder bounds");
    check(!displays.view.canRemove() && !displays.begin(Op::Remove,1,19,3040,1904,120,0),
          "current official screen could be removed manually");
    displays.observe({physical,session},0);
    check(!displays.view.captureDesktop() && !displays.view.canRemove() &&
          !displays.begin(Op::Remove,2,19,3040,1904,120,0), "inactive official screen could be removed manually");
    displays.observe({physical},0);
    displays.beginSecondScreenSession(3040,1904,120);
    displays.observe({physical,other},22);
    check(displays.view.sessionManaged < 0, "same session claimed a replacement screen");
    displays = VirtualDisplays{};
    displays.beginSecondScreenSession(3040,1904,120);
    displays.observe({physical,session,other},19);
    check(displays.view.sessionManaged < 0 && !displays.view.captureDesktop(), "ambiguous virtual screens were claimed");
    displays = VirtualDisplays{};
    displays.beginSecondScreenSession(3040,1904,120);
    displays.observe({physical},0);
    check(displays.view.sessionManaged < 0, "physical screen was claimed");
    check(!displays.begin(Op::Create,42,-1,3040,1904,120,0),
          "official session manually created a screen before its managed screen arrived");
    displays.observe({physical,session},19);
    check(displays.view.sessionManaged == 19, "later confirmed current virtual screen was not identified");
    auto reused = session;
    reused.name = "Different screen";
    displays.observe({physical,reused},19);
    check(displays.view.sessionManaged < 0 && !displays.view.captureDesktop(), "changed identity retained session ownership");
    displays = VirtualDisplays{};
    check(displays.view.sessionManaged < 0 && !displays.view.captureDesktop(), "new connection inherited an old screen");
    displays.beginSecondScreenSession(1600,900,120);
    displays.observe({physical,session},19);
    const auto initial = displays.beginInitialMode(44,0);
    check(initial && initial->id == 19 && initial->w == 1600 && initial->h == 900 && initial->fps == 120,
          "official session cannot initialize its confirmed supported desktop mode");
    check(!displays.beginInitialMode(45,0), "initial mode was sent twice");
    displays.ackSelection(44,0);
    check(displays.view.pending, "mode ACK alone confirmed a desktop size");
    auto resized = session;
    resized.width = 1600;resized.height = 900;
    displays.observe({physical,resized},19);
    check(!displays.view.pending && displays.view.capturesOwnedSize(1600,900) &&
          !displays.beginInitialMode(46,0), "confirmed official desktop mode did not settle once");
}

void commandGuards() {
    DisplayView view;
    view.session = 71;
    check(desktopModeChangeAllowed(view,true,true,false,"host-a","host-a",71), "valid desktop mode command rejected");
    check(!desktopModeChangeAllowed(view,false,true,false,"host-a","host-a",71), "disconnected mode command admitted");
    check(!desktopModeChangeAllowed(view,true,false,false,"host-a","host-a",71), "unauthenticated mode command admitted");
    check(!desktopModeChangeAllowed(view,true,true,true,"host-a","host-a",71), "competing operation admitted a mode command");
    check(!desktopModeChangeAllowed(view,true,true,false,"host-a","host-b",71), "wrong host mode command admitted");
    check(!desktopModeChangeAllowed(view,true,true,false,"host-a","host-a",70), "stale session mode command admitted");
    check(!desktopModeChangeAllowed(view,true,true,false,"","",71), "missing host mode command admitted");
    view.pending = true;
    check(!desktopModeChangeAllowed(view,true,true,false,"host-a","host-a",71), "pending topology admitted a mode command");
    view.pending = false;view.session = 0;
    check(!desktopModeChangeAllowed(view,true,true,false,"host-a","host-a",0), "missing session mode command admitted");
}
}

int main() {
    try {
        wire();persistence();lifecycle();commandGuards();
        std::puts("PASS second-screen protocol, stable identity, per-host persistence and session display guards");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr,"%s\n",error.what());
        return 1;
    }
}
