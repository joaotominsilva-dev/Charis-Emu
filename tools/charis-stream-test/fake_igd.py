# SPDX-FileCopyrightText: 2026 Charis
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A pretend home router for the UPnP test: answers SSDP searches and the IGD SOAP calls the
# PC makes (AddPortMapping, GetExternalIPAddress, DeletePortMapping). Port 48010 is "taken by
# another PC" so the fallback to the next port is exercised. GET /mappings shows the state.
import http.server, json, re, socket, struct, sys, threading

HTTP_PORT = 5431
EXTERNAL_IP = '203.0.113.7'
mappings = {}

def local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect(('8.8.8.8', 53))
    ip = s.getsockname()[0]
    s.close()
    return ip

IP = local_ip()

def ssdp():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('', 1900))
    group = struct.pack('4s4s', socket.inet_aton('239.255.255.250'), socket.inet_aton(IP))
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, group)
    while True:
        data, sender = sock.recvfrom(2048)
        text = data.decode(errors='replace')
        if 'M-SEARCH' in text and 'InternetGatewayDevice:1' in text:
            reply = ('HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\n'
                     'ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n'
                     f'LOCATION: http://{IP}:{HTTP_PORT}/root.xml\r\nSERVER: fake/1.0\r\n\r\n')
            sock.sendto(reply.encode(), sender)

ROOT = f'''<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0"><device>
<deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>
<deviceList><device><deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType>
<deviceList><device><deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType>
<serviceList>
<service><serviceType>urn:schemas-upnp-org:service:WANCommonInterfaceConfig:1</serviceType>
<controlURL>/ctl/common</controlURL></service>
<service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>
<serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId>
<controlURL>ctl/ip</controlURL></service>
</serviceList></device></deviceList></device></deviceList></device></root>'''

def value(body, tag):
    m = re.search(f'<{tag}>(.*?)</{tag}>', body, re.S)
    return m.group(1) if m else ''

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *args):
        pass
    def send(self, status, body, chunked=False):
        data = body.encode()
        self.send_response(status)
        self.send_header('Content-Type', 'text/xml')
        if chunked:
            self.send_header('Transfer-Encoding', 'chunked')
            self.end_headers()
            for i in range(0, len(data), 100):
                part = data[i:i + 100]
                self.wfile.write(b'%x\r\n%s\r\n' % (len(part), part))
            self.wfile.write(b'0\r\n\r\n')
        else:
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
    def do_GET(self):
        if self.path == '/root.xml':
            self.send(200, ROOT, chunked=True)
        elif self.path == '/mappings':
            self.send(200, json.dumps(mappings))
        else:
            self.send(404, '')
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0))).decode()
        action = self.headers.get('SOAPAction', '').strip('"').split('#')[-1]
        if self.path != '/ctl/ip':
            return self.send(404, '')
        def answer(inner):
            self.send(200, '<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body>'
                      + inner + '</s:Body></s:Envelope>')
        def error(code):
            self.send(500, '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><s:Fault>'
                      f'<detail><UPnPError><errorCode>{code}</errorCode></UPnPError></detail></s:Fault></s:Body></s:Envelope>')
        if action == 'AddPortMapping':
            port = value(body, 'NewExternalPort')
            if port == '48010':
                return error(718)
            if value(body, 'NewLeaseDuration') != '0':
                return error(725)
            mappings[port] = {'internal': value(body, 'NewInternalClient') + ':' + value(body, 'NewInternalPort'),
                              'protocol': value(body, 'NewProtocol')}
            answer('<u:AddPortMappingResponse xmlns:u="urn:schemas-upnp-org:service:WANIPConnection:1"/>')
        elif action == 'GetExternalIPAddress':
            answer('<u:GetExternalIPAddressResponse xmlns:u="urn:schemas-upnp-org:service:WANIPConnection:1">'
                   f'<NewExternalIPAddress>{EXTERNAL_IP}</NewExternalIPAddress></u:GetExternalIPAddressResponse>')
        elif action == 'DeletePortMapping':
            mappings.pop(value(body, 'NewExternalPort'), None)
            answer('<u:DeletePortMappingResponse xmlns:u="urn:schemas-upnp-org:service:WANIPConnection:1"/>')
        else:
            error(401)

threading.Thread(target=ssdp, daemon=True).start()
print(f'fake IGD on {IP}:{HTTP_PORT}', flush=True)
http.server.ThreadingHTTPServer(('', HTTP_PORT), Handler).serve_forever()
