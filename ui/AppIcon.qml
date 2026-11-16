import QtQuick

Canvas {
    id: iconCanvas
    property string name: ""
    property string color: "#000000"
    width: 22
    height: 22
    antialiasing: true
    renderStrategy: Canvas.Immediate

    onNameChanged: requestPaint()
    onColorChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    onAvailableChanged: if (available) requestPaint()
    onVisibleChanged: if (visible) requestPaint()
    Component.onCompleted: requestPaint()

    onPaint: {
        if (!available || width <= 0 || height <= 0) return;
        var ctx = getContext("2d");
        ctx.reset();
        ctx.clearRect(0, 0, width, height);
        if (!name || name.length === 0) return;

        var w = iconCanvas.width;
        var h = iconCanvas.height;
        var s = Math.min(w, h);
        var sw = Math.max(2.0, s * 0.115);
        var col = iconCanvas.color;

        ctx.strokeStyle = col;
        ctx.fillStyle = col;
        ctx.lineWidth = sw;
        ctx.lineCap = "round";
        ctx.lineJoin = "round";

        if (name === "close") {
            ctx.beginPath();
            ctx.moveTo(w * 0.20, h * 0.20);
            ctx.lineTo(w * 0.80, h * 0.80);
            ctx.moveTo(w * 0.80, h * 0.20);
            ctx.lineTo(w * 0.20, h * 0.80);
            ctx.stroke();
        } else if (name === "check") {
            ctx.lineWidth = Math.max(2.2, s * 0.13);
            ctx.beginPath();
            ctx.moveTo(w * 0.15, h * 0.54);
            ctx.lineTo(w * 0.41, h * 0.80);
            ctx.lineTo(w * 0.86, h * 0.22);
            ctx.stroke();
        } else if (name === "refresh") {
            var cx = w * 0.50;
            var cy = h * 0.52;
            var r = s * 0.34;
            ctx.beginPath();
            ctx.arc(cx, cy, r, -0.30, Math.PI * 1.48, false);
            ctx.stroke();

            // Arrowhead at top-right of arc
            ctx.beginPath();
            ctx.moveTo(w * 0.56, h * 0.12);
            ctx.lineTo(w * 0.88, h * 0.17);
            ctx.lineTo(w * 0.75, h * 0.45);
            ctx.closePath();
            ctx.fill();
        } else if (name === "search") {
            var scx = w * 0.43;
            var scy = h * 0.43;
            var sr = s * 0.28;
            ctx.beginPath();
            ctx.arc(scx, scy, sr, 0, Math.PI * 2.0, false);
            ctx.stroke();

            ctx.lineWidth = Math.max(2.2, s * 0.13);
            ctx.beginPath();
            ctx.moveTo(scx + sr * 0.72, scy + sr * 0.72);
            ctx.lineTo(w * 0.86, h * 0.86);
            ctx.stroke();
        } else if (name === "settings") {
            var gcx = w * 0.50;
            var gcy = h * 0.50;
            ctx.beginPath();
            ctx.arc(gcx, gcy, s * 0.24, 0, Math.PI * 2.0, false);
            ctx.stroke();

            for (var i = 0; i < 8; ++i) {
                var ang = i * Math.PI * 0.25;
                ctx.beginPath();
                ctx.moveTo(gcx + Math.cos(ang) * s * 0.27, gcy + Math.sin(ang) * s * 0.27);
                ctx.lineTo(gcx + Math.cos(ang) * s * 0.43, gcy + Math.sin(ang) * s * 0.43);
                ctx.stroke();
            }
        } else if (name === "sparkle") {
            ctx.beginPath();
            ctx.moveTo(w * 0.50, h * 0.08);
            ctx.quadraticCurveTo(w * 0.50, h * 0.50, w * 0.92, h * 0.50);
            ctx.quadraticCurveTo(w * 0.50, h * 0.50, w * 0.50, h * 0.92);
            ctx.quadraticCurveTo(w * 0.50, h * 0.50, w * 0.08, h * 0.50);
            ctx.quadraticCurveTo(w * 0.50, h * 0.50, w * 0.50, h * 0.08);
            ctx.closePath();
            ctx.fill();
        } else if (name === "download") {
            ctx.beginPath();
            ctx.moveTo(w * 0.50, h * 0.14);
            ctx.lineTo(w * 0.50, h * 0.68);
            ctx.moveTo(w * 0.26, h * 0.46);
            ctx.lineTo(w * 0.50, h * 0.70);
            ctx.lineTo(w * 0.74, h * 0.46);
            ctx.moveTo(w * 0.18, h * 0.86);
            ctx.lineTo(w * 0.82, h * 0.86);
            ctx.stroke();
        } else if (name === "play") {
            ctx.beginPath();
            ctx.moveTo(w * 0.22, h * 0.15);
            ctx.lineTo(w * 0.84, h * 0.50);
            ctx.lineTo(w * 0.22, h * 0.85);
            ctx.closePath();
            ctx.fill();
        } else if (name === "menu") {
            ctx.beginPath();
            ctx.moveTo(w * 0.14, h * 0.24);
            ctx.lineTo(w * 0.86, h * 0.24);
            ctx.moveTo(w * 0.14, h * 0.50);
            ctx.lineTo(w * 0.86, h * 0.50);
            ctx.moveTo(w * 0.14, h * 0.76);
            ctx.lineTo(w * 0.86, h * 0.76);
            ctx.stroke();
        } else if (name === "library") {
            ctx.lineWidth = Math.max(2.0, s * 0.10);
            ctx.strokeRect(w * 0.14, h * 0.14, w * 0.72, h * 0.72);
            ctx.fillRect(w * 0.31, h * 0.31, w * 0.38, h * 0.38);
        } else if (name === "clock") {
            ctx.beginPath();
            ctx.arc(w * 0.50, h * 0.50, s * 0.38, 0, Math.PI * 2.0, false);
            ctx.stroke();

            ctx.beginPath();
            ctx.moveTo(w * 0.50, h * 0.27);
            ctx.lineTo(w * 0.50, h * 0.50);
            ctx.lineTo(w * 0.68, h * 0.61);
            ctx.stroke();
        } else if (name === "circle") {
            ctx.beginPath();
            ctx.arc(w * 0.50, h * 0.50, s * 0.35, 0, Math.PI * 2.0, false);
            ctx.stroke();
        } else if (name === "shift") {
            ctx.beginPath();
            ctx.moveTo(w * 0.50, h * 0.14);
            ctx.lineTo(w * 0.16, h * 0.52);
            ctx.lineTo(w * 0.37, h * 0.52);
            ctx.lineTo(w * 0.37, h * 0.84);
            ctx.lineTo(w * 0.63, h * 0.84);
            ctx.lineTo(w * 0.63, h * 0.52);
            ctx.lineTo(w * 0.84, h * 0.52);
            ctx.closePath();
            ctx.stroke();
        } else if (name === "backspace") {
            ctx.lineWidth = Math.max(1.8, s * 0.095);
            ctx.beginPath();
            ctx.moveTo(w * 0.34, h * 0.22);
            ctx.lineTo(w * 0.90, h * 0.22);
            ctx.lineTo(w * 0.90, h * 0.78);
            ctx.lineTo(w * 0.34, h * 0.78);
            ctx.lineTo(w * 0.10, h * 0.50);
            ctx.closePath();
            ctx.stroke();

            ctx.beginPath();
            ctx.moveTo(w * 0.47, h * 0.37);
            ctx.lineTo(w * 0.73, h * 0.63);
            ctx.moveTo(w * 0.73, h * 0.37);
            ctx.lineTo(w * 0.47, h * 0.63);
            ctx.stroke();
        } else if (name === "enter") {
            ctx.beginPath();
            ctx.moveTo(w * 0.80, h * 0.20);
            ctx.lineTo(w * 0.80, h * 0.62);
            ctx.lineTo(w * 0.22, h * 0.62);
            ctx.moveTo(w * 0.40, h * 0.44);
            ctx.lineTo(w * 0.20, h * 0.62);
            ctx.lineTo(w * 0.40, h * 0.80);
            ctx.stroke();
        } else if (name === "chevron-down") {
            ctx.beginPath();
            ctx.moveTo(w * 0.22, h * 0.36);
            ctx.lineTo(w * 0.50, h * 0.68);
            ctx.lineTo(w * 0.78, h * 0.36);
            ctx.stroke();
        } else if (name === "arrow-left") {
            ctx.beginPath();
            ctx.moveTo(w * 0.84, h * 0.50);
            ctx.lineTo(w * 0.18, h * 0.50);
            ctx.moveTo(w * 0.44, h * 0.24);
            ctx.lineTo(w * 0.18, h * 0.50);
            ctx.lineTo(w * 0.44, h * 0.76);
            ctx.stroke();
        } else if (name === "arrow-right") {
            ctx.beginPath();
            ctx.moveTo(w * 0.16, h * 0.50);
            ctx.lineTo(w * 0.82, h * 0.50);
            ctx.moveTo(w * 0.56, h * 0.24);
            ctx.lineTo(w * 0.82, h * 0.50);
            ctx.lineTo(w * 0.56, h * 0.76);
            ctx.stroke();
        } else if (name === "arrow-up") {
            ctx.beginPath();
            ctx.moveTo(w * 0.50, h * 0.84);
            ctx.lineTo(w * 0.50, h * 0.18);
            ctx.moveTo(w * 0.24, h * 0.44);
            ctx.lineTo(w * 0.50, h * 0.18);
            ctx.lineTo(w * 0.76, h * 0.44);
            ctx.stroke();
        } else if (name === "arrow-down") {
            ctx.beginPath();
            ctx.moveTo(w * 0.50, h * 0.16);
            ctx.lineTo(w * 0.50, h * 0.82);
            ctx.moveTo(w * 0.24, h * 0.56);
            ctx.lineTo(w * 0.50, h * 0.82);
            ctx.lineTo(w * 0.76, h * 0.56);
            ctx.stroke();
        }
    }
}
