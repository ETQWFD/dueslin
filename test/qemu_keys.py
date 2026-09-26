import socket, time, sys
def send_keys(sock_path, keys, delay=0.8):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    for k in keys:
        s.send(("sendkey %s\n" % k).encode())
        time.sleep(delay)
    s.close()
if __name__ == "__main__":
    send_keys(sys.argv[1], sys.argv[2:])
