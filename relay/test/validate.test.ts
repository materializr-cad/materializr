import { test } from "node:test";
import assert from "node:assert/strict";
import {
  LIMITS, buildIssueBody, decodeLog, isGzip, isPng, labelsFor, neutralizeMarkdown, newId,
  parseEnv, pngSize, redactPaths, sanitizeLine, sanitizeText,
} from "../src/validate.ts";
import { fakePng, gzipBytes } from "./helpers.ts";

test("sanitizeText strips control chars, normalises newlines, caps length", () => {
  assert.equal(sanitizeText("a\u0000b\u0007c\r\nd", 100), "abc\nd");
  assert.equal(sanitizeText("x".repeat(50), 10).length, 10);
});

test("sanitizeLine collapses whitespace and caps", () => {
  assert.equal(sanitizeLine("  a \n\t b  ", 50), "a b");
  assert.equal(sanitizeLine("y".repeat(500), LIMITS.titleChars).length, LIMITS.titleChars);
});

test("neutralizeMarkdown defuses mentions, issue refs, images, links and HTML", () => {
  const out = neutralizeMarkdown("hi @octocat see #123 ![x](http://evil) [a](http://b) <img src=x>");
  assert.ok(!/@octocat/.test(out), "mention must not survive intact");
  assert.ok(!/#123/.test(out), "issue reference must not survive intact");
  assert.ok(!/!\[x\]\(/.test(out), "image syntax must not survive intact");
  assert.ok(!/\[a\]\(/.test(out), "link syntax must not survive intact");
  assert.ok(!out.includes("<img"), "html must be escaped");
  // ...but it still reads the same once the zero-width spaces are ignored.
  assert.equal(out.replace(/\u200b/g, "").replace(/&lt;/g, "<"), "hi @octocat see #123 ![x](http://evil) [a](http://b) <img src=x>");
});

test("redactPaths hides account names in home paths", () => {
  assert.equal(redactPaths("open /home/kevin/Documents/a.mzr"), "open /home/<user>/Documents/a.mzr");
  assert.equal(redactPaths("C:\\Users\\Steve\\x.mzr"), "C:\\Users\\<user>\\x.mzr");
  assert.equal(redactPaths("/Users/sam/Library"), "/Users/<user>/Library");
  assert.equal(redactPaths("/data/user/10/com.x/files"), "/data/user/0/com.x/files");
});

test("PNG sniffing accepts a real header and rejects other bytes", () => {
  assert.equal(isPng(fakePng()), true);
  assert.deepEqual(pngSize(fakePng(640, 480)), { w: 640, h: 480 });
  assert.equal(isPng(new TextEncoder().encode("GIF89a........................")), false);
  assert.equal(isPng(new Uint8Array(10)), false);
});

test("gzip sniffing", () => {
  assert.equal(isGzip(gzipBytes()), true);
  assert.equal(isGzip(new TextEncoder().encode("not gzip at all")), false);
});

test("decodeLog accepts utf-8 text and refuses NUL bytes / invalid utf-8", () => {
  assert.equal(decodeLog(new TextEncoder().encode("hello\nworld \u00e9")), "hello\nworld \u00e9");
  assert.equal(decodeLog(new Uint8Array([104, 0, 105])), null);
  assert.equal(decodeLog(new Uint8Array([0xff, 0xfe, 0xfd])), null);
});

test("parseEnv keeps only known string keys, from JSON or an object", () => {
  const e = parseEnv(JSON.stringify({ version: "1.8.0", os: "Android", evil: "x", arch: 5, graphics: "  Adreno\n650 " }));
  assert.deepEqual(e, { version: "1.8.0", os: "Android", graphics: "Adreno 650" });
  assert.deepEqual(parseEnv("not json"), {});
  assert.deepEqual(parseEnv([1, 2]), {});
});

test("labels follow the report kind", () => {
  assert.deepEqual(labelsFor("Bug"), ["bug", "from-app"]);
  assert.deepEqual(labelsFor("Feature"), ["enhancement", "from-app"]);
  assert.deepEqual(labelsFor("Question"), ["question", "from-app"]);
});

test("issue body: attachments, environment, neutralised description", () => {
  const body = buildIssueBody({
    kind: "Bug", description: "ping @someone", env: { version: "1.8.0", os: "Linux" },
    reportId: "abc", screenshotUrl: "https://r/f/abc.png", logUrl: "https://r/f/abc.log",
    projectId: "abc", projectBytes: 2048,
  });
  assert.match(body, /^\*\*Type:\*\* Bug/);
  assert.match(body, /!\[screenshot\]\(https:\/\/r\/f\/abc\.png\)/);
  assert.match(body, /\[Log\]\(https:\/\/r\/f\/abc\.log\)/);
  assert.match(body, /stored privately, 2\.0 KB/);
  assert.match(body, /- Materializr: 1\.8\.0/);
  assert.ok(!/@someone/.test(body));
  assert.ok(!body.includes("https://r/p/"), "the private project must never be linked");
});

test("issue body without attachments omits the section", () => {
  const body = buildIssueBody({ kind: "Question", description: "", env: {}, reportId: "z" });
  assert.ok(!body.includes("### Attachments"));
  assert.match(body, /no description given/);
});

test("ids are 32 hex chars and differ", () => {
  const a = newId(), b = newId();
  assert.match(a, /^[0-9a-f]{32}$/);
  assert.notEqual(a, b);
});
