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

#include <dbt_debug.hpp>
#include <cstring>
#include <string>
#include <memory>
#include <cstdint>
#include <vector>
#include <cstdio>

#include  <algorithm>

#include "BTTypes.hpp"

using namespace direct_bt;

#define CHAR_DECL_BDADDRESSTYPE_ENUM(X) \
        X(BDADDR_BREDR) \
        X(BDADDR_LE_PUBLIC) \
        X(BDADDR_LE_RANDOM) \
        X(BDADDR_UNDEFINED)

#define CASE_TO_STRING(V) case V: return #V;
#define CASE2_TO_STRING(U,V) case U::V: return #V;

std::string direct_bt::getBDAddressTypeString(const BDAddressType type) {
    switch(type) {
    CHAR_DECL_BDADDRESSTYPE_ENUM(CASE_TO_STRING)
        default: ; // fall through intended
    }
    return "Unknown address type";
}

std::string EUI48::toString() const {
    const int length = 17;
    std::string str;
    str.reserve(length+1); // including EOS for snprintf
    str.resize(length);

    const int count = snprintf(&str[0], str.capacity(), "%2.2X:%2.2X:%2.2X:%2.2X:%2.2X:%2.2X",
                                b[5], b[4], b[3], b[2], b[1], b[0]);
    if( length != count ) {
        throw direct_bt::InternalError("EUI48 string not of length "+std::to_string(length)+" but "+std::to_string(count), E_FILE_LINE);
    }
    return str;
}

EUI48::EUI48(const std::string str) {
    if( 17 != str.length() ) {
        std::string msg("EUI48 string not of length 17 but ");
        msg.append(std::to_string(str.length()));
        msg.append(": "+str);
        throw direct_bt::IllegalArgumentException(msg, E_FILE_LINE);
    }
    if ( sscanf(str.c_str(), "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
                     &b[5], &b[4], &b[3], &b[2], &b[1], &b[0]) != 6 )
    {
        std::string msg("EUI48 string not in format '00:00:00:00:00:00' but "+str);
        throw direct_bt::IllegalArgumentException(msg, E_FILE_LINE);
    }

    // sscanf provided host data type, in which we store the values,
    // hence no endian conversion
}

EUI48::EUI48(const uint8_t * _b) {
    memcpy(b, _b, sizeof(b));
}

const EUI48 EUI48_ANY_DEVICE; // default ctor is zero bytes!
static uint8_t _EUI48_ALL_DEVICE[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static uint8_t _EUI48_LOCAL_DEVICE[] = {0x00, 0x00, 0x00, 0xff, 0xff, 0xff};
const EUI48 EUI48_ALL_DEVICE( _EUI48_ALL_DEVICE );
const EUI48 EUI48_LOCAL_DEVICE( _EUI48_LOCAL_DEVICE );

// *************************************************
// *************************************************
// *************************************************

using namespace direct_bt;

static inline const int8_t * const_uint8_to_const_int8_ptr(const uint8_t* p) {
    return static_cast<const int8_t *>( static_cast<void *>( const_cast<uint8_t*>( p ) ) );
}

static std::string bt_compidtostr(const uint16_t companyid) {
    return std::to_string(companyid);
}

ManufactureSpecificData::ManufactureSpecificData(uint16_t const company, uint8_t const * const data, int const data_len)
: company(company), companyName(std::string(bt_compidtostr(company))), data(data, data_len) {
}

std::string ManufactureSpecificData::toString() const {
  std::string out("MSD[company[");
  out.append(std::to_string(company)+" "+companyName);
  out.append("], data["+data.toString()+"]]");
  return out;
}

// *************************************************
// *************************************************
// *************************************************

#define EIRDATATYPE_ENUM(X) \
    X(EIRDataType,NONE) \
    X(EIRDataType,EVT_TYPE) \
    X(EIRDataType,BDADDR_TYPE) \
    X(EIRDataType,BDADDR) \
    X(EIRDataType,FLAGS) \
    X(EIRDataType,NAME) \
    X(EIRDataType,NAME_SHORT) \
    X(EIRDataType,RSSI) \
    X(EIRDataType,TX_POWER) \
    X(EIRDataType,MANUF_DATA) \
    X(EIRDataType,DEVICE_CLASS) \
    X(EIRDataType,APPEARANCE) \
    X(EIRDataType,HASH) \
    X(EIRDataType,RANDOMIZER) \
    X(EIRDataType,DEVICE_ID) \
    X(EIRDataType,SERVICE_UUID)

std::string direct_bt::eirDataBitToString(const EIRDataType bit) {
    switch(bit) {
    EIRDATATYPE_ENUM(CASE2_TO_STRING)
        default: ; // fall through intended
    }
    return "Unknown EIRDataType Bit";
}

std::string direct_bt::eirDataMaskToString(const EIRDataType mask) {
    const uint32_t one = 1;
    bool has_pre = false;
    std::string out("[");
    for(int i=0; i<32; i++) {
        const EIRDataType settingBit = static_cast<EIRDataType>( one << i );
        if( EIRDataType::NONE != ( mask & settingBit ) ) {
            if( has_pre ) { out.append(", "); }
            out.append(eirDataBitToString(settingBit));
            has_pre = true;
        }
    }
    out.append("]");
    return out;
}

// *************************************************
// *************************************************
// *************************************************

std::string EInfoReport::getSourceString() const {
    switch (source) {
        case Source::NA: return "N/A";
        case Source::AD: return "AD";
        case Source::EIR: return "EIR";
    }
    return "N/A";
}

void EInfoReport::setName(const uint8_t *buffer, int buffer_len) {
    name = get_string(buffer, buffer_len, 30);
    set(EIRDataType::NAME);
}

void EInfoReport::setShortName(const uint8_t *buffer, int buffer_len) {
    name_short = get_string(buffer, buffer_len, 30);
    set(EIRDataType::NAME_SHORT);
}

void EInfoReport::addService(std::shared_ptr<uuid_t> const &uuid)
{
    auto begin = services.begin();
    auto it = std::find_if(begin, services.end(), [&](std::shared_ptr<uuid_t> const& p) {
        return *p == *uuid;
    });
    if ( it == std::end(services) ) {
        services.push_back(uuid);
    }
}

std::string EInfoReport::eirDataMaskToString() const {
    return std::string("DataSet"+ direct_bt::eirDataMaskToString(eir_data_mask) );
}
std::string EInfoReport::toString() const {
    std::string msdstr = nullptr != msd ? msd->toString() : "MSD[null]";
    std::string out("EInfoReport::"+getSourceString()+
                    "[address["+getAddressString()+", "+getBDAddressTypeString(getAddressType())+"], name['"+name+"'/'"+name_short+
                    "'], "+eirDataMaskToString()+
                    ", evt-type "+std::to_string(evt_type)+", rssi "+std::to_string(rssi)+
                    ", tx-power "+std::to_string(tx_power)+
                    ", dev-class "+uint32HexString(device_class, true)+
                    ", appearance "+uint16HexString(appearance, true)+
                    ", hash["+hash.toString()+
                    "], randomizer["+randomizer.toString()+
                    "], device-id[source "+uint16HexString(did_source, true)+
                    ", vendor "+uint16HexString(did_vendor, true)+
                    ", product "+uint16HexString(did_product, true)+
                    ", version "+uint16HexString(did_version, true)+
                    "], "+msdstr+"]");

    if(services.size() > 0 ) {
        out.append("\n");
        for(auto it = services.begin(); it != services.end(); it++) {
            std::shared_ptr<uuid_t> p = *it;
            out.append("  ").append(p->toUUID128String()).append(", ").append(std::to_string(static_cast<int>(p->getTypeSize()))).append(" bytes\n");
        }
    }
    return out;
}

std::string EInfoReport::getDeviceIDModalias() const {
    char *cstr = NULL;
    int length;

    switch (did_source) {
        case 0x0001:
            length = asprintf(&cstr, "bluetooth:v%04Xp%04Xd%04X", did_vendor, did_product, did_version);
            break;
        case 0x0002:
            length = asprintf(&cstr, "usb:v%04Xp%04Xd%04X", did_vendor, did_product, did_version);
            break;
        default:
            length = asprintf(&cstr, "source<0x%X>:v%04Xp%04Xd%04X", did_source, did_vendor, did_product, did_version);
            break;
    }
    if( 0 >= length ) {
        if( NULL != cstr ) {
            free(cstr);
        }
        return std::string();
    }
    std::string res(cstr);
    free(cstr);
    return res;
}

// *************************************************
// *************************************************
// *************************************************

int EInfoReport::next_data_elem(uint8_t *eir_elem_len, uint8_t *eir_elem_type, uint8_t const **eir_elem_data,
                               uint8_t const * data, int offset, int const size)
{
    if (offset < size) {
        uint8_t len = data[offset]; // covers: type + data, less len field itself

        if (len == 0) {
            return 0; // end of significant part
        }

        if (len + offset > size) {
            return -ENOENT;
        }

        *eir_elem_type = data[offset + 1];
        *eir_elem_data = data + offset + 2; // net data ptr
        *eir_elem_len = len - 1; // less type -> net data length

        return offset + 1 + len; // next ad_struct offset: + len + type + data
    }
    return -ENOENT;
}

int EInfoReport::read_data(uint8_t const * data, uint8_t const data_length) {
    int count = 0;
    int offset = 0;
    uint8_t elem_len, elem_type;
    uint8_t const *elem_data;

    while( 0 < ( offset = next_data_elem( &elem_len, &elem_type, &elem_data, data, offset, data_length ) ) )
    {
        DBG_PRINT("%s-Element[%d] @ [%d/%d]: type 0x%.2X with %d bytes net",
                getSourceString().c_str(), count, offset, data_length, elem_type, elem_len);
        count++;

        // Guaranteed: elem_len >= 0!
        switch ( elem_type ) {
            case GAP_T::FLAGS:
                if( 1 <= elem_len ) {
                    setFlags(*const_uint8_to_const_int8_ptr(elem_data));
                }
                break;
            case GAP_T::UUID16_INCOMPLETE:
            case GAP_T::UUID16_COMPLETE:
                for(int j=0; j<elem_len/2; j++) {
                    const std::shared_ptr<uuid_t> uuid(new uuid16_t(elem_data, j*2, true));
                    addService(std::move(uuid));
                }
                break;
            case GAP_T::UUID32_INCOMPLETE:
            case GAP_T::UUID32_COMPLETE:
                for(int j=0; j<elem_len/4; j++) {
                    const std::shared_ptr<uuid_t> uuid(new uuid32_t(elem_data, j*4, true));
                    addService(std::move(uuid));
                }
                break;
            case GAP_T::UUID128_INCOMPLETE:
            case GAP_T::UUID128_COMPLETE:
                for(int j=0; j<elem_len/16; j++) {
                    const std::shared_ptr<uuid_t> uuid(new uuid128_t(elem_data, j*16, true));
                    addService(std::move(uuid));
                }
                break;
            case GAP_T::NAME_LOCAL_SHORT:
                setShortName(elem_data, elem_len);
                break;
            case GAP_T::NAME_LOCAL_COMPLETE:
                setName(elem_data, elem_len);
                break;
            case GAP_T::TX_POWER_LEVEL:
                if( 1 <= elem_len ) {
                    setTxPower(*const_uint8_to_const_int8_ptr(elem_data));
                }
                break;

            case GAP_T::SSP_CLASS_OF_DEVICE:
                if( 3 <= elem_len ) {
                    setDeviceClass(  elem_data[0] |
                                   ( elem_data[1] << 8 ) |
                                   ( elem_data[2] << 16 ) );
                }
                break;
            case GAP_T::DEVICE_ID:
                if( 8 <= elem_len ) {
                    setDeviceID(
                        data[0] | ( data[1] << 8 ), // source
                        data[2] | ( data[3] << 8 ), // vendor
                        data[4] | ( data[5] << 8 ), // product
                        data[6] | ( data[7] << 8 )); // version
                }
                break;
            case GAP_T::SOLICIT_UUID16:
            case GAP_T::SOLICIT_UUID128:
            case GAP_T::SVC_DATA_UUID16:
            case GAP_T::PUB_TRGT_ADDR:
            case GAP_T::RND_TRGT_ADDR:
            case GAP_T::GAP_APPEARANCE:
                if( 2 <= elem_len ) {
                    setAppearance(get_uint16(elem_data, 0, true /* littleEndian */));
                }
                break;
            case SSP_HASH_C192:
                if( 16 <= elem_len ) {
                    setHash(elem_data);
                }
                break;
            case SSP_RANDOMIZER_R192:
                if( 16 <= elem_len ) {
                    setRandomizer(elem_data);
                }
                break;
            case GAP_T::SOLICIT_UUID32:
            case GAP_T::SVC_DATA_UUID32:
            case GAP_T::SVC_DATA_UUID128:
                break;
            case GAP_T::MANUFACTURE_SPECIFIC:
                if( 2 <= elem_len ) {
                    uint16_t company = get_uint16(elem_data, 0, true /* littleEndian */);
                    setManufactureSpecificData(company, elem_data+2, elem_len-2);
                }
                break;
            default:
                // FIXME: Use a data blob!!!!
                fprintf(stderr, "%s-Element @ [%d/%d]: Warning: Unhandled type 0x%.2X with %d bytes net\n",
                        getSourceString().c_str(), offset, data_length, elem_type, elem_len);
                break;
        }
    }
    return count;
}

std::vector<std::shared_ptr<EInfoReport>> EInfoReport::read_ad_reports(uint8_t const * data, uint8_t const data_length) {
    int const num_reports = (int) data[0];
    std::vector<std::shared_ptr<EInfoReport>> ad_reports;

    if( 0 >= num_reports || num_reports > 0x19 ) {
        DBG_PRINT("AD-Reports: Invalid reports count: %d", num_reports);
        return ad_reports;
    }
    uint8_t const *limes = data + data_length;
    uint8_t const *i_octets = data + 1;
    uint8_t ad_data_len[0x19];
    const int segment_count = 6;
    int read_segments = 0;
    int i;
    const uint64_t timestamp = getCurrentMilliseconds();

    for(i = 0; i < num_reports && i_octets < limes; i++) {
        ad_reports.push_back(std::shared_ptr<EInfoReport>(new EInfoReport()));
        ad_reports[i]->setSource(Source::AD);
        ad_reports[i]->setTimestamp(timestamp);
        ad_reports[i]->setEvtType(*i_octets++);
        read_segments++;
    }
    for(i = 0; i < num_reports && i_octets < limes; i++) {
        ad_reports[i]->setAddressType(static_cast<BDAddressType>(*i_octets++));
        read_segments++;
    }
    for(i = 0; i < num_reports && i_octets + 5 < limes; i++) {
        ad_reports[i]->setAddress( *((EUI48 const *)i_octets) );
        i_octets += 6;
        read_segments++;
    }
    for(i = 0; i < num_reports && i_octets < limes; i++) {
        ad_data_len[i] = *i_octets++;
        read_segments++;
    }
    for(i = 0; i < num_reports && i_octets + ad_data_len[i] < limes; i++) {
        ad_reports[i]->read_data(i_octets, ad_data_len[i]);
        i_octets += ad_data_len[i];
        read_segments++;
    }
    for(i = 0; i < num_reports && i_octets < limes; i++) {
        ad_reports[i]->setRSSI(*const_uint8_to_const_int8_ptr(i_octets));
        i_octets++;
        read_segments++;
    }
    const int bytes_left = limes - i_octets;

    if( segment_count != read_segments ) {
        fprintf(stderr, "AD-Reports: Warning: Incomplete %d reports within %d bytes: Segment read %d < %d, data-ptr %d bytes to limes\n",
                num_reports, data_length, read_segments, segment_count, bytes_left);
    } else {
        DBG_PRINT("AD-Reports: Completed %d reports within %d bytes: Segment read %d == %d, data-ptr %d bytes to limes",
                num_reports, data_length, read_segments, segment_count, bytes_left);
    }
    return ad_reports;
}

// *************************************************
// *************************************************
// *************************************************

