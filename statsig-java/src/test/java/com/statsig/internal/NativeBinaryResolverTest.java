package com.statsig.internal;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.io.InterruptedIOException;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Test;

public class NativeBinaryResolverTest {

  @AfterEach
  public void tearDown() {
    System.clearProperty(NativeBinaryResolver.STATSIG_SKIP_MUSL_DETECTION_PROPERTY);
  }

  @Test
  public void testParseBooleanOverrideTrueValues() {
    assertEquals(Boolean.TRUE, NativeBinaryResolver.parseBooleanOverride("true"));
    assertEquals(Boolean.TRUE, NativeBinaryResolver.parseBooleanOverride("TRUE"));
    assertEquals(Boolean.TRUE, NativeBinaryResolver.parseBooleanOverride("1"));
    assertEquals(Boolean.TRUE, NativeBinaryResolver.parseBooleanOverride(" true "));
  }

  @Test
  public void testParseBooleanOverrideFalseValues() {
    assertEquals(Boolean.FALSE, NativeBinaryResolver.parseBooleanOverride("false"));
    assertEquals(Boolean.FALSE, NativeBinaryResolver.parseBooleanOverride("FALSE"));
    assertEquals(Boolean.FALSE, NativeBinaryResolver.parseBooleanOverride("0"));
  }

  @Test
  public void testParseBooleanOverrideUnknownValues() {
    assertNull(NativeBinaryResolver.parseBooleanOverride(null));
    assertNull(NativeBinaryResolver.parseBooleanOverride(""));
    assertNull(NativeBinaryResolver.parseBooleanOverride("   "));
    assertNull(NativeBinaryResolver.parseBooleanOverride("yes"));
  }

  @Test
  public void testResolveSkipMuslDetectionPropertyWinsOverEnv() {
    assertEquals(Boolean.TRUE, NativeBinaryResolver.resolveSkipMuslDetection("true", "false"));
    assertEquals(Boolean.FALSE, NativeBinaryResolver.resolveSkipMuslDetection("false", "true"));
  }

  @Test
  public void testResolveSkipMuslDetectionFallsBackToEnv() {
    assertEquals(Boolean.TRUE, NativeBinaryResolver.resolveSkipMuslDetection(null, "true"));
    assertEquals(Boolean.TRUE, NativeBinaryResolver.resolveSkipMuslDetection("not-a-bool", "true"));
    assertEquals(Boolean.FALSE, NativeBinaryResolver.resolveSkipMuslDetection(null, "false"));
  }

  @Test
  public void testResolveSkipMuslDetectionUnsetOrInvalidEverywhere() {
    assertNull(NativeBinaryResolver.resolveSkipMuslDetection(null, null));
    assertNull(NativeBinaryResolver.resolveSkipMuslDetection("not-a-bool", "also-not-a-bool"));
  }

  @Test
  public void testReadShouldSkipMuslDetectionReadsSystemProperty() {
    System.setProperty(NativeBinaryResolver.STATSIG_SKIP_MUSL_DETECTION_PROPERTY, "true");
    assertEquals(Boolean.TRUE, NativeBinaryResolver.readShouldSkipMuslDetection());

    System.setProperty(NativeBinaryResolver.STATSIG_SKIP_MUSL_DETECTION_PROPERTY, "false");
    assertEquals(Boolean.FALSE, NativeBinaryResolver.readShouldSkipMuslDetection());
  }

  @Test
  public void testPreserveInterruptRestoresFlagForInterruptSignals() {
    assertTrue(interruptFlagAfterPreserve(new InterruptedException()));
    assertTrue(interruptFlagAfterPreserve(new InterruptedIOException()));
  }

  @Test
  public void testPreserveInterruptLeavesFlagClearForOtherFailures() {
    assertFalse(interruptFlagAfterPreserve(new IOException("no ldd")));
    assertFalse(interruptFlagAfterPreserve(new ClassCastException("instrumented exec")));
  }

  /** Reads and clears the flag so a restored interrupt doesn't leak into the next test. */
  private static boolean interruptFlagAfterPreserve(Throwable t) {
    NativeBinaryResolver.preserveInterrupt(t);
    return Thread.interrupted();
  }
}
