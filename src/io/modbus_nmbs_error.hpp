#pragma once

// Internal helper shared by modbus_client.cpp (TCP) and modbus_rtu_bus.cpp (RTU) --
// not part of the public include/ API, since both translate the exact same
// nanoMODBUS error space into softplc::io::RequestResult and this logic would
// otherwise drift between the two transports.

#include "nanomodbus.h"
#include "softplc/io/modbus_client.hpp"

namespace softplc::io {

inline RequestResult translateModbusError(nmbs_error rc) {
    RequestResult result;
    if (rc == NMBS_ERROR_NONE) {
        return result;  // outcome defaults to Success
    }
    if (nmbs_error_is_exception(rc)) {
        result.outcome = RequestOutcome::ModbusException;
        result.modbusExceptionCode = static_cast<std::uint8_t>(rc);
        result.detail = nmbs_strerror(rc);
        return result;
    }

    result.detail = nmbs_strerror(rc);
    switch (rc) {
        case NMBS_ERROR_TIMEOUT:
            result.outcome = RequestOutcome::Timeout;
            break;
        case NMBS_ERROR_TRANSPORT:
        case NMBS_ERROR_INVALID_TCP_MBAP:
        case NMBS_ERROR_INVALID_UNIT_ID:
        case NMBS_ERROR_CRC:
            result.outcome = RequestOutcome::ConnectionLost;
            break;
        default:
            result.outcome = RequestOutcome::ProtocolError;
            break;
    }
    return result;
}

}  // namespace softplc::io
