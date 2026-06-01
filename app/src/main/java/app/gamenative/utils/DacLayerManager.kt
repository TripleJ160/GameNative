package app.gamenative.utils

import android.content.Context
import com.winlator.container.Container
import com.winlator.core.FileUtils
import com.winlator.core.envvars.EnvVars
import com.winlator.xconnector.UnixSocketConfig
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
    // DAC is opt-in on this fork: existing containers keep their current
    // (X11/Vortek/native) behaviour until the user selects a DAC pipeline.
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

    // Bumped when the bundled .so changes (forces re-copy into containers)
    private const val RUNTIME_VERSION = "v1.0.0-android-arm64-v8a"

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
    fun applyLaunchEnv(container: Container, envVars: EnvVars): Boolean {
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
        // Guest sees the socket at the absolute container path (bionic = real fs).
        envVars.put(ENV_AHB_SERVER, File(container.rootDir, UnixSocketConfig.AHB_SERVER_PATH).absolutePath)

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
