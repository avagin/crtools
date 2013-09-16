import socket
import sys

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(1)
s.connect((sys.argv[1], int(sys.argv[2])))
msg = s.recv(3)
print msg
if msg != 'RFB':
	sys.exit(1)
