package com.mewlips.nxremote;

import android.graphics.Bitmap;
import android.util.Log;

import java.io.IOException;
import java.io.InputStream;
import java.net.Socket;

import static com.mewlips.nxremote.Configurations.FRAME_HEIGHT;
import static com.mewlips.nxremote.Configurations.FRAME_WIDTH;
import static com.mewlips.nxremote.Configurations.XWIN_SEGMENT_NUM_PIXELS;
import static com.mewlips.nxremote.Configurations.XWIN_SEGMENT_SIZE;
import static com.mewlips.nxremote.Configurations.XWIN_STREAMER_PORT;

/**
 * Created by mewlips on 16. 6. 29.
 */

public class XWinViewer extends Thread {
    private static final String TAG = "XWinViewer";

    private NXCameraInfo mCameraInfo;
    private MainActivity mActivity;
    private Socket mSocket;
    private InputStream mReader;

    private byte[] mBuffer = new byte[XWIN_SEGMENT_SIZE];
    final int[] mIntArray = new int[FRAME_WIDTH * FRAME_HEIGHT];

    public XWinViewer(NXCameraInfo cameraInfo, MainActivity activity) {
        mCameraInfo = cameraInfo;
        mActivity = activity;
    }

    @Override
    public void run() {
        if (mCameraInfo == null || mCameraInfo.getIpAddress() == null) {
            return;
        }
        try {
            mSocket = new Socket(mCameraInfo.getIpAddress(), XWIN_STREAMER_PORT);
            mReader = mSocket.getInputStream();
        } catch (IOException e) {
            e.printStackTrace();
            return;
        }

        int updateCount = 0;
        while (mSocket != null) {
            int readSize = 0;
            try {
                while (readSize != XWIN_SEGMENT_SIZE) {
                    if (mReader == null) {
                        readSize = -1;
                        break;
                    }
                    readSize += mReader.read(mBuffer, readSize, XWIN_SEGMENT_SIZE - readSize);
                    if (readSize == -1) {
                        break;
                    }
                }
            } catch (Exception e) {
                e.printStackTrace();
            }
            if (readSize == -1) {
                Log.d(TAG, "xwin read failed.");
                if (mSocket != null) {
                    closeSocket();
                }
                break;
            }
            if (readSize == XWIN_SEGMENT_SIZE) {
                int index = (mBuffer[0] & 0xff) << 8 | mBuffer[1] & 0xff;
//                    Log.d(TAG, "index = " + index);
                if (index == 0x0fff) { // end of frame
                    if (updateCount > 0) {
//                            Log.d(TAG, "update xwin");
                        Bitmap bitmap = Bitmap.createBitmap(mIntArray, FRAME_WIDTH, FRAME_HEIGHT, Bitmap.Config.ARGB_8888);
                        mActivity.setXWinBitmap(bitmap);
                    }
                    updateCount = 0;
                } else {
                    int offset = index * XWIN_SEGMENT_NUM_PIXELS;
                    for (int i = 0; i < XWIN_SEGMENT_NUM_PIXELS; i++) {
                        int j = 2 + i * 4;
                        int b = mBuffer[j] & 0xff;
                        int g = mBuffer[j+1] & 0xff;
                        int r = mBuffer[j+2] & 0xff;
                        int a = mBuffer[j+3] & 0xff;
                        mIntArray[offset + i] = (a << 24) | (r << 16) | (g << 8) | b;
                    }
                    updateCount++;
                }
            } else {
                Log.d(TAG, "readSize = " + readSize);
            }
        }
    }

    protected void closeSocket() {
        if (mReader != null) {
            try {
                mReader.close();
            } catch (IOException e) {
                e.printStackTrace();
            }
            mReader = null;
        }
        if (mSocket != null) {
            try {
                mSocket.close();
            } catch (IOException e) {
                e.printStackTrace();
            }
            mSocket = null;
        }
    }
}