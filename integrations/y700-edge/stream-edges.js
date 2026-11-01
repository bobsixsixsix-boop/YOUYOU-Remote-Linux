// Give the first touch to the active streaming window. Merely ignoring back()
// would leave KWin's registered touch strip between the finger and the client.
function youyouOwnsSideEdges(removed) {
    const w = workspace.activeWindow;
    return !!(w && w !== removed && !w.deleted && !w.minimized &&
        String(w.resourceClass) === "youyou-linux" &&
        String(w.caption).startsWith("YOUYOU · 串流中 | "));
}

function watchYouyouWindow(w) {
    if (String(w.resourceClass) !== "youyou-linux") return;
    w.captionChanged.connect(function () {
        if (workspace.activeWindow === w) update(null, null);
    });
}

workspace.windowList().forEach(watchYouyouWindow);
workspace.windowAdded.connect(watchYouyouWindow);
workspace.windowActivated.connect(function () { update(null, null); });
workspace.windowRemoved.connect(function (w) {
    if (String(w.resourceClass) === "youyou-linux") update(null, w);
});
