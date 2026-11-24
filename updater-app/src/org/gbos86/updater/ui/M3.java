package org.gbos86.updater.ui;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.LayerDrawable;
import android.graphics.drawable.RippleDrawable;
import android.graphics.drawable.StateListDrawable;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

/**
 * Material Design 3 UI component factory.
 * Provides cards, buttons, badges, typography and responsive layout containers
 * adhering strictly to Material 3 guidelines and dynamic color tokens.
 */
public final class M3 {
    public static final int BADGE_NEUTRAL = 0;
    public static final int BADGE_SUCCESS = 1;
    public static final int BADGE_WARNING = 2;
    public static final int BADGE_ERROR = 3;
    public static final int BADGE_PRIMARY = 4;

    private M3() {}

    public static int dp(Context c, int v) {
        return (int) (v * c.getResources().getDisplayMetrics().density + 0.5f);
    }

    public static GradientDrawable roundRect(int bgColor, float radiusPx) {
        GradientDrawable d = new GradientDrawable();
        d.setShape(GradientDrawable.RECTANGLE);
        d.setColor(bgColor);
        d.setCornerRadius(radiusPx);
        return d;
    }

    public static GradientDrawable roundRectBorder(int bgColor, int borderColor, int borderWidthPx, float radiusPx) {
        GradientDrawable d = roundRect(bgColor, radiusPx);
        d.setStroke(borderWidthPx, borderColor);
        return d;
    }

    public static RippleDrawable ripple(Drawable content, int rippleColor, float radiusPx) {
        GradientDrawable mask = roundRect(Color.WHITE, radiusPx);
        return new RippleDrawable(ColorStateList.valueOf(rippleColor), content, mask);
    }

    // -------------------------------------------------------------------------------- Cards & Containers
    public static LinearLayout card(Context c, int padDp) {
        LinearLayout l = new LinearLayout(c);
        l.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(c, padDp);
        l.setPadding(pad, pad, pad, pad);
        float r = dp(c, 20);
        int bg = M3Theme.surfaceContainer(c);
        int border = M3Theme.outlineVariant(c);
        l.setBackground(roundRectBorder(bg, border, dp(c, 1), r));
        return l;
    }

    public static LinearLayout card(Context c) {
        return card(c, 16);
    }

    /** Width the content gets inside responsiveScaffold: the window minus its side margins, at most maxWidthDp. */
    public static int contentWidthDp(Context c, int maxWidthDp) {
        int avail = c.getResources().getConfiguration().screenWidthDp - 32;
        return maxWidthDp > 0 ? Math.min(maxWidthDp, avail) : avail;
    }

    /** True when the window is wide enough (a laptop screen, not a phone) for two columns of cards. */
    public static boolean wide(Context c) {
        return c.getResources().getConfiguration().screenWidthDp >= 840;
    }

    public static View responsiveScaffold(Context c, View innerContent, int maxWidthDp) {
        FrameLayout outer = new FrameLayout(c);
        outer.setBackgroundColor(M3Theme.surface(c));
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                maxWidthDp > 0 ? dp(c, contentWidthDp(c, maxWidthDp)) : FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.CENTER_HORIZONTAL
        );
        int horiz = dp(c, 16);
        lp.setMargins(horiz, 0, horiz, 0);
        outer.addView(innerContent, lp);
        return outer;
    }

    /**
     * A banner picture with rounded corners, drawn at width / aspect height and cropped to the middle (the picture's own
     * edges are empty background, so a wide window shows a thinner band of the same artwork).
     */
    public static ImageView banner(Context c, int drawableRes, float aspect, int radiusDp) {
        ImageView v = new ImageView(c) {
            @Override
            protected void onMeasure(int w, int h) {
                int width = View.MeasureSpec.getSize(w);
                setMeasuredDimension(width, Math.round(width / aspect));
            }
        };
        v.setImageResource(drawableRes);
        v.setScaleType(ImageView.ScaleType.CENTER_CROP);
        v.setBackground(roundRect(M3Theme.surfaceContainer(c), dp(c, radiusDp)));
        v.setClipToOutline(true);
        v.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        return v;
    }

    // -------------------------------------------------------------------------------- Buttons
    public static Button filledButton(Context c, String label, View.OnClickListener l) {
        Button b = new Button(c);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextSize(14);
        b.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        b.setTextColor(M3Theme.onPrimary(c));
        float r = dp(c, 22);
        int bg = M3Theme.primary(c);
        int rip = Color.argb(40, 255, 255, 255);
        b.setBackground(ripple(roundRect(bg, r), rip, r));
        b.setPadding(dp(c, 24), dp(c, 12), dp(c, 24), dp(c, 12));
        b.setMinimumHeight(dp(c, 44));
        b.setOnClickListener(l);
        return b;
    }

    public static Button tonalButton(Context c, String label, View.OnClickListener l) {
        Button b = new Button(c);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextSize(14);
        b.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        b.setTextColor(M3Theme.onPrimaryContainer(c));
        float r = dp(c, 20);
        int bg = M3Theme.primaryContainer(c);
        int rip = Color.argb(30, 0, 0, 0);
        b.setBackground(ripple(roundRect(bg, r), rip, r));
        b.setPadding(dp(c, 18), dp(c, 10), dp(c, 18), dp(c, 10));
        b.setMinimumHeight(dp(c, 40));
        b.setOnClickListener(l);
        return b;
    }

    public static Button outlinedButton(Context c, String label, View.OnClickListener l) {
        Button b = new Button(c);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextSize(14);
        b.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        b.setTextColor(M3Theme.primary(c));
        float r = dp(c, 20);
        int bg = Color.TRANSPARENT;
        int border = M3Theme.outlineVariant(c);
        int rip = Color.argb(30, 0, 0, 0);
        b.setBackground(ripple(roundRectBorder(bg, border, dp(c, 1), r), rip, r));
        b.setPadding(dp(c, 16), dp(c, 9), dp(c, 16), dp(c, 9));
        b.setMinimumHeight(dp(c, 40));
        b.setOnClickListener(l);
        return b;
    }

    public static Button textButton(Context c, String label, View.OnClickListener l) {
        Button b = new Button(c);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextSize(14);
        b.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        b.setTextColor(M3Theme.primary(c));
        float r = dp(c, 18);
        int rip = Color.argb(25, 0, 0, 0);
        b.setBackground(ripple(roundRect(Color.TRANSPARENT, r), rip, r));
        b.setPadding(dp(c, 12), dp(c, 8), dp(c, 12), dp(c, 8));
        b.setOnClickListener(l);
        return b;
    }

    public static Button dangerButton(Context c, String label, View.OnClickListener l) {
        Button b = new Button(c);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextSize(14);
        b.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        b.setTextColor(M3Theme.onErrorContainer(c));
        float r = dp(c, 20);
        int bg = M3Theme.errorContainer(c);
        int rip = Color.argb(40, 255, 255, 255);
        b.setBackground(ripple(roundRect(bg, r), rip, r));
        b.setPadding(dp(c, 16), dp(c, 10), dp(c, 16), dp(c, 10));
        b.setMinimumHeight(dp(c, 40));
        b.setOnClickListener(l);
        return b;
    }

    // -------------------------------------------------------------------------------- Badges & Chips
    public static TextView badge(Context c, String text, int type) {
        TextView t = new TextView(c);
        t.setText(text);
        t.setTextSize(12);
        t.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        int bg, fg;
        switch (type) {
            case BADGE_SUCCESS:
                bg = M3Theme.successContainer(c);
                fg = M3Theme.onSuccessContainer(c);
                break;
            case BADGE_WARNING:
                bg = M3Theme.warningContainer(c);
                fg = M3Theme.onWarningContainer(c);
                break;
            case BADGE_ERROR:
                bg = M3Theme.errorContainer(c);
                fg = M3Theme.onErrorContainer(c);
                break;
            case BADGE_PRIMARY:
                bg = M3Theme.primaryContainer(c);
                fg = M3Theme.onPrimaryContainer(c);
                break;
            default:
                bg = M3Theme.surfaceContainerHighest(c);
                fg = M3Theme.onSurfaceVariant(c);
                break;
        }
        t.setTextColor(fg);
        t.setBackground(roundRect(bg, dp(c, 12)));
        t.setPadding(dp(c, 10), dp(c, 4), dp(c, 10), dp(c, 4));
        t.setGravity(Gravity.CENTER);
        return t;
    }

    // -------------------------------------------------------------------------------- Typography
    public static TextView headline(Context c, String text) {
        TextView t = new TextView(c);
        t.setText(text);
        t.setTextSize(26);
        t.setTypeface(Typeface.create("sans-serif-medium", Typeface.BOLD));
        t.setTextColor(M3Theme.onSurface(c));
        return t;
    }

    public static TextView titleLarge(Context c, String text) {
        TextView t = new TextView(c);
        t.setText(text);
        t.setTextSize(20);
        t.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        t.setTextColor(M3Theme.onSurface(c));
        return t;
    }

    public static TextView titleMedium(Context c, String text) {
        TextView t = new TextView(c);
        t.setText(text);
        t.setTextSize(16);
        t.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        t.setTextColor(M3Theme.onSurface(c));
        return t;
    }

    public static TextView eyebrow(Context c, String text) {
        TextView t = new TextView(c);
        t.setText(text);
        t.setTextSize(11);
        t.setTypeface(Typeface.create("sans-serif-medium", Typeface.BOLD));
        t.setTextColor(M3Theme.primary(c));
        t.setLetterSpacing(0.08f);
        return t;
    }

    public static TextView body(Context c, String text) {
        TextView t = new TextView(c);
        t.setText(text);
        t.setTextSize(14);
        t.setTextColor(M3Theme.onSurface(c));
        t.setLineSpacing(dp(c, 2), 1.15f);
        return t;
    }

    public static TextView bodySmall(Context c, String text) {
        TextView t = new TextView(c);
        t.setText(text);
        t.setTextSize(12);
        t.setTextColor(M3Theme.onSurfaceVariant(c));
        t.setLineSpacing(dp(c, 2), 1.15f);
        return t;
    }

    // -------------------------------------------------------------------------------- Progress Bar
    public static ProgressBar progressBar(Context c) {
        ProgressBar bar = new ProgressBar(c, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        float r = dp(c, 4);
        GradientDrawable track = roundRect(M3Theme.surfaceContainerHighest(c), r);
        GradientDrawable prog = roundRect(M3Theme.primary(c), r);
        android.graphics.drawable.ClipDrawable clip = new android.graphics.drawable.ClipDrawable(prog, Gravity.START, android.graphics.drawable.ClipDrawable.HORIZONTAL);
        LayerDrawable layers = new LayerDrawable(new Drawable[]{track, clip});
        layers.setId(0, android.R.id.background);
        layers.setId(1, android.R.id.progress);
        bar.setProgressDrawable(layers);
        return bar;
    }

    public static View divider(Context c) {
        View v = new View(c);
        v.setBackgroundColor(M3Theme.outlineVariant(c));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(c, 1));
        int m = dp(c, 12);
        lp.setMargins(0, m, 0, m);
        v.setLayoutParams(lp);
        return v;
    }

    // -------------------------------------------------------------------------------- Segmented Control
    public interface OnTabSelectedListener {
        void onTabSelected(int index);
    }

    public static LinearLayout segmentedControl(Context c, String[] tabs, int initialIndex, OnTabSelectedListener listener) {
        LinearLayout row = new LinearLayout(c);
        row.setOrientation(LinearLayout.HORIZONTAL);
        int pad = dp(c, 4);
        row.setPadding(pad, pad, pad, pad);
        float r = dp(c, 24);
        row.setBackground(roundRectBorder(M3Theme.surfaceContainerHigh(c), M3Theme.outlineVariant(c), dp(c, 1), r));

        final Button[] buttons = new Button[tabs.length];
        for (int i = 0; i < tabs.length; i++) {
            final int index = i;
            Button b = new Button(c);
            b.setText(tabs[i]);
            b.setAllCaps(false);
            b.setTextSize(13);
            b.setPadding(dp(c, 16), dp(c, 8), dp(c, 16), dp(c, 8));
            b.setMinimumHeight(dp(c, 36));
            buttons[i] = b;

            b.setOnClickListener(v -> {
                for (int j = 0; j < buttons.length; j++) {
                    updateTabStyle(c, buttons[j], j == index);
                }
                if (listener != null) listener.onTabSelected(index);
            });

            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f);
            row.addView(b, lp);
            updateTabStyle(c, b, i == initialIndex);
        }
        return row;
    }

    private static void updateTabStyle(Context c, Button b, boolean active) {
        float r = dp(c, 20);
        if (active) {
            b.setTextColor(M3Theme.onPrimaryContainer(c));
            b.setTypeface(Typeface.create("sans-serif-medium", Typeface.BOLD));
            b.setBackground(roundRect(M3Theme.primaryContainer(c), r));
        } else {
            b.setTextColor(M3Theme.onSurfaceVariant(c));
            b.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
            b.setBackground(roundRect(Color.TRANSPARENT, r));
        }
    }
}
