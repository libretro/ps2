/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
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

#include "../SW/GSScanlineEnvironment.h"
#include "../../GSExtra.h"

#include "../../../VirtualMemory.h"

template <class KEY, class VALUE>
class GSFunctionMap
{
protected:
	struct ActivePtr
	{
		u64 frame;
		VALUE f;
	};

	std::unordered_map<KEY, ActivePtr*> m_map_active;

	ActivePtr* m_active;

	virtual VALUE GetDefaultFunction(KEY key) = 0;

public:
	GSFunctionMap()
		: m_active(NULL)
	{
	}

	virtual ~GSFunctionMap()
	{
		for (auto& i : m_map_active)
			delete i.second;
	}

	VALUE operator[](KEY key)
	{
		m_active = NULL;

		auto it = m_map_active.find(key);

		if (it != m_map_active.end())
			m_active = it->second;
		else
		{
			ActivePtr* p = new ActivePtr();

			memset(p, 0, sizeof(*p));

			p->frame = (u64)-1;

			p->f = GetDefaultFunction(key);

			m_map_active[key] = p;

			m_active = p;
		}

		return m_active->f;
	}
};

// --------------------------------------------------------------------------------------
//  GSCodeReserve
// --------------------------------------------------------------------------------------
// Stores code buffers for the GS software JIT.
//
class GSCodeReserve : public RecompiledCodeReserve
{
public:
	GSCodeReserve();
	~GSCodeReserve();

	static GSCodeReserve& GetInstance();

	void Assign(VirtualMemoryManagerPtr allocator);
	void Reset();

	u8* Reserve(size_t size);
	void Commit(size_t size);

private:
	size_t m_memory_used = 0;
};

template <class CG, class KEY, class VALUE>
class GSCodeGeneratorFunctionMap : public GSFunctionMap<KEY, VALUE>
{
	std::unordered_map<u64, VALUE> m_cgmap;

	enum { MAX_SIZE = 8192 };

public:
	GSCodeGeneratorFunctionMap() { }

	~GSCodeGeneratorFunctionMap() = default;

	void Clear()
	{
		m_cgmap.clear();
	}

	VALUE GetDefaultFunction(KEY key)
	{
		VALUE ret = nullptr;

		auto i = m_cgmap.find(key);

		if (i != m_cgmap.end())
			ret = i->second;
		else
		{
			u8* code_ptr = GSCodeReserve::GetInstance().Reserve(MAX_SIZE);
			CG cg(key, code_ptr, MAX_SIZE);

			GSCodeReserve::GetInstance().Commit(cg.getSize());

			ret = (VALUE)cg.getCode();

			m_cgmap[key] = ret;
		}

		return ret;
	}
};
