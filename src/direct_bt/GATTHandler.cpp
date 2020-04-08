/*
 * Author: Sven Gothel <sgothel@jausoft.com>
 * Copyright (c) 2020 Gothel Software e.K.
 * Copyright (c) 2020 ZAFENA AB
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <cstring>
#include <string>
#include <memory>
#include <cstdint>
#include <vector>
#include <cstdio>

#include  <algorithm>

extern "C" {
    #include <unistd.h>
    #include <sys/socket.h>
    #include <poll.h>
    #include <signal.h>
}

// #define PERF_PRINT_ON 1
// #define VERBOSE_ON 1

#include "L2CAPIoctl.hpp"
#include "GATTNumbers.hpp"

#include "GATTHandler.hpp"

#include "dbt_debug.hpp"

using namespace direct_bt;

#define STATE_ENUM(X) \
    X(Error) \
    X(Disconnected) \
    X(Connecting) \
    X(Connected) \
    X(RequestInProgress) \
    X(DiscoveringCharacteristics) \
    X(GetClientCharaceristicConfiguration) \
    X(WaitWriteResponse) \
    X(WaitReadResponse)

#define CASE_TO_STRING(V) case V: return #V;

std::string GATTHandler::getStateString(const State state) {
    switch(state) {
        STATE_ENUM(CASE_TO_STRING)
        default: ; // fall through intended
    }
    return "Unknown State";
}

GATTHandler::State GATTHandler::validateState() {
    const bool a = Disconnected < state;
    const bool b = l2cap->isOpen();
    const bool c = L2CAPComm::State::Disconnected < l2cap->getState();
    if( a || b || c ) {
        // something is open ...
        if( a != b || a != c || b != c ) {
            throw InvalidStateException("Inconsistent open state: GattHandler "+getStateString()+
                    ", l2cap[open "+std::to_string(b)+", state "+l2cap->getStateString()+"]", E_FILE_LINE);
        }
    }
    return state;
}

std::shared_ptr<GATTNotificationListener> GATTHandler::setGATTNotificationListener(std::shared_ptr<GATTNotificationListener> l) {
    std::shared_ptr<GATTNotificationListener> o = gattNotificationListener;
    gattNotificationListener = l;
    return o;
}

std::shared_ptr<GATTIndicationListener> GATTHandler::setGATTIndicationListener(std::shared_ptr<GATTIndicationListener> l, bool sendConfirmation) {
    std::shared_ptr<GATTIndicationListener> o = gattIndicationListener;
    gattIndicationListener = l;
    sendIndicationConfirmation = sendConfirmation;
    return o;
}

void GATTHandler::l2capReaderThreadImpl() {
    l2capReaderShallStop = false;
    l2capReaderRunning = true;
    INFO_PRINT("l2capReaderThreadImpl Started");

    while( !l2capReaderShallStop ) {
        int len;
        if( Disconnected >= validateState() ) {
            // not open
            perror("GATTHandler::l2capReaderThread: Not connected");
            l2capReaderShallStop = true;
            break;
        }

        len = l2cap->read(rbuffer.get_wptr(), rbuffer.getSize(), Defaults::L2CAP_READER_THREAD_POLL_TIMEOUT);
        if( 0 < len ) {
            const AttPDUMsg * attPDU = AttPDUMsg::getSpecialized(rbuffer.get_ptr(), len);
            const AttPDUMsg::Opcode opc = attPDU->getOpcode();

            if( AttPDUMsg::Opcode::ATT_HANDLE_VALUE_NTF == opc ) {
                const AttHandleValueRcv * a = static_cast<const AttHandleValueRcv*>(attPDU);
                INFO_PRINT("GATTHandler: NTF: %s", a->toString().c_str());
                if( nullptr != gattNotificationListener ) {
                    GATTCharacterisicsDeclRef decl = findCharacterisics(a->getHandle());
                    gattNotificationListener->notificationReceived(this->l2cap->getDevice(), decl, std::shared_ptr<const AttHandleValueRcv>(a));
                    attPDU = nullptr;
                }
            } else if( AttPDUMsg::Opcode::ATT_HANDLE_VALUE_IND == opc ) {
                const AttHandleValueRcv * a = static_cast<const AttHandleValueRcv*>(attPDU);
                INFO_PRINT("GATTHandler: IND: %s, sendIndicationConfirmation %d", a->toString().c_str(), sendIndicationConfirmation);
                bool cfmSent = false;
                if( sendIndicationConfirmation ) {
                    AttHandleValueCfm cfm;
                    cfmSent = send(cfm);
                    DBG_PRINT("GATTHandler: CFM send: %s, confirmationSent %d", cfm.toString().c_str(), cfmSent);
                }
                if( nullptr != gattIndicationListener ) {
                    GATTCharacterisicsDeclRef decl = findCharacterisics(a->getHandle());
                    gattIndicationListener->indicationReceived(this->l2cap->getDevice(), decl, std::shared_ptr<const AttHandleValueRcv>(a), cfmSent);
                    attPDU = nullptr;
                }
            } else if( AttPDUMsg::Opcode::ATT_MULTIPLE_HANDLE_VALUE_NTF == opc ) {
                // FIXME TODO ..
                INFO_PRINT("GATTHandler: MULTI-NTF: %s", attPDU->toString().c_str());
            } else {
                attPDURing.putBlocking( std::shared_ptr<const AttPDUMsg>( attPDU ) );
                attPDU = nullptr;
            }
            if( nullptr != attPDU ) {
                delete attPDU; // free unhandled PDU
            }
        } else if( ETIMEDOUT != errno && !l2capReaderShallStop ) { // expected exits
            perror("GATTHandler::l2capReaderThread: l2cap read error");
        }
    }

    INFO_PRINT("l2capReaderThreadImpl Ended");
    l2capReaderRunning = false;
}

static void gatthandler_sigaction(int sig, siginfo_t *info, void *ucontext) {
    INFO_PRINT("GATTHandler.sigaction: sig %d, info[code %d, errno %d, signo %d, pid %d, uid %d, fd %d]",
            sig, info->si_code, info->si_errno, info->si_signo,
            info->si_pid, info->si_uid, info->si_fd);

    if( SIGINT != sig ) {
        return;
    }
    {
        struct sigaction sa_setup;
        bzero(&sa_setup, sizeof(sa_setup));
        sa_setup.sa_handler = SIG_DFL;
        sigemptyset(&(sa_setup.sa_mask));
        sa_setup.sa_flags = 0;
        if( 0 != sigaction( SIGINT, &sa_setup, NULL ) ) {
            perror("GATTHandler.sigaction: Resetting sighandler");
        }
    }
}

bool GATTHandler::connect() {
    if( Disconnected < validateState() ) {
        // already open
        DBG_PRINT("GATTHandler.connect: Already open");
        return true;
    }
    state = static_cast<GATTHandler::State>(l2cap->connect());

    if( Disconnected >= validateState() ) {
        DBG_PRINT("GATTHandler.connect: Could not connect");
        return false;
    }

    {
        struct sigaction sa_setup;
        bzero(&sa_setup, sizeof(sa_setup));
        sa_setup.sa_sigaction = gatthandler_sigaction;
        sigemptyset(&(sa_setup.sa_mask));
        sa_setup.sa_flags = SA_SIGINFO;
        if( 0 != sigaction( SIGINT, &sa_setup, NULL ) ) {
            perror("GATTHandler.connect: Setting sighandler");
        }
    }
    l2capReaderThread = std::thread(&GATTHandler::l2capReaderThreadImpl, this);

    const uint16_t mtu = exchangeMTU(ClientMaxMTU);;
    if( 0 < mtu ) {
        serverMTU = mtu;
    } else {
        WARN_PRINT("Ignoring zero serverMTU.");
    }
    usedMTU = std::min((int)ClientMaxMTU, (int)serverMTU);

    return true;
}

GATTHandler::~GATTHandler() {
    disconnect();
}

bool GATTHandler::disconnect() {
    if( Disconnected >= validateState() ) {
        // not open
        return false;
    }
    DBG_PRINT("GATTHandler.disconnect Start");
    if( l2capReaderRunning && l2capReaderThread.joinable() ) {
        l2capReaderShallStop = true;
        pthread_t tid = l2capReaderThread.native_handle();
        pthread_kill(tid, SIGINT);
    }

    l2cap->disconnect();
    state = Disconnected;

    if( l2capReaderRunning && l2capReaderThread.joinable() ) {
        // still running ..
        DBG_PRINT("GATTHandler.disconnect join l2capReaderThread");
        l2capReaderThread.join();
    }
    l2capReaderThread = std::thread(); // empty
    DBG_PRINT("GATTHandler.disconnect End");
    return Disconnected == validateState();
}

bool GATTHandler::send(const AttPDUMsg &msg) {
    if( Disconnected >= validateState() ) {
        // not open
        return false;
    }
    if( msg.pdu.getSize() > usedMTU ) {
        throw IllegalArgumentException("clientMaxMTU "+std::to_string(msg.pdu.getSize())+" > usedMTU "+std::to_string(usedMTU), E_FILE_LINE);
    }

    const int res = l2cap->write(msg.pdu.get_ptr(), msg.pdu.getSize());
    if( 0 > res ) {
        perror("GATTHandler::send: l2cap write error");
        state = Error;
        return false;
    }
    return res == msg.pdu.getSize();
}

std::shared_ptr<const AttPDUMsg> GATTHandler::receiveNext() {
    return attPDURing.getBlocking();
}

uint16_t GATTHandler::exchangeMTU(const uint16_t clientMaxMTU) {
    /***
     * BT Core Spec v5.2: Vol 3, Part G GATT: 4.3.1 Exchange MTU (Server configuration)
     */
    if( clientMaxMTU > ClientMaxMTU ) {
        throw IllegalArgumentException("clientMaxMTU "+std::to_string(clientMaxMTU)+" > ClientMaxMTU "+std::to_string(ClientMaxMTU), E_FILE_LINE);
    }
    const AttExchangeMTU req(clientMaxMTU);

    PERF_TS_T0();

    uint16_t mtu = 0;
    DBG_PRINT("GATT send: %s", req.toString().c_str());

    if( send(req) ) {
        const std::shared_ptr<const AttPDUMsg> pdu = receiveNext();
        DBG_PRINT("GATT recv: %s", pdu->toString().c_str());
        if( pdu->getOpcode() == AttPDUMsg::ATT_EXCHANGE_MTU_RSP ) {
            const AttExchangeMTU * p = static_cast<const AttExchangeMTU*>(pdu.get());
            mtu = p->getMTUSize();
        }
    }
    PERF_TS_TD("GATT exchangeMTU");

    return mtu;
}

GATTCharacterisicsDeclRef GATTHandler::findCharacterisics(const uint16_t charHandle) {
    return findCharacterisics(charHandle, services);
}

GATTCharacterisicsDeclRef GATTHandler::findCharacterisics(const uint16_t charHandle, std::vector<GATTPrimaryServiceRef> &services) {
    for(auto it = services.begin(); it != services.end(); it++) {
        GATTCharacterisicsDeclRef decl = findCharacterisics(charHandle, *it);
        if( nullptr != decl ) {
            return decl;
        }
    }
    return nullptr;
}

GATTCharacterisicsDeclRef GATTHandler::findCharacterisics(const uint16_t charHandle, GATTPrimaryServiceRef service) {
    for(auto it = service->characteristicDeclList.begin(); it != service->characteristicDeclList.end(); it++) {
        GATTCharacterisicsDeclRef decl = *it;
        if( charHandle == decl->handle ) {
            return decl;
        }
    }
    return nullptr;
}

std::vector<GATTPrimaryServiceRef> & GATTHandler::discoverCompletePrimaryServices() {
    if( !discoverPrimaryServices(services) ) {
        return services;
    }
    for(auto it = services.begin(); it != services.end(); it++) {
        GATTPrimaryServiceRef primSrv = *it;
        if( discoverCharacteristics(primSrv) ) {
            discoverClientCharacteristicConfig(primSrv);
        }
    }
    return services;
}

bool GATTHandler::discoverPrimaryServices(std::vector<GATTPrimaryServiceRef> & result) {
    /***
     * BT Core Spec v5.2: Vol 3, Part G GATT: 4.4.1 Discover All Primary Services
     *
     * This sub-procedure is complete when the ATT_ERROR_RSP PDU is received
     * and the error code is set to Attribute Not Found or when the End Group Handle
     * in the Read by Type Group Response is 0xFFFF.
     */
    const uuid16_t groupType = uuid16_t(GattAttributeType::PRIMARY_SERVICE);

    PERF_TS_T0();

    bool done=false;
    uint16_t startHandle=0x0001;
    result.clear();
    while(!done) {
        const AttReadByNTypeReq req(true /* group */, startHandle, 0xffff, groupType);
        DBG_PRINT("GATT PRIM SRV discover send: %s", req.toString().c_str());

        if( send(req) ) {
            const std::shared_ptr<const AttPDUMsg> pdu = receiveNext();
            DBG_PRINT("GATT PRIM SRV discover recv: %s", pdu->toString().c_str());
            if( pdu->getOpcode() == AttPDUMsg::ATT_READ_BY_GROUP_TYPE_RSP ) {
                const AttReadByGroupTypeRsp * p = static_cast<const AttReadByGroupTypeRsp*>(pdu.get());
                const int count = p->getElementCount();

                for(int i=0; i<count; i++) {
                    const int ePDUOffset = p->getElementPDUOffset(i);
                    const int esz = p->getElementTotalSize();
                    result.push_back( GATTPrimaryServiceRef( new GATTPrimaryService( GATTUUIDHandleRange(
                    	GATTUUIDHandleRange::Type::Service,
                        p->pdu.get_uint16(ePDUOffset), // start-handle
                        p->pdu.get_uint16(ePDUOffset + 2), // end-handle
                        p->pdu.get_uuid( ePDUOffset + 2 + 2, uuid_t::toTypeSize(esz-2-2) ) ) ) ) ); // uuid
                    DBG_PRINT("GATT PRIM SRV discovered[%d/%d]: %s", i, count, result.at(result.size()-1)->toString().c_str());
                }
                startHandle = p->getElementEndHandle(count-1);
                if( startHandle < 0xffff ) {
                    startHandle++;
                } else {
                    done = true; // OK by spec: End of communication
                }
            } else if( pdu->getOpcode() == AttPDUMsg::ATT_ERROR_RSP ) {
                done = true; // OK by spec: End of communication
            } else {
                WARN_PRINT("GATT discoverPrimary unexpected reply %s", pdu->toString().c_str());
                done = true;
            }
        } else {
            ERR_PRINT("GATT discoverPrimary send failed");
            done = true; // send failed
        }
    }
    PERF_TS_TD("GATT discoverPrimaryServices");

    return result.size() > 0;
}

bool GATTHandler::discoverCharacteristics(GATTPrimaryServiceRef service) {
    /***
     * BT Core Spec v5.2: Vol 3, Part G GATT: 4.6.1 Discover All Characteristics of a Service
     * <p>
     * BT Core Spec v5.2: Vol 3, Part G GATT: 3.3.1 Characteristic Declaration Attribute Value
     * </p>
     * <p>
     * BT Core Spec v5.2: Vol 3, Part G GATT: 3.3.3.3 Client Characteristic Configuration
     * </p>
     */
    const uuid16_t characteristicTypeReq = uuid16_t(GattAttributeType::CHARACTERISTIC);
    const uuid16_t clientCharConfigTypeReq = uuid16_t(GattAttributeType::CLIENT_CHARACTERISTIC_CONFIGURATION);

    PERF_TS_T0();

    bool done=false;
    uint16_t handle=service->declaration.startHandle;
    service->characteristicDeclList.clear();
    while(!done) {
        const AttReadByNTypeReq req(false /* group */, handle, service->declaration.endHandle, characteristicTypeReq);
        DBG_PRINT("GATT CCD discover send: %s", req.toString().c_str());

        if( send(req) ) {
            std::shared_ptr<const AttPDUMsg> pdu = receiveNext();
            DBG_PRINT("GATT CCD discover recv: %s", pdu->toString().c_str());
            if( pdu->getOpcode() == AttPDUMsg::ATT_READ_BY_TYPE_RSP ) {
                const AttReadByTypeRsp * p = static_cast<const AttReadByTypeRsp*>(pdu.get());
                const int count = p->getElementCount();

                for(int i=0; i<count; i++) {
                    // handle: handle for the Characteristics declaration
                    // value: Characteristics Property, Characteristics Value Handle _and_ Characteristics UUID
                    const int ePDUOffset = p->getElementPDUOffset(i);
                    const int esz = p->getElementTotalSize();
                    service->characteristicDeclList.push_back( GATTCharacterisicsDeclRef( new GATTCharacterisicsDecl(
                        service->declaration.uuid,
                        p->pdu.get_uint16(ePDUOffset), // service-handle
                        service->declaration.endHandle,
                        static_cast<GATTCharacterisicsDecl::PropertyBitVal>(p->pdu.get_uint8(ePDUOffset  + 2)), // properties
                        p->pdu.get_uint16(ePDUOffset + 2 + 1), // handle
                        p->pdu.get_uuid(ePDUOffset   + 2 + 1 + 2, uuid_t::toTypeSize(esz-2-1-2) ) ) ) ); // uuid
                    DBG_PRINT("GATT CCD discovered[%d/%d]: %s", i, count, service->characteristicDeclList.at(service->characteristicDeclList.size()-1)->toString().c_str());
                }
                handle = p->getElementHandle(count-1);
                if( handle < service->declaration.endHandle ) {
                    handle++;
                } else {
                    done = true; // OK by spec: End of communication
                }
            } else if( pdu->getOpcode() == AttPDUMsg::ATT_ERROR_RSP ) {
                done = true; // OK by spec: End of communication
            } else {
                WARN_PRINT("GATT discoverCharacteristics unexpected reply %s", pdu->toString().c_str());
                done = true;
            }
        } else {
            ERR_PRINT("GATT discoverCharacteristics send failed");
            done = true;
        }
    }

    PERF_TS_TD("GATT discoverCharacteristics");

    return service->characteristicDeclList.size() > 0;
}

bool GATTHandler::discoverClientCharacteristicConfig(GATTPrimaryServiceRef service) {
    /***
     * BT Core Spec v5.2: Vol 3, Part G GATT: 4.6.1 Discover All Characteristics of a Service
     * <p>
     * BT Core Spec v5.2: Vol 3, Part G GATT: 3.3.1 Characteristic Declaration Attribute Value
     * </p>
     * <p>
     * BT Core Spec v5.2: Vol 3, Part G GATT: 3.3.3.3 Client Characteristic Configuration
     * </p>
     */
    const uuid16_t clientCharConfigTypeReq = uuid16_t(GattAttributeType::CLIENT_CHARACTERISTIC_CONFIGURATION);

    PERF_TS_T0();

    bool done=false;
    uint16_t handle=service->declaration.startHandle;
    // list.clear();
    while(!done) {
        const AttReadByNTypeReq req(false /* group */, handle, service->declaration.endHandle, clientCharConfigTypeReq);
        DBG_PRINT("GATT CCC discover send: %s", req.toString().c_str());

        if( send(req) ) {
            std::shared_ptr<const AttPDUMsg> pdu = receiveNext();
            DBG_PRINT("GATT CCC discover recv: %s", pdu->toString().c_str());
            if( pdu->getOpcode() == AttPDUMsg::ATT_READ_BY_TYPE_RSP ) {
                const AttReadByTypeRsp * p = static_cast<const AttReadByTypeRsp*>(pdu.get());
                const int count = p->getElementCount();

                for(int i=0; i<count; i++) {
                    // handle: handle for the Characteristics declaration
                    // value: Characteristics Property, Characteristics Value Handle _and_ Characteristics UUID
                    const int ePDUOffset = p->getElementPDUOffset(i);
                    const int esz = p->getElementTotalSize();
                    if( 4 == esz ) {
                        const uint16_t config_handle = p->pdu.get_uint16(ePDUOffset);
                        const uint16_t config_value = p->pdu.get_uint16(ePDUOffset+2);
                        for(size_t j=0; j<service->characteristicDeclList.size(); j++) {
                            GATTCharacterisicsDecl & decl = *service->characteristicDeclList.at(j);
                            uint16_t decl_handle_end;
                            if( j+1 < service->characteristicDeclList.size() ) {
                                decl_handle_end = service->characteristicDeclList.at(j+1)->handle;
                            } else {
                                decl_handle_end = decl.service_handle_end;
                            }
                            if( config_handle > decl.handle && config_handle <= decl_handle_end ) {
                                decl.config = std::shared_ptr<GATTClientCharacteristicConfigDecl>(
                                        new GATTClientCharacteristicConfigDecl(config_handle, config_value));
                                DBG_PRINT("GATT CCC discovered[%d/%d]: %s", i, count, decl.toString().c_str());
                            }
                        }
                    } else {
                        WARN_PRINT("GATT discoverCharacteristicsClientConfig unexpected PDU Element size reply %s", pdu->toString().c_str());
                    }
                }
                handle = p->getElementHandle(count-1);
                if( handle < service->declaration.endHandle ) {
                    handle++;
                } else {
                    done = true; // OK by spec: End of communication
                }
            } else if( pdu->getOpcode() == AttPDUMsg::ATT_ERROR_RSP ) {
                done = true; // OK by spec: End of communication
            } else {
                WARN_PRINT("GATT discoverCharacteristicsClientConfig unexpected opcode reply %s", pdu->toString().c_str());
                done = true;
            }
        } else {
            ERR_PRINT("GATT discoverCharacteristicsClientConfig send failed");
            done = true;
        }
    }

    PERF_TS_TD("GATT discoverCharacteristicsClientConfig");

    return service->characteristicDeclList.size() > 0;
}

bool GATTHandler::discoverCharacteristicDescriptors(const GATTUUIDHandleRange & service, std::vector<GATTUUIDHandle> & result) {
    /* BT Core Spec v5.2: Vol 3, Part G GATT: 4.7.1 Discover All Characteristic Descriptors */

    PERF_TS_T0();

    bool done=false;
    uint16_t handle=service.startHandle+1;
    result.clear();
    while(!done) {
        const AttFindInfoReq req(handle, service.endHandle);
        DBG_PRINT("GATT CCD discover2 send: %s", req.toString().c_str());

        if( send(req) ) {
            const std::shared_ptr<const AttPDUMsg> pdu = receiveNext();
            DBG_PRINT("GATT CCD discover2 recv: %s", pdu->toString().c_str());
            if( pdu->getOpcode() == AttPDUMsg::ATT_FIND_INFORMATION_RSP ) {
                const AttFindInfoRsp * p = static_cast<const AttFindInfoRsp*>(pdu.get());
                const int count = p->getElementCount();

                for(int i=0; i<count; i++) {
                    // handle: handle of Characteristic Descriptor Declaration.
                    // value: Characteristic Descriptor UUID.
                    result.push_back( GATTUUIDHandle( p->getElementHandle(i), p->getElementValue(i) ) );
                    DBG_PRINT("GATT CCD discovered2[%d/%d]: %s", i, count, result.at(result.size()-1).toString().c_str());
                }
                handle = p->getElementHandle(count-1);
                if( handle < service.endHandle ) {
                    handle++;
                } else {
                    done = true; // OK by spec: End of communication
                }
            } else if( pdu->getOpcode() == AttPDUMsg::ATT_ERROR_RSP ) {
                done = true; // OK by spec: End of communication
            } else {
                WARN_PRINT("GATT discoverDescriptors unexpected reply %s", pdu->toString().c_str());
                done = true;
            }
        } else {
            ERR_PRINT("GATT discoverDescriptors send failed");
            done = true;
        }
    }
    PERF_TS_TD("GATT discoverDescriptors");

    return result.size() > 0;
}

bool GATTHandler::readCharacteristicValue(const GATTCharacterisicsDecl & decl, POctets & res, int expectedLength) {
    /* BT Core Spec v5.2: Vol 3, Part G GATT: 4.8.1 Read Characteristic Value */
    /* BT Core Spec v5.2: Vol 3, Part G GATT: 4.8.3 Read Long Characteristic Value */
    PERF_TS_T0();

    bool done=false;
    int offset=0;

    DBG_PRINT("GATTHandler::readCharacteristicValue expLen %d, decl %s", expectedLength, decl.toString().c_str());

    while(!done) {
        if( 0 < expectedLength && expectedLength <= offset ) {
            break; // done
        } else if( 0 == expectedLength && 0 < offset ) {
            break; // done w/ only one request
        } // else 0 > expectedLength: implicit

        bool sendRes;

        if( 0 == offset ) {
            const AttReadReq req (decl.handle);
            DBG_PRINT("GATT CV send: %s", req.toString().c_str());
            sendRes = send(req);
        } else {
            const AttReadBlobReq req (decl.handle, offset);
            DBG_PRINT("GATT CV send: %s", req.toString().c_str());
            sendRes = send(req);
        }

        if( sendRes ) {
            const std::shared_ptr<const AttPDUMsg> pdu = receiveNext();
            DBG_PRINT("GATT CV recv: %s", pdu->toString().c_str());
            if( pdu->getOpcode() == AttPDUMsg::ATT_READ_RSP ) {
                const AttReadRsp * p = static_cast<const AttReadRsp*>(pdu.get());
                const TOctetSlice & v = p->getValue();
                res += v;
                offset += v.getSize();
                if( p->getPDUValueSize() < p->getMaxPDUValueSize(usedMTU) ) {
                    done = true; // No full ATT_MTU PDU used - end of communication
                }
            } else if( pdu->getOpcode() == AttPDUMsg::ATT_READ_BLOB_RSP ) {
                const AttReadBlobRsp * p = static_cast<const AttReadBlobRsp*>(pdu.get());
                const TOctetSlice & v = p->getValue();
                if( 0 == v.getSize() ) {
                    done = true; // OK by spec: No more data - end of communication
                } else {
                    res += v;
                    offset += v.getSize();
                    if( p->getPDUValueSize() < p->getMaxPDUValueSize(usedMTU) ) {
                        done = true; // No full ATT_MTU PDU used - end of communication
                    }
                }
            } else if( pdu->getOpcode() == AttPDUMsg::ATT_ERROR_RSP ) {
                /**
                 * BT Core Spec v5.2: Vol 3, Part G GATT: 4.8.3 Read Long Characteristic Value
                 *
                 * If the Characteristic Value is not longer than (ATT_MTU – 1)
                 * an ATT_ERROR_RSP PDU with the error
                 * code set to Attribute Not Long shall be received on the first
                 * ATT_READ_BLOB_REQ PDU.
                 */
                const AttErrorRsp * p = static_cast<const AttErrorRsp *>(pdu.get());
                if( AttErrorRsp::ATTRIBUTE_NOT_LONG == p->getErrorCode() ) {
                    done = true; // OK by spec: No more data - end of communication
                } else {
                    WARN_PRINT("GATT readCharacteristicValue unexpected error %s", pdu->toString().c_str());
                    done = true;
                }
            } else {
                WARN_PRINT("GATT readCharacteristicValue unexpected reply %s", pdu->toString().c_str());
                done = true;
            }
        } else {
            ERR_PRINT("GATT readCharacteristicValue send failed");
            done = true;
        }
    }
    PERF_TS_TD("GATT readCharacteristicValue");

    return offset > 0;
}

bool GATTHandler::writeClientCharacteristicConfigReq(const GATTClientCharacteristicConfigDecl & cccd, const TROOctets & value) {
    /* BT Core Spec v5.2: Vol 3, Part G GATT: 3.3.3.3 Client Characteristic Configuration */
    /* BT Core Spec v5.2: Vol 3, Part G GATT: 4.9.3 Write Characteristic Value */
    /* BT Core Spec v5.2: Vol 3, Part G GATT: 4.11 Characteristic Value Indication */

    AttWriteReq req(cccd.handle, value);
    DBG_PRINT("GATT send: %s", req.toString().c_str());
    bool res = false;
    bool sendRes = send(req);
    if( sendRes ) {
        const std::shared_ptr<const AttPDUMsg> pdu = receiveNext();
        DBG_PRINT("GATT recv: %s", pdu->toString().c_str());
        if( pdu->getOpcode() == AttPDUMsg::ATT_WRITE_RSP ) {
            // OK
            res = true;
        } else if( pdu->getOpcode() == AttPDUMsg::ATT_ERROR_RSP ) {
            const AttErrorRsp * p = static_cast<const AttErrorRsp *>(pdu.get());
            WARN_PRINT("GATT writeCharacteristicValueReq unexpected error %s", p->toString().c_str());
        } else {
            WARN_PRINT("GATT writeCharacteristicValueReq unexpected reply %s", pdu->toString().c_str());
        }
    }
    return res;
}

bool GATTHandler::writeCharacteristicValueReq(const GATTCharacterisicsDecl & decl, const TROOctets & value) {
    /* BT Core Spec v5.2: Vol 3, Part G GATT: 4.9.3 Write Characteristic Value */

    DBG_PRINT("GATTHandler::writeCharacteristicValueReq decl %s, value %s",
            decl.toString().c_str(), value.toString().c_str());

    AttWriteReq req(decl.handle, value);
    DBG_PRINT("GATT send: %s", req.toString().c_str());
    bool res = false;
    bool sendRes = send(req);
    if( sendRes ) {
        const std::shared_ptr<const AttPDUMsg> pdu = receiveNext();
        DBG_PRINT("GATT recv: %s", pdu->toString().c_str());
        if( pdu->getOpcode() == AttPDUMsg::ATT_WRITE_RSP ) {
            // OK
            res = true;
        } else if( pdu->getOpcode() == AttPDUMsg::ATT_ERROR_RSP ) {
            const AttErrorRsp * p = static_cast<const AttErrorRsp *>(pdu.get());
            WARN_PRINT("GATT writeCharacteristicValueReq unexpected error %s", p->toString().c_str());
        } else {
            WARN_PRINT("GATT writeCharacteristicValueReq unexpected reply %s", pdu->toString().c_str());
        }
    }
    return res;
}


bool GATTHandler::configIndicationNotification(const GATTClientCharacteristicConfigDecl & cccd, const bool enableNotification, const bool enableIndication) {
    /* BT Core Spec v5.2: Vol 3, Part G GATT: 3.3.3.3 Client Characteristic Configuration */
    const uint16_t ccc_value = enableNotification | ( enableIndication << 1 );
    DBG_PRINT("GATTHandler::configIndicationNotification decl %s, enableNotification %d, enableIndication %d",
            cccd.toString().c_str(), enableNotification, enableIndication);
    POctets ccc(2);
    ccc.put_uint16(0, ccc_value);
    return writeClientCharacteristicConfigReq(cccd, ccc);
}

/*********************************************************************************************************************/
/*********************************************************************************************************************/
/*********************************************************************************************************************/

static const uuid16_t _GENERIC_ACCESS(GattServiceType::GENERIC_ACCESS);
static const uuid16_t _DEVICE_NAME(GattCharacteristicType::DEVICE_NAME);
static const uuid16_t _APPEARANCE(GattCharacteristicType::APPEARANCE);
static const uuid16_t _PERIPHERAL_PREFERRED_CONNECTION_PARAMETERS(GattCharacteristicType::PERIPHERAL_PREFERRED_CONNECTION_PARAMETERS);

static const uuid16_t _DEVICE_INFORMATION(GattServiceType::DEVICE_INFORMATION);
static const uuid16_t _SYSTEM_ID(GattCharacteristicType::SYSTEM_ID);
static const uuid16_t _MODEL_NUMBER_STRING(GattCharacteristicType::MODEL_NUMBER_STRING);
static const uuid16_t _SERIAL_NUMBER_STRING(GattCharacteristicType::SERIAL_NUMBER_STRING);
static const uuid16_t _FIRMWARE_REVISION_STRING(GattCharacteristicType::FIRMWARE_REVISION_STRING);
static const uuid16_t _HARDWARE_REVISION_STRING(GattCharacteristicType::HARDWARE_REVISION_STRING);
static const uuid16_t _SOFTWARE_REVISION_STRING(GattCharacteristicType::SOFTWARE_REVISION_STRING);
static const uuid16_t _MANUFACTURER_NAME_STRING(GattCharacteristicType::MANUFACTURER_NAME_STRING);
static const uuid16_t _REGULATORY_CERT_DATA_LIST(GattCharacteristicType::REGULATORY_CERT_DATA_LIST);
static const uuid16_t _PNP_ID(GattCharacteristicType::PNP_ID);

std::shared_ptr<GenericAccess> GATTHandler::getGenericAccess(std::vector<GATTCharacterisicsDeclRef> & genericAccessCharDeclList) {
    std::shared_ptr<GenericAccess> res = nullptr;
    POctets value(GATTHandler::ClientMaxMTU, 0);
    std::string deviceName = "";
    GenericAccess::AppearanceCat category = GenericAccess::AppearanceCat::UNKNOWN;
    PeriphalPreferredConnectionParameters * prefConnParam = nullptr;

    for(size_t i=0; i<genericAccessCharDeclList.size(); i++) {
        const GATTCharacterisicsDecl & charDecl = *genericAccessCharDeclList.at(i);
        if( _GENERIC_ACCESS != *charDecl.service_uuid ) {
        	continue;
        }
        if( _DEVICE_NAME == *charDecl.uuid ) {
            if( readCharacteristicValue(charDecl, value.resize(0)) ) {
            	deviceName = GattNameToString(value);
            }
        } else if( _APPEARANCE == *charDecl.uuid ) {
            if( readCharacteristicValue(charDecl, value.resize(0)) ) {
            	category = static_cast<GenericAccess::AppearanceCat>(value.get_uint16(0));
            }
        } else if( _PERIPHERAL_PREFERRED_CONNECTION_PARAMETERS == *charDecl.uuid ) {
            if( readCharacteristicValue(charDecl, value.resize(0)) ) {
            	prefConnParam = new PeriphalPreferredConnectionParameters(value);
            }
        }
    }
    if( deviceName.size() > 0 && nullptr != prefConnParam ) {
    	res = std::shared_ptr<GenericAccess>(new GenericAccess(deviceName, category, *prefConnParam));
    }
    if( nullptr != prefConnParam ) {
        delete prefConnParam;
    }
    return res;
}

std::shared_ptr<GenericAccess> GATTHandler::getGenericAccess(std::vector<GATTPrimaryServiceRef> & primServices) {
	std::shared_ptr<GenericAccess> res = nullptr;
	for(size_t i=0; i<primServices.size() && nullptr == res; i++) {
		res = getGenericAccess(primServices.at(i)->characteristicDeclList);
	}
	return res;
}

std::shared_ptr<DeviceInformation> GATTHandler::getDeviceInformation(std::vector<GATTCharacterisicsDeclRef> & characteristicDeclList) {
    std::shared_ptr<DeviceInformation> res = nullptr;
    POctets value(GATTHandler::ClientMaxMTU, 0);

    POctets systemID(8, 0);
    std::string modelNumber;
    std::string serialNumber;
    std::string firmwareRevision;
    std::string hardwareRevision;
    std::string softwareRevision;
    std::string manufacturer;
    POctets regulatoryCertDataList(128, 0);
    PnP_ID * pnpID = nullptr;
    bool found = false;

    for(size_t i=0; i<characteristicDeclList.size(); i++) {
        const GATTCharacterisicsDecl & charDecl = *characteristicDeclList.at(i);
        if( _DEVICE_INFORMATION != *charDecl.service_uuid ) {
            continue;
        }
        found = true;
        if( _SYSTEM_ID == *charDecl.uuid ) {
            if( readCharacteristicValue(charDecl, systemID.resize(0)) ) {
                // nop
            }
        } else if( _REGULATORY_CERT_DATA_LIST == *charDecl.uuid ) {
            if( readCharacteristicValue(charDecl, regulatoryCertDataList.resize(0)) ) {
                // nop
            }
        } else if( _PNP_ID == *charDecl.uuid ) {
            if( readCharacteristicValue(charDecl, value.resize(0)) ) {
                pnpID = new PnP_ID(value);
            }
        } else if( _MODEL_NUMBER_STRING == *charDecl.uuid ) {
            if( readCharacteristicValue(charDecl, value.resize(0)) ) {
                modelNumber = GattNameToString(value);
            }
        } else if( _SERIAL_NUMBER_STRING == *charDecl.uuid ) {
            if( readCharacteristicValue(charDecl, value.resize(0)) ) {
                serialNumber = GattNameToString(value);
            }
        } else if( _FIRMWARE_REVISION_STRING == *charDecl.uuid ) {
            if( readCharacteristicValue(charDecl, value.resize(0)) ) {
                firmwareRevision = GattNameToString(value);
            }
        } else if( _HARDWARE_REVISION_STRING == *charDecl.uuid ) {
            if( readCharacteristicValue(charDecl, value.resize(0)) ) {
                hardwareRevision = GattNameToString(value);
            }
        } else if( _SOFTWARE_REVISION_STRING == *charDecl.uuid ) {
            if( readCharacteristicValue(charDecl, value.resize(0)) ) {
                softwareRevision = GattNameToString(value);
            }
        } else if( _MANUFACTURER_NAME_STRING == *charDecl.uuid ) {
            if( readCharacteristicValue(charDecl, value.resize(0)) ) {
                manufacturer = GattNameToString(value);
            }
        }
    }
    if( nullptr == pnpID ) {
        pnpID = new PnP_ID();
    }
    if( found ) {
        res = std::shared_ptr<DeviceInformation>(new DeviceInformation(systemID, modelNumber, serialNumber,
                                                      firmwareRevision, hardwareRevision, softwareRevision,
                                                      manufacturer, regulatoryCertDataList, *pnpID) );
    }
    delete pnpID;
    return res;
}

std::shared_ptr<DeviceInformation> GATTHandler::getDeviceInformation(std::vector<GATTPrimaryServiceRef> & primServices) {
    std::shared_ptr<DeviceInformation> res = nullptr;
    for(size_t i=0; i<primServices.size() && nullptr == res; i++) {
        res = getDeviceInformation(primServices.at(i)->characteristicDeclList);
    }
    return res;
}

