package org.wowdecomp.wow;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.graphics.Insets;
import android.graphics.Rect;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.text.InputType;
import android.util.DisplayMetrics;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;

import java.util.ArrayList;
import java.util.concurrent.CountDownLatch;

/**
 * The game's activity: a NativeActivity running libwow.so (WoW/Source/Android/main.cpp), with what native code can't
 * do on its own. The game calls these methods through JNI (Engine/Source/Os/Android/AndroidActivity.cpp); the ones that
 * wait for the player block the calling game thread, never the UI thread.
 */
public class WowActivity extends NativeActivity {
    // Results of the text and message dialogs, handed back to the waiting game thread.
    private String mText;
    private int mChoice;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        // Draw under the notch too; the game keeps its UI inside getSafeInsets.
        WindowManager.LayoutParams params = getWindow().getAttributes();
        params.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        getWindow().setAttributes(params);
        hideSystemBars();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);

        if (hasFocus) {
            hideSystemBars();
        }
    }

    private void hideSystemBars() {
        getWindow().setDecorFitsSystemWindows(false);
        WindowInsetsController controller = getWindow().getInsetsController();

        if (controller != null) {
            controller.hide(WindowInsets.Type.systemBars());
            controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }

    /** Whether the app may read and write shared storage (All files access), where the game folder is. */
    public boolean hasStorageAccess() {
        return Environment.isExternalStorageManager();
    }

    /** Opens the system page where the player grants All files access to this app. */
    public void requestStorageAccess() {
        runOnUiThread(() -> {
            Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName()));

            try {
                startActivity(intent);
            } catch (Exception e) {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        });
    }

    /** Shared storage's root (/storage/emulated/0), under which the game folder is. */
    public String getStorageRoot() {
        return Environment.getExternalStorageDirectory().getAbsolutePath();
    }

    /**
     * A message box with up to three buttons, from the right: the first is the default. Returns the index of the button
     * pressed, or -1 if the box was dismissed. Blocks the calling (game) thread until then.
     */
    public int showMessage(String title, String message, String[] buttons) {
        CountDownLatch done = new CountDownLatch(1);
        mChoice = -1;

        runOnUiThread(() -> {
            AlertDialog.Builder builder = new AlertDialog.Builder(this).setTitle(title).setMessage(message).setCancelable(true);

            if (buttons.length > 0) {
                builder.setPositiveButton(buttons[0], (dialog, which) -> mChoice = 0);
            }

            if (buttons.length > 1) {
                builder.setNegativeButton(buttons[1], (dialog, which) -> mChoice = 1);
            }

            if (buttons.length > 2) {
                builder.setNeutralButton(buttons[2], (dialog, which) -> mChoice = 2);
            }

            builder.setOnDismissListener(dialog -> done.countDown());
            builder.show();
        });

        try {
            done.await();
        } catch (InterruptedException e) {
            return -1;
        }

        return mChoice;
    }

    /**
     * Android's keyboard over a one-line text box holding the text, for the game's focused edit box. Returns the text
     * entered, or null if the player cancelled. Blocks the calling (game) thread until then.
     */
    public String editText(String title, String text, boolean password) {
        CountDownLatch done = new CountDownLatch(1);
        mText = null;

        runOnUiThread(() -> {
            EditText field = new EditText(this);
            field.setText(text);
            field.setSelection(field.getText().length());
            field.setSingleLine(true);
            field.setImeOptions(EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_EXTRACT_UI);
            field.setInputType(password ? InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD : InputType.TYPE_CLASS_TEXT);

            AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle(title)
                .setView(field)
                .setPositiveButton(android.R.string.ok, (d, which) -> mText = field.getText().toString())
                .setNegativeButton(android.R.string.cancel, null)
                .create();

            // The keyboard's Done key sends the text, as Enter does in the game.
            field.setOnEditorActionListener((view, action, event) -> {
                boolean enter = event != null && event.getKeyCode() == KeyEvent.KEYCODE_ENTER && event.getAction() == KeyEvent.ACTION_DOWN;

                if (action == EditorInfo.IME_ACTION_DONE || enter) {
                    mText = field.getText().toString();
                    dialog.dismiss();
                    return true;
                }

                return false;
            });

            dialog.setOnDismissListener(d -> done.countDown());
            dialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
            dialog.show();
            field.requestFocus();
        });

        try {
            done.await();
        } catch (InterruptedException e) {
            return null;
        }

        return mText;
    }

    /** Opens a link in the app Android picks for it (a browser for http links). */
    public boolean openUrl(String url) {
        try {
            Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse(url));
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            startActivity(intent);
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    public String getClipboardText() {
        ClipboardManager clipboard = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        ClipData clip = clipboard.getPrimaryClip();

        if (clip == null || clip.getItemCount() == 0) {
            return null;
        }

        CharSequence text = clip.getItemAt(0).coerceToText(this);
        return text != null ? text.toString() : null;
    }

    public void setClipboardText(String text) {
        ClipboardManager clipboard = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        clipboard.setPrimaryClip(ClipData.newPlainText("World of Warcraft", text));
    }

    /** The display's full size in pixels (whatever the window's size while it settles), for the game's modes. */
    public int[] getDisplaySize() {
        android.graphics.Rect bounds = getWindowManager().getMaximumWindowMetrics().getBounds();
        return new int[] { bounds.width(), bounds.height() };
    }

    /** The screen's density in dots per inch, for the UI's size on a phone. */
    public float getDensityDpi() {
        DisplayMetrics metrics = getResources().getDisplayMetrics();
        return metrics.xdpi > 0 ? (metrics.xdpi + metrics.ydpi) / 2 : metrics.densityDpi;
    }

    /**
     * Areas, in pixels (left, top, right, bottom each), where a swipe in from the screen's edge goes to the game rather
     * than being the system's back gesture: the joystick. The system honours 200dp of each edge.
     */
    public void setGestureExclusion(int[] rects) {
        runOnUiThread(() -> {
            ArrayList<Rect> list = new ArrayList<>();

            for (int i = 0; i + 3 < rects.length; i += 4) {
                list.add(new Rect(rects[i], rects[i + 1], rects[i + 2], rects[i + 3]));
            }

            getWindow().getDecorView().setSystemGestureExclusionRects(list);
        });
    }

    /** The display's refresh rate now, in hertz (it can change: 60, 90, 120). */
    public float getRefreshRate() {
        android.view.Display display = getWindow().getDecorView().getDisplay();

        if (display == null) {
            display = getWindowManager().getDefaultDisplay();
        }

        return display != null ? display.getRefreshRate() : 60.0f;
    }

    /** The parts of the screen the notch and the home gesture bar cover, in pixels: left, top, right, bottom. */
    public int[] getSafeInsets() {
        View view = getWindow().getDecorView();
        WindowInsets insets = view.getRootWindowInsets();

        if (insets == null) {
            return new int[] { 0, 0, 0, 0 };
        }

        Insets safe = insets.getInsets(WindowInsets.Type.displayCutout() | WindowInsets.Type.mandatorySystemGestures());
        return new int[] { safe.left, safe.top, safe.right, safe.bottom };
    }
}
