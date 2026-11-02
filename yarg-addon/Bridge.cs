// Receives the guitar's state from the wiimote-test tool over a local connection and
// presents it to YARG as a regular five-fret guitar.
using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Threading;
using PlasticBand.Devices;
using UnityEngine;
using UnityEngine.InputSystem;
using UnityEngine.InputSystem.Layouts;
using UnityEngine.InputSystem.LowLevel;
using UnityEngine.InputSystem.Utilities;

namespace WiimoteGuitar
{
    // The button bits must match wm_guitar_to_packet in src/wiimote.c.
    [StructLayout(LayoutKind.Explicit, Size = 12)]
    public struct WiiGuitarState : IInputStateTypeInfo
    {
        public FourCC format => new FourCC('W', 'G', 'T', 'R');

        [InputControl(name = "dpad", layout = "Dpad", format = "BIT", sizeInBits = 4, bit = 0)]
        [InputControl(name = "greenFret", layout = "Button", bit = 4)]
        [InputControl(name = "redFret", layout = "Button", bit = 5)]
        [InputControl(name = "yellowFret", layout = "Button", bit = 6)]
        [InputControl(name = "blueFret", layout = "Button", bit = 7)]
        [InputControl(name = "orangeFret", layout = "Button", bit = 8)]
        [InputControl(name = "startButton", layout = "Button", bit = 9)]
        [InputControl(name = "selectButton", layout = "Button", bit = 10)]
        [FieldOffset(0)]
        public uint buttons;

        [InputControl(name = "whammy", layout = "Axis")]
        [FieldOffset(4)]
        public float whammy;

        [InputControl(name = "tilt", layout = "Axis")]
        [FieldOffset(8)]
        public float tilt;
    }

    [InputControlLayout(stateType = typeof(WiiGuitarState), displayName = "Wii Guitar")]
    public class WiiGuitar : FiveFretGuitar
    {
    }

    public static class Bridge
    {
        private const int PORT = 47283;
        private const uint MAGIC = 0x31544757; // "WGT1"
        private const int PACKET_SIZE = 16;
        private const uint VALID_BUTTONS = 0x7F3;
        // Reports arrive about every 10ms, so this long without one means the Wiimote
        // has gone away. Releases everything so that no note stays held.
        private const double SILENCE_SECONDS = 0.5;

        private static readonly string LogPath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.Personal),
            "Library/Logs/wiimote-guitar-addon.log");

        private struct Received
        {
            public WiiGuitarState state;
            public long arrivedAt;
        }

        private static readonly ConcurrentQueue<Received> Incoming = new ConcurrentQueue<Received>();
        private static readonly ConcurrentQueue<string> LogLines = new ConcurrentQueue<string>();

        private static WiiGuitar _device;
        private static uint _lastLoggedPressed;
        private static long _lastPacketAt;
        private static bool _released = true;

        public static void Initialize()
        {
            Log("Add-on loaded");
            try
            {
                InputSystem.RegisterLayout<WiiGuitar>();
                InputSystem.onBeforeUpdate += OnBeforeUpdate;
                new Thread(ReceiveLoop) { IsBackground = true, Name = "WiimoteGuitarBridge" }.Start();
            }
            catch (Exception ex)
            {
                Log("Failed to start: " + ex);
            }
        }

        private static void ReceiveLoop()
        {
            try
            {
                using var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, PORT));
                socket.Client.ReceiveTimeout = 200;
                var sender = new IPEndPoint(IPAddress.Any, 0);
                LogLines.Enqueue("Listening");
                while (true)
                {
                    WriteLogLines();
                    byte[] data;
                    try
                    {
                        data = socket.Receive(ref sender);
                    }
                    catch (SocketException ex) when (ex.SocketErrorCode == SocketError.TimedOut)
                    {
                        continue;
                    }

                    if (data.Length != PACKET_SIZE || BitConverter.ToUInt32(data, 0) != MAGIC)
                    {
                        continue;
                    }
                    Incoming.Enqueue(new Received
                    {
                        arrivedAt = Stopwatch.GetTimestamp(),
                        state = new WiiGuitarState
                        {
                            buttons = BitConverter.ToUInt32(data, 4) & VALID_BUTTONS,
                            whammy = Mathf.Clamp01(BitConverter.ToSingle(data, 8)),
                            tilt = Mathf.Clamp01(BitConverter.ToSingle(data, 12)),
                        },
                    });
                }
            }
            catch (Exception ex)
            {
                Log("Receiving stopped: " + ex);
            }
        }

        // Runs on the main thread just before Unity processes input events, so the
        // guitar is handled in the same update it arrived for.
        private static void OnBeforeUpdate()
        {
            long now = Stopwatch.GetTimestamp();
            while (Incoming.TryDequeue(out var received))
            {
                if (_device == null)
                {
                    _device = InputSystem.AddDevice<WiiGuitar>();
                    LogLines.Enqueue($"Added device '{_device.displayName}'");
                }

                // Report when it actually arrived, so that YARG judges the hit by the
                // moment of the press and not by when this frame came around.
                double age = (now - received.arrivedAt) / (double) Stopwatch.Frequency;
                InputSystem.QueueStateEvent(_device, received.state, InputState.currentTime - age);
                _lastPacketAt = received.arrivedAt;
                _released = false;

                if (received.state.buttons != _lastLoggedPressed)
                {
                    _lastLoggedPressed = received.state.buttons;
                    LogLines.Enqueue($"Buttons {received.state.buttons:X3} whammy {received.state.whammy:F2}");
                }
            }

            if (_device != null && !_released &&
                (now - _lastPacketAt) / (double) Stopwatch.Frequency > SILENCE_SECONDS)
            {
                InputSystem.QueueStateEvent(_device, new WiiGuitarState());
                _released = true;
                _lastLoggedPressed = 0;
                LogLines.Enqueue("No data from the guitar, released all controls");
            }
        }

        private static void WriteLogLines()
        {
            while (LogLines.TryDequeue(out string line))
            {
                Log(line);
            }
        }

        private static void Log(string message)
        {
            File.AppendAllText(LogPath, $"{DateTime.Now:HH:mm:ss.fff} {message}\n");
        }
    }
}
