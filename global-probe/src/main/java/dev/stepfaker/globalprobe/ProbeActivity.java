package dev.stepfaker.globalprobe;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/** Independent foreground client: no Xposed APIs, module dependency or network. */
public final class ProbeActivity extends Activity implements SensorEventListener, LocationListener {
    private static final String TAG = "GlobalClientProbe";
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable timeout = this::stopSampling;
    private SensorManager sensors;
    private LocationManager locations;
    private TextView text;
    private boolean sampling;
    private int locationEvents;
    private int counterEvents;
    private int detectorEvents;
    private int wakeUpCounterEvents;
    private int nonWakeCounterEvents;
    private int wakeUpDetectorEvents;
    private int nonWakeDetectorEvents;
    private float firstCounter = Float.NaN;
    private float lastCounter = Float.NaN;
    private long started;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        sensors = (SensorManager) getSystemService(SENSOR_SERVICE);
        locations = (LocationManager) getSystemService(LOCATION_SERVICE);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(24, 24, 24, 24);
        Button start = new Button(this);
        start.setText("Start 60-second sample");
        start.setOnClickListener(v -> startSampling());
        layout.addView(start);
        Button stop = new Button(this);
        stop.setText("Stop");
        stop.setOnClickListener(v -> stopSampling());
        layout.addView(stop);
        text = new TextView(this);
        text.setTextIsSelectable(true);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(text);
        layout.addView(scroll);
        setContentView(layout);
        line("Independent system API client; do NOT add to LSPosed scope.");
        line("Coordinates are logged locally. No network permission or synthetic events.");
    }

    @Override public void onResume() {
        super.onResume();
        if (!sampling) startSampling();
    }

    private void startSampling() {
        if (sampling) return;
        ArrayList<String> missing = new ArrayList<>();
        for (String permission : new String[]{Manifest.permission.ACCESS_FINE_LOCATION,
                Manifest.permission.ACCESS_COARSE_LOCATION}) {
            if (checkSelfPermission(permission) != PackageManager.PERMISSION_GRANTED) missing.add(permission);
        }
        if (Build.VERSION.SDK_INT >= 29 && checkSelfPermission(Manifest.permission.ACTIVITY_RECOGNITION)
                != PackageManager.PERMISSION_GRANTED) missing.add(Manifest.permission.ACTIVITY_RECOGNITION);
        if (!missing.isEmpty()) {
            line("Permission required; sampling has not started.");
            requestPermissions(missing.toArray(new String[0]), 1);
            return;
        }
        locationEvents = counterEvents = detectorEvents = 0;
        wakeUpCounterEvents = nonWakeCounterEvents = wakeUpDetectorEvents = nonWakeDetectorEvents = 0;
        firstCounter = lastCounter = Float.NaN;
        started = SystemClock.elapsedRealtime();
        sampling = true;
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        line("START pid=" + Process.myPid() + " uid=" + Process.myUid());
        registerStepSensors(Sensor.TYPE_STEP_COUNTER);
        registerStepSensors(Sensor.TYPE_STEP_DETECTOR);
        for (String provider : new String[]{LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER}) {
            try {
                if (locations == null || !locations.getAllProviders().contains(provider)) {
                    line("LOCATION provider unavailable=" + provider);
                    continue;
                }
                locations.requestLocationUpdates(provider, 1000L, 0F, this, Looper.getMainLooper());
                line("LOCATION listener registered=" + provider);
            } catch (RuntimeException e) {
                line("LOCATION registration failed=" + provider + " " + e.getClass().getSimpleName());
            }
        }
        handler.postDelayed(timeout, 60_000L);
    }

    /**
     * Registers every sensor of this type, not just getDefaultSensor: this ROM publishes a
     * wake-up and a non-wake step counter/detector, and the module must deliver to whichever
     * variant a client actually holds.
     */
    private void registerStepSensors(int type) {
        if (sensors == null) { line("SENSOR unavailable type=" + type); return; }
        List<Sensor> available = sensors.getSensorList(type);
        if (available.isEmpty()) { line("SENSOR unavailable type=" + type); return; }
        for (Sensor sensor : available) {
            try {
                boolean accepted = sensors.registerListener(this, sensor, SensorManager.SENSOR_DELAY_NORMAL, handler);
                line("SENSOR register type=" + type + " wakeUp=" + sensor.isWakeUpSensor()
                        + " accepted=" + accepted + " name=" + sensor.getName());
            } catch (RuntimeException e) {
                line("SENSOR registration failed type=" + type + " wakeUp=" + sensor.isWakeUpSensor()
                        + " " + e.getClass().getSimpleName());
            }
        }
    }

    private void stopSampling() {
        if (!sampling) return;
        sampling = false;
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        handler.removeCallbacks(timeout);
        if (sensors != null) sensors.unregisterListener(this);
        if (locations != null) {
            try { locations.removeUpdates(this); }
            catch (RuntimeException e) { line("LOCATION cleanup failed=" + e.getClass().getSimpleName()); }
        }
        line(String.format(Locale.US,
                "SUMMARY elapsedMs=%d locations=%d counterEvents=%d counterDelta=%s detectorEvents=%d"
                        + " counterNonWake=%d counterWakeUp=%d detectorNonWake=%d detectorWakeUp=%d",
                SystemClock.elapsedRealtime() - started, locationEvents, counterEvents,
                Float.isNaN(firstCounter) ? "unobserved" : Float.toString(lastCounter - firstCounter), detectorEvents,
                nonWakeCounterEvents, wakeUpCounterEvents, nonWakeDetectorEvents, wakeUpDetectorEvents));
    }

    @Override public void onLocationChanged(Location location) {
        if (!sampling) return;
        ++locationEvents;
        line(String.format(Locale.US, "LOCATION provider=%s lat=%.8f lon=%.8f accuracy=%.1f elapsedNs=%d",
                location.getProvider(), location.getLatitude(), location.getLongitude(), location.getAccuracy(),
                location.getElapsedRealtimeNanos()));
    }

    @Override public void onSensorChanged(SensorEvent event) {
        if (!sampling || event.values.length == 0) return;
        final boolean wakeUp = event.sensor.isWakeUpSensor();
        if (event.sensor.getType() == Sensor.TYPE_STEP_COUNTER) {
            ++counterEvents;
            if (wakeUp) ++wakeUpCounterEvents; else ++nonWakeCounterEvents;
            lastCounter = event.values[0];
            if (Float.isNaN(firstCounter)) firstCounter = lastCounter;
        } else if (event.sensor.getType() == Sensor.TYPE_STEP_DETECTOR) {
            ++detectorEvents;
            if (wakeUp) ++wakeUpDetectorEvents; else ++nonWakeDetectorEvents;
        } else return;
        line("SENSOR type=" + event.sensor.getType() + " wakeUp=" + wakeUp + " value=" + event.values[0]
                + " timestamp=" + event.timestamp);
    }

    @Override public void onAccuracyChanged(Sensor sensor, int accuracy) {}
    @Override public void onProviderEnabled(String provider) { line("PROVIDER enabled=" + provider); }
    @Override public void onProviderDisabled(String provider) { line("PROVIDER disabled=" + provider); }
    @Override public void onStatusChanged(String provider, int status, Bundle extras) {}

    @Override public void onRequestPermissionsResult(int request, String[] permissions, int[] grants) {
        super.onRequestPermissionsResult(request, permissions, grants);
        if (request == 1) {
            if (grants.length == 0) { line("Permission request canceled."); return; }
            for (int grant : grants) if (grant != PackageManager.PERMISSION_GRANTED) {
                line("Permission denied; sampling remains stopped.");
                return;
            }
            startSampling();
        }
    }

    @Override public void onPause() {
        stopSampling();
        super.onPause();
    }

    private void line(String message) {
        Log.i(TAG, message);
        if (text.length() > 20_000) text.setText("");
        text.append(message + "\n");
    }
}
