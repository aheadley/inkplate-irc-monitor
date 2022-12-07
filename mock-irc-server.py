#!/usr/bin/env python3

import hashlib
import socketserver
import time
import random

PERIOD = 45
MESSAGE_FORMAT = ':mock!~mock@mock.server PRIVMSG #mock-channel :{}\r\n'

ON_CONNECT_SPAM = """
:tantalum.libera.chat NOTICE * :*** Checking Ident
:tantalum.libera.chat NOTICE * :*** Looking up your hostname...
:tantalum.libera.chat NOTICE * :*** Found your hostname: li2292-254.members.linode.com
:tantalum.libera.chat NOTICE * :*** No Ident response
:tantalum.libera.chat 001 aheadley-ink :Welcome to the Libera.Chat Internet Relay Chat Network aheadley-ink
:tantalum.libera.chat 002 aheadley-ink :Your host is tantalum.libera.chat[93.158.237.2/6667], running version solanum-1.0-dev
:tantalum.libera.chat 003 aheadley-ink :This server was created Sat Nov 5 2022 at 00:03:06 UTC
:tantalum.libera.chat 004 aheadley-ink tantalum.libera.chat solanum-1.0-dev DGIMQRSZaghilopsuwz CFILMPQRSTbcefgijklmnopqrstuvz bkloveqjfI
:tantalum.libera.chat 005 aheadley-ink WHOX MONITOR=100 SAFELIST ELIST=CMNTU ETRACE FNC CALLERID=g KNOCK CHANTYPES=# EXCEPTS INVEX CHANMODES=eIbq,k,flj,CFLMPQRSTcgimnprstuz :are supported by this server
:tantalum.libera.chat 005 aheadley-ink CHANLIMIT=#:250 PREFIX=(ov)@+ MAXLIST=bqeI:100 MODES=4 NETWORK=Libera.Chat STATUSMSG=@+ CASEMAPPING=rfc1459 NICKLEN=16 MAXNICKLEN=16 CHANNELLEN=50 TOPICLEN=390 DEAF=D :are supported by this server
:tantalum.libera.chat 005 aheadley-ink TARGMAX=NAMES:1,LIST:1,KICK:1,WHOIS:1,PRIVMSG:4,NOTICE:4,ACCEPT:,MONITOR: EXTBAN=$,ajrxz :are supported by this server
:tantalum.libera.chat 251 aheadley-ink :There are 68 users and 45187 invisible on 28 servers
:tantalum.libera.chat 252 aheadley-ink 36 :IRC Operators online
:tantalum.libera.chat 253 aheadley-ink 122 :unknown connection(s)
:tantalum.libera.chat 254 aheadley-ink 22947 :channels formed
""".strip()

def gen_message():
    now = str(int(time.time()))
    hash = hashlib.md5(now.encode('utf-8')).hexdigest()
    if random.random() > 0.7:
        c = '{} {}'.format(now, hash)
    else:
        c = hash
    return MESSAGE_FORMAT.format(c)

class MockIRCHandler(socketserver.BaseRequestHandler):
    def handle(self):
        print('Connection from: {}'.format(self.client_address[0]))
        for line in ON_CONNECT_SPAM.splitlines():
            self.request.sendall((line.strip() + '\r\n').encode('utf-8'))
        while True:
            m = gen_message()
            print('Sending: {}'.format(m.strip()))
            self.request.sendall(m.encode('utf-8'))
            time.sleep(PERIOD)

if __name__ == '__main__':
    listen_host = '0.0.0.0'
    listen_port = 9660

    with socketserver.TCPServer((listen_host, listen_port), MockIRCHandler) as server:
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            server.shutdown()
