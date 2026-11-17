import { test } from "node:test";
import assert from "node:assert/strict";
import { handle } from "../src/index.ts";
import { fakePng, gzipBytes, makeDeps, makeEnv, reportRequest } from "./helpers.ts";
import { RATE } from "../src/ratelimit.ts";

const png = () => new Blob([fakePng(200, 100, 1000) as BlobPart], { type: "image/png" });
const gz = () => new Blob([gzipBytes(500) as BlobPart], { type: "application/octet-stream" });
const logb = (s = "line one\nopened /home/kevin/secret.mzr\n") => new Blob([s], { type: "text/plain" });

test("a full report stores files, files the issue (dry run), and counts it", async () => {
  const deps = makeDeps();
  const env = makeEnv();
  const res = await handle(reportRequest({
    kind: "Bug", title: "It crashed", description: "ping @someone",
    env: JSON.stringify({ version: "1.8.0", os: "Android" }),
    screenshot: png(), log: logb(), project: gz(),
  }), env, deps);
  assert.equal(res.status, 200);
  const j = await res.json() as { ok: boolean; id: string; issueUrl: string; dryRun: boolean };
  assert.equal(j.ok, true);
  assert.equal(j.dryRun, true);
  assert.match(j.id, /^[0-9a-f]{32}$/);
  assert.ok(env.PUBLIC_FILES.m.has(`f/${j.id}.png`));
  assert.ok(env.PUBLIC_FILES.m.has(`f/${j.id}.log`));
  assert.ok(env.PRIVATE_FILES.m.has(`p/${j.id}.mzr`));
  assert.ok(!env.PUBLIC_FILES.m.has(`p/${j.id}.mzr`), "project must never land in the public bucket");
  // the stored log has the account name redacted
  const logStored = new TextDecoder().decode(env.PUBLIC_FILES.m.get(`f/${j.id}.log`)!.data);
  assert.ok(logStored.includes("/home/<user>/secret.mzr") && !logStored.includes("kevin"));
  assert.ok(deps.store.writes >= 4, "an accepted report is counted");
});

test("kind is required and validated", async () => {
  const deps = makeDeps();
  const res = await handle(reportRequest({ kind: "Rant", description: "x" }), makeEnv(), deps);
  assert.equal(res.status, 400);
});

test("an empty report is refused", async () => {
  const deps = makeDeps();
  const res = await handle(reportRequest({ kind: "Bug", title: "", description: "   " }), makeEnv(), deps);
  assert.equal(res.status, 400);
});

test("a non-PNG 'screenshot' is refused and nothing is stored", async () => {
  const deps = makeDeps();
  const env = makeEnv();
  const res = await handle(reportRequest({ kind: "Bug", description: "x", screenshot: new Blob(["<html>evil</html>"], { type: "image/png" }) }), env, deps);
  assert.equal(res.status, 400);
  assert.equal(env.PUBLIC_FILES.m.size, 0);
});

test("a binary 'log' is refused", async () => {
  const deps = makeDeps();
  const res = await handle(reportRequest({ kind: "Bug", description: "x", log: new Blob([new Uint8Array([1, 0, 2])]) }), makeEnv(), deps);
  assert.equal(res.status, 400);
});

test("a non-gzip 'project' is refused", async () => {
  const deps = makeDeps();
  const res = await handle(reportRequest({ kind: "Bug", description: "x", project: new Blob(["MZ\u0090 exe"]) }), makeEnv(), deps);
  assert.equal(res.status, 400);
});

test("oversize body is refused before it is read", async () => {
  const deps = makeDeps();
  const res = await handle(reportRequest({ kind: "Bug", description: "x" }, { "content-length": String(50 * 1024 * 1024) }), makeEnv(), deps);
  assert.equal(res.status, 413);
});

test("oversize attachment is refused", async () => {
  const deps = makeDeps();
  const big = new Blob([fakePng(10, 10, 9 * 1024 * 1024) as BlobPart], { type: "image/png" });
  const res = await handle(reportRequest({ kind: "Bug", description: "x", screenshot: big }, { "content-length": "9500000" }), makeEnv(), deps);
  assert.equal(res.status, 413);
});

test("wrong content type and wrong method", async () => {
  const deps = makeDeps();
  const env = makeEnv();
  const wrongType = new Request("https://relay.test/v1/report", { method: "POST", body: "{}", headers: { "content-type": "application/json", "content-length": "2" } });
  assert.equal((await handle(wrongType, env, deps)).status, 415);
  assert.equal((await handle(new Request("https://relay.test/v1/report"), env, deps)).status, 405);
});

test("rate limit: the 4th report from one address in an hour gets 429 + Retry-After", async () => {
  const deps = makeDeps();
  const env = makeEnv();
  for (let i = 0; i < RATE.perIpPerHour; i++)
    assert.equal((await handle(reportRequest({ kind: "Bug", description: `n${i}` }), env, deps)).status, 200);
  const res = await handle(reportRequest({ kind: "Bug", description: "one too many" }), env, deps);
  assert.equal(res.status, 429);
  assert.ok(Number(res.headers.get("Retry-After")) > 0);
  // a different address is fine
  const other = await handle(reportRequest({ kind: "Bug", description: "hi" }, { "CF-Connecting-IP": "198.51.100.9" }), env, deps);
  assert.equal(other.status, 200);
});

test("kill switch", async () => {
  const deps = makeDeps();
  const res = await handle(reportRequest({ kind: "Bug", description: "x" }), makeEnv({ DISABLED: "1" }), deps);
  assert.equal(res.status, 503);
});

test("a GitHub failure rolls back stored files and does not count against the limit", async () => {
  const deps = makeDeps();
  const env = makeEnv({ DRY_RUN: "0" });
  const res = await handle(reportRequest({ kind: "Bug", description: "x", screenshot: png(), project: gz() }), env, deps);
  assert.equal(res.status, 502);
  assert.equal(env.PUBLIC_FILES.m.size, 0);
  assert.equal(env.PRIVATE_FILES.m.size, 0);
  const day = Math.floor(deps.now() / 86_400_000);
  assert.equal(deps.store.m.get(`g:d:${day}`), "0", "a failed report is not counted against the limit");
  assert.equal(deps.store.m.get(`b:d:${day}`), "0");
});

test("public files are served with safe headers; unknown ids 404", async () => {
  const deps = makeDeps();
  const env = makeEnv();
  const r = await handle(reportRequest({ kind: "Bug", description: "x", screenshot: png() }), env, deps);
  const { id } = await r.json() as { id: string };
  const got = await handle(new Request(`https://relay.test/f/${id}.png`), env, deps);
  assert.equal(got.status, 200);
  assert.equal(got.headers.get("content-type"), "image/png");
  assert.equal(got.headers.get("x-content-type-options"), "nosniff");
  assert.equal((await handle(new Request(`https://relay.test/f/${"0".repeat(32)}.png`), env, deps)).status, 404);
  // only well-formed ids reach the bucket
  assert.equal((await handle(new Request("https://relay.test/f/../etc.png"), env, deps)).status, 404);
});

test("private project: 404 without the maintainer token, served with it", async () => {
  const deps = makeDeps();
  const env = makeEnv();
  const r = await handle(reportRequest({ kind: "Bug", description: "x", project: gz() }), env, deps);
  const { id } = await r.json() as { id: string };
  assert.equal((await handle(new Request(`https://relay.test/p/${id}`), env, deps)).status, 404);
  assert.equal((await handle(new Request(`https://relay.test/p/${id}`, { headers: { Authorization: "Bearer wrong" } }), env, deps)).status, 404);
  const ok = await handle(new Request(`https://relay.test/p/${id}`, { headers: { Authorization: "Bearer maint-secret" } }), env, deps);
  assert.equal(ok.status, 200);
  assert.equal((await ok.arrayBuffer()).byteLength, 500);
  // no token configured at all => never served
  const noTok = makeEnv({ MAINTAINER_TOKEN: "" });
  noTok.PRIVATE_FILES.m.set(`p/${id}.mzr`, { data: new Uint8Array(3) });
  assert.equal((await handle(new Request(`https://relay.test/p/${id}`, { headers: { Authorization: "Bearer " } }), noTok, deps)).status, 404);
});

test("health endpoint", async () => {
  const deps = makeDeps();
  const res = await handle(new Request("https://relay.test/"), makeEnv(), deps);
  assert.equal(res.status, 200);
});

test("the daily upload budget stops further uploads and says when to retry", async () => {
  const deps = makeDeps();
  const env = makeEnv();
  await deps.limiter.reserve("someone-else", deps.now(), RATE.dailyBytes - 100);
  const res = await handle(reportRequest({ kind: "Bug", description: "x", screenshot: png() }), env, deps);
  assert.equal(res.status, 503);
  assert.ok(Number(res.headers.get("Retry-After")) > 0);
  assert.equal(env.PUBLIC_FILES.m.size, 0);
  // a text-only report (no attachments) still fits in what is left
  assert.equal((await handle(reportRequest({ kind: "Bug", description: "text only" }), env, deps)).status, 200);
});

test("accepted attachment bytes are added to the daily budget", async () => {
  const deps = makeDeps();
  const env = makeEnv();
  await handle(reportRequest({ kind: "Bug", description: "x", project: gz() }), env, deps);
  const day = Math.floor(deps.now() / 86_400_000);
  assert.equal(deps.store.m.get(`b:d:${day}`), "500");
});

test("worst case stays inside the free R2 tier", () => {
  // 10 GB free. Everything expires after 14 days, so the daily byte budget
  // bounds the steady-state worst case.
  const GB = 1024 ** 3;
  assert.ok(RATE.dailyBytes * 14 < 10 * GB, "daily budget x 14 days must stay under 10 GB");
  // Class A (writes): at most 3 per report, capped reports per day, vs 1M a month.
  assert.ok(RATE.globalPerDay * 3 * 31 < 1_000_000);
});

test("the limiter being unavailable fails closed with a 503", async () => {
  const deps = makeDeps();
  const env = makeEnv();
  deps.limiter.peek = async () => { throw new Error("limiter unavailable"); };
  const res = await handle(reportRequest({ kind: "Bug", description: "x" }), env, deps);
  assert.equal(res.status, 503);
});
