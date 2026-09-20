// Creator 5 heaterBoard vendor commands
//
// Copyright (C) 2026
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <stdint.h> // uint32_t
#include "command.h" // DECL_COMMAND

void
command_get_mcu_version(uint32_t *args)
{
    (void)args;
    sendf("mcu_version year=%u date=%u version=%u", 2026u, 920u, 1u);
}
DECL_COMMAND(command_get_mcu_version, "get_mcu_version");

// These handlers are inert in the stock firmware.
void
command_set_trigger_threshold(uint32_t *args)
{
    (void)args;
}
DECL_COMMAND(command_set_trigger_threshold,
             "set_trigger_threshold threshold=%i");

void
command_get_basic_param(uint32_t *args)
{
    (void)args;
}
DECL_COMMAND(command_get_basic_param, "get_basic_param num=%u");

void
command_pa_action(uint32_t *args)
{
    (void)args;
}
DECL_COMMAND(command_pa_action, "pa_action action=%u pc=%u");

void
command_get_emcu_pa_value(uint32_t *args)
{
    (void)args;
}
DECL_COMMAND(command_get_emcu_pa_value, "get_emcu_pa_value");

void
command_remove_peel(uint32_t *args)
{
    (void)args;
}
DECL_COMMAND(command_remove_peel, "remove_peel action=%u");
