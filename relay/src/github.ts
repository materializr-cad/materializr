// GitHub App auth + issue creation. The app's private key is a Worker secret;
// it is used only to mint a short-lived installation token (cached in memory).

export interface GithubEnv {
  GITHUB_APP_ID: string;
  GITHUB_INSTALLATION_ID: string;
  GITHUB_APP_PRIVATE_KEY: string;
  GITHUB_OWNER: string;
  GITHUB_REPO: string;
}

export type FetchFn = (input: string, init?: RequestInit) => Promise<Response>;

// ---- key handling -----------------------------------------------------------------

function b64ToBytes(b64: string): Uint8Array {
  const bin = atob(b64);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

export function bytesToB64Url(b: Uint8Array): string {
  let s = "";
  for (const x of b) s += String.fromCharCode(x);
  return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

function derLen(n: number): number[] {
  if (n < 0x80) return [n];
  const bytes: number[] = [];
  for (let v = n; v > 0; v = Math.floor(v / 256)) bytes.unshift(v & 0xff);
  return [0x80 | bytes.length, ...bytes];
}

// GitHub hands out PKCS#1 ("BEGIN RSA PRIVATE KEY"); WebCrypto only imports
// PKCS#8. Wrap the PKCS#1 blob: SEQ { INT 0, SEQ { rsaEncryption OID, NULL }, OCTET STRING key }.
export function pkcs1ToPkcs8(pkcs1: Uint8Array): Uint8Array {
  const algId = [0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01, 0x05, 0x00];
  const version = [0x02, 0x01, 0x00];
  const octet = [0x04, ...derLen(pkcs1.length)];
  const innerLen = version.length + algId.length + octet.length + pkcs1.length;
  const header = [0x30, ...derLen(innerLen), ...version, ...algId, ...octet];
  const out = new Uint8Array(header.length + pkcs1.length);
  out.set(header, 0);
  out.set(pkcs1, header.length);
  return out;
}

export function pemToPkcs8(pem: string): Uint8Array {
  const isPkcs1 = /BEGIN RSA PRIVATE KEY/.test(pem);
  const body = pem.replace(/-----[^-]+-----/g, "").replace(/\s+/g, "");
  const der = b64ToBytes(body);
  return isPkcs1 ? pkcs1ToPkcs8(der) : der;
}

export async function signAppJwt(appId: string, pem: string, nowSec: number): Promise<string> {
  const key = await crypto.subtle.importKey(
    "pkcs8", pemToPkcs8(pem), { name: "RSASSA-PKCS1-v1_5", hash: "SHA-256" }, false, ["sign"]);
  const enc = (o: unknown) => bytesToB64Url(new TextEncoder().encode(JSON.stringify(o)));
  // iat backdated a minute for clock skew; GitHub allows at most 10 minutes of life.
  const signingInput = `${enc({ alg: "RS256", typ: "JWT" })}.${enc({ iat: nowSec - 60, exp: nowSec + 540, iss: appId })}`;
  const sig = new Uint8Array(await crypto.subtle.sign("RSASSA-PKCS1-v1_5", key, new TextEncoder().encode(signingInput)));
  return `${signingInput}.${bytesToB64Url(sig)}`;
}

// ---- API -----------------------------------------------------------------------------

const API = "https://api.github.com";
const HEADERS = {
  Accept: "application/vnd.github+json",
  "X-GitHub-Api-Version": "2022-11-28",
  "User-Agent": "materializr-report-relay",
};

let cached: { token: string; expiresMs: number } | null = null;
export function _resetTokenCache(): void { cached = null; }

export async function installationToken(env: GithubEnv, fetchFn: FetchFn, nowMs: number): Promise<string> {
  if (cached && cached.expiresMs - 5 * 60_000 > nowMs) return cached.token;
  const jwt = await signAppJwt(env.GITHUB_APP_ID, env.GITHUB_APP_PRIVATE_KEY, Math.floor(nowMs / 1000));
  const res = await fetchFn(`${API}/app/installations/${env.GITHUB_INSTALLATION_ID}/access_tokens`, {
    method: "POST",
    headers: { ...HEADERS, Authorization: `Bearer ${jwt}` },
    // Narrow the token to the one repo and the one permission this relay needs.
    body: JSON.stringify({ repositories: [env.GITHUB_REPO], permissions: { issues: "write" } }),
  });
  if (!res.ok) throw new Error(`installation token: HTTP ${res.status}`);
  const j = (await res.json()) as { token?: string; expires_at?: string };
  if (!j.token) throw new Error("installation token: empty response");
  cached = { token: j.token, expiresMs: j.expires_at ? Date.parse(j.expires_at) : nowMs + 50 * 60_000 };
  return j.token;
}

export interface CreatedIssue { number: number; html_url: string }

export async function createIssue(
  env: GithubEnv, fetchFn: FetchFn, nowMs: number,
  issue: { title: string; body: string; labels: string[] },
): Promise<CreatedIssue> {
  const token = await installationToken(env, fetchFn, nowMs);
  const res = await fetchFn(`${API}/repos/${env.GITHUB_OWNER}/${env.GITHUB_REPO}/issues`, {
    method: "POST",
    headers: { ...HEADERS, Authorization: `Bearer ${token}` },
    body: JSON.stringify(issue),
  });
  if (!res.ok) throw new Error(`create issue: HTTP ${res.status}`);
  const j = (await res.json()) as CreatedIssue;
  if (!j.html_url) throw new Error("create issue: no url in response");
  return { number: j.number, html_url: j.html_url };
}
