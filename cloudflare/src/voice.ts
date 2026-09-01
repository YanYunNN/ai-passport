// Standalone voice-assistant module: a self-contained web page for recording
// audio and chatting with an AI (Grok via xAI API), plus the ASR pipeline
// (Whisper via Cloudflare Workers AI). TTS goes through the Edge TTS proxy
// Worker on tts.yanyun.asia (OpenAI-compatible POST /v1/audio/speech).
import { bearerToken, hasBearerSecret, verifyAdminBasicAuth, verifyDeviceCredential } from "./auth";
import { writeVoiceLog, listVoiceLogs } from "./db";
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
    if (deviceId && credential) {
        const verified = await verifyDeviceCredential(env, deviceId, credential);
        if (verified) return true;
        console.warn(`voiceAuthorized: device auth failed for deviceId=${deviceId}`);
    } else if (deviceId || credential) {
        console.warn(`voiceAuthorized: partial device headers (deviceId=${deviceId}, hasCred=${!!credential})`);
    }
    const adminUser = await verifyAdminBasicAuth(request, env);
    if (adminUser) return true;
    console.warn(`voiceAuthorized: unauthorized request to ${request.url}`);
    return false;
}

const VOICE_PAGE_CSS = `
:root { --bg:#080a10; --panel:#0e1117; --border:rgba(148,163,184,0.14); --text:#e7ecf3; --muted:#8b94a3; --accent:#6366f1; --accent2:#22d3ee; --ok:#4ade80; --err:#f87171; }
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
.cfgbtn { background:rgba(148,163,184,0.1); border:1px solid var(--border); color:var(--text); border-radius:999px; width:34px; height:34px; cursor:pointer; font-size:1rem; flex:none; }
.cfgbtn:hover { background:rgba(148,163,184,0.22); }
.logbtn { background:rgba(148,163,184,0.1); border:1px solid var(--border); color:var(--text); border-radius:999px; padding:0.25rem 0.7rem; cursor:pointer; font-size:0.78rem; display:flex; align-items:center; gap:0.3rem; }
.logbtn:hover { background:rgba(148,163,184,0.22); color:#22d3ee; border-color:rgba(34,211,238,0.4); }
.cfgpanel { position:fixed; top:62px; right:16px; width:300px; max-width:calc(100vw - 32px); background:var(--panel); border:1px solid var(--border); border-radius:14px; padding:1rem 1.1rem 1.1rem; z-index:20; box-shadow:0 18px 48px -12px rgba(0,0,0,0.75); display:none; }
.cfgpanel.open { display:block; animation:pop .18s ease; }
.logpanel { position:fixed; top:0; right:0; width:440px; max-width:100vw; height:100vh; background:var(--panel); border-left:1px solid var(--border); z-index:30; box-shadow:-12px 0 36px rgba(0,0,0,0.6); display:flex; flex-direction:column; transform:translateX(100%); transition:transform .22s cubic-bezier(0.16,1,0.3,1); }
.logpanel.open { transform:translateX(0); }
.logheader { padding:0.9rem 1.1rem; border-bottom:1px solid var(--border); display:flex; align-items:center; justify-content:space-between; }
.logtitle { font-weight:700; font-size:0.98rem; display:flex; align-items:center; gap:0.4rem; }
.loglist { flex:1; overflow-y:auto; padding:0.9rem; display:flex; flex-direction:column; gap:0.8rem; }
.logitem { background:rgba(255,255,255,0.03); border:1px solid var(--border); border-radius:10px; padding:0.75rem; font-size:0.82rem; }
.logitem.err { border-color:rgba(248,113,113,0.35); background:rgba(248,113,113,0.05); }
.logitem-top { display:flex; justify-content:space-between; align-items:center; margin-bottom:0.4rem; }
.logbadge { font-size:0.7rem; padding:0.15rem 0.5rem; border-radius:999px; font-weight:600; }
.logbadge.ok { background:rgba(74,222,128,0.15); color:var(--ok); border:1px solid rgba(74,222,128,0.3); }
.logbadge.err { background:rgba(248,113,113,0.15); color:var(--err); border:1px solid rgba(248,113,113,0.3); }
.logtime { font-size:0.72rem; color:var(--muted); }
.logmetrics { display:flex; gap:0.6rem; flex-wrap:wrap; font-size:0.72rem; color:var(--accent2); margin-bottom:0.4rem; padding-bottom:0.3rem; border-bottom:1px dashed var(--border); }
.logdev { color:var(--muted); font-size:0.7rem; margin-left:auto; }
.logbubble { margin-top:0.3rem; padding:0.4rem 0.6rem; border-radius:6px; font-size:0.8rem; }
.logbubble.u { background:rgba(99,102,241,0.12); color:#c7d2fe; }
.logbubble.a { background:rgba(255,255,255,0.05); color:#e2e8f0; margin-top:0.25rem; }
.cfgrow { display:flex; flex-direction:column; gap:0.3rem; margin-bottom:0.9rem; }
.cfgrow label { font-size:0.78rem; color:var(--muted); }
.cfgrow select, .cfgrow input[type=text] { background:rgba(255,255,255,0.05); border:1px solid var(--border); border-radius:8px; padding:0.5rem 0.6rem; color:var(--text); font-size:0.88rem; outline:none; width:100%; }
.cfgrow select:focus, .cfgrow input[type=text]:focus { border-color:rgba(129,140,248,0.65); }
.cfgrow input[type=range] { width:100%; accent-color:var(--accent); cursor:pointer; }
.cfgval { float:right; font-weight:600; color:var(--accent2); }
.cfgbtns { display:flex; gap:0.6rem; margin-top:0.4rem; }
.cfgbtn-act { flex:1; background:var(--accent); color:#fff; border:none; border-radius:10px; padding:0.62rem; cursor:pointer; font-weight:600; }
.cfgbtn-act:hover { background:#818cf8; }
.cfgbtn-sec { flex:1; background:rgba(148,163,184,0.12); color:var(--text); border:1px solid var(--border); border-radius:10px; padding:0.62rem; cursor:pointer; }
.cfgbtn-sec:hover { background:rgba(148,163,184,0.22); }
.cfgbtn-act:disabled, .cfgbtn-sec:disabled { opacity:0.5; cursor:not-allowed; }
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
    <button class="logbtn" id="logBtn" title="查看对话与录音追溯日志">📋 日志</button>
    <button class="status" id="ttsToggle" title="自动朗读 AI 回复">🔊 自动朗读</button>
    <button class="stopbtn" id="stopBtn" title="停止朗读" hidden>⏹ 停止</button>
    <div class="status" id="statusPill">检测中…</div>
    <button class="cfgbtn" id="cfgBtn" title="TTS 设置">⚙️</button>
</div>
<div class="cfgpanel" id="cfgPanel">
    <div class="cfgrow">
        <label>音色 <span class="cfgval" id="voiceVal"></span></label>
        <select id="voiceSel"></select>
        <input type="text" id="voiceInput" placeholder="或直接输入音色名，如 zh-CN-YunxiNeural" spellcheck="false" autocomplete="off">
    </div>
    <div class="cfgrow">
        <label>语速 <span class="cfgval" id="speedVal"></span></label>
        <input type="range" id="speedRange" min="0.25" max="2" step="0.05">
    </div>
    <div class="cfgrow">
        <label>音调 <span class="cfgval" id="pitchVal"></span></label>
        <input type="range" id="pitchRange" min="0.5" max="1.5" step="0.05">
    </div>
    <div class="cfgrow">
        <label>风格 <span class="cfgval" id="styleVal"></span></label>
        <select id="styleSel"></select>
    </div>
    <div class="cfgrow">
        <label>输出方式 <span class="cfgval" id="streamVal"></span></label>
        <select id="streamSel">
            <option value="standard">标准（完整音频）</option>
            <option value="stream">流式（Stream）</option>
        </select>
        <span style="font-size:0.7rem; color:var(--muted);">流式：Edge 边合成边发送，页面边下边播（不支持 MSE 时自动降级为缓冲播放）</span>
    </div>
    <div class="cfgbtns">
        <button class="cfgbtn-sec" id="previewBtn">🔊 试听</button>
        <button class="cfgbtn-act" id="saveCfgBtn">保存</button>
    </div>
</div>
<div class="chat" id="chat">
    <div class="bubble system">按住麦克风说话，或直接输入文字；识别后交给 AI 回答。</div>
</div>
<div class="logpanel" id="logPanel">
    <div class="logheader">
        <div class="logtitle">📋 <span>对话与录音追踪</span></div>
        <div style="display:flex; gap:0.4rem;">
            <button class="logbtn" id="refreshLogBtn">🔄 刷新</button>
            <button class="logbtn" id="closeLogBtn">✕</button>
        </div>
    </div>
    <div class="loglist" id="logList">
        <div style="text-align:center; color:var(--muted); font-size:0.8rem; margin-top:2rem;">加载中…</div>
    </div>
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
    const logBtn = document.getElementById("logBtn");
    const logPanel = document.getElementById("logPanel");
    const closeLogBtn = document.getElementById("closeLogBtn");
    const refreshLogBtn = document.getElementById("refreshLogBtn");
    const logList = document.getElementById("logList");

    logBtn.addEventListener("click", () => {
        logPanel.classList.toggle("open");
        if (logPanel.classList.contains("open")) loadLogs();
    });
    closeLogBtn.addEventListener("click", () => logPanel.classList.remove("open"));
    refreshLogBtn.addEventListener("click", () => loadLogs());

    function formatTime(unixSec) {
        if (!unixSec) return "--";
        const d = new Date(unixSec * 1000);
        return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
    }

    async function loadLogs() {
        logList.innerHTML = '<div style="text-align:center; color:var(--muted); font-size:0.8rem; margin-top:2rem;">加载中…</div>';
        try {
            const data = await api("/v1/voice/logs?limit=40");
            if (!data.logs || data.logs.length === 0) {
                logList.innerHTML = '<div style="text-align:center; color:var(--muted); font-size:0.8rem; margin-top:2rem;">暂无对话日志</div>';
                return;
            }
            logList.innerHTML = "";
            for (const item of data.logs) {
                const div = document.createElement("div");
                const isOk = item.status === "success";
                div.className = "logitem" + (isOk ? "" : " err");
                div.innerHTML = \`
                    <div class="logitem-top">
                        <span class="logbadge \${isOk ? 'ok' : 'err'}">\${item.status}</span>
                        <span class="logtime">\${formatTime(item.created_at)}</span>
                        <span class="logdev">\${item.device_id || 'unknown'}</span>
                    </div>
                    <div class="logmetrics">
                        <span>总耗时: <b>\${item.latency_total_ms || 0}ms</b></span>
                        <span>ASR: \${item.latency_asr_ms || 0}ms</span>
                        <span>LLM: \${item.latency_chat_ms || 0}ms</span>
                        <span>TTS: \${item.latency_tts_ms || 0}ms</span>
                        \${item.audio_bytes ? '<span>音频: ' + Math.round(item.audio_bytes/1024) + 'KB</span>' : ''}
                    </div>
                    \${item.asr_text ? '<div class="logbubble u">🗣️ ' + escapeHtml(item.asr_text) + '</div>' : ''}
                    \${item.ai_reply ? '<div class="logbubble a">🤖 ' + escapeHtml(item.ai_reply) + '</div>' : ''}
                    \${item.error_msg ? '<div style="color:var(--err); margin-top:0.3rem; font-size:0.75rem;">⚠️ ' + escapeHtml(item.error_msg) + '</div>' : ''}
                \`;
                logList.appendChild(div);
            }
        } catch (err) {
            logList.innerHTML = '<div style="text-align:center; color:var(--err); font-size:0.8rem; margin-top:2rem;">日志加载失败: ' + err.message + '</div>';
        }
    }

    function escapeHtml(str) {
        return String(str).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
    }

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

    // ---------- TTS 设置（右上角 ⚙️，localStorage 持久化） ----------
    const TTS_ZH_VOICES = [
        ["zh-CN-XiaoxiaoNeural", "晓晓 · 女声 温柔"],
        ["zh-CN-XiaoyiNeural", "晓伊 · 女声 活泼"],
        ["zh-CN-YunxiNeural", "云希 · 男声 阳光"],
        ["zh-CN-YunyangNeural", "云扬 · 男声 专业"],
        ["zh-CN-YunjianNeural", "云健 · 男声 浑厚"],
        ["zh-CN-YunxiaNeural", "云夏 · 男声 少年"],
        ["zh-CN-YunfengNeural", "云枫 · 男声 磁性"],
        ["zh-CN-XiaochenNeural", "晓辰 · 女声 温润"],
        ["zh-CN-XiaohanNeural", "晓涵 · 女声 温暖"],
        ["zh-CN-XiaomoNeural", "晓墨 · 女声 解说"],
        ["zh-CN-XiaoruiNeural", "晓睿 · 女声 细腻"],
        ["zh-CN-XiaoshuangNeural", "晓双 · 童声 可爱"],
        ["zh-CN-XiaoxuanNeural", "晓萱 · 女声 亲和"],
        ["zh-CN-XiaoyanNeural", "晓颜 · 女声 柔和"],
        ["zh-CN-XiaoyouNeural", "晓悠 · 女声 温暖"],
        ["zh-CN-XiaozhenNeural", "晓甄 · 女声 自信"],
        ["zh-CN-YunzeNeural", "云泽 · 男声 青涩"],
        ["zh-CN-liaoning-XiaobeiNeural", "晓北 · 东北话 女声"],
        ["zh-CN-shaanxi-XiaoniNeural", "晓妮 · 陕西话 女声"],
    ];
    const TTS_STYLES = [
        ["", "无"],
        ["assistant", "助理"],
        ["chat", "聊天"],
        ["cheerful", "欢快"],
        ["newscast", "新闻播报"],
        ["calm", "平静"],
        ["gentle", "温柔"],
        ["affectionate", "深情"],
        ["sad", "悲伤"],
        ["serious", "严肃"],
        ["lyrical", "抒情"],
        ["narration-professional", "专业解说"],
        ["sports-commentary", "体育解说"],
    ];
    const TTS_STORAGE_KEY = "kiroVoiceTtsConfig";
    let ttsCfg = { voice: "", speed: 1.0, pitch: 1.0, style: "", stream: false };
    try {
        const saved = JSON.parse(localStorage.getItem(TTS_STORAGE_KEY) || "null");
        if (saved && typeof saved === "object") ttsCfg = Object.assign(ttsCfg, saved);
    } catch (e) {}

    const cfgPanel = document.getElementById("cfgPanel");
    const cfgBtn = document.getElementById("cfgBtn");
    const voiceSel = document.getElementById("voiceSel");
    const voiceInput = document.getElementById("voiceInput");
    const voiceVal = document.getElementById("voiceVal");
    const speedRange = document.getElementById("speedRange");
    const speedVal = document.getElementById("speedVal");
    const pitchRange = document.getElementById("pitchRange");
    const pitchVal = document.getElementById("pitchVal");
    const styleSel = document.getElementById("styleSel");
    const styleVal = document.getElementById("styleVal");
    const streamSel = document.getElementById("streamSel");
    const streamVal = document.getElementById("streamVal");

    function fillVoiceSelect() {
        voiceSel.innerHTML = "";
        TTS_ZH_VOICES.forEach(function (pair) {
            const opt = document.createElement("option");
            opt.value = pair[0];
            opt.textContent = pair[1];
            voiceSel.appendChild(opt);
        });
    }
    function fillStyleSelect() {
        styleSel.innerHTML = "";
        TTS_STYLES.forEach(function (pair) {
            const opt = document.createElement("option");
            opt.value = pair[0];
            opt.textContent = pair[1];
            styleSel.appendChild(opt);
        });
    }
    function applyCfgUI() {
        ttsCfg.voice = voiceInput.value.trim();
        ttsCfg.speed = parseFloat(speedRange.value) || 1.0;
        ttsCfg.pitch = parseFloat(pitchRange.value) || 1.0;
        ttsCfg.style = styleSel.value;
        ttsCfg.stream = streamSel.value === "stream";
    }
    function syncCfgUI() {
        voiceInput.value = ttsCfg.voice || "";
        const match = Array.prototype.find.call(voiceSel.options, function (o) { return o.value === ttsCfg.voice; });
        voiceSel.value = match ? ttsCfg.voice : "";
        speedRange.value = String(ttsCfg.speed);
        pitchRange.value = String(ttsCfg.pitch);
        styleSel.value = ttsCfg.style;
        streamSel.value = ttsCfg.stream ? "stream" : "standard";
        voiceVal.textContent = ttsCfg.voice || "默认";
        speedVal.textContent = ttsCfg.speed + "x";
        pitchVal.textContent = ttsCfg.pitch + "x";
        styleVal.textContent = ttsCfg.style ? ttsCfg.style : "无";
        streamVal.textContent = ttsCfg.stream ? "Stream" : "标准";
    }
    function ttsPayload(text) {
        const p = { text: text };
        if (ttsCfg.voice) p.voice = ttsCfg.voice;
        if (ttsCfg.speed && ttsCfg.speed !== 1) p.speed = ttsCfg.speed;
        if (ttsCfg.pitch && ttsCfg.pitch !== 1) p.pitch = ttsCfg.pitch;
        if (ttsCfg.style) p.style = ttsCfg.style;
        if (ttsCfg.stream) p.stream = true;
        return p;
    }

    fillVoiceSelect();
    fillStyleSelect();
    voiceSel.addEventListener("change", function () { voiceInput.value = voiceSel.value; applyCfgUI(); syncCfgUI(); });
    voiceInput.addEventListener("input", function () { applyCfgUI(); syncCfgUI(); });
    speedRange.addEventListener("input", function () { applyCfgUI(); syncCfgUI(); });
    pitchRange.addEventListener("input", function () { applyCfgUI(); syncCfgUI(); });
    styleSel.addEventListener("change", function () { applyCfgUI(); syncCfgUI(); });
    streamSel.addEventListener("change", function () { applyCfgUI(); syncCfgUI(); });

    cfgBtn.addEventListener("click", function () {
        cfgPanel.classList.toggle("open");
        if (cfgPanel.classList.contains("open")) syncCfgUI();
    });
    document.addEventListener("click", function (e) {
        if (cfgPanel.classList.contains("open") && !cfgPanel.contains(e.target) && e.target !== cfgBtn) {
            cfgPanel.classList.remove("open");
        }
    });
    const saveCfgBtn = document.getElementById("saveCfgBtn");
    saveCfgBtn.addEventListener("click", function () {
        applyCfgUI();
        try { localStorage.setItem(TTS_STORAGE_KEY, JSON.stringify(ttsCfg)); } catch (e) {}
        cfgPanel.classList.remove("open");
        statusPill.textContent = "TTS: " + (ttsCfg.voice || "默认");
    });
    const previewBtn = document.getElementById("previewBtn");
    previewBtn.addEventListener("click", async function () {
        previewBtn.disabled = true;
        try {
            await speak("你好，我是跃云。语音设置试听。", null);
        } catch (err) {
            appendBubble("试听失败: " + err.message, "system");
        } finally {
            previewBtn.disabled = false;
        }
    });

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
    // 播放开始后的公共 UI 状态（气泡按钮 / 停止键 / 结束钩子）。
    function startPlaybackUi(audio, btn) {
        currentAudio = audio;
        currentBtn = btn;
        if (btn) {
            btn.textContent = "⏸";
            btn.classList.add("playing");
            btn.classList.remove("paused");
            btn.title = "暂停";
        }
        stopBtn.hidden = false;
        audio.onended = stopPlayback;
        audio.onerror = () => { stopPlayback(); appendBubble("朗读失败", "system"); };
    }

    // 不支持 MSE 时的兜底：把流攒成 Blob 再播。
    async function bufferToBlob(body, btn) {
        const chunks = [];
        const reader = body.getReader();
        try {
            while (true) {
                const r = await reader.read();
                if (r.done) break;
                chunks.push(r.value);
            }
            const url = URL.createObjectURL(new Blob(chunks, { type: "audio/mpeg" }));
            const audio = new Audio(url);
            startPlaybackUi(audio, btn);
            audio.play().catch(stopPlayback);
        } catch (err) {
            appendBubble("朗读失败: " + err.message, "system");
        }
    }

    // 流式播放原始 MP3（ReadableStream）：支持 MSE 就边下边播，否则缓冲成 Blob。
    function playMp3Stream(body, btn) {
        let mseOk = false;
        try { mseOk = "MediaSource" in window && MediaSource.isTypeSupported("audio/mpeg"); } catch (e) { mseOk = false; }
        if (!mseOk) { bufferToBlob(body, btn); return; }

        const mediaSource = new MediaSource();
        const url = URL.createObjectURL(mediaSource);
        const audio = new Audio(url);
        startPlaybackUi(audio, btn);
        audio.play().catch(stopPlayback);
        mediaSource.addEventListener("sourceopen", () => {
            let sourceBuffer = null;
            try { sourceBuffer = mediaSource.addSourceBuffer("audio/mpeg"); }
            catch (e) { bufferToBlob(body, btn); return; }
            const reader = body.getReader();
            (async () => {
                try {
                    while (true) {
                        const r = await reader.read();
                        if (r.done) break;
                        if (sourceBuffer.updating) {
                            await new Promise((res) => sourceBuffer.addEventListener("updateend", res, { once: true }));
                        }
                        sourceBuffer.appendBuffer(r.value);
                        await new Promise((res) => sourceBuffer.addEventListener("updateend", res, { once: true }));
                    }
                    if (mediaSource.readyState === "open") mediaSource.endOfStream();
                } catch (err) {
                    stopPlayback();
                    appendBubble("流式朗读失败: " + err.message, "system");
                }
            })();
        });
    }

    async function speak(text, btn) {
        // 同一按钮再次点击 → 播放/暂停切换。
        if (currentAudio && currentBtn === btn) {
            if (currentAudio.paused) {
                currentAudio.play().catch(stopPlayback);
                if (btn) { btn.textContent = "⏸"; btn.classList.remove("paused"); btn.classList.add("playing"); btn.title = "暂停"; }
            } else {
                currentAudio.pause();
                if (btn) { btn.textContent = "▶"; btn.classList.remove("playing"); btn.classList.add("paused"); btn.title = "继续播放"; }
            }
            return;
        }
        // 切换到新音频：停掉上一段，再取新音频。
        stopPlayback();
        try {
            const res = await fetch("/v1/voice/tts", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify(ttsPayload(text))
            });
            if (!res.ok) {
                let msg = "HTTP " + res.status;
                try { const d = await res.json(); if (d && d.error) msg = d.error; } catch (e) {}
                throw new Error(msg);
            }
            const ct = res.headers.get("Content-Type") || "";
            if (ct.indexOf("json") >= 0) {
                // 标准模式：JSON data URL
                const data = await res.json();
                const audio = new Audio(data.audio);
                startPlaybackUi(audio, btn);
                audio.play().catch(stopPlayback);
            } else {
                // 流式模式：原始 MP3 流
                playMp3Stream(res.body, btn);
            }
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
        // 未保存过自定义设置时，跟随服务端默认音色。
        if (!localStorage.getItem(TTS_STORAGE_KEY) && s.voice) {
            ttsCfg.voice = s.voice;
        }
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
            "Content-Security-Policy": "default-src 'none'; connect-src 'self'; style-src 'unsafe-inline'; img-src 'self' data:; media-src data: blob:; script-src 'unsafe-inline'; base-uri 'none'; frame-ancestors 'none'",
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

interface TtsParams {
    voice?: string;
    speed?: number;
    pitch?: number;
    style?: string;
    stream?: boolean;
}

function clampNumber(value: unknown, min: number, max: number): number {
    if (typeof value !== "number" || !Number.isFinite(value)) return 1;
    return Math.min(max, Math.max(min, value));
}

function cleanTtsString(value: unknown, maxLength: number): string | undefined {
    if (typeof value !== "string") return undefined;
    const cleaned = value.trim();
    if (!cleaned || cleaned.length > maxLength || /[\r\n]/u.test(cleaned)) return undefined;
    return cleaned;
}

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
async function ttsEdge(env: Env, text: string, params: TtsParams = {}): Promise<Response> {
    const apiKey = env.TTS_API_KEY;
    if (!apiKey) throw new VoiceError("TTS_API_KEY 未配置（需与 tts.yanyun.asia 共享密钥）", 503);
    const baseUrl = (env.TTS_BASE_URL || TTS_DEFAULT_BASE_URL).replace(/\/+$/u, "");
    const voice = params.voice || env.TTS_VOICE || TTS_DEFAULT_VOICE;
    const speed = clampNumber(params.speed, 0.25, 2.0);
    const pitch = clampNumber(params.pitch, 0.5, 1.5);
    const stream = params.stream === true;
    const payload: Record<string, unknown> = {
        model: "tts-1", voice, input: text, stream, speed, pitch,
    };
    if (params.style) payload.style = params.style;
    // 30s 硬超时：上游流卡住时返回明确错误而不是无限等待。
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 30000);
    let res: Response;
    try {
        res = await fetch(`${baseUrl}/audio/speech`, {
            method: "POST",
            headers: { "Content-Type": "application/json", Authorization: `Bearer ${apiKey}` },
            body: JSON.stringify(payload),
            signal: controller.signal,
        });
    } catch (err) {
        clearTimeout(timer);
        console.error("Edge TTS fetch failed", err);
        throw new VoiceError("TTS 服务暂不可用或超时", 503);
    }
    clearTimeout(timer);
    if (!res.ok) {
        const detail = await res.text().catch(() => "");
        console.error("Edge TTS error", res.status, detail);
        throw new VoiceError(`TTS 服务错误 (${res.status})`, 502);
    }
    return res;
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

        if (request.method === "GET" && url.pathname === "/v1/voice/logs") {
            const deviceId = url.searchParams.get("device_id") || undefined;
            const limit = parseInt(url.searchParams.get("limit") || "50", 10);
            const logs = await listVoiceLogs(env, { deviceId, limit });
            return json({ ok: true, logs });
        }

        if (request.method === "POST" && url.pathname === "/v1/voice/asr") {
            const deviceId = request.headers.get("X-Device-Id") || "unknown";
            const form = await request.formData().catch(() => null);
            const file = form?.get("file");
            if (!file || typeof file === "string") return json({ error: "missing audio file" }, 400);
            const audio = await file.arrayBuffer();
            if (!audio || audio.byteLength === 0) return json({ error: "empty audio" }, 400);
            if (audio.byteLength > 2_000_000) {
                return json({ error: "音频过大（单次请控制在 30 秒内）" }, 400);
            }
            const t0 = Date.now();
            let text = "";
            try {
                text = await whisperTranscribe(env, audio);
                const dur = Date.now() - t0;
                console.log(JSON.stringify({ tag: "voice_asr", device_id: deviceId, duration_ms: dur, text }));
                return json({ ok: true, text });
            } catch (err: any) {
                const dur = Date.now() - t0;
                console.error(JSON.stringify({ tag: "voice_asr_error", device_id: deviceId, duration_ms: dur, error: err?.message }));
                throw err;
            }
        }

        if (request.method === "POST" && url.pathname === "/v1/voice/converse") {
            const deviceId = request.headers.get("X-Device-Id") || "web-user";
            const form = await request.formData().catch(() => null);
            const file = form?.get("file");
            if (!file || typeof file === "string") {
                await writeVoiceLog(env, {
                    device_id: deviceId,
                    session_id: null,
                    asr_text: null,
                    ai_reply: null,
                    audio_bytes: 0,
                    mp3_bytes: 0,
                    latency_asr_ms: 0,
                    latency_chat_ms: 0,
                    latency_tts_ms: 0,
                    latency_total_ms: 0,
                    status: "missing_audio",
                    error_msg: "missing audio file",
                    created_at: Math.floor(Date.now() / 1000),
                });
                return json({ error: "missing audio file" }, 400);
            }
            const audio = await file.arrayBuffer();
            if (!audio || audio.byteLength === 0) return json({ error: "empty audio" }, 400);
            if (audio.byteLength > 2_000_000) {
                return json({ error: "音频过大（单次请控制在 30 秒内）" }, 400);
            }
            const historyStr = form?.get("history");
            let history: ChatMessage[] = [];
            if (typeof historyStr === "string" && historyStr.length > 0) {
                try {
                    const parsed = JSON.parse(historyStr);
                    history = validateMessages(parsed) || [];
                } catch {
                    history = [];
                }
            }

            const t0 = Date.now();
            let asrText = "";
            let t1 = t0;
            try {
                asrText = await whisperTranscribe(env, audio);
                t1 = Date.now();
                console.log(JSON.stringify({
                    tag: "voice_trace",
                    stage: "asr",
                    device_id: deviceId,
                    duration_ms: t1 - t0,
                    audio_bytes: audio.byteLength,
                    text: asrText,
                }));
            } catch (err: any) {
                const dur = Date.now() - t0;
                await writeVoiceLog(env, {
                    device_id: deviceId,
                    session_id: null,
                    asr_text: null,
                    ai_reply: null,
                    audio_bytes: audio.byteLength,
                    mp3_bytes: 0,
                    latency_asr_ms: dur,
                    latency_chat_ms: 0,
                    latency_tts_ms: 0,
                    latency_total_ms: dur,
                    status: "asr_error",
                    error_msg: err?.message || String(err),
                    created_at: Math.floor(Date.now() / 1000),
                });
                throw err;
            }

            if (!asrText) {
                const dur = t1 - t0;
                await writeVoiceLog(env, {
                    device_id: deviceId,
                    session_id: null,
                    asr_text: "",
                    ai_reply: null,
                    audio_bytes: audio.byteLength,
                    mp3_bytes: 0,
                    latency_asr_ms: dur,
                    latency_chat_ms: 0,
                    latency_tts_ms: 0,
                    latency_total_ms: dur,
                    status: "asr_empty",
                    error_msg: "未识别到有效语音",
                    created_at: Math.floor(Date.now() / 1000),
                });
                return json({ error: "未识别到有效语音", asr: "" }, 400);
            }

            const messages: ChatMessage[] = [...history, { role: "user", content: asrText }];
            let reply = "";
            let t2 = t1;
            try {
                reply = await aiChat(env, messages);
                t2 = Date.now();
                console.log(JSON.stringify({
                    tag: "voice_trace",
                    stage: "chat",
                    device_id: deviceId,
                    duration_ms: t2 - t1,
                    reply_len: reply.length,
                    reply,
                }));
            } catch (err: any) {
                const tErr = Date.now();
                await writeVoiceLog(env, {
                    device_id: deviceId,
                    session_id: null,
                    asr_text: asrText,
                    ai_reply: null,
                    audio_bytes: audio.byteLength,
                    mp3_bytes: 0,
                    latency_asr_ms: t1 - t0,
                    latency_chat_ms: tErr - t1,
                    latency_tts_ms: 0,
                    latency_total_ms: tErr - t0,
                    status: "ai_error",
                    error_msg: err?.message || String(err),
                    created_at: Math.floor(Date.now() / 1000),
                });
                throw err;
            }

            const params: TtsParams = {
                speed: 1.0,
                pitch: 1.0,
                stream: false,
            };
            let mp3Bytes: Uint8Array;
            let t3 = t2;
            try {
                const ttsRes = await ttsEdge(env, reply, params);
                t3 = Date.now();
                mp3Bytes = new Uint8Array(await ttsRes.arrayBuffer());
                console.log(JSON.stringify({
                    tag: "voice_trace",
                    stage: "tts",
                    device_id: deviceId,
                    duration_ms: t3 - t2,
                    mp3_bytes: mp3Bytes.length,
                }));
            } catch (err: any) {
                const tErr = Date.now();
                await writeVoiceLog(env, {
                    device_id: deviceId,
                    session_id: null,
                    asr_text: asrText,
                    ai_reply: reply,
                    audio_bytes: audio.byteLength,
                    mp3_bytes: 0,
                    latency_asr_ms: t1 - t0,
                    latency_chat_ms: t2 - t1,
                    latency_tts_ms: tErr - t2,
                    latency_total_ms: tErr - t0,
                    status: "tts_error",
                    error_msg: err?.message || String(err),
                    created_at: Math.floor(Date.now() / 1000),
                });
                throw err;
            }

            const totalMs = t3 - t0;
            console.log(JSON.stringify({
                tag: "voice_converse_success",
                device_id: deviceId,
                asr_text: asrText,
                ai_reply: reply,
                audio_bytes: audio.byteLength,
                mp3_bytes: mp3Bytes.length,
                latencies: {
                    asr_ms: t1 - t0,
                    chat_ms: t2 - t1,
                    tts_ms: t3 - t2,
                    total_ms: totalMs,
                },
            }));

            await writeVoiceLog(env, {
                device_id: deviceId,
                session_id: null,
                asr_text: asrText,
                ai_reply: reply,
                audio_bytes: audio.byteLength,
                mp3_bytes: mp3Bytes.length,
                latency_asr_ms: t1 - t0,
                latency_chat_ms: t2 - t1,
                latency_tts_ms: t3 - t2,
                latency_total_ms: totalMs,
                status: "success",
                error_msg: null,
                created_at: Math.floor(Date.now() / 1000),
            });

            return new Response(mp3Bytes, {
                status: 200,
                headers: {
                    "Content-Type": "audio/mpeg",
                    "Cache-Control": "no-store",
                    "X-Asr-Text": encodeURIComponent(asrText),
                    "X-Ai-Reply": encodeURIComponent(reply),
                },
            });
        }

        if (request.method === "POST" && url.pathname === "/v1/voice/tts") {
            const body = await request
                .json<{ text?: string; voice?: string; speed?: number; pitch?: number; style?: string; stream?: boolean }>()
                .catch(() => null);
            const text = typeof body?.text === "string" ? body.text.trim() : "";
            if (!text || text.length > TTS_MAX_TEXT) return json({ error: "invalid text" }, 400);
            const params: TtsParams = {
                voice: cleanTtsString(body?.voice, 64),
                speed: clampNumber(body?.speed, 0.25, 2.0),
                pitch: clampNumber(body?.pitch, 0.5, 1.5),
                style: cleanTtsString(body?.style, 32),
                stream: body?.stream === true,
            };
            const t0 = Date.now();
            let ttsRes: Response;
            try {
                ttsRes = await ttsEdge(env, text, params);
            } catch (err) {
                if (err instanceof VoiceError) throw err;
                const detail = err instanceof Error ? err.message : String(err);
                console.error("Edge TTS failed", detail);
                throw new VoiceError(`TTS 服务暂不可用: ${detail.slice(0, 120)}`, 503);
            }
            console.log(`[voice latency] tts(synthesis): ${Date.now() - t0}ms, chars=${text.length}`);
            // 流式：原样透传 edgetts 的 MP3 流（不缓冲），页面用 MSE 边下边播，固件可流式接收。
            if (params.stream === true) {
                return new Response(ttsRes.body, {
                    headers: { "Content-Type": "audio/mpeg", "Cache-Control": "no-store" },
                });
            }
            const audio = new Uint8Array(await ttsRes.arrayBuffer());
            if (audio.length < 1000) throw new VoiceError("TTS 返回音频为空", 502);
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
