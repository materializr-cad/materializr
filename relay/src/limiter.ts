// The Durable Object that owns the abuse counters. One global instance: every
// call is processed one at a time, so limits are exact.

import { LimiterCore, type LimiterLike, type StoreLike, type Verdict } from "./ratelimit.ts";

interface DoStorage {
  get(key: string): Promise<unknown>;
  put(key: string, value: string): Promise<void>;
  delete(key: string): Promise<boolean>;
  list(): Promise<Map<string, unknown>>;
}

function adapt(s: DoStorage): StoreLike {
  return {
    async get(k) { return (await s.get(k)) as string | undefined; },
    put: (k, v) => s.put(k, v),
    async delete(k) { await s.delete(k); },
    async keys() { return [...(await s.list()).keys()]; },
  };
}

export class Limiter {
  private core: LimiterCore;
  constructor(state: { storage: DoStorage }) {
    this.core = new LimiterCore(adapt(state.storage));
  }

  async fetch(request: Request): Promise<Response> {
    const { op, ip, now, bytes } = (await request.json()) as { op: string; ip: string; now: number; bytes?: number };
    if (op === "peek") return Response.json(await this.core.peek(ip, now));
    if (op === "reserve") return Response.json(await this.core.reserve(ip, now, bytes ?? 0));
    if (op === "release") { await this.core.release(ip, now, bytes ?? 0); return Response.json({ ok: true }); }
    return new Response("bad op", { status: 400 });
  }
}

// Client used by the Worker: talks to the single global instance.
export function limiterClient(ns: { idFromName(n: string): unknown; get(id: unknown): { fetch(r: Request): Promise<Response> } }): LimiterLike {
  const stub = () => ns.get(ns.idFromName("global"));
  const call = async (body: object): Promise<Response> => {
    const res = await stub().fetch(new Request("https://limiter/op", { method: "POST", body: JSON.stringify(body) }));
    if (!res.ok) throw new Error(`limiter: HTTP ${res.status}`);
    return res;
  };
  return {
    async peek(ip, now) { return (await (await call({ op: "peek", ip, now })).json()) as Verdict; },
    async reserve(ip, now, bytes) { return (await (await call({ op: "reserve", ip, now, bytes })).json()) as Verdict; },
    async release(ip, now, bytes) { await call({ op: "release", ip, now, bytes }); },
  };
}
