/* Facts about the machine, captured once from the ROM's low-memory globals
 * before the kernel takes over the exception vectors. */
#include "kernel.h"
#include "hw.h"

struct machine machine;

void machine_init(void)
{
    machine.mem_top = PEEK32(LM_MemTop);
    machine.screen = (u8 *)PEEK32(LM_ScrnBase);
    machine.rom_base = PEEK32(LM_ROMBase);
    machine.rom_version = PEEK16(machine.rom_base + 8);
    machine.rom_via = PEEK32(LM_VIA);
    machine.rom_scc_rd = PEEK32(LM_SCCRd);
    machine.rom_scc_wr = PEEK32(LM_SCCWr);
    machine.rom_sys_zone = PEEK32(LM_SysZone);
    /* A heap zone starts with a pointer to its last block. */
    machine.rom_sys_zone_end = PEEK32(machine.rom_sys_zone);
    machine.rom_appl_zone = PEEK32(LM_ApplZone);
    machine.boot_drive = (s16)PEEK16(LM_BootDrive);
    machine.boot_refnum = (s16)PEEK16(LM_BtDskRfn);
    machine.sound_base = PEEK32(LM_SoundBase);
    machine.rom_time = PEEK32(LM_Time);
    machine.rom_ticks = PEEK32(LM_Ticks);
}
