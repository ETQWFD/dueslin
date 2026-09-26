import socket, time, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
s.send(b"screendump " + sys.argv[2].encode() + b"\n")
time.sleep(4)
s.close()
print("SHOT_SAVED")
