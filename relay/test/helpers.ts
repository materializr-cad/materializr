// In-memory fakes for KV and R2, so the whole handler runs under node:test.
import type { Env, R2Like } from "../src/index.ts";
import { LimiterCore, type StoreLike } from "../src/ratelimit.ts";

export class MemStore implements StoreLike {
  m = new Map<string, string>();
  writes = 0;
  async get(k: string) { return this.m.get(k); }
  async put(k: string, v: string) { this.writes++; this.m.set(k, v); }
  async delete(k: string) { this.m.delete(k); }
  async keys() { return [...this.m.keys()]; }
}

export class FakeR2 implements R2Like {
  m = new Map<string, { data: Uint8Array; type?: string }>();
  async put(k: string, v: ArrayBuffer | Uint8Array | string, o?: { httpMetadata?: { contentType?: string } }) {
    const data = typeof v === "string" ? new TextEncoder().encode(v) : new Uint8Array(v as ArrayBuffer);
    this.m.set(k, { data, type: o?.httpMetadata?.contentType });
  }
  async get(k: string) {
    const e = this.m.get(k);
    if (!e) return null;
    return { body: new Blob([e.data as BlobPart]).stream(), httpMetadata: { contentType: e.type } };
  }
  async delete(k: string) { this.m.delete(k); }
}

export function makeEnv(over: Partial<Env> = {}): Env & { PUBLIC_FILES: FakeR2; PRIVATE_FILES: FakeR2 } {
  return {
    GITHUB_APP_ID: "1", GITHUB_INSTALLATION_ID: "2", GITHUB_APP_PRIVATE_KEY: "unused-in-dry-run",
    GITHUB_OWNER: "o", GITHUB_REPO: "r",
    MAINTAINER_TOKEN: "maint-secret", IP_SALT: "salt", DRY_RUN: "1",
    LIMITER: undefined, PUBLIC_FILES: new FakeR2(), PRIVATE_FILES: new FakeR2(),
    ...over,
  } as unknown as Env & { PUBLIC_FILES: FakeR2; PRIVATE_FILES: FakeR2 };
}

// A tiny but structurally valid PNG header (signature + IHDR) of the given size.
export function fakePng(w = 100, h = 50, extra = 0): Uint8Array {
  const b = new Uint8Array(33 + extra);
  b.set([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13, 0x49, 0x48, 0x44, 0x52]);
  const dv = new DataView(b.buffer);
  dv.setUint32(16, w);
  dv.setUint32(20, h);
  return b;
}

export const gzipBytes = (n = 64) => { const b = new Uint8Array(n); b[0] = 0x1f; b[1] = 0x8b; return b; };

export function reportRequest(fields: Record<string, string | Blob | undefined>, headers: Record<string, string> = {}): Request {
  const fd = new FormData();
  for (const [k, v] of Object.entries(fields)) {
    if (v === undefined) continue;
    if (typeof v === "string") fd.append(k, v);
    else fd.append(k, v, k);
  }
  // Request computes Content-Length for a FormData body only at send time; set
  // a realistic header so the size gate has something to read.
  return new Request("https://relay.test/v1/report", { method: "POST", body: fd, headers: { "content-length": "5000", "CF-Connecting-IP": "203.0.113.7", ...headers } });
}

// Per-test dependencies: its own clock and its own limiter state.
export function makeDeps(now = Date.UTC(2026, 10, 17, 12)) {
  const store = new MemStore();
  return {
    fetch: async () => new Response("unused", { status: 500 }),
    now: () => now,
    limiter: new LimiterCore(store),
    store,
  };
}
