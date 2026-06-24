import json
import socket
import threading
import time
from math import floor
from random import random

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)


target_port = 5005
target_ip = "127.0.0.1"

pair_device_name = None
request_device_name = None


def send_ping():
    global target_ip, target_port, computer_name
    while True:
        message = {"type": "ping", "name": computer_name}
        s.sendto(json.dumps(message).encode(), (target_ip, target_port))
        time.sleep(2)


def receive_response():
    global s, pair_device_name, request_device_name, target_ip, target_port
    while True:
        response, addr = s.recvfrom(4096)
        # print(f"Server response: {response.decode()}")

        data = json.loads(response)

        if data["type"] == "request":
            print(f"Request received: {data['from']}")
            request_device_name = data["from"]
        elif data["type"] == "accept":
            print(f"Accept received: {data['from']}")
            pair_device_name = data["from"]
        elif data["type"] == "list_clients":
            print(data["clients"])
        elif data["type"] == "client_info":
            # Changed ['from'] to ['target']
            print(f"\n[Client info received! Target is at]: {data['addr']}")
            print(f"Target name: {data['target']}\nEnter command: ", end="")
            target_ip = data["addr"][0]
            target_port = data["addr"][1]
        elif data["type"] == "message":
            print(f"Message received: {data['message']}")


computer_name = socket.gethostname() + str(floor(random() * 100))

message = {"type": "connect", "name": computer_name}

s.sendto(json.dumps(message).encode(), (target_ip, target_port))

ack, addr = s.recvfrom(1024)
print(f"Server ack: {ack.decode()}")

if not ack:
    print("No ack received from server")
    exit(1)

ack = json.loads(ack)

if ack["type"] != "connect_ack":
    print("Invalid ack received from server")
    exit(1)

threading.Thread(target=send_ping, daemon=True).start()
threading.Thread(target=receive_response, daemon=True).start()

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
        # Added "name": computer_name
        s.sendto(
            json.dumps(
                {"type": "request", "target": device_name, "name": computer_name}
            ).encode(),
            (target_ip, target_port),
        )

    elif command == "accept":
        print("Accepting device...")
        # Added "name": computer_name
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
