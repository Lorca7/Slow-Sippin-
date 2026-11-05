/*
 * codec.js — reading and writing audio files.
 *
 * WAV and AIFF/AIFC are parsed here directly (at their native sample rate,
 * any common bit depth, including float). MP3 and FLAC go through the
 * browser's decoder, run at the file's own sample rate where it can be read
 * from the header, so nothing is resampled on the way in.
 *
 * Export: WAV or AIFF at 16-bit (TPDF dither), 24-bit or 32-bit float.
 */
(function (root) {
  'use strict';

  function str(u8, off, len) {
    let s = '';
    for (let i = 0; i < len && off + i < u8.length; i++) s += String.fromCharCode(u8[off + i]);
    return s;
  }

  function alloc(ch, frames) {
    const out = [];
    for (let c = 0; c < ch; c++) out.push(new Float32Array(frames));
    return out;
  }

  // ----------------------------------------------------------------- WAV --

  function parseWAV(ab) {
    const u8 = new Uint8Array(ab);
    if (u8.length < 12) return null;
    const riff = str(u8, 0, 4);
    if ((riff !== 'RIFF' && riff !== 'RF64') || str(u8, 8, 4) !== 'WAVE') return null;
    const dv = new DataView(ab);
    let off = 12, fmt = null, dataOff = -1, dataLen = 0, ds64 = null;
    while (off + 8 <= u8.length) {
      const id = str(u8, off, 4);
      const size = dv.getUint32(off + 4, true);
      const body = off + 8;
      if (id === 'ds64' && body + 16 <= u8.length) {
        ds64 = dv.getUint32(body + 8, true) + dv.getUint32(body + 12, true) * 4294967296;
      } else if (id === 'fmt ' && body + 16 <= u8.length) {
        let tag = dv.getUint16(body, true);
        if (tag === 0xFFFE && size >= 26) tag = dv.getUint16(body + 24, true);
        fmt = {
          tag,
          ch: dv.getUint16(body + 2, true),
          sr: dv.getUint32(body + 4, true),
          align: dv.getUint16(body + 12, true),
          bits: dv.getUint16(body + 14, true),
        };
      } else if (id === 'data') {
        dataOff = body;
        dataLen = (size === 0xFFFFFFFF && ds64 != null) ? ds64 : size;
        dataLen = Math.min(dataLen, u8.length - body);
        break;
      }
      off = body + size + (size & 1);
    }
    if (!fmt || dataOff < 0 || !fmt.ch || !fmt.sr) return null;
    const ch = fmt.ch;
    const bytes = Math.floor(fmt.align / ch) || Math.ceil(fmt.bits / 8);
    const isFloat = fmt.tag === 3;
    if (!(fmt.tag === 1 || isFloat)) return null;
    if (isFloat && bytes !== 4 && bytes !== 8) return null;
    if (!isFloat && (bytes < 1 || bytes > 4)) return null;
    const frames = Math.floor(dataLen / (bytes * ch));
    const out = alloc(ch, frames);
    const stride = bytes * ch;
    for (let c = 0; c < ch; c++) {
      const d = out[c];
      let p = dataOff + c * bytes;
      if (isFloat && bytes === 4) for (let i = 0; i < frames; i++, p += stride) d[i] = dv.getFloat32(p, true);
      else if (isFloat) for (let i = 0; i < frames; i++, p += stride) d[i] = dv.getFloat64(p, true);
      else if (bytes === 1) for (let i = 0; i < frames; i++, p += stride) d[i] = (u8[p] - 128) / 128;
      else if (bytes === 2) for (let i = 0; i < frames; i++, p += stride) d[i] = ((u8[p] | (u8[p + 1] << 8)) << 16 >> 16) / 32768;
      else if (bytes === 3) for (let i = 0; i < frames; i++, p += stride) d[i] = ((u8[p] | (u8[p + 1] << 8) | (u8[p + 2] << 16)) << 8 >> 8) / 8388608;
      else for (let i = 0; i < frames; i++, p += stride) d[i] = dv.getInt32(p, true) / 2147483648;
    }
    return { sampleRate: fmt.sr, channels: out };
  }

  // ---------------------------------------------------------------- AIFF --

  function readExt80(dv, o) {
    const se = dv.getUint16(o);
    const sign = se & 0x8000 ? -1 : 1;
    const e = se & 0x7FFF;
    const hi = dv.getUint32(o + 2), lo = dv.getUint32(o + 6);
    if (e === 0 && hi === 0 && lo === 0) return 0;
    return sign * (hi * Math.pow(2, e - 16383 - 31) + lo * Math.pow(2, e - 16383 - 63));
  }

  function writeExt80(dv, o, v) {
    const e = Math.floor(Math.log2(v));
    const m = v / Math.pow(2, e);
    const hi = Math.floor(m * 2147483648);
    const lo = Math.floor((m * 2147483648 - hi) * 4294967296);
    dv.setUint16(o, e + 16383);
    dv.setUint32(o + 2, hi >>> 0);
    dv.setUint32(o + 6, lo >>> 0);
  }

  function parseAIFF(ab) {
    const u8 = new Uint8Array(ab);
    if (u8.length < 12 || str(u8, 0, 4) !== 'FORM') return null;
    const form = str(u8, 8, 4);
    if (form !== 'AIFF' && form !== 'AIFC') return null;
    const dv = new DataView(ab);
    let off = 12, comm = null, ssnd = -1;
    while (off + 8 <= u8.length) {
      const id = str(u8, off, 4);
      const size = dv.getUint32(off + 4);
      const body = off + 8;
      if (id === 'COMM' && body + 18 <= u8.length) {
        comm = {
          ch: dv.getInt16(body),
          frames: dv.getUint32(body + 2),
          bits: dv.getInt16(body + 6),
          sr: readExt80(dv, body + 8),
          comp: form === 'AIFC' && size >= 22 ? str(u8, body + 18, 4) : 'NONE',
        };
      } else if (id === 'SSND' && body + 8 <= u8.length) {
        ssnd = body + 8 + dv.getUint32(body);
      }
      off = body + size + (size & 1);
    }
    if (!comm || ssnd < 0 || comm.ch < 1 || !(comm.sr > 0)) return null;
    const comp = comm.comp;
    let kind;
    if (comp === 'NONE' || comp === 'twos') kind = 'be';
    else if (comp === 'sowt') kind = 'le';
    else if (comp === 'fl32' || comp === 'FL32') kind = 'f32';
    else if (comp === 'fl64' || comp === 'FL64') kind = 'f64';
    else return null;
    const bytes = kind === 'f32' ? 4 : kind === 'f64' ? 8 : Math.ceil(comm.bits / 8);
    if (bytes < 1 || bytes > 8) return null;
    const ch = comm.ch, stride = bytes * ch;
    const frames = Math.min(comm.frames, Math.floor((u8.length - ssnd) / stride));
    const out = alloc(ch, frames);
    for (let c = 0; c < ch; c++) {
      const d = out[c];
      let p = ssnd + c * bytes;
      if (kind === 'f32') for (let i = 0; i < frames; i++, p += stride) d[i] = dv.getFloat32(p);
      else if (kind === 'f64') for (let i = 0; i < frames; i++, p += stride) d[i] = dv.getFloat64(p);
      else if (kind === 'le' && bytes === 2) for (let i = 0; i < frames; i++, p += stride) d[i] = dv.getInt16(p, true) / 32768;
      else if (bytes === 1) for (let i = 0; i < frames; i++, p += stride) d[i] = dv.getInt8(p) / 128;
      else if (bytes === 2) for (let i = 0; i < frames; i++, p += stride) d[i] = ((u8[p] << 8 | u8[p + 1]) << 16 >> 16) / 32768;
      else if (bytes === 3 && kind === 'be') for (let i = 0; i < frames; i++, p += stride) d[i] = ((u8[p] << 16 | u8[p + 1] << 8 | u8[p + 2]) << 8 >> 8) / 8388608;
      else if (bytes === 3) for (let i = 0; i < frames; i++, p += stride) d[i] = ((u8[p + 2] << 16 | u8[p + 1] << 8 | u8[p]) << 8 >> 8) / 8388608;
      else if (bytes === 4) for (let i = 0; i < frames; i++, p += stride) d[i] = dv.getInt32(p, kind === 'le') / 2147483648;
      else return null;
    }
    return { sampleRate: Math.round(comm.sr), channels: out };
  }

  // ------------------------------------------------- MP3 / FLAC: sniffing --

  /** Read the native sample rate from a FLAC or MP3 header (or null). */
  function sniffRate(ab) {
    const u8 = new Uint8Array(ab);
    if (str(u8, 0, 4) === 'fLaC' && u8.length > 21) {
      const sr = (u8[18] << 12) | (u8[19] << 4) | (u8[20] >> 4);
      return sr > 0 ? sr : null;
    }
    let o = 0;
    if (str(u8, 0, 3) === 'ID3' && u8.length > 10) {
      const size = (u8[6] & 0x7F) << 21 | (u8[7] & 0x7F) << 14 | (u8[8] & 0x7F) << 7 | (u8[9] & 0x7F);
      o = 10 + size + ((u8[5] & 0x10) ? 10 : 0);
    }
    const end = Math.min(u8.length - 4, o + 131072);
    for (let i = o; i < end; i++) {
      if (u8[i] !== 0xFF || (u8[i + 1] & 0xE0) !== 0xE0) continue;
      const ver = (u8[i + 1] >> 3) & 3, layer = (u8[i + 1] >> 1) & 3;
      const br = u8[i + 2] >> 4, sri = (u8[i + 2] >> 2) & 3;
      if (ver === 1 || layer === 0 || br === 0 || br === 15 || sri === 3) continue;
      const basis = [44100, 48000, 32000][sri];
      return ver === 3 ? basis : ver === 2 ? basis / 2 : basis / 4;
    }
    return null;
  }

  function toAudioBuffer(ctx, parsed) {
    const ch = parsed.channels.length, len = Math.max(1, parsed.channels[0].length);
    const buf = ctx.createBuffer(ch, len, parsed.sampleRate);
    for (let c = 0; c < ch; c++) buf.copyToChannel(parsed.channels[c], c);
    return buf;
  }

  /** ArrayBuffer → AudioBuffer at the file's own sample rate. */
  async function decode(ab, ctx) {
    let parsed = null;
    try { parsed = parseWAV(ab) || parseAIFF(ab); } catch (e) { parsed = null; }
    if (parsed && parsed.channels[0].length > 0) return toAudioBuffer(ctx, parsed);

    const rate = sniffRate(ab);
    if (rate && typeof OfflineAudioContext !== 'undefined') {
      try {
        const oc = new OfflineAudioContext(1, 1, rate);
        return await oc.decodeAudioData(ab.slice(0));
      } catch (e) { /* fall through to the live context */ }
    }
    return await new Promise((resolve, reject) => {
      const p = ctx.decodeAudioData(ab.slice(0), resolve, reject);
      if (p && p.then) p.then(resolve, reject);
    });
  }

  // ------------------------------------------------------------- encoding --

  const CHUNK_FRAMES = 1 << 18;

  /**
   * Interleave + quantise `chans` (scaled by `gain`) into Blob parts.
   * bits: 16 | 24 | 32 (32 = float). little: endianness.
   */
  function pcmParts(chans, bits, gain, little) {
    const ch = chans.length, n = chans[0].length, bps = bits / 8;
    const parts = [];
    const rng = Math.random;
    for (let f0 = 0; f0 < n; f0 += CHUNK_FRAMES) {
      const f1 = Math.min(n, f0 + CHUNK_FRAMES);
      const buf = new ArrayBuffer((f1 - f0) * ch * bps);
      const dv = new DataView(buf);
      let p = 0;
      for (let i = f0; i < f1; i++) {
        for (let c = 0; c < ch; c++) {
          let v = chans[c][i] * gain;
          if (bits === 32) {
            dv.setFloat32(p, v, little);
          } else if (bits === 16) {
            v = v * 32767 + (rng() - rng());
            v = Math.round(v);
            v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
            dv.setInt16(p, v, little);
          } else {
            v = Math.round(v * 8388607);
            v = v > 8388607 ? 8388607 : v < -8388608 ? -8388608 : v;
            if (little) {
              dv.setUint8(p, v & 0xFF); dv.setUint8(p + 1, (v >> 8) & 0xFF); dv.setUint8(p + 2, (v >> 16) & 0xFF);
            } else {
              dv.setUint8(p, (v >> 16) & 0xFF); dv.setUint8(p + 1, (v >> 8) & 0xFF); dv.setUint8(p + 2, v & 0xFF);
            }
          }
          p += bps;
        }
      }
      parts.push(buf);
    }
    return parts;
  }

  function encodeWAV(chans, sr, bits, gain) {
    const ch = chans.length, n = chans[0].length, bps = bits / 8;
    const dataLen = n * ch * bps;
    if (dataLen + 44 > 0xFFFFFFFF) throw new Error('file larger than 4 GB — use a shorter output');
    const h = new ArrayBuffer(44), dv = new DataView(h);
    const w = (o, s) => { for (let i = 0; i < 4; i++) dv.setUint8(o + i, s.charCodeAt(i)); };
    w(0, 'RIFF'); dv.setUint32(4, 36 + dataLen, true); w(8, 'WAVE');
    w(12, 'fmt '); dv.setUint32(16, 16, true);
    dv.setUint16(20, bits === 32 ? 3 : 1, true);
    dv.setUint16(22, ch, true);
    dv.setUint32(24, sr, true);
    dv.setUint32(28, sr * ch * bps, true);
    dv.setUint16(32, ch * bps, true);
    dv.setUint16(34, bits, true);
    w(36, 'data'); dv.setUint32(40, dataLen, true);
    return new Blob([h].concat(pcmParts(chans, bits, gain, true)), { type: 'audio/wav' });
  }

  function encodeAIFF(chans, sr, bits, gain) {
    const ch = chans.length, n = chans[0].length, bps = bits / 8;
    const dataLen = n * ch * bps;
    const isFloat = bits === 32;
    // AIFC is required for float samples.
    const commLen = isFloat ? 18 + 4 + 14 : 18;
    const fver = isFloat ? 12 : 0;
    const headLen = 12 + fver + 8 + commLen + 16;
    if (headLen + dataLen > 0xFFFFFFFF) throw new Error('file larger than 4 GB — use a shorter output');
    const h = new ArrayBuffer(headLen), dv = new DataView(h);
    const w = (o, s) => { for (let i = 0; i < s.length; i++) dv.setUint8(o + i, s.charCodeAt(i)); };
    let o = 0;
    w(o, 'FORM'); dv.setUint32(o + 4, headLen - 8 + dataLen); w(o + 8, isFloat ? 'AIFC' : 'AIFF'); o += 12;
    if (isFloat) { w(o, 'FVER'); dv.setUint32(o + 4, 4); dv.setUint32(o + 8, 0xA2805140); o += 12; }
    w(o, 'COMM'); dv.setUint32(o + 4, commLen);
    dv.setInt16(o + 8, ch);
    dv.setUint32(o + 10, n);
    dv.setInt16(o + 14, bits);
    writeExt80(dv, o + 16, sr);
    if (isFloat) {
      w(o + 26, 'fl32');
      dv.setUint8(o + 30, 12); w(o + 31, '32-bit float'); dv.setUint8(o + 43, 0);
    }
    o += 8 + commLen;
    w(o, 'SSND'); dv.setUint32(o + 4, 8 + dataLen); dv.setUint32(o + 8, 0); dv.setUint32(o + 12, 0);
    return new Blob([h].concat(pcmParts(chans, bits, gain, false)), { type: 'audio/aiff' });
  }

  const Codec = { parseWAV, parseAIFF, sniffRate, decode, encodeWAV, encodeAIFF };
  if (typeof module !== 'undefined' && module.exports) module.exports = Codec;
  root.Codec = Codec;
})(typeof self !== 'undefined' ? self : this);
