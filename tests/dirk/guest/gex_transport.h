#ifndef DIRK_GEX_TRANSPORT_H
#define DIRK_GEX_TRANSPORT_H

#include "dirk_guest.h"

/* The wire over /dev/gex-d3d. 0, or -1 with the reason said. */
int dirk_gex_transport(struct dirk_transport *out);

#endif
