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

#include "../../common/Threading.h"
#include <vector>
#include <unordered_map>

template <class Key, class T>
class ThreadSafeMap
{
	// Exclusive lock for readers and writers alike: these maps hold a
	// handful of network sessions touched per packet event, not per
	// frame - shared-read parallelism is not worth a second lock kind.
	Threading::Mutex accessMutex;

	std::unordered_map<Key, T> map;

public:
	void Add(Key key, T value)
	{
		Threading::ScopedLock modifyLock(accessMutex);
		//Todo, check if key already exists?
		map[key] = value;
	}

	void Remove(Key key)
	{
		Threading::ScopedLock modifyLock(accessMutex);
		map.erase(key);
	}

	void Clear()
	{
		Threading::ScopedLock modifyLock(accessMutex);
		map.clear();
	}

	std::vector<Key> GetKeys()
	{
		Threading::ScopedLock readLock(accessMutex);

		std::vector<Key> keys;
		keys.reserve(map.size());

		for (auto iter = map.begin(); iter != map.end(); ++iter)
			keys.push_back(iter->first);

		return keys;
	}

	//Does not error or insert if no key is found
	bool TryGetValue(Key key, T* value)
	{
		Threading::ScopedLock readLock(accessMutex);
		auto search = map.find(key);
		if (search != map.end())
		{
			*value = map[key];
			return true;
		}
		else
			return false;
	}

	bool ContainsKey(Key key)
	{
		Threading::ScopedLock readLock(accessMutex);
		auto search = map.find(key);
		if (search != map.end())
			return true;
		else
			return false;
	}
};
