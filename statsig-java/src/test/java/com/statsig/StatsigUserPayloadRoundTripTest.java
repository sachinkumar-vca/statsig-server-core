package com.statsig;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.alibaba.fastjson2.JSONObject;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ExecutionException;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

/**
 * [S2SDK-165] End-to-end verification of the binary user-construction payload.
 *
 * <p>The user here crosses the real JNI boundary: it is encoded by StatsigUserPayload, decoded by
 * user_payload.rs, and materialized as a native StatsigUser. getClientInitializeResponse then
 * serializes that native user back out as JSON, so asserting on the response's "user" object
 * verifies the entire encode/decode round trip for every field and value type -- including the
 * typed fast paths (string/bool/long/double), the JSON fallback for nested values, and UTF-8
 * handling for non-ASCII strings.
 */
public class StatsigUserPayloadRoundTripTest {
  private Statsig statsigServer;

  @BeforeEach
  public void setUp() throws ExecutionException, InterruptedException {
    statsigServer = new Statsig("secret-key");
    statsigServer.initialize().get();
  }

  @AfterEach
  public void tearDown() throws ExecutionException, InterruptedException {
    statsigServer.shutdown().get();
  }

  @Test
  public void testAllFieldsSurviveTheBinaryPayload() {
    Map<String, String> customIDs = new HashMap<>();
    customIDs.put("companyID", "c-42");
    customIDs.put("stableID", "stable-7");

    List<String> tags = new ArrayList<>();
    tags.add("alpha");
    tags.add("beta");

    Map<String, Object> nested = new HashMap<>();
    nested.put("inner", "value");

    Map<String, Object> custom = new HashMap<>();
    custom.put("plan", "pro"); // TAG_STRING
    custom.put("beta_opt_in", true); // TAG_BOOL_TRUE
    custom.put("suspended", false); // TAG_BOOL_FALSE
    custom.put("seats", 42); // TAG_I64 (Integer)
    custom.put("bytes_used", 9_876_543_210L); // TAG_I64 (Long)
    custom.put("score", 1.5d); // TAG_F64 (Double)
    custom.put("ratio", 0.25f); // TAG_F64 (Float, via decimal string)
    custom.put("tags", tags); // TAG_JSON fallback (List)
    custom.put("nested", nested); // TAG_JSON fallback (Map)
    custom.put("emoji", "héllo \uD83D\uDE80"); // UTF-8 correctness
    custom.put("skipped", null); // null entries are dropped (old behavior)

    Map<String, String> privateAttributes = new HashMap<>();
    privateAttributes.put("secret", "do-not-log");

    StatsigUser user =
        new StatsigUser.Builder()
            .setUserID("user-123")
            .setCustomIDs(customIDs)
            .setEmail("test@example.com")
            .setIp("10.0.0.1")
            .setUserAgent("Mozilla/5.0 (Test)")
            .setCountry("US")
            .setLocale("en_US")
            .setAppVersion("5.2.1")
            .setCustom(custom)
            .setPrivateAttributes(privateAttributes)
            .build();

    String res = statsigServer.getClientInitializeResponse(user);
    assertNotNull(res);

    JSONObject echoed = JSONObject.parseObject(res).getJSONObject("user");
    assertNotNull(echoed, "GCIR response should echo the native-side user");

    assertEquals("user-123", echoed.getString("userID"));
    assertEquals("test@example.com", echoed.getString("email"));
    assertEquals("10.0.0.1", echoed.getString("ip"));
    assertEquals("Mozilla/5.0 (Test)", echoed.getString("userAgent"));
    assertEquals("US", echoed.getString("country"));
    assertEquals("en_US", echoed.getString("locale"));
    assertEquals("5.2.1", echoed.getString("appVersion"));

    JSONObject echoedCustomIDs = echoed.getJSONObject("customIDs");
    assertEquals("c-42", echoedCustomIDs.getString("companyID"));
    assertEquals("stable-7", echoedCustomIDs.getString("stableID"));

    JSONObject echoedCustom = echoed.getJSONObject("custom");
    assertEquals("pro", echoedCustom.getString("plan"));
    assertEquals(Boolean.TRUE, echoedCustom.getBoolean("beta_opt_in"));
    assertEquals(Boolean.FALSE, echoedCustom.getBoolean("suspended"));
    assertEquals(42L, (long) echoedCustom.getLong("seats"));
    assertEquals(9_876_543_210L, (long) echoedCustom.getLong("bytes_used"));
    assertEquals(1.5d, echoedCustom.getDouble("score"), 0.0);
    // Float crosses via its decimal string, so 0.25f arrives as exactly 0.25.
    assertEquals(0.25d, echoedCustom.getDouble("ratio"), 0.0);
    assertEquals("héllo \uD83D\uDE80", echoedCustom.getString("emoji"));
    assertEquals("alpha", echoedCustom.getJSONArray("tags").getString(0));
    assertEquals("beta", echoedCustom.getJSONArray("tags").getString(1));
    assertEquals("value", echoedCustom.getJSONObject("nested").getString("inner"));
    assertFalse(echoedCustom.containsKey("skipped"), "null-valued entries are dropped");

    // The native side never serializes privateAttributes back out.
    assertFalse(res.contains("do-not-log"), "private attributes must not leak");
  }

  @Test
  public void testUserWithOnlyCustomIDs() {
    Map<String, String> customIDs = new HashMap<>();
    customIDs.put("companyID", "c-1");

    StatsigUser user = new StatsigUser.Builder().setCustomIDs(customIDs).build();
    assertTrue(user.getRef() != 0, "construction must succeed without a userID");

    String res = statsigServer.getClientInitializeResponse(user);
    JSONObject echoed = JSONObject.parseObject(res).getJSONObject("user");
    assertEquals("c-1", echoed.getJSONObject("customIDs").getString("companyID"));
  }

  @Test
  @SuppressWarnings("unchecked")
  public void testErasedTypeMapsWithNonStringValues() {
    // Callers routinely funnel JSON-deserialized maps into the builder via
    // unchecked casts (e.g. Gson's context.deserialize(..., Map.class)), so a
    // map declared Map<String, String> can hold Booleans and Numbers at
    // runtime. The old fastjson2 path serialized those faithfully; the encoder
    // must not throw ClassCastException on them. This mirrors the kong bridge
    // setup that caught exactly that regression.
    Map<String, Object> erased = new HashMap<>();
    erased.put("isAdmin", true);
    erased.put("passWord", 1017);

    StatsigUser user =
        new StatsigUser.Builder()
            .setUserID("erased-types")
            .setPrivateAttributes((Map<String, String>) (Map<?, ?>) erased)
            .build();
    assertTrue(user.getRef() != 0, "construction must survive erased non-String values");

    String res = statsigServer.getClientInitializeResponse(user);
    JSONObject echoed = JSONObject.parseObject(res).getJSONObject("user");
    assertEquals("erased-types", echoed.getString("userID"));
    assertFalse(res.contains("isAdmin"), "private attributes must not leak");
  }

  @Test
  public void testEmptyAndNullFields() {
    StatsigUser user = new StatsigUser.Builder().setUserID("").setEmail(null).build();
    assertTrue(user.getRef() != 0, "empty userID must still construct");

    String res = statsigServer.getClientInitializeResponse(user);
    JSONObject echoed = JSONObject.parseObject(res).getJSONObject("user");
    assertEquals("", echoed.getString("userID"));
    assertFalse(echoed.containsKey("email"), "null fields stay absent");
  }
}
