// Runs without a desktop session: node test-stream-edges.js
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
class Signal {
    callbacks = [];
    connect(fn) { this.callbacks.push(fn); }
    emit(w) { this.callbacks.forEach(fn => fn(w)); }
}
const stream = {resourceClass: "youyou-linux", caption: "YOUYOU · 设备 | 在线", captionChanged: new Signal()};
const foreign = {resourceClass: "other", caption: "YOUYOU · 串流中 | 模拟标题", captionChanged: new Signal()};
const workspace = {
    windowList: () => [stream, foreign],
    windowAdded: new Signal(), windowActivated: new Signal(), windowRemoved: new Signal(),
    activeWindow: stream,
};
let armed;
let oskVisible = false;
const ctx = vm.createContext({workspace, update: (_added, removed) => {
    armed = !oskVisible && !ctx.youyouOwnsSideEdges(removed);
}});
vm.runInContext(fs.readFileSync(path.join(__dirname, "stream-edges.js"), "utf8"), ctx);
ctx.update(null, null);
assert.equal(armed, true);
stream.caption = "YOUYOU · 串流中 | 已连接";
stream.captionChanged.emit();
assert.equal(armed, false);
workspace.activeWindow = foreign;
workspace.windowActivated.emit(foreign);
assert.equal(armed, true);
workspace.activeWindow = stream;
workspace.windowActivated.emit(stream);
assert.equal(armed, false);
workspace.windowRemoved.emit(stream); // KWin can still report the removed window as active here.
assert.equal(armed, true);
stream.caption = "YOUYOU · 设备 | 已断开";
stream.captionChanged.emit();
assert.equal(armed, true);
oskVisible = true;
workspace.activeWindow = foreign;
workspace.windowActivated.emit(foreign);
assert.equal(armed, false);
console.log("Account, stream, focus, removal, disconnect and OSK checks passed.");
