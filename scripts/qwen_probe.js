#!/usr/bin/env node
/*
 * Probe the Bailian realtime WebSocket exactly like the relay does, and print
 * only the HTTP status line + server event types / error text (never keys).
 *
 * Usage: node scripts/qwen_probe.js [model] [--payload]
 *   model   default qwen-audio-3.1-realtime-plus
 *   --payload  also send the relay's session.update and report the reply
 *
 * Reads BAILIAN_API_KEY / BAILIAN_WORKSPACE_ID / BAILIAN_REALTIME_REGION from
 * the newest .gates/secrets_backup_*.json.
 */

const fs = require("fs");
const path = require("path");
const tls = require("tls");
const crypto = require("crypto");

const root = path.join(__dirname, "..");
const gateDir = path.join(root, ".gates");

function loadSecrets() {
  const file = fs
    .readdirSync(gateDir)
    .filter((f) => f.startsWith("secrets_backup_") && f.endsWith(".json"))
    .sort()
    .pop();
  if (!file) throw new Error("no secrets_backup_*.json in .gates");
  const list = JSON.parse(fs.readFileSync(path.join(gateDir, file), "utf8"));
  const get = (n) => {
    const hit = list.find((s) => s.name === n);
    return hit && typeof hit.value === "string" ? hit.value : "";
  };
  return { get, file };
}

function frameText(text) {
  const payload = Buffer.from(text, "utf8");
  const mask = crypto.randomBytes(4);
  const len = payload.length;
  let header;
  if (len < 126) {
    header = Buffer.from([0x81, 0x80 | len]);
  } else if (len < 65536) {
    header = Buffer.from([0x81, 0x80 | 126, (len >> 8) & 0xff, len & 0xff]);
  } else {
    header = Buffer.alloc(10);
    header[0] = 0x81;
    header[1] = 0x80 | 127;
    header.writeBigUInt64BE(BigInt(len), 2);
  }
  const masked = Buffer.alloc(len);
  for (let i = 0; i < len; i++) masked[i] = payload[i] ^ mask[i % 4];
  return Buffer.concat([header, mask, masked]);
}

function parseFrames(buf, onMessage) {
  let off = 0;
  while (off + 2 <= buf.length) {
    const b0 = buf[off];
    const b1 = buf[off + 1];
    const opcode = b0 & 0x0f;
    let len = b1 & 0x7f;
    let hdr = 2;
    if (len === 126) {
      if (off + 4 > buf.length) break;
      len = buf.readUInt16BE(off + 2);
      hdr = 4;
    } else if (len === 127) {
      if (off + 10 > buf.length) break;
      len = Number(buf.readBigUInt64BE(off + 2));
      hdr = 10;
    }
    const maskedSrv = (b1 & 0x80) !== 0;
    let maskKey = null;
    if (maskedSrv) {
      if (off + hdr + 4 > buf.length) break;
      maskKey = buf.subarray(off + hdr, off + hdr + 4);
      hdr += 4;
    }
    if (off + hdr + len > buf.length) break;
    let payload = Buffer.from(buf.subarray(off + hdr, off + hdr + len));
    if (maskKey) for (let i = 0; i < len; i++) payload[i] ^= maskKey[i % 4];
    off += hdr + len;
    if (opcode === 0x1) onMessage(payload.toString("utf8"));
    if (opcode === 0x8) return { rest: Buffer.alloc(0), closed: true };
  }
  return { rest: buf.subarray(off), closed: false };
}

function main() {
  const model = process.argv[2] || "qwen-audio-3.1-realtime-plus";
  const withPayload = process.argv.includes("--payload");
  const { get, file } = loadSecrets();
  const key = get("BAILIAN_API_KEY");
  const workspace = get("BAILIAN_WORKSPACE_ID");
  const region = get("BAILIAN_REALTIME_REGION") || "cn";
  if (!key || !workspace) {
    console.log("missing BAILIAN_API_KEY / BAILIAN_WORKSPACE_ID in " + file);
    process.exit(2);
  }
  const host = `${workspace}.${region === "intl" ? "ap-southeast-1" : "cn-beijing"}.maas.aliyuncs.com`;
  const reqPath = `/api-ws/v1/realtime?model=${encodeURIComponent(model)}`;
  console.log(`secrets: ${file}`);
  console.log(`host:    ${host}`);
  console.log(`path:    ${reqPath}`);

  const sock = tls.connect({ host, port: 443, servername: host }, () => {
    const wsKey = crypto.randomBytes(16).toString("base64");
    sock.write(
      `GET ${reqPath} HTTP/1.1\r\n` +
        `Host: ${host}\r\n` +
        `Authorization: Bearer ${key}\r\n` +
        "Upgrade: websocket\r\n" +
        "Connection: Upgrade\r\n" +
        `Sec-WebSocket-Key: ${wsKey}\r\n` +
        "Sec-WebSocket-Version: 13\r\n\r\n"
    );
  });

  let buf = Buffer.alloc(0);
  let handshakeDone = false;
  let events = 0;
  const timeout = setTimeout(() => {
    console.log("-- timeout, closing");
    sock.destroy();
  }, withPayload ? 15000 : 8000);

  sock.on("data", (chunk) => {
    buf = Buffer.concat([buf, chunk]);
    if (!handshakeDone) {
      const text = buf.toString("latin1");
      const idx = text.indexOf("\r\n\r\n");
      if (idx === -1) return;
      handshakeDone = true;
      const head = text.slice(0, idx);
      console.log("status:  " + head.split("\r\n")[0]);
      for (const line of head.split("\r\n").slice(1)) {
        if (/^(location|retry-after|x-request-id|x-ms-error-code)/i.test(line)) {
          console.log("header:  " + line.trim());
        }
      }
      buf = buf.subarray(idx + 4);
      if (!head.startsWith("HTTP/1.1 101")) {
        console.log("-- handshake rejected");
        clearTimeout(timeout);
        sock.destroy();
        return;
      }
      if (withPayload) {
        const session = {
          type: "session.update",
          session: {
            modalities: ["text", "audio"],
            audio: {
              input: {
                format: {
                  type: "pcm",
                  sample_rate: 16000,
                  sample_format: "s16le",
                  channels: 1,
                  packing: "interleaved",
                  channel_layout: "mono",
                },
              },
              output: {
                voice: process.env.PROBE_VOICE || "Tina",
                format: { type: "pcm", sample_rate: 24000 },
              },
            },
            input_audio_transcription: { model: "qwen3-asr-flash" },
            turn_detection: { type: "server_vad", threshold: 0.5, silence_duration_ms: 500, prefix_padding_ms: 300 },
            instructions: "You are a friendly English speaking coach.",
            temperature: 0.7,
            max_tokens: 256,
          },
        };
        sock.write(frameText(JSON.stringify(session)));
        console.log("-- sent session.update");
      }
    }

    const res = parseFrames(buf, (text) => {
      events++;
      let obj;
      try {
        obj = JSON.parse(text);
      } catch {
        console.log("msg:     " + text.slice(0, 200));
        return;
      }
      const type = obj.type || "?";
      if (obj.error) {
        console.log(`event:   ${type}  ERROR ${JSON.stringify(obj.error)}`);
      } else if (type === "session.created" || type === "session.updated") {
        const s = obj.session || {};
        console.log(`event:   ${type}  model=${s.model || "?"} voice=${(s.audio && s.audio.output && s.audio.output.voice) || "?"}`);
      } else {
        console.log(`event:   ${type}`);
      }
      if (events >= 6) {
        clearTimeout(timeout);
        sock.destroy();
      }
    });
    buf = res.rest;
    if (res.closed) {
      console.log("-- closed by peer");
      clearTimeout(timeout);
      sock.destroy();
    }
  });

  sock.on("error", (e) => console.log("socket error: " + e.message));
  sock.on("close", () => {
    clearTimeout(timeout);
    console.log("-- done, events=" + events);
  });
}

main();
