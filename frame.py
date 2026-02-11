import socket, struct
import time

def send_frame(sock, payload: bytes):
    packet = struct.pack("!I", len(payload)) + payload
    sock.sendall(packet)

s = socket.create_connection(("localhost", 9000))

# send_frame(s, b"hello")
# resp = s.recv(1024)

# print("RAW:", resp)

# length = struct.unpack("!I", resp[:4])[0]
# print("LEN:", length)
# print("PAYLOAD:", resp[4:4+length])


# send_frame(s, b"one")
# send_frame(s, b"two")
# send_frame(s, b"three")

# resp = s.recv(1024)
# print(resp)

# msg = b"fragmented"

# packet = struct.pack("!I", len(msg)) + msg

# # Send header first
# s.sendall(packet[:2])
# time.sleep(0.5)

# # Send rest
# s.sendall(packet[2:])

for i in range(1000):
    send_frame(s, f"msg{i}".encode())