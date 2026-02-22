import socket

server_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

SERVER_IP = "127.0.0.1"
SERVER_PORT = 12345

server_socket.bind((SERVER_IP, SERVER_PORT))

print(f"UDP сервер запущен на {SERVER_IP}:{SERVER_PORT}")

while True:
    data, client_address = server_socket.recvfrom(1024)

    message = data.decode()
    print(f"Получено сообщение: '{message}' от {client_address}")

    server_socket.sendto(data, client_address)