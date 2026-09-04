// Shared design-token layer for every web page served by the relay (admin
// console, voice assistant, web simulator). Values follow the Slash style
// system used as the visual reference: a near-black "midnight" canvas, a
// layered graphite surface stack with hairline borders instead of drop
// shadows, and a single warm copper accent. Functional status colors (ok /
// warn / err) are kept muted so they sit inside the palette.
export const UI_CSS = `
:root {
    /* canvas & surface stack */
    --void: #08080a;        /* page canvas */
    --onyx: #040406;        /* card surface (recessed) */
    --carbon: #121317;      /* panel: inputs, popovers, code */
    --graphite: #1c1d22;    /* primary borders / dividers */
    --slate: #2e3038;       /* strong borders, hover rows */
    --smoke: #464853;       /* tertiary borders */
    /* text ramp (darker = dimmer) */
    --ash: #5e616e;
    --steel: #777a88;
    --fog: #9194a1;         /* secondary text */
    --mist: #acafb9;
    --silver: #c7c9d1;
    --bone: #e2e3e9;        /* default text */
    --white: #ffffff;       /* scarce: primary action / headline */
    /* single warm accent */
    --copper: #cc9166;
    --copper-hi: #e0b28c;
    --copper-soft: rgba(204, 145, 102, 0.13);
    /* functional status colors (muted, low saturation) */
    --ok: #7ecf9f;
    --ok-bg: rgba(126, 207, 159, 0.12);
    --ok-line: rgba(126, 207, 159, 0.3);
    --warn: #d9b98a;
    --warn-bg: rgba(217, 185, 138, 0.12);
    --warn-line: rgba(217, 185, 138, 0.3);
    --err: #eb9090;
    --err-bg: rgba(235, 144, 144, 0.12);
    --err-line: rgba(235, 144, 144, 0.3);
    /* data-viz (gilded line) */
    --gold: #ae9357;
    --gold-hi: #d0b478;
    /* shape & type */
    --r-card: 10px;
    --r-pill: 9999px;
    --font-ui: "Inter", -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "PingFang SC", "Hiragino Sans GB", "Microsoft YaHei", "Helvetica Neue", Arial, sans-serif;
    --font-mono: ui-monospace, "SF Mono", SFMono-Regular, Menlo, Consolas, "Liberation Mono", monospace;
    /* legacy aliases so old class hooks and inline var(--*) styles keep working */
    --bg: var(--void);
    --card-bg: var(--onyx);
    --panel: var(--carbon);
    --border: rgba(226, 227, 233, 0.1);
    --text: var(--bone);
    --text-muted: var(--fog);
    --muted: var(--fog);
    --accent: var(--copper);
    --accent-hover: var(--copper-hi);
    --accent-soft: var(--copper-soft);
    --accent2: var(--copper);
    --radius: var(--r-card);
    --online: var(--ok);
    --offline: var(--ash);
    --danger: #b95852;
    --danger-hover: #c96862;
    --primary: var(--white);
    --primary-hover: var(--bone);
    --bg-main: var(--void);
    --bg-card: var(--onyx);
    --bg-card-header: var(--graphite);
    --border-color: var(--graphite);
    --border-focus: var(--copper);
    --text-primary: var(--bone);
    --text-accent: var(--copper);
    --color-success: var(--ok);
    --color-success-hover: #97dcb6;
    --color-danger: var(--err);
    --color-warning: var(--warn);
    --device-casing: linear-gradient(160deg, #17181d, #0b0c10);
    --device-border: #2e3038;
    --screen-bezel: #040406;
}
* { box-sizing: border-box; margin: 0; padding: 0; }
html { -webkit-text-size-adjust: 100%; }
body {
    font-family: var(--font-ui);
    background: var(--void);
    color: var(--bone);
    line-height: 1.5;
    min-height: 100vh;
    -webkit-font-smoothing: antialiased;
    text-rendering: optimizeLegibility;
}
::selection { background: rgba(204, 145, 102, 0.3); color: var(--white); }
* { scrollbar-width: thin; scrollbar-color: var(--slate) transparent; }
::-webkit-scrollbar { width: 10px; height: 10px; }
::-webkit-scrollbar-track { background: transparent; }
::-webkit-scrollbar-thumb { background: var(--slate); border: 2px solid transparent; border-radius: 8px; background-clip: content-box; }
::-webkit-scrollbar-thumb:hover { background: var(--smoke); border: 2px solid transparent; background-clip: content-box; }
button, input, select, textarea { font: inherit; color: inherit; }
:focus-visible { outline: 2px solid var(--copper); outline-offset: 2px; }
/* Inline SVG icons (emoji are banned in UI; stroke follows currentColor) */
.icon {
    display: inline-block;
    flex: none;
    width: 1.08em;
    height: 1.08em;
    vertical-align: -0.16em;
}
.brand .icon { width: 1.2em; height: 1.2em; color: var(--copper); }
h1 .icon { width: 1.08em; height: 1.08em; }
.btn .icon { width: 1.02em; height: 1.02em; }
.badge .icon { width: 0.95em; height: 0.95em; }
.stat-icon, .quick-icon { color: var(--silver); }
.stat-icon .icon { width: 1.45rem; height: 1.45rem; color: var(--copper-hi); }
.quick-icon .icon { width: 1.35rem; height: 1.35rem; color: var(--copper-hi); }
.micbtn .icon { width: 1.35rem; height: 1.35rem; }
.cfgbtn .icon, .logbtn .icon, .dropzone-icon { color: var(--steel); }
.dropzone-icon .icon { width: 1.9rem; height: 1.9rem; }
.logbubble .icon { width: 0.95em; height: 0.95em; }
@media (prefers-reduced-motion: reduce) {
    *, *::before, *::after { animation-duration: 0.01ms !important; animation-iteration-count: 1 !important; transition-duration: 0.01ms !important; }
}
`;
