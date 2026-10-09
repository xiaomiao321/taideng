#include "bsp_common.h"

const char *bsp_status_str(bsp_status_t status)
{
    switch (status)
    {
        case BSP_OK:
            return "OK";
        case BSP_IDLE:
            return "IDLE";
        case BSP_BUSY:
            return "BUSY";
        case BSP_ERR_NOT_INITIALIZED:
            return "NOT_INITIALIZED";
        case BSP_ERR_PARAM:
            return "PARAM";
        case BSP_ERR_CONFIG:
            return "CONFIG";
        case BSP_ERR_TIMEOUT:
            return "TIMEOUT";
        case BSP_ERR_HAL:
            return "HAL";
        case BSP_ERR_DATA:
            return "DATA";
        case BSP_ERR_BUFFER_TOO_SMALL:
            return "BUFFER_TOO_SMALL";
        default:
            return "UNKNOWN";
    }
}
