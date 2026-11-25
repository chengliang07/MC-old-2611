#!/usr/bin/env node
/*
 * Deploy an edge function from a local directory via the Supabase Management API.
 * Used to ship a diagnostic build of the oral relay (forwards upstream error text).
 *
 * Usage: node scripts/relay_deploy.js <slug> <dir>
 */
const fs = require("fs");
const path = require("path");

const root = path.join(__dirname, "..");
const tok = fs.readFileSync(path.join(root, ".gates", "supabase.pat"), "utf8").trim();
const ref = "oemsbxavqkmtuyasxsba";
const slug = process.argv[2];
const dir = process.argv[3];
if (!slug || !dir) {
  console.error("usage: node scripts/relay_deploy.js <slug> <dir>");
  process.exit(2);
}

const files = fs.readdirSync(dir).filter((f) => f.endsWith(".ts"));
if (!files.length) {
  console.error("no .ts files in " + dir);
  process.exit(2);
}

const metadata = {
  name: slug,
  entrypoint_path: "index.ts",
  verify_jwt: false,
};

(async () => {
  const form = new FormData();
  form.append("metadata", JSON.stringify(metadata));
  for (const f of files) {
    const content = fs.readFileSync(path.join(dir, f), "utf8");
    // server writes each part under its filename, relative to the bundle root
    const rel = "source/" + f;
    form.append("file", new Blob([content], { type: "application/typescript" }), rel);
  }

  const url = `https://api.supabase.com/v1/projects/${ref}/functions/deploy?slug=${encodeURIComponent(slug)}`;
  const r = await fetch(url, {
    method: "POST",
    headers: { Authorization: `Bearer ${tok}` },
    body: form,
  });
  const text = await r.text();
  console.log(`HTTP ${r.status}`);
  console.log(text.slice(0, 600));
  if (!r.ok) process.exit(1);
  console.log(`deployed: ${slug} files=${files.join(",")}`);
})();
