import { test } from "node:test";
import assert from "node:assert/strict";
import { generateKeyPairSync, createPublicKey, verify, createVerify } from "node:crypto";
import { _resetTokenCache, createIssue, pemToPkcs8, pkcs1ToPkcs8, signAppJwt } from "../src/github.ts";

function b64urlToBuf(s: string): Buffer { return Buffer.from(s.replace(/-/g, "+").replace(/_/g, "/"), "base64"); }

for (const [name, fmt] of [["PKCS#1 (what GitHub issues)", "pkcs1"], ["PKCS#8", "pkcs8"]] as const) {
  test(`JWT signed from a ${name} key verifies against the public key`, async () => {
    const { privateKey, publicKey } = generateKeyPairSync("rsa", { modulusLength: 2048 });
    const pem = privateKey.export({ type: fmt, format: "pem" }).toString();
    const jwt = await signAppJwt("12345", pem, 1_700_000_000);
    const [h, p, s] = jwt.split(".");
    assert.deepEqual(JSON.parse(b64urlToBuf(h).toString()), { alg: "RS256", typ: "JWT" });
    const claims = JSON.parse(b64urlToBuf(p).toString());
    assert.equal(claims.iss, "12345");
    assert.equal(claims.iat, 1_700_000_000 - 60);
    assert.equal(claims.exp, 1_700_000_000 + 540);
    const ok = verify("RSA-SHA256", Buffer.from(`${h}.${p}`), publicKey, b64urlToBuf(s));
    assert.equal(ok, true);
  });
}

test("pkcs1ToPkcs8 yields exactly what node's own PKCS#8 export gives", () => {
  const { privateKey } = generateKeyPairSync("rsa", { modulusLength: 2048 });
  const p1 = privateKey.export({ type: "pkcs1", format: "der" });
  const p8 = privateKey.export({ type: "pkcs8", format: "der" });
  assert.deepEqual(Buffer.from(pkcs1ToPkcs8(new Uint8Array(p1))), p8);
});

test("pemToPkcs8 handles both header styles and stray whitespace", () => {
  const { privateKey } = generateKeyPairSync("rsa", { modulusLength: 2048 });
  const p8 = privateKey.export({ type: "pkcs8", format: "der" });
  for (const fmt of ["pkcs1", "pkcs8"] as const) {
    const pem = privateKey.export({ type: fmt, format: "pem" }).toString().replace(/\n/g, "\r\n  ");
    assert.deepEqual(Buffer.from(pemToPkcs8(pem)), p8);
  }
});

test("createIssue mints a narrowly-scoped token, caches it, and posts the issue", async () => {
  _resetTokenCache();
  const { privateKey } = generateKeyPairSync("rsa", { modulusLength: 2048 });
  const env = {
    GITHUB_APP_ID: "9", GITHUB_INSTALLATION_ID: "77", GITHUB_OWNER: "org", GITHUB_REPO: "repo",
    GITHUB_APP_PRIVATE_KEY: privateKey.export({ type: "pkcs1", format: "pem" }).toString(),
  };
  const calls: Array<{ url: string; init: RequestInit }> = [];
  const fakeFetch = async (url: string, init?: RequestInit) => {
    calls.push({ url, init: init! });
    if (url.endsWith("/access_tokens"))
      return new Response(JSON.stringify({ token: "ghs_tok", expires_at: new Date(Date.now() + 3600_000).toISOString() }), { status: 201 });
    return new Response(JSON.stringify({ number: 42, html_url: "https://github.com/org/repo/issues/42" }), { status: 201 });
  };
  const now = Date.now();
  const a = await createIssue(env, fakeFetch, now, { title: "T", body: "B", labels: ["bug"] });
  assert.equal(a.number, 42);
  const tokenCall = calls[0];
  assert.equal(tokenCall.url, "https://api.github.com/app/installations/77/access_tokens");
  assert.match(String((tokenCall.init.headers as Record<string, string>).Authorization), /^Bearer ey/);
  assert.deepEqual(JSON.parse(tokenCall.init.body as string), { repositories: ["repo"], permissions: { issues: "write" } });
  const issueCall = calls[1];
  assert.equal(issueCall.url, "https://api.github.com/repos/org/repo/issues");
  assert.equal((issueCall.init.headers as Record<string, string>).Authorization, "Bearer ghs_tok");
  assert.deepEqual(JSON.parse(issueCall.init.body as string), { title: "T", body: "B", labels: ["bug"] });
  // second issue reuses the cached installation token
  await createIssue(env, fakeFetch, now + 1000, { title: "T2", body: "B", labels: [] });
  assert.equal(calls.filter((c) => c.url.endsWith("/access_tokens")).length, 1);
});

test("a GitHub error surfaces as a thrown error", async () => {
  _resetTokenCache();
  const { privateKey } = generateKeyPairSync("rsa", { modulusLength: 2048 });
  const env = {
    GITHUB_APP_ID: "9", GITHUB_INSTALLATION_ID: "77", GITHUB_OWNER: "o", GITHUB_REPO: "r",
    GITHUB_APP_PRIVATE_KEY: privateKey.export({ type: "pkcs8", format: "pem" }).toString(),
  };
  await assert.rejects(createIssue(env, async () => new Response("no", { status: 403 }), Date.now(), { title: "t", body: "b", labels: [] }), /HTTP 403/);
});
