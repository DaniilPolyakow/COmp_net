PYTHON=python3

SERVER=udp_server.py
CLIENT=udp_client.py

.PHONY: run-server run-client clean help

run-server:
	$(PYTHON) $(SERVER)

run-client:
	$(PYTHON) $(CLIENT)

clean:
	rm -f *.pyc
	rm -rf __pycache__

