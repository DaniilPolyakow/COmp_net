import socket

client_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

SERVER_IP = "127.0.0.1"
SERVER_PORT = 12345

while True:
    message = input("Введите сообщение (или 'exit' для выхода): ")

    if message.lower() == "exit":
        break

    client_socket.sendto(message.encode(), (SERVER_IP, SERVER_PORT))

    data, server_address = client_socket.recvfrom(1024)
    print("Ответ от сервера:", data.decode())

client_socket.close()