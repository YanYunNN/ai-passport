import { createRemoteJWKSet, jwtVerify } from "jose";
import type { Env } from "./env";

export interface AccessIdentity {
    subject: string;
    email: string;
}

function configuration(env: Env): { issuer: string; audience: string; jwks: URL } | null {
    const domain = env.ACCESS_TEAM_DOMAIN?.trim();
    const audience = env.ACCESS_AUD?.trim();
    // Accept only a team-domain hostname. This prevents a configuration typo from
    // redirecting JWT key retrieval to an arbitrary endpoint.
    if (!domain || !audience || !/^[A-Za-z0-9.-]+$/u.test(domain)) return null;
    const issuer = `https://${domain.toLowerCase()}`;
    return { issuer, audience, jwks: new URL("/cdn-cgi/access/certs", issuer) };
}

export async function verifyAccessAssertion(request: Request, env: Env): Promise<AccessIdentity | null> {
    const config = configuration(env);
    const assertion = request.headers.get("Cf-Access-Jwt-Assertion");
    if (!config || !assertion || assertion.length > 16_384) return null;
    try {
        const { payload } = await jwtVerify(assertion, createRemoteJWKSet(config.jwks), {
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
