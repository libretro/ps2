/* The C++ the IsoFS harness cannot avoid: IsoDirectory is a C++ class
 * and the sector source is declared with C++ linkage. Everything here
 * forwards; the disc and the checks are in main.c. */
#include <string.h>
#include <libretro.h>

#include "CDVD/IsoFS/IsoFS.h"
#include "CDVD/IsoFS/IsoFile.h"
#include "CDVD/IsoFS/SectorSource.h"

extern "C" {
int isofs_harness_read(unsigned char* buffer, int lba);
void isofs_harness_log(enum retro_log_level level, const char* fmt, ...);

/* 1 and the entry's place when the path is found, 0 when it is not. */
int isofs_harness_find(const char* path, unsigned* lba, unsigned* size)
{
	IsoDirectory root;
	if (!root.OpenRootDirectory())
		return -1;
	const std::optional<IsoFileDescriptor> fd = root.FindFile(path);
	if (!fd.has_value())
		return 0;
	*lba  = fd->lba;
	*size = fd->size;
	return 1;
}
}

retro_log_printf_t log_cb = isofs_harness_log;

bool isofs_read_sector(unsigned char* buffer, int lba)
{
	return isofs_harness_read(buffer, lba) != 0;
}
