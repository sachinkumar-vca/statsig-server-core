package com.statsig;

import com.alibaba.fastjson2.JSON;
import java.nio.charset.StandardCharsets;
import java.util.Map;

/**
 * [S2SDK-165] Binary encoder for the user-construction JNI payload.
 *
 * <p>User construction is the expensive JNI crossing for this SDK: evaluations afterwards only pass
 * the returned native handle. The previous implementation serialized the map fields ({@code
 * customIDs}, {@code custom}, {@code privateAttributes}) to JSON <em>text</em> with fastjson2,
 * passed ten separate strings through JNI (each paying a UTF-16 to UTF-8 conversion and copy), and
 * the native side immediately re-parsed the JSON back into maps. Every populated field paid
 * serialize + copy + parse, which is why construction cost scaled with the number of non-null
 * fields.
 *
 * <p>This encoder writes the whole user into a single {@code byte[]}:
 *
 * <ul>
 *   <li>one JNI argument and one array copy instead of ten string conversions;
 *   <li>strings are encoded straight to UTF-8 here, so the boundary does no charset work;
 *   <li>flat values (string/bool/integer/floating-point) cross as tagged fixed-width bytes and
 *       become native values directly -- no JSON stringify/parse round trip;
 *   <li>only genuinely nested values (lists, maps, POJOs inside {@code custom}) fall back to a
 *       JSON-encoded leaf, which is the uncommon case for user objects.
 * </ul>
 *
 * <p>The format is decoded by {@code user_payload.rs} in the native library; the two ship in the
 * same artifact, and a leading version byte makes any mismatch fail loudly instead of decoding
 * garbage. Format summary (integers big-endian):
 *
 * <pre>
 * payload    := version:u8 scalar*7 stringMap(customIDs) typedMap(custom) typedMap(privateAttrs)
 * scalar     := len:i32 utf8            // len == -1 means null
 *               // order: userID, email, ip, userAgent, country, locale, appVersion
 * stringMap  := count:i32 (key value)*  // count == -1 means the map itself was null
 * typedMap   := count:i32 (key tagged)* // count == -1 means the map itself was null
 * tagged     := tag:u8 payload          // 1=string 2=true 3=false 4=i64 5=f64 6=json
 * </pre>
 *
 * <p>Null-valued entries are skipped entirely, matching the previous behavior where fastjson2
 * omitted null map values from the serialized JSON.
 */
final class StatsigUserPayload {
  static final byte VERSION = 1;

  private static final byte TAG_STRING = 1;
  private static final byte TAG_BOOL_TRUE = 2;
  private static final byte TAG_BOOL_FALSE = 3;
  private static final byte TAG_I64 = 4;
  private static final byte TAG_F64 = 5;
  private static final byte TAG_JSON = 6;

  private StatsigUserPayload() {}

  static byte[] encode(
      String userID,
      Map<String, ?> customIDs,
      String email,
      String ip,
      String userAgent,
      String country,
      String locale,
      String appVersion,
      Map<String, ?> custom,
      Map<String, ?> privateAttributes) {
    Writer w = new Writer();
    w.writeByte(VERSION);

    w.writeString(userID);
    w.writeString(email);
    w.writeString(ip);
    w.writeString(userAgent);
    w.writeString(country);
    w.writeString(locale);
    w.writeString(appVersion);

    writeStringMap(w, customIDs);
    writeTypedMap(w, custom);
    // privateAttributes is declared Map<String, String> on the public API, but
    // callers routinely stuff non-String values in via type erasure (e.g. maps
    // deserialized from JSON), and the old fastjson2 path serialized those
    // runtime types faithfully. Encode by runtime type -- assuming String here
    // would throw ClassCastException on such maps.
    writeTypedMap(w, privateAttributes);

    return w.toByteArray();
  }

  // All three map writers encode in a single pass, reserving the count slot up
  // front and backpatching it afterwards. Besides skipping a full extra
  // iteration on the hot path, this keeps the written count consistent with the
  // entries actually encoded even if the caller hands us a concurrently-mutated
  // map (e.g. a ConcurrentHashMap with a weakly-consistent iterator) -- a
  // count/entries mismatch would make the strict native decoder reject the
  // whole payload.
  private static void writeStringMap(Writer w, Map<String, ?> map) {
    if (map == null) {
      w.writeInt(-1);
      return;
    }
    int countPos = w.reserveInt();
    int count = 0;
    for (Map.Entry<String, ?> e : map.entrySet()) {
      // The instanceof check doubles as the null check and guards against
      // type-erased maps holding non-String values (see writeTypedMap): the old
      // JSON path made the native side drop the whole customIDs map on a
      // non-string value, so skipping the entry is not a behavior regression.
      if (e.getKey() == null || !(e.getValue() instanceof String)) {
        continue;
      }
      w.writeString(e.getKey());
      w.writeString((String) e.getValue());
      count++;
    }
    w.patchInt(countPos, count);
  }

  // The wildcard value type matters: generics are erased, so callers can (and
  // do, e.g. via unchecked casts of JSON-deserialized maps) hand a map declared
  // Map<String, String> that actually holds Booleans or Integers. Iterating with
  // a String-typed entry would make the compiler insert a checkcast and throw
  // ClassCastException; dispatching on the runtime type in writeTaggedValue
  // preserves the old fastjson2 behavior, which serialized whatever was there.
  private static void writeTypedMap(Writer w, Map<String, ?> map) {
    if (map == null) {
      w.writeInt(-1);
      return;
    }
    int countPos = w.reserveInt();
    int count = 0;
    for (Map.Entry<String, ?> e : map.entrySet()) {
      if (e.getKey() == null || e.getValue() == null) {
        continue;
      }
      w.writeString(e.getKey());
      writeTaggedValue(w, e.getValue());
      count++;
    }
    w.patchInt(countPos, count);
  }

  private static void writeTaggedValue(Writer w, Object value) {
    if (value instanceof String) {
      w.writeByte(TAG_STRING);
      w.writeString((String) value);
    } else if (value instanceof Boolean) {
      w.writeByte(((Boolean) value) ? TAG_BOOL_TRUE : TAG_BOOL_FALSE);
    } else if (value instanceof Integer
        || value instanceof Long
        || value instanceof Short
        || value instanceof Byte) {
      w.writeByte(TAG_I64);
      w.writeLong(((Number) value).longValue());
    } else if (value instanceof Double) {
      w.writeByte(TAG_F64);
      w.writeDouble((Double) value);
    } else if (value instanceof Float) {
      // Route through the decimal string, not a raw float->double cast: the old
      // JSON path serialized 0.1f as "0.1" and parsed it as the double 0.1,
      // whereas (double) 0.1f is 0.10000000149...; this keeps values identical.
      w.writeByte(TAG_F64);
      w.writeDouble(Double.parseDouble(Float.toString((Float) value)));
    } else {
      // Nested / arbitrary values (List, Map, arrays, BigDecimal, POJOs): fall
      // back to a JSON leaf. This is the only remaining JSON round trip on the
      // construction path, and it matches the previous semantics exactly since
      // the whole map used to travel as fastjson2 JSON.
      w.writeByte(TAG_JSON);
      w.writeString(JSON.toJSONString(value));
    }
  }

  /**
   * Minimal growable big-endian writer. Deliberately not ByteArrayOutputStream / DataOutputStream:
   * those synchronize on every write, and this runs on the per-request hot path.
   */
  private static final class Writer {
    // Slightly under Integer.MAX_VALUE, matching the JDK's soft max array length;
    // requests beyond this cannot be satisfied by any byte[].
    private static final int MAX_CAPACITY = Integer.MAX_VALUE - 8;

    private byte[] buf = new byte[256];
    private int pos = 0;

    private void ensure(int extra) {
      // long arithmetic: pos + extra could overflow int for pathologically large
      // inputs (e.g. a >1GB string field), which would skip the grow and fail
      // later with an opaque ArrayIndexOutOfBoundsException.
      long needed = (long) pos + extra;
      if (needed <= buf.length) {
        return;
      }
      if (needed > MAX_CAPACITY) {
        throw new IllegalStateException("StatsigUser payload exceeds maximum size: " + needed);
      }
      int newLen = (int) Math.min(Math.max((long) buf.length * 2, needed), MAX_CAPACITY);
      byte[] next = new byte[newLen];
      System.arraycopy(buf, 0, next, 0, pos);
      buf = next;
    }

    void writeByte(byte value) {
      ensure(1);
      buf[pos++] = value;
    }

    void writeInt(int value) {
      ensure(4);
      buf[pos++] = (byte) (value >>> 24);
      buf[pos++] = (byte) (value >>> 16);
      buf[pos++] = (byte) (value >>> 8);
      buf[pos++] = (byte) value;
    }

    /** Reserves 4 bytes for an int written later via {@link #patchInt}. */
    int reserveInt() {
      int at = pos;
      writeInt(0);
      return at;
    }

    void patchInt(int at, int value) {
      buf[at] = (byte) (value >>> 24);
      buf[at + 1] = (byte) (value >>> 16);
      buf[at + 2] = (byte) (value >>> 8);
      buf[at + 3] = (byte) value;
    }

    void writeLong(long value) {
      ensure(8);
      for (int shift = 56; shift >= 0; shift -= 8) {
        buf[pos++] = (byte) (value >>> shift);
      }
    }

    void writeDouble(double value) {
      writeLong(Double.doubleToLongBits(value));
    }

    /** Length-prefixed UTF-8; -1 length encodes null. */
    void writeString(String value) {
      if (value == null) {
        writeInt(-1);
        return;
      }
      byte[] utf8 = value.getBytes(StandardCharsets.UTF_8);
      writeInt(utf8.length);
      ensure(utf8.length);
      System.arraycopy(utf8, 0, buf, pos, utf8.length);
      pos += utf8.length;
    }

    byte[] toByteArray() {
      byte[] out = new byte[pos];
      System.arraycopy(buf, 0, out, 0, pos);
      return out;
    }
  }
}
