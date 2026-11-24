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

    private void fpMemoryOption(LinearLayout card, Spec.Option option) {
        String title = Texts.of(this, option.title);
        int current = option.choices.indexOf(Launch.option(this, option));
        if (current < 0) current = option.choices.indexOf(option.value);
        String[] labels = new String[option.choices.size()];
        for (int i = 0; i < labels.length; ++i) labels[i] = Texts.of(this, option.choiceTitles.get(i));
        String label = current < 0 ? title : title + ": " + labels[current];
        TextView button = ui.button(label, Ui.QUIET, () -> ui.show(ui.dialog().setTitle(title)
            .setItems(labels, (dialog, which) -> {
                State.put(this, State.CHOICES, "option." + option.key, option.choices.get(which));
                refresh(Texts.OPTIONS);
            })));
        button.setTooltipText(Texts.of(this, option.hint));
        card.addView(ui.buttons(button));
    }

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

    /** The driver workarounds of the renderer a start would use: switches where the player may decide,
     *  notes where the renderer always applies them. The game reads PORT_OPTION_<KEY> and MESA_GRALLOC_LINEAR. */
    private void fpWorkarounds(LinearLayout card, Spec.Mode mode, java.util.Map<String, String> report) {
        List<FrostpunkDevice.Workaround> list = FrostpunkDevice.workarounds(this, mode, report);
        if (list.isEmpty()) return;
        card.addView(ui.label(FrostpunkDevice.tr(this, "Driver workarounds", "Обходы проблем драйвера"), 14, ui.text));
        for (FrostpunkDevice.Workaround item : list) {
            if (!item.toggle) {
                card.addView(ui.label("• " + item.title + ". " + item.hint, 12, ui.muted));
                continue;
            }
            View control = ui.toggle(item.title, item.hint, item.on, on -> {
                State.put(this, State.CHOICES, "option." + item.key, on ? "1" : "0");
            });
            Ui.enable(control, idle());
            card.addView(control);
        }
    }

    private String fpDeviceProblem(Spec.Distribution distribution) {
        if (gpu == null || initializing || !ready(distribution)) return null;
        if (!Probe.carried(this)) return FrostpunkDevice.text(this, "no_probe");
        if (Probe.report(this) == null) return null;
        List<Renderers.Status> statuses = rendererStatuses(distribution);
        for (Renderers.Status status : statuses) if (status.available) return null;
        return statuses.isEmpty() ? FrostpunkDevice.text(this, "no_renderer")
            : FrostpunkDevice.tr(this, "No renderer of this build can run on this device. The Renderer card names the reason for each.",
                "Ни один рендерер этой сборки не может работать на этом устройстве. Причина для каждого указана в карточке «Отрисовка».");
    }

    /* Resource packs and the mounted ZIP are inspected on a worker; the card shows the last snapshot. */
    private String fpResourceKey;
    private List<String> fpResourceNotes, fpMissingPacks;
    private boolean fpResourceScanning;
    private int fpResourceEpoch;

    private void fpResourceWarnings() {
        if (initializing || selected == null || !ready(selected)) return;
        Spec.Distribution distribution = selected;
        File game = Install.folder(this, spec, distribution, distribution.part("game"));
        String key = distribution.id + "\n" + game + "\n" + Install.mount(this, distribution, distribution.part("game"))
            + "\n" + fpResourceEpoch + "\n" + Texts.language(this);
        if (!key.equals(fpResourceKey)) { fpResourceKey = key; fpResourceNotes = null; fpMissingPacks = null; }
        if (fpResourceNotes == null) {
            if (fpResourceScanning) return;
            fpResourceScanning = true;
            List<String> missing = new ArrayList<>();
            background(() -> {
                List<String> notes = new ArrayList<>();
                try { FrostpunkFiles.launcher(this, game, distribution); }
                catch (java.io.IOException failure) { notes.add(String.valueOf(failure.getMessage())); }
                for (String kind : FrostpunkData.KINDS) {
                    boolean valid = State.get(this, "resources", kind + ".error", "").isEmpty();
                    boolean present = FrostpunkData.packagePresent(this, game, kind);
                    if (!present) missing.add(kind);
                    if (!FrostpunkData.installed(this, game, kind) && !(valid && present))
                        notes.add(kind.equals("port-ui")
                            ? FrostpunkDevice.tr(this, "Port UI data is missing or incompatible. Interface scale is limited to 100%.", "Данные адаптации интерфейса отсутствуют или несовместимы. Масштаб интерфейса ограничен 100%.")
                            : FrostpunkDevice.tr(this, "Switch UI data is missing or incompatible. The Switch interface cannot be enabled.", "Данные интерфейса Switch отсутствуют или несовместимы. Включить интерфейс Switch нельзя."));
                }
                return notes;
            }, notes -> {
                fpResourceScanning = false;
                if (key.equals(fpResourceKey)) { fpResourceNotes = notes; fpMissingPacks = missing; }
                if (!isFinishing() && !isDestroyed()) refresh(Texts.GAME_FILES);
            }, failure -> {
                fpResourceScanning = false;
                if (key.equals(fpResourceKey)) {
                    fpResourceNotes = new ArrayList<>(java.util.Collections.singletonList(String.valueOf(failure.getMessage())));
                    fpMissingPacks = new ArrayList<>();
                }
                if (!isFinishing() && !isDestroyed()) refresh(Texts.GAME_FILES);
            });
            return;
        }
        if (!fpResourceNotes.isEmpty()) {
            LinearLayout card = ui.card(files);
            for (String warning : fpResourceNotes) card.addView(ui.label(warning, 13, ui.error));
        }
        fpOfferResourcePacks(game, fpMissingPacks);
    }

    /** Frostpunk asks for a conversion itself (fpPlay): a driver without DXT cannot show the game at all,
     *  so the shared launcher's optional first-start offer would only repeat the question. */
    private boolean fpOwnTextureOffer() { return true; }

    /** The start of an edition after the shared checks: game data, texture cache, then the renderer. */
    private boolean fpPlay(Spec.Distribution distribution, String renderer) {
        java.util.Map<String, String> report = Probe.report(this);
        if (report == null) {
            note(Texts.PLAY, Probe.carried(this) ? Texts.t(this, Texts.R_CHECKING) : FrostpunkDevice.text(this, "no_probe"), true);
            if (Probe.carried(this)) checkRenderer();
            return false;
        }
        File game = Install.folder(this, spec, distribution, distribution.part("game"));
        if (fpDataPlay != distribution) {
            fpDataPlay = distribution;
            setBusy(Texts.PLAY, FrostpunkDevice.tr(this, "Checking game files…", "Проверка файлов игры…"));
            background(() -> { FrostpunkFiles.launcher(this, game, distribution); return new FrostpunkData(this, game); }, job -> {
                setBusy(null);
                startJob(job);
            }, failure -> {
                setBusy(null);
                fpDataPlay = null;
                note(Texts.GAME_FILES, String.valueOf(failure.getMessage()), true);
            });
            return true;
        }
        fpDataPlay = null;
        Spec.Mode mode = spec.mode(renderer);
        FrostpunkTextures backend = new FrostpunkTextures();
        boolean reduced = fpReducedTextureQuality(); backend.quality(reduced);
        File root = Install.folder(this, spec, distribution, distribution.parts.get(0));
        setBusy(Texts.PLAY, FrostpunkDevice.tr(this, "Checking game textures…", "Проверка текстур игры…"));
        background(() -> {
            TextureBackend.Scan scan = backend.scan(root);
            boolean covers = scan.failure != null || scan.converted == 0 || FrostpunkTextures.coversQuality(root, reduced);
            return new Object[] {scan, covers};
        }, found -> {
            setBusy(null);
            TextureBackend.Scan scan = (TextureBackend.Scan) found[0];
            if (scan.failure != null) { note(Texts.TEXTURES, scan.failure, true); return; }
            boolean dxt = FrostpunkDevice.dxt(report, mode);
            boolean astc = FrostpunkDevice.astc(report, mode) && !spec.astcFormats.isEmpty();
            boolean etc2 = FrostpunkDevice.etc2(report, mode) && !spec.etc2Formats.isEmpty();
            String title = rendererTitle(mode);
            if (scan.converted != 0) {
                boolean readable = scan.converted == TextureBackend.ASTC ? astc : etc2;
                String name = scan.converted == TextureBackend.ASTC ? "ASTC" : "ETC2";
                if (!readable) {
                    note(Texts.PLAY, String.format(FrostpunkDevice.tr(this,
                        "The textures are converted to %1$s, which %2$s cannot read. Restore the originals in the Textures card, or choose another renderer.",
                        "Текстуры сконвертированы в %1$s, а %2$s этот формат не читает. Верните оригиналы в карточке «Текстуры» или выберите другой рендерер."),
                        name, title), true);
                    return;
                }
                if (!(Boolean) found[1]) {
                    ui.show(ui.dialog().setTitle(FrostpunkDevice.tr(this, "Texture quality is not ready", "Текстур нужного качества нет"))
                        .setMessage(dxt ? FrostpunkDevice.tr(this,
                            "The existing conversion only has Switch mips. Restore the originals and start the game?",
                            "Готовый набор содержит только mip-уровни Switch. Вернуть оригиналы и запустить игру?")
                            : FrostpunkDevice.tr(this,
                            "The existing conversion only has Switch mips. Restore the originals and convert at PC quality? The source ZIP is kept.",
                            "Готовый набор содержит только mip-уровни Switch. Вернуть оригиналы и сконвертировать в качестве ПК? Исходный ZIP сохранится."))
                        .setPositiveButton(FrostpunkDevice.tr(this, "Prepare and play", "Подготовить и играть"), (dialog, which) -> {
                            textures = backend; fpTexturePlay = distribution; fpTextureRebuild = !dxt;
                            startTextures(dxt ? 0 : scan.converted);
                        }).setNegativeButton(Texts.t(this, Texts.CANCEL), null));
                    return;
                }
                fpStart(distribution, renderer);
                return;
            }
            if (scan.textures == 0 || dxt) { fpStart(distribution, renderer); return; }
            if (!astc && !etc2) { note(Texts.PLAY, FrostpunkDevice.text(this, "no_texture_format"), true); return; }
            String body = String.format(FrostpunkDevice.tr(this,
                "%s cannot read the game's DXT textures, so they must be converted once. The game then loads faster and uses less memory. Original files are kept, and the converted set can be removed in the Textures card. Unneeded top mips are skipped at Switch quality. The game starts after the conversion.",
                "%s не читает DXT-текстуры игры, поэтому их нужно один раз сконвертировать. После этого игра загружается быстрее и расходует меньше памяти. Исходные файлы сохранятся, а готовый набор можно удалить в карточке «Текстуры». Для качества Switch ненужные верхние mip-уровни пропускаются. После конвертации игра запустится."), title);
            body += "\n\n" + Texts.t(this, Texts.TEX_COMPARE);
            body += "\n\n" + fpTextureQualityTitle(reduced);
            body += "\n" + FrostpunkDevice.tr(this, "Additional space: ", "Дополнительное место: ") + Texts.size(this, backend.outputBytes());
            if (notificationsUnasked()) body += "\n\n" + Texts.t(this, Texts.PERM_NOTIFY);
            android.app.AlertDialog.Builder dialog = ui.dialog().setTitle(Texts.t(this, Texts.TEXTURES)).setMessage(body)
                .setNegativeButton(Texts.t(this, Texts.CANCEL), null);
            if (astc) dialog.setPositiveButton(Texts.fmt(this, Texts.TEX_CONVERT, "ASTC"), (dlg, which) -> {
                textures = backend; fpTexturePlay = distribution; startTextures(TextureBackend.ASTC);
            });
            if (etc2) dialog.setNeutralButton(Texts.fmt(this, Texts.TEX_CONVERT, "ETC2"), (dlg, which) -> {
                textures = backend; fpTexturePlay = distribution; startTextures(TextureBackend.ETC2);
            });
            ui.show(dialog);
        }, failure -> { setBusy(null); note(Texts.TEXTURES, String.valueOf(failure.getMessage()), true); });
        return true;
    }

    private void fpStart(Spec.Distribution distribution, String renderer) {
        Spec.Mode chosen = spec.mode(renderer);
        if (chosen != null && !FrostpunkDevice.nativeMode(chosen)
                && !"1".equals(State.get(this, State.CHOICES, FrostpunkHooks.ASKED_NATIVE, ""))) {
            String nativeMode = FrostpunkHooks.nativeTwin(renderer);
            Renderers.Status nativeStatus = Renderers.status(rendererStatuses(distribution), nativeMode);
            if (nativeStatus != null && nativeStatus.available && nativeStatus.verified) {
                ui.show(ui.dialog().setTitle(FrostpunkDevice.tr(this, "Try Native Vulkan?", "Попробовать нативный Vulkan?"))
                    .setMessage(FrostpunkDevice.tr(this,
                        "Native Vulkan is experimental. You can change the renderer in the launcher.",
                        "Нативный Vulkan работает в экспериментальном режиме. Рендерер можно сменить в лаунчере."))
                    .setPositiveButton(FrostpunkDevice.tr(this, "Use Native Vulkan", "Включить нативный Vulkan"), (dialog, which) -> {
                        Renderers.choose(this, distribution, hooks, nativeMode);
                        State.put(this, State.CHOICES, FrostpunkHooks.ASKED_NATIVE, "1");
                        fpLaunch(distribution, nativeMode);
                    }).setNegativeButton(FrostpunkDevice.tr(this, "Keep DXVK", "Оставить DXVK"), (dialog, which) -> {
                        State.put(this, State.CHOICES, FrostpunkHooks.ASKED_NATIVE, "1");
                        fpLaunch(distribution, renderer);
                    }));
                return;
            }
        }
        fpLaunch(distribution, renderer);
    }

    private void fpLaunch(Spec.Distribution distribution, String renderer) {
        try {
            Launch.start(this, distribution, renderer, hooks);
            finishAfterLaunch = true;
        } catch (ActivityNotFoundException failure) {
            note(Texts.PLAY, Texts.t(this, Texts.NOT_IN_BUILD), true);
        }
    }

    /** After any finished job: the data check and a conversion started by Play go on to the game. */
    private void fpFinished(Work.Job job) {
        ++fpResourceEpoch;
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

    private void fpOfferResourcePacks(File game, List<String> missing) {
        if (fpPacksOffered || spec == null || !spec.updates || missing == null || missing.isEmpty() || !idle()) return;
        fpPacksOffered = true;
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
