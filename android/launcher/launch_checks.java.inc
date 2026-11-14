    private android.app.AlertDialog fpCompatibilityDialog;
    private Spec.Distribution fpTexturePlay;
    private Spec.Distribution fpDataPlay;
    private boolean fpTextureRebuild;

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
        if (gpu == null || Probe.report(this) == null) return null;
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
        if (isFinishing() || isDestroyed() || fpCompatibilityDialog != null && fpCompatibilityDialog.isShowing()) return;
        String body = FrostpunkDevice.text(this, "intro") + "\n\n"
            + FrostpunkDevice.text(this, "gpu", gpu == null ? "?" : gpu.renderer) + "\n\n" + problem;
        LinearLayout content = new LinearLayout(this); content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(ui.dp(24), ui.dp(20), ui.dp(24), ui.dp(8));
        content.setBackgroundColor(ui.cell);
        content.addView(ui.label(FrostpunkDevice.text(this, "title"), 22, ui.text));
        content.addView(ui.label(body, 16, ui.text));
        android.widget.ScrollView scroll = new android.widget.ScrollView(this); scroll.addView(content);
        fpCompatibilityDialog = ui.show(ui.dialog().setView(scroll).setCancelable(false)
            .setPositiveButton(FrostpunkDevice.text(this, "exit"), (dialog, which) -> finishAndRemoveTask()));
        fpCompatibilityDialog.getWindow().setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(ui.cell));
        ((View)fpCompatibilityDialog.getButton(android.app.AlertDialog.BUTTON_POSITIVE).getParent()).setBackgroundColor(ui.cell);
        fpCompatibilityDialog.setCanceledOnTouchOutside(false);
    }
    private void fpCheckCompatibility() {
        if (gpu == null || !hasWindowFocus() || isFinishing() || isDestroyed() || Work.busy()) return;
        if (!Probe.carried(this)) { fpUnsupported(FrostpunkDevice.text(this, "no_probe")); return; }
        String problem = fpDeviceProblem(selected);
        if (problem != null) fpUnsupported(problem);
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
    }
    private void fpPlay(Spec.Distribution distribution) {
        if (gpu == null || Probe.report(this) == null) {
            if (!Probe.carried(this)) { fpUnsupported(FrostpunkDevice.text(this, "no_probe")); return; }
            checkRenderer(); setBusy(Texts.PLAY, Texts.t(this, Texts.R_CHECKING));
            handler.postDelayed(() -> { if (!isFinishing() && !isDestroyed()) { setBusy(null); play(distribution); } }, PROBE_POLL_MS);
            return;
        }
        String problem = fpDeviceProblem(distribution);
        if (problem != null) { fpUnsupported(problem); return; }
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
