// Abuse limits, enforced by a single Durable Object so that every decision is
// strictly serialised: a burst of simultaneous requests cannot slip past a
// counter (an eventually-consistent store like KV let a 4th report through a
// limit of 3). The logic here is storage-agnostic and tested in plain node.

export interface StoreLike {
  get(key: string): Promise<string | null | undefined>;
  put(key: string, value: string): Promise<void>;
  delete(key: string): Promise<void>;
  keys(): Promise<string[]>;
}

export const RATE = {
  // Generous enough for a tester working through a list of ten things. The byte
  // budget below, not these counts, is what protects the R2 free tier.
  perIpPerHour: 10,
  perIpPerDay: 25,
  globalPerDay: 100,
  // Bytes accepted per day, all reports together. Every stored object expires
  // after 14 days (R2 lifecycle rule on all four buckets), so even if someone
  // saturates this budget every single day, R2 holds at most 80 MB x 14 d =
  // 1.1 GB, far under the 10 GB free tier.
  dailyBytes: 80 * 1024 * 1024,
} as const;

const HOUR = 3600_000;
const DAY = 86_400_000;

export type Scope = "ip-hour" | "ip-day" | "global" | "bytes";
export type Verdict = { ok: true } | { ok: false; scope: Scope; retryAfterSec: number };

export interface LimiterLike {
  // Read-only: would a report from this address be accepted right now?
  peek(ipHash: string, nowMs: number): Promise<Verdict>;
  // Atomically check AND count a report of `bytes` attachment bytes.
  reserve(ipHash: string, nowMs: number, bytes: number): Promise<Verdict>;
  // Undo a reserve() (the report failed after being counted).
  release(ipHash: string, nowMs: number, bytes: number): Promise<void>;
}

export function keysFor(ipHash: string, nowMs: number) {
  const h = Math.floor(nowMs / HOUR);
  const d = Math.floor(nowMs / DAY);
  return {
    ipHour: `ip:${ipHash}:h:${h}`,
    ipDay: `ip:${ipHash}:d:${d}`,
    global: `g:d:${d}`,
    bytes: `b:d:${d}`,
    hourEnds: (h + 1) * HOUR,
    dayEnds: (d + 1) * DAY,
    h, d,
  };
}

export class LimiterCore implements LimiterLike {
  private chain: Promise<unknown> = Promise.resolve();
  private store: StoreLike;
  constructor(store: StoreLike) { this.store = store; }

  // One operation at a time, in arrival order.
  private serial<T>(fn: () => Promise<T>): Promise<T> {
    const run = this.chain.then(fn, fn);
    this.chain = run.catch(() => undefined);
    return run;
  }

  private async n(key: string): Promise<number> {
    const v = await this.store.get(key);
    const x = v ? parseInt(v, 10) : 0;
    return Number.isFinite(x) ? x : 0;
  }

  private async verdict(ipHash: string, nowMs: number, bytes: number): Promise<Verdict> {
    const k = keysFor(ipHash, nowMs);
    const [ih, id, g, b] = await Promise.all([this.n(k.ipHour), this.n(k.ipDay), this.n(k.global), this.n(k.bytes)]);
    const secs = (end: number) => Math.max(1, Math.ceil((end - nowMs) / 1000));
    if (ih >= RATE.perIpPerHour) return { ok: false, scope: "ip-hour", retryAfterSec: secs(k.hourEnds) };
    if (id >= RATE.perIpPerDay) return { ok: false, scope: "ip-day", retryAfterSec: secs(k.dayEnds) };
    if (g >= RATE.globalPerDay) return { ok: false, scope: "global", retryAfterSec: secs(k.dayEnds) };
    if (b + bytes > RATE.dailyBytes) return { ok: false, scope: "bytes", retryAfterSec: secs(k.dayEnds) };
    return { ok: true };
  }

  peek(ipHash: string, nowMs: number): Promise<Verdict> {
    return this.serial(() => this.verdict(ipHash, nowMs, 0));
  }

  reserve(ipHash: string, nowMs: number, bytes: number): Promise<Verdict> {
    return this.serial(async () => {
      const v = await this.verdict(ipHash, nowMs, bytes);
      if (!v.ok) return v;
      const k = keysFor(ipHash, nowMs);
      const [ih, id, g, b] = await Promise.all([this.n(k.ipHour), this.n(k.ipDay), this.n(k.global), this.n(k.bytes)]);
      await Promise.all([
        this.store.put(k.ipHour, String(ih + 1)),
        this.store.put(k.ipDay, String(id + 1)),
        this.store.put(k.global, String(g + 1)),
        this.store.put(k.bytes, String(b + bytes)),
      ]);
      await this.gc(nowMs);
      return v;
    });
  }

  release(ipHash: string, nowMs: number, bytes: number): Promise<void> {
    return this.serial(async () => {
      const k = keysFor(ipHash, nowMs);
      const [ih, id, g, b] = await Promise.all([this.n(k.ipHour), this.n(k.ipDay), this.n(k.global), this.n(k.bytes)]);
      await Promise.all([
        this.store.put(k.ipHour, String(Math.max(0, ih - 1))),
        this.store.put(k.ipDay, String(Math.max(0, id - 1))),
        this.store.put(k.global, String(Math.max(0, g - 1))),
        this.store.put(k.bytes, String(Math.max(0, b - bytes))),
      ]);
    });
  }

  // Drop counters from past hours/days, at most once an hour, so storage stays tiny.
  private async gc(nowMs: number): Promise<void> {
    const k = keysFor("-", nowMs);
    if ((await this.n("meta:gc")) === k.h) return;
    await this.store.put("meta:gc", String(k.h));
    for (const key of await this.store.keys()) {
      const m = /:([hd]):(\d+)$/.exec(key) ?? /^[gb]:(d):(\d+)$/.exec(key);
      if (!m) continue;
      const idx = parseInt(m[2], 10);
      if ((m[1] === "h" && idx < k.h) || (m[1] === "d" && idx < k.d)) await this.store.delete(key);
    }
  }
}

// Never store a raw address: a salted, truncated hash is enough to count by.
export async function hashIp(ip: string, salt: string): Promise<string> {
  const data = new TextEncoder().encode(`${salt}|${ip}`);
  const dig = new Uint8Array(await crypto.subtle.digest("SHA-256", data));
  return Array.from(dig.slice(0, 12), (x) => x.toString(16).padStart(2, "0")).join("");
}
