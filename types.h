#ifndef TYPES_H
#define TYPES_H

#include <iostream>
struct strart_message
{
    uint8_t command;

    // время старта (мировое время)
    uint8_t hour;
    uint8_t min;
    uint8_t sec;
};

struct yps_tester_rs_message
{
    uint8_t cmd;
    uint8_t crc8;
};

struct yps_tester_rs_ans
{
    uint8_t ans;
    uint8_t crc8;
};

#endif // TYPES_H
