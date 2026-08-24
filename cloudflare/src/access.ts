import { createRemoteJWKSet, jwtVerify } from "jose";
import type { Env } from "./env";

export interface AccessIdentity {
    subject: string;
    email: string;
}

function configuration(env: Env): { issuer: string; audience: string; jwks: URL } | null {
    const configuredDomain = env.ACCESS_TEAM_DOMAIN?.trim();
    const audience = env.ACCESS_AUD?.trim();
    if (!configuredDomain || !audience) return null;
    try {
        const issuer = configuredDomain.startsWith("https://") ? configuredDomain.replace(/\/$/u, "") :
            `https://${configuredDomain}`;
        const parsed = new URL(issuer);
        // Reject paths, credentials, ports and non-HTTPS values so a configuration typo
        // cannot redirect JWK retrieval away from the intended Access team domain.
        if (parsed.protocol !== "https:" || parsed.pathname !== "/" || parsed.search || parsed.hash ||
            parsed.username || parsed.password || parsed.port || !/^[A-Za-z0-9.-]+$/u.test(parsed.hostname)) return null;
        return { issuer, audience, jwks: new URL("/cdn-cgi/access/certs", issuer) };
    } catch {
        return null;
    }
}

const remoteJwkSets = new Map<string, ReturnType<typeof createRemoteJWKSet>>();

function remoteJwkSet(jwks: URL): ReturnType<typeof createRemoteJWKSet> {
    const cached = remoteJwkSets.get(jwks.href);
    if (cached) return cached;
    const resolver = createRemoteJWKSet(jwks);
    remoteJwkSets.set(jwks.href, resolver);
    return resolver;
}

export async function verifyAccessAssertion(request: Request, env: Env): Promise<AccessIdentity | null> {
    const config = configuration(env);
    const assertion = request.headers.get("Cf-Access-Jwt-Assertion");
    if (!config || !assertion || assertion.length > 16_384) return null;
    try {
        const { payload } = await jwtVerify(assertion, remoteJwkSet(config.jwks), {
            issuer: config.issuer,
            audience: config.audience,
            algorithms: ["RS256"],
        });
        if (typeof payload.sub !== "string" || !payload.sub || typeof payload.email !== "string" || !payload.email) {
            return null;
        }
        return { subject: payload.sub, email: payload.email };
    } catch {
        // Access assertions and configuration errors are intentionally indistinguishable to callers.
        return null;
    }
}
