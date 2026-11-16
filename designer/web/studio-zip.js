// SPDX-License-Identifier: MIT

// The studio's zip container: a reader and a writer with **no dependency** (issue #391, D-S4 of
// docs/specs/designer-studio-v1.md section 7).
//
// The acceptance names two load paths and only one of them exists in every browser: Chrome and Edge
// have the File System Access API, every other browser does not. So a project travels as a zip --
// dragged in to load it, downloaded to export it -- and this module is that container. The issue
// forbids a new dependency for it (no JSZip), so this is the format itself:
//
//   * **Writer: STORE only.** Every entry is stored (method 0), with the CRC-32 the format requires.
//     A preview project is text; a few hundred kilobytes of CSS and TSX compressing to nothing is not
//     worth a deflate implementation, and STORE is the one method every unzip tool accepts without
//     negotiation. The output is **deterministic**: entries are sorted by path and the timestamp is
//     fixed at 1980-01-01, so the same project writes the same bytes twice and the round-trip test
//     can compare bytes rather than "equivalent archives".
//   * **Reader: STORE and DEFLATE.** A zip a person actually has (from Finder, Explorer, `zip -r`,
//     a CI artifact) is deflated, so refusing method 8 would refuse almost every real file. The
//     inflater is the platform's own `DecompressionStream("deflate-raw")` -- the same browser
//     primitive `share.js` uses for the share link, and the same rule: a platform API, never a
//     library. A browser without it gets a named refusal instead of a silent empty import.
//
// **What the reader does not do.** It does not follow a symlink out of the archive: a Unix symlink
// entry (`S_IFLNK` in the external attributes) is refused by `readZip`, because a preview project is
// files and a link is a path to somewhere else (spec section 10). It does not read a ZIP64 archive
// (the studio's own artifacts are bounded far below 4 GiB) and does not decrypt. Each refusal is a
// `ZipError` with a code the page branches on, never a message match.
//
// Paths are the caller's business, not this module's: `readZip` returns entry names verbatim and says
// nothing about whether they are safe. `project-paths.js` is the one place that decides that, and a
// shell renders a hostile archive as a named refusal before any entry is kept in memory. Keeping that
// split is what lets this module be tested without a page and reused by any shell.

/** A refusal the page branches on by `code`. */
export class ZipError extends Error {
  /**
   * @param {string} code `signature` | `truncated` | `zip64` | `encrypted` | `method` |
   *   `no-inflater` | `crc` | `duplicate` | `symlink` | `directory-entry`
   * @param {string} message a sentence for a person
   */
  constructor(code, message) {
    super(message);
    this.name = 'ZipError';
    this.code = code;
  }
}

// Signatures of the four records this module reads or writes (APPNOTE 4.3).
const SIG_LOCAL = 0x04034b50;
const SIG_CENTRAL = 0x02014b50;
const SIG_EOCD = 0x06054b50;
const SIG_ZIP64_LOCATOR = 0x07064b50;

// Flag bits: bit 0 is "encrypted", bit 3 is "sizes in the data descriptor", bit 11 is "the name is
// UTF-8". The writer sets only bit 11, because every path a project uses is UTF-8 by definition.
const FLAG_ENCRYPTED = 0x0001;
const FLAG_UTF8 = 0x0800;

// A fixed MS-DOS timestamp: 1980-01-01 00:00. Not an accident -- a zip with "now" in it is not
// reproducible, and a download a visitor compares with the previous one should differ only where the
// project did.
const DOS_TIME = 0x0000;
const DOS_DATE = 0x0021; // (1980-1980) << 9 | 1 << 5 | 1

const S_IFLNK = 0xa000;

const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder('utf-8');

// --- CRC-32 (APPNOTE 4.4.7) ---------------------------------------------------------------------

// The table is built once from the polynomial. The format stores a CRC of every entry and every
// archive tool verifies it, so writing a zero here would produce a file that opens in some readers and
// fails in others; computing it is 20 lines.
const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let index = 0; index < 256; index += 1) {
    let value = index;
    for (let bit = 0; bit < 8; bit += 1) {
      value = value & 1 ? 0xedb88320 ^ (value >>> 1) : value >>> 1;
    }
    table[index] = value >>> 0;
  }
  return table;
})();

/** The CRC-32 of `bytes` (a `Uint8Array`), as the format stores it. */
export function crc32(bytes) {
  let crc = 0xffffffff;
  for (let index = 0; index < bytes.length; index += 1) {
    crc = CRC_TABLE[(crc ^ bytes[index]) & 0xff] ^ (crc >>> 8);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

// --- writer -------------------------------------------------------------------------------------

/**
 * Writes a zip of `entries`, stored (uncompressed) and deterministic.
 *
 * @param {Array<{path: string, bytes: Uint8Array}>} entries
 * @returns {Uint8Array} the archive
 */
export function writeZip(entries) {
  // Sorted so the bytes do not depend on the order a caller walked a directory handle, and so a
  // round trip through this module is byte-identical to the original export.
  const sorted = [...entries].sort((left, right) => (left.path < right.path ? -1 : left.path > right.path ? 1 : 0));
  const locals = [];
  const centrals = [];
  let offset = 0;

  for (const entry of sorted) {
    const name = textEncoder.encode(entry.path);
    const data = entry.bytes instanceof Uint8Array ? entry.bytes : new Uint8Array(entry.bytes);
    const crc = crc32(data);

    const local = new Uint8Array(30 + name.length);
    const localView = new DataView(local.buffer);
    localView.setUint32(0, SIG_LOCAL, true);
    localView.setUint16(4, 20, true); // version needed: 2.0
    localView.setUint16(6, FLAG_UTF8, true);
    localView.setUint16(8, 0, true); // method: store
    localView.setUint16(10, DOS_TIME, true);
    localView.setUint16(12, DOS_DATE, true);
    localView.setUint32(14, crc, true);
    localView.setUint32(18, data.length, true);
    localView.setUint32(22, data.length, true);
    localView.setUint16(26, name.length, true);
    localView.setUint16(28, 0, true); // extra field length
    local.set(name, 30);

    const central = new Uint8Array(46 + name.length);
    const centralView = new DataView(central.buffer);
    centralView.setUint32(0, SIG_CENTRAL, true);
    // Version made by: 3.0, host 0 (MS-DOS/FAT). The host matters: a Unix host makes readers look at
    // the external attributes, and a preview project has no file modes to declare.
    centralView.setUint16(4, 20, true);
    centralView.setUint16(6, 20, true);
    centralView.setUint16(8, FLAG_UTF8, true);
    centralView.setUint16(10, 0, true);
    centralView.setUint16(12, DOS_TIME, true);
    centralView.setUint16(14, DOS_DATE, true);
    centralView.setUint32(16, crc, true);
    centralView.setUint32(20, data.length, true);
    centralView.setUint32(24, data.length, true);
    centralView.setUint16(28, name.length, true);
    centralView.setUint16(30, 0, true); // extra
    centralView.setUint16(32, 0, true); // comment
    centralView.setUint16(34, 0, true); // disk number start
    centralView.setUint16(36, 0, true); // internal attributes
    centralView.setUint32(38, 0, true); // external attributes
    centralView.setUint32(42, offset, true);
    central.set(name, 46);

    locals.push(local, data);
    centrals.push(central);
    offset += local.length + data.length;
  }

  const centralSize = centrals.reduce((total, part) => total + part.length, 0);
  const end = new Uint8Array(22);
  const endView = new DataView(end.buffer);
  endView.setUint32(0, SIG_EOCD, true);
  endView.setUint16(4, 0, true); // this disk
  endView.setUint16(6, 0, true); // disk with the central directory
  endView.setUint16(8, sorted.length, true);
  endView.setUint16(10, sorted.length, true);
  endView.setUint32(12, centralSize, true);
  endView.setUint32(16, offset, true);
  endView.setUint16(20, 0, true); // comment length

  const total = offset + centralSize + end.length;
  const out = new Uint8Array(total);
  let cursor = 0;
  for (const part of locals) {
    out.set(part, cursor);
    cursor += part.length;
  }
  for (const part of centrals) {
    out.set(part, cursor);
    cursor += part.length;
  }
  out.set(end, cursor);
  return out;
}

// --- reader -------------------------------------------------------------------------------------

/**
 * Reads a zip into `[{ path, bytes }]`, in central-directory order.
 *
 * Both methods a real archiver produces are supported: store and deflate (through the platform's
 * `DecompressionStream`). Every entry's CRC is verified, so a truncated or corrupted download is a
 * `crc` refusal rather than a project that half-loaded.
 *
 * A symlink entry is refused (`symlink`), a duplicate entry name is refused (`duplicate`), and a
 * directory entry (a name ending in `/`) is skipped: the tree is derived from file paths, and an
 * empty directory carries nothing a project needs.
 *
 * @param {Uint8Array} bytes
 * @returns {Promise<Array<{path: string, bytes: Uint8Array}>>}
 */
export async function readZip(bytes) {
  const data = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  const end = findEndOfCentralDirectory(view, data.length);

  if (view.getUint32(Math.max(0, end - 20), true) === SIG_ZIP64_LOCATOR) {
    throw new ZipError(
      'zip64',
      'This archive is ZIP64, which the studio does not read: its own exports are far below the ' +
        '4 GiB boundary that needs it.',
    );
  }

  const count = view.getUint16(end + 10, true);
  let cursor = view.getUint32(end + 16, true);
  const entries = [];
  const seen = new Set();

  for (let index = 0; index < count; index += 1) {
    if (cursor + 46 > data.length || view.getUint32(cursor, true) !== SIG_CENTRAL) {
      throw new ZipError('signature', 'The archive’s central directory is not where it claims to be.');
    }
    const flags = view.getUint16(cursor + 8, true);
    const method = view.getUint16(cursor + 10, true);
    const crc = view.getUint32(cursor + 16, true);
    const compressedSize = view.getUint32(cursor + 20, true);
    const size = view.getUint32(cursor + 24, true);
    const nameLength = view.getUint16(cursor + 28, true);
    const extraLength = view.getUint16(cursor + 30, true);
    const commentLength = view.getUint16(cursor + 32, true);
    const externalAttributes = view.getUint32(cursor + 38, true);
    const localOffset = view.getUint32(cursor + 42, true);
    const name = textDecoder.decode(data.subarray(cursor + 46, cursor + 46 + nameLength));
    cursor += 46 + nameLength + extraLength + commentLength;

    if (flags & FLAG_ENCRYPTED) {
      throw new ZipError('encrypted', `The archive entry ${name} is encrypted.`);
    }
    if (name.endsWith('/')) {
      continue; // a directory: the tree is built from file paths
    }
    // A Unix symlink: the high 16 bits of the external attributes are the mode, and the entry's body
    // is the link target. Following it would read a file outside the project (spec section 10). The
    // test is on the file-type bits alone: an archiver also writes the permission bits, so comparing
    // the whole 16 bits against `S_IFLNK` would miss every real entry.
    if (((externalAttributes >>> 16) & 0xf000) === S_IFLNK) {
      throw new ZipError(
        'symlink',
        `The archive entry ${name} is a symbolic link; a project is files, and a link can point ` +
          'outside it.',
      );
    }
    if (seen.has(name)) {
      throw new ZipError('duplicate', `The archive lists ${name} twice.`);
    }
    seen.add(name);

    if (localOffset + 30 > data.length || view.getUint32(localOffset, true) !== SIG_LOCAL) {
      throw new ZipError('signature', `The archive entry ${name} has no local header.`);
    }
    const localNameLength = view.getUint16(localOffset + 26, true);
    const localExtraLength = view.getUint16(localOffset + 28, true);
    const bodyStart = localOffset + 30 + localNameLength + localExtraLength;
    if (bodyStart + compressedSize > data.length) {
      throw new ZipError('truncated', `The archive entry ${name} runs past the end of the file.`);
    }
    const body = data.subarray(bodyStart, bodyStart + compressedSize);
    const decoded = await decodeEntry(body, method, size, name);
    if (crc32(decoded) !== crc) {
      throw new ZipError(
        'crc',
        `The archive entry ${name} does not match its checksum; the download is incomplete or ` +
          'corrupt.',
      );
    }
    entries.push({ path: name, bytes: decoded });
  }
  return entries;
}

/** Unpacks one entry's body by its method, or refuses the method with a named error. */
async function decodeEntry(body, method, size, name) {
  if (method === 0) {
    return body.slice();
  }
  if (method !== 8) {
    throw new ZipError(
      'method',
      `The archive entry ${name} uses compression method ${method}; the studio reads stored (0) ` +
        'and deflated (8) entries only.',
    );
  }
  // `deflate-raw` is exactly the DEFLATE stream a zip entry carries (RFC 1951, no zlib or gzip
  // wrapper). It is the same primitive `share.js` uses, so this adds no dependency and no polyfill.
  if (typeof DecompressionStream !== 'function') {
    throw new ZipError(
      'no-inflater',
      'This browser has no DecompressionStream, so a deflated zip cannot be read here. Exporting ' +
        'the project from a browser that has it, or opening the folder directly, still works.',
    );
  }
  try {
    const stream = new DecompressionStream('deflate-raw');
    const writer = stream.writable.getWriter();
    // The writer's promise is awaited below through the reader: writing and reading concurrently is
    // what keeps a large entry from needing the whole inflated buffer twice.
    writer.write(body).catch(() => {});
    writer.close().catch(() => {});
    const chunks = [];
    let total = 0;
    const reader = stream.readable.getReader();
    for (;;) {
      const { done, value } = await reader.read();
      if (done) {
        break;
      }
      chunks.push(value);
      total += value.length;
      if (total > size) {
        // The central directory declares the exact uncompressed size, so a stream that produces even one
        // byte more is not this entry. Stopping here is what bounds a zip bomb: without it, a 1 KB entry
        // that inflates to a gigabyte would be buffered in full before the CRC could disagree. The
        // reader is cancelled explicitly so the decompressor stops rather than filling its queue.
        await reader.cancel().catch(() => {});
        throw new ZipError(
          'crc',
          `The archive entry ${name} inflates past its declared size of ${size} bytes.`,
        );
      }
    }
    const out = new Uint8Array(total);
    let cursor = 0;
    for (const chunk of chunks) {
      out.set(chunk, cursor);
      cursor += chunk.length;
    }
    return out;
  } catch (error) {
    if (error instanceof ZipError) {
      throw error;
    }
    throw new ZipError('crc', `The archive entry ${name} could not be inflated: ${String(error)}`);
  }
}

/**
 * The offset of the end-of-central-directory record.
 *
 * Scans backwards because the record sits at the end and may be followed by a comment of up to 64 KiB
 * (APPNOTE 4.4.16). A file shorter than a record is `truncated`, not a signature mismatch, because
 * "the download was cut off at 900 bytes" is the useful sentence.
 */
function findEndOfCentralDirectory(view, length) {
  if (length < 22) {
    throw new ZipError('truncated', `The file is ${length} bytes, shorter than a zip can be.`);
  }
  const lowest = Math.max(0, length - 22 - 0xffff);
  for (let offset = length - 22; offset >= lowest; offset -= 1) {
    if (view.getUint32(offset, true) === SIG_EOCD) {
      return offset;
    }
  }
  throw new ZipError('signature', 'This file is not a zip: it has no end-of-central-directory record.');
}
