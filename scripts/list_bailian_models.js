#!/usr/bin/env node
/* List the Bailian/DashScope models this API key can actually use.
 * Prints model IDs only (never the key). Usage: node scripts/list_bailian_models.js [filter]
 */
const fs = require("fs");
const path = require("path");

const root = path.join(__dirname, "..");
const gate = path.join(root, ".gates");
const file = fs
  .readdirSync(gate)
  .filter((f) => f.startsWith("secrets_backup_") && f.endsWith(".json"))
  .sort()
  .pop();
const list = JSON.parse(fs.readFileSync(path.join(gate, file), "utf8"));
const key = (list.find((s) => s.name === "BAILIAN_API_KEY") || {}).value || "";
if (!key) {
  console.error("no BAILIAN_API_KEY in " + file);
  process.exit(2);
}
const filter = (process.argv[2] || "audio|omni|realtime").toLowerCase();
const re = new RegExp(filter);
const ws = "https://dashscope.aliyuncs.com";

(async () => {
  // 1) OpenAI-compatible model list
  for (const url of [
    `${ws}/compatible-mode/v1/models`,
    `${ws}/api/v1/models`,
  ]) {
    try {
      const r = await fetch(url, { headers: { Authorization: `Bearer ${key}` } });
      const text = await r.text();
      console.log(`${url} -> HTTP ${r.status}`);
      if (!r.ok) {
        console.log("  " + text.slice(0, 200));
        continue;
      }
      let ids = [];
      try {
        const j = JSON.parse(text);
        const arr = j.data || j.models || j.output?.models || [];
        ids = arr.map((m) => m.id || m.model || m.name).filter(Boolean);
      } catch (e) {
        console.log("  parse failed: " + text.slice(0, 120));
        continue;
      }
      console.log(`  total=${ids.length}, matching /${filter}/:`);
      for (const id of ids.filter((i) => re.test(String(i).toLowerCase()))) {
        console.log("    " + id);
      }
      if (!ids.length) console.log("  (empty list)");
      return;
    } catch (e) {
      console.log(`${url} -> ERR ${e.message}`);
    }
  }
})();
