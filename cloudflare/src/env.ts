export interface Env {
    ADMIN_API_KEY: string;
    HOOK_AUTH_SECRET: string;
    DEVICE_CREDENTIAL_PEPPER: string;
    ADMIN_UI_USERNAME: string;
    ADMIN_UI_PASSWORD: string;
    DB: D1Database;
    PASSPORTS: DurableObjectNamespace<PassportRelay>;
    AI: Ai;
    // Optional info-wallpaper configuration (weather source city/coords).
    WALLPAPER_LAT?: string;
    WALLPAPER_LON?: string;
    WALLPAPER_CITY?: string;
    // Voice-assistant AI: OpenAI-compatible gateway (e.g. http://grok.yanyun.asia/v1).
    // Key must be set via wrangler secret; GROK_* names are kept as legacy fallbacks.
    AI_BASE_URL?: string;
    AI_API_KEY?: string;
    AI_MODEL?: string;
    AI_ASR_LANGUAGE?: string;
    TTS_VOICE?: string;
    GROK_API_KEY?: string;
    GROK_MODEL?: string;
}

// Kept as a type-only import to avoid a runtime circular dependency.
import type { PassportRelay } from "./passport-relay";
