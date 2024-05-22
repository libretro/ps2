/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <deque>
#include <memory>
#include <vector>

#include "Memory.h"

enum class FreezeAction
{
	Load,
	Save,
	Size,
};

// Savestate Versioning!

// NOTICE: When updating g_SaveVersion, please make sure you add the following line to your commit message somewhere:
// [SAVEVERSION+]

static const u32 g_SaveVersion = (0x9A43 << 16) | 0x0000;


// the freezing data between submodules and core
// an interesting thing to note is that this dates back from before plugin
// merges and was used to pass data between plugins and cores, although the
// struct was system dependant as the size of int differs between systems, thus
// subsystems making use of freezeData, like save states aren't
// necessarily portable; we might want to investigate this in the future -- govanify
struct freezeData
{
	int size;
	u8* data;
};

// --------------------------------------------------------------------------------------
//  SaveStateBase class
// --------------------------------------------------------------------------------------
// Provides the base API for both loading and saving savestates.  Normally you'll want to
// use one of the four "functional" derived classes rather than this class directly: gzLoadingState, gzSavingState (gzipped disk-saved
// states), and memLoadingState, memSavingState (uncompressed memory states).
class SaveStateBase
{
protected:
	std::vector<u8>& m_memory;
	char m_tagspace[32];

	int m_idx = 0;			// current read/write index of the allocation
	bool m_error = false; // error occurred while reading/writing

public:
	SaveStateBase( std::vector<u8>& memblock );
	virtual ~SaveStateBase() { }

	__fi bool IsOkay() const { return !m_error; }

	bool FreezeBios();
	bool FreezeInternals();

	// Loads or saves an arbitrary data type.  Usable on atomic types, structs, and arrays.
	// For dynamically allocated pointers use FreezeMem instead.
	template<typename T>
	void Freeze( T& data )
	{
		FreezeMem( const_cast<void*>((void*)&data), sizeof( T ) );
	}

	void PrepBlock( int size );

	template <typename T>
	void FreezeDeque(std::deque<T>& q)
	{
		// overwritten when loading
		u32 count = static_cast<u32>(q.size());
		Freeze(count);

		// have to use a temp array, because deque doesn't have a contiguous block of memory
		std::unique_ptr<T[]> temp;
		if (count > 0)
		{
			temp = std::make_unique<T[]>(count);
			if (IsSaving())
			{
				u32 pos = 0;
				for (const T& it : q)
					temp[pos++] = it;
			}

			FreezeMem(temp.get(), static_cast<int>(sizeof(T) * count));
		}

		if (IsLoading())
		{
			q.clear();
			for (u32 i = 0; i < count; i++)
				q.push_back(temp[i]);
		}
	}

	u8* GetBlockPtr()
	{
		return &m_memory[m_idx];
	}

	u8* GetPtrEnd() const
	{
		return &m_memory[m_idx];
	}

	void CommitBlock( int size )
	{
		m_idx += size;
	}

	// Freezes an identifier value into the savestate for troubleshooting purposes.
	// Identifiers can be used to determine where in a savestate that data has become
	// skewed (if the value does not match then the error occurs somewhere prior to that
	// position).
	bool FreezeTag( const char* src );

	// Returns true if this object is a StateLoading type object.
	bool IsLoading() const { return !IsSaving(); }

	// Loads or saves a memory block.
	virtual void FreezeMem( void* data, int size )=0;

	// Returns true if this object is a StateSaving type object.
	virtual bool IsSaving() const=0;

public:
	// note: gsFreeze() needs to be public because of the GSState recorder.
	bool gsFreeze();

protected:
	void Init( std::vector<u8>* memblock );

	// Load/Save functions for the various components of our glorious emulator!
	//bool vmFreeze();
	bool mtvuFreeze();
	bool rcntFreeze();
	bool vuMicroFreeze();
	bool vuJITFreeze();
	bool vif0Freeze();
	bool vif1Freeze();
	bool sifFreeze();
	bool ipuFreeze();
	bool ipuDmaFreeze();
	bool gifFreeze();
	bool gifDmaFreeze();
	bool gifPathFreeze(u32 path); // called by gifFreeze()

	bool sprFreeze();

	bool sioFreeze();
	bool cdrFreeze();
	bool cdvdFreeze();
	bool psxRcntFreeze();
	bool sio2Freeze();

	bool deci2Freeze();

};

// --------------------------------------------------------------------------------------
//  Saving and Loading Specialized Implementations...
// --------------------------------------------------------------------------------------

class memSavingState : public SaveStateBase
{
	typedef SaveStateBase _parent;

protected:
	// 256k reallocation block size.
	static const int ReallocThreshold	= _1mb / 4;
	// 8 meg base alloc when PS2 main memory is excluded
	static const int MemoryBaseAllocSize	= _8mb;

public:
	virtual ~memSavingState() = default;
	memSavingState( std::vector<u8>& save_to );

	void FreezeMem( void* data, int size );

	bool IsSaving() const { return true; }
};

class memLoadingState : public SaveStateBase
{
public:
	virtual ~memLoadingState() = default;
	memLoadingState( const std::vector<u8>& load_from );

	void FreezeMem( void* data, int size );

	bool IsSaving() const { return false; }
};
