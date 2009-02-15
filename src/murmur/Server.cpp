/* Copyright (C) 2005-2009, Thorvald Natvig <thorvald@natvig.com>

   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.
   - Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.
   - Neither the name of the Mumble Developers nor the names of its
     contributors may be used to endorse or promote products derived from this
     software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "murmur_pch.h"

#include "Player.h"
#include "Channel.h"
#include "ACL.h"
#include "Group.h"
#include "Message.h"
#include "ServerDB.h"
#include "Connection.h"
#include "Server.h"
#include "DBus.h"
#include "PacketDataStream.h"
#include "Meta.h"

uint qHash(const Peer &p) {
	return p.first ^ p.second;
}

LogEmitter::LogEmitter(QObject *p) : QObject(p) {
};

void LogEmitter::addLogEntry(const QString &msg) {
	emit newLogEntry(msg);
};

SslServer::SslServer(QObject *p) : QTcpServer(p) {
}

void SslServer::incomingConnection(int v) {
	QSslSocket *s = new QSslSocket(this);
	s->setSocketDescriptor(v);
	qlSockets.append(s);
}

QSslSocket *SslServer::nextPendingSSLConnection() {
	if (qlSockets.isEmpty())
		return NULL;
	return qlSockets.takeFirst();
}

User::User(Server *p, QSslSocket *socket) : Connection(p, socket), Player() {
	saiUdpAddress.sin_port = 0;
	saiUdpAddress.sin_addr.s_addr = htonl(socket->peerAddress().toIPv4Address());
	saiUdpAddress.sin_family = AF_INET;
}


Server::Server(int snum, QObject *p) : QThread(p) {
	bValid = true;
	iServerNum = snum;

	readParams();
	initialize();

	qtsServer = new SslServer(this);

	connect(qtsServer, SIGNAL(newConnection()), this, SLOT(newClient()), Qt::QueuedConnection);

	if (! qtsServer->listen(qhaBind, usPort)) {
		log("Server: TCP Listen on port %d failed",usPort);
		bValid = false;
	} else {
		log("Server listening on port %d",usPort);
	}

	sUdpSocket = INVALID_SOCKET;

	if (bValid) {
#ifdef Q_OS_UNIX
		sUdpSocket = ::socket(PF_INET, SOCK_DGRAM, 0);
#else
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR,12)
#endif
		sUdpSocket = ::WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
		DWORD dwBytesReturned = 0;
		BOOL bNewBehaviour = FALSE;
		if (WSAIoctl(sUdpSocket, SIO_UDP_CONNRESET, &bNewBehaviour, sizeof(bNewBehaviour), NULL, 0, &dwBytesReturned, NULL, NULL) == SOCKET_ERROR) {
			log("Failed to set SIO_UDP_CONNRESET: %d", WSAGetLastError());
		}
#endif
		if (sUdpSocket == INVALID_SOCKET) {
			log("Failed to create UDP Socket");
			bValid = false;
		} else {
			struct sockaddr_in addr;
			memset(&addr, 0, sizeof(addr));
			addr.sin_family = AF_INET;
			addr.sin_port = htons(usPort);
			addr.sin_addr.s_addr = htonl(qhaBind.toIPv4Address());
			if (::bind(sUdpSocket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR) {
				log("Failed to bind UDP Socket to port %d", usPort);
			} else {
#ifdef Q_OS_UNIX
				int val = IPTOS_PREC_FLASHOVERRIDE | IPTOS_LOWDELAY | IPTOS_THROUGHPUT;
				if (setsockopt(sUdpSocket, IPPROTO_IP, IP_TOS, &val, sizeof(val)))
					log("Server: Failed to set TOS for UDP Socket");
#endif
			}
		}
	}

	connect(this, SIGNAL(tcpTransmit(QByteArray, unsigned int)), this, SLOT(tcpTransmitData(QByteArray, unsigned int)), Qt::QueuedConnection);
	connect(this, SIGNAL(reqSync(unsigned int)), this, SLOT(doSync(unsigned int)));

	for (int i=1;i<5000;i++)
		qqIds.enqueue(i);

	qtTimeout = new QTimer(this);
	connect(qtTimeout, SIGNAL(timeout()), this, SLOT(checkTimeout()));
	qtTimeout->start(15500);

	getBans();
	readChannels();
	readLinks();
	initializeCert();

	if (bValid)
		initRegister();

	bRunning = true;
}

Server::~Server() {
	qrwlUsers.lockForWrite();
	bRunning = false;
	terminate();
	wait();
	qrwlUsers.unlock();
	if (sUdpSocket != INVALID_SOCKET)
#ifdef Q_OS_UNIX
		close(sUdpSocket);
#else
		closesocket(sUdpSocket);
#endif
	clearACLCache();
	log("Stopped");
}

void Server::readParams() {
	qsPassword = Meta::mp.qsPassword;
	usPort = static_cast<unsigned short>(Meta::mp.usPort + iServerNum - 1);
	iTimeout = Meta::mp.iTimeout;
	iMaxBandwidth = Meta::mp.iMaxBandwidth;
	iMaxUsers = Meta::mp.iMaxUsers;
	iDefaultChan = Meta::mp.iDefaultChan;
	qsWelcomeText = Meta::mp.qsWelcomeText;
	qhaBind = Meta::mp.qhaBind;
	qsRegName = Meta::mp.qsRegName;
	qsRegPassword = Meta::mp.qsRegPassword;
	qsRegHost = Meta::mp.qsRegHost;
	qurlRegWeb = Meta::mp.qurlRegWeb;
	qrPlayerName = Meta::mp.qrPlayerName;
	qrChannelName = Meta::mp.qrChannelName;

	QString qsHost = getConf("host", QString()).toString();
	if (! qsHost.isEmpty()) {
		if (! qhaBind.setAddress(qsHost)) {
			QHostInfo hi = QHostInfo::fromName(qsHost);
			foreach(QHostAddress qha, hi.addresses()) {
				if (qha.protocol() == QAbstractSocket::IPv4Protocol) {
					qhaBind = qha;
					break;
				}
			}
			if ((qhaBind == QHostAddress::Any) || (qhaBind.isNull())) {
				log("Lookup of bind hostname %s failed", qPrintable(qsHost));
				qhaBind = Meta::mp.qhaBind;
			}

		}
		log("Binding to address %s", qPrintable(qhaBind.toString()));
	}

	qsPassword = getConf("password", qsPassword).toString();
	usPort = static_cast<unsigned short>(getConf("port", usPort).toUInt());
	iTimeout = getConf("timeout", iTimeout).toInt();
	iMaxBandwidth = getConf("bandwidth", iMaxBandwidth).toInt();
	iMaxUsers = getConf("users", iMaxUsers).toInt();
	iDefaultChan = getConf("defaultchannel", iDefaultChan).toInt();
	qsWelcomeText = getConf("welcometext", qsWelcomeText).toString();

	qsRegName = getConf("registername", qsRegName).toString();
	qsRegPassword = getConf("registerpassword", qsRegPassword).toString();
	qsRegHost = getConf("registerhostname", qsRegHost).toString();
	qurlRegWeb = QUrl(getConf("registerurl", qurlRegWeb.toString()).toString());

	qrPlayerName=QRegExp(getConf("playername", qrPlayerName.pattern()).toString());
	qrChannelName=QRegExp(getConf("channelname", qrChannelName.pattern()).toString());
}

void Server::setLiveConf(const QString &key, const QString &value) {
	QString v = value.trimmed().isEmpty() ? QString() : value;
	int i = v.toInt();
	if (key == "password")
		qsPassword = !v.isNull() ? v : Meta::mp.qsPassword;
	else if (key == "timeout")
		iTimeout = i ? i : Meta::mp.iTimeout;
	else if (key == "bandwidth")
		iMaxBandwidth = i ? i : Meta::mp.iMaxBandwidth;
	else if (key == "users")
		iMaxUsers = i ? i : Meta::mp.iMaxUsers;
	else if (key == "defaultchannel")
		iDefaultChan = i ? i : Meta::mp.iDefaultChan;
	else if (key == "welcometext")
		qsWelcomeText = !v.isNull() ? v : Meta::mp.qsWelcomeText;
	else if (key == "registername")
		qsRegName = !v.isNull() ? v : Meta::mp.qsRegName;
	else if (key == "registerpassword")
		qsRegPassword = !v.isNull() ? v : Meta::mp.qsRegPassword;
	else if (key == "registerhostname")
		qsRegHost = !v.isNull() ? v : Meta::mp.qsRegHost;
	else if (key == "registerurl")
		qurlRegWeb = !v.isNull() ? v : Meta::mp.qurlRegWeb;
	else if (key == "playername")
		qrPlayerName=!v.isNull() ? QRegExp(v) : Meta::mp.qrPlayerName;
	else if (key == "channelname")
		qrChannelName=!v.isNull() ? QRegExp(v) : Meta::mp.qrChannelName;
}


BandwidthRecord::BandwidthRecord() {
	iRecNum = 0;
	iSum = 0;
	for (int i=0;i<N_BANDWIDTH_SLOTS;i++)
		a_iBW[i] = 0;
}

void BandwidthRecord::addFrame(int size) {
	iSum -= a_iBW[iRecNum];
	a_iBW[iRecNum] = static_cast<unsigned char>(size);
	iSum += a_iBW[iRecNum];

	a_qtWhen[iRecNum].restart();

	iRecNum++;
	if (iRecNum == N_BANDWIDTH_SLOTS)
		iRecNum = 0;
}

int BandwidthRecord::bytesPerSec() const {
	quint64 elapsed = a_qtWhen[iRecNum].elapsed();
	return static_cast<int>((iSum * 1000000LL) / elapsed);
}

int BandwidthRecord::onlineSeconds() const {
	return static_cast<int>(qtFirst.elapsed() / 1000000LL);
}

int BandwidthRecord::bandwidth() const {
	int sincelast = static_cast<int>(a_qtWhen[iRecNum].elapsed() / 20000LL);
	int todo = N_BANDWIDTH_SLOTS - sincelast;
	if (todo < 0)
		return 0;

	int sum = 0;
	for (int i=0;i<todo;i++)
		sum += a_iBW[(iRecNum+N_BANDWIDTH_SLOTS - i) % N_BANDWIDTH_SLOTS];
	return (sum*50)/sincelast;
}

void Server::run() {
	qint32 len;
#if defined(__LP64__)
	char encbuff[65536+8];
	char *encrypted = encbuff + 4;
#else
	char encrypted[65536];
#endif
	char buffer[65536];

	quint32 msgType = 0;
	unsigned int uiSession = 0;

	sockaddr_in from;
#ifdef Q_OS_UNIX
	socklen_t fromlen;
#else
	int fromlen;
#endif

	if (sUdpSocket == INVALID_SOCKET)
		return;

	while (bRunning) {
#ifdef Q_OS_DARWIN
		/*
		 * Pthreads on Darwin suck.  They won't allow us to shut down the
		 * server thread while we're in the recvfrom() syscall.
		 *
		 * We use this little hack to loop through our outer loop once in
		 * a while to determine if we should still be running.
		 */
		static struct pollfd fds = {
			sUdpSocket, POLLIN, 0
		};
		int ret = poll(&fds, 1, 1000);

		if (ret == 0) {
			continue;
		} else if (ret == -1) {
			qCritical("poll() failed: %s", strerror(errno));
			break;
		}
#endif

		fromlen = sizeof(from);
		len=::recvfrom(sUdpSocket, encrypted, 65535, 0, reinterpret_cast<struct sockaddr *>(&from), &fromlen);

		if (len == 0) {
			break;
		} else if (len == SOCKET_ERROR) {
			break;
		} else if (len < 6) {
			// 4 bytes crypt header + type + session
			continue;
		}

		QReadLocker rl(&qrwlUsers);

		quint64 key = (static_cast<unsigned long long>(from.sin_addr.s_addr) << 16) ^ from.sin_port;
		PacketDataStream pds(buffer, len - 4);

		User *u = qhPeerUsers.value(key);
		if (u) {
			if (! checkDecrypt(u, encrypted, buffer, len)) {
				continue;
			}
			pds >> msgType >> uiSession;
			if (u->uiSession != uiSession) {
				continue;
			}
		} else {
			// Unknown peer
			foreach(User *usr, qhHostUsers.value(from.sin_addr.s_addr)) {
				pds.rewind();
				if (usr->csCrypt.isValid() && checkDecrypt(usr, encrypted, buffer, len)) {
					pds >> msgType >> uiSession;
					if (usr->uiSession == uiSession) {
						// Every time we relock, reverify users' existance.
						// The main thread might delete the user while the lock isn't held.
						rl.unlock();
						qrwlUsers.lockForWrite();
						if (qhUsers.contains(uiSession)) {
							u = usr;
							qhHostUsers[from.sin_addr.s_addr].remove(u);
							qhPeerUsers.insert(key, u);
							u->saiUdpAddress.sin_port = from.sin_port;
							qrwlUsers.unlock();
							rl.relock();
							if (! qhUsers.contains(uiSession))
								u = NULL;
						}
					}

				}
			}
			if (! u) {
				continue;
			}
			len -= 4;
		}

		if ((msgType != Message::Speex) && (msgType != Message::Ping))
			continue;
		if (! pds.isValid())
			continue;

		if (msgType == Message::Ping) {
			QByteArray qba;
			sendMessage(u, buffer, len, qba);
		} else {
			processMsg(pds, qhUsers.value(uiSession));
		}
	}
}

void Server::fakeUdpPacket(Message *msg, Connection *source) {
	char buffer[65535];
	PacketDataStream pds(buffer, 65535);
	msg->messageToNetwork(pds);
	pds.rewind();

	quint32 msgType;
	int uiSession;

	pds >> msgType >> uiSession;

	QReadLocker rl(&qrwlUsers);
	processMsg(pds, source);
}

bool Server::checkDecrypt(User *u, const char *encrypted, char *plain, unsigned int len) {
	if (u->csCrypt.isValid() && u->csCrypt.decrypt(reinterpret_cast<const unsigned char *>(encrypted), reinterpret_cast<unsigned char *>(plain), len))
		return true;

	if (u->csCrypt.tLastGood.elapsed() > 5000000ULL) {
		if (u->csCrypt.tLastRequest.elapsed() > 5000000ULL) {
			u->csCrypt.tLastRequest.restart();
			emit reqSync(u->uiSession);
		}
	}
	return false;
}

void Server::sendMessage(User *u, const char *data, int len, QByteArray &cache) {
	if ((u->saiUdpAddress.sin_port != 0) && u->csCrypt.isValid()) {
#if defined(__LP64__)
		STACKVAR(char, ebuffer, len+4+16);
		char *buffer = reinterpret_cast<char *>(((reinterpret_cast<quint64>(ebuffer) + 8) & ~7) + 4);
#else
		STACKVAR(char, buffer, len+4);
#endif
		u->csCrypt.encrypt(reinterpret_cast<const unsigned char *>(data), reinterpret_cast<unsigned char *>(buffer), len);
		::sendto(sUdpSocket, buffer, len+4, 0, reinterpret_cast<struct sockaddr *>(& u->saiUdpAddress), sizeof(u->saiUdpAddress));
	} else {
		if (cache.isEmpty())
			cache = QByteArray(data, len);
		emit tcpTransmit(cache,u->uiSession);
	}
}

void Server::processMsg(PacketDataStream &pds, Connection *cCon) {
	User *u = static_cast<User *>(cCon);
	if (! u || (u->sState != Player::Authenticated))
		return;

	Player *p;
	int seq, flags;

	if (u->bMute || u->bSuppressed)
		return;

	BandwidthRecord *bw = & u->bwr;

	pds >> seq;
	pds >> flags;

	// IP + UDP + Crypt + Data
	int packetsize = 20 + 8 + 4 + pds.capacity();
	bw->addFrame(packetsize);

	if (bw->bytesPerSec() > iMaxBandwidth) {
		// Suppress packet.
		return;
	}

	Channel *c = u->cChannel;

	QByteArray qba;

	pds.rewind();
	const char *data = pds.charPtr();
	int len = pds.left();

	if (flags & MessageSpeex::LoopBack) {
		sendMessage(u, data, len, qba);
		return;
	}

	foreach(p, c->qlPlayers) {
		if (! p->bDeaf && ! p->bSelfDeaf && (p != static_cast<Player *>(u)))
			sendMessage(static_cast<User *>(p), data, len, qba);
	}

	if (! c->qhLinks.isEmpty()) {
		QSet<Channel *> chans = c->allLinks();
		chans.remove(c);

		QMutexLocker qml(&qmCache);

		foreach(Channel *l, chans) {
			if (ChanACL::hasPermission(u, l, (flags & MessageSpeex::AltSpeak) ? ChanACL::AltSpeak : ChanACL::Speak, acCache)) {
				foreach(p, l->qlPlayers) {
					if (! p->bDeaf && ! p->bSelfDeaf)
						sendMessage(static_cast<User *>(p), data, len, qba);
				}
			}
		}
	}
}

void Server::log(User *u, const char *format, ...) {
	char buffer[4096];
	va_list ap;
	va_start(ap, format);
	vsnprintf(buffer, 4096, format, ap);
	va_end(ap);

	char fin[4096];
	snprintf(fin, 4096, "<%d:%s(%d)> %s", u->uiSession, qPrintable(u->qsName), u->iId, buffer);

	dblog(fin);
	qWarning("%d => %s", iServerNum, fin);
}

void Server::log(const char *format, ...) {
	char buffer[4096];
	va_list ap;
	va_start(ap, format);
	vsnprintf(buffer, 4096, format, ap);
	va_end(ap);

	dblog(buffer);
	qWarning("%d => %s", iServerNum, buffer);
}

void Server::newClient() {
	forever {
		QSslSocket *sock = qtsServer->nextPendingSSLConnection();
		if (! sock)
			return;

		QHostAddress adr = sock->peerAddress();
		quint32 base = adr.toIPv4Address();

		if (meta->banCheck(adr)) {
			log("Ignoring connection: %s:%d (Global ban)",qPrintable(addressToString(sock->peerAddress())),sock->peerPort());
			sock->disconnectFromHost();
			sock->deleteLater();
			return;
		}

		QPair<quint32,int> ban;

		foreach(ban, qlBans) {
			int mask = 32 - ban.second;
			mask = (1 << mask) - 1;
			if ((base & ~mask) == (ban.first & ~mask)) {
				log("Ignoring connection: %s:%d",qPrintable(addressToString(sock->peerAddress())),sock->peerPort());
				sock->disconnectFromHost();
				sock->deleteLater();
				return;
			}
		}

		sock->setPrivateKey(qskKey);
		sock->setLocalCertificate(qscCert);

		if (qqIds.isEmpty()) {
			sock->disconnectFromHost();
			sock->deleteLater();
			return;
		}

		User *u = new User(this, sock);
		u->uiSession = qqIds.dequeue();

		{
			QWriteLocker wl(&qrwlUsers);
			qhUsers.insert(u->uiSession, u);
			qhHostUsers[htonl(sock->peerAddress().toIPv4Address())].insert(u);
		}

		connect(u, SIGNAL(connectionClosed(QString)), this, SLOT(connectionClosed(QString)));
		connect(u, SIGNAL(message(QByteArray &)), this, SLOT(message(QByteArray &)));
		connect(u, SIGNAL(handleSslErrors(const QList<QSslError> &)), this, SLOT(sslError(const QList<QSslError> &)));

		log(u, "New connection: %s:%d",qPrintable(addressToString(sock->peerAddress())),sock->peerPort());

		sock->startServerEncryption();
	}
}

void Server::sslError(const QList<QSslError> &errors) {
	bool ok = true;
	foreach(QSslError e, errors) {
		switch (e.error()) {
			case QSslError::NoPeerCertificate:
				break;
			default:
				ok = false;
		}
	}
	Connection *c = qobject_cast<User *>(sender());
	if (! c)
		return;

	if (ok)
		c->proceedAnyway();
	else
		c->disconnectSocket();
}

void Server::connectionClosed(QString reason) {
	Connection *c = qobject_cast<Connection *>(sender());
	if (! c)
		return;
	User *u = static_cast<User *>(c);

	log(u, "Connection closed: %s", qPrintable(reason));

	if (u->sState == Player::Authenticated) {
		MessageServerLeave mslMsg;
		mslMsg.uiSession=u->uiSession;
		sendExcept(&mslMsg, c);

		emit playerDisconnected(u);
	}

	{
		QWriteLocker wl(&qrwlUsers);

		qhUsers.remove(u->uiSession);
		qhHostUsers[u->saiUdpAddress.sin_addr.s_addr].remove(u);
		quint64 key = (static_cast<unsigned long long>(u->saiUdpAddress.sin_addr.s_addr) << 16) ^ u->saiUdpAddress.sin_port;
		qhPeerUsers.remove(key);

		if (u->cChannel)
			u->cChannel->removePlayer(u);
	}

	qqIds.enqueue(u->uiSession);
	qhUserTextureCache.remove(u->iId);

	if (u->sState == Player::Authenticated)
		clearACLCache(u);

	u->deleteLater();
}

void Server::message(QByteArray &qbaMsg, Connection *cCon) {
	if (cCon == NULL) {
		cCon = static_cast<Connection *>(sender());
	}
	Message *mMsg = Message::networkToMessage(qbaMsg);

	if (mMsg) {
		dispatch(cCon, mMsg);
		delete mMsg;
	} else {
		cCon->disconnectSocket();
	}
}


void Server::checkTimeout() {
	QList<User *> qlClose;

	qrwlUsers.lockForRead();
	foreach(User *u, qhUsers) {
		if (u->activityTime() > (iTimeout * 1000)) {
			log(u, "Timeout");
			qlClose.append(u);
		}
	}
	qrwlUsers.unlock();
	foreach(User *u, qlClose)
		u->disconnectSocket(true);
}

void Server::tcpTransmitData(QByteArray a, unsigned int id) {
	Connection *c = qhUsers.value(id);
	if (c) {
		c->sendMessage(a);
		c->forceFlush();
	}
}

void Server::doSync(unsigned int id) {
	User *u = qhUsers.value(id);
	if (u) {
		log(u, "Requesting crypt-nonce resync");
		MessageCryptSync mcs;
		u->sendMessage(&mcs);
	}
}

void Server::sendMessage(Connection *c, Message *mMsg) {
	c->sendMessage(mMsg);
}

void Server::sendAll(Message *mMsg) {
	sendExcept(mMsg, NULL);
}

void Server::sendExcept(Message *mMsg, Connection *cCon) {
	foreach(User *u, qhUsers)
		if ((static_cast<Connection *>(u) != cCon) && (u->sState == Player::Authenticated))
			u->sendMessage(mMsg);
}

void Server::removeChannel(Channel *chan, Player *src, Channel *dest) {
	Channel *c;
	Player *p;

	if (dest == NULL)
		dest = chan->cParent;

	chan->unlink(NULL);

	foreach(c, chan->qlChannels) {
		removeChannel(c, src, dest);
	}

	foreach(p, chan->qlPlayers) {
		chan->removePlayer(p);

		MessagePlayerMove mpm;
		mpm.uiSession = 0;
		mpm.uiVictim = p->uiSession;
		mpm.iChannelId = dest->iId;
		sendAll(&mpm);

		playerEnterChannel(p, dest);
	}

	MessageChannelRemove mcr;
	mcr.uiSession = src ? src->uiSession : 0;
	mcr.iId = chan->iId;
	sendAll(&mcr);

	removeChannel(chan);
	emit channelRemoved(chan);

	if (chan->cParent) {
		QWriteLocker wl(&qrwlUsers);
		chan->cParent->removeChannel(chan);
	}

	delete chan;
}

void Server::playerEnterChannel(Player *p, Channel *c, bool quiet) {
	clearACLCache(p);

	if (quiet && (p->cChannel == c))
		return;

	{
		QWriteLocker wl(&qrwlUsers);
		c->addPlayer(p);
	}

	if (quiet)
		return;

	setLastChannel(p);

	bool mayspeak = hasPermission(p, c, ChanACL::Speak);
	bool sup = p->bSuppressed;

	if (! p->bMute) {
		if (mayspeak == sup) {
			// Ok, he can speak and was suppressed, or vice versa
			p->bSuppressed = ! mayspeak;

			MessagePlayerMute mpm;
			mpm.uiSession = 0;
			mpm.uiVictim = p->uiSession;
			mpm.bMute = p->bSuppressed;
			sendAll(&mpm);
		}
	}
	emit playerStateChanged(p);
}

bool Server::hasPermission(Player *p, Channel *c, ChanACL::Perm perm) {
	QMutexLocker qml(&qmCache);
	return ChanACL::hasPermission(p, c, perm, acCache);
}

void Server::clearACLCache(Player *p) {
	QMutexLocker qml(&qmCache);

	if (p) {
		ChanACL::ChanCache *h = acCache.take(p);
		if (h)
			delete h;
	} else {
		foreach(ChanACL::ChanCache *h, acCache)
			delete h;
		acCache.clear();
	}
}

QString Server::addressToString(const QHostAddress &adr) {
	if (Meta::mp.iObfuscate == 0)
		return adr.toString();

	quint32 num = adr.toIPv4Address() ^ Meta::mp.iObfuscate;

	QHostAddress n(num);
	return n.toString();
}

bool Server::validatePlayerName(const QString &name) {
	return (qrPlayerName.exactMatch(name) && (name.length() <= 512));
}

bool Server::validateChannelName(const QString &name) {
	return (qrChannelName.exactMatch(name) && (name.length() <= 512));
}
