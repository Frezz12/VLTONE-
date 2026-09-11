import QtQuick
import QtMultimedia
import Vlt.Graphics 1.0

Item {
    id: root
    objectName: "GpuMediaBackground"
    required property var media
    property real density: media.textureScale
    property real dpr: Screen.devicePixelRatio
    property int fps: media.frameLimit
    property bool video: media.videoSource
    property real aspect: video && output.sourceRect.height > 0
        ? output.sourceRect.width / output.sourceRect.height
        : (media.imageSize.height > 0 ? media.imageSize.width / media.imageSize.height : 1)
    property real sourceWidth: video ? output.sourceRect.width / dpr : media.imageSize.width / dpr
    property real sourceHeight: video ? output.sourceRect.height / dpr : media.imageSize.height / dpr
    property bool firstVideoFrame: false
    property bool dirty: true

    function refresh() { dirty = true; if (fps === 0 || video) { capture.scheduleUpdate(); dirty = false } }
    onWidthChanged: refresh()
    onHeightChanged: refresh()
    onDensityChanged: refresh()
    Connections {
        target: root.media
        function onGpuFrameChanged() { root.refresh() }
        function onGpuConfigurationChanged() { root.refresh() }
    }
    Connections {
        target: videoGate
        function onFramePresented() {
            root.firstVideoFrame = true
            if (!root.media.motionEnabled) player.pause()
            root.refresh()
        }
    }
    Connections {
        target: root.media
        function onGpuConfigurationChanged() {
            if (root.video && (root.media.motionEnabled || !root.firstVideoFrame)) player.play()
            else player.pause()
        }
    }
    MediaPlayer {
        id: player
        objectName: "GpuMediaPlayer"
        source: root.video ? root.media.sourceUrl : ""
        loops: MediaPlayer.Infinite
        videoOutput: videoGate
        // Decorative video has no audio output, avoiding a second audio route.
        onSourceChanged: { root.firstVideoFrame = false; if (root.video) play() }
        onErrorOccurred: function(error, message) { console.warn("Background video:", message) }
    }
    VideoFrameGate { id: videoGate; target: output.videoSink; frameLimit: root.fps }
    Item {
        id: content
        width: root.width; height: root.height
        clip: true
        Item {
            id: placement
            objectName: "MediaPlacement"
            anchors.centerIn: parent
            width: root.media.placementMode === 1 ? root.width
                : root.media.placementMode >= 2 ? Math.max(1, root.sourceWidth)
                : Math.max(root.width, root.height * root.aspect)
            height: root.media.placementMode === 1 ? root.height
                : root.media.placementMode >= 2 ? Math.max(1, root.sourceHeight)
                : width / root.aspect
            MediaImage { anchors.fill: parent; image: root.media.gpuImage; visible: !root.video }
            VideoOutput { id: output; anchors.fill: parent; fillMode: VideoOutput.Stretch; visible: root.video }
        }
        ShaderEffect {
            anchors.fill: parent
            visible: root.media.placementMode === 2
            property var source: tileCapture
            property vector2d repeats: Qt.vector2d(root.width / Math.max(1, placement.width),
                                                   root.height / Math.max(1, placement.height))
            fragmentShader: "qrc:/vlt/graphics/shaders/media-tile.frag.qsb"
        }
    }
    ShaderEffectSource {
        id: tileCapture
        sourceItem: root.media.placementMode === 2 ? placement : null
        hideSource: root.media.placementMode === 2
        textureSize: Qt.size(Math.max(1, Math.min(4096, placement.width * root.dpr * root.density)),
                            Math.max(1, Math.min(4096, placement.height * root.dpr * root.density)))
        live: true
        visible: false
    }
    ShaderEffectSource {
        id: capture
        anchors.fill: parent
        sourceItem: content
        hideSource: true
        live: root.fps === 0
        textureSize: Qt.size(Math.max(1, Math.ceil(root.width * root.dpr * root.density)),
                            Math.max(1, Math.ceil(root.height * root.dpr * root.density)))
        visible: false
    }
    Timer {
        interval: root.fps > 0 ? Math.max(1, Math.ceil(1000 / root.fps)) : 1000
        running: root.visible && root.fps > 0 && root.dirty
        repeat: true
        onTriggered: { capture.scheduleUpdate(); root.dirty = false }
    }
    ShaderEffect {
        id: horizontal
        anchors.fill: parent
        property var source: capture
        property vector2d delta: Qt.vector2d(root.media.blurPixels / Math.max(1, root.width) / 4, 0)
        property vector2d extent: Qt.vector2d(root.width, root.height)
        property real cornerRadius: 0
        fragmentShader: "qrc:/vlt/graphics/shaders/media-blur.frag.qsb"
        visible: false
    }
    ShaderEffectSource {
        id: blurCapture
        anchors.fill: parent
        sourceItem: horizontal
        hideSource: true
        textureSize: capture.textureSize
        live: true
        visible: false
    }
    ShaderEffect {
        anchors.fill: parent
        property var source: root.media.blurPixels > 0 ? blurCapture : capture
        property vector2d delta: Qt.vector2d(0, root.media.blurPixels / Math.max(1, root.height) / 4)
        property vector2d extent: Qt.vector2d(root.width, root.height)
        property real cornerRadius: root.media.cornerRadius
        fragmentShader: "qrc:/vlt/graphics/shaders/media-blur.frag.qsb"
    }
}
