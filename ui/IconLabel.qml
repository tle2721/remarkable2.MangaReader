import QtQuick

Row {
    id: labelRoot
    property string icon: ""
    property string rightIcon: ""
    property string text: ""
    property string color: "#000000"
    property int pixelSize: 22
    property bool bold: true
    property int iconSize: Math.round(pixelSize * 0.92)
    spacing: 8

    AppIcon {
        visible: labelRoot.icon !== ""
        name: labelRoot.icon
        width: labelRoot.iconSize
        height: labelRoot.iconSize
        color: labelRoot.color
        anchors.verticalCenter: parent.verticalCenter
    }

    Text {
        visible: labelRoot.text !== ""
        text: labelRoot.text
        font.pixelSize: labelRoot.pixelSize
        font.bold: labelRoot.bold
        color: labelRoot.color
        anchors.verticalCenter: parent.verticalCenter
    }

    AppIcon {
        visible: labelRoot.rightIcon !== ""
        name: labelRoot.rightIcon
        width: labelRoot.iconSize
        height: labelRoot.iconSize
        color: labelRoot.color
        anchors.verticalCenter: parent.verticalCenter
    }
}
