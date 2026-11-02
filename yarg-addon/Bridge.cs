// SPDX-License-Identifier: LGPL-3.0-or-later
// Receives guitar states from Wiimote Mac over a local connection and presents each
// Wiimote's guitar to YARG as a regular five-fret guitar.
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
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
        private const uint MAGIC = 0x32544757; // "WGT2"
        private const int PACKET_SIZE = 24;
        private const uint VALID_BUTTONS = 0x7FF;
        private const string INTERFACE_NAME = "WiimoteMac";
        // Reports arrive about every 10ms, so this long without one means the Wiimote
        // has gone away. Releases everything so that no note stays held.
        private const double SILENCE_SECONDS = 0.5;

        private static readonly string LogPath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.Personal),
            "Library/Logs/wiimote-mac-addon.log");

        private struct Received
        {
            public ulong address;
            public WiiGuitarState state;
            public long arrivedAt;
        }

        private class Guitar
        {
            public WiiGuitar device;
            public string label;
            public long lastPacketAt;
            public bool released = true;
        }

        private static readonly ConcurrentQueue<Received> Incoming = new ConcurrentQueue<Received>();
        private static readonly ConcurrentQueue<string> LogLines = new ConcurrentQueue<string>();
        private static readonly Dictionary<ulong, Guitar> Guitars = new Dictionary<ulong, Guitar>();

        public static void Initialize()
        {
            Log("Add-on loaded");
            try
            {
                InputSystem.RegisterLayout<WiiGuitar>(
                    matches: new InputDeviceMatcher().WithInterface(INTERFACE_NAME));
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

                    ulong address = 0;
                    for (int i = 0; i < 6; i++)
                    {
                        address = (address << 8) | data[16 + i];
                    }
                    Incoming.Enqueue(new Received
                    {
                        address = address,
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

        // The address in the description is what lets YARG remember which profile goes
        // with which Wiimote, and the end of it tells them apart in YARG's device list.
        private static Guitar AddGuitar(ulong address)
        {
            string text = address.ToString("X12");
            string label = $"Wii Guitar {text.Substring(8, 2)}:{text.Substring(10, 2)}";
            var device = (WiiGuitar) InputSystem.AddDevice(new InputDeviceDescription
            {
                interfaceName = INTERFACE_NAME,
                product = label,
                serial = text,
            });
            LogLines.Enqueue($"Added device '{device.displayName}'");
            var guitar = new Guitar { device = device, label = label };
            Guitars[address] = guitar;
            return guitar;
        }

        // Runs on the main thread just before Unity processes input events, so each
        // guitar is handled in the same update it arrived for.
        private static void OnBeforeUpdate()
        {
            long now = Stopwatch.GetTimestamp();
            while (Incoming.TryDequeue(out var received))
            {
                if (!Guitars.TryGetValue(received.address, out var guitar))
                {
                    guitar = AddGuitar(received.address);
                }

                // Report when it actually arrived, so that YARG judges the hit by the
                // moment of the press and not by when this frame came around.
                double age = (now - received.arrivedAt) / (double) Stopwatch.Frequency;
                InputSystem.QueueStateEvent(guitar.device, received.state, InputState.currentTime - age);
                guitar.lastPacketAt = received.arrivedAt;
                guitar.released = false;
            }

            foreach (var guitar in Guitars.Values)
            {
                if (!guitar.released && (now - guitar.lastPacketAt) / (double) Stopwatch.Frequency > SILENCE_SECONDS)
                {
                    InputSystem.QueueStateEvent(guitar.device, new WiiGuitarState());
                    guitar.released = true;
                    LogLines.Enqueue($"{guitar.label}: no data, released all controls");
                }
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
