// frame-buffer.js — RGB565 → RGBA 像素转换 + rAF 渲染
//
// 给按帧拿到 RGB565 的 QEMU 帧缓冲数据 -> 转 RGBA -> 推到 canvas(240x320)。
// 用于替换 simulator.ts 里写死的假 renderScreen()：画面将来自真实固件。

export function rgb565ToRgba(u16, out, w = 240, h = 320) {
  // 需要时把 out(Uint8ClampedArray<len>) 填成 RGBA
  for (let i = 0; i < w * h; i++) {
    const px = u16[i];
    const r = (px >> 8) & 0xf8;
    const g = (px >> 3) & 0xfc;
    const b = (px << 3) & 0xf8;
    const o = i * 4;
    out[o] = r; out[o + 1] = g; out[o + 2] = b; out[o + 3] = 255;
  }
}

/** 渲染一帧到 canvas。u16 = RGB565 pixels 数组（长度 240*320）。 */
export function renderFrameToCanvas(canvas, u16) {
  const ctx = canvas.getContext("2d");
  const img = ctx.createImageData(240, 320);
  rgb565ToRgba(u16, img.data); // 原地填入 RGBA
  ctx.putImageData(img, 0, 0);
}