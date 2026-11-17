import { test } from "node:test";
import assert from "node:assert/strict";
import { LimiterCore, RATE, hashIp, keysFor } from "../src/ratelimit.ts";
import { MemStore } from "./helpers.ts";

const T0 = Date.UTC(2026, 10, 17, 12, 0, 0);
const mk = () => { const store = new MemStore(); return { store, core: new LimiterCore(store) }; };

test("a fresh address is allowed", async () => {
  assert.deepEqual(await mk().core.peek("ip1", T0), { ok: true });
});

test("per-IP hourly limit trips, with a sensible Retry-After", async () => {
  const { core } = mk();
  for (let i = 0; i < RATE.perIpPerHour; i++) assert.equal((await core.reserve("ip1", T0, 0)).ok, true);
  const v = await core.reserve("ip1", T0 + 60_000, 0);
  assert.equal(v.ok, false);
  if (!v.ok) {
    assert.equal(v.scope, "ip-hour");
    assert.ok(v.retryAfterSec > 0 && v.retryAfterSec <= 3600);
  }
  assert.deepEqual(await core.peek("ip2", T0), { ok: true });
});

test("a burst of simultaneous requests cannot slip past the limit", async () => {
  // The bug that moved this out of KV: concurrent requests all saw the old count.
  const { core } = mk();
  const results = await Promise.all(Array.from({ length: 25 }, () => core.reserve("ip1", T0, 0)));
  assert.equal(results.filter((r) => r.ok).length, RATE.perIpPerHour);
});

test("the hourly window resets in the next hour", async () => {
  const { core } = mk();
  for (let i = 0; i < RATE.perIpPerHour; i++) await core.reserve("ip1", T0, 0);
  assert.deepEqual(await core.reserve("ip1", T0 + 3600_000, 0), { ok: true });
});

test("per-IP daily limit trips even when the hours differ", async () => {
  const { core } = mk();
  // 25-minute spacing keeps every hour under its own limit while the day fills up.
  for (let i = 0; i < RATE.perIpPerDay; i++) assert.equal((await core.reserve("ip1", T0 + i * 1_500_000, 0)).ok, true);
  const v = await core.reserve("ip1", T0 + 11 * 3600_000, 0);
  assert.equal(v.ok, false);
  if (!v.ok) assert.equal(v.scope, "ip-day");
});

test("the global daily cap trips for everyone", async () => {
  const { core } = mk();
  for (let i = 0; i < RATE.globalPerDay; i++) assert.equal((await core.reserve(`ip${i}`, T0, 0)).ok, true);
  const v = await core.reserve("brand-new", T0 + 1000, 0);
  assert.equal(v.ok, false);
  if (!v.ok) assert.equal(v.scope, "global");
});

test("the daily byte budget refuses what would overflow it, and counts what fits", async () => {
  const { core } = mk();
  assert.equal((await core.reserve("a", T0, RATE.dailyBytes - 1000)).ok, true);
  const over = await core.reserve("b", T0, 2000);
  assert.equal(over.ok, false);
  if (!over.ok) assert.equal(over.scope, "bytes");
  assert.equal((await core.reserve("c", T0, 1000)).ok, true);   // exactly fits
  assert.equal((await core.reserve("d", T0, 1)).ok, false);
});

test("release gives back a reservation (failed reports don't count)", async () => {
  const { core } = mk();
  for (let i = 0; i < RATE.perIpPerHour; i++) await core.reserve("ip1", T0, 500);
  assert.equal((await core.peek("ip1", T0)).ok, false);
  await core.release("ip1", T0, 500);
  assert.equal((await core.peek("ip1", T0)).ok, true);
  // never goes negative
  for (let i = 0; i < 10; i++) await core.release("ghost", T0, 999);
  assert.equal((await core.reserve("ghost", T0, 0)).ok, true);
});

test("peek is read-only", async () => {
  const { core, store } = mk();
  for (let i = 0; i < 50; i++) await core.peek("ip1", T0);
  assert.equal(store.writes, 0);
});

test("old counters are cleaned up so storage stays tiny", async () => {
  const { core, store } = mk();
  await core.reserve("ip1", T0, 10);
  const before = keysFor("ip1", T0);
  assert.ok(store.m.has(before.ipDay));
  // two days later: next reserve's gc drops yesterday's keys
  await core.reserve("ip1", T0 + 2 * 86_400_000, 10);
  assert.ok(!store.m.has(before.ipDay), "stale day counter removed");
  assert.ok(!store.m.has(before.ipHour), "stale hour counter removed");
  assert.ok(!store.m.has(before.global));
  assert.ok(!store.m.has(before.bytes));
});

test("IP hashes are stable, salted and not the address", async () => {
  const a = await hashIp("203.0.113.7", "s1");
  assert.equal(a, await hashIp("203.0.113.7", "s1"));
  assert.notEqual(a, await hashIp("203.0.113.7", "s2"));
  assert.ok(!a.includes("203"));
});
