export interface Env {
    ADMIN_API_KEY: string;
    HOOK_AUTH_SECRET: string;
    DEVICE_CREDENTIAL_PEPPER: string;
    ADMIN_UI_USERNAME: string;
    ADMIN_UI_PASSWORD: string;
    DB: D1Database;
    PASSPORTS: DurableObjectNamespace<PassportRelay>;
    // Optional info-wallpaper configuration (weather source city/coords).
    WALLPAPER_LAT?: string;
    WALLPAPER_LON?: string;
    WALLPAPER_CITY?: string;
}

// Kept as a type-only import to avoid a runtime circular dependency.
import type { PassportRelay } from "./passport-relay";
