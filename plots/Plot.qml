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
    function tickStep(span, divisions) {
        const raw = span / divisions
        const magnitude = Math.pow(10, Math.floor(Math.log10(raw)))
        const scaled = raw / magnitude
        return (scaled < 1.5 ? 1 : scaled < 3 ? 2 : scaled < 7 ? 5 : 10) * magnitude
    }

    Binding {
        target: graph.xValues
        property: "tickInterval"
        value: graph.tickStep((graph.xValues.max - graph.xValues.min) / graph.xValues.zoom,
                              Math.max(2, Math.floor((graph.width - 100) / 85)))
    }
    Binding {
        target: graph.yValues
        property: "tickInterval"
        value: Math.max(graph.yValues.labelDecimals === 0 ? 1 : 0,
                        graph.tickStep((graph.yValues.max - graph.yValues.min) / graph.yValues.zoom,
                                       Math.max(2, Math.floor((graph.height - 90) / 55))))
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
