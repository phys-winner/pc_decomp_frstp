    private void fpWarnLowMemory() {
        if ("1".equals(State.get(this, State.CHOICES, "warned.lowMemory", "0"))) return;
        android.app.ActivityManager manager = (android.app.ActivityManager)getSystemService(ACTIVITY_SERVICE);
        if (manager == null) return;
        android.app.ActivityManager.MemoryInfo memory = new android.app.ActivityManager.MemoryInfo();
        manager.getMemoryInfo(memory);
        if (!FrostpunkDevice.lowMemory(memory.totalMem)) return;
        ui.show(ui.dialog()
            .setTitle(FrostpunkDevice.tr(this, "Low device RAM", "Недостаточный объём ОЗУ"))
            .setMessage(FrostpunkDevice.tr(this,
                "This device has less than 8 GB of RAM available to Android. The game may close because memory is low, especially in large cities. Use Auto or a reduced memory mode in the launcher. This warning appears once.",
                "На этом устройстве Android доступно менее 8 ГБ оперативной памяти. Игра может закрываться из-за нехватки памяти, особенно в больших городах. Используйте режим расхода памяти «Авто» или экономичный режим в лаунчере. Это предупреждение показывается один раз."))
            .setCancelable(false)
            .setPositiveButton(android.R.string.ok, (dialog, which) ->
                State.put(this, State.CHOICES, "warned.lowMemory", "1")));
    }

    private Spec.Distribution fpTexturePlay;
    private Spec.Distribution fpDataPlay;
    private boolean fpTextureRebuild;
    private boolean fpPacksOffered;
    private org.json.JSONArray fpTurnipReleases;
    private int fpTurnipIndex;
    private String fpTurnipRepo;
    private android.app.AlertDialog fpTurnipDialog;
    private static final String[] FP_TURNIP_REPOS = {
        "MrPurple666/purple-turnip",
        "StevenMXZ/Adreno-Tools-Drivers",
        "The412Banner/Banners-Turnip",
        "Vauzi-17/710",
        "whitebelyash/AdrenoToolsDrivers",
        "aydar-kamaltdinov/mesa-unified",
        "K11MCH1/AdrenoToolsDrivers"
    };

    private boolean fpReducedTextureQuality() {
        boolean enabled = true, reduced = true;
        for (Spec.Option option : spec.options(selected.id)) {
            if (option.key.equals("switch_graphics")) enabled = !Launch.option(this, option).equals("0");
            if (option.key.equals("textures")) reduced = Launch.option(this, option).equals("switch");
        }
        return enabled && reduced;
    }
    private String fpTextureQualityTitle(boolean reduced) {
        return reduced ? FrostpunkDevice.tr(this, "Switch — selected world mips", "Switch — сокращённые mip-цепочки мира")
            : FrostpunkDevice.tr(this, "PC — all mip levels", "ПК — все mip-уровни");
    }
    private void fpChooseTextureQuality() {
        String[] titles = {fpTextureQualityTitle(true), fpTextureQualityTitle(false)};
        ui.show(ui.dialog().setTitle(FrostpunkDevice.tr(this, "Texture quality", "Качество текстур"))
            .setSingleChoiceItems(titles, fpReducedTextureQuality() ? 0 : 1, (dialog, which) -> {
                State.put(this, State.CHOICES, "option.textures", which == 0 ? "switch" : "pc");
                if (which == 0) State.put(this, State.CHOICES, "option.switch_graphics", "1");
                textureScan = null; dialog.dismiss(); refresh(Texts.OPTIONS, Texts.TEXTURES);
            }).setNegativeButton(Texts.t(this, Texts.CANCEL), null));
    }
    private void fpTextureQualityCard(LinearLayout card, boolean idle) {
        View quality = ui.choice(FrostpunkDevice.tr(this, "Texture quality", "Качество текстур"),
            fpTextureQualityTitle(fpReducedTextureQuality()), false, true, this::fpChooseTextureQuality);
        Ui.enable(quality, idle); card.addView(quality);
        card.addView(ui.label(FrostpunkDevice.tr(this,
            "Switch converts only the mips needed for its world texture sizes. UI and numerical textures keep their detail. A full PC conversion also covers Switch quality; raising quality may require conversion again. GPUs with native DXT use the original ZIP.",
            "Switch конвертирует только mip-уровни, нужные для его размеров текстур мира. Интерфейс и числовые текстуры сохраняют детализацию. Полный набор ПК подходит и для Switch; повышение качества может потребовать повторной конвертации. GPU с поддержкой DXT читают оригиналы из ZIP."), 13, ui.muted));
    }

    private boolean fpGraphicsGroup(String key) {
        return key.equals("snow") || key.equals("buildings") || key.equals("textures") || key.equals("shadows") || key.equals("effects");
    }
    private void fpGraphicsAdvanced() {
        android.widget.ScrollView scroll = new android.widget.ScrollView(this);
        LinearLayout values = new LinearLayout(this); values.setOrientation(LinearLayout.VERTICAL);
        values.setPadding(ui.dp(20), ui.dp(8), ui.dp(20), ui.dp(8));
        values.addView(ui.label(FrostpunkDevice.tr(this,
            "Selected groups use Switch graphics while the main switch is enabled. Changes apply from the next game start.",
            "Выбранные группы используют графику Switch при включённой общей галке. Изменения применятся при следующем запуске игры."), 13, ui.muted));
        for (Spec.Option option : spec.options(selected.id)) if (fpGraphicsGroup(option.key)) {
            values.addView(ui.toggle(Texts.of(this, option.title), Texts.of(this, option.hint),
                "switch".equals(Launch.option(this, option)),
                on -> { State.put(this, State.CHOICES, "option." + option.key, on ? "switch" : "pc");
                    if (option.key.equals("textures")) { textureScan = null; refresh(Texts.TEXTURES); } }));
        }
        scroll.addView(values);
        ui.show(ui.dialog().setTitle(FrostpunkDevice.tr(this, "Advanced graphics settings", "Дополнительные настройки"))
            .setView(scroll).setPositiveButton(Texts.t(this, Texts.OK), null));
    }

    private String fpDeviceProblem(Spec.Distribution distribution) {
        if (gpu == null) return null;
        if (!Probe.carried(this)) return FrostpunkDevice.text(this, "no_probe");
        List<Renderers.Status> statuses = Renderers.evaluate(this, spec, distribution, gpu, false);
        StringBuilder why = new StringBuilder();
        for (Renderers.Status status : statuses) {
            if (status.available && status.verified) return null;
            if (why.length() != 0) why.append("\n\n");
            why.append(status.mode.title).append(": ").append(status.reason);
        }
        return why.length() == 0 ? FrostpunkDevice.text(this, "no_renderer") : why.toString();
    }
    private void fpUnsupported(String problem) {
        note(Texts.PLAY, problem, true);
    }
    private void fpResourceWarnings() {
        File game = Install.folder(this, spec, selected, selected.part("game"));
        java.util.List<String> warnings = new java.util.ArrayList<>();
        try { FrostpunkFiles.launcher(this, game, selected); }
        catch (java.io.IOException failure) { warnings.add(failure.getMessage()); }
        for (String kind : FrostpunkData.KINDS) {
            boolean valid = State.get(this, "resources", kind + ".error", "").isEmpty();
            if (!FrostpunkData.installed(this, game, kind) && !(valid && FrostpunkData.packagePresent(this, game, kind)))
                warnings.add(kind.equals("port-ui")
                    ? FrostpunkDevice.tr(this, "Port UI data is missing or incompatible. Interface scale is limited to 100%.", "Данные адаптации интерфейса отсутствуют или несовместимы. Масштаб интерфейса ограничен 100%.")
                    : FrostpunkDevice.tr(this, "Switch UI data is missing or incompatible. The Switch interface cannot be enabled.", "Данные интерфейса Switch отсутствуют или несовместимы. Включить интерфейс Switch нельзя."));
        }
        if (!warnings.isEmpty()) {
            LinearLayout card = ui.card(files);
            for (String warning : warnings) card.addView(ui.label(warning, 13, ui.error));
        }
        fpOfferResourcePacks();
    }
    private void fpPlay(Spec.Distribution distribution) {
        if (gpu == null || Probe.report(this) == null) {
            if (!Probe.carried(this)) { fpUnsupported(FrostpunkDevice.text(this, "no_probe")); return; }
            checkRenderer(); setBusy(Texts.PLAY, Texts.t(this, Texts.R_CHECKING));
            handler.postDelayed(() -> { if (!isFinishing() && !isDestroyed()) { setBusy(null); play(distribution); } }, PROBE_POLL_MS);
            return;
        }
        String problem = fpDeviceProblem(distribution);
        if (problem != null) android.util.Log.w("FrostpunkLauncher", problem);
        if (fpDataPlay != distribution) {
            fpDataPlay = distribution;
            File game = Install.folder(this, spec, distribution, distribution.part("game"));
            try { FrostpunkFiles.launcher(this, game, distribution); }
            catch (java.io.IOException failure) { fpDataPlay = null; note(Texts.GAME_FILES, failure.getMessage(), true); return; }
            startJob(new FrostpunkData(this, game));
            return;
        }
        fpDataPlay = null;
        FrostpunkTextures backend = new FrostpunkTextures();
        boolean reduced = fpReducedTextureQuality(); backend.quality(reduced);
        File root = Install.folder(this, spec, distribution, distribution.parts.get(0));
        setBusy(Texts.PLAY, FrostpunkDevice.tr(this, "Checking game textures…", "Проверка текстур игры…"));
        background(() -> backend.scan(root), scan -> {
            setBusy(null);
            if (scan.failure != null) { note(Texts.TEXTURES, scan.failure, true); return; }
            java.util.Map<String, String> report = Probe.report(this);
            if (report == null) { play(distribution); return; }
            String renderer = effectiveRenderer(distribution);
            if (scan.converted == TextureBackend.ASTC) {
                try {
                    if (!FrostpunkTextures.coversQuality(root, reduced)) {
                        boolean nativeDxt = FrostpunkDevice.dxt(report, renderer);
                        ui.show(ui.dialog().setTitle(FrostpunkDevice.tr(this, "Texture quality is not ready", "Текстур нужного качества нет"))
                            .setMessage(FrostpunkDevice.tr(this,
                                "The existing conversion has Switch mips; PC quality requires the full chain. Restore the originals" + (nativeDxt ? " and start the game?" : " and convert at PC quality? The source ZIP is kept."),
                                "Готовый набор содержит mip-уровни Switch; для качества ПК нужна полная цепочка. Вернуть оригиналы" + (nativeDxt ? " и запустить игру?" : " и сконвертировать качество ПК? Исходный ZIP сохранится.")))
                            .setPositiveButton(FrostpunkDevice.tr(this, "Prepare and play", "Подготовить и играть"), (dialog, which) -> {
                                textures = backend; fpTexturePlay = distribution; fpTextureRebuild = !nativeDxt;
                                startTextures(nativeDxt ? 0 : TextureBackend.ASTC);
                            }).setNegativeButton(Texts.t(this, Texts.CANCEL), null));
                        return;
                    }
                } catch (java.io.IOException failure) { note(Texts.PLAY, failure.getMessage(), true); return; }
            }
            if ((scan.converted == TextureBackend.ASTC || scan.textures > 0 && !FrostpunkDevice.dxt(report, renderer))
                    && !FrostpunkDevice.astc(report, renderer)) { fpUnsupported(FrostpunkDevice.explain(this, "astc")); return; }
            if (scan.textures > 0 && !FrostpunkDevice.dxt(report, renderer)) {
                String body = FrostpunkDevice.tr(this,
                    "This GPU needs ASTC textures. Convert at the selected quality? Unneeded top mips are skipped at Switch quality. Original files are kept; the game starts after conversion.",
                    "Для этого GPU нужны текстуры ASTC. Сконвертировать выбранное качество? Для Switch ненужные верхние mip-уровни будут пропущены. Исходные файлы сохранятся, после конвертации игра запустится.");
                body += "\n\n" + fpTextureQualityTitle(reduced);
                body += "\n" + FrostpunkDevice.tr(this, "Additional space: ", "Дополнительное место: ") + Texts.size(this, backend.outputBytes());
                if (notificationsUnasked()) body += "\n\n" + Texts.t(this, Texts.PERM_NOTIFY);
                ui.show(ui.dialog().setTitle("DXT → ASTC").setMessage(body)
                    .setPositiveButton(FrostpunkDevice.tr(this, "Convert", "Сконвертировать"), (dialog, which) -> {
                        textures = backend; fpTexturePlay = distribution; startTextures(TextureBackend.ASTC);
                    }).setNegativeButton(Texts.t(this, Texts.CANCEL), null));
            } else fpStart(distribution);
        }, failure -> { setBusy(null); note(Texts.TEXTURES, String.valueOf(failure.getMessage()), true); });
    }
    private void fpStart(Spec.Distribution distribution) {
        try { Launch.start(this, distribution, effectiveRenderer(distribution), hooks); finish(); }
        catch (ActivityNotFoundException failure) { note(Texts.PLAY, Texts.t(this, Texts.NOT_IN_BUILD), true); }
    }
    private void fpTexturesFinished(Work.Job job) {
        if (job instanceof FrostpunkData) {
            Spec.Distribution distribution = fpDataPlay;
            if (job.failure == null && distribution != null) handler.post(() -> play(distribution));
            else fpDataPlay = null;
            return;
        }
        if (fpTexturePlay == null || (job.section != Texts.TEXTURES && job.section != Texts.PLAY)) return;
        Spec.Distribution distribution = fpTexturePlay; fpTexturePlay = null;
        if (job.failure == null) handler.post(() -> play(distribution));
    }

    private void fpOfferResourcePacks() {
        if (fpPacksOffered || spec == null || !spec.updates || selected == null || !Install.ready(this, spec, selected)) return;
        fpPacksOffered = true;
        File game = Install.folder(this, spec, selected, selected.part("game"));
        List<String> missing = new ArrayList<>();
        for (String kind : FrostpunkData.KINDS)
            if (!FrostpunkData.packagePresent(this, game, kind)) missing.add(kind);
        if (missing.isEmpty()) return;
        background(() -> FrostpunkPacks.offers(this, spec.github, missing), offers -> {
            if (offers == null || offers.isEmpty() || isFinishing()) return;
            StringBuilder body = new StringBuilder(FrostpunkDevice.tr(this,
                "Interface packages are missing. Download them now?",
                "Пакетов интерфейса нет. Скачать их сейчас?"));
            for (FrostpunkPacks.Offer offer : offers)
                body.append("\n").append(offer.kind).append(" — ").append(offer.name)
                    .append(offer.size > 0 ? " (" + Texts.size(this, offer.size) + ")" : "");
            ui.show(ui.dialog().setTitle(FrostpunkDevice.tr(this, "Interface packages", "Пакеты интерфейса"))
                .setMessage(body.toString())
                .setPositiveButton(FrostpunkDevice.tr(this, "Download", "Скачать"), (dialog, which) -> fpDownloadPacks(game, offers))
                .setNegativeButton(Texts.t(this, Texts.CANCEL), null));
        }, failure -> android.util.Log.w("FrostpunkLauncher", "resource packs", failure));
    }

    private void fpDownloadPacks(File game, List<FrostpunkPacks.Offer> offers) {
        setBusy(Texts.GAME_FILES, FrostpunkDevice.tr(this, "Downloading interface packages…", "Скачивание пакетов интерфейса…"));
        background(() -> {
            for (FrostpunkPacks.Offer offer : offers) FrostpunkPacks.install(this, game, offer);
            return true;
        }, done -> {
            setBusy(null);
            startJob(new FrostpunkData(this, game));
            refresh(Texts.GAME_FILES);
        }, failure -> {
            setBusy(null);
            note(Texts.GAME_FILES, String.valueOf(failure.getMessage()), true);
        });
    }

    private void fpTurnipSources(LinearLayout card) {
        card.addView(ui.buttons(ui.button(FrostpunkDevice.tr(this, "Download a Turnip driver", "Скачать драйвер Turnip"),
            Ui.SECONDARY, this::fpChooseTurnipRepo)));
    }

    private void fpChooseTurnipRepo() {
        ui.show(ui.dialog().setTitle(FrostpunkDevice.tr(this, "Turnip repositories", "Репозитории Turnip"))
            .setItems(FP_TURNIP_REPOS, (dialog, which) -> fpOpenTurnipRepo(FP_TURNIP_REPOS[which]))
            .setNegativeButton(Texts.t(this, Texts.CANCEL), null));
    }

    private void fpOpenTurnipRepo(String repo) {
        setBusy(Texts.RENDERER, FrostpunkDevice.tr(this, "Loading releases…", "Загрузка релизов…"));
        background(() -> FrostpunkGithub.releases(this, repo), releases -> {
            setBusy(null);
            fpTurnipRepo = repo;
            fpTurnipReleases = releases;
            fpTurnipIndex = fpTurnipStep(-1, 1);
            fpShowTurnipRelease();
        }, failure -> {
            setBusy(null);
            note(Texts.RENDERER, String.valueOf(failure.getMessage()), true);
        });
    }

    private int fpTurnipStep(int from, int direction) {
        if (fpTurnipReleases == null) return 0;
        for (int i = from + direction; i >= 0 && i < fpTurnipReleases.length(); i += direction) {
            org.json.JSONObject item = fpTurnipReleases.optJSONObject(i);
            if (item != null && !item.optBoolean("draft")) return i;
        }
        return Math.max(0, Math.min(from, fpTurnipReleases.length() - 1));
    }

    private void fpShowTurnipRelease() {
        if (fpTurnipDialog != null) fpTurnipDialog.dismiss();
        if (fpTurnipReleases == null || fpTurnipReleases.length() == 0) {
            note(Texts.RENDERER, FrostpunkDevice.tr(this, "This repository has no releases.", "В этом репозитории нет релизов."), true);
            return;
        }
        org.json.JSONObject release = fpTurnipReleases.optJSONObject(fpTurnipIndex);
        if (release == null) return;
        ScrollView scroll = new ScrollView(this);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(ui.dp(20), ui.dp(8), ui.dp(20), ui.dp(8));
        String tag = release.optString("tag_name", "");
        layout.addView(ui.label(fpTurnipRepo + "  " + tag, 16, ui.text));
        int newer = fpTurnipStep(fpTurnipIndex, -1), older = fpTurnipStep(fpTurnipIndex, 1);
        List<TextView> moves = new ArrayList<>();
        if (newer != fpTurnipIndex) moves.add(ui.button(FrostpunkDevice.tr(this, "Newer release", "Более новый релиз"), Ui.QUIET, () -> {
            fpTurnipIndex = newer; fpShowTurnipRelease();
        }));
        if (older != fpTurnipIndex) moves.add(ui.button(FrostpunkDevice.tr(this, "Older release", "Более старый релиз"), Ui.QUIET, () -> {
            fpTurnipIndex = older; fpShowTurnipRelease();
        }));
        if (!moves.isEmpty()) layout.addView(ui.buttons(moves.toArray(new TextView[0])));
        String body = release.optString("body", "");
        if (body.length() > 100000) body = body.substring(0, 100000);
        String plain = body.isEmpty() ? FrostpunkDevice.tr(this, "No release notes.", "Нет описания релиза.") : body;
        try {
            android.webkit.WebView web = new android.webkit.WebView(this);
            web.setBackgroundColor(0xFF101820);
            web.getSettings().setJavaScriptEnabled(false);
            web.loadDataWithBaseURL("https://github.com", fpReleaseHtml(plain), "text/html", "utf-8", null);
            layout.addView(web, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ui.dp(220)));
        } catch (RuntimeException failure) {
            layout.addView(ui.label(plain, 13, ui.text));
        }
        org.json.JSONArray assets = release.optJSONArray("assets");
        if (assets == null || assets.length() == 0) layout.addView(ui.label(FrostpunkDevice.tr(this, "This release has no files.", "В этом релизе нет файлов."), 13, ui.muted));
        else for (int i = 0; i < assets.length(); ++i) {
            org.json.JSONObject asset = assets.optJSONObject(i);
            if (asset == null) continue;
            String name = asset.optString("name");
            long size = asset.optLong("size");
            String url = asset.optString("browser_download_url");
            String label = name + (size > 0 ? " (" + Texts.size(this, size) + ")" : "");
            layout.addView(ui.button(label, Ui.SECONDARY, () -> fpDownloadTurnip(name, url, size)));
        }
        scroll.addView(layout);
        fpTurnipDialog = ui.show(ui.dialog().setTitle(FrostpunkDevice.tr(this, "Turnip release", "Релиз Turnip"))
            .setView(scroll).setNegativeButton(Texts.t(this, Texts.CANCEL), null));
    }

    private String fpReleaseHtml(String body) {
        String escaped = body.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;");
        escaped = escaped.replaceAll("(?m)\\*\\*([^*\\n]+)\\*\\*", "<b>$1</b>");
        escaped = escaped.replaceAll("(https?://[\\\\w./?&=%#@:+~_-]+)", "<a href=\"$1\">$1</a>");
        return "<html><head><meta charset='utf-8'></head><body style='color:#E8EEF2;background:#101820;font-size:14px'>"
            + escaped.replace("\n", "<br>") + "</body></html>";
    }

    private void fpDownloadTurnip(String name, String url, long size) {
        if (url == null || url.isEmpty()) { note(Texts.RENDERER, FrostpunkDevice.tr(this, "No download link.", "Нет ссылки на скачивание."), true); return; }
        if (size > (256L << 20)) { note(Texts.RENDERER, FrostpunkDevice.tr(this, "The file is larger than 256 MB.", "Файл больше 256 МБ."), true); return; }
        if (fpTurnipDialog != null) fpTurnipDialog.dismiss();
        startJob(new FrostpunkDriverDownload(this, name, url));
    }

    private void fpDriverCleanup(LinearLayout card) {
        if (TurnipDriver.unused(this).isEmpty()) return;
        TextView clear = ui.button(FrostpunkDevice.tr(this, "Delete unused external drivers", "Удалить неиспользуемые внешние драйверы"),
            Ui.SECONDARY, () -> {
                if (!idle()) return;
                try { TurnipDriver.removeUnused(this); refresh(Texts.ABOUT); }
                catch (java.io.IOException failure) { note(Texts.ABOUT, failure.getMessage(), true); }
            });
        Ui.enable(clear, idle());
        card.addView(ui.buttons(clear));
    }
