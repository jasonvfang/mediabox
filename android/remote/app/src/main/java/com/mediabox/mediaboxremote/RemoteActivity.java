package com.mediabox.mediaboxremote;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothSocket;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.os.Handler;
import android.os.Parcelable;
import android.preference.PreferenceManager;
import android.support.v4.app.DialogFragment;
import android.support.v4.app.FragmentManager;
import android.support.v7.app.AppCompatActivity;
import android.os.Bundle;
import android.util.Base64;
import android.util.Log;
import android.view.KeyEvent;
import android.view.Menu;
import android.view.MenuInflater;
import android.view.MenuItem;
import android.view.View;
import android.view.WindowManager;
import android.view.inputmethod.InputMethodManager;
import android.widget.Toast;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.OutputStreamWriter;
import java.io.PrintWriter;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.net.InetAddress;
import java.net.Socket;
import java.net.UnknownHostException;

public class RemoteActivity extends AppCompatActivity
{
    public static final int ACTION_DOWNLOAD = 0;
    public static final int ACTION_STREAM = 1;

    private static final int SERVER_PORT = 2048;
    private static final String BT_ADDRESS = "00:02:72:13:75:93";

    private Socket socket = null;
    private BluetoothSocket btsocket = null;
    private BluetoothAdapter btdev = BluetoothAdapter.getDefaultAdapter();
    private Handler mHandler = null;

    private final BroadcastReceiver mReceiver = new BroadcastReceiver() {
        public void onReceive(Context context, Intent intent)
        {
            String action = intent.getAction();
            if (BluetoothDevice.ACTION_UUID.equals(action))
            {
                BluetoothDevice device = intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
                Parcelable[] uuids = intent.getParcelableArrayExtra(BluetoothDevice.EXTRA_UUID);
                if (uuids.length > 0)
                {
                    for (Parcelable uuid : uuids)
                    {
                        Log.d("RemoteActivity", String.format("Found %s UUID: %s",
                                device.getName(), uuid.toString()));
                    }
                }
            }
        }
    };


    private BluetoothSocket createRfcommSocket(BluetoothDevice dev, int channel)
    {
        BluetoothSocket tmp;

        try
        {
            Method m = dev.getClass().getMethod(
                    "createRfcommSocket", new Class[]{int.class});
            tmp = (BluetoothSocket) m.invoke(dev, channel);
        }
        catch (InvocationTargetException ex)
        {
            ex.printStackTrace();
            tmp = null;
        }
        catch (IllegalAccessException ex)
        {
            ex.printStackTrace();
            tmp = null;
        }
        catch (NoSuchMethodException ex)
        {
            ex.printStackTrace();
            tmp = null;
        }
        return tmp;
    }


    private void closeSocket()
    {
        if (this.socket != null)
        {
            try
            {
                this.socket.close();
                this.socket = null;
            }
            catch (java.io.IOException e)
            {
                e.printStackTrace();
            }
        }
        if (this.btsocket != null)
        {
            try
            {
                this.btsocket.close();
                this.btsocket = null;
            }
            catch (java.io.IOException e)
            {
                e.printStackTrace();
            }
        }
    }


    private void openSocket()
    {
        closeSocket();
        new Thread(new ClientThread()).start();
    }


    private void sendMessage(String msg)
    {
        try
        {
            PrintWriter out;
            if (this.socket != null)
            {
                Log.d("RemoteActivity", "Sending via TCP");
                out = new PrintWriter(new BufferedWriter(
                        new OutputStreamWriter(socket.getOutputStream())), true);
            }
            else if (this.btsocket != null)
            {
                Log.d("RemoteActivity", "Sending via Bluetooth");
                out = new PrintWriter(new BufferedWriter(
                        new OutputStreamWriter(btsocket.getOutputStream())), true);
            }
            else
            {
                return;
            }
            out.println(msg);
            out.flush();

        }
        catch (UnknownHostException e)
        {
            e.printStackTrace();
        }
        catch (IOException e)
        {
            e.printStackTrace();
        }
        catch (Exception e)
        {
            e.printStackTrace();
        }
    }


    private void waitForDiscovery()
    {
        while (btdev != null && btdev.isDiscovering())
        {
            try
            {
                Thread.sleep(500);
            }
            catch (InterruptedException ex)
            {
                ex.printStackTrace();
            }
        }
    }

    private String readTorrent(String filename) {
        try {
            if (android.os.Build.VERSION.SDK_INT >= 9) {
                File f = new File(filename);
                if (f.length() <= Integer.MAX_VALUE) {
                    FileInputStream fs = new FileInputStream(filename);
                    byte data[] = new byte[(int) f.length()];
                    if (fs.read(data, 0, (int) f.length()) > 0) {
                        return Base64.encodeToString(data, Base64.DEFAULT);
                    }
                }
            }
        } catch (FileNotFoundException e) {
            Log.d("RemoteActivity", e.toString());
        } catch (IOException e) {
            Log.d("RemoteActivity", e.toString());
        }
        return "";
    }

    private void handleIntent(Intent intent)
    {
        if (intent != null) {
            if (intent.getAction().equals("android.intent.action.VIEW") ||
                    intent.getAction().equals("android.intent.action.EDIT")) {
                StreamOpenDialog dialog = new StreamOpenDialog();
                if (intent.getScheme().equals("file")) {
                    dialog.setUrl(String.format("torrent: %s",
                            readTorrent(intent.getData().toString())));
                } else {
                    dialog.setUrl(intent.getData().toString());
                }
                dialog.show(getSupportFragmentManager(), "dialog");
            } else if (intent.getAction().equals("android.intent.action.SEND")) {
                String url;
                if ((url = intent.getStringExtra(Intent.EXTRA_TEXT)) != null) {
                    StreamOpenDialog dialog = new StreamOpenDialog();
                    dialog.setUrl(url);
                    dialog.show(getSupportFragmentManager(), "dialog");
                    Log.d("RemoteActivity",
                            String.format("URL Received: %s", url));
                }
            }
        } else {
            Log.d("Intent", intent.getAction());
        }
    }

    protected void onNewIntent(Intent intent)
    {
        super.onNewIntent(intent);
        handleIntent(intent);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState)
    {
        super.onCreate(savedInstanceState);
        mHandler = new Handler();
        setContentView(R.layout.activity_remote);

        /* Start the discovery service */
        startService(new Intent(this, DiscoveryService.class));

        /* Keep the screen on */
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        /* Enable the action bar even on devices with a menu key */
        /*
        try
        {
            ViewConfiguration config = ViewConfiguration.get(this);
            Field menuKeyField = ViewConfiguration.class.getDeclaredField("sHasPermanentMenuKey");
            if (menuKeyField != null)
            {
                menuKeyField.setAccessible(true);
                menuKeyField.setBoolean(config, false);
            }
        }
        catch (Exception ex)
        {
        }
        */

        this.findViewById(R.id.btnMenu).setOnLongClickListener(new View.OnLongClickListener() {
            @Override
            public boolean onLongClick(View v) {
                sendMessage("MENU_LONG");
                return true;
            }
        });
        this.findViewById(R.id.btnTrack).setOnLongClickListener(new View.OnLongClickListener() {
            @Override
            public boolean onLongClick(View v) {
                sendMessage("TRACK_LONG");
                return true;
            }
        });

        /* Capture keyboard input */
        this.findViewById(R.id.btnKeyboard).setOnKeyListener(new View.OnKeyListener()
        {
            @Override
            public boolean onKey(View v, int keyCode, KeyEvent event)
            {
                if (event.getAction() == KeyEvent.ACTION_UP)
                {
                    if (keyCode == KeyEvent.KEYCODE_DEL)
                    {
                        sendMessage("CLEAR");
                    }
                    else
                    {
                        sendMessage(String.format("KEY:%c",
                                Character.toUpperCase((char) event.getUnicodeChar())));
                    }
                }
                return true;
            }
        });

        /* Register broadcast receiver to get SDP discovery results */
        IntentFilter filter = new IntentFilter(BluetoothDevice.ACTION_UUID);
        registerReceiver(mReceiver, filter);

        if (savedInstanceState == null)
        {
            handleIntent(getIntent());
        }
    }


    public boolean onCreateOptionsMenu(Menu menu)
    {
        Log.d("RemoteActivity", "Inflating menu");
        MenuInflater inflater = getMenuInflater();
        inflater.inflate(R.menu.main_menu, menu);
        return true;
    }

    public void onSendURL(String url, int action)
    {
        if (action == ACTION_STREAM) {
            Log.d("RemoteActivity", String.format("Sending %s", url));
            sendMessage(String.format("URL:%s", url));
        } else {
            Log.d("RemoteActivity", String.format("Downloading %s", url));
            sendMessage(String.format("DOWNLOAD:%s", url));
        }
    }

    public boolean onOptionsItemSelected(MenuItem item)
    {
        int id = item.getItemId();
        if (id == R.id.discover)
        {
            startActivity(new Intent(this, DeviceListActivity.class));
            return true;
        }
        else if (id == R.id.send_url)
        {
            FragmentManager fm = getSupportFragmentManager();
            DialogFragment dialog = new SendURLDialogFragment();
            dialog.show(fm, "dialog");
        }
        else if (id == R.id.bluetooth)
        {
            SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(this);
            SharedPreferences.Editor editor = prefs.edit();
            editor.putString("device", BT_ADDRESS);
            if (android.os.Build.VERSION.SDK_INT >= 9) {
                editor.apply();
            } else {
                editor.commit();
            }
            openSocket();
        }
        else if (id == R.id.about)
        {
            startActivity(new Intent(this, AboutActivity.class));
            return true;
        }
        return false;
    }


    @Override
    protected void onDestroy()
    {
        Log.d("Remote", "Destroying");
        unregisterReceiver(mReceiver);
        stopService(new Intent(this, DiscoveryService.class));
        super.onDestroy();
    }


    @Override
    protected void onStart()
    {
        super.onStart();
        this.openSocket();

    }


    @Override
    protected void onStop()
    {
        super.onStop();
        this.closeSocket();
        ((InputMethodManager) this.getSystemService(Context.INPUT_METHOD_SERVICE))
                .hideSoftInputFromWindow(findViewById(R.id.btnKeyboard).getWindowToken(), 0);
    }


    /* Handles KEYBOARD button onClick event */
    public void onKeyboard(View view)
    {
        InputMethodManager im = (InputMethodManager) view.getContext()
                .getSystemService(Context.INPUT_METHOD_SERVICE);
        im.showSoftInput(view, InputMethodManager.SHOW_FORCED);
    }


    public void onButtonPressed(View view)
    {
        Log.d("RemoteActivity", "Button pressed");

        if (view == this.findViewById(R.id.btnMenu))
        {
            sendMessage("MENU");
        }
        else if (view == this.findViewById(R.id.btnUp))
        {
            sendMessage("UP");
        }
        else if (view == this.findViewById(R.id.btnLeft))
        {
            sendMessage("LEFT");
        }
        else if (view == this.findViewById(R.id.btnRight))
        {
            sendMessage("RIGHT");
        }
        else if (view == this.findViewById(R.id.btnDown))
        {
            sendMessage("DOWN");
        }
        else if (view == this.findViewById(R.id.btnBack))
        {
            sendMessage("BACK");
        }
        else if (view == this.findViewById(R.id.btnEnter))
        {
            sendMessage("ENTER");
        }
        else if (view == this.findViewById(R.id.btnStop))
        {
            sendMessage("STOP");
        }
        else if (view == this.findViewById(R.id.btnPlay))
        {
            sendMessage("PLAY");
        }
        else if (view == this.findViewById(R.id.btnInfo))
        {
            sendMessage("INFO");
        }
        else if (view == this.findViewById(R.id.btnPrev))
        {
            sendMessage("PREV");
        }
        else if (view == this.findViewById(R.id.btnNext))
        {
            sendMessage("NEXT");
        }
        else if (view == this.findViewById(R.id.btnRew))
        {
            sendMessage("RW");
        }
        else if (view == this.findViewById(R.id.btnFF))
        {
            sendMessage("FF");
        }
        else if (view == this.findViewById(R.id.btnVolUp))
        {
            sendMessage("VOLUP");
        }
        else if (view == this.findViewById(R.id.btnVolDown))
        {
            sendMessage("VOLDOWN");
        }
        else if (view == this.findViewById(R.id.btnTrack))
        {
            sendMessage("TRACK");
        }
        else if (view == this.findViewById(R.id.btnMute))
        {
            sendMessage("MUTE");
        }
    }


    class ClientThread implements Runnable {
        @Override
        public void run()
        {
            InetAddress deviceAddress = null;
            String address = null;
            try
            {
                final SharedPreferences prefs = PreferenceManager.
                        getDefaultSharedPreferences(RemoteActivity.this);
                if (android.os.Build.VERSION.SDK_INT >= 15 &&
                        prefs.getString("device", "").equals(BT_ADDRESS))
                {
                    Log.d("RemoteActivity", "Opening Bluetooth socket");

                    if (btdev != null)
                    {
                        BluetoothDevice dev = btdev.getRemoteDevice(BT_ADDRESS);

                        if (dev != null) {
                            Log.d("RemoteActivity", String.format("Connecting to %s",
                                    dev.getName()));

                            waitForDiscovery();

                            /* Discover services via SDP */
                            if (!dev.fetchUuidsWithSdp())
                            {
                                Log.d("RemoteActivity", "fetchUuidsWithSdp() failed");
                            }

                            /*
                            btsocket = dev.createRfcommSocketToServiceRecord(
                                    UUID.fromString("00000000-0000-0000-0000-0000cdab0000"));
                            waitForDiscovery();
                            btsocket.connect();
                            */

                            btsocket = createRfcommSocket(dev, 1);
                            btsocket.connect();
                        }
                    }
                    else
                    {
                        Log.d("RemoteActivity", "No Bluetooth adapter!");
                    }
                }
                else
                {
                    Log.d("RemoteActivity", "Opening TCP socket");
                    address = prefs.getString("device", "10.10.0.14");
                    socket = new Socket(address, SERVER_PORT);

                    final String addr = address;
                    mHandler.post(new Runnable() {
                        @Override
                        public void run() {
                            Toast.makeText(RemoteActivity.this,
                                String.format("Connected to %s", addr),
                                Toast.LENGTH_LONG).show();
                        }
                    });
                }
            }
            catch (UnknownHostException e1)
            {
                final String addr = address;
                mHandler.post(new Runnable() {
                    @Override
                    public void run() {
                        Toast.makeText(RemoteActivity.this,
                            String.format("Unknown Host: %s", addr),
                            Toast.LENGTH_LONG).show();
                    }
                });
                e1.printStackTrace();
            }
            catch (final IOException e1)
            {
                final String addr = address;
                mHandler.post(new Runnable() {
                    @Override
                    public void run() {
                        Toast.makeText(RemoteActivity.this,
                            String.format("Error: %s: %s", e1.getMessage(), addr),
                            Toast.LENGTH_LONG).show();
                    }
                });

                if (socket != null)
                {
                    if (socket.isConnected())
                    {
                        try
                        {
                            socket.close();
                        }
                        catch (IOException e2)
                        {
                            e2.printStackTrace();
                        }
                    }
                }
                if (btsocket != null)
                {
                    if (android.os.Build.VERSION.SDK_INT <= 14 ||
                            btsocket.isConnected())
                    {
                        try
                        {
                            btsocket.close();
                        }
                        catch (IOException e2)
                        {
                            e2.printStackTrace();
                        }
                    }
                }
                e1.printStackTrace();
            }
        }
    }
}
