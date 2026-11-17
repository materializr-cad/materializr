// Pure helpers: everything the relay trusts or emits is decided here, so it can
// be tested without Cloudflare. Nothing in this file touches the network.

export const LIMITS = {
  // Sized against Cloudflare's FREE R2 tier (10 GB stored, 1M writes, 10M reads
  // a month; overage is billed). See ratelimit.ts for the daily byte budget.
  requestBytes: 12 * 1024 * 1024,   // whole multipart body
  screenshotBytes: 3 * 1024 * 1024,
  logBytes: 600 * 1024,
  projectBytes: 8 * 1024 * 1024,
  titleChars: 120,
  descriptionChars: 6000,
  envValueChars: 120,
  maxImageSide: 16384,
} as const;

export const KINDS = ["Bug", "Feature", "Question"] as const;
export type Kind = (typeof KINDS)[number];

const LABELS: Record<Kind, string> = { Bug: "bug", Feature: "enhancement", Question: "question" };
export function labelsFor(kind: Kind): string[] {
  return [LABELS[kind], "from-app"];
}

export function isKind(v: unknown): v is Kind {
  return typeof v === "string" && (KINDS as readonly string[]).includes(v);
}

// ---- text -------------------------------------------------------------------

// Strip control characters (keep \n and \t in free text), cap the length.
export function sanitizeText(s: string, max: number): string {
  // eslint-disable-next-line no-control-regex
  const cleaned = s.replace(/[\u0000-\u0008\u000B\u000C\u000E-\u001F\u007F]/g, "").replace(/\r\n?/g, "\n");
  return cleaned.length > max ? cleaned.slice(0, max) : cleaned;
}

export function sanitizeLine(s: string, max: number): string {
  return sanitizeText(s, max * 2).replace(/\s+/g, " ").trim().slice(0, max);
}

// The report is anonymous and public, so user text must not be able to ping
// people, cross-link other issues, smuggle HTML, or plant clickable links/images
// dressed up as part of OUR template. A zero-width space defuses each trigger
// without changing how the text reads.
export function neutralizeMarkdown(s: string): string {
  const zw = "​";
  return s
    .replace(/</g, "&lt;")
    .replace(/@(?=[A-Za-z0-9_-])/g, "@" + zw)
    .replace(/#(?=\d)/g, "#" + zw)
    .replace(/!\[/g, "!" + zw + "[")
    .replace(/\]\(/g, "]" + zw + "(");
}

// Logs carry the user's folder names. Replace the account segment of home paths.
export function redactPaths(s: string): string {
  return s
    .replace(/\/home\/[^/\s"'`:]+/g, "/home/<user>")
    .replace(/\/Users\/[^/\s"'`:]+/g, "/Users/<user>")
    .replace(/([A-Za-z]:\\Users\\)[^\\\s"'`:]+/gi, "$1<user>")
    .replace(/\/data\/user\/\d+\//g, "/data/user/0/");
}

// ---- binary sniffing ----------------------------------------------------------

const PNG_SIG = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];

export function isPng(b: Uint8Array): boolean {
  return b.length >= 24 && PNG_SIG.every((v, i) => b[i] === v);
}

export function pngSize(b: Uint8Array): { w: number; h: number } | null {
  if (!isPng(b)) return null;
  const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
  return { w: dv.getUint32(16), h: dv.getUint32(20) };
}

export function isGzip(b: Uint8Array): boolean {
  return b.length >= 10 && b[0] === 0x1f && b[1] === 0x8b;
}

// Valid UTF-8, no NUL bytes: a log, not a disguised binary.
export function decodeLog(b: Uint8Array): string | null {
  if (b.includes(0)) return null;
  try {
    return new TextDecoder("utf-8", { fatal: true, ignoreBOM: false }).decode(b);
  } catch {
    return null;
  }
}

// ---- environment block --------------------------------------------------------

export const ENV_KEYS = [
  "version", "os", "arch", "occt", "graphics", "layout", "touchMode", "units", "language",
] as const;
export type EnvKey = (typeof ENV_KEYS)[number];
export type Env = Partial<Record<EnvKey, string>>;

export function parseEnv(raw: unknown): Env {
  let obj: unknown = raw;
  if (typeof raw === "string") {
    try { obj = JSON.parse(raw); } catch { return {}; }
  }
  const out: Env = {};
  if (!obj || typeof obj !== "object" || Array.isArray(obj)) return out;
  for (const k of ENV_KEYS) {
    const v = (obj as Record<string, unknown>)[k];
    if (typeof v === "string" && v.trim()) out[k] = sanitizeLine(v, LIMITS.envValueChars);
  }
  return out;
}

const ENV_LABELS: Record<EnvKey, string> = {
  version: "Materializr", os: "OS", arch: "Architecture", occt: "OpenCASCADE",
  graphics: "Graphics", layout: "Layout", touchMode: "Touch mode", units: "Units", language: "Language",
};

export function envBlock(env: Env): string {
  let out = "";
  for (const k of ENV_KEYS) {
    const v = env[k];
    if (v) out += `- ${ENV_LABELS[k]}: ${neutralizeMarkdown(v)}\n`;
  }
  return out;
}

// ---- issue ----------------------------------------------------------------------

export interface IssueInput {
  kind: Kind;
  description: string;      // already sanitised
  env: Env;
  reportId: string;
  screenshotUrl?: string;
  logUrl?: string;
  projectId?: string;
  projectBytes?: number;
}

export function humanSize(n: number): string {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  return `${(n / (1024 * 1024)).toFixed(1)} MB`;
}

export function buildIssueBody(i: IssueInput): string {
  let b = `**Type:** ${i.kind}\n\n### What happened\n\n`;
  b += i.description.trim() ? neutralizeMarkdown(i.description.trim()) : "_(no description given)_";
  b += "\n\n";
  const hasAttach = i.screenshotUrl || i.logUrl || i.projectId;
  if (hasAttach) {
    b += "### Attachments\n\n";
    if (i.screenshotUrl) b += `![screenshot](${i.screenshotUrl})\n\n`;
    if (i.logUrl) b += `- [Log](${i.logUrl})\n`;
    if (i.projectId)
      b += `- Project file: stored privately, ${humanSize(i.projectBytes ?? 0)} (id \`${i.projectId}\`; maintainers only)\n`;
    b += "\n";
  }
  const env = envBlock(i.env);
  if (env) b += `### Environment\n\n${env}\n`;
  b += `_Sent from inside Materializr. Report \`${i.reportId}\`._\n`;
  return b;
}

// ---- ids ------------------------------------------------------------------------

export function newId(rand: (n: number) => Uint8Array = (n) => crypto.getRandomValues(new Uint8Array(n))): string {
  return Array.from(rand(16), (x) => x.toString(16).padStart(2, "0")).join("");
}

export const ID_RE = /^[0-9a-f]{32}$/;
