#!/usr/bin/env python3
"""
DAP2 Satellite Protocol Test Client

Tests the satellite WebSocket protocol by connecting to the DAWN daemon
and sending satellite_register, satellite_query, and satellite_ping messages.

Usage:
    python3 test_satellite_protocol.py [--host HOST] [--port PORT]

Example:
    python3 test_satellite_protocol.py --host localhost --port 8080
"""

import argparse
import json
import sys
import uuid
import time

try:
    import websocket
except ImportError:
    print("ERROR: websocket-client not installed. Install with:")
    print("  pip3 install websocket-client")
    sys.exit(1)


def create_register_message(satellite_name="Test Satellite", location="test_room"):
    """Create a satellite_register message."""
    return json.dumps({
        "type": "satellite_register",
        "payload": {
            "uuid": str(uuid.uuid4()),
            "name": satellite_name,
            "location": location,
            "tier": 1,
            "capabilities": {
                "local_asr": True,
                "local_tts": True,
                "wake_word": True
            }
        }
    })


def create_query_message(text):
    """Create a satellite_query message."""
    return json.dumps({
        "type": "satellite_query",
        "payload": {
            "text": text
        }
    })


def create_ping_message():
    """Create a satellite_ping message."""
    return json.dumps({
        "type": "satellite_ping"
    })


def on_message(ws, message):
    """Handle incoming WebSocket messages."""
    try:
        data = json.loads(message)
        msg_type = data.get("type", "unknown")

        if msg_type == "satellite_register_ack":
            payload = data.get("payload", {})
            if payload.get("success"):
                print(f"\n✓ Registration successful! Session ID: {payload.get('session_id')}")
            else:
                print(f"\n✗ Registration failed: {payload.get('message', 'Unknown error')}")

        elif msg_type == "satellite_pong":
            print("\n✓ Pong received")

        elif msg_type == "state":
            payload = data.get("payload", {})
            state = payload.get("state", "unknown")
            detail = payload.get("detail", "")
            print(f"\n[State: {state}] {detail}")

        elif msg_type == "stream_start":
            payload = data.get("payload", {})
            stream_id = payload.get("stream_id", 0)
            print(f"\n--- Stream {stream_id} Start ---")

        elif msg_type == "stream_delta":
            payload = data.get("payload", {})
            text = payload.get("delta", "")  # Server sends "delta" not "text"
            print(text, end="", flush=True)

        elif msg_type == "stream_end":
            payload = data.get("payload", {})
            reason = payload.get("reason", "complete")
            print(f"\n--- Stream End ({reason}) ---")

        elif msg_type == "error":
            payload = data.get("payload", {})
            code = payload.get("code", "UNKNOWN")
            message = payload.get("message", "Unknown error")
            print(f"\n✗ Error [{code}]: {message}")

        elif msg_type == "transcript":
            payload = data.get("payload", {})
            role = payload.get("role", "unknown")
            text = payload.get("text", "")
            if role == "satellite_response":
                print(f"\n[Response]: {text}")
            else:
                print(f"\n[{role}]: {text[:100]}{'...' if len(text) > 100 else ''}")

        else:
            # Print unknown message types for debugging
            print(f"\n[{msg_type}]: {json.dumps(data, indent=2)[:200]}")

    except json.JSONDecodeError:
        print(f"\n[Raw]: {message[:200]}")


def on_error(ws, error):
    """Handle WebSocket errors."""
    print(f"\n✗ WebSocket error: {error}")


def on_close(ws, close_status_code, close_msg):
    """Handle WebSocket close."""
    print(f"\n[Disconnected] Status: {close_status_code}, Message: {close_msg}")


def on_open(ws):
    """Handle WebSocket connection established."""
    print("[Connected to DAWN daemon]")


def run_interactive(ws, auto_registered=False):
    """Run interactive satellite test session."""
    print("\n" + "=" * 60)
    print("DAP2 Satellite Protocol Test Client")
    print("=" * 60)
    print("\nCommands:")
    print("  /register [name] [location] - Register satellite")
    print("  /query <text>               - Send query to LLM")
    print("  /ping                       - Send keepalive ping")
    print("  /quit                       - Exit")
    print("  <text>                      - Send query (shortcut)")
    print("=" * 60)

    registered = auto_registered

    while True:
        try:
            user_input = input("\n> ").strip()

            if not user_input:
                continue

            if user_input.lower() in ["/quit", "/exit", "/q"]:
                print("Goodbye!")
                break

            if user_input.lower().startswith("/register"):
                parts = user_input.split(maxsplit=2)
                name = parts[1] if len(parts) > 1 else "Test Satellite"
                location = parts[2] if len(parts) > 2 else "test_room"

                print(f"Registering as '{name}' in '{location}'...")
                ws.send(create_register_message(name, location))
                registered = True
                time.sleep(0.5)  # Wait for response

            elif user_input.lower() == "/ping":
                print("Sending ping...")
                ws.send(create_ping_message())
                time.sleep(0.5)

            elif user_input.lower().startswith("/query "):
                query_text = user_input[7:].strip()
                if not registered:
                    print("Warning: Not registered yet. Use /register first.")
                if query_text:
                    print(f"Sending query: {query_text}")
                    ws.send(create_query_message(query_text))
                else:
                    print("Usage: /query <text>")

            else:
                # Treat as direct query
                if not registered:
                    print("Warning: Not registered yet. Use /register first.")
                print(f"Sending query: {user_input}")
                ws.send(create_query_message(user_input))

        except KeyboardInterrupt:
            print("\nInterrupted. Use /quit to exit.")
        except Exception as e:
            print(f"Error: {e}")


def main():
    parser = argparse.ArgumentParser(description="DAP2 Satellite Protocol Test Client")
    parser.add_argument("--host", default="localhost", help="DAWN daemon host")
    parser.add_argument("--port", type=int, default=8080, help="WebUI port")
    parser.add_argument("--ssl", action="store_true", help="Use wss:// (SSL)")
    parser.add_argument("--insecure", action="store_true",
                        help="Skip SSL certificate verification (for self-signed certs)")
    parser.add_argument("--auto-register", action="store_true",
                        help="Automatically register on connect")
    parser.add_argument("--name", default="Test Satellite", help="Satellite name")
    parser.add_argument("--location", default="test_room", help="Satellite location")
    args = parser.parse_args()

    protocol = "wss" if args.ssl else "ws"
    url = f"{protocol}://{args.host}:{args.port}/"

    print(f"Connecting to {url}...")

    ws = websocket.WebSocketApp(
        url,
        on_open=on_open,
        on_message=on_message,
        on_error=on_error,
        on_close=on_close,
        subprotocols=["dawn-1.0"]
    )

    # Run WebSocket in a thread
    import threading
    import ssl

    sslopt = None
    if args.ssl and args.insecure:
        sslopt = {"cert_reqs": ssl.CERT_NONE, "check_hostname": False}

    ws_thread = threading.Thread(target=lambda: ws.run_forever(sslopt=sslopt), daemon=True)
    ws_thread.start()

    # Wait for connection
    time.sleep(1)

    if not ws.sock or not ws.sock.connected:
        print("Failed to connect to DAWN daemon")
        sys.exit(1)

    # Auto-register if requested
    auto_registered = False
    if args.auto_register:
        print(f"\nAuto-registering as '{args.name}' in '{args.location}'...")
        ws.send(create_register_message(args.name, args.location))
        auto_registered = True
        time.sleep(0.5)

    # Run interactive session
    try:
        run_interactive(ws, auto_registered)
    finally:
        ws.close()


if __name__ == "__main__":
    main()
