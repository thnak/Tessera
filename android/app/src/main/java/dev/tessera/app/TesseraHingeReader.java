// SPDX-License-Identifier: MIT

package dev.tessera.app;

import android.content.Context;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;

/**
 * Reads the hinge angle and hands it to the engine (issue #189,
 * docs/design/mobile-integration.md section 3.5).
 *
 * <p><b>Why this is not part of {@link TesseraFoldReader}.</b> The two read different sources at
 * different rates. Posture comes from Jetpack WindowManager's {@code FoldingFeature}, which reports
 * a discrete state and changes only when the device crosses between flat and half-opened. The angle
 * comes from the platform's hinge sensor ({@code Sensor.TYPE_HINGE_ANGLE}), which reports a degree
 * value continuously while the user folds. A device can have one without the other: a foldable with
 * no hinge sensor still reports posture, and a device whose window is not laid out yet reports no
 * posture while the sensor keeps working.
 *
 * <p><b>Why the platform SDK is enough here.</b> Unlike posture, the hinge sensor needs no library:
 * {@code android.hardware.Sensor.TYPE_HINGE_ANGLE} is a platform constant (36) present in
 * {@code android.jar} for API 35 and later, and {@code SensorManager} is the platform's own API. So
 * this file adds no dependency, which is why it is separate from the one AndroidX call.
 *
 * <p><b>Why a listener rather than a poll.</b> The sensor is an on-change sensor: it reports when the
 * angle changes and stays silent otherwise. Polling it would either miss a fold or burn power
 * asking for a value that has not moved.
 */
final class TesseraHingeReader implements SensorEventListener {
    /**
     * The engine's "no sensor" sentinel, matching {@code hal::kUnknownHingeAngle}. It is negative
     * because no valid angle is: 0 is a fully closed hinge, so a sentinel of 0 would be
     * indistinguishable from a measurement.
     */
    private static final float UNKNOWN_ANGLE = -1.0F;

    private final SensorManager sensorManager;
    private final Sensor hingeSensor;
    private long handle;

    /**
     * Resolves the sensor, or null when the device has none.
     *
     * <p>A device without a hinge sensor is the common case (every slab phone), so this is not an
     * error: {@link #start} then does nothing and the engine keeps reporting the unknown angle.
     */
    TesseraHingeReader(Context context) {
        this.sensorManager = (SensorManager) context.getSystemService(Context.SENSOR_SERVICE);
        this.hingeSensor = sensorManager == null
                ? null
                : sensorManager.getDefaultSensor(Sensor.TYPE_HINGE_ANGLE);
    }

    /** True when this device can report an angle at all. */
    boolean available() {
        return hingeSensor != null;
    }

    /**
     * Starts reporting until {@link #stop()}.
     *
     * <p>Called once the native shell exists, because a push before that has no handle to reach.
     * The registration is on the main looper, which is where the engine's JNI setters expect to be
     * called from: they take the window's metrics mutex, and the render thread is the only other
     * holder.
     *
     * <p>A failed registration is not fatal: the app degrades to "no angle" rather than losing the
     * fold feature, which is what {@link TesseraFoldReader} still provides.
     */
    void start(long handle) {
        if (handle == 0 || hingeSensor == null || sensorManager == null) {
            return;
        }
        this.handle = handle;
        // SENSOR_DELAY_NORMAL is the right rate for a value a layout may react to: the sensor is
        // on-change, so the delay only bounds how often a fast fold is sampled, and a faster rate
        // would wake the render thread for degrees no frame will show.
        sensorManager.registerListener(this, hingeSensor, SensorManager.SENSOR_DELAY_NORMAL);
    }

    /** Stops reporting. Safe twice, and safe when {@link #start} never ran. */
    void stop() {
        if (sensorManager != null && hingeSensor != null) {
            sensorManager.unregisterListener(this, hingeSensor);
        }
        handle = 0;
    }

    @Override
    public void onSensorChanged(SensorEvent event) {
        if (handle == 0 || event.values.length == 0) {
            return;
        }
        // The hinge sensor reports one scalar in degrees. The engine clamps and validates it, so a
        // vendor driver reporting something out of range is handled there rather than here.
        TesseraView.nativeSetHingeAngle(handle, event.values[0]);
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {
        // The hinge angle has no meaningful accuracy tier: it is a mechanical measurement, not a
        // calibrated field. A change is not something the engine can act on, so it is ignored.
    }

    /** The value the engine reads when no sensor answered. Exposed for the shell's initial push. */
    static float unknownAngle() {
        return UNKNOWN_ANGLE;
    }
}
