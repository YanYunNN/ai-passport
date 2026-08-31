// Standalone voice-assistant module: a self-contained web page for recording
// audio and chatting with an AI (Grok via xAI API), plus the ASR pipeline
// (Whisper via Cloudflare Workers AI). TTS goes through the Edge TTS proxy
// Worker on tts.yanyun.asia (OpenAI-compatible POST /v1/audio/speech).
import { bearerToken, hasBearerSecret, verifyAdminBasicAuth, verifyDeviceCredential } from "./auth";
import type { Env } from "./env";

const json = (body: unknown, status = 200): Response =>
    new Response(JSON.stringify(body), {
        status,
        headers: { "Content-Type": "application/json; charset=utf-8", "Cache-Control": "no-store" },
    });

async function voiceAuthorized(request: Request, env: Env): Promise<boolean> {
    if (hasBearerSecret(request, env.HOOK_AUTH_SECRET)) return true;
    // 固件 Chat 用 X-Device-Id + Bearer 设备凭证调用 ASR/Chat/TTS。
    const deviceId = request.headers.get("X-Device-Id");
    const credential = bearerToken(request);
    if (deviceId && credential && (await verifyDeviceCredential(env, deviceId, credential))) {
        return true;
    }
    return (await verifyAdminBasicAuth(request, env)) !== null;
}

const VOICE_PAGE_CSS = `
:root { --bg:#080a10; --panel:#0e1117; --border:rgba(148,163,184,0.14); --text:#e7ecf3; --muted:#8b94a3; --accent:#6366f1; --accent2:#22d3ee; --ok:#4ade80; }
* { box-sizing:border-box; margin:0; padding:0; }
body {
    font-family:-apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
    background:var(--bg);
    background-image:radial-gradient(900px 420px at 15% -10%, rgba(99,102,241,0.16), transparent 60%),
                     radial-gradient(700px 380px at 90% 0%, rgba(34,211,238,0.10), transparent 55%);
    color:var(--text); min-height:100vh; display:flex; flex-direction:column; line-height:1.5;
}
.topbar {
    display:flex; align-items:center; gap:0.8rem; padding:0.8rem 1.2rem;
    border-bottom:1px solid var(--border); backdrop-filter:blur(14px);
    background:rgba(8,10,16,0.72); position:sticky; top:0; z-index:10;
}
.brand { font-weight:700; font-size:1.05rem; letter-spacing:0.01em; }
.brand span { background:linear-gradient(92deg,#818cf8,#c084fc 55%,#22d3ee); -webkit-background-clip:text; background-clip:text; -webkit-text-fill-color:transparent; }
.status { margin-left:auto; font-size:0.75rem; color:var(--muted); background:rgba(148,163,184,0.1); border:1px solid var(--border); padding:0.25rem 0.7rem; border-radius:999px; }
.status.ok { color:var(--ok); border-color:rgba(74,222,128,0.3); }
.chat { flex:1; overflow-y:auto; padding:1.2rem; display:flex; flex-direction:column; gap:0.8rem; max-width:720px; width:100%; margin:0 auto; }
.bubble { max-width:78%; padding:0.65rem 0.9rem; border-radius:14px; font-size:0.92rem; white-space:pre-wrap; word-break:break-word; animation:pop .22s ease; }
@keyframes pop { from { opacity:0; transform:translateY(6px);} to { opacity:1; transform:none; } }
.bubble.user { align-self:flex-end; background:linear-gradient(135deg,#4f46e5,#7c3aed); border-bottom-right-radius:4px; }
.bubble.ai { align-self:flex-start; background:var(--panel); border:1px solid var(--border); border-bottom-left-radius:4px; }
.bubble.system { align-self:center; color:var(--muted); font-size:0.8rem; background:transparent; border:none; }
.bubble.loading { color:var(--muted); font-style:italic; }
.inputbar { border-top:1px solid var(--border); padding:0.9rem 1.2rem; display:flex; gap:0.7rem; align-items:center; max-width:720px; width:100%; margin:0 auto; }
.micbtn {
    width:54px; height:54px; border-radius:50%; border:none; cursor:pointer; flex:none;
    background:linear-gradient(135deg,#4f46e5,#7c3aed); color:#fff; font-size:1.3rem;
    box-shadow:0 6px 20px -6px rgba(99,102,241,0.6); transition:transform .15s ease, box-shadow .15s ease;
}
.micbtn:hover { transform:translateY(-2px); }
.micbtn:active { transform:scale(0.95); }
.micbtn.recording { background:linear-gradient(135deg,#dc2626,#f43f5e); animation:pulse 1.2s ease infinite; }
@keyframes pulse { 0%,100% { box-shadow:0 0 0 0 rgba(244,63,94,0.5);} 50% { box-shadow:0 0 0 12px rgba(244,63,94,0);} }
.textinput { flex:1; background:rgba(255,255,255,0.04); border:1px solid var(--border); border-radius:12px; padding:0.7rem 0.9rem; color:var(--text); font-size:0.92rem; outline:none; }
.textinput:focus { border-color:rgba(129,140,248,0.65); box-shadow:0 0 0 3px rgba(99,102,241,0.15); }
.sendbtn { background:var(--accent); color:#fff; border:none; border-radius:12px; padding:0.7rem 1.1rem; cursor:pointer; font-weight:600; }
.sendbtn:hover { background:#818cf8; }
.sendbtn:disabled, .micbtn:disabled { opacity:0.5; cursor:not-allowed; }
.speakbtn { background:none; border:1px solid var(--border); color:var(--muted); border-radius:999px; padding:0.15rem 0.6rem; font-size:0.75rem; cursor:pointer; margin-top:0.4rem; }
.speakbtn:disabled { opacity:0.4; cursor:not-allowed; }
.speakbtn.playing { color:#22d3ee; border-color:rgba(34,211,238,0.45); }
.speakbtn.paused { color:#fbbf24; border-color:rgba(251,191,36,0.45); }
.stopbtn { background:none; border:1px solid var(--border); color:var(--muted); border-radius:999px; padding:0.25rem 0.7rem; font-size:0.75rem; cursor:pointer; }
.stopbtn:hover { color:#f87171; border-color:rgba(248,113,113,0.5); }
.stopbtn[hidden] { display:none; }
.hint { text-align:center; color:var(--muted); font-size:0.75rem; padding:0.4rem 1rem 0.8rem; }
@media (max-width:560px){ .bubble{max-width:90%;} }
`;

function voicePage(): Response {
    const body = `<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Kiro Voice Assistant</title>
<style>${VOICE_PAGE_CSS}</style>
</head>
<body>
<div class="topbar">
    <div class="brand">🎙️ <span>Kiro Voice</span></div>
    <button class="status" id="ttsToggle" title="自动朗读 AI 回复">🔊 自动朗读</button>
    <button class="stopbtn" id="stopBtn" title="停止朗读" hidden>⏹ 停止</button>
    <div class="status" id="statusPill">检测中…</div>
</div>
<div class="chat" id="chat">
    <div class="bubble system">按住麦克风说话，或直接输入文字；识别后交给 AI 回答。</div>
</div>
<div class="inputbar">
    <button class="micbtn" id="micBtn" title="点击开始/结束录音">🎙️</button>
    <input class="textinput" id="textInput" placeholder="输入文字，或点麦克风说话…" autocomplete="off">
    <button class="sendbtn" id="sendBtn">发送</button>
</div>
<div class="hint">ASR: Whisper Large v3 Turbo (Workers AI) · LLM: OpenAI 兼容网关 · TTS: Edge TTS</div>
<script>
(function () {
    "use strict";
    const chat = document.getElementById("chat");
    const micBtn = document.getElementById("micBtn");
    const textInput = document.getElementById("textInput");
    const sendBtn = document.getElementById("sendBtn");
    const statusPill = document.getElementById("statusPill");
    const MAX_RECORD_MS = 30000;
    let history = [];
    let recorder = null;
    let chunks = [];
    let stopTimer = null;
    let busy = false;
    let autoplay = true;
    // Playback state: the active Audio and the speak-button it is bound to.
    let currentAudio = null;
    let currentBtn = null;

    const ttsToggle = document.getElementById("ttsToggle");
    const stopBtn = document.getElementById("stopBtn");
    ttsToggle.addEventListener("click", () => {
        autoplay = !autoplay;
        ttsToggle.textContent = autoplay ? "🔊 自动朗读" : "🔇 已静音";
    });

    // 停止并重置当前朗读（无论播放中还是暂停）。幂等：无播放时直接返回。
    function stopPlayback() {
        if (currentAudio) {
            currentAudio.pause();
            currentAudio.currentTime = 0;
            currentAudio = null;
        }
        if (currentBtn) {
            currentBtn.textContent = "🔊";
            currentBtn.classList.remove("playing", "paused");
            currentBtn.title = "朗读";
            currentBtn = null;
        }
        stopBtn.hidden = true;
    }
    stopBtn.addEventListener("click", stopPlayback);

    function appendBubble(text, kind) {
        const div = document.createElement("div");
        div.className = "bubble " + kind;
        div.textContent = text;
        chat.appendChild(div);
        chat.scrollTop = chat.scrollHeight;
        return div;
    }

    async function api(path, options) {
        const res = await fetch(path, options);
        const data = await res.json().catch(() => ({}));
        if (!res.ok || data.ok === false) throw new Error(data.error || ("HTTP " + res.status));
        return data;
    }

    function setBusy(v) {
        busy = v;
        sendBtn.disabled = v;
        micBtn.disabled = v;
    }

    // 录音 -> WAV(16k/16bit/mono) -> ASR
    async function blobToWav16k(blob) {
        const buf = await blob.arrayBuffer();
        const ctx = new (window.AudioContext || window.webkitAudioContext)();
        const audio = await ctx.decodeAudioData(buf);
        const srcRate = audio.sampleRate;
        const ratio = srcRate / 16000;
        const outLen = Math.floor(audio.length / ratio);
        const out = new Float32Array(outLen);
        for (let i = 0; i < outLen; i++) {
            const idx = Math.min(Math.floor(i * ratio), audio.length - 1);
            let s = 0;
            for (let c = 0; c < audio.numberOfChannels; c++) s += audio.getChannelData(c)[idx];
            out[i] = s / audio.numberOfChannels;
        }
        const dataSize = outLen * 2;
        const wav = new ArrayBuffer(44 + dataSize);
        const view = new DataView(wav);
        const wstr = (o, s) => { for (let i = 0; i < s.length; i++) view.setUint8(o + i, s.charCodeAt(i)); };
        wstr(0, "RIFF"); view.setUint32(4, 36 + dataSize, true); wstr(8, "WAVE");
        wstr(12, "fmt "); view.setUint32(16, 16, true); view.setUint16(20, 1, true); view.setUint16(22, 1, true);
        view.setUint32(24, 16000, true); view.setUint32(28, 32000, true);
        view.setUint16(32, 2, true); view.setUint16(34, 16, true);
        wstr(36, "data"); view.setUint32(40, dataSize, true);
        let off = 44;
        for (let i = 0; i < outLen; i++) {
            const s = Math.max(-1, Math.min(1, out[i]));
            view.setInt16(off, s < 0 ? s * 0x8000 : s * 0x7FFF, true);
            off += 2;
        }
        return new Blob([wav], { type: "audio/wav" });
    }

    async function runVoice() {
        if (busy) return;
        setBusy(true);
        const loading = appendBubble("正在识别语音…", "loading");
        try {
            const wav = await blobToWav16k(new Blob(chunks, { type: recorder ? recorder.mimeType : "audio/webm" }));
            const form = new FormData();
            form.append("file", wav, "voice.wav");
            const data = await api("/v1/voice/asr", { method: "POST", body: form });
            const text = (data.text || "").trim();
            loading.remove();
            if (!text) { appendBubble("没有听清，请再说一次。", "ai"); return; }
            appendBubble(text, "user");
            history.push({ role: "user", content: text });
            await runChat();
        } catch (err) {
            loading.remove();
            appendBubble("识别失败: " + err.message, "system");
        } finally {
            setBusy(false);
        }
    }

    // 朗读给定文本。btn 为绑定到该气泡上的朗读按钮，用于播放/暂停切换与状态展示。
    // 再次点击同一按钮会在播放/暂停间切换；点击其它气泡或「停止」按钮会停止当前朗读。
    async function speak(text, btn) {
        // 同一按钮再次点击 → 播放/暂停切换。
        if (currentAudio && currentBtn === btn) {
            if (currentAudio.paused) {
                currentAudio.play().catch(stopPlayback);
                btn.textContent = "⏸";
                btn.classList.remove("paused");
                btn.classList.add("playing");
                btn.title = "暂停";
            } else {
                currentAudio.pause();
                btn.textContent = "▶";
                btn.classList.remove("playing");
                btn.classList.add("paused");
                btn.title = "继续播放";
            }
            return;
        }
        // 切换到新音频：停掉上一段，再取新音频。
        stopPlayback();
        try {
            const data = await api("/v1/voice/tts", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ text: text })
            });
            const audio = new Audio(data.audio);
            currentAudio = audio;
            currentBtn = btn;
            btn.textContent = "⏸";
            btn.classList.add("playing");
            btn.classList.remove("paused");
            btn.title = "暂停";
            stopBtn.hidden = false;
            audio.onended = stopPlayback;
            audio.onerror = () => { stopPlayback(); appendBubble("朗读失败", "system"); };
            audio.play().catch(stopPlayback);
        } catch (err) {
            appendBubble("朗读失败: " + err.message, "system");
        }
    }

    function appendAiBubble(text) {
        const div = appendBubble(text, "ai");
        const btn = document.createElement("button");
        btn.className = "speakbtn";
        btn.textContent = "🔊";
        btn.title = "朗读";
        btn.onclick = () => speak(text, btn);
        div.appendChild(btn);
        return div;
    }

    async function runChat() {
        const loading = appendBubble("AI 思考中…", "loading");
        try {
            const data = await api("/v1/voice/chat", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ messages: history.slice(-12) })
            });
            loading.remove();
            const div = appendAiBubble(data.reply);
            history.push({ role: "assistant", content: data.reply });
            if (autoplay) speak(data.reply, div.querySelector("button"));
        } catch (err) {
            loading.remove();
            appendBubble("AI 调用失败: " + err.message, "system");
        }
    }

    async function sendText() {
        const text = textInput.value.trim();
        if (!text || busy) return;
        textInput.value = "";
        appendBubble(text, "user");
        history.push({ role: "user", content: text });
        setBusy(true);
        await runChat();
        setBusy(false);
    }

    micBtn.addEventListener("click", async () => {
        if (recorder) {
            recorder.stop();
            if (stopTimer) { clearTimeout(stopTimer); stopTimer = null; }
            return;
        }
        try {
            const stream = await navigator.mediaDevices.getUserMedia({ audio: true });
            chunks = [];
            recorder = new MediaRecorder(stream);
            recorder.ondataavailable = (e) => { if (e.data.size > 0) chunks.push(e.data); };
            recorder.onstop = () => {
                if (stopTimer) { clearTimeout(stopTimer); stopTimer = null; }
                recorder = null;
                stream.getTracks().forEach((t) => t.stop());
                micBtn.classList.remove("recording");
                micBtn.title = "点击开始/结束录音";
                runVoice();
            };
            recorder.start();
            micBtn.classList.add("recording");
            micBtn.title = "点击结束录音";
            stopTimer = setTimeout(() => { if (recorder) recorder.stop(); }, MAX_RECORD_MS);
        } catch (err) {
            appendBubble("无法使用麦克风: " + err.message, "system");
        }
    });

    sendBtn.addEventListener("click", sendText);
    textInput.addEventListener("keydown", (e) => { if (e.key === "Enter") sendText(); });

    api("/v1/voice/status").then((s) => {
        if (s.grok) { statusPill.textContent = "AI (" + (s.model || "") + ") 已就绪"; statusPill.classList.add("ok"); }
        else { statusPill.textContent = "AI 未配置（AI_API_KEY）"; }
    }).catch(() => { statusPill.textContent = "状态获取失败"; });
})();
</script>
</body>
</html>`;
    return new Response(body, {
        status: 200,
        headers: {
            "Content-Type": "text/html; charset=utf-8",
            "Cache-Control": "no-store",
            "Content-Security-Policy": "default-src 'none'; connect-src 'self'; style-src 'unsafe-inline'; img-src 'self' data:; media-src data:; script-src 'unsafe-inline'; base-uri 'none'; frame-ancestors 'none'",
            "Referrer-Policy": "no-referrer",
            "X-Content-Type-Options": "nosniff",
        },
    });
}

class VoiceError extends Error {
    constructor(message: string, readonly status: number) {
        super(message);
    }
}

/** Whisper ASR via Cloudflare Workers AI (large-v3-turbo); defaults to Chinese recognition. */
async function whisperTranscribe(env: Env, audio: ArrayBuffer): Promise<string> {
    try {
        // binding 会把 number[]/TypedArray 序列化成 base64 string 再转发给模型网关，
        // 而 turbo 的 schema 只接受 array/binary，实测显式传 base64 字符串才可通过校验（否则 5006）。
        const result = await env.AI.run("@cf/openai/whisper-large-v3-turbo", {
            audio: bytesToBase64(new Uint8Array(audio)),
            language: env.AI_ASR_LANGUAGE || "zh",
        } as any);
        const text = (result as { text?: string }).text ?? "";
        return text.trim();
    } catch (err) {
        const detail = err instanceof Error ? err.message : String(err);
        console.error("Whisper ASR failed", detail);
        throw new VoiceError(`ASR 服务暂不可用: ${detail.slice(0, 200)}`, 503);
    }
}

interface ChatMessage {
    role: "system" | "user" | "assistant";
    content: string;
}

function validateMessages(value: unknown): ChatMessage[] | null {
    if (!Array.isArray(value) || value.length === 0 || value.length > 20) return null;
    const messages: ChatMessage[] = [];
    for (const item of value) {
        if (!item || typeof item !== "object" || Array.isArray(item)) return null;
        const role = (item as { role?: unknown }).role;
        const content = (item as { content?: unknown }).content;
        if (role !== "user" && role !== "assistant" && role !== "system") return null;
        if (typeof content !== "string" || content.length === 0 || content.length > 4000) return null;
        messages.push({ role, content });
    }
    return messages;
}

const AI_DEFAULT_BASE_URL = "https://grok.yanyun.asia/v1";
const AI_DEFAULT_MODEL = "grok-chat-fast";
// Built-in voice-assistant system prompt, used when AI_SYSTEM_PROMPT is not set.
const AI_DEFAULT_SYSTEM_PROMPT =
    "你是一个内置在智能语音护照设备里的中文语音助手，名字叫「跃云」。请用中文回答，语气简洁、可靠、略带温度。回答控制在 3 句话以内，适合在设备的窄屏上显示；除非用户明确要求，否则不要使用标题、列表、Markdown 或换行。若用户询问设备状态、时间或出行信息，给出清楚直接的回答；涉及安全或敏感话题时，给出审慎、负责的建议。";
const TTS_MAX_TEXT = 800;
// Edge TTS proxy Worker (OpenAI-compatible /v1/audio/speech) on tts.yanyun.asia;
// it handles the Edge WebSocket handshake server-side, so plain HTTPS fetch works.
const TTS_DEFAULT_BASE_URL = "https://tts.yanyun.asia/v1";
const TTS_DEFAULT_VOICE = "zh-CN-XiaoxiaoNeural";

function bytesToBase64(bytes: Uint8Array): string {
    let binary = "";
    for (let offset = 0; offset < bytes.length; offset += 0x8000) {
        binary += String.fromCharCode(...bytes.subarray(offset, offset + 0x8000));
    }
    return btoa(binary);
}

/**
 * TTS via the Edge TTS proxy Worker (OpenAI-compatible /v1/audio/speech).
 * Requires TTS_API_KEY (shared secret with the edgetts-proxy Worker).
 * Returns MP3 bytes.
 */
async function ttsEdge(env: Env, text: string): Promise<Uint8Array> {
    const apiKey = env.TTS_API_KEY;
    if (!apiKey) throw new VoiceError("TTS_API_KEY 未配置（需与 tts.yanyun.asia 共享密钥）", 503);
    const baseUrl = (env.TTS_BASE_URL || TTS_DEFAULT_BASE_URL).replace(/\/+$/u, "");
    const voice = env.TTS_VOICE || TTS_DEFAULT_VOICE;
    let res: Response;
    try {
        res = await fetch(`${baseUrl}/audio/speech`, {
            method: "POST",
            headers: { "Content-Type": "application/json", Authorization: `Bearer ${apiKey}` },
            body: JSON.stringify({ model: "tts-1", voice, input: text, stream: false }),
        });
    } catch (err) {
        console.error("Edge TTS fetch failed", err);
        throw new VoiceError("TTS 服务暂不可用", 503);
    }
    if (!res.ok) {
        const detail = await res.text().catch(() => "");
        console.error("Edge TTS error", res.status, detail);
        throw new VoiceError(`TTS 服务错误 (${res.status})`, 502);
    }
    const bytes = new Uint8Array(await res.arrayBuffer());
    if (bytes.length < 1000) throw new VoiceError("TTS 返回音频为空", 502);
    return bytes;
}

/** AI chat via an OpenAI-compatible gateway (default http://grok.yanyun.asia/v1). */
async function aiChat(env: Env, messages: ChatMessage[]): Promise<string> {
    const apiKey = env.AI_API_KEY || env.GROK_API_KEY;
    if (!apiKey) throw new VoiceError("AI_API_KEY 未配置", 503);
    const baseUrl = (env.AI_BASE_URL || AI_DEFAULT_BASE_URL).replace(/\/+$/u, "");
    const model = env.AI_MODEL || env.GROK_MODEL || AI_DEFAULT_MODEL;
    const payload: ChatMessage[] = [
        { role: "system", content: env.AI_SYSTEM_PROMPT || AI_DEFAULT_SYSTEM_PROMPT },
        ...messages,
    ];
    let res: Response;
    try {
        res = await fetch(`${baseUrl}/chat/completions`, {
            method: "POST",
            headers: { "Content-Type": "application/json", Authorization: `Bearer ${apiKey}` },
            body: JSON.stringify({ model, messages: payload, max_tokens: 800, temperature: 0.7 }),
        });
    } catch (err) {
        console.error("AI chat fetch failed", err);
        throw new VoiceError("AI 服务暂不可用", 503);
    }
    if (!res.ok) {
        console.error("AI gateway error", res.status, await res.text().catch(() => ""));
        throw new VoiceError(`AI 服务错误 (${res.status})`, 502);
    }
    const data = await res.json<{ choices?: Array<{ message?: { content?: string } }> }>();
    const reply = data.choices?.[0]?.message?.content?.trim() ?? "";
    if (!reply) throw new VoiceError("AI 返回为空", 502);
    return reply;
}

export async function handleVoice(request: Request, env: Env, url: URL): Promise<Response> {
    if (request.method === "GET" && url.pathname === "/voice") {
        const authorized = await voiceAuthorized(request, env);
        if (!authorized) {
            return new Response("Authentication required", {
                status: 401,
                headers: {
                    "WWW-Authenticate": 'Basic realm="Passport admin", charset="UTF-8"',
                    "Cache-Control": "no-store",
                },
            });
        }
        return voicePage();
    }

    if (!url.pathname.startsWith("/v1/voice/")) return json({ error: "not found" }, 404);
    if (!(await voiceAuthorized(request, env))) return json({ error: "unauthorized" }, 401);

    try {
        if (request.method === "GET" && url.pathname === "/v1/voice/status") {
            return json({
                ok: true,
                grok: Boolean(env.AI_API_KEY || env.GROK_API_KEY),
                model: env.AI_MODEL || env.GROK_MODEL || AI_DEFAULT_MODEL,
                base_url: env.AI_BASE_URL || AI_DEFAULT_BASE_URL,
                tts: Boolean(env.TTS_API_KEY),
                tts_base_url: env.TTS_BASE_URL || TTS_DEFAULT_BASE_URL,
                voice: env.TTS_VOICE || TTS_DEFAULT_VOICE,
            });
        }

        if (request.method === "POST" && url.pathname === "/v1/voice/asr") {
            const form = await request.formData().catch(() => null);
            const file = form?.get("file");
            if (!file || typeof file === "string") return json({ error: "missing audio file" }, 400);
            const audio = await file.arrayBuffer();
            if (!audio || audio.byteLength === 0) return json({ error: "empty audio" }, 400);
            if (audio.byteLength > 2_000_000) {
                return json({ error: "音频过大（单次请控制在 30 秒内）" }, 400);
            }
            const text = await whisperTranscribe(env, audio);
            return json({ ok: true, text });
        }

        if (request.method === "POST" && url.pathname === "/v1/voice/tts") {
            const body = await request.json<{ text?: string }>().catch(() => null);
            const text = typeof body?.text === "string" ? body.text.trim() : "";
            if (!text || text.length > TTS_MAX_TEXT) return json({ error: "invalid text" }, 400);
            let audio: Uint8Array;
            const t0 = Date.now();
            try {
                audio = await ttsEdge(env, text);
            } catch (err) {
                if (err instanceof VoiceError) throw err;
                const detail = err instanceof Error ? err.message : String(err);
                console.error("Edge TTS failed", detail);
                throw new VoiceError(`TTS 服务暂不可用: ${detail.slice(0, 120)}`, 503);
            }
            console.log(`[voice latency] tts(synthesis): ${Date.now() - t0}ms, chars=${text.length}`);
            // 固件 Chat 用 Accept: audio/mpeg 直接拿 MP3 字节流，避免 base64 大缓冲。
            if (request.headers.get("Accept")?.includes("audio/mpeg")) {
                return new Response(audio, {
                    headers: { "Content-Type": "audio/mpeg", "Cache-Control": "no-store" },
                });
            }
            return json({ ok: true, source: "edgetts", audio: `data:audio/mpeg;base64,${bytesToBase64(audio)}` });
        }

        if (request.method === "POST" && url.pathname === "/v1/voice/chat") {
            const body = await request.json<{ messages?: unknown }>().catch(() => null);
            const messages = validateMessages(body?.messages);
            if (!messages) return json({ error: "invalid messages" }, 400);
            const t0 = Date.now();
            const reply = await aiChat(env, messages);
            console.log(`[voice latency] chat: ${Date.now() - t0}ms`);
            return json({ ok: true, reply });
        }

        return json({ error: "not found" }, 404);
    } catch (err) {
        if (err instanceof VoiceError) return json({ error: err.message }, err.status);
        console.error("Voice handler error", err);
        return json({ error: "voice unavailable" }, 503);
    }
}
