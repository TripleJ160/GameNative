package app.gamenative.utils

import android.content.Context
import com.winlator.container.Container
import com.winlator.core.FileUtils
import com.winlator.core.envvars.EnvVars
import com.winlator.xconnector.UnixSocketConfig
import com.winlator.xenvironment.ImageFs
import java.io.File
import timber.log.Timber
import kotlin.jvm.JvmStatic

/**
 * Manages the Direct Android Compositing (DAC) Vulkan implicit layer
 * (`libahb_layer.so` / `VK_LAYER_WINLATOR_ahb_direct`) for the container.
 *
 * Ported from Winlator-Ludashi-Plus. The layer runs inside the guest Wine
 * process on the **wrapper / Turnip in-guest Vulkan ICD path** (it has nothing
 * to intercept on the Vortek socket-proxy path). It hooks
 * vkCreateSwapchainKHR / vkAcquireNextImageKHR / vkQueuePresentKHR, renders
 * DXVK frames into AHardwareBuffers from a shared pool, and ships them over a
 * Unix socket to the Android app, which scans them out via SurfaceControl —
 * bypassing the X11 compositor.
 *
 * Deployment mirrors {@link LsfgVkManager}: install the .so + manifest into the
 * container's implicit_layer.d, append it to VK_LAYER_PATH, and gate enablement
 * via env vars.
 *
 * Graphics Pipeline (container extra "graphicsPipeline"):
 *   - "quality"     → layer ON, WINLATOR_AHB_DIRECT_RENDER=1 (direct-render)
 *   - "performance" → layer ON, WINLATOR_AHB_DIRECT_RENDER=0 (trojan-blit)
 *   - "native"      → layer OFF (DISABLE_AHB_LAYER=1, manifest removed) [default]
 */
object DacLayerManager {
    private const val TAG = "DacLayerManager"

    // Container extra key + values
    const val EXTRA_GRAPHICS_PIPELINE = "graphicsPipeline"
    const val PIPELINE_QUALITY = "quality"
    const val PIPELINE_PERFORMANCE = "performance"
    const val PIPELINE_NATIVE = "native"
    // EXPERIMENTAL: same guest env as "native" (AHB layer OFF, real X11/DRI3 WSI),
    // but the host renderer activates the dormant DRI3-AHB -> Present-FLIP ->
    // nativeScanoutSetBuffer direct-scanout path (renderer.setNativeMode(true)).
    // Lets us A/B the directScanout latency against classic composite "native".
    // Requires DRI3 on (the path consumes AHB DRI3 pixmaps, modifiers==1255).
    const val PIPELINE_NATIVE_SCANOUT = "native-scanout"
    // TESTING DEFAULT: Quality so that a wrapper/Turnip-driver container auto-arms
    // DAC without the (not-yet-built) Graphics Pipeline UI. Vortek/virgl containers
    // are unaffected (isSupported() gates on the wrapper driver). Revert to
    // PIPELINE_NATIVE once the UI lands so DAC is explicit opt-in.
    // DAC is explicit opt-in: containers default to NATIVE (DAC off) and the user
    // selects Quality/Performance in the container's Graphics tab (GraphicsTab.kt).
    const val DEFAULT_PIPELINE = PIPELINE_NATIVE

    // Paths inside the container's HOME (relative to rootDir)
    private const val LIB_RELATIVE_DIR = ".local/lib"
    private const val LAYER_RELATIVE_DIR = ".local/share/vulkan/implicit_layer.d"
    private const val LIB_FILENAME = "libahb_layer.so"
    private const val MANIFEST_FILENAME = "ahb_layer.json"
    private const val VERSION_FILENAME = ".ahb_layer_runtime_version"
    private const val MANIFEST_LIBRARY_PATH = "../../../lib/$LIB_FILENAME"

    // Env vars consumed by the AHB layer (must match dlls/vulkan_layer/ahb_layer.c)
    private const val ENV_ENABLE = "ENABLE_AHB_LAYER"
    private const val ENV_DISABLE = "DISABLE_AHB_LAYER"
    private const val ENV_INSTANCE_LAYERS = "VK_INSTANCE_LAYERS"
    private const val ENV_AHB_SERVER = "ANDROID_AHB_SERVER"
    private const val ENV_DIRECT_RENDER = "WINLATOR_AHB_DIRECT_RENDER"
    private const val LAYER_NAME = "VK_LAYER_WINLATOR_ahb_direct"

    // ── Forward-compat WSI extension gating (Option 1, SURGICAL) ────────────
    // DAC bypasses the real vkQueuePresentKHR. The layer ALREADY mirrors DXVK's
    // present-wait contract correctly: it intercepts vkWaitForPresentKHR (v1) and
    // satisfies it via the phase-anchored vsync sleep — this is exactly how the
    // working FIFO titles (Vampire Survivors) pace. So we must NOT gate
    // present_wait/present_id v1: doing so (a) forces DXVK off the path we handle
    // and onto a fence-based throttle that DEADLOCKS under the bypass, and (b)
    // disables the layer's own WFP interception (it loads vkWaitForPresentKHR by
    // name → "extension absent" → no pacing hook). Verified on-device: gating v1
    // froze the spinning-cube test after 4 frames in vkWaitForFences(UINT64_MAX).
    //
    // What we DO gate = only the parts the layer does NOT mirror:
    //   - present_wait2 / present_id2 (the per-surface variants DXVK is migrating
    //     to; our interception only covers v1, so hide v2 until we implement it,
    //     forcing DXVK to keep using v1 which we handle). No-op on DXVK <= 2.7.1.
    //   - swapchain_maintenance1 (EXT+KHR): the 2.6 swapchain rework; gating it
    //     makes DXVK use the plain recreate path, avoiding the deferred
    //     apply-at-acquireNextImage behavior a CreateSwapchain-intercepting layer
    //     can mishandle. Harmless fallback (DXVK recreates on vsync toggle).
    // Consumed by the wrapper ICD via WRAPPER_EXTENSION_BLACKLIST (comma-sep).
    // See project memory + the 2026-06 DXVK/DAC research note.
    private val DAC_GATED_EXTENSIONS = listOf(
        "VK_KHR_present_id2",
        "VK_KHR_present_wait2",
        "VK_EXT_swapchain_maintenance1",
        "VK_KHR_swapchain_maintenance1",
    )

    // Bumped when the bundled .so changes (forces re-copy into containers)
    // v1.1.0: JTRACE runtime-gated (WINLATOR_AHB_JTRACE=1 to enable) — removes
    // ~4 logcat writes/frame from the release hot path.
    private const val RUNTIME_VERSION = "v1.1.0-android-arm64-v8a"

    // Asset paths (shipped in Chunk B once the wrapper-ABI .so is built)
    private const val ASSET_DIR = "dac/android_arm64_v8a"
    private const val ASSET_LIB = "$ASSET_DIR/$LIB_FILENAME"
    private const val ASSET_MANIFEST = "$ASSET_DIR/$MANIFEST_FILENAME"

    // ---- Public API --------------------------------------------------------

    /** The selected pipeline for this container ("quality"/"performance"/"native"). */
    @JvmStatic
    fun pipeline(container: Container): String =
        container.getExtra(EXTRA_GRAPHICS_PIPELINE, DEFAULT_PIPELINE)

    /**
     * Whether DAC can run for this container. Requires the BIONIC variant and a
     * wrapper/Turnip in-guest Vulkan ICD (Vortek/virgl proxy the driver app-side,
     * so there is no in-guest swapchain for the layer to intercept).
     */
    @JvmStatic
    fun isSupported(container: Container): Boolean {
        if (!container.containerVariant.equals(Container.BIONIC, ignoreCase = true)) return false
        val driver = container.getGraphicsDriver().lowercase()
        return driver.contains("wrapper") || driver.contains("turnip")
    }

    /** Whether a DAC pipeline (quality/performance) is selected and supported. */
    @JvmStatic
    fun isArmed(container: Container): Boolean {
        if (!isSupported(container)) return false
        val p = pipeline(container)
        return p == PIPELINE_QUALITY || p == PIPELINE_PERFORMANCE
    }

    /**
     * Whether the experimental host-side direct-scanout path should be activated
     * (renderer.setNativeMode(true)). This is NOT a DAC-layer pipeline — the guest
     * AHB layer stays OFF (isArmed=false), so the guest presents through the real
     * X11/DRI3 WSI; only the renderer's dormant DRI3-AHB scanout fast path is armed.
     */
    @JvmStatic
    fun usesNativeScanout(container: Container): Boolean =
        pipeline(container) == PIPELINE_NATIVE_SCANOUT

    /**
     * Install the AHB layer runtime into the container's filesystem.
     * Mirrors {@link LsfgVkManager#ensureRuntimeInstalled}.
     *
     * - libahb_layer.so → ~/.local/lib/
     * - ahb_layer.json  → ~/.local/share/vulkan/implicit_layer.d/ (library_path patched)
     *
     * @return true if installed/up-to-date; false if the asset is missing
     *         (e.g. before the Chunk B .so is bundled) or on error.
     */
    @JvmStatic
    fun ensureRuntimeInstalled(context: Context, container: Container): Boolean {
        if (!isSupported(container)) return false

        val rootDir = container.rootDir
        val localLibDir = File(rootDir, LIB_RELATIVE_DIR)
        val layerDir = File(rootDir, LAYER_RELATIVE_DIR)
        val libFile = File(localLibDir, LIB_FILENAME)
        val manifestFile = File(layerDir, MANIFEST_FILENAME)
        val versionFile = File(layerDir, VERSION_FILENAME)

        val installedVersion = versionFile.takeIf { it.exists() }?.readText()?.trim().orEmpty()
        val needsInstall = installedVersion != RUNTIME_VERSION ||
            !libFile.isFile || !manifestFile.isFile
        if (!needsInstall) {
            Timber.tag(TAG).d("AHB layer runtime %s already installed in %s", RUNTIME_VERSION, rootDir)
            return true
        }

        return try {
            localLibDir.mkdirs()
            layerDir.mkdirs()

            FileUtils.copy(context, ASSET_LIB, libFile)
            val manifestText = context.assets.open(ASSET_MANIFEST)
                .bufferedReader().use { it.readText() }
                .replace(
                    "\"library_path\": \"$LIB_FILENAME\"",
                    "\"library_path\": \"$MANIFEST_LIBRARY_PATH\""
                )
            FileUtils.writeString(manifestFile, manifestText)
            FileUtils.writeString(versionFile, RUNTIME_VERSION)

            if (libFile.exists()) FileUtils.chmod(libFile, 0b111101101)
            if (manifestFile.exists()) FileUtils.chmod(manifestFile, 0b110100100)
            if (versionFile.exists()) FileUtils.chmod(versionFile, 0b110100100)

            val ok = libFile.isFile && manifestFile.isFile
            if (ok) Timber.tag(TAG).i("Installed AHB layer runtime %s into %s", RUNTIME_VERSION, rootDir)
            else Timber.tag(TAG).e("AHB layer runtime install verification failed")
            ok
        } catch (t: Throwable) {
            // Expected until the Chunk B wrapper-ABI .so is bundled as an asset.
            Timber.tag(TAG).w(t, "AHB layer asset not available yet (Chunk B pending)")
            false
        }
    }

    /**
     * Apply DAC env vars to the launch environment, mirroring
     * {@link LsfgVkManager#applyLaunchEnv}. Called from the launcher.
     *
     * @return true if a DAC pipeline is armed and env vars were applied.
     */
    @JvmStatic
    fun applyLaunchEnv(context: Context, container: Container, envVars: EnvVars): Boolean {
        // Clear stale DAC vars first
        envVars.remove(ENV_ENABLE)
        envVars.remove(ENV_DISABLE)
        envVars.remove(ENV_AHB_SERVER)
        envVars.remove(ENV_DIRECT_RENDER)

        if (!isArmed(container)) {
            // Disable: tell the loader to skip the layer, and drop the manifest.
            envVars.put(ENV_DISABLE, "1")
            disableLayerInContainer(container)
            Timber.tag(TAG).i("DAC disabled (pipeline=%s, supported=%s)",
                pipeline(container), isSupported(container))
            return false
        }

        val directRender = if (pipeline(container) == PIPELINE_QUALITY) "1" else "0"
        envVars.put(ENV_ENABLE, "1")
        envVars.put(ENV_DIRECT_RENDER, directRender)
        // The AHB server (AHBSocketServerComponent) binds under the IMAGEFS ROOT —
        // createSocket(imageFs.rootDir, AHB_SERVER_PATH) → <imagefs>/tmp/.ahb/AHB0 —
        // NOT under container.rootDir (the per-game home, .../imagefs/home/xuser-*).
        // The guest layer does a literal connect(getenv("ANDROID_AHB_SERVER")), and
        // the bionic redirect passes /data/.../imagefs/* host paths through unchanged,
        // so both sides must use the identical imagefs-rooted host path. This mirrors
        // the working Winlator fork (rootDir.getPath() + AHB_SOCKET_PATH).
        val imagefsRoot = ImageFs.find(context).rootDir
        envVars.put(ENV_AHB_SERVER, File(imagefsRoot, UnixSocketConfig.AHB_SERVER_PATH).absolutePath)

        // ── FORWARD-COMPAT WSI EXTENSION GATING (Option 1, SURGICAL) ────────
        // Keep present_wait/present_id v1 (the layer intercepts vkWaitForPresentKHR
        // and paces correctly — do NOT disable it) and only hide the variants the
        // layer doesn't mirror (DAC_GATED_EXTENSIONS: present_*2 + swapchain_
        // maintenance1). Crucially we do NOT set WRAPPER_DISABLE_PRESENT_WAIT — the
        // earlier full gating set it to 1, which disabled the layer's WFP hook and
        // froze games in vkWaitForFences. We MERGE with any user-set blacklist
        // (XServerScreen already put the container's value into envVars before this
        // runs — BionicProgramLauncherComponent does envVars.putAll(this.envVars)
        // then applyLaunchEnv), so we never clobber it.
        // A/B escape hatch: marker <externalFiles>/.ahb_no_gating disables ALL of
        // this (toggle via adb, no rebuild) to compare against ungated DAC.
        val noGating = File(context.getExternalFilesDir(null), ".ahb_no_gating").exists()
        if (noGating) {
            Timber.tag(TAG).w("DAC DIAG: .ahb_no_gating present — skipping WSI extension-gating")
        } else {
            val existingBlacklist = (envVars["WRAPPER_EXTENSION_BLACKLIST"] ?: "")
                .split(",").map { it.trim() }.filter { it.isNotEmpty() }
            val mergedBlacklist = (existingBlacklist + DAC_GATED_EXTENSIONS).distinct()
            envVars.put("WRAPPER_EXTENSION_BLACKLIST", mergedBlacklist.joinToString(","))
            Timber.tag(TAG).i("DAC extension-gating (surgical, present_wait kept): blacklist=[%s]",
                mergedBlacklist.joinToString(","))
        }

        // ── DIAGNOSTIC TOGGLE (no rebuild needed) ───────────────────────────
        // If the marker file <externalFiles>/.ahb_no_intercept exists, pass
        // WINLATOR_AHB_NO_INTERCEPT=1 to the guest layer so it loads + connects
        // the AHB socket but DECLINES to hook any swapchain (pure X11 passthrough
        // with the layer still present). Used to isolate whether a game's hang is
        // caused by the layer's swapchain interception / scanout mode vs the DAC
        // X-server lifecycle itself. The app's external files dir is used because
        // it's adb-writable (no root, app not debuggable) AND app-readable.
        // Toggle with:
        //   adb shell touch /sdcard/Android/data/app.gamenative/files/.ahb_no_intercept   (enable)
        //   adb shell rm    /sdcard/Android/data/app.gamenative/files/.ahb_no_intercept   (disable)
        val noInterceptMarker = File(context.getExternalFilesDir(null), ".ahb_no_intercept")
        if (noInterceptMarker.exists()) {
            envVars.put("WINLATOR_AHB_NO_INTERCEPT", "1")
            Timber.tag(TAG).w("DAC DIAG: WINLATOR_AHB_NO_INTERCEPT=1 (marker present; layer will NOT hook swapchains)")
        } else {
            envVars.remove("WINLATOR_AHB_NO_INTERCEPT")
        }

        // Implicit layer is auto-discovered, but set VK_INSTANCE_LAYERS too to
        // match the reference fork's explicit enablement.
        val existingInstanceLayers = envVars[ENV_INSTANCE_LAYERS] ?: ""
        if (existingInstanceLayers.isEmpty()) envVars.put(ENV_INSTANCE_LAYERS, LAYER_NAME)
        else if (!existingInstanceLayers.contains(LAYER_NAME))
            envVars.put(ENV_INSTANCE_LAYERS, "$existingInstanceLayers:$LAYER_NAME")

        // Append the container's implicit_layer.d to VK_LAYER_PATH.
        val containerLayerDir = File(container.rootDir, LAYER_RELATIVE_DIR)
        val existingLayerPath = envVars["VK_LAYER_PATH"] ?: ""
        if (existingLayerPath.isNotEmpty())
            envVars.put("VK_LAYER_PATH", "$existingLayerPath:${containerLayerDir.absolutePath}")
        else
            envVars.put("VK_LAYER_PATH", containerLayerDir.absolutePath)

        Timber.tag(TAG).i("DAC armed: pipeline=%s, directRender=%s", pipeline(container), directRender)

        // ── DIAGNOSTIC: DAC + LSFG-VK co-existence ──────────────────────────
        // Both are swapchain-intercepting implicit Vulkan layers (both hook
        // vkCreateSwapchainKHR / vkQueuePresentKHR). When both are armed, turn on
        // the Vulkan loader's layer-chain logging and dump the resolved layer env
        // so we can see in logcat which layer ends up above the other and whether
        // one is effectively dropped. Remove once the interaction is understood.
        if (LsfgVkManager.isArmed(container)) {
            envVars.put("VK_LOADER_DEBUG", "all")
            Timber.tag(TAG).w("DAC_LSFG_DIAG: BOTH ARMED (DAC pipeline=%s + LSFG mult=%d) — layer-chain conflict diag on",
                pipeline(container), LsfgVkManager.multiplier(container))
            Timber.tag(TAG).w("DAC_LSFG_DIAG: VK_INSTANCE_LAYERS=%s", envVars[ENV_INSTANCE_LAYERS] ?: "(unset)")
            Timber.tag(TAG).w("DAC_LSFG_DIAG: VK_LAYER_PATH=%s", envVars["VK_LAYER_PATH"] ?: "(unset)")
        }
        return true
    }

    /** Remove the layer manifest so the Vulkan loader can't discover it. */
    private fun disableLayerInContainer(container: Container) {
        val manifest = File(container.rootDir, "$LAYER_RELATIVE_DIR/$MANIFEST_FILENAME")
        if (manifest.exists()) {
            manifest.delete()
            Timber.tag(TAG).d("Removed AHB layer manifest to disable DAC")
        }
    }
}
