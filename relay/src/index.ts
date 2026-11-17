// Materializr bug-report relay.
//
//   POST /v1/report   multipart from the app -> stores attachments, files the issue
//   GET  /f/<id>.png|.log   public screenshot / log (served inline in the issue)
//   GET  /p/<id>      private project file, maintainers only (Bearer token)
//   GET  /            health
//
// The app holds no credentials. The GitHub App's private key lives here as a
// secret; abuse limits (size, type, per-IP and global rate) are enforced here.

import {
  LIMITS, ID_RE, buildIssueBody, decodeLog, isGzip, isKind, isPng, labelsFor, newId,
  parseEnv, pngSize, redactPaths, sanitizeLine, sanitizeText,
} from "./validate.ts";
import { hashIp, type LimiterLike, type Verdict } from "./ratelimit.ts";
import { limiterClient } from "./limiter.ts";
export { Limiter } from "./limiter.ts";
import { createIssue, type FetchFn, type GithubEnv } from "./github.ts";

export interface R2Like {
  put(key: string, value: ArrayBuffer | Uint8Array | string, opts?: { httpMetadata?: { contentType?: string } }): Promise<unknown>;
  get(key: string): Promise<{ body: ReadableStream | null; httpMetadata?: { contentType?: string } } | null>;
  delete(key: string): Promise<void>;
}

export interface Env extends GithubEnv {
  LIMITER: { idFromName(n: string): unknown; get(id: unknown): { fetch(r: Request): Promise<Response> } };
  PUBLIC_FILES: R2Like;
  PRIVATE_FILES: R2Like;
  MAINTAINER_TOKEN: string;
  IP_SALT: string;
  DRY_RUN?: string;
  DISABLED?: string;
}

export interface Deps { fetch: FetchFn; now: () => number; limiter?: LimiterLike }

const SECURITY = {
  "X-Content-Type-Options": "nosniff",
  "Referrer-Policy": "no-referrer",
};

function json(status: number, body: unknown, extra: Record<string, string> = {}): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json; charset=utf-8", "Cache-Control": "no-store", ...SECURITY, ...extra },
  });
}

function fail(status: number, message: string, extra: Record<string, string> = {}): Response {
  return json(status, { ok: false, message }, extra);
}

type Entry = string | File;

function str(v: Entry | null): string {
  return typeof v === "string" ? v : "";
}

function asFile(v: Entry | null): File | null {
  return v && typeof v !== "string" ? v : null;
}

// Constant-time compare so the maintainer token can't be probed byte by byte.
function safeEqual(a: string, b: string): boolean {
  const ea = new TextEncoder().encode(a), eb = new TextEncoder().encode(b);
  let diff = ea.length ^ eb.length;
  const n = Math.max(ea.length, eb.length);
  for (let i = 0; i < n; i++) diff |= (ea[i] ?? 0) ^ (eb[i] ?? 0);
  return diff === 0;
}

// ---- POST /v1/report ----------------------------------------------------------------

async function report(request: Request, env: Env, deps: Deps): Promise<Response> {
  if (env.DISABLED === "1") return fail(503, "Bug reports are switched off for the moment.");
  if (!(request.headers.get("content-type") ?? "").toLowerCase().startsWith("multipart/form-data"))
    return fail(415, "Unsupported content type.");

  const len = Number(request.headers.get("content-length") ?? "");
  if (!Number.isFinite(len) || len <= 0) return fail(411, "Length required.");
  if (len > LIMITS.requestBytes) return fail(413, "That report is too large to send.");

  const now = deps.now();
  const ipHash = await hashIp(request.headers.get("CF-Connecting-IP") ?? "unknown", env.IP_SALT || "salt");
  const limiter = deps.limiter ?? limiterClient(env.LIMITER);
  const refuse = (v: Extract<Verdict, { ok: false }>): Response => {
    const wait = String(v.retryAfterSec);
    if (v.scope === "bytes") return fail(503, "Reports are at capacity for today. Please try again tomorrow.", { "Retry-After": wait });
    return fail(429, v.scope === "global"
      ? "Lots of reports are coming in right now. Please try again later."
      : "You've sent several reports recently. Please try again later.", { "Retry-After": wait });
  };
  try {
    // Cheap early refusal, before reading the body. The binding decision is reserve() below.
    const early = await limiter.peek(ipHash, now);
    if (!early.ok) return refuse(early);
  } catch {
    return fail(503, "Bug reports are unavailable right now. Please try again later.");
  }

  let form: FormData;
  try { form = await request.formData(); } catch { return fail(400, "Couldn't read the report."); }

  const kindRaw = str(form.get("kind"));
  if (!isKind(kindRaw)) return fail(400, "Unknown report type.");
  const kind = kindRaw;
  const title = sanitizeLine(str(form.get("title")), LIMITS.titleChars);
  const description = sanitizeText(str(form.get("description")), LIMITS.descriptionChars);
  if (!title && !description.trim()) return fail(400, "Please describe the problem.");
  const envInfo = parseEnv(str(form.get("env")));

  // Attachments: sniff the bytes, never trust the filename or declared type.
  let screenshot: Uint8Array | null = null;
  const sf = asFile(form.get("screenshot"));
  if (sf && sf.size > 0) {
    if (sf.size > LIMITS.screenshotBytes) return fail(413, "The screenshot is too large.");
    const bytes = new Uint8Array(await sf.arrayBuffer());
    const dim = pngSize(bytes);
    if (!isPng(bytes) || !dim || dim.w < 1 || dim.h < 1 || dim.w > LIMITS.maxImageSide || dim.h > LIMITS.maxImageSide)
      return fail(400, "The screenshot isn't a valid PNG image.");
    screenshot = bytes;
  }
  let logText: string | null = null;
  const lf = asFile(form.get("log"));
  if (lf && lf.size > 0) {
    if (lf.size > LIMITS.logBytes) return fail(413, "The log is too large.");
    const text = decodeLog(new Uint8Array(await lf.arrayBuffer()));
    if (text === null) return fail(400, "The log isn't plain text.");
    logText = redactPaths(text);
  }
  let project: Uint8Array | null = null;
  const pf = asFile(form.get("project"));
  if (pf && pf.size > 0) {
    if (pf.size > LIMITS.projectBytes) return fail(413, "The project file is too large.");
    const bytes = new Uint8Array(await pf.arrayBuffer());
    if (!isGzip(bytes)) return fail(400, "That doesn't look like a Materializr project.");
    project = bytes;
  }

  const totalBytes = (screenshot?.length ?? 0) + (logText ? new TextEncoder().encode(logText).length : 0) + (project?.length ?? 0);
  // Count it now, atomically; give it back if the report doesn't become an issue.
  let reserved = false;
  try {
    const r = await limiter.reserve(ipHash, now, totalBytes);
    if (!r.ok) return refuse(r);
    reserved = true;
  } catch {
    return fail(503, "Bug reports are unavailable right now. Please try again later.");
  }

  const id = newId();
  const base = new URL(request.url).origin;
  const stored: Array<() => Promise<void>> = [];
  try {
    if (screenshot) {
      await env.PUBLIC_FILES.put(`f/${id}.png`, screenshot, { httpMetadata: { contentType: "image/png" } });
      stored.push(() => env.PUBLIC_FILES.delete(`f/${id}.png`));
    }
    if (logText !== null) {
      await env.PUBLIC_FILES.put(`f/${id}.log`, logText, { httpMetadata: { contentType: "text/plain; charset=utf-8" } });
      stored.push(() => env.PUBLIC_FILES.delete(`f/${id}.log`));
    }
    if (project) {
      await env.PRIVATE_FILES.put(`p/${id}.mzr`, project, { httpMetadata: { contentType: "application/octet-stream" } });
      stored.push(() => env.PRIVATE_FILES.delete(`p/${id}.mzr`));
    }

    const body = buildIssueBody({
      kind, description, env: envInfo, reportId: id,
      screenshotUrl: screenshot ? `${base}/f/${id}.png` : undefined,
      logUrl: logText !== null ? `${base}/f/${id}.log` : undefined,
      projectId: project ? id : undefined,
      projectBytes: project?.length,
    });
    const issueTitle = title || `${kind} report from the app`;

    let issue: { number: number; html_url: string };
    if (env.DRY_RUN === "1") {
      issue = { number: 0, html_url: `https://github.com/${env.GITHUB_OWNER}/${env.GITHUB_REPO}/issues/0` };
    } else {
      issue = await createIssue(env, deps.fetch, now, { title: issueTitle, body, labels: labelsFor(kind) });
    }

    return json(200, { ok: true, issueUrl: issue.html_url, number: issue.number, id, dryRun: env.DRY_RUN === "1" });
  } catch (e) {
    // Nothing is left behind for a report that never became an issue.
    await Promise.allSettled(stored.map((f) => f()));
    if (reserved) await limiter.release(ipHash, now, totalBytes).catch(() => undefined);
    console.error("report failed:", e instanceof Error ? e.message : String(e));
    return fail(502, "Couldn't reach GitHub just now. Please try again in a few minutes.");
  }
}

// ---- GET ------------------------------------------------------------------------------

async function serveFile(bucket: R2Like, key: string, cache: string, fallbackType: string): Promise<Response> {
  const obj = await bucket.get(key);
  if (!obj || !obj.body) return new Response("Not found", { status: 404, headers: SECURITY });
  return new Response(obj.body, {
    headers: {
      "Content-Type": obj.httpMetadata?.contentType ?? fallbackType,
      "Cache-Control": cache,
      "Content-Disposition": "inline",
      "Content-Security-Policy": "default-src 'none'; sandbox",
      ...SECURITY,
    },
  });
}

export async function handle(request: Request, env: Env, deps: Deps): Promise<Response> {
  const url = new URL(request.url);
  const path = url.pathname;

  if (path === "/v1/report") {
    if (request.method !== "POST") return fail(405, "Method not allowed.", { Allow: "POST" });
    return report(request, env, deps);
  }

  if (request.method === "GET") {
    if (path === "/") return json(200, { ok: true, service: "materializr-report-relay" });

    const pub = /^\/f\/([0-9a-f]{32})\.(png|log)$/.exec(path);
    if (pub) {
      // Edge-cache public files: repeat views (GitHub's image proxy, people
      // re-opening the issue) never touch R2, so reads stay far below the free tier.
      const cache = (globalThis as { caches?: { default?: Cache } }).caches?.default;
      const cacheKey = new Request(url.origin + path);
      const hit = cache ? await cache.match(cacheKey) : undefined;
      if (hit) return hit;
      const res = await serveFile(env.PUBLIC_FILES, `f/${pub[1]}.${pub[2]}`, "public, max-age=31536000, immutable",
        pub[2] === "png" ? "image/png" : "text/plain; charset=utf-8");
      if (cache && res.status === 200) await cache.put(cacheKey, res.clone());
      return res;
    }

    const priv = /^\/p\/([0-9a-f]{32})$/.exec(path);
    if (priv && ID_RE.test(priv[1])) {
      const auth = request.headers.get("Authorization") ?? "";
      const token = auth.startsWith("Bearer ") ? auth.slice(7) : "";
      // 404, not 401: don't confirm that a project id exists to someone without the token.
      if (!env.MAINTAINER_TOKEN || !safeEqual(token, env.MAINTAINER_TOKEN))
        return new Response("Not found", { status: 404, headers: SECURITY });
      return serveFile(env.PRIVATE_FILES, `p/${priv[1]}.mzr`, "private, no-store", "application/octet-stream");
    }
  }
  return new Response("Not found", { status: 404, headers: SECURITY });
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    try {
      return await handle(request, env, { fetch: (u, i) => fetch(u, i), now: () => Date.now() });
    } catch (e) {
      console.error("unhandled:", e instanceof Error ? e.message : String(e));
      return fail(500, "Something went wrong.");
    }
  },
};
