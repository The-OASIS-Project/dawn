# MQTT Setup Guide -- The OASIS Project

This guide covers the complete MQTT setup for all OASIS components: **DAWN** (AI assistant), **MIRAGE** (HUD), and **STAT** (telemetry). Configure the broker once, then point each client at it.

## Table of Contents

- [1. Install Mosquitto](#1-install-mosquitto)
- [2. Authentication](#2-authentication)
- [3. TLS Certificates](#3-tls-certificates)
- [4. Broker Configuration](#4-broker-configuration)
- [5. Client Configuration](#5-client-configuration)
  - [DAWN](#dawn)
  - [MIRAGE](#mirage)
  - [STAT](#stat)
  - [stat-monitor (GUI)](#stat-monitor-gui)
- [6. Verification](#6-verification)
- [7. Troubleshooting](#7-troubleshooting)

---

## 1. Install Mosquitto

```bash
sudo apt update && sudo apt install -y mosquitto mosquitto-clients
sudo systemctl enable mosquitto
sudo systemctl start mosquitto
```

Verify it's running:
```bash
systemctl status mosquitto
```

By default, Mosquitto listens on `127.0.0.1:1883` with no authentication. Anything that can publish on the broker can make DAWN act as the local user, so authentication (a username and password, with ACLs) is required as soon as anything other than the DAWN daemon itself can reach the broker (other OASIS components, other accounts on the machine, other hosts), and TLS is required when that traffic crosses a network. The steps below add both.

---

## 2. Authentication

Create a password file with a shared user for all OASIS components:

```bash
sudo mosquitto_passwd -c /etc/mosquitto/passwd oasis
# Enter a password when prompted
```

To add additional users (e.g., separate credentials per component):
```bash
sudo mosquitto_passwd /etc/mosquitto/passwd dawn
sudo mosquitto_passwd /etc/mosquitto/passwd mirage
```

> **Note**: Use `-c` only for the first user (it creates the file). Subsequent users use the command without `-c` to append.

---

## 3. TLS Certificates

Generate a private CA and broker certificate for encrypted MQTT.

### Create a certificate directory

```bash
sudo mkdir -p /etc/mosquitto/certs
cd /etc/mosquitto/certs
```

### Generate CA (Certificate Authority)

```bash
sudo openssl genrsa -out ca.key 2048
sudo openssl req -new -x509 -days 3650 -key ca.key -out ca.crt \
  -subj "/CN=OASIS MQTT CA"
```

> **Tip**: If you already have a private CA from DAWN's SSL setup (`ssl/ca.crt`), you can reuse it here instead of creating a new one. Copy it to `/etc/mosquitto/certs/ca.crt`.

### Generate broker certificate

```bash
sudo openssl genrsa -out server.key 2048
sudo openssl req -new -key server.key -out server.csr \
  -subj "/CN=localhost"
```

### Sign the broker certificate

The broker cert needs Subject Alternative Names (SANs) for all hostnames/IPs clients will use to connect:

```bash
# Get your machine's LAN IP
JETSON_IP=$(hostname -I | awk '{print $1}')

# Create SAN extension file
echo "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:${JETSON_IP}" | \
  sudo tee /etc/mosquitto/certs/san.cnf

# Sign the certificate
sudo openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key \
  -CAcreateserial -out server.crt -days 3650 \
  -extfile /etc/mosquitto/certs/san.cnf
```

### Set permissions

```bash
sudo chown mosquitto:mosquitto /etc/mosquitto/certs/*
sudo chmod 640 /etc/mosquitto/certs/*.key
```

---

## 4. Broker Configuration

Create the configuration file:

```bash
sudo tee /etc/mosquitto/conf.d/oasis.conf << 'EOF'
# Authentication
allow_anonymous false
password_file /etc/mosquitto/passwd

# Plain MQTT on localhost only (for local-only setups without TLS)
listener 1883 127.0.0.1

# TLS listener on all interfaces
listener 8883
certfile /etc/mosquitto/certs/server.crt
keyfile /etc/mosquitto/certs/server.key
cafile /etc/mosquitto/certs/ca.crt
EOF
```

Restart and verify:

```bash
sudo systemctl restart mosquitto
systemctl status mosquitto

# Verify both listeners are active
ss -tlnp | grep -E '1883|8883'
```

You should see:
- `127.0.0.1:1883` -- authenticated plaintext (localhost only)
- `0.0.0.0:8883` -- authenticated + TLS (all interfaces)

> **Security note**: Once TLS is working, you can remove the plain listener by deleting the `listener 1883` line. For development, keeping both is convenient.

---

## 5. Client Configuration

All OASIS components support the same security features. Configure each one with the credentials created in step 2 and the CA certificate from step 3.

### DAWN

**Connection settings** in `dawn.toml`:

```toml
[mqtt]
enabled = true
broker = "127.0.0.1"
port = 8883
tls = true
tls_ca_cert = "/etc/mosquitto/certs/ca.crt"
# tls_cert_path = ""  # Only for mutual TLS (broker requires client cert)
# tls_key_path = ""   # Only for mutual TLS
```

**Credentials** in `secrets.toml`:

```toml
[secrets]
mqtt_username = "oasis"
mqtt_password = "your-password"
```

Or via environment variables:
```bash
export DAWN_MQTT_TLS=true
export DAWN_MQTT_PORT=8883
export DAWN_MQTT_TLS_CA_CERT="/etc/mosquitto/certs/ca.crt"
```

**Expected log output:**
```
[INFO] MQTT authentication configured for user: oasis
[INFO] MQTT TLS enabled (CA: /etc/mosquitto/certs/ca.crt)
[INFO] Connected to local MQTT server.
```

### MIRAGE

**Connection settings** in `config.json`:

```json
"mqtt": {
   "host": "127.0.0.1",
   "port": 8883,
   "tls": true,
   "tls_ca_cert": "/etc/mosquitto/certs/ca.crt"
}
```

**Credentials** in `secrets.json`:

```json
{
   "mqtt_username": "oasis",
   "mqtt_password": "your-password"
}
```

**Expected log output:**
```
[INFO] MQTT authentication configured for user: oasis
[INFO] MQTT TLS enabled (CA: /etc/mosquitto/certs/ca.crt)
[INFO] Connecting to MQTT broker at 127.0.0.1:8883
[INFO] Mosquitto successfully connected. Subscribing to...
```

### STAT

**Service mode** -- edit `/etc/oasis-stat/stat.conf` (or the local `config/stat.conf`):

```bash
MQTT_HOST=127.0.0.1
MQTT_PORT=8883
MQTT_TOPIC=stat
MQTT_USERNAME=oasis
MQTT_PASSWORD=your-password
MQTT_TLS=true
MQTT_CA_CERT=/etc/mosquitto/certs/ca.crt
```

**CLI mode:**

```bash
oasis-stat --mqtt-host 127.0.0.1 --mqtt-port 8883 \
  --mqtt-username oasis --mqtt-password your-password \
  --mqtt-ca-cert /etc/mosquitto/certs/ca.crt
```

**Expected log output:**
```
MQTT: Authentication configured for user: oasis
MQTT: TLS enabled (CA: /etc/mosquitto/certs/ca.crt)
MQTT: Connecting to broker at 127.0.0.1:8883
MQTT: Connected to broker
```

### AURA (Helmet Sensors -- ESP32)

AURA is configured at compile time via `arduino_secrets.h`:

```c
#define SECRET_MQTT_BROKER "192.168.1.100"  // Jetson IP ("" = auto-detect via gateway)
#define SECRET_MQTT_USERNAME "oasis"
#define SECRET_MQTT_PASSWORD "your-password"

// CA certificate in PEM format (paste from: cat /etc/mosquitto/certs/ca.crt)
#define SECRET_MQTT_CA_CERT \
  "-----BEGIN CERTIFICATE-----\n" \
  "MIIDazCCAlOgAwIBAgIU...\n" \
  "...\n" \
  "-----END CERTIFICATE-----\n"
```

Enable TLS in `config.h`:
```c
#define ENABLE_MQTT
#define MQTT_USE_TLS
```

> **Note**: The CA certificate must be embedded as a C string constant since ESP32 has no filesystem. Copy the PEM content with `\n` at the end of each line. The port auto-switches from 1883 to 8883 when TLS is enabled.

**Expected serial output:**
```
MQTT: Authentication configured for user: oasis
MQTT: TLS enabled with CA certificate
MQTT: Auto-switched to TLS port 8883
MQTT: Connecting to broker at 192.168.1.100:8883
MQTT: Connected to broker
MQTT: Subscribing to topic: helmet
```

### stat-monitor (GUI)

```bash
python3 stat_monitor.py --host 127.0.0.1 --port 8883 \
  --username oasis --password your-password \
  --ca-cert /etc/mosquitto/certs/ca.crt
```

---

## 6. Verification

### Test with mosquitto CLI tools

```bash
# Test authenticated TLS connection (subscribe in background)
mosquitto_sub -h 127.0.0.1 -p 8883 \
  -u oasis -P 'your-password' \
  --cafile /etc/mosquitto/certs/ca.crt \
  -t 'test' -v &

# Publish a test message
mosquitto_pub -h 127.0.0.1 -p 8883 \
  -u oasis -P 'your-password' \
  --cafile /etc/mosquitto/certs/ca.crt \
  -t 'test' -m 'hello from OASIS'

# You should see: test hello from OASIS
kill %1  # Stop the subscriber
```

### Verify cross-component communication

1. Start DAWN and MIRAGE
2. Check MIRAGE logs for: `Published hud status: online`
3. Check DAWN logs for: `Dawn is now ONLINE` or HUD discovery messages
4. Check MIRAGE logs for: `Received discovery request, republishing capabilities`

### Monitor all MQTT traffic (debugging)

```bash
mosquitto_sub -h 127.0.0.1 -p 8883 \
  -u oasis -P 'your-password' \
  --cafile /etc/mosquitto/certs/ca.crt \
  -t '#' -v
```

This subscribes to all topics. You'll see `hud/status`, `dawn/status`, `stat`, `hud/discovery/*`, and any sensor data flowing between components.

---

## 7. Troubleshooting

### Connection refused

```
Error: Connection refused
```

- Check Mosquitto is running: `systemctl status mosquitto`
- Check the port is listening: `ss -tlnp | grep -E '1883|8883'`
- Check the broker log: `sudo tail -20 /var/log/mosquitto/mosquitto.log`

### Authentication failure

```
Connection Refused: not authorised
```

- Verify credentials match the password file: `sudo mosquitto_passwd -U /etc/mosquitto/passwd`
- Check that `allow_anonymous false` is set in broker config
- Ensure the username/password in your component config match exactly

### TLS certificate errors

```
Error: Unable to load server certificate
OpenSSL Error: No such file or directory
```

- Verify cert files exist: `ls -la /etc/mosquitto/certs/`
- Check permissions: `sudo chown mosquitto:mosquitto /etc/mosquitto/certs/*`
- Regenerate if missing (see [step 3](#3-tls-certificates))

```
SSL routines: certificate verify failed
```

- The CA cert used by the client must be the same CA that signed the server cert
- Check SANs match the hostname you're connecting to: `openssl x509 -in /etc/mosquitto/certs/server.crt -text | grep -A1 "Subject Alternative"`
- If you changed the machine's IP, regenerate the server cert with updated SANs

### Mosquitto won't start after config changes

```bash
# Test config syntax
sudo mosquitto -c /etc/mosquitto/mosquitto.conf -v

# Common issues:
# - Missing cert files (check paths in oasis.conf)
# - Password file not found
# - Port already in use
```

### No data flowing between components

- Subscribe to all topics to see what's being published: `mosquitto_sub -t '#' -v ...`
- Check that components are subscribing to the correct topics (MIRAGE subscribes to `hud`, `helmet`, `stat`, armor topics)
- Check component status: look for `hud/status` and `dawn/status` messages
- Verify LWT is set: disconnect a component and watch for its offline status message

### Auth-only setup (no TLS)

For localhost-only deployments where encryption isn't needed, use port 1883 with just authentication:

**Broker** -- keep the `listener 1883 127.0.0.1` line in `oasis.conf` but remove or comment out the `listener 8883` block.

**Clients** -- set port to 1883, set `tls` to false (or omit it), and provide only username/password.

---

## MQTT Topics Reference

| Topic | Publisher | Subscriber | Description |
|-------|-----------|------------|-------------|
| `hud` | DAWN | MIRAGE | HUD element control commands |
| `helmet` | DAWN | MIRAGE | Forwarded to serial (faceplate control) |
| `stat` | STAT | MIRAGE | Battery, system metrics, fan data |
| `dawn` | DAWN | MIRAGE | AI state and audio commands |
| `hud/status` | MIRAGE | DAWN | HUD online/offline status (LWT) |
| `dawn/status` | DAWN | MIRAGE | DAWN online/offline status (LWT) |
| `hud/discovery/elements` | MIRAGE | DAWN | Available HUD elements |
| `hud/discovery/modes` | MIRAGE | DAWN | Available HUD screen modes |
| `hud/discovery/request` | DAWN | MIRAGE | Trigger capability republish |
| `shoulder/*`, `chest`, etc. | SPARK | MIRAGE | Armor component status |

For the full protocol specification, see [OASIS Communications Protocol (OCP v1.5)](OASIS_COMMUNICATIONS_PROTOCOL.md).
