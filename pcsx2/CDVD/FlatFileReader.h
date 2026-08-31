// SPDX-FileCopyrightText: 2002-2024 PCSX2 Dev Team
// SPDX-License-Identifier: LGPL-3.0+

#pragma once

#include "../../common/Pcsx2Defs.h"

#include "HostFS.h"

#include "ThreadedFileReader.h"

class FlatFileReader final : public ThreadedFileReader
{
	DeclareNoncopyableObject(FlatFileReader);

	RFILE* m_file = nullptr;
	u64 m_file_size = 0;

public:
	FlatFileReader();
	~FlatFileReader();

	bool Open2(const char* filename);

	static const Ops s_ops;

	Chunk ChunkForOffset(u64 offset);
	int ReadChunk(void* dst, s64 blockID);

	void Close2();

	u32 GetBlockCount() const;
};
