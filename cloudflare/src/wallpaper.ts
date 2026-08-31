// Info-wallpaper renderer: draws a 240x320 info screen (clock + weather +
// notes) into an RGB buffer and encodes it as a JPEG, pushed through the
// existing image-push channel. Pure Worker-side, no device changes.
import jpeg from "jpeg-js";

export const WALLPAPER_WIDTH = 240;
export const WALLPAPER_HEIGHT = 320;

// 5x7 dot-matrix font, each glyph is 7 rows of 5 pixels. Only uppercase
// letters, digits and a few symbols are included; lowercase is folded to
// uppercase and unknown characters render as a hollow box.
const FONT: Record<string, string[]> = {
    " ": ["00000", "00000", "00000", "00000", "00000", "00000", "00000"],
    ".": ["00000", "00000", "00000", "00000", "00000", "01100", "01100"],
    ":": ["00000", "01100", "01100", "00000", "01100", "01100", "00000"],
    "-": ["00000", "00000", "00000", "11111", "00000", "00000", "00000"],
    "/": ["00001", "00010", "00010", "00100", "01000", "01000", "10000"],
    "%": ["11001", "11001", "00010", "00100", "01000", "10011", "10011"],
    "?": ["01110", "10001", "00001", "00110", "00100", "00000", "00100"],
    "0": ["01110", "10001", "10011", "10101", "11001", "10001", "01110"],
    "1": ["00100", "01100", "00100", "00100", "00100", "00100", "01110"],
    "2": ["01110", "10001", "00001", "00110", "01000", "10000", "11111"],
    "3": ["11110", "00001", "00001", "01110", "00001", "00001", "11110"],
    "4": ["00010", "00110", "01010", "10010", "11111", "00010", "00010"],
    "5": ["11111", "10000", "10000", "11110", "00001", "00001", "11110"],
    "6": ["00110", "01000", "10000", "11110", "10001", "10001", "01110"],
    "7": ["11111", "00001", "00010", "00100", "01000", "01000", "01000"],
    "8": ["01110", "10001", "10001", "01110", "10001", "10001", "01110"],
    "9": ["01110", "10001", "10001", "01111", "00001", "00010", "01100"],
    A: ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    B: ["11110", "10001", "10001", "11110", "10001", "10001", "11110"],
    C: ["01110", "10001", "10000", "10000", "10000", "10001", "01110"],
    D: ["11100", "10010", "10001", "10001", "10001", "10010", "11100"],
    E: ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    F: ["11111", "10000", "10000", "11110", "10000", "10000", "10000"],
    G: ["01110", "10001", "10000", "10111", "10001", "10001", "01111"],
    H: ["10001", "10001", "10001", "11111", "10001", "10001", "10001"],
    I: ["01110", "00100", "00100", "00100", "00100", "00100", "01110"],
    J: ["00111", "00010", "00010", "00010", "00010", "10010", "01100"],
    K: ["10001", "10010", "10100", "11000", "10100", "10010", "10001"],
    L: ["10000", "10000", "10000", "10000", "10000", "10000", "11111"],
    M: ["10001", "11011", "10101", "10101", "10001", "10001", "10001"],
    N: ["10001", "11001", "10101", "10011", "10001", "10001", "10001"],
    O: ["01110", "10001", "10001", "10001", "10001", "10001", "01110"],
    P: ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
    Q: ["01110", "10001", "10001", "10001", "10101", "10010", "01101"],
    R: ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
    S: ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    T: ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
    U: ["10001", "10001", "10001", "10001", "10001", "10001", "01110"],
    V: ["10001", "10001", "10001", "10001", "10001", "01010", "00100"],
    W: ["10001", "10001", "10001", "10101", "10101", "10101", "01010"],
    X: ["10001", "10001", "01010", "00100", "01010", "10001", "10001"],
    Y: ["10001", "10001", "01010", "00100", "00100", "00100", "00100"],
    Z: ["11111", "00001", "00010", "00100", "01000", "10000", "11111"],
};

const FONT_ADVANCE = 6; // 5px glyph + 1px spacing (scaled by the text scale)

type Rgb = [number, number, number];

class WallpaperCanvas {
    readonly data: Uint8Array;

    constructor(
        readonly width: number,
        readonly height: number,
    ) {
        this.data = new Uint8Array(width * height * 3);
    }

    setPixel(x: number, y: number, [r, g, b]: Rgb): void {
        if (x < 0 || y < 0 || x >= this.width || y >= this.height) return;
        const offset = (y * this.width + x) * 3;
        this.data[offset] = r;
        this.data[offset + 1] = g;
        this.data[offset + 2] = b;
    }

    fillRect(x: number, y: number, w: number, h: number, color: Rgb): void {
        for (let py = y; py < y + h; py++) {
            for (let px = x; px < x + w; px++) this.setPixel(px, py, color);
        }
    }

    strokeRect(x: number, y: number, w: number, h: number, color: Rgb): void {
        this.fillRect(x, y, w, 1, color);
        this.fillRect(x, y + h - 1, w, 1, color);
        this.fillRect(x, y, 1, h, color);
        this.fillRect(x + w - 1, y, 1, h, color);
    }

    verticalGradient(top: Rgb, bottom: Rgb): void {
        for (let y = 0; y < this.height; y++) {
            const t = y / (this.height - 1);
            const color: Rgb = [
                Math.round(top[0] + (bottom[0] - top[0]) * t),
                Math.round(top[1] + (bottom[1] - top[1]) * t),
                Math.round(top[2] + (bottom[2] - top[2]) * t),
            ];
            this.fillRect(0, y, this.width, 1, color);
        }
    }

    textWidth(text: string, scale: number): number {
        return text.length * FONT_ADVANCE * scale;
    }

    text(x: number, y: number, text: string, scale: number, color: Rgb): void {
        let cursor = x;
        for (const char of text) {
            const glyph = glyphFor(char);
            if (!glyph) {
                cursor += FONT_ADVANCE * scale;
                continue;
            }
            for (let col = 0; col < 5; col++) {
                for (let row = 0; row < 7; row++) {
                    if (glyph[row][col] === "1") {
                        this.fillRect(cursor + col * scale, y + row * scale, scale, scale, color);
                    }
                }
            }
            cursor += FONT_ADVANCE * scale;
        }
    }
}

function glyphFor(char: string): string[] | null {
    if (char === " ") return FONT[" "];
    return FONT[char.toUpperCase()] ?? FONT["?"];
}

export interface WallpaperWeather {
    tempC: number;
    text: string;
}

export interface WallpaperInput {
    now: Date;
    city: string;
    weather: WallpaperWeather | null;
    notes: string[];
}

const DAY_NAMES = ["SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"];

function pad2(value: number): string {
    return String(value).padStart(2, "0");
}

/** WMO weather-code -> short English label. */
export function weatherCodeLabel(code: number): string {
    if (code === 0) return "CLEAR";
    if (code === 1) return "MAINLY CLEAR";
    if (code === 2) return "PARTLY CLOUDY";
    if (code === 3) return "OVERCAST";
    if (code >= 45 && code <= 48) return "FOG";
    if (code >= 51 && code <= 55) return "DRIZZLE";
    if (code >= 61 && code <= 65) return "RAIN";
    if (code >= 66 && code <= 67) return "FREEZING RAIN";
    if (code >= 71 && code <= 75) return "SNOW";
    if (code === 77) return "SNOW GRAINS";
    if (code >= 80 && code <= 82) return "SHOWERS";
    if (code >= 95) return "THUNDERSTORM";
    return "UNKNOWN";
}

/** Renders the info wallpaper and returns a base64 JPEG data payload. */
export function renderWallpaperJpeg(input: WallpaperInput, quality = 80): string {
    const canvas = new WallpaperCanvas(WALLPAPER_WIDTH, WALLPAPER_HEIGHT);

    // Background: deep indigo gradient.
    canvas.verticalGradient([9, 13, 26], [30, 34, 74]);

    // Header row: left brand, right city.
    canvas.text(16, 16, "KIRO PASSPORT", 1, [140, 160, 215]);
    const city = input.city.toUpperCase().slice(0, 12);
    canvas.text(WALLPAPER_WIDTH - 16 - canvas.textWidth(city, 1), 16, city, 1, [90, 185, 225]);

    // Big clock (HH:MM at 2x) + running seconds.
    const clock = `${pad2(input.now.getHours())}:${pad2(input.now.getMinutes())}`;
    const seconds = pad2(input.now.getSeconds());
    const clockWidth = canvas.textWidth(clock, 2);
    canvas.text((WALLPAPER_WIDTH - clockWidth) / 2, 52, clock, 2, [238, 242, 255]);
    canvas.text((WALLPAPER_WIDTH + clockWidth) / 2 + 10, 64, seconds, 1, [120, 140, 205]);

    // Date line.
    const dateStr = `${input.now.getFullYear()}-${pad2(input.now.getMonth() + 1)}-${pad2(input.now.getDate())} ${DAY_NAMES[input.now.getDay()]}`;
    canvas.text((WALLPAPER_WIDTH - canvas.textWidth(dateStr, 1)) / 2, 98, dateStr, 1, [165, 178, 208]);

    // Weather card.
    canvas.fillRect(16, 118, 208, 56, [17, 23, 45]);
    canvas.strokeRect(16, 118, 208, 56, [62, 82, 132]);
    canvas.text(26, 128, "WEATHER", 1, [102, 132, 190]);
    if (input.weather) {
        const temp = `${Math.round(input.weather.tempC)} C`;
        canvas.text(26, 142, temp, 2, [122, 222, 200]);
        canvas.text(26 + canvas.textWidth(temp, 2) + 14, 152, input.weather.text.slice(0, 14), 1, [152, 202, 232]);
    } else {
        canvas.text(26, 152, "UNAVAILABLE", 1, [120, 140, 185]);
    }

    // Notes card (up to 3 lines).
    canvas.fillRect(16, 186, 208, 92, [17, 23, 45]);
    canvas.strokeRect(16, 186, 208, 92, [62, 82, 132]);
    canvas.text(26, 196, "TODAY", 1, [102, 132, 190]);
    const lines = input.notes.map((line) => line.trim()).filter(Boolean).slice(0, 3);
    if (lines.length === 0) {
        canvas.text(26, 216, "NO NOTES", 1, [112, 128, 158]);
    } else {
        lines.forEach((line, index) => {
            canvas.text(26, 214 + index * 20, line.toUpperCase().slice(0, 28), 1, [202, 210, 228]);
        });
    }

    // Footer.
    const footer = "WS.YANYUN.ASIA";
    canvas.text((WALLPAPER_WIDTH - canvas.textWidth(footer, 1)) / 2, 302, footer, 1, [82, 96, 132]);

    return encodeJpegBase64(canvas, quality);
}

function encodeJpegBase64(canvas: WallpaperCanvas, quality: number): string {
    const encoded = jpeg.encode({ data: canvas.data, width: canvas.width, height: canvas.height }, quality);
    let binary = "";
    for (let offset = 0; offset < encoded.data.length; offset += 0x8000) {
        binary += String.fromCharCode(...encoded.data.subarray(offset, offset + 0x8000));
    }
    return btoa(binary);
}
