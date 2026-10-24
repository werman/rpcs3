#include "stdafx.h"
#include "Emu/Cell/PPUModule.h"
#include "Emu/Cell/Modules/cellPamf.h"
#include "Emu/Cell/Modules/libavcdec.h"
#include "Emu/Cell/Modules/libdivx311dec.h"
#include "Emu/Cell/Modules/libdivxdec.h"
#include "Emu/Cell/Modules/libmvcdec.h"
#include "Emu/Cell/Modules/libsjvtd.h"
#include "Emu/Cell/Modules/libsmvd2.h"
#include "Emu/Cell/Modules/libsmvd4.h"
#include "Emu/Cell/Modules/libsvc1d.h"
#include "Emu/Cell/Modules/cellVdec.h"

DECLARE(ppu_module_manager::cellVdec)("libvdec", []()
{
});

bool check_if_vdec_contexts_exist()
{
	return false;
}

template<>
void fmt_class_string<CellVdecError>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto error)
	{
		switch (error)
		{
			STR_CASE(CELL_VDEC_ERROR_ARG);
			STR_CASE(CELL_VDEC_ERROR_SEQ);
			STR_CASE(CELL_VDEC_ERROR_BUSY);
			STR_CASE(CELL_VDEC_ERROR_EMPTY);
			STR_CASE(CELL_VDEC_ERROR_AU);
			STR_CASE(CELL_VDEC_ERROR_PIC);
			STR_CASE(CELL_VDEC_ERROR_UNK);
			STR_CASE(CELL_VDEC_ERROR_FATAL);
		}

		return unknown;
	});
}
