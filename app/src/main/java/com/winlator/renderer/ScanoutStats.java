package com.winlator.renderer;

import android.util.Log;

import java.util.concurrent.atomic.AtomicInteger;

/**
 * Lightweight, throttled instrumentation for the native DRI3 -> Present -> scanout
 * fast path (the "Option 5" / native-mode direct-scanout route).
 *
 * Host-side only; no guest changes. It counts which branch each of the three stages
 * takes per frame so we can see, on a real run, WHY native mode composites instead of
 * scanning out (and therefore why native latency was measured high). One summary line
 * is emitted to logcat ~every 2s under tag DAC_SCANOUT.
 *
 *   adb logcat -s DAC_SCANOUT
 *
 * Reading it:
 *   DRI3   = how the wrapper delivers swapchain pixmaps (ahb = zero-copy AHB modifiers==1255,
 *            shm = SysV SHM fd, oth = neither). If ahb==0 the directScanout gate can never arm.
 *   Present= which presentPixmap branch fired (flip = native directScanout FLIP = the win;
 *            copy = onUpdateWindowContentDirect; comp = copyArea compositor fallback).
 *   Render = which native call fired in onUpdateWindowContent (SCANOUT = nativeScanoutSetBuffer
 *            = the Option-5 overlay path; compAHB = nativeUpdateWindowContentAHB composite;
 *            shm = nativeUpdateWindowContent).
 *   gates  = the last-seen decision inputs in onUpdateWindowContent.
 */
public final class ScanoutStats {
    private ScanoutStats() {}

    public static final String TAG = "DAC_SCANOUT";
    public static volatile boolean enabled = true;

    // A — DRI3 pixmap delivery (DRI3Extension.pixmapFromBuffers)
    public static final AtomicInteger dri3Ahb   = new AtomicInteger(); // modifiers==1255 (AHardwareBuffer)
    public static final AtomicInteger dri3Shm   = new AtomicInteger(); // modifiers==1274 (SysV SHM fd)
    public static final AtomicInteger dri3Other = new AtomicInteger(); // anything else

    // B — Present branch (PresentExtension.presentPixmap)
    public static final AtomicInteger presentFlip       = new AtomicInteger(); // native + directScanout -> FLIP
    public static final AtomicInteger presentCopyDirect = new AtomicInteger(); // onUpdateWindowContentDirect
    public static final AtomicInteger presentComposite  = new AtomicInteger(); // copyArea fallback

    // C — Renderer branch (VulkanRenderer.onUpdateWindowContent)
    public static final AtomicInteger rScanout      = new AtomicInteger(); // nativeScanoutSetBuffer (the win)
    public static final AtomicInteger rCompositeAhb = new AtomicInteger(); // nativeUpdateWindowContentAHB
    public static final AtomicInteger rShm          = new AtomicInteger(); // nativeUpdateWindowContent (SHM/virtual)

    // last-seen gate state at the renderer decision point
    public static volatile boolean gDirectScanout;
    public static volatile boolean gScanoutActive;
    public static volatile boolean gEffectsCompositor;

    private static volatile long lastDumpNs = 0L;
    private static final long PERIOD_NS = 2_000_000_000L;

    /** Call once per presented frame (cheap; throttled to one logcat line / ~2s). */
    public static void maybeDump() {
        if (!enabled) return;
        final long now = System.nanoTime();
        if (now - lastDumpNs < PERIOD_NS) return;
        synchronized (ScanoutStats.class) {
            if (now - lastDumpNs < PERIOD_NS) return;
            lastDumpNs = now;
        }
        final int a = dri3Ahb.getAndSet(0),   s = dri3Shm.getAndSet(0),   o = dri3Other.getAndSet(0);
        final int pf = presentFlip.getAndSet(0), pc = presentCopyDirect.getAndSet(0), pk = presentComposite.getAndSet(0);
        final int rs = rScanout.getAndSet(0),  rc = rCompositeAhb.getAndSet(0), rh = rShm.getAndSet(0);
        Log.i(TAG, "2s: DRI3[ahb=" + a + " shm=" + s + " oth=" + o + "]"
                + "  Present[flip=" + pf + " copy=" + pc + " comp=" + pk + "]"
                + "  Render[SCANOUT=" + rs + " compAHB=" + rc + " shm=" + rh + "]"
                + "  gates[directScanout=" + gDirectScanout
                + " scanoutActive=" + gScanoutActive
                + " effectsCompositor=" + gEffectsCompositor + "]");
    }
}
