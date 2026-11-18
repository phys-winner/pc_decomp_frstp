package com.frostpunk.staticrecomp.launcher;

import android.content.Context;
import android.os.CancellationSignal;
import java.io.File;

/** Driver downloads use the launcher's standard cancellable work panel. */
final class FrostpunkDriverDownload extends Work.Job {
    private final Context context;
    private final String url;
    private final CancellationSignal cancellation = new CancellationSignal();
    String installedTitle;

    FrostpunkDriverDownload(Context context, String name, String url) {
        super(Texts.RENDERER, FrostpunkDevice.tr(context, "Downloading ", "Скачивание ") + name,
              new File(context.getCacheDir(), "turnip-download.part"));
        this.context = context.getApplicationContext();
        this.url = url;
        file = name;
    }

    @Override void cancel() {
        super.cancel();
        cancellation.cancel();
    }

    @Override void work() throws Exception {
        try {
            check();
            String key = FrostpunkFiles.hex(java.security.MessageDigest.getInstance("SHA-256")
                .digest(url.getBytes(java.nio.charset.StandardCharsets.UTF_8)));
            String name = State.get(context, "turnip-cache", key, "");
            File cached = new File(VulkanDriver.TURNIP.folder(context), name);
            // shortcut: release URLs key the cache; add asset digests if publishers replace files in place.
            if (name.matches("libturnip-[0-9a-f]{64}[.]so") && cached.isFile()
                    && name.equals("libturnip-" + VulkanDriver.sha(cached) + ".so")) {
                check();
                installedTitle = VulkanDriver.TURNIP.select(context, cached,
                    State.get(context, "turnip-cache", name + ".title", name));
                return;
            }
            FrostpunkGithub.download(context, url, target, 256L << 20, cancellation, this);
            check();
            installedTitle = VulkanDriver.TURNIP.install(context, null, target);
            String stored = VulkanDriver.TURNIP.chosen(context).getName();
            if (!State.update(context, "turnip-cache", values -> {
                values.setProperty(key, stored);
                values.setProperty(stored + ".title", installedTitle);
            })) throw new java.io.IOException("cannot save the driver cache");
        } finally {
            if (target.exists() && !target.delete()) target.deleteOnExit();
        }
    }
}
