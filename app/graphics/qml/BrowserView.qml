import QtQuick
import QtWebEngine
import QtWebChannel

WebEngineView {
    id: web
    required property var bridge
    activeFocusOnPress: true
    webChannel: WebChannel {
        id: channel
        Component.onCompleted: web.bridge.attachWebChannel(channel)
    }
    function setDocument(html, base) { loadHtml(html, base) }
    function back() { goBack() }
    function forward() { goForward() }
    function refresh() { reload() }
    function halt() { stop() }
    function configure(name, enabled) { settings[name] = enabled }
    function edit(name) { triggerWebAction(WebEngineView[name]) }
    function find(text, backwards) { findText(text, backwards ? WebEngineView.FindBackward : 0) }
    function acceptWindow(request) { acceptAsNewWindow(request) }
    function downloadFile(url, name) {
        runJavaScript("(()=>{const a=document.createElement('a');a.href=" + JSON.stringify(url.toString())
                      + ";a.download=" + JSON.stringify(name) + ";document.body.appendChild(a);a.click();a.remove()})()")
    }
    onUrlChanged: bridge.notifyUrl()
    onTitleChanged: bridge.notifyTitle()
    onLoadProgressChanged: bridge.notifyProgress()
    onIconChanged: bridge.notifyIcon()
    onLoadingChanged: function(info) { bridge.receiveFrame(mainFrame); bridge.notifyLoading(info) }
    onNavigationRequested: function(request) {
        if (!bridge.allowNavigation(request.url, request.isMainFrame)) request.reject()
    }
    onNewWindowRequested: function(request) { bridge.notifyNewWindow(request) }
    onRenderProcessTerminated: function(status, code) { bridge.notifyTerminated(status, code) }
    onPermissionRequested: function(permission) { permission.deny() }
    onJavaScriptConsoleMessage: function(level, message, line, source) { }
}
