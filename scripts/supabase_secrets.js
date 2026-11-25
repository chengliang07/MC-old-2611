#!/usr/bin/env node
/*
 * Update the oral relay's Bailian realtime model via the Supabase Management API.
 *
 * Why node: this shell's Windows schannel cannot acquire credentials under the
 * sandbox (curl / Invoke-WebRequest fail with SEC_E_NO_CREDENTIALS), while
 * node's bundled OpenSSL works.
 *
 * Token sources, in order: -Token, .gates/supabase.pat, $SUPABASE_ACCESS_TOKEN
 *
 * Usage:
 *   node scripts/supabase_secrets.js --list
 *   node scripts/supabase_secrets.js --model qwen-audio-3.1-realtime-plus
 *   node scripts/supabase_secrets.js --model X --dry-run
 */

const fs = require("fs");
const path = require("path");

const DEFAULT_REF = "oemsbxavqkmtuyasxsba"; // from wss://oemsbxavqkmtuyasxsba.supabase.co

function parseArgs(argv) {
  const out = { list: false, dryRun: false, model: "", ref: DEFAULT_REF, token: "" };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--list") out.list = true;
    else if (a === "--dry-run") out.dryRun = true;
    else if (a === "--model") out.model = argv[++i] || "";
    else if (a === "--ref") out.ref = argv[++i] || DEFAULT_REF;
    else if (a === "--token") out.token = argv[++i] || "";
  }
  return out;
}

function readToken(explicit) {
  if (explicit) return explicit.trim();
  const file = path.join(__dirname, "..", ".gates", "supabase.pat");
  try {
    const raw = fs.readFileSync(file, "utf8").trim();
    if (raw) return raw;
  } catch (_) {}
  return (process.env.SUPABASE_ACCESS_TOKEN || "").trim();
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const token = readToken(args.token);
  if (!token) {
    console.error("no token: put it in .gates/supabase.pat (gitignored) or pass --token");
    process.exit(2);
  }
  const base = `https://api.supabase.com/v1/projects/${args.ref}/secrets`;
  const headers = { Authorization: `Bearer ${token}`, "Content-Type": "application/json" };

  if (args.list) {
    const r = await fetch(base, { headers });
    const body = await r.text();
    if (!r.ok) {
      console.error(`GET failed HTTP ${r.status}: ${body}`);
      process.exit(1);
    }
    const list = JSON.parse(body);
    console.log(`secrets on ${args.ref} (names only):`);
    for (const s of list) console.log("  " + s.name);
    return;
  }

  if (!args.model) {
    console.error("missing --model");
    process.exit(2);
  }
  const payload = [
    { name: "QWEN_REALTIME_MODEL", value: args.model },
    // allow-list gate: without this the relay silently falls back to the old model
    { name: "ORAL_COACH_ALLOWED_MODELS", value: args.model },
  ];
  if (args.dryRun) {
    console.log(`DRY RUN: POST ${base}`);
    console.log(JSON.stringify(payload, null, 2));
    return;
  }
  const r = await fetch(base, { method: "POST", headers, body: JSON.stringify(payload) });
  const body = await r.text();
  if (!r.ok) {
    console.error(`POST failed HTTP ${r.status}: ${body}`);
    process.exit(1);
  }
  console.log("OK: secrets updated; Supabase restarts the function automatically.");
  try {
    const list = JSON.parse(body);
    for (const s of list) if (s.name) console.log("  " + s.name);
  } catch (_) {}
}

main().catch((e) => {
  console.error("error:", e && e.message ? e.message : e);
  process.exit(1);
});
