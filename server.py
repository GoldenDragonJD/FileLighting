import json
import socket
import threading
import time

clients = {}
Max_timeout = 10


def garbage_collect():
    global clients
    while True:
        time.sleep(2)

        clients_to_remove = []
        for client_name, client_data in clients.items():
            if time.time() - client_data["last_ping"] > Max_timeout:
                clients_to_remove.append(client_name)

        for client_name in clients_to_remove:
            del clients[client_name]
            print(f"Client {client_name} removed due to timeout")


server_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

server_socket.bind(("0.0.0.0", 5005))

threading.Thread(target=garbage_collect, daemon=True).start()

while True:
    data, addr = server_socket.recvfrom(4096)

    try:
        json_data = json.loads(data.decode())

        if json_data["type"] == "connect":
            client_name = json_data["name"]
            clients[client_name] = {"addr": addr, "last_ping": time.time()}
            print(f"Client {client_name} connected from {addr}")

            # Send the ACK
            server_socket.sendto(json.dumps({"type": "connect_ack"}).encode(), addr)
            # Add this line to prove it fired!
            print(f"--> Sent connect_ack back to {addr}")

        elif json_data["type"] == "ping":
            client_name = json_data["name"]
            if client_name in clients:
                clients[client_name]["last_ping"] = time.time()
            else:
                clients[client_name] = {"addr": addr, "last_ping": time.time()}
            response = json.dumps({"type": "ping_ack"})
            server_socket.sendto(response.encode(), addr)

        elif json_data["type"] == "list_clients":
            client_list = list(clients.keys())
            response = json.dumps({"type": "list_clients", "clients": client_list})
            server_socket.sendto(response.encode(), addr)
            print(f"List of clients sent to {addr}")

        elif json_data["type"] == "request":
            target_client = json_data["target"]
            requester_name = json_data.get(
                "name"
            )  # Get the string name of the requester

            if target_client in clients:
                message = {
                    "type": "request",
                    "target": target_client,
                    "from": requester_name,  # Send the string name, NOT the tuple addr!
                }
                server_socket.sendto(
                    json.dumps(message).encode(), clients[target_client]["addr"]
                )
                print(f"Request sent to {target_client} from {requester_name}")
            else:
                message = {"type": "request_error"}
                server_socket.sendto(json.dumps(message).encode(), addr)
                print(f"Client {target_client} not found")

        elif json_data["type"] == "accept":
            target_client = json_data["target"]  # The person who originally asked
            acceptor_name = json_data.get("name")  # The person accepting

            if target_client in clients:
                # Grab the original requester's address
                requester_addr = clients[target_client]["addr"]
                print(f"Client {acceptor_name} accepted request from {target_client}")

                # 1. Send the Acceptor's info back to the original Requester
                message_to_requester = {
                    "type": "client_info",
                    "target": acceptor_name,
                    "addr": addr,
                }
                server_socket.sendto(
                    json.dumps(message_to_requester).encode(), requester_addr
                )

                # 2. Send the original Requester's info to the Acceptor
                message_to_acceptor = {
                    "type": "client_info",
                    "target": target_client,
                    "addr": requester_addr,
                }
                server_socket.sendto(json.dumps(message_to_acceptor).encode(), addr)
            else:
                print(f"Client {target_client} not found anymore")

    except json.JSONDecodeError:
        pass
