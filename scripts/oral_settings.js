#!/usr/bin/env node
/*
 * Read / patch the oral_coach_settings row (id=default) used by the relay.
 * Prints only the fields we touch, never credentials.
 *
 * Usage:
 *   node scripts/oral_settings.js --show
 *   node scripts/oral_settings.js --voice Cherry
 */
const fs = require("fs");
const path = require("path");

const gate = path.join(__dirname, "..", ".gates");
const file = fs
  .readdirSync(gate)
  .filter((f) => f.startsWith("secrets_backup_") && f.endsWith(".json"))
  .sort()
  .pop();
const list = JSON.parse(fs.readFileSync(path.join(gate, file), "utf8"));
const get = (n) => ((list.find((s) => s.name === n) || {}).value || "");
const url = get("SUPABASE_URL").replace(/\/$/, "");
const key = get("SUPABASE_SERVICE_ROLE_KEY");
if (!url || !key) {
  console.error("missing SUPABASE_URL / SUPABASE_SERVICE_ROLE_KEY in " + file);
  process.exit(2);
}

const headers = {
  apikey: key,
  Authorization: `Bearer ${key}`,
  "Content-Type": "application/json",
};
const endpoint = `${url}/rest/v1/oral_coach_settings?id=eq.default`;

(async () => {
  const show = process.argv.includes("--show");
  if (show) {
    const r = await fetch(`${endpoint}&select=*`, { headers });
    const rows = await r.json();
    const row = rows[0] || {};
    console.log(JSON.stringify({
      voice: row.voice,
      model: row.model,
      enabled: row.enabled,
      vad_threshold: row.vad_threshold,
      vad_silence_ms: row.vad_silence_ms,
    }, null, 2));
    return;
  }
  const i = process.argv.indexOf("--voice");
  if (i === -1) {
    console.error("need --voice <name> or --show");
    process.exit(2);
  }
  const voice = process.argv[i + 1];
  const r = await fetch(endpoint, {
    method: "PATCH",
    headers: { ...headers, Prefer: "return=representation" },
    body: JSON.stringify({ voice }),
  });
  const text = await r.text();
  if (!r.ok) {
    console.error(`PATCH failed HTTP ${r.status}: ${text.slice(0, 200)}`);
    process.exit(1);
  }
  console.log(`voice -> ${voice} (HTTP ${r.status})`);
})();
