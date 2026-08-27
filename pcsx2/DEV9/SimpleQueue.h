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


#include <retro_atomic.h>
#include "common/Console.h"

//Designed to allow one thread to queue data to another thread
template <class T>
class SimpleQueue
{
private:
	struct SimpleQueueEntry
	{
		retro_atomic_int_t ready = RETRO_ATOMIC_INT_INITIALIZER(0);
		SimpleQueueEntry* next;
		T value;
	};

	retro_atomic_ptr_t head = 0;
	SimpleQueueEntry* tail = nullptr;

public:
	SimpleQueue();

	//Used by single queue thread (i.e. EE)
	void Enqueue(T entry);
	//Used by single worker thread (i.e. IO)
	bool Dequeue(T* entry);
	//May return false negative when another thread is mid Queue()
	//Intended to only be used from queue thread
	bool IsQueueEmpty();

	~SimpleQueue();
};

template <class T>
SimpleQueue<T>::SimpleQueue()
{
	tail = new SimpleQueueEntry();
	retro_atomic_store_release_ptr(&head, tail);
}

template <class T>
void SimpleQueue<T>::Enqueue(T entry)
{
	//Allocate Next entry, and assign to head
	SimpleQueueEntry* newHead = new SimpleQueueEntry();
	SimpleQueueEntry* newEntry = (SimpleQueueEntry*)retro_atomic_exchange_ptr(&head, newHead);

	//Fill in
	newEntry->value = entry;
	newEntry->next = newHead;

	//Set ready (can be dequeued)
	retro_atomic_store_release_int(&newEntry->ready, 1);
}

template <class T>
bool SimpleQueue<T>::Dequeue(T* entry)
{
	if (!retro_atomic_load_acquire_int(&tail->ready))
		return false;

	SimpleQueueEntry* retEntry = tail;
	tail = retEntry->next;

	*entry = retEntry->value;
	delete retEntry;
	return true;
}

//Note, next entry may not be ready to dequeue
template <class T>
bool SimpleQueue<T>::IsQueueEmpty()
{
	return retro_atomic_load_acquire_ptr(&head) == tail;
}

template <class T>
SimpleQueue<T>::~SimpleQueue()
{
	/* head is retro_atomic_ptr_t, i.e. a void* under every backend. Reading
	 * it through the accessor and typing the result is what keeps `delete`
	 * off a void* -- deleting one is undefined and calls no destructor,
	 * which is what -Wdelete-incomplete was pointing at. Dequeue() only
	 * moves tail, so after the drain below head still names the same
	 * sentinel node this loaded. */
	SimpleQueueEntry* sentinel =
		(SimpleQueueEntry*)retro_atomic_load_acquire_ptr(&head);

	if (sentinel != nullptr)
	{
		if (!IsQueueEmpty())
		{
			Console.Error("DEV9: Queue not empty");

			//Empty Queue
			T entry;
			while (!IsQueueEmpty())
				Dequeue(&entry);
		}

		delete sentinel;
		retro_atomic_store_release_ptr(&head, nullptr);
		tail = nullptr;
	}
}
