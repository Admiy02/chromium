// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.webapps;

import android.app.Activity;
import android.app.Dialog;
import android.content.DialogInterface;
import android.support.annotation.StringRes;
import android.support.v7.app.AlertDialog;

import org.chromium.base.ApiCompatibilityUtils;
import org.chromium.chrome.R;

/**
 * A dialog to notify users that WebAPKs and Trusted Web Activities need a network connection to
 * launch.
 */
public class WebappOfflineDialog {
    private Dialog mDialog;

    /**
     * Shows the dialog that notifies users that the WebAPK or TWA is offline.
     * @param activity Activity that will be used for {@link Dialog#show()}.
     * @param appName The name of the Android native client for which the dialog is shown.
     * @param isWebApk Whether the app above is a WebAPK.
     */
    public void show(final Activity activity, String appName, boolean isWebApk) {
        @StringRes int messageID = isWebApk
                ? R.string.webapk_offline_dialog : R.string.webapp_twa_offline_dialog;
        AlertDialog.Builder builder = new AlertDialog.Builder(activity, R.style.AlertDialogTheme);
        builder.setMessage(activity.getString(messageID, appName))
                .setPositiveButton(R.string.ok,
                        new DialogInterface.OnClickListener() {
                            @Override
                            public void onClick(DialogInterface dialog, int which) {
                                ApiCompatibilityUtils.finishAndRemoveTask(activity);
                            }
                        });

        mDialog = builder.create();
        mDialog.setCanceledOnTouchOutside(false);
        mDialog.show();
    }

    /** Closes the dialog. */
    public void cancel() {
        mDialog.cancel();
    }
}
