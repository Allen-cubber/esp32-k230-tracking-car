package com.trackingcar.dashboard;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.ConnectivityManager;
import android.net.NetworkInfo;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.util.Base64;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.EditText;
import android.widget.GridLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Locale;
import java.util.Random;

public class MainActivity extends Activity {
    private static final String PREFS = "dashboard_native";
    private static final String KEY_WS_HOST = "ws_host";
    private static final String KEY_WS_PORT = "ws_port";
    private static final String DEFAULT_WS_HOST = "10.0.2.2";
    private static final String DEFAULT_WS_PORT = "8765";

    private final Handler ui = new Handler(Looper.getMainLooper());
    private SimpleWebSocketClient wsClient;

    private TextView backendText;
    private TextView wsBadge;
    private TextView mqttBadge;
    private TextView espBadge;
    private TextView stateValue;
    private TextView modeValue;
    private TextView distanceValue;
    private TextView leftSpeedValue;
    private TextView rightSpeedValue;
    private TextView yawValue;
    private TextView panValue;
    private TextView tiltValue;
    private TextView headingValue;
    private TextView turnValue;
    private TextView pidAckValue;
    private TextView eventLog;
    private EditText pidKp;
    private EditText pidKi;
    private EditText pidKd;
    private EditText pidTarget;
    private EditText pidIntegral;
    private EditText pidCorrection;
    private EditText pidMinDuty;
    private TrackView trackView;

    private String baseTopic = "";
    private long lastStatusAt = 0L;
    private int packetCount = 0;
    private int rootBaseBottomPadding;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        configureWindow();
        buildUi();
        connectAll();
    }

    @Override
    protected void onDestroy() {
        disconnectAll();
        super.onDestroy();
    }

    private void configureWindow() {
        Window window = getWindow();
        window.setStatusBarColor(color(14, 17, 22));
        window.setNavigationBarColor(color(14, 17, 22));
    }

    private void buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(color(14, 17, 22));
        rootBaseBottomPadding = dp(8);
        root.setPadding(0, 0, 0, rootBaseBottomPadding);
        applySystemInsets(root);

        root.addView(buildHeader(), new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
        ));

        ScrollView scroll = new ScrollView(this);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(12), dp(10), dp(12), dp(18));
        scroll.addView(content, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
        ));

        content.addView(buildStatusGrid());
        content.addView(buildPoseCard());
        content.addView(buildControlCard());
        content.addView(buildPidCard());
        content.addView(buildLogCard());

        root.addView(scroll, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                0,
                1
        ));
        setContentView(root);
    }

    private View buildHeader() {
        LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.VERTICAL);
        header.setPadding(dp(16), dp(12), dp(16), dp(10));
        header.setBackgroundColor(color(18, 23, 30));

        LinearLayout top = new LinearLayout(this);
        top.setOrientation(LinearLayout.HORIZONTAL);
        top.setGravity(Gravity.CENTER_VERTICAL);

        LinearLayout titleGroup = new LinearLayout(this);
        titleGroup.setOrientation(LinearLayout.VERTICAL);

        TextView title = text("\u5c0f\u8f66\u539f\u751f\u63a7\u5236\u53f0", 20, Color.WHITE, true);
        backendText = text("", 12, color(155, 168, 181), false);
        backendText.setSingleLine(true);
        titleGroup.addView(title);
        titleGroup.addView(backendText);
        top.addView(titleGroup, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));

        Button refresh = headerButton("\u5237\u65b0");
        refresh.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                connectAll();
            }
        });
        top.addView(refresh, new LinearLayout.LayoutParams(dp(60), dp(40)));

        Button settings = headerButton("\u8bbe\u7f6e");
        settings.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showSettingsDialog();
            }
        });
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(dp(60), dp(40));
        p.leftMargin = dp(8);
        top.addView(settings, p);

        LinearLayout badges = new LinearLayout(this);
        badges.setOrientation(LinearLayout.HORIZONTAL);
        badges.setPadding(0, dp(10), 0, 0);
        wsBadge = badge("WebSocket offline", false);
        mqttBadge = badge("MQTT offline", false);
        espBadge = badge("ESP32 offline", false);
        badges.addView(wsBadge);
        badges.addView(mqttBadge);
        badges.addView(espBadge);

        header.addView(top);
        header.addView(badges);
        return header;
    }

    private View buildStatusGrid() {
        GridLayout grid = new GridLayout(this);
        grid.setColumnCount(2);
        grid.setPadding(0, dp(2), 0, 0);

        stateValue = metric(grid, "\u8f66\u8f86\u72b6\u6001", "--");
        distanceValue = metric(grid, "\u8d85\u58f0\u6ce2\u8ddd\u79bb", "-- cm");
        leftSpeedValue = metric(grid, "\u5de6\u8f6e\u901f\u5ea6", "-- pps");
        rightSpeedValue = metric(grid, "\u53f3\u8f6e\u901f\u5ea6", "-- pps");
        modeValue = metric(grid, "\u6a21\u5f0f", "--");

        return grid;
    }

    private View buildPoseCard() {
        LinearLayout card = card("\u89d2\u5ea6\u4e0e\u8ffd\u8e2a");
        GridLayout grid = new GridLayout(this);
        grid.setColumnCount(2);
        yawValue = metric(grid, "Yaw", "-- deg");
        panValue = metric(grid, "Pan", "-- deg");
        tiltValue = metric(grid, "Tilt", "-- deg");
        headingValue = metric(grid, "Heading Err", "-- deg");
        turnValue = metric(grid, "Turn Output", "--");
        card.addView(grid);

        trackView = new TrackView(this);
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                dp(180)
        );
        p.topMargin = dp(10);
        card.addView(trackView, p);
        return card;
    }

    private View buildControlCard() {
        LinearLayout card = card("\u5feb\u6377\u63a7\u5236");
        GridLayout grid = new GridLayout(this);
        grid.setColumnCount(3);
        addActionButton(grid, "\u524d\u8fdb", "forward", "chassis");
        addActionButton(grid, "\u5de6\u8f6c", "turn_left", "chassis");
        addActionButton(grid, "\u6025\u505c", "stop", "chassis");
        addActionButton(grid, "\u53f3\u8f6c", "turn_right", "chassis");
        addActionButton(grid, "\u540e\u9000", "backward", "chassis");
        addActionButton(grid, "\u6062\u590d", "resume_follow", "mode");
        card.addView(grid);

        Button center = fullButton("\u4e91\u53f0\u56de\u4e2d");
        center.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                sendGimbalCenter();
            }
        });
        card.addView(center);
        return card;
    }

    private View buildPidCard() {
        LinearLayout card = card("\u5e95\u76d8 PID");
        pidAckValue = text("\u7b49\u5f85\u540c\u6b65", 13, color(155, 168, 181), false);
        card.addView(pidAckValue);

        GridLayout grid = new GridLayout(this);
        grid.setColumnCount(2);
        pidKp = pidInput(grid, "Kp", "0.65");
        pidKi = pidInput(grid, "Ki", "0.18");
        pidKd = pidInput(grid, "Kd", "0");
        pidTarget = pidInput(grid, "\u76ee\u6807 PPS", "3000");
        pidIntegral = pidInput(grid, "\u79ef\u5206\u9650\u5e45", "4000");
        pidCorrection = pidInput(grid, "\u4fee\u6b63\u9650\u5e45", "2200");
        pidMinDuty = pidInput(grid, "\u6700\u5c0f PWM", "500");
        card.addView(grid);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        Button apply = fullButton("\u5e94\u7528 PID");
        apply.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                sendPid();
            }
        });
        Button defaults = fullButton("\u6062\u590d\u9ed8\u8ba4");
        defaults.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                setDefaultPidInputs();
                sendPid();
            }
        });
        row.addView(apply, new LinearLayout.LayoutParams(0, dp(48), 1));
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(0, dp(48), 1);
        p.leftMargin = dp(8);
        row.addView(defaults, p);
        card.addView(row);
        return card;
    }

    private View buildLogCard() {
        LinearLayout card = card("\u4e8b\u4ef6\u6d41");
        eventLog = text("--", 12, color(203, 213, 225), false);
        eventLog.setTypeface(Typeface.MONOSPACE);
        card.addView(eventLog);
        return card;
    }

    private void connectAll() {
        disconnectAll();
        String host = getPref(KEY_WS_HOST, DEFAULT_WS_HOST);
        String port = getPref(KEY_WS_PORT, DEFAULT_WS_PORT);
        backendText.setText("\u540e\u53f0 " + host + ":" + port);
        setBadge(wsBadge, false, "WebSocket connecting");
        setBadge(mqttBadge, false, "MQTT offline");
        setBadge(espBadge, false, "ESP32 offline");

        if (!isNetworkAvailable()) {
            Toast.makeText(this, "\u5f53\u524d\u7f51\u7edc\u4e0d\u53ef\u7528", Toast.LENGTH_LONG).show();
        }

        wsClient = new SimpleWebSocketClient(host, parsePort(port), new SimpleWebSocketClient.Listener() {
            @Override
            public void onOpen() {
                ui.post(new Runnable() {
                    @Override
                    public void run() {
                        setBadge(wsBadge, true, "WebSocket online");
                    }
                });
            }

            @Override
            public void onMessage(final String message) {
                ui.post(new Runnable() {
                    @Override
                    public void run() {
                        handleWsMessage(message);
                    }
                });
            }

            @Override
            public void onClosed(final String error) {
                ui.post(new Runnable() {
                    @Override
                    public void run() {
                        setBadge(wsBadge, false, "WebSocket offline");
                        appendLog("ws closed " + error);
                    }
                });
            }
        });
        wsClient.start();
    }

    private void disconnectAll() {
        if (wsClient != null) {
            wsClient.close();
            wsClient = null;
        }
    }

    private void handleWsMessage(String text) {
        try {
            JSONObject packet = new JSONObject(text);
            String type = packet.optString("type", "");
            if ("hello".equals(type)) {
                baseTopic = packet.optString("base_topic", "");
                setBadge(mqttBadge, packet.optBoolean("connected", false),
                        packet.optBoolean("connected", false) ? "MQTT online" : "MQTT offline");
                JSONArray latest = packet.optJSONArray("latest");
                if (latest != null) {
                    for (int i = 0; i < latest.length(); i++) {
                        handlePacket(latest.optJSONObject(i));
                    }
                }
                return;
            }
            if ("broker".equals(type)) {
                boolean connected = packet.optBoolean("connected", false);
                setBadge(mqttBadge, connected, connected ? "MQTT online" : "MQTT offline");
                return;
            }
            handlePacket(packet);
        } catch (JSONException exc) {
            appendLog("bad json " + exc.getMessage());
        }
    }

    private void handlePacket(JSONObject packet) {
        if (packet == null || !"mqtt".equals(packet.optString("type", ""))) {
            return;
        }
        String suffix = packet.optString("suffix", "");
        JSONObject payload = packet.optJSONObject("payload");
        if (payload == null) {
            appendLog(suffix + " " + packet.opt("payload"));
            return;
        }
        if ("status".equals(suffix)) {
            updateStatus(payload);
        } else if ("track".equals(suffix)) {
            trackView.updateTrack(payload);
        } else if ("cmd/ack".equals(suffix)) {
            boolean ok = payload.optBoolean("ok", false);
            if ("set_pid".equals(payload.optString("cmd", ""))) {
                pidAckValue.setText(ok ? "\u5df2\u4e0b\u53d1\uff0c\u7b49 ESP32 \u786e\u8ba4" :
                        "\u5931\u8d25 " + payload.optString("error", ""));
            }
        }
        appendLog(suffix + " " + payload.toString());
    }

    private void updateStatus(JSONObject status) {
        lastStatusAt = System.currentTimeMillis();
        stateValue.setText(status.optString("state", "--"));
        modeValue.setText((status.optBoolean("manual_mode", false) ? "\u624b\u52a8\u4fdd\u6301" : "\u81ea\u52a8\u8ddf\u968f") +
                " / " + (status.optBoolean("obstacle_hold", false) ? "\u907f\u969c" : "\u901a\u884c"));

        JSONObject ultrasonic = status.optJSONObject("ultrasonic");
        JSONObject motion = status.optJSONObject("motion");
        JSONObject mpu = status.optJSONObject("mpu");
        JSONObject track = status.optJSONObject("track");
        JSONObject control = status.optJSONObject("control");
        JSONObject pid = status.optJSONObject("pid");

        distanceValue.setText(fmt(objDouble(ultrasonic, "distance_cm"), 1) + " cm");
        leftSpeedValue.setText(fmt(objDouble(motion, "left_measured_pps"), 0) + " pps");
        rightSpeedValue.setText(fmt(objDouble(motion, "right_measured_pps"), 0) + " pps");
        yawValue.setText(fmt(objDouble(mpu, "yaw_deg"), 1) + " deg");
        panValue.setText(fmt(objDouble(track, "pan_deg"), 1) + " deg");
        tiltValue.setText(fmt(objDouble(track, "tilt_deg"), 1) + " deg");
        headingValue.setText(fmt(objDouble(control, "heading_error_deg"), 1) + " deg");
        turnValue.setText(fmt(objDouble(control, "turn_output"), 0));
        trackView.updateStatus(status);

        if (pid != null && !isPidInputFocused()) {
            setText(pidKp, fmt(pid.optDouble("kp", 0.65), 3));
            setText(pidKi, fmt(pid.optDouble("ki", 0.18), 3));
            setText(pidKd, fmt(pid.optDouble("kd", 0.0), 3));
            setText(pidTarget, fmt(pid.optDouble("target_max_pps", 3000), 0));
            setText(pidIntegral, fmt(pid.optDouble("integral_limit", 4000), 0));
            setText(pidCorrection, fmt(pid.optDouble("correction_limit", 2200), 0));
            setText(pidMinDuty, fmt(pid.optDouble("min_duty", 500), 0));
        }
        setBadge(espBadge, true, "ESP32 online");
    }

    private void sendAction(String name, String target) {
        try {
            JSONObject action = new JSONObject();
            action.put("target", target);
            action.put("name", name);
            action.put("speed", 35);
            action.put("duration_ms", "stop".equals(name) ? 0 : 700);
            action.put("pan_delta_deg", 0);
            action.put("tilt_delta_deg", 0);

            JSONObject payload = new JSONObject();
            payload.put("emotion", "stop".equals(name) ? "alert" : "neutral");
            payload.put("action", action);
            payload.put("voice", new JSONObject().put("text", "android-native"));

            send(new JSONObject().put("type", "execute_action").put("payload", payload));
        } catch (JSONException ignored) {
        }
    }

    private void sendGimbalCenter() {
        try {
            send(new JSONObject()
                    .put("type", "gimbal")
                    .put("action", "center")
                    .put("duration_ms", 800));
        } catch (JSONException ignored) {
        }
    }

    private void sendPid() {
        try {
            JSONObject payload = new JSONObject();
            payload.put("kp", number(pidKp, 0.65));
            payload.put("ki", number(pidKi, 0.18));
            payload.put("kd", number(pidKd, 0.0));
            payload.put("target_max_pps", number(pidTarget, 3000));
            payload.put("integral_limit", number(pidIntegral, 4000));
            payload.put("correction_limit", number(pidCorrection, 2200));
            payload.put("min_duty", number(pidMinDuty, 500));
            send(new JSONObject().put("type", "set_pid").put("payload", payload));
            pidAckValue.setText("\u6b63\u5728\u4e0b\u53d1...");
        } catch (JSONException ignored) {
        }
    }

    private void send(JSONObject packet) {
        if (wsClient != null) {
            wsClient.send(packet.toString());
        }
    }

    private void showSettingsDialog() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(4), 0, dp(4), 0);
        final EditText host = dialogInput("\u540e\u53f0 IP", getPref(KEY_WS_HOST, DEFAULT_WS_HOST));
        final EditText port = dialogInput("\u540e\u53f0 WebSocket \u7aef\u53e3", getPref(KEY_WS_PORT, DEFAULT_WS_PORT));
        box.addView(host);
        box.addView(port);

        new AlertDialog.Builder(this)
                .setTitle("\u540e\u53f0\u548c\u6444\u50cf\u5934")
                .setMessage("\u6a21\u62df\u5668\u8bbf\u95ee\u7535\u8111\u7528 10.0.2.2\uff1b\u771f\u624b\u673a\u586b\u7535\u8111\u6216\u6811\u8393\u6d3e\u5c40\u57df\u7f51 IP\u3002")
                .setView(box)
                .setNegativeButton("\u53d6\u6d88", null)
                .setNeutralButton("\u6811\u8393\u6d3e", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        saveSettings("10.179.231.220", DEFAULT_WS_PORT);
                    }
                })
                .setPositiveButton("\u8fde\u63a5", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        saveSettings(host.getText().toString(), port.getText().toString());
                    }
                })
                .show();
    }

    private void saveSettings(String host, String port) {
        host = sanitizeHost(host);
        if (host.length() == 0) {
            host = DEFAULT_WS_HOST;
        }
        if (port == null || port.trim().length() == 0) {
            port = DEFAULT_WS_PORT;
        }
        getPreferences().edit()
                .putString(KEY_WS_HOST, host)
                .putString(KEY_WS_PORT, port.trim())
                .apply();
        connectAll();
    }

    private void appendLog(String line) {
        packetCount += 1;
        String old = eventLog.getText().toString();
        String next = "[" + packetCount + "] " + line + "\n" + old;
        if (next.length() > 2400) {
            next = next.substring(0, 2400);
        }
        eventLog.setText(next);
    }

    private void addActionButton(GridLayout grid, String label, final String name, final String target) {
        Button button = fullButton(label);
        button.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                sendAction(name, target);
            }
        });
        GridLayout.LayoutParams p = new GridLayout.LayoutParams();
        p.width = 0;
        p.height = dp(48);
        p.columnSpec = GridLayout.spec(GridLayout.UNDEFINED, 1f);
        p.setMargins(dp(3), dp(3), dp(3), dp(3));
        grid.addView(button, p);
    }

    private TextView metric(GridLayout grid, String label, String value) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(12), dp(10), dp(12), dp(10));
        box.setBackground(makePanelBackground(color(23, 29, 37)));
        TextView l = text(label, 12, color(155, 168, 181), false);
        TextView v = text(value, 20, Color.WHITE, true);
        v.setSingleLine(true);
        box.addView(l);
        box.addView(v);
        GridLayout.LayoutParams p = new GridLayout.LayoutParams();
        p.width = 0;
        p.height = dp(86);
        p.columnSpec = GridLayout.spec(GridLayout.UNDEFINED, 1f);
        p.setMargins(dp(4), dp(4), dp(4), dp(4));
        grid.addView(box, p);
        return v;
    }

    private EditText pidInput(GridLayout grid, String label, String value) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        TextView l = text(label, 12, color(155, 168, 181), false);
        EditText input = new EditText(this);
        input.setSingleLine(true);
        input.setText(value);
        input.setTextSize(16);
        input.setTextColor(Color.WHITE);
        input.setHintTextColor(color(155, 168, 181));
        input.setInputType(InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_DECIMAL | InputType.TYPE_NUMBER_FLAG_SIGNED);
        input.setBackground(makePanelBackground(color(14, 20, 26)));
        input.setPadding(dp(10), 0, dp(10), 0);
        box.addView(l);
        box.addView(input, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(48)));
        GridLayout.LayoutParams p = new GridLayout.LayoutParams();
        p.width = 0;
        p.height = dp(76);
        p.columnSpec = GridLayout.spec(GridLayout.UNDEFINED, 1f);
        p.setMargins(dp(4), dp(4), dp(4), dp(4));
        grid.addView(box, p);
        return input;
    }

    private LinearLayout card(String title) {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(12), dp(12), dp(12), dp(12));
        card.setBackground(makePanelBackground(color(22, 28, 36)));
        LinearLayout.LayoutParams cp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
        );
        cp.setMargins(0, dp(8), 0, dp(8));
        card.setLayoutParams(cp);
        card.addView(text(title, 18, Color.WHITE, true));
        return card;
    }

    private Button headerButton(String label) {
        Button button = new Button(this);
        button.setText(label);
        button.setTextColor(Color.WHITE);
        button.setTextSize(13);
        button.setAllCaps(false);
        button.setBackground(makeButtonBackground());
        return button;
    }

    private Button fullButton(String label) {
        Button button = headerButton(label);
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                dp(48)
        );
        p.topMargin = dp(8);
        button.setLayoutParams(p);
        return button;
    }

    private TextView text(String s, int sp, int c, boolean bold) {
        TextView v = new TextView(this);
        v.setText(s);
        v.setTextSize(sp);
        v.setTextColor(c);
        if (bold) {
            v.setTypeface(Typeface.DEFAULT_BOLD);
        }
        return v;
    }

    private TextView badge(String s, boolean online) {
        TextView b = text(s, 12, online ? color(68, 222, 170) : color(155, 168, 181), false);
        b.setPadding(dp(9), dp(5), dp(9), dp(5));
        b.setBackground(makeBadgeBackground(online));
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
        );
        p.rightMargin = dp(6);
        b.setLayoutParams(p);
        return b;
    }

    private void setBadge(TextView b, boolean online, String label) {
        b.setText(label);
        b.setTextColor(online ? color(68, 222, 170) : color(155, 168, 181));
        b.setBackground(makeBadgeBackground(online));
    }

    private EditText dialogInput(String hint, String value) {
        EditText input = new EditText(this);
        input.setSingleLine(true);
        input.setHint(hint);
        input.setText(value);
        input.setSelectAllOnFocus(true);
        return input;
    }

    private void applySystemInsets(final View root) {
        if (Build.VERSION.SDK_INT < 20) {
            return;
        }
        root.setOnApplyWindowInsetsListener(new View.OnApplyWindowInsetsListener() {
            @Override
            public WindowInsets onApplyWindowInsets(View v, WindowInsets insets) {
                v.setPadding(0, insets.getSystemWindowInsetTop(), 0,
                        rootBaseBottomPadding + insets.getSystemWindowInsetBottom());
                return insets;
            }
        });
    }

    private boolean isPidInputFocused() {
        return pidKp.hasFocus() || pidKi.hasFocus() || pidKd.hasFocus() || pidTarget.hasFocus() ||
                pidIntegral.hasFocus() || pidCorrection.hasFocus() || pidMinDuty.hasFocus();
    }

    private void setDefaultPidInputs() {
        pidKp.setText("0.65");
        pidKi.setText("0.18");
        pidKd.setText("0");
        pidTarget.setText("3000");
        pidIntegral.setText("4000");
        pidCorrection.setText("2200");
        pidMinDuty.setText("500");
    }

    private double number(EditText input, double fallback) {
        try {
            return Double.parseDouble(input.getText().toString().trim());
        } catch (Exception exc) {
            return fallback;
        }
    }

    private double objDouble(JSONObject object, String key) {
        return object == null ? Double.NaN : object.optDouble(key, Double.NaN);
    }

    private String fmt(double value, int digits) {
        if (Double.isNaN(value) || Double.isInfinite(value)) {
            return "--";
        }
        String s = String.format(Locale.US, "%." + digits + "f", value);
        if (digits > 0) {
            while (s.endsWith("0")) {
                s = s.substring(0, s.length() - 1);
            }
            if (s.endsWith(".")) {
                s = s.substring(0, s.length() - 1);
            }
        }
        return s;
    }

    private void setText(EditText input, String value) {
        if (!input.getText().toString().equals(value)) {
            input.setText(value);
        }
    }

    private String getPref(String key, String fallback) {
        return getPreferences().getString(key, fallback);
    }

    private SharedPreferences getPreferences() {
        return getSharedPreferences(PREFS, MODE_PRIVATE);
    }

    private String sanitizeHost(String host) {
        if (host == null) {
            return "";
        }
        host = host.trim().replace("http://", "").replace("https://", "").replace("ws://", "");
        int slash = host.indexOf('/');
        if (slash >= 0) {
            host = host.substring(0, slash);
        }
        int colon = host.indexOf(':');
        if (colon >= 0) {
            host = host.substring(0, colon);
        }
        return host.trim();
    }

    private int parsePort(String port) {
        try {
            return Integer.parseInt(port.trim());
        } catch (Exception exc) {
            return 8765;
        }
    }

    private boolean isNetworkAvailable() {
        ConnectivityManager manager = (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
        if (manager == null) {
            return false;
        }
        NetworkInfo info = manager.getActiveNetworkInfo();
        return info != null && info.isConnected();
    }

    private GradientDrawable makeButtonBackground() {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color(31, 39, 49));
        d.setStroke(dp(1), color(55, 68, 83));
        d.setCornerRadius(dp(10));
        return d;
    }

    private GradientDrawable makePanelBackground(int fill) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(fill);
        d.setStroke(dp(1), color(48, 60, 74));
        d.setCornerRadius(dp(8));
        return d;
    }

    private GradientDrawable makeBadgeBackground(boolean online) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(online ? color(17, 52, 44) : color(26, 33, 42));
        d.setStroke(dp(1), online ? color(31, 115, 91) : color(48, 60, 74));
        d.setCornerRadius(dp(8));
        return d;
    }

    private int color(int r, int g, int b) {
        return Color.rgb(r, g, b);
    }

    private int dp(int value) {
        return (int) (value * getResources().getDisplayMetrics().density + 0.5f);
    }

    public static class TrackView extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private JSONObject track;

        public TrackView(Context context) {
            super(context);
            setBackgroundColor(Color.rgb(16, 21, 27));
        }

        public void updateStatus(JSONObject status) {
            track = status.optJSONObject("track");
            invalidate();
        }

        public void updateTrack(JSONObject payload) {
            track = payload;
            invalidate();
        }

        @Override
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            int w = getWidth();
            int h = getHeight();
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(Color.rgb(16, 21, 27));
            canvas.drawRect(0, 0, w, h, paint);

            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(1f);
            paint.setColor(Color.rgb(38, 48, 58));
            for (int x = 0; x < w; x += Math.max(1, w / 6)) {
                canvas.drawLine(x, 0, x, h, paint);
            }
            for (int y = 0; y < h; y += Math.max(1, h / 4)) {
                canvas.drawLine(0, y, w, y, paint);
            }

            if (track == null) {
                drawCenterText(canvas, "\u7b49\u5f85\u76ee\u6807");
                return;
            }
            double bx = track.optDouble("x", 0.0);
            double by = track.optDouble("y", 0.0);
            double bw = track.optDouble("w", 0.0);
            double bh = track.optDouble("h", 0.0);
            boolean valid = track.optBoolean("valid", false);
            RectF r = new RectF((float) (bx / 480.0 * w),
                    (float) (by / 360.0 * h),
                    (float) ((bx + bw) / 480.0 * w),
                    (float) ((by + bh) / 360.0 * h));
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(4f);
            paint.setColor(valid ? Color.rgb(66, 214, 164) : Color.rgb(255, 92, 122));
            canvas.drawRect(r, paint);
        }

        private void drawCenterText(Canvas canvas, String s) {
            paint.setStyle(Paint.Style.FILL);
            paint.setTextAlign(Paint.Align.CENTER);
            paint.setTextSize(34f);
            paint.setColor(Color.rgb(155, 168, 181));
            canvas.drawText(s, getWidth() / 2f, getHeight() / 2f, paint);
        }
    }

    public static class SimpleWebSocketClient {
        public interface Listener {
            void onOpen();
            void onMessage(String message);
            void onClosed(String error);
        }

        private final String host;
        private final int port;
        private final Listener listener;
        private volatile boolean running;
        private Socket socket;
        private OutputStream output;

        public SimpleWebSocketClient(String host, int port, Listener listener) {
            this.host = host;
            this.port = port;
            this.listener = listener;
        }

        public void start() {
            running = true;
            Thread thread = new Thread(new Runnable() {
                @Override
                public void run() {
                    runClient();
                }
            }, "dashboard-ws");
            thread.setDaemon(true);
            thread.start();
        }

        public void close() {
            running = false;
            try {
                if (socket != null) {
                    socket.close();
                }
            } catch (IOException ignored) {
            }
        }

        public synchronized void send(String text) {
            if (output == null) {
                return;
            }
            try {
                writeFrame(output, text);
                output.flush();
            } catch (IOException ignored) {
            }
        }

        private void runClient() {
            try {
                socket = new Socket();
                socket.connect(new InetSocketAddress(host, port), 2500);
                InputStream input = socket.getInputStream();
                output = socket.getOutputStream();
                doHandshake(input, output);
                listener.onOpen();
                while (running) {
                    String message = readFrame(input);
                    if (message != null) {
                        listener.onMessage(message);
                    }
                }
            } catch (Exception exc) {
                if (running) {
                    listener.onClosed(exc.getClass().getSimpleName());
                }
            } finally {
                running = false;
                close();
            }
        }

        private void doHandshake(InputStream input, OutputStream output) throws Exception {
            byte[] nonce = new byte[16];
            new Random().nextBytes(nonce);
            String key = Base64.encodeToString(nonce, Base64.NO_WRAP);
            String req = "GET / HTTP/1.1\r\n" +
                    "Host: " + host + ":" + port + "\r\n" +
                    "Upgrade: websocket\r\n" +
                    "Connection: Upgrade\r\n" +
                    "Sec-WebSocket-Key: " + key + "\r\n" +
                    "Sec-WebSocket-Version: 13\r\n\r\n";
            output.write(req.getBytes(StandardCharsets.US_ASCII));
            output.flush();

            String headers = readHttpHeaders(input);
            if (!headers.startsWith("HTTP/1.1 101") && !headers.startsWith("HTTP/1.0 101")) {
                throw new IOException("websocket handshake failed");
            }
            String expected = acceptKey(key);
            if (!headers.toLowerCase(Locale.US).contains(expected.toLowerCase(Locale.US))) {
                throw new IOException("websocket accept mismatch");
            }
        }

        private String readHttpHeaders(InputStream input) throws IOException {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            int b;
            int matched = 0;
            byte[] end = new byte[] {'\r', '\n', '\r', '\n'};
            while ((b = input.read()) != -1) {
                out.write(b);
                matched = b == end[matched] ? matched + 1 : 0;
                if (matched == end.length) {
                    break;
                }
            }
            return new String(out.toByteArray(), StandardCharsets.US_ASCII);
        }

        private String acceptKey(String key) throws Exception {
            String magic = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
            MessageDigest sha1 = MessageDigest.getInstance("SHA-1");
            return Base64.encodeToString(sha1.digest(magic.getBytes(StandardCharsets.US_ASCII)), Base64.NO_WRAP);
        }

        private String readFrame(InputStream input) throws IOException {
            int b0 = input.read();
            int b1 = input.read();
            if (b0 < 0 || b1 < 0) {
                throw new EOFException();
            }
            int opcode = b0 & 0x0F;
            boolean masked = (b1 & 0x80) != 0;
            long len = b1 & 0x7F;
            if (len == 126) {
                len = ((long) readByte(input) << 8) | readByte(input);
            } else if (len == 127) {
                len = 0;
                for (int i = 0; i < 8; i++) {
                    len = (len << 8) | readByte(input);
                }
            }
            byte[] mask = null;
            if (masked) {
                mask = new byte[] {(byte) readByte(input), (byte) readByte(input), (byte) readByte(input), (byte) readByte(input)};
            }
            if (len > 1024 * 1024) {
                throw new IOException("frame too large");
            }
            byte[] payload = new byte[(int) len];
            int off = 0;
            while (off < payload.length) {
                int n = input.read(payload, off, payload.length - off);
                if (n < 0) {
                    throw new EOFException();
                }
                off += n;
            }
            if (masked && mask != null) {
                for (int i = 0; i < payload.length; i++) {
                    payload[i] = (byte) (payload[i] ^ mask[i % 4]);
                }
            }
            if (opcode == 0x8) {
                throw new EOFException();
            }
            if (opcode == 0x1) {
                return new String(payload, StandardCharsets.UTF_8);
            }
            return null;
        }

        private int readByte(InputStream input) throws IOException {
            int b = input.read();
            if (b < 0) {
                throw new EOFException();
            }
            return b & 0xFF;
        }

        private void writeFrame(OutputStream output, String text) throws IOException {
            byte[] payload = text.getBytes(StandardCharsets.UTF_8);
            ByteArrayOutputStream frame = new ByteArrayOutputStream();
            frame.write(0x81);
            if (payload.length < 126) {
                frame.write(0x80 | payload.length);
            } else if (payload.length <= 65535) {
                frame.write(0x80 | 126);
                frame.write((payload.length >> 8) & 0xFF);
                frame.write(payload.length & 0xFF);
            } else {
                frame.write(0x80 | 127);
                frame.write(ByteBuffer.allocate(8).putLong(payload.length).array());
            }
            byte[] mask = new byte[4];
            new Random().nextBytes(mask);
            frame.write(mask);
            for (int i = 0; i < payload.length; i++) {
                frame.write(payload[i] ^ mask[i % 4]);
            }
            output.write(frame.toByteArray());
        }
    }
}
