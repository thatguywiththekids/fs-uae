#ifndef UAE_GAYLE_H
#define UAE_GAYLE_H

#include "uae/types.h"

extern void gayle_reset (int);
extern void gayle_hsync (void);
extern void gayle_free (void);
extern void gayle_add_ide_unit (int ch, struct uaedev_config_info *ci, struct romconfig *rc);
extern bool gayle_ide_init(struct autoconfig_info*);
extern void gayle_free_units (void);
extern void rethink_gayle (void);
extern void gayle_map_pcmcia (void);
extern void check_prefs_changed_gayle(void);
extern bool gayle_init_pcmcia(struct autoconfig_info *aci);
extern bool gayle_init_board_io_pcmcia(struct autoconfig_info *aci);
extern bool gayle_init_board_common_pcmcia(struct autoconfig_info *aci);
void pcmcia_eject(struct uae_prefs *p);
bool isideint(void);

extern int gary_toenb; // non-existing memory access = bus error.
extern int gary_timeout; // non-existing memory access = delay

#define PCMCIA_COMMON_START 0x600000
#define PCMCIA_COMMON_SIZE 0x400000
#define PCMCIA_ATTRIBUTE_START 0xa00000
#define PCMCIA_ATTRIBUTE_SIZE 0x80000

extern void gayle_dataflyer_enable(bool);

#endif /* UAE_GAYLE_H */
