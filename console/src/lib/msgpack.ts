// A minimal MsgPack ENCODER, covering exactly the shapes this console sends.
//
// Why hand-rolled rather than a dependency: the only body that needs msgpack is
// the user upsert, a four-field map of strings, booleans and string arrays.
// Every byte here ends up embedded in a database binary as .rodata, and the
// engine itself links only gRPC, protobuf and msgpack for the same reason. A
// full library would be several kilobytes to encode one map.
//
// Deliberately encode-only. Nothing the engine returns is msgpack: every
// response is JSON or a raw value, so there is no decoder to get wrong.
//
// Supported types are exactly str, bool, array<T> and map<string, T>. Anything
// else throws rather than silently encoding something the C++ side will reject
// with a 400 that is hard to trace back to here.

/** Values this encoder accepts. */
export type MsgPackValue =
  | string
  | boolean
  | readonly MsgPackValue[]
  | { readonly [key: string]: MsgPackValue };

class ByteSink {
  private parts: number[] = [];

  byte(value: number): void {
    this.parts.push(value & 0xff);
  }

  bytes(values: Uint8Array): void {
    for (const value of values) this.parts.push(value);
  }

  /** Big-endian, which is what MsgPack specifies for every multi-byte field. */
  uint16(value: number): void {
    this.byte(value >>> 8);
    this.byte(value);
  }

  uint32(value: number): void {
    this.byte(value >>> 24);
    this.byte(value >>> 16);
    this.byte(value >>> 8);
    this.byte(value);
  }

  toUint8Array(): Uint8Array {
    return new Uint8Array(this.parts);
  }
}

// A password may hold any character, so strings are measured in UTF-8 BYTES,
// not in JS code units. Using .length here would under-count any non-ASCII
// password and produce a truncated, unparseable body.
const UTF8 = new TextEncoder();

function encodeString(sink: ByteSink, value: string): void {
  const bytes = UTF8.encode(value);
  if (bytes.length < 32) {
    sink.byte(0xa0 | bytes.length); // fixstr
  } else if (bytes.length < 0x100) {
    sink.byte(0xd9); // str 8
    sink.byte(bytes.length);
  } else if (bytes.length < 0x10000) {
    sink.byte(0xda); // str 16
    sink.uint16(bytes.length);
  } else {
    sink.byte(0xdb); // str 32
    sink.uint32(bytes.length);
  }
  sink.bytes(bytes);
}

function encodeValue(sink: ByteSink, value: MsgPackValue): void {
  if (typeof value === 'string') {
    encodeString(sink, value);
    return;
  }
  if (typeof value === 'boolean') {
    sink.byte(value ? 0xc3 : 0xc2);
    return;
  }
  if (Array.isArray(value)) {
    if (value.length < 16) {
      sink.byte(0x90 | value.length); // fixarray
    } else if (value.length < 0x10000) {
      sink.byte(0xdc); // array 16
      sink.uint16(value.length);
    } else {
      sink.byte(0xdd); // array 32
      sink.uint32(value.length);
    }
    for (const entry of value) encodeValue(sink, entry);
    return;
  }
  if (typeof value === 'object' && value !== null) {
    // Array.isArray's guard is `arg is any[]`, which does not subtract
    // `readonly MsgPackValue[]` from the union, so TypeScript still thinks this
    // could be an array even though the branch above returned for one. The
    // assertion records what the control flow already guarantees.
    const record = value as { readonly [key: string]: MsgPackValue };
    const keys = Object.keys(record);
    if (keys.length < 16) {
      sink.byte(0x80 | keys.length); // fixmap
    } else if (keys.length < 0x10000) {
      sink.byte(0xde); // map 16
      sink.uint16(keys.length);
    } else {
      sink.byte(0xdf); // map 32
      sink.uint32(keys.length);
    }
    for (const key of keys) {
      encodeString(sink, key);
      // Non-null assertion is sound: `key` came from Object.keys(record).
      encodeValue(sink, record[key]!);
    }
    return;
  }
  throw new TypeError(`cannot msgpack-encode ${typeof value}`);
}

/**
 * Encode `value` as MsgPack.
 *
 * The engine's `KVCommand` and `UserUpsertRequest` both use MSGPACK_DEFINE_MAP,
 * so a map is what they expect -- never the array variant.
 */
export function encodeMsgPack(value: MsgPackValue): Uint8Array {
  const sink = new ByteSink();
  encodeValue(sink, value);
  return sink.toUint8Array();
}
