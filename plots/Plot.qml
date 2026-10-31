import QtQuick
import QtGraphs
import QtQuick.Controls.Basic

GraphsView {
    id: graph
    objectName: "plotGraph"
    dynamicLabelMargins: true
    readonly property ValueAxis xValues: axisX as ValueAxis
    readonly property ValueAxis yValues: axisY as ValueAxis
    marginLeft: 8
    marginRight: 20
    marginTop: 16
    marginBottom: 32
    panStyle: GraphsView.PanStyle.Drag
    zoomStyle: GraphsView.ZoomStyle.Center

    // Qt's automatic tick step has a 0.0001 floor and does not follow zoom.
    function tickStep(minimum, maximum, zoom, divisions) {
        // Divide before subtracting so opposite finite extremes do not overflow.
        const raw = (maximum / divisions - minimum / divisions) / zoom
        const resolution = Math.max(Number.MIN_VALUE,
                                    Math.max(Math.abs(minimum), Math.abs(maximum)) * Number.EPSILON * 4)
        if (!(raw > 0)) return Math.max(1, resolution)
        if (!isFinite(raw)) return Number.MAX_VALUE / divisions
        const magnitude = Math.max(Number.MIN_VALUE, Math.pow(10, Math.floor(Math.log10(raw))))
        const scaled = raw / magnitude
        const step = (scaled < 1.5 ? 1 : scaled < 3 ? 2 : scaled < 7 ? 5 : 10) * magnitude
        return Math.max(resolution, Math.min(Number.MAX_VALUE, step))
    }

    Binding {
        target: graph.xValues
        property: "tickInterval"
        value: graph.tickStep(graph.xValues.min, graph.xValues.max, graph.xValues.zoom,
                              Math.max(2, Math.floor((graph.width - 100) / 85)))
    }
    Binding {
        target: graph.yValues
        property: "tickInterval"
        value: Math.max(graph.yValues.labelDecimals === 0 ? 1 : 0,
                        graph.tickStep(graph.yValues.min, graph.yValues.max, graph.yValues.zoom,
                                       Math.max(2, Math.floor((graph.height - 90) / 55))))
    }
    Binding {
        target: graph.xValues
        property: "tickAnchor"
        value: graph.xValues.visualMin
    }
    Binding {
        target: graph.yValues
        property: "tickAnchor"
        value: graph.yValues.visualMin
    }

    property int dataRevision: 0
    Connections {
        target: graph.seriesList[0]
        function onPointsReplaced() { graph.dataRevision++ }
    }

    HoverHandler {
        id: pointer
        parent: graph
    }
    readonly property bool inspecting: pointer.hovered
        && pointer.point.position.x >= plotArea.x
        && pointer.point.position.x <= plotArea.x + plotArea.width
        && pointer.point.position.y >= plotArea.y
        && pointer.point.position.y <= plotArea.y + plotArea.height
    readonly property point sample: {
        // Profiles and histograms are ordered by X. Find the bracketing samples.
        const revision = dataRevision
        const series = seriesList[0]
        if (!inspecting || !series || series.count === 0)
            return Qt.point(NaN, NaN)
        const x = xValues.visualMin + (pointer.point.position.x - plotArea.x)
                  / plotArea.width * (xValues.visualMax - xValues.visualMin)
        let low = 0
        let high = series.count - 1
        while (low < high) {
            const mid = Math.floor((low + high) / 2)
            if (series.at(mid).x < x)
                low = mid + 1
            else
                high = mid
        }
        const right = series.at(low)
        const left = series.at(Math.max(0, low - 1))
        if (interpolation.checked && x >= left.x && x <= right.x && right.x > left.x)
            return Qt.point(x, left.y + (right.y - left.y) * (x - left.x) / (right.x - left.x))
        return Math.abs(x - left.x) <= Math.abs(right.x - x) ? left : right
    }

    Rectangle {
        parent: graph
        objectName: "plotCursor"
        z: 1
        visible: graph.inspecting && isFinite(graph.sample.x)
                 && x >= graph.plotArea.x && x <= graph.plotArea.x + graph.plotArea.width
        x: graph.plotArea.x + (graph.sample.x - graph.xValues.visualMin)
           / (graph.xValues.visualMax - graph.xValues.visualMin) * graph.plotArea.width
        y: graph.plotArea.y
        width: 1
        height: graph.plotArea.height
        color: "#80766A"
    }

    Rectangle {
        parent: graph
        objectName: "plotReadout"
        z: 2
        visible: graph.inspecting && isFinite(graph.sample.x)
        x: Math.max(graph.plotArea.x, Math.min(pointer.point.position.x + 12,
                     graph.plotArea.x + graph.plotArea.width - width))
        y: graph.plotArea.y + 6
        width: readout.implicitWidth + 16
        height: readout.implicitHeight + 12
        color: "#FAF7F2"
        border.color: "#D8D2C9"
        radius: 4
        Text {
            id: readout
            objectName: "plotReadoutText"
            anchors.centerIn: parent
            color: "#1A1C1F"
            font.pointSize: 11
            text: graph.xValues.titleText + ": " + Number(graph.sample.x.toPrecision(8))
                  + "\n" + graph.yValues.titleText + ": " + Number(graph.sample.y.toPrecision(8))
        }
    }

    // Optional gel annotations. Coordinates follow the plot's zoom and pan.
    property var bandLabels: []
    property point bandStart: Qt.point(0, 0)
    property point bandEnd: Qt.point(0, 0)
    property bool bandVisible: false
    Item {
        parent: graph
        x: graph.plotArea.x
        y: graph.plotArea.y
        width: graph.plotArea.width
        height: graph.plotArea.height
        clip: true
        z: 1
        Rectangle {
            readonly property real endX: (graph.bandEnd.x - graph.xValues.visualMin)
                / (graph.xValues.visualMax - graph.xValues.visualMin) * parent.width
            readonly property real endY: (graph.yValues.visualMax - graph.bandEnd.y)
                / (graph.yValues.visualMax - graph.yValues.visualMin) * parent.height
            visible: graph.bandVisible
            x: (graph.bandStart.x - graph.xValues.visualMin)
               / (graph.xValues.visualMax - graph.xValues.visualMin) * parent.width
            y: (graph.yValues.visualMax - graph.bandStart.y)
               / (graph.yValues.visualMax - graph.yValues.visualMin) * parent.height
            width: Math.hypot(endX - x, endY - y)
            height: 2
            color: "#3874CB"
            transformOrigin: Item.TopLeft
            rotation: Math.atan2(endY - y, endX - x) * 180 / Math.PI
        }
        Repeater {
            model: graph.bandLabels
            Text {
                required property var modelData
                objectName: "gelPeakLabel"
                x: (modelData.x - graph.xValues.visualMin)
                   / (graph.xValues.visualMax - graph.xValues.visualMin) * parent.width - width / 2
                y: (graph.yValues.visualMax - modelData.y)
                   / (graph.yValues.visualMax - graph.yValues.visualMin) * parent.height - height
                text: modelData.text
                color: "#3874CB"
                font.pointSize: 11
            }
        }
    }

    CheckBox {
        id: interpolation
        parent: graph
        objectName: "plotInterpolation"
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 4
        text: qsTr("Interpolation")
        checked: false
        leftPadding: 24
        rightPadding: 4
        topPadding: 2
        bottomPadding: 2
        indicator: Rectangle {
            x: 4
            anchors.verticalCenter: parent.verticalCenter
            width: 15
            height: 15
            radius: 3
            color: interpolation.checked ? "#1A1C1F" : "#FAF7F2"
            border.color: interpolation.activeFocus ? "#3874CB" : "#80766A"
            Text {
                anchors.centerIn: parent
                text: interpolation.checked ? "✓" : ""
                color: "#FAF7F2"
                font.pixelSize: 12
            }
        }
        contentItem: Text {
            text: interpolation.text
            color: "#1A1C1F"
            font.pointSize: 11
            verticalAlignment: Text.AlignVCenter
        }
    }

    Button {
        id: reset
        parent: graph
        objectName: "plotReset"
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.margins: 4
        text: qsTr("Reset")
        implicitWidth: 52
        implicitHeight: 24
        enabled: graph.xValues && graph.yValues
                 && (graph.xValues.zoom !== 1 || graph.yValues.zoom !== 1
                     || graph.xValues.pan !== 0 || graph.yValues.pan !== 0)
        contentItem: Text {
            text: reset.text
            color: reset.enabled ? "#1A1C1F" : "#80766A"
            font.pointSize: 11
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            color: reset.down ? "#E8E2D9" : "#FAF7F2"
            border.color: reset.activeFocus ? "#3874CB" : "#D8D2C9"
            radius: 4
        }
        onClicked: {
            graph.xValues.zoom = 1
            graph.yValues.zoom = 1
            graph.xValues.pan = 0
            graph.yValues.pan = 0
        }
    }
}
