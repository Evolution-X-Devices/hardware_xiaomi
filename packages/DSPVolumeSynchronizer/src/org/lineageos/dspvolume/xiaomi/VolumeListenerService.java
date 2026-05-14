package org.lineageos.dspvolume.xiaomi;

import android.app.Service;
import android.content.Intent;
import android.content.IntentFilter;
import android.media.AudioManager;
import android.os.IBinder;

public class VolumeListenerService extends Service {
    private VolumeListenerReceiver mReceiver;
    private boolean mReceiverRegistered;

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (!mReceiverRegistered) {
            IntentFilter intentFilter = new IntentFilter();
            intentFilter.addAction("android.media.VOLUME_CHANGED_ACTION");
            mReceiver = new VolumeListenerReceiver();
            registerReceiver(mReceiver, intentFilter);
            mReceiverRegistered = true;
        }

        syncCurrentMusicVolume();

        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        if (mReceiverRegistered && mReceiver != null) {
            unregisterReceiver(mReceiver);
            mReceiverRegistered = false;
            mReceiver = null;
        }

        super.onDestroy();
    }

    private void syncCurrentMusicVolume() {
        AudioManager audioManager = getSystemService(AudioManager.class);
        if (audioManager == null) {
            return;
        }

        int current = audioManager.getStreamVolume(AudioManager.STREAM_MUSIC);
        audioManager.setParameters("volume_change=" + current + ";flags=8");
    }
}
