# UDP Capture Over LAN (FFmpeg)

UDP capture receives an MJPEG byte stream from another PC or process.

## Receiver PC Setup

1. Set capture config:

```ini
capture_method = udp_capture
udp_ip = 0.0.0.0
udp_port = 1234
detection_resolution = 640
capture_fps = 60
```

2. Open the UDP port in Windows Firewall. Run PowerShell as Administrator:

```powershell
New-NetFirewallRule -DisplayName "Sunone UDP Capture 1234" -Direction Inbound -Protocol UDP -LocalPort 1234 -Action Allow
```

3. Find the receiver PC local IP address:

```powershell
ipconfig
```

Use the `IPv4 Address` from the receiver PC as `RECEIVER_IP` in the sender commands below.

## Sender PC Examples

Desktop capture:

```bash
ffmpeg -f gdigrab -framerate 60 -i desktop -vf scale=640:640 -vcodec mjpeg -q:v 5 -f mjpeg udp://RECEIVER_IP:1234
```

Lower bandwidth or weaker sender PC. Use this with `detection_resolution = 320`:

```bash
ffmpeg -f gdigrab -framerate 30 -i desktop -vf scale=320:320 -vcodec mjpeg -q:v 7 -f mjpeg udp://RECEIVER_IP:1234
```

OBS Virtual Camera:

```bash
ffmpeg -f dshow -i video="OBS Virtual Camera" -vf scale=640:640 -vcodec mjpeg -q:v 5 -f mjpeg udp://RECEIVER_IP:1234
```

Match the FFmpeg `scale` size to `detection_resolution` when possible. Valid detection resolutions are `160`, `320`, and `640`; unsupported values fall back to `320`.

## UDP Capture Does Not Receive Frames

1. `capture_method = udp_capture`
2. `udp_port` matches the sender command.
3. The receiver firewall allows inbound UDP on that port.
4. The sender streams MJPEG to the receiver PC IP, not to `0.0.0.0`.
5. `udp_ip = 0.0.0.0` while diagnosing, so packets from any sender are accepted.

`udp_ip` filters the sender address; it is not the local bind address.

Related docs:

- [Capture config](../config.md#capture)
- [Capture diagnostics](capture-diagnostics.md)
