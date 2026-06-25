import json
import math
import os
import socket
import struct
import threading
import time
from random import randint, random

import xxhash

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

target_port = 5005
target_ip = "127.0.0.1"

pair_device_name = None
request_device_name = None

sending_buffer = {}
CHUNK_SIZE = 1024


def send_ping():
    global target_ip, target_port, computer_name
    while True:
        message = {"type": "ping", "name": computer_name}
        s.sendto(json.dumps(message).encode(), (target_ip, target_port))
        time.sleep(2)


def receive_response():
    global s, pair_device_name, request_device_name, target_ip, target_port
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

            if check_sum != xxhash.xxh3_64_intdigest(file_data):
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
            with open(sending_buffer[file_id]["file_path"], "rb") as file:
                file.seek(chunk_num * CHUNK_SIZE)
                chunk = file.read(CHUNK_SIZE)
                # Pack with 1 to indicate it is binary data
                header = struct.pack(
                    "!B I I Q", 1, file_id, chunk_num, xxhash.xxh3_64_intdigest(chunk)
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
                    "!B I I Q", 1, file_id, chunk_num, xxhash.xxh3_64_intdigest(chunk)
                )
                s.sendto(header + chunk, (target_ip, target_port))

                # OPTIMIZATION 2: Batch the network throttling (sleep every 100 packets)
                if i % 100 == 0:
                    time.sleep(0.001)

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
    print(
        f"\n[Transfer Complete]: {total_mb:.2f} MB sent in {total_time:.2f}s (Avg: {total_mb / total_time:.2f} MB/s)"
    )
    print("Enter command: ", end="", flush=True)

    del sending_buffer[file_id]


# --- STARTUP AND CONNECTION ---
computer_name = socket.gethostname() + str(math.floor(random() * 100))
message = {"type": "connect", "name": computer_name}

print("Connecting to Hub...")
s.settimeout(2.0)
connected = False

for attempt in range(5):
    try:
        s.sendto(json.dumps(message).encode(), (target_ip, target_port))
        ack, addr = s.recvfrom(1024)
        ack_data = json.loads(ack.decode())
        if ack_data.get("type") == "connect_ack":
            connected = True
            break
    except (socket.timeout, json.JSONDecodeError):
        print(f"Attempt {attempt + 1} timed out...")

if not connected:
    print("No ack received from server. Exiting.")
    exit(1)

print("Connected to Server successfully!")
s.settimeout(None)

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

        sending_buffer[file_id] = {
            "file_path": file_path,
            "total_chunks": total_chunks,
            "ack": 0,
        }

        # FIXED: Start the background sending thread!
        print(f"Starting transfer for {file_name}...")
        threading.Thread(target=sending_thread, args=(file_id,), daemon=True).start()
