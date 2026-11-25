#!/usr/bin/env node
/*
 * Minimal WebSocket client for the oral relay, used to reproduce relay-side
 * behaviour without the device.
 *
 * Usage: node scripts/relay_probe.js [seconds] [--say]
 *   --say  after relay.ready, ask the model to speak once (audio path test)
 * Prints relay events, model/voice, upstream errors and the audio byte count.
 */

const tls = require("tls");
const crypto = require("crypto");

const HOST = "oemsbxavqkmtuyasxsba.supabase.co";
let PATHQ = "/functions/v1/oral-coach-relay-probe";
{
  const qs = [];
  const idxV = process.argv.indexOf("--voice");
  if (idxV > -1) qs.push("voice=" + encodeURIComponent(process.argv[idxV + 1] || ""));
  const idxM = process.argv.indexOf("--model");
  if (idxM > -1) qs.push("model=" + encodeURIComponent(process.argv[idxM + 1] || ""));
  const idxT = process.argv.indexOf("--text");
  if (idxT > -1) qs.push("probe_text=" + encodeURIComponent(process.argv[idxT + 1] || "hi"));
  const idxSay = process.argv.indexOf("--say-text");
  if (idxSay > -1) qs.push("say_text=" + encodeURIComponent(process.argv[idxSay + 1] || ""));
  const idxS = process.argv.indexOf("--speed");
  if (idxS > -1) qs.push("speed=" + encodeURIComponent(process.argv[idxS + 1] || "1"));
  if (qs.length) PATHQ += "?" + qs.join("&");
}

// --wav out.wav : dump received PCM (24 kHz mono s16le) to a playable file
const idxW = process.argv.indexOf("--wav");
const wavPath = idxW > -1 ? process.argv[idxW + 1] : "";
const pcmChunks = [];
let transcript = '';

function frameText(text) {
  const payload = Buffer.from(text, "utf8");
  const mask = crypto.randomBytes(4);
  const len = payload.length;
  let header;
  if (len < 126) header = Buffer.from([0x81, 0x80 | len]);
  else if (len < 65536) header = Buffer.from([0x81, 0x80 | 126, (len >> 8) & 0xff, len & 0xff]);
  else {
    header = Buffer.alloc(10);
    header[0] = 0x81;
    header[1] = 0x80 | 127;
    header.writeBigUInt64BE(BigInt(len), 2);
  }
  const masked = Buffer.alloc(len);
  for (let i = 0; i < len; i++) masked[i] = payload[i] ^ mask[i % 4];
  return Buffer.concat([header, mask, masked]);
}

function parseFrames(buf, onText, onBinary) {
  let off = 0;
  while (off + 2 <= buf.length) {
    const b1 = buf[off + 1];
    const opcode = buf[off] & 0x0f;
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
    if (off + hdr + len > buf.length) break;
    const payload = buf.subarray(off + hdr, off + hdr + len);
    off += hdr + len;
    if (opcode === 0x1) onText(payload.toString("utf8"));
    else if (opcode === 0x2) onBinary(payload);
    else if (opcode === 0x8) return { rest: Buffer.alloc(0), closed: true };
  }
  return { rest: buf.subarray(off), closed: false };
}

function frameBin(payload) {
  const mask = crypto.randomBytes(4);
  const len = payload.length;
  let header;
  if (len < 126) header = Buffer.from([0x82, 0x80 | len]);
  else if (len < 65536) header = Buffer.from([0x82, 0x80 | 126, (len >> 8) & 0xff, len & 0xff]);
  else {
    header = Buffer.alloc(10);
    header[0] = 0x82;
    header[1] = 0x80 | 127;
    header.writeBigUInt64BE(BigInt(len), 2);
  }
  const masked = Buffer.alloc(len);
  for (let i = 0; i < len; i++) masked[i] = payload[i] ^ mask[i % 4];
  return Buffer.concat([header, mask, masked]);
}

const seconds = Number(process.argv[2] || 20);
const sock = tls.connect({ host: HOST, port: 443, servername: HOST }, () => {
  const key = crypto.randomBytes(16).toString("base64");
  sock.write(
    `GET ${PATHQ} HTTP/1.1\r\nHost: ${HOST}\r\n` +
      "Upgrade: websocket\r\nConnection: Upgrade\r\n" +
      `Sec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`
  );
});

let buf = Buffer.alloc(0);
let handshaken = false;
let audioBytes = 0;
let audioFrames = 0;
const audioTimes = [];
let said = false;
const t0 = Date.now();

const timer = setTimeout(() => {
  console.log(`-- end (${seconds}s) audio=${audioBytes}B frames=${audioFrames}`);
  sock.destroy();
}, seconds * 1000);

sock.on("data", (chunk) => {
  buf = Buffer.concat([buf, chunk]);
  if (!handshaken) {
    const text = buf.toString("latin1");
    const idx = text.indexOf("\r\n\r\n");
    if (idx === -1) return;
    handshaken = true;
    console.log("status: " + text.slice(0, text.indexOf("\r\n")));
    buf = buf.subarray(idx + 4);
    sock.write(frameText(JSON.stringify({ type: "start" })));
    console.log("-- sent start");
  }
  const res = parseFrames(
    buf,
    (text) => {
      let obj;
      try {
        obj = JSON.parse(text);
      } catch {
        console.log("msg: " + text.slice(0, 160));
        return;
      }
      if (obj.type === 'relay.text.delta' && typeof obj.delta === 'string') transcript += obj.delta;
      const extra = [];
      if (obj.model) extra.push("model=" + obj.model);
      if (obj.voice) extra.push("voice=" + obj.voice);
      if (obj.error) extra.push("ERROR=" + obj.error);
      if (obj.type === "relay.qwen.event") {
        if (obj.fmt) extra.push("fmt=" + JSON.stringify(obj.fmt));
        else extra.push("raw=" + JSON.stringify(obj).slice(0, 200));
      }
      console.log(
        `${String(Date.now() - t0).padStart(6)}ms ${obj.type}${extra.length ? "  " + extra.join("  ") : ""}`
      );
      if (obj.type === "relay.response.done" && wavPath) {
        setTimeout(() => {
          console.log("-- response done, closing");
          sock.destroy();
        }, 1300);
      }
      if (obj.type === "relay.ready" && process.argv.includes("--say") && !said) {
        said = true;
        // 闂佺绻愰悧鍡涘级?1 缂?16k 闂佸憡顨嗗ú鏍晬閹扮増鐒?PCM闂佹寧绋戦悧濠囶敆濠婂喚鍤曢柨鏃囧亹缁€鍡涙煥濞戞鏆爋mmit 闂佸憡鑹炬鎼佸疮閳ь剟鎮规担鍙夋珕閼垛晠鏌涢妸銉剰缂佽鲸鍨垮畷锝夋晲韫囨洜鐛?        // 濠电偛澶囬崜婵嗭耿?user message 闂佸搫鍟﹢鍦箔閸屾哎鈧帡姊归幇顔剧崶闂佸綊鏀辩敮鐐靛垝?response.create闂?        let n = 0;
        const total = 32;
        const chunk = 512; // 32 ms
        const feeder = setInterval(() => {
          const pcm = Buffer.alloc(chunk * 2);
          for (let i = 0; i < chunk; i++) {
            const v = Math.round(1200 * Math.sin((2 * Math.PI * 220 * (n * chunk + i)) / 16000));
            pcm.writeInt16LE(v, i * 2);
          }
          sock.write(Buffer.concat([frameBin(pcm)]));
          n++;
          if (n >= total) {
            clearInterval(feeder);
            sock.write(frameText(JSON.stringify({ type: "commit" })));
            console.log("-- sent 1s audio + commit");
            setTimeout(() => {
              sock.write(
                frameText(
                  JSON.stringify({
                    type: "response.create",
                    instructions: "Say one short sentence to test the audio path.",
                  })
                )
              );
              console.log("-- sent response.create");
            }, 1200);
          }
        }, 32);
      }
    },
    (payload) => {
      audioBytes += payload.length;
      audioFrames += 1;
      audioTimes.push(Date.now());
      if (wavPath) pcmChunks.push(Buffer.from(payload));
    }
  );  buf = res.rest;
  if (res.closed) {
    console.log("-- closed by relay");
    clearTimeout(timer);
    sock.destroy();
  }
});

sock.on("error", (e) => console.log("socket error: " + e.message));
sock.on("close", () => {
  clearTimeout(timer);
  if (wavPath && pcmChunks.length) {
    const pcm = Buffer.concat(pcmChunks);
    const h = Buffer.alloc(44);
    h.write("RIFF", 0);
    h.writeUInt32LE(36 + pcm.length, 4);
    h.write("WAVEfmt ", 8);
    h.writeUInt32LE(16, 16);
    h.writeUInt16LE(1, 20);
    h.writeUInt16LE(1, 22);
    h.writeUInt32LE(24000, 24);
    h.writeUInt32LE(24000 * 2, 28);
    h.writeUInt16LE(2, 32);
    h.writeUInt16LE(16, 34);
    h.write("data", 36);
    h.writeUInt32LE(pcm.length, 40);
    require("fs").writeFileSync(wavPath, Buffer.concat([h, pcm]));
    console.log(`-- wav ${wavPath} (${(pcm.length / 48000).toFixed(2)}s @24k)`);
  }
  if (audioTimes.length > 1) { const gaps = []; for (let i = 1; i < audioTimes.length; i++) gaps.push(audioTimes[i] - audioTimes[i-1]); const avg = gaps.reduce((a,b)=>a+b,0)/gaps.length; console.log(`-- audio cadence avg=${avg.toFixed(0)}ms min=${Math.min(...gaps)} max=${Math.max(...gaps)}`); }
  { const words = transcript.trim().split(/\s+/).filter(Boolean).length; const secs = audioBytes / 48000; console.log(`-- transcript words=${words} audioSec=${secs.toFixed(2)} wps=${secs > 0 ? (words / secs).toFixed(2) : "n/a"}`); console.log("-- text: " + transcript.trim().slice(0, 160)); }
  console.log(`-- closed audio=${audioBytes}B frames=${audioFrames}`);
});
