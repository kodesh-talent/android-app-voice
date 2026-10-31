package com.xeno.voicelatency;

import android.Manifest;
import android.graphics.Color;
import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.Menu;
import android.view.MenuItem;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.TableLayout;
import android.widget.TableRow;
import android.widget.TextView;

import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import com.google.android.material.appbar.MaterialToolbar;
import com.google.android.material.button.MaterialButton;
import com.google.android.material.button.MaterialButtonToggleGroup;
import com.google.android.material.chip.Chip;
import com.google.android.material.dialog.MaterialAlertDialogBuilder;
import com.google.android.material.floatingactionbutton.ExtendedFloatingActionButton;
import com.google.android.material.materialswitch.MaterialSwitch;
import com.google.android.material.progressindicator.LinearProgressIndicator;
import com.google.android.material.slider.Slider;
import com.google.android.material.snackbar.Snackbar;
import com.google.android.material.textfield.MaterialAutoCompleteTextView;
import com.google.android.material.textfield.TextInputEditText;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Main screen: the latency test bench.
 *
 * <p>Every control is a Material 3 component. The screen runs a batch of probes,
 * then shows the headline metric, its breakdown, aggregate statistics and the
 * history. The selected {@link ProbeMode} is the single source of truth for
 * whether a run talks to the gateway or uses the simulated link.
 */
public final class MainActivity extends AppCompatActivity implements ProbeEngine.Listener {

    private static final String[] VOICES = {"Ara", "Rex", "Sal", "Eve", "Leo"};
    private static final String[] LANGUAGES = {"zh", "en"};

    private ProbeSettings settings;
    private ProbeEngine engine;
    private HistoryAdapter historyAdapter;
    private ProbeMode mode = ProbeMode.TTS;
    private final List<Double> samples = new ArrayList<>();

    private MaterialToolbar toolbar;
    private Chip chipMode;
    private Chip chipSource;
    private TextView labelPrimary;
    private TextView valuePrimary;
    private TextView textDiagnosis;
    private LinearProgressIndicator progress;
    private TableLayout tableBreakdown;
    private TextView statAvg;
    private TextView statP95;
    private TextView statJitter;
    private TextView statRange;
    private TextView textModeHint;
    private TextView textDiagnosticDetail;
    private MaterialButton buttonCopyDiagnostic;
    private MaterialButtonToggleGroup toggleMode;
    private ExtendedFloatingActionButton fabRun;

    private final ActivityResultLauncher<String> permissionLauncher =
            registerForActivityResult(new ActivityResultContracts.RequestPermission(), granted -> {
                if (granted) {
                    startBatch();
                } else {
                    snack("未授予录音权限，STT 与实时模式无法采集音频");
                }
            });

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        settings = ProbeSettings.load(this);
        engine = new ProbeEngine(this);

        bindViews();
        configureToolbar();
        configureModeToggle();
        configureHistory();
        configureActions();

        mode = settings.simulated ? ProbeMode.SIMULATED : ProbeMode.TTS;
        checkModeButton(mode);
        renderMode();
        renderEmptyMetrics();
        toolbar.setSubtitle(settings.hostLabel());
    }

    private void bindViews() {
        toolbar = findViewById(R.id.toolbar);
        chipMode = findViewById(R.id.chip_mode);
        chipSource = findViewById(R.id.chip_source);
        labelPrimary = findViewById(R.id.label_primary);
        valuePrimary = findViewById(R.id.value_primary);
        textDiagnosis = findViewById(R.id.text_diagnosis);
        progress = findViewById(R.id.progress);
        tableBreakdown = findViewById(R.id.table_breakdown);
        statAvg = findViewById(R.id.stat_avg);
        statP95 = findViewById(R.id.stat_p95);
        statJitter = findViewById(R.id.stat_jitter);
        statRange = findViewById(R.id.stat_range);
        textModeHint = findViewById(R.id.text_mode_hint);
        textDiagnosticDetail = findViewById(R.id.text_diagnostic_detail);
        buttonCopyDiagnostic = findViewById(R.id.button_copy_diagnostic);
        toggleMode = findViewById(R.id.toggle_mode);
        fabRun = findViewById(R.id.fab_run);
    }

    private void configureToolbar() {
        setSupportActionBar(toolbar);
    }

    private void configureModeToggle() {
        toggleMode.addOnButtonCheckedListener((group, checkedId, isChecked) -> {
            if (!isChecked) {
                return;
            }
            if (checkedId == R.id.mode_stt) {
                mode = ProbeMode.STT;
            } else if (checkedId == R.id.mode_realtime) {
                mode = ProbeMode.REALTIME;
            } else if (checkedId == R.id.mode_simulated) {
                mode = ProbeMode.SIMULATED;
            } else {
                mode = ProbeMode.TTS;
            }
            settings.simulated = mode == ProbeMode.SIMULATED;
            settings.save(this);
            renderMode();
        });
    }

    private void checkModeButton(ProbeMode target) {
        switch (target) {
            case STT:
                toggleMode.check(R.id.mode_stt);
                break;
            case REALTIME:
                toggleMode.check(R.id.mode_realtime);
                break;
            case SIMULATED:
                toggleMode.check(R.id.mode_simulated);
                break;
            case TTS:
            default:
                toggleMode.check(R.id.mode_tts);
                break;
        }
    }

    private void configureHistory() {
        historyAdapter = new HistoryAdapter();
        RecyclerView listHistory = findViewById(R.id.list_history);
        listHistory.setLayoutManager(new LinearLayoutManager(this));
        listHistory.setAdapter(historyAdapter);
    }

    private void configureActions() {
        fabRun.setOnClickListener(v -> {
            if (engine.isRunning()) {
                engine.cancel();
                return;
            }
            startBatch();
        });

        findViewById(R.id.button_clear).setOnClickListener(v -> {
            historyAdapter.clear();
            samples.clear();
            renderEmptyMetrics();
        });

        chipSource.setOnClickListener(v ->
                checkModeButton(mode.usesNetwork() ? ProbeMode.SIMULATED : ProbeMode.TTS));

        buttonCopyDiagnostic.setOnClickListener(v -> {
            String text = textDiagnosticDetail.getText().toString();
            if (text.isEmpty()) {
                return;
            }
            android.content.ClipboardManager clipboard =
                    (android.content.ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
            clipboard.setPrimaryClip(android.content.ClipData.newPlainText(
                    "Voice Latency diagnostic", text));
            snack("全部异常信息已复制");
        });
    }

    private void startBatch() {
        if (mode.needsMicrophone() && !AudioCapture.hasPermission(this)) {
            permissionLauncher.launch(Manifest.permission.RECORD_AUDIO);
            return;
        }
        samples.clear();
        historyAdapter.clear();
        renderEmptyMetrics();
        fabRun.setText(R.string.action_stop);
        progress.setVisibility(View.VISIBLE);
        if (mode == ProbeMode.REALTIME) {
            snack("实时会话已开始：停顿约 0.7 秒自动提交，点“停止”结束");
        }
        int iterations = mode == ProbeMode.REALTIME
                ? 1
                : Math.max(1, settings.iterations);
        engine.runBatch(mode, settings, iterations, this);
    }

    private void renderMode() {
        textModeHint.setText(mode.subtitle);
        chipMode.setText(mode.title);
        if (mode == ProbeMode.STT) {
            labelPrimary.setText("转写延迟 TTFT");
        } else if (mode == ProbeMode.REALTIME) {
            labelPrimary.setText("实时响应首音 TTFA（每轮说话结束后）");
        } else {
            labelPrimary.setText("首音延迟 TTFA");
        }

        boolean simulated = !mode.usesNetwork();
        chipSource.setText(simulated ? "模拟链路" : "真实接口");
        chipSource.setChecked(simulated);
    }
    private void renderEmptyMetrics() {
        valuePrimary.setText("—");
        valuePrimary.setTextColor(themeColor(R.attr.vlColorPrimary));
        textDiagnosis.setVisibility(View.GONE);
        textDiagnosticDetail.setText("");
        textDiagnosticDetail.setVisibility(View.GONE);
        buttonCopyDiagnostic.setVisibility(View.GONE);
        tableBreakdown.removeAllViews();
        statAvg.setText("—");
        statP95.setText("—");
        statJitter.setText("—");
        statRange.setText("—");
    }

    // ------------------------------------------------------------ probe events

    @Override
    public void onProbeStarted(ProbeMode probeMode, int iteration, int total) {
        chipMode.setText(String.format(Locale.US, "%s %d/%d", probeMode.title, iteration, total));
    }

    @Override
    public void onProbeFinished(ProbeResult result, int completed, int total) {
        historyAdapter.add(result);
        // Only successful samples feed the aggregate statistics; a failed call has
        // no meaningful latency and would skew the mean towards zero.
        if (result.ok) {
            samples.add(result.primaryMetricMs());
        }
        renderResult(result);
        renderStats();
    }

    @Override
    public void onBatchFinished(ProbeMode probeMode, List<ProbeResult> results,
                                boolean cancelled) {
        progress.setVisibility(View.GONE);
        fabRun.setText(R.string.action_start);
        chipMode.setText(probeMode.title);
        textModeHint.setText(probeMode.subtitle);

        if (results.isEmpty()) {
            snack("未产生结果");
            return;
        }

        ProbeResult last = results.get(results.size() - 1);
        if (cancelled) {
            snack(probeMode == ProbeMode.REALTIME ? "实时会话已结束" : "测试已停止");
        } else if (!last.ok) {
            if (last.isInsufficientBalance()) {
                showInsufficientBalanceDialog();
            } else {
                snack(last.diagnosis());
            }
        }
    }

    @Override
    public void onStatus(String message) {
        if (message != null && !message.isEmpty()) {
            chipMode.setText(message);
        }
    }

    @Override
    public void onRealtimeState(String state) {
        if (state != null && !state.isEmpty() && engine.isRunning()) {
            textModeHint.setText(state);
        }
    }

    // ------------------------------------------------------------- rendering

    private void renderResult(ProbeResult result) {
        valuePrimary.setText(result.ok
                ? ProbeResult.format(result.primaryMetricMs())
                : "失败");
        valuePrimary.setTextColor(themeColor(result.ok
                ? R.attr.vlColorPrimary
                : R.attr.vlColorError));

        String diagnosis = result.diagnosis();
        if (diagnosis.isEmpty()) {
            textDiagnosis.setVisibility(View.GONE);
        } else {
            textDiagnosis.setText(diagnosis);
            textDiagnosis.setVisibility(View.VISIBLE);
        }

        if (result.ok) {
            textDiagnosticDetail.setText("");
            textDiagnosticDetail.setVisibility(View.GONE);
            buttonCopyDiagnostic.setVisibility(View.GONE);
        } else {
            textDiagnosticDetail.setText(result.diagnosticText());
            textDiagnosticDetail.setVisibility(View.VISIBLE);
            buttonCopyDiagnostic.setVisibility(View.VISIBLE);
        }

        tableBreakdown.removeAllViews();
        for (String[] row : result.detailRows()) {
            TableRow tableRow = new TableRow(this);
            tableRow.setPadding(0, dp(6), 0, dp(6));

            TextView label = new TextView(this);
            label.setText(row[0]);
            label.setTextAppearance(com.google.android.material.R.style
                    .TextAppearance_Material3_BodyMedium);
            label.setTextColor(themeColor(R.attr.vlColorOnSurfaceVariant));

            TextView value = new TextView(this);
            value.setText(row[1]);
            value.setTextAppearance(com.google.android.material.R.style
                    .TextAppearance_Material3_BodyMedium);
            value.setGravity(android.view.Gravity.END);

            tableRow.addView(label, new TableRow.LayoutParams(
                    0, TableRow.LayoutParams.WRAP_CONTENT, 1f));
            tableRow.addView(value, new TableRow.LayoutParams(
                    0, TableRow.LayoutParams.WRAP_CONTENT, 1f));
            tableBreakdown.addView(tableRow);
        }
    }

    private void renderStats() {
        StatSummary summary = StatSummary.of(samples);
        statAvg.setText(summary.formatAvg());
        statP95.setText(summary.formatP95());
        statJitter.setText(summary.formatJitter());
        statRange.setText(summary.count == 0
                ? "—"
                : String.format(Locale.US, "样本 %d · 区间 %s", summary.count,
                summary.formatRange()));
    }

    private void showInsufficientBalanceDialog() {
        new MaterialAlertDialogBuilder(this)
                .setTitle("接口返回 403：账户欠费")
                .setMessage("网关返回 INSUFFICIENT_BALANCE，说明当前 API Key 所属账户余额不足，"
                        + "真实语音链路暂时无法调用。\n\n"
                        + "充值后无需修改代码即可继续测试；在此之前可切换到「模拟链路」"
                        + "验证完整的测量流程。")
                .setPositiveButton("使用模拟链路", (dialog, which) ->
                        checkModeButton(ProbeMode.SIMULATED))
                .setNegativeButton("知道了", null)
                .show();
    }

    // --------------------------------------------------------------- settings

    @Override
    public boolean onCreateOptionsMenu(Menu menu) {
        getMenuInflater().inflate(R.menu.menu_main, menu);
        return true;
    }

    @Override
    public boolean onOptionsItemSelected(@NonNull MenuItem item) {
        if (item.getItemId() == R.id.action_settings) {
            showSettings();
            return true;
        }
        return super.onOptionsItemSelected(item);
    }

    private void showSettings() {
        View content = LayoutInflater.from(this).inflate(R.layout.sheet_settings, null, false);

        TextInputEditText inputBaseUrl = content.findViewById(R.id.input_base_url);
        TextInputEditText inputApiKey = content.findViewById(R.id.input_api_key);
        TextInputEditText inputText = content.findViewById(R.id.input_text);
        MaterialAutoCompleteTextView inputVoice = content.findViewById(R.id.input_voice);
        MaterialAutoCompleteTextView inputLanguage = content.findViewById(R.id.input_language);
        Slider sliderIterations = content.findViewById(R.id.slider_iterations);
        MaterialSwitch switchSimulated = content.findViewById(R.id.switch_simulated);
        TextView endpointHint = content.findViewById(R.id.text_endpoint_hint);

        inputBaseUrl.setText(settings.baseUrl);
        inputApiKey.setText(settings.apiKey);
        inputText.setText(settings.text);

        inputVoice.setAdapter(new ArrayAdapter<>(this,
                android.R.layout.simple_list_item_1, VOICES));
        inputVoice.setText(settings.voice, false);
        inputLanguage.setAdapter(new ArrayAdapter<>(this,
                android.R.layout.simple_list_item_1, LANGUAGES));
        inputLanguage.setText(settings.language, false);

        sliderIterations.setValue(Math.min(20, Math.max(1, settings.iterations)));
        switchSimulated.setChecked(mode == ProbeMode.SIMULATED);

        endpointHint.setText("当前生效地址：" + settings.baseUrl
                + "\n密钥：" + settings.maskedKey()
                + "\n语音模型：grok-voice-tts / grok-voice-stt / grok-voice-realtime");

        new MaterialAlertDialogBuilder(this)
                .setTitle("测试设置")
                .setView(content)
                .setPositiveButton(R.string.action_save, (dialog, which) -> {
                    String url = text(inputBaseUrl);
                    if (!url.isEmpty()) {
                        settings.baseUrl = url;
                    }
                    String key = text(inputApiKey);
                    if (!key.isEmpty()) {
                        settings.apiKey = key;
                    }
                    String voiceText = text(inputText);
                    if (!voiceText.isEmpty()) {
                        settings.text = voiceText;
                    }
                    if (inputVoice.getText() != null) {
                        String voice = inputVoice.getText().toString().trim();
                        if (!voice.isEmpty()) {
                            settings.voice = voice;
                        }
                    }
                    if (inputLanguage.getText() != null) {
                        String language = inputLanguage.getText().toString().trim();
                        if (!language.isEmpty()) {
                            settings.language = language;
                        }
                    }
                    settings.iterations = (int) sliderIterations.getValue();
                    settings.save(this);
                    toolbar.setSubtitle(settings.hostLabel());

                    if (switchSimulated.isChecked() && mode.usesNetwork()) {
                        checkModeButton(ProbeMode.SIMULATED);
                    } else if (!switchSimulated.isChecked() && mode == ProbeMode.SIMULATED) {
                        checkModeButton(ProbeMode.TTS);
                    }
                })
                .setNegativeButton(R.string.action_cancel, null)
                .show();
    }

    private static String text(TextInputEditText field) {
        return field.getText() == null ? "" : field.getText().toString().trim();
    }

    /** Resolves a theme attribute to a colour, falling back to grey. */
    private int themeColor(int attrRes) {
        android.util.TypedValue value = new android.util.TypedValue();
        if (!getTheme().resolveAttribute(attrRes, value, true)) {
            return Color.GRAY;
        }
        return value.data;
    }

    private void snack(String message) {
        Snackbar.make(findViewById(R.id.root), message, Snackbar.LENGTH_LONG)
                .setAction("设置", v -> showSettings())
                .show();
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        engine.shutdown();
    }
}
