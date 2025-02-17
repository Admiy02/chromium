// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.webapps;

import android.content.Context;
import android.content.res.Resources;
import android.graphics.Bitmap;
import android.view.LayoutInflater;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.TextView;

import org.chromium.base.ApiCompatibilityUtils;
import org.chromium.base.ContextUtils;
import org.chromium.base.VisibleForTesting;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.compositor.CompositorViewHolder;
import org.chromium.chrome.browser.metrics.WebappUma;
import org.chromium.chrome.browser.tab.EmptyTabObserver;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.util.ColorUtils;
import org.chromium.chrome.browser.webapps.WebappActivity.ActivityType;
import org.chromium.net.NetError;
import org.chromium.net.NetworkChangeNotifier;

/** Shows and hides splash screen. */
class WebappSplashScreenController extends EmptyTabObserver {
    /** Used to schedule splash screen hiding. */
    private CompositorViewHolder mCompositorViewHolder;

    /** View to which the splash screen is added. */
    private ViewGroup mParentView;

    /** Whether native was loaded. Native must be loaded in order to record metrics. */
    private boolean mNativeLoaded;

    /** Whether the splash screen layout was initialized. */
    private boolean mInitializedLayout;

    private ViewGroup mSplashScreen;
    private WebappUma mWebappUma;

    /** The error code of the navigation. */
    private int mErrorCode;

    private WebappOfflineDialog mOfflineDialog;

    /** Indicates whether reloading is allowed. */
    private boolean mAllowReloads;

    private String mAppName;

    private @ActivityType int mActivityType;

    public WebappSplashScreenController() {
        mWebappUma = new WebappUma();
    }

    /** Shows the splash screen. */
    public void showSplashScreen(
            @ActivityType int activityType, ViewGroup parentView, final WebappInfo webappInfo) {
        mActivityType = activityType;
        mParentView = parentView;
        mAppName = webappInfo.name();

        Context context = ContextUtils.getApplicationContext();
        final int backgroundColor = ColorUtils.getOpaqueColor(webappInfo.backgroundColor(
                ApiCompatibilityUtils.getColor(context.getResources(), R.color.webapp_default_bg)));

        mSplashScreen = new FrameLayout(context);
        mSplashScreen.setBackgroundColor(backgroundColor);
        mParentView.addView(mSplashScreen);

        mWebappUma.splashscreenVisible();
        mWebappUma.recordSplashscreenBackgroundColor(webappInfo.hasValidBackgroundColor()
                        ? WebappUma.SPLASHSCREEN_COLOR_STATUS_CUSTOM
                        : WebappUma.SPLASHSCREEN_COLOR_STATUS_DEFAULT);
        mWebappUma.recordSplashscreenThemeColor(webappInfo.hasValidThemeColor()
                        ? WebappUma.SPLASHSCREEN_COLOR_STATUS_CUSTOM
                        : WebappUma.SPLASHSCREEN_COLOR_STATUS_DEFAULT);

        WebappDataStorage storage =
                WebappRegistry.getInstance().getWebappDataStorage(webappInfo.id());
        if (storage == null) {
            initializeLayout(webappInfo, backgroundColor, null);
            return;
        }

        storage.getSplashScreenImage(new WebappDataStorage.FetchCallback<Bitmap>() {
            @Override
            public void onDataRetrieved(Bitmap splashImage) {
                initializeLayout(webappInfo, backgroundColor, splashImage);
            }
        });
    }

    /** Should be called once native has loaded. */
    public void onFinishedNativeInit(Tab tab, CompositorViewHolder compositorViewHolder) {
        mNativeLoaded = true;
        mCompositorViewHolder = compositorViewHolder;
        tab.addObserver(this);
        if (mInitializedLayout) {
            mWebappUma.commitMetrics();
        }
    }

    @VisibleForTesting
    ViewGroup getSplashScreenForTests() {
        return mSplashScreen;
    }

    @Override
    public void didFirstVisuallyNonEmptyPaint(Tab tab) {
        if (canHideSplashScreen()) {
            hideSplashScreenOnDrawingFinished(tab, WebappUma.SPLASHSCREEN_HIDES_REASON_PAINT);
        }
    }

    @Override
    public void onPageLoadFinished(Tab tab) {
        if (canHideSplashScreen()) {
            hideSplashScreenOnDrawingFinished(
                    tab, WebappUma.SPLASHSCREEN_HIDES_REASON_LOAD_FINISHED);
        }
    }

    @Override
    public void onPageLoadFailed(Tab tab, int errorCode) {
        if (canHideSplashScreen()) {
            animateHidingSplashScreen(tab, WebappUma.SPLASHSCREEN_HIDES_REASON_LOAD_FAILED);
        }
    }

    @Override
    public void onCrash(Tab tab, boolean sadTabShown) {
        animateHidingSplashScreen(tab, WebappUma.SPLASHSCREEN_HIDES_REASON_CRASH);
    }

    @Override
    public void onDidFinishNavigation(final Tab tab, final String url, boolean isInMainFrame,
            boolean isErrorPage, boolean hasCommitted, boolean isSameDocument,
            boolean isFragmentNavigation, Integer pageTransition, int errorCode,
            int httpStatusCode) {
        if (mActivityType == WebappActivity.ACTIVITY_TYPE_WEBAPP) return;

        mErrorCode = errorCode;

        switch (mErrorCode) {
            case NetError.ERR_NETWORK_CHANGED:
                onNetworkChanged(tab);
                break;
            case NetError.ERR_INTERNET_DISCONNECTED:
                onNetworkDisconnected(tab);
                break;
            default:
                if (mOfflineDialog != null) {
                    mOfflineDialog.cancel();
                    mOfflineDialog = null;
                }
                break;
        }
    }

    protected boolean canHideSplashScreen() {
        if (mActivityType == WebappActivity.ACTIVITY_TYPE_WEBAPP) return true;
        return mErrorCode != NetError.ERR_INTERNET_DISCONNECTED
                && mErrorCode != NetError.ERR_NETWORK_CHANGED;
    }

    private void onNetworkChanged(Tab tab) {
        if (!mAllowReloads) return;

        // It is possible that we get {@link NetError.ERR_NETWORK_CHANGED} during the first
        // reload after the device is online. The navigation will fail until the next auto
        // reload fired by {@link NetErrorHelperCore}. We call reload explicitly to reduce the
        // waiting time.
        tab.reloadIgnoringCache();
        mAllowReloads = false;
    }

    private void onNetworkDisconnected(final Tab tab) {
        if (mOfflineDialog != null || tab.getActivity() == null) return;

        final NetworkChangeNotifier.ConnectionTypeObserver observer =
                new NetworkChangeNotifier.ConnectionTypeObserver() {
                    @Override
                    public void onConnectionTypeChanged(int connectionType) {
                        if (!NetworkChangeNotifier.isOnline()) return;

                        NetworkChangeNotifier.removeConnectionTypeObserver(this);
                        tab.reloadIgnoringCache();
                        // One more reload is allowed after the network connection is back.
                        mAllowReloads = true;
                    }
                };

        NetworkChangeNotifier.addConnectionTypeObserver(observer);
        mOfflineDialog = new WebappOfflineDialog();
        mOfflineDialog.show(
                tab.getActivity(), mAppName, mActivityType == WebappActivity.ACTIVITY_TYPE_WEBAPK);
    }

    /** Sets the splash screen layout and sets the splash screen's title and icon. */
    private void initializeLayout(WebappInfo webappInfo, int backgroundColor, Bitmap splashImage) {
        mInitializedLayout = true;
        Context context = ContextUtils.getApplicationContext();
        Resources resources = context.getResources();

        Bitmap displayIcon = (splashImage == null) ? webappInfo.icon() : splashImage;
        int minimiumSizeThreshold =
                resources.getDimensionPixelSize(R.dimen.webapp_splash_image_size_minimum);
        int bigThreshold =
                resources.getDimensionPixelSize(R.dimen.webapp_splash_image_size_threshold);

        // Inflate the correct layout for the image.
        int layoutId;
        if (displayIcon == null || displayIcon.getWidth() < minimiumSizeThreshold
                || (displayIcon == webappInfo.icon() && webappInfo.isIconGenerated())) {
            mWebappUma.recordSplashscreenIconType(WebappUma.SPLASHSCREEN_ICON_TYPE_NONE);
            layoutId = R.layout.webapp_splash_screen_no_icon;
        } else {
            // The size of the splash screen image determines which layout to use.
            boolean isUsingSmallSplashImage = displayIcon.getWidth() <= bigThreshold
                    || displayIcon.getHeight() <= bigThreshold;
            if (isUsingSmallSplashImage) {
                layoutId = R.layout.webapp_splash_screen_small;
            } else {
                layoutId = R.layout.webapp_splash_screen_large;
            }

            // Record stats about the splash screen.
            int splashScreenIconType;
            if (splashImage == null) {
                splashScreenIconType = WebappUma.SPLASHSCREEN_ICON_TYPE_FALLBACK;
            } else if (isUsingSmallSplashImage) {
                splashScreenIconType = WebappUma.SPLASHSCREEN_ICON_TYPE_CUSTOM_SMALL;
            } else {
                splashScreenIconType = WebappUma.SPLASHSCREEN_ICON_TYPE_CUSTOM;
            }
            mWebappUma.recordSplashscreenIconType(splashScreenIconType);
            mWebappUma.recordSplashscreenIconSize(
                    Math.round(displayIcon.getWidth() / resources.getDisplayMetrics().density));
        }

        ViewGroup subLayout =
                (ViewGroup) LayoutInflater.from(context).inflate(layoutId, mSplashScreen, true);

        // Set up the elements of the splash screen.
        TextView appNameView = (TextView) subLayout.findViewById(R.id.webapp_splash_screen_name);
        ImageView splashIconView =
                (ImageView) subLayout.findViewById(R.id.webapp_splash_screen_icon);
        appNameView.setText(webappInfo.name());
        if (splashIconView != null) splashIconView.setImageBitmap(displayIcon);

        if (ColorUtils.shouldUseLightForegroundOnBackground(backgroundColor)) {
            appNameView.setTextColor(
                    ApiCompatibilityUtils.getColor(resources, R.color.webapp_splash_title_light));
        }

        if (mNativeLoaded) {
            mWebappUma.commitMetrics();
        }
    }

    /**
     * Schedules the splash screen hiding once the compositor has finished drawing a frame.
     *
     * Without this callback we were seeing a short flash of white between the splash screen and
     * the web content (crbug.com/734500).
     * */
    private void hideSplashScreenOnDrawingFinished(final Tab tab, final int reason) {
        if (mSplashScreen == null) return;

        if (mCompositorViewHolder == null) {
            animateHidingSplashScreen(tab, reason);
            return;
        }

        mCompositorViewHolder.getCompositorView().surfaceRedrawNeededAsync(null, () -> {
            animateHidingSplashScreen(tab, reason);
        });
    }

    /** Performs the splash screen hiding animation. */
    private void animateHidingSplashScreen(final Tab tab, final int reason) {
        if (mSplashScreen == null) return;

        mSplashScreen.animate().alpha(0f).withEndAction(new Runnable() {
            @Override
            public void run() {
                if (mSplashScreen == null) return;
                mParentView.removeView(mSplashScreen);
                tab.removeObserver(WebappSplashScreenController.this);
                mSplashScreen = null;
                mCompositorViewHolder = null;
                mWebappUma.splashscreenHidden(reason);
            }
        });
    }
}
