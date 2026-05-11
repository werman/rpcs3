#include "stdafx.h"
#include "Emu/Cell/PPUModule.h"

DECLARE(ppu_module_manager::cellVdec)("libvdec", []()
{
});

bool check_if_vdec_contexts_exist()
{
	return false;
}
