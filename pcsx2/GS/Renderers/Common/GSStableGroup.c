/* See GSStableGroup.h. C89. */
#include <string.h>

#include "GSStableGroup.h"

/* The largest item it moves through a stack buffer; larger ones are
 * swapped down a byte at a time. */
#define GS_GROUP_ITEM_MAX 128

void gs_stable_group(void* items, size_t count, size_t size,
	int (*same)(const void* a, const void* b))
{
	unsigned char* base = (unsigned char*)items;
	unsigned char  held[GS_GROUP_ITEM_MAX];
	size_t start = 0;

	while (start < count)
	{
		/* [start, end) is the group of base[start]; everything past end
		 * is still in its original order. */
		size_t end = start + 1;
		size_t i;

		for (i = end; i < count; i++)
		{
			if (!same(base + start * size, base + i * size))
				continue;
			if (i != end)
			{
				if (size <= GS_GROUP_ITEM_MAX)
				{
					memcpy(held, base + i * size, size);
					memmove(base + (end + 1) * size, base + end * size, (i - end) * size);
					memcpy(base + end * size, held, size);
				}
				else
				{
					size_t j, b;
					for (j = i; j > end; j--)
						for (b = 0; b < size; b++)
						{
							const unsigned char t = base[j * size + b];
							base[j * size + b] = base[(j - 1) * size + b];
							base[(j - 1) * size + b] = t;
						}
				}
			}
			end++;
		}
		start = end;
	}
}
