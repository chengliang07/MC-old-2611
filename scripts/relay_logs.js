#!/usr/bin/env node
/* Query Supabase edge-function logs to see upstream Qwen errors.
 * Usage: node scripts/relay_logs.js [minutes]
 */
const fs = require("fs");
const path = require("path");

const root = path.join(__dirname, "..");
const tok = fs.readFileSync(path.join(root, ".gates", "supabase.pat"), "utf8").trim();
const ref = "oemsbxavqkmtuyasxsba";
const minutes = Number(process.argv[2] || 30);

const sqlCandidates = [
  "select timestamp, event_message from function_edge_logs order by timestamp desc limit 20",
  "select timestamp, event_message from edge_logs where request.path like '%oral%' order by timestamp desc limit 20",
  "select timestamp, event_message from function_logs order by timestamp desc limit 20",
];

(async () => {
  for (const sql of sqlCandidates) {
    const body = {
      sql,
      iso_timestamp_start: new Date(Date.now() - minutes * 60000).toISOString(),
      iso_timestamp_end: new Date().toISOString(),
    };
    const r = await fetch(
      `https://api.supabase.com/v1/projects/${ref}/analytics/endpoints/logs.all`,
      {
        method: "POST",
        headers: { Authorization: `Bearer ${tok}`, "Content-Type": "application/json" },
        body: JSON.stringify(body),
      }
    );
    const text = await r.text();
    console.log(`\n=== HTTP ${r.status}  sql="${sql.slice(0, 60)}..."`);
    if (!r.ok) {
      console.log(text.slice(0, 400));
      continue;
    }
    let parsed;
    try {
      parsed = JSON.parse(text);
    } catch {
      console.log(text.slice(0, 400));
      continue;
    }
    const rows = parsed.result || parsed.data || parsed;
    if (!Array.isArray(rows)) {
      console.log(JSON.stringify(parsed).slice(0, 400));
      continue;
    }
    console.log(`rows=${rows.length}`);
    for (const row of rows.slice(0, 20)) {
      const msg = row.event_message || row.msg || JSON.stringify(row);
      console.log("  " + String(msg).replace(/\n/g, " ").slice(0, 260));
    }
    if (rows.length) return;
  }
})();
