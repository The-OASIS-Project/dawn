#!/bin/bash
#
# Generate self-signed SSL certificate for DAWN WebUI
#
# This creates a certificate that allows HTTPS access from any device on
# your local network. Required for microphone access via WebUI on non-localhost.
#
# Usage: ./generate_ssl_cert.sh [output_dir]
#   output_dir: Directory to store cert/key files (default: ./ssl)
#
# After running, configure dawn.toml:
#   [webui]
#   https = true
#   ssl_cert_path = "ssl/dawn.crt"
#   ssl_key_path = "ssl/dawn.key"
#

set -e

OUTPUT_DIR="${1:-ssl}"

# Create output directory
mkdir -p "$OUTPUT_DIR"

CERT_FILE="$OUTPUT_DIR/dawn.crt"
KEY_FILE="$OUTPUT_DIR/dawn.key"

# Get the machine's hostname and IP addresses for the certificate
HOSTNAME=$(hostname)
IP_ADDRS=$(hostname -I 2>/dev/null | tr ' ' '\n' | grep -v '^$' || echo "")

# Build Subject Alternative Names (SAN)
SAN="DNS:localhost,DNS:$HOSTNAME"
IP_COUNT=1
for ip in $IP_ADDRS; do
    SAN="$SAN,IP:$ip"
    IP_COUNT=$((IP_COUNT + 1))
done
SAN="$SAN,IP:127.0.0.1"

echo "============================================"
echo "  DAWN WebUI SSL Certificate Generator"
echo "============================================"
echo ""
echo "Hostname: $HOSTNAME"
echo "IP addresses: $IP_ADDRS"
echo "Output directory: $OUTPUT_DIR"
echo ""

# Generate certificate with SAN extension
openssl req -x509 \
    -newkey rsa:2048 \
    -keyout "$KEY_FILE" \
    -out "$CERT_FILE" \
    -days 365 \
    -nodes \
    -subj "/CN=$HOSTNAME/O=DAWN WebUI/OU=Self-Signed" \
    -addext "subjectAltName=$SAN" \
    2>/dev/null

echo "Certificate generated successfully!"
echo ""
echo "Files created:"
echo "  Certificate: $CERT_FILE"
echo "  Private key: $KEY_FILE"
echo ""
echo "============================================"
echo "  Configuration"
echo "============================================"
echo ""
echo "Add to your dawn.toml:"
echo ""
echo "[webui]"
echo "https = true"
echo "ssl_cert_path = \"$CERT_FILE\""
echo "ssl_key_path = \"$KEY_FILE\""
echo ""
echo "============================================"
echo "  Browser Setup"
echo "============================================"
echo ""
echo "Since this is a self-signed certificate, your browser will show"
echo "a security warning. You'll need to:"
echo ""
echo "1. Navigate to https://<your-ip>:3000 (or your configured port)"
echo "2. Click 'Advanced' or 'Show Details'"
echo "3. Click 'Proceed to <hostname> (unsafe)' or 'Accept the Risk'"
echo ""
echo "This is safe for local network use. The certificate is valid for:"
for ip in localhost $HOSTNAME $IP_ADDRS 127.0.0.1; do
    [ -n "$ip" ] && echo "  - https://$ip:3000"
done
echo ""
