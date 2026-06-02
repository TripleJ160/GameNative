package com.winlator.xenvironment.components;

import android.os.Build;
import android.util.Log;

import com.winlator.renderer.AHardwareBufferPool;
import com.winlator.renderer.VulkanRenderer;
import com.winlator.xenvironment.EnvironmentComponent;

import java.util.function.Supplier;

/**
 * Owns the {@link AHardwareBufferPool} and coordinates the Direct Android
 * Compositing (DAC) lifecycle. Ported from Winlator-Ludashi-Plus and adapted to
 * GameNative.
 *
 * <p>GameNative adaptation: the VulkanRenderer is created lazily (on SurfaceView
 * ready) and is NOT an XEnvironment component, so it may not exist when this
 * component starts. We therefore take a {@link Supplier} and resolve the
 * renderer on demand. The pool is created at {@link #start()} (no renderer
 * needed); the renderer is only required when Wine's WSI connects — by which
 * time the surface (and renderer) is ready — to start the native present
 * receiver. The Winlator-side renderer.setDirectCompositor/setAHBPool/
 * detach/reattach calls are intentionally dropped: GameNative's recv-thread
 * (libdac.so) drives scanout directly via dlsym and receives the pool buffers
 * explicitly, so those renderer hooks are unused here.
 */
public class DirectCompositorComponent extends EnvironmentComponent {

    private static final String LOG_TAG = "DirectCompositor";

    private final Supplier<VulkanRenderer> rendererSupplier;
    private final int poolSize;
    private final int screenWidth;
    private final int screenHeight;

    private AHardwareBufferPool pool;
    private boolean active = false;

    public DirectCompositorComponent(Supplier<VulkanRenderer> rendererSupplier, int poolSize,
                                     int screenWidth, int screenHeight) {
        this.rendererSupplier = rendererSupplier;
        this.poolSize = poolSize;
        this.screenWidth = screenWidth;
        this.screenHeight = screenHeight;
    }

    /** Convenience constructor with the default pool size of 4. */
    public DirectCompositorComponent(Supplier<VulkanRenderer> rendererSupplier,
                                     int screenWidth, int screenHeight) {
        this(rendererSupplier, 4, screenWidth, screenHeight);
    }

    @Override
    public void start() {
        if (Build.VERSION.SDK_INT < 26) {
            Log.w(LOG_TAG, "start: API level " + Build.VERSION.SDK_INT
                    + " < 26, AHardwareBuffer not available — skipping activation");
            return;
        }

        pool = new AHardwareBufferPool(screenWidth, screenHeight, poolSize);
        if (!pool.init()) {
            Log.e(LOG_TAG, "start: AHardwareBufferPool.init() failed, DAC inactive");
            pool = null;
            return;
        }
        active = true;
        Log.i(LOG_TAG, "start: pool ready (pool=" + poolSize + ", " + screenWidth + "x" + screenHeight
                + "), waiting for Wine WSI to connect to the AHB socket");
    }

    @Override
    public void stop() {
        active = false;
        if (pool != null) {
            pool.destroy();
            pool = null;
        }
        Log.i(LOG_TAG, "stop: direct compositing deactivated");
    }

    /** Returns the AHardwareBufferPool, or {@code null} if not active. */
    public AHardwareBufferPool getPool() {
        return pool;
    }

    /** Resolves the VulkanRenderer on demand (null until the surface is ready). */
    public VulkanRenderer getRenderer() {
        return rendererSupplier != null ? rendererSupplier.get() : null;
    }

    /** Returns {@code true} if the direct compositing path is active. */
    public boolean isActive() {
        return active;
    }
}
