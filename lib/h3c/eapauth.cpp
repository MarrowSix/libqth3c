#include "eapauth.h"

QH3C::EapAuth::EapAuth(Profile &profile, const char* dest) :
    profile(profile)
{
    clientSocket = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_PAE));
    if (clientSocket < 0) {
        emit this->socketCreateFailed();
    }

    /*
     * use the struct ifreq to get the device index
     */
    memset(&tempIfreq, 0, sizeof(tempIfreq));
    strncpy(tempIfreq.ifr_name, profile.ethInterface().c_str(),
            IFNAMSIZ-1);
    if ((ioctl(clientSocket, SIOCGIFINDEX, &tempIfreq)) < 0) {}
    interfaceIndex = tempIfreq.ifr_ifindex;

    /*
     * use the struct ifreq to get the device mac address
     */
    memset(&tempIfreq, 0, sizeof(tempIfreq));
    strncpy(tempIfreq.ifr_name, profile.ethInterface().c_str(),
            IFNAMSIZ-1);
    if ((ioctl(clientSocket, SIOCGIFHWADDR, &tempIfreq)) < 0) {}
//    strncpy(macAddress, tempIfreq.ifr_hwaddr.sa_data, ETH_ALEN);
    mempcpy(ethernetHeader.h_source, tempIfreq.ifr_hwaddr.sa_data, ETH_ALEN);
    ethernetHeader.h_proto = ETH_P_PAE;
    mempcpy(ethernetHeader.h_dest, dest, ETH_ALEN);
    /*
     * Version Info, get by wireshark
     */
    versionInfo = VERSION_INFO;

    sadr_ll.sll_ifindex = interfaceIndex;
    sadr_ll.sll_halen = ETH_ALEN;
    mempcpy(sadr_ll.sll_addr, dest, ETH_ALEN);
}

QH3C::EapAuth::~EapAuth() {

}

void QH3C::EapAuth::serveLoop() {
    try {
        if (sendStart(PAE_GROUP_ADDR) < 0) {
        }
        while (true) {
            char *receive = nullptr;
            struct sockaddr saddr;
            int sdrlen = sizeof(saddr);
            ssize_t recvLen = 0;
            try {
                receive = new char[BUFFER_SIZE];
            } catch (std::exception &e) {
                qDebug() << "Allocate Failed " << e.what();
                exit(1);
            }
            recvLen = recvfrom(clientSocket, receive, BUFFER_SIZE, 0, &saddr, (socklen_t *)(&sdrlen));
            eapHandler(receive+sizeof(struct ethhdr), recvLen-sizeof(struct ethhdr));
        }
    } catch (...) {
        qDebug() << "Start EAP Authenticate Failed!";
    }

}

QByteArray QH3C::EapAuth::getEAPOL(int8_t type, const QByteArray &payload) {
    QByteArray tempData;
    QDataStream eapolStream(tempData);
    eapolStream.setByteOrder(QDataStream::BigEndian);
    eapolStream.setVersion(QDataStream::Qt_5_9);

    eapolStream << EAPOL_VERSION << type << (int16_t)payload.size();
    eapolStream.writeRawData(payload.constData(), payload.size());
    return tempData;
}

QByteArray QH3C::EapAuth::getEAP(int8_t code, int8_t identifer, const QByteArray &data, int8_t type = 0) {
    QByteArray tempData;
    QDataStream eapStream(tempData);
    eapStream.setByteOrder(QDataStream::BigEndian);
    eapStream.setVersion(QDataStream::Qt_5_9);
    if (EAP_CODE_SUCCESS == code || EAP_CODE_FAILURE == code) {
        eapStream << code << identifer << (int16_t)(2*sizeof(int8_t) + sizeof(int16_t));
    } else if (EAP_CODE_REQUEST == code || EAP_CODE_RESPONSE == code) {
        eapStream << code << identifer << (int16_t)(2*sizeof(int8_t) + sizeof(int16_t) + data.size()) << type;
        eapStream.writeRawData(data.constData(), data.size());
    }
    return tempData;
}

ssize_t QH3C::EapAuth::sendStart(const char* dest) {
    /*
     * dest addr struct
     */
    char *sendbuff = new char[ETHMINFRAME];
    temp.clear();
    QByteArray tempData = getEAPOL(EAPOL_TYPE_START, temp);

    memset(sendbuff, 0, ETHMINFRAME);
    mempcpy(sendbuff, &ethernetHeader, sizeof(ethhdr));
    mempcpy(sendbuff+sizeof(ethhdr), tempData.constData(), (size_t)tempData.size());

    ssize_t status = sendto(clientSocket, sendbuff, ETHMINFRAME, 0, (const struct sockaddr*)&sadr_ll, sizeof(sadr_ll));
    if (status < 0) {
        qDebug() << "EAP Start Failed...";
    }
    delete[] sendbuff;
    return status;
}

void QH3C::EapAuth::sendLogoff(int8_t id) {
    char *sendbuff = new char[ETHMINFRAME];
    temp.clear();
    QByteArray tempData = getEAPOL(EAPOL_TYPE_LOGOFF, temp);

    memset(sendbuff, 0, ETHMINFRAME);
    mempcpy(sendbuff, &ethernetHeader, sizeof(ethhdr));
    mempcpy(sendbuff+sizeof(ethhdr), tempData.constData(), (size_t)tempData.size());

    sendto(clientSocket, sendbuff, ETHMINFRAME, 0, (const struct sockaddr*)&sadr_ll, sizeof(sadr_ll));
    qDebug() << ">> Sending Logoff";
    delete[] sendbuff;
}

void QH3C::EapAuth::sendResponceId(int8_t id) {
    char *sendbuff = new char[ETHMINFRAME];

    temp.clear();
    temp.append(versionInfo.c_str());
    temp.append(profile.id().c_str());
    QByteArray tempData = getEAPOL(EAPOL_PACKE, getEAP(EAP_CODE_RESPONSE, id, temp, EAP_TYPE_ID));

    memset(sendbuff, 0, ETHMINFRAME);
    mempcpy(sendbuff, &ethernetHeader, sizeof(ethhdr));
    mempcpy(sendbuff+sizeof(ethhdr), tempData.constData(), (size_t)tempData.size());
    sendto(clientSocket, sendbuff, ETHMINFRAME, 0, (const struct sockaddr*)&sadr_ll, sizeof(sadr_ll));
    qDebug() << ">> Sending Response of ID";
    delete[] sendbuff;
}

void QH3C::EapAuth::sendResponceMd5(int8_t id, QByteArray &md5data) {
    char *md5 = new char[MD5LEN];
    mempcpy(md5, profile.password().c_str(), profile.password().size());
    if (profile.password().size() < MD5LEN) {
        memset(md5, 0, MD5LEN-(profile.password().size()));
    }
    char *resp = new char[sizeof(int8_t) + MD5LEN + profile.id().size()];
    resp[0] = MD5LEN;
    for (int i=1; i<=MD5LEN; i++) {
        resp[i] = md5[i] ^ md5data[i];
    }

    mempcpy(resp+sizeof(int8_t)+MD5LEN, profile.id().c_str(), profile.id().size());
    char *sendbuff = new char[ETHMINFRAME];
    temp.clear();
    temp.append(resp, sizeof(int8_t) + MD5LEN + profile.id().size());
    QByteArray tempData = getEAPOL(EAPOL_PACKE, getEAP(EAP_CODE_RESPONSE, id, temp, EAP_TYPE_MD5));

    memset(sendbuff, 0, ETHMINFRAME);
    mempcpy(sendbuff, &ethernetHeader, sizeof(ethhdr));
    mempcpy(sendbuff+ sizeof(ethhdr), tempData.constData(), (size_t)tempData.size());
    sendto(clientSocket, sendbuff, ETHMINFRAME, 0, (const struct sockaddr*)&sadr_ll, sizeof(sadr_ll));
    qDebug() << ">> Sending Response of MD5 Challenge";
    delete[] sendbuff;
}

void QH3C::EapAuth::eapHandler(const char *packet, ssize_t len) {
    /*
     * the section of EAPOL header
     */
    int8_t eapolVersion = 0;
    int8_t eapolType = 0;
    uint16_t eapolLength = 0;
    /*
     * the basic section of EAP header
     */
    int8_t eapCode = 0;
    int8_t eapIdentifier = 0;
    int16_t eapLength = 0;
    QByteArray temp = QByteArray::fromRawData(packet, len);
    QDataStream eapIn(temp);

    eapIn >> eapolVersion >> eapolType >> eapolLength;
    eapIn >> eapCode >> eapIdentifier >> eapLength;
    if (EAPOL_PACKE != eapolType) {
        qDebug() << "Got Unknown EAPOL packet type " << eapolType;
    }

    if (EAP_CODE_SUCCESS == eapCode) {
        qDebug() << "Got EAP Success";
    } else if (EAP_CODE_FAILURE == eapCode) {
        qDebug() << "Got EAP Failure";
        exit(-1);
    } else if (EAP_CODE_RESPONSE == eapCode) {
        qDebug() << "Got Unknown EAP Response";
    } else if (EAP_CODE_REQUEST == eapCode) {
        int8_t eapType = 0;
        int eapLen = eapLength - sizeof(int8_t)*3 - sizeof(int16_t);
        char *eapTypeData = new char[eapLen];
        eapIn >> eapType;
        eapIn.readRawData(eapTypeData, eapLen);
        if (EAP_TYPE_ID == eapType) {
            qDebug() << "Got EAP Request for identity";
            sendResponceId(eapIdentifier);
        } else if (EAP_TYPE_MD5 == eapType) {
            qDebug() << "Got EAP Request for MD5-Challenge";
            int8_t eapType;
            QByteArray md5data;
            eapIn >> eapType >> md5data;
            sendResponceMd5(eapIdentifier, md5data);
        }
    }
}
