import base64
import json
import math
import os
import socket
import struct
import threading
import time
import zlib
from random import randint, random

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

target_port = 5005
target_ip = "172.104.105.130"

pair_device_name = None
request_device_name = None
peer_connected = False

sending_buffer = {}
CHUNK_SIZE = 1024


def copy_to_clipboard(text):
    # Copy using OSC 52 escape sequence (works in modern terminals & over SSH)
    b64_text = base64.b64encode(text.encode()).decode()
    print(f"\033]52;c;{b64_text}\a", end="", flush=True)


def fetch_public_endpoint(sock):
    stun_servers = [
        ("stun.l.google.com", 19302),
        ("stun1.l.google.com", 19302),
        ("stun.cloudflare.com", 3478),  # Excellent secondary fallback
    ]

    prev_timeout = sock.gettimeout()
    sock.settimeout(2.0)
    try:
        for host, server_port in stun_servers:
            try:
                # 1. Resolve STUN server endpoint
                addr_info = socket.getaddrinfo(
                    host, server_port, socket.AF_INET, socket.SOCK_DGRAM
                )
                if not addr_info:
                    continue
                stun_endpoint = addr_info[0][4]

                # 2. Construct the 20-byte STUN Binding Request packet manually
                # Message Type: 0x0001 (Binding Request)
                # Message Length: 0x0000 (No attributes)
                # Magic Cookie: 0x2112A442
                # Transaction ID: 12 random bytes
                tx_id = os.urandom(12)
                request = struct.pack("!HHI12s", 0x0001, 0x0000, 0x2112A442, tx_id)

                # 3. Send the request
                sock.sendto(request, stun_endpoint)

                # 4. Read the response packet
                recv_buffer, _ = sock.recvfrom(1024)
                if len(recv_buffer) < 20:
                    continue

                # 5. Parse the STUN attributes to locate the XOR-MAPPED-ADDRESS (Type: 0x0020)
                index = 20
                length = len(recv_buffer)
                while index + 4 <= length:
                    attr_type, attr_len = struct.unpack(
                        "!HH", recv_buffer[index : index + 4]
                    )
                    index += 4

                    # Check if this attribute is the XOR-MAPPED-ADDRESS (0x0020)
                    if attr_type == 0x0020 and index + attr_len <= length:
                        # Byte 1: Protocol Family (0x01 for IPv4, 0x02 for IPv6)
                        family = recv_buffer[index + 1]
                        if family == 0x01:  # IPv4 Handling
                            # The port is obfuscated by XORing it with the top 16 bits of the Magic Cookie
                            xor_port = struct.unpack(
                                "!H", recv_buffer[index + 2 : index + 4]
                            )[0]
                            actual_port = xor_port ^ 0x2112

                            # The IP address is obfuscated by XORing it directly with the 32-bit Magic Cookie
                            actual_ip = (
                                ((recv_buffer[index + 4] ^ 0x21) << 24)
                                | ((recv_buffer[index + 5] ^ 0x12) << 16)
                                | ((recv_buffer[index + 6] ^ 0xA4) << 8)
                                | (recv_buffer[index + 7] ^ 0x42)
                            )

                            ip_str = (
                                f"{(actual_ip >> 24) & 0xFF}."
                                f"{(actual_ip >> 16) & 0xFF}."
                                f"{(actual_ip >> 8) & 0xFF}."
                                f"{actual_ip & 0xFF}"
                            )
                            return ip_str, actual_port

                    # STUN attributes are padded to multiples of 4 bytes
                    index += (attr_len + 3) & ~3
            except Exception as e:
                print(f"[STUN] Error with {host}: {e}")
                continue
    finally:
        sock.settimeout(prev_timeout)

    return None


def encode_stun_key(ip, port):
    secure_key = f"{ip}:{port}"
    return base64.urlsafe_b64encode(secure_key.encode()).decode()


def decode_stun_key(peer_code):
    peer_code = peer_code.strip()
    if not peer_code:
        return None

    # Allow raw ip:port input as a fallback
    if ":" in peer_code and all(c.isdigit() or c in ".:" for c in peer_code):
        parts = peer_code.split(":")
        try:
            socket.inet_aton(parts[0])
            return parts[0], int(parts[1])
        except Exception:
            pass

    try:
        padding = "=" * (-len(peer_code) % 4)
        decoded = base64.urlsafe_b64decode(peer_code + padding).decode(
            "utf-8", errors="ignore"
        )
        parts = decoded.split(":")
        if len(parts) >= 2:
            ip = parts[0]
            port = int(parts[1])
            socket.inet_aton(ip)
            return ip, port
    except Exception:
        return None
    return None


def send_ping():
    global target_ip, target_port, computer_name
    while True:
        message = {"type": "ping", "name": computer_name}
        s.sendto(json.dumps(message).encode(), (target_ip, target_port))
        time.sleep(2)


def receive_response():
    global s, pair_device_name, request_device_name, target_ip, target_port, peer_connected
    file_buffer = {}

    while True:
        try:
            response, addr = s.recvfrom(4096)
        except Exception:
            continue

        # --- 1. HANDLE BINARY FILE CHUNKS ---
        if response[0] == 1:
            header = response[:17]
            file_data = response[17:]
            _, file_id, chunk_num, check_sum = struct.unpack("!B I I Q", header)

            if check_sum != zlib.crc32(file_data):
                print(
                    f"\n[Warning] Checksum mismatch on chunk {chunk_num}, dropping packet."
                )
                continue

            if file_id in file_buffer:
                file_buffer[file_id]["chunk_buffer"][chunk_num] = file_data
            continue

        # --- 2. HANDLE JSON CONTROL MESSAGES ---
        try:
            data = json.loads(response.decode())
        except json.JSONDecodeError:
            continue

        if data["type"] == "block_checkpoint":
            file_id = data["file_id"]
            if file_id not in file_buffer:
                continue

            block_end = data["block_end"]
            chunk_range = file_buffer[file_id]["chunk_range"]

            # 1. Check if we have every chunk in the current block
            missing_chunks = []
            for i in range(chunk_range[0], block_end + 1):
                if i not in file_buffer[file_id]["chunk_buffer"]:
                    missing_chunks.append(i)

            # 2. If chunks are missing, request them and STOP. Do NOT write to disk.
            if missing_chunks:
                for missing_num in missing_chunks:
                    message = {
                        "type": "request_chunk",
                        "file_id": file_id,
                        "chunk_num": missing_num,
                    }
                    s.sendto(json.dumps(message).encode(), (target_ip, target_port))
                continue  # Exit out. The next checkpoint will re-trigger this check.

            # 3. OPTIMIZATION 3: Batch the disk write
            with open(file_buffer[file_id]["file_path"], "ab") as f:
                # Create a single contiguous byte array in RAM
                ordered_data = bytearray()
                for i in range(chunk_range[0], block_end + 1):
                    ordered_data.extend(file_buffer[file_id]["chunk_buffer"][i])
                    del file_buffer[file_id]["chunk_buffer"][i]  # Free up RAM

                # Dump it to the hard drive in one massive burst
                f.write(ordered_data)

            # 4. Update the range for the next block
            file_buffer[file_id]["chunk_range"][0] = block_end + 1

            # 5. Send the ACK to unfreeze the sender
            s.sendto(
                json.dumps({"type": "file_ack", "file_id": file_id}).encode(),
                (target_ip, target_port),
            )

            # 6. Cleanup if the file is completely done
            if block_end + 1 >= file_buffer[file_id]["total_chunks"]:
                print(
                    f"\n[Transfer Complete]: Received file successfully at {file_buffer[file_id]['file_path']}\nEnter command: ",
                    end="",
                )
                del file_buffer[file_id]

        elif data["type"] == "request":
            print(f"\nRequest received from: {data['from']}\nEnter command: ", end="")
            request_device_name = data["from"]

        elif data["type"] == "accept":
            print(f"\nAccept received from: {data['from']}\nEnter command: ", end="")
            pair_device_name = data["from"]

        elif data["type"] == "list_clients":
            print(f"\n{data['clients']}\nEnter command: ", end="")

        elif data["type"] == "client_info":
            print(f"\n[Client info received! Target is at]: {data['addr']}")
            print(f"Target name: {data['target']}\nEnter command: ", end="")
            target_ip = data["addr"][0]
            target_port = data["addr"][1]

        elif data["type"] == "message":
            print(f"\nMessage received: {data['message']}\nEnter command: ", end="")

        elif data["type"] == "request_chunk":
            file_id = data["file_id"]
            chunk_num = data["chunk_num"]

            if file_id not in sending_buffer:
                continue

            # --- ADD THIS LINE TO COUNT THE DROP ---
            sending_buffer[file_id]["dropped_packets"] += 1

            with open(sending_buffer[file_id]["file_path"], "rb") as file:
                file.seek(chunk_num * CHUNK_SIZE)
                chunk = file.read(CHUNK_SIZE)
                # Pack with 1 to indicate it is binary data
                header = struct.pack(
                    "!B I I Q", 1, file_id, chunk_num, zlib.crc32(chunk)
                )
                s.sendto(header + chunk, (target_ip, target_port))

        elif data["type"] == "sending_file":
            file_id = data["file_id"]
            file_path = data["file_path"]
            total_chunks = data["total_chunks"]

            # Ensure the receive directory exists
            os.makedirs("./Received", exist_ok=True)
            receive_path = os.path.join("./Received", file_path)

            # Wipe existing file if it already exists from a previous failed test
            if os.path.exists(receive_path):
                os.remove(receive_path)

            file_buffer[file_id] = {
                "file_path": receive_path,
                "chunk_range": [0, 0],  # Tracked dynamically by checkpoint logic
                "chunk_buffer": {},
                "total_chunks": total_chunks,
            }

        elif data["type"] == "file_ack":
            file_id = data["file_id"]
            if file_id in sending_buffer:
                sending_buffer[file_id]["ack"] = 1

        elif data["type"] == "ping":
            s.sendto(
                json.dumps({"type": "ping_ack", "name": computer_name}).encode(),
                addr,
            )
            if not peer_connected:
                peer_connected = True
                pair_device_name = data.get("name", "Peer")
                print(
                    f"\n[Hole Punch Successful! Connected to {pair_device_name} at {addr[0]}:{addr[1]}]\nEnter command: ",
                    end="",
                    flush=True,
                )

        elif data["type"] == "ping_ack":
            if not peer_connected:
                peer_connected = True
                pair_device_name = data.get("name", "Peer")
                print(
                    f"\n[Hole Punch Successful! Connected to {pair_device_name} at {addr[0]}:{addr[1]}]\nEnter command: ",
                    end="",
                    flush=True,
                )


def sending_thread(file_id):
    global sending_buffer, s, target_ip, target_port
    file_info = sending_buffer[file_id]
    total_chunks = file_info["total_chunks"]

    overall_start_time = time.perf_counter()

    with open(file_info["file_path"], "rb") as file:
        current_chunk = 0

        while current_chunk < total_chunks:
            block_end = min(current_chunk + 1000, total_chunks)
            chunks_in_block = block_end - current_chunk

            # OPTIMIZATION 1: Read the entire block into RAM at once (~1MB max)
            block_data = file.read(chunks_in_block * CHUNK_SIZE)

            block_start_time = time.perf_counter()

            # Send the block
            for i, chunk_num in enumerate(range(current_chunk, block_end)):
                # Slice the chunk from our RAM buffer
                start_idx = i * CHUNK_SIZE
                end_idx = start_idx + CHUNK_SIZE
                chunk = block_data[start_idx:end_idx]

                header = struct.pack(
                    "!B I I Q", 1, file_id, chunk_num, zlib.crc32(chunk)
                )
                s.sendto(header + chunk, (target_ip, target_port))

                # OPTIMIZATION 2: Batch the network throttling (sleep every 100 packets)
                # if i % 100 == 0:
                # time.sleep(0.001)

            # Announce block is finished
            checkpoint_msg = {
                "type": "block_checkpoint",
                "file_id": file_id,
                "block_end": block_end - 1,
            }
            checkpoint_bytes = json.dumps(checkpoint_msg).encode()
            s.sendto(checkpoint_bytes, (target_ip, target_port))

            # --- THE DEADLOCK FIX ---
            last_checkpoint_time = time.perf_counter()

            # Wait for ACK
            while sending_buffer.get(file_id, {}).get("ack", 0) == 0:
                time.sleep(0.005)  # Tiny sleep so we don't fry the CPU while waiting

                # If 0.5 seconds pass with no ACK, re-trigger the receiver!
                if time.perf_counter() - last_checkpoint_time > 0.5:
                    s.sendto(checkpoint_bytes, (target_ip, target_port))
                    last_checkpoint_time = time.perf_counter()

            # Reset ack and move forward
            sending_buffer[file_id]["ack"] = 0

            # --- THE SPEEDOMETER ---
            block_time = time.perf_counter() - block_start_time
            bytes_sent = chunks_in_block * CHUNK_SIZE
            speed_mbps = (bytes_sent / 1024 / 1024) / block_time

            print(
                f"\r[Transferring]: Block {block_end}/{total_chunks} | Speed: {speed_mbps:.2f} MB/s",
                end="",
                flush=True,
            )

            current_chunk = block_end

    # Final Transfer Stats
    total_time = time.perf_counter() - overall_start_time
    total_mb = (total_chunks * CHUNK_SIZE) / (1024 * 1024)
    dropped = sending_buffer[file_id].get("dropped_packets", 0)  # <--- GET THE COUNT

    print(
        f"\n[Transfer Complete]: {total_mb:.2f} MB sent in {total_time:.2f}s (Avg: {total_mb / total_time:.2f} MB/s)"
    )
    # --- PRINT THE NETWORK STATS ---
    print(f"[Network Stats]: {dropped} packets were dropped and recovered.")
    print("Enter command: ", end="", flush=True)

    del sending_buffer[file_id]


# --- STARTUP AND CONNECTION ---
computer_name = socket.gethostname() + str(math.floor(random() * 100))

# Bind to an ephemeral port to reuse for STUN and P2P communication
s.bind(("0.0.0.0", 0))

print("Fetching public endpoint via Google STUN...")
endpoint = fetch_public_endpoint(s)
if not endpoint:
    print("Unable to reach STUN server. Exiting.")
    exit(1)

public_ip, public_port = endpoint
my_key = encode_stun_key(public_ip, public_port)
copy_to_clipboard(my_key)

print(f"Public Endpoint: {public_ip}:{public_port}")
print("Share this STUN key with your peer (copy & paste):")
print(f"\n{my_key}\n")

# Keep NAT binding alive while waiting for the user to paste the peer's key
waiting_for_peer_key = True
stun_lock = threading.Lock()


def stun_keepalive():
    global my_key
    while waiting_for_peer_key:
        for _ in range(150):
            if not waiting_for_peer_key:
                return
            time.sleep(0.1)
        if waiting_for_peer_key:
            with stun_lock:
                if waiting_for_peer_key:
                    refreshed = fetch_public_endpoint(s)
                    if refreshed:
                        new_key = encode_stun_key(refreshed[0], refreshed[1])
                        if new_key != my_key:
                            my_key = new_key
                            copy_to_clipboard(my_key)
                            print(f"\n[STUN key updated]: {my_key}\nPaste peer STUN key: ", end="", flush=True)


threading.Thread(target=stun_keepalive, daemon=True).start()

while True:
    peer_input = input("Paste peer STUN key: ").strip()
    parsed = decode_stun_key(peer_input)
    if parsed:
        target_ip, target_port = parsed
        break
    print("Invalid key format. Please paste a valid base64 STUN key (or IP:Port).")

waiting_for_peer_key = False
with stun_lock:
    s.settimeout(None)

print(f"Connecting to peer at {target_ip}:{target_port} (punching UDP hole)...")

threading.Thread(target=send_ping, daemon=True).start()
threading.Thread(target=receive_response, daemon=True).start()

# --- MAIN UI LOOP ---
while True:
    command = input("Enter command: ")
    if not command:
        continue

    if command == "list":
        print("Listing devices...")
        s.sendto(
            json.dumps({"type": "list_clients"}).encode(), (target_ip, target_port)
        )

    elif command == "request":
        print("Requesting device...")
        device_name = input("Enter device name: ")
        pair_device_name = device_name
        s.sendto(
            json.dumps(
                {"type": "request", "target": device_name, "name": computer_name}
            ).encode(),
            (target_ip, target_port),
        )

    elif command == "accept":
        print("Accepting device...")
        s.sendto(
            json.dumps(
                {"type": "accept", "target": request_device_name, "name": computer_name}
            ).encode(),
            (target_ip, target_port),
        )
        pair_device_name = request_device_name
        request_device_name = None

    elif command == "message":
        message = input("Enter message: ")
        s.sendto(
            json.dumps(
                {"type": "message", "target": pair_device_name, "message": message}
            ).encode(),
            (target_ip, target_port),
        )

    elif command == "send file":
        file_path = input("Enter file path: ")

        if not os.path.exists(file_path):
            print("File not found.")
            continue

        file_name = os.path.basename(file_path)
        file_id = randint(100000, 99999999)

        # FIXED: Exact math for chunk counts using math.ceil
        total_chunks = math.ceil(os.path.getsize(file_path) / CHUNK_SIZE)

        while file_id in sending_buffer:
            file_id = randint(100000, 99999999)

        message = {
            "type": "sending_file",
            "file_id": file_id,
            "file_path": file_name,
            "total_chunks": total_chunks,
        }
        s.sendto(json.dumps(message).encode(), (target_ip, target_port))

        # ... inside command == "send file" ...
        sending_buffer[file_id] = {
            "file_path": file_path,
            "total_chunks": total_chunks,
            "ack": 0,
            "dropped_packets": 0,  # <--- ADD THIS LINE
        }

        # FIXED: Start the background sending thread!
        print(f"Starting transfer for {file_name}...")
        threading.Thread(target=sending_thread, args=(file_id,), daemon=True).start()
