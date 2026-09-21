/* sio2Freeze's FIFO serialisation, round-tripped against the real SioFifo.
 *
 * The state carries a SioFifo as a u32 count followed by its bytes in
 * front-to-back order. That format has not changed; what changed is the
 * load side, which used to read into a std::unique_ptr<u8[]> and push the
 * bytes across one at a time, and now sizes the fifo first and reads
 * straight into its storage. Those are only the same if a cleared fifo
 * really does lay count pushes out contiguously from data[0] -- so that is
 * what this checks, against the fifo itself rather than a copy of it.
 *
 * The cases that matter are the ones where head is not 0: a fifo that has
 * been partly drained stores its live bytes at an offset, and the save side
 * writes from data + head while the load side writes back to data + 0.
 */

#include <cstdio>
#include <cstdlib>
#include "Sio.h"

extern "C" {
#include "SaveState.h"
}

static long checks;
static long failures;

static void expect(bool cond, const char *what)
{
	checks++;
	if (!cond)
	{
		printf("  FAIL: %s\n", what);
		failures++;
	}
}

/* Lifted from Sio.cpp so the harness exercises the same body. Keep the two
 * in step; a divergence here is the finding. */
static void FreezeSioFifo(SaveStateBase *s, SioFifo &q)
{
	u32 count = (u32)q.size();

	SaveState_Freeze(s, count);

	if (count == 0)
	{
		if (SaveState_IsLoading(s))
			q.clear();
		return;
	}

	if (SaveState_IsSaving(s))
	{
		SaveState_FreezeMem(s, q.data + q.head, (int)count);
		return;
	}

	q.clear();
	{
		u32 i;

		for (i = 0; i < count; i++)
			q.push_back(0);
		SaveState_FreezeMem(s, q.data, (int)count);
	}
}

/* Build a fifo holding `live` bytes with `drained` already popped off the
 * front, so head lands where a half-used fifo would leave it. */
static void build(SioFifo &q, int drained, int live, u8 seed)
{
	int i;

	q.clear();
	for (i = 0; i < drained + live; i++)
		q.push_back((u8)(seed + i));
	for (i = 0; i < drained; i++)
		q.pop_front();
}

static void round_trip(int drained, int live)
{
	SioFifo out, in;
	SaveStateBase w, r;
	int i;

	build(out, drained, live, 0x40);
	expect((int)out.size() == live, "the fifo holds what was put in it");

	/* and something different in the destination, so a no-op load shows */
	build(in, 3, 11, 0x90);

	SaveState_Init(&w, NULL, 0, 0, true);
	FreezeSioFifo(&w, out);

	SaveState_Init(&r, w.memory, w.memory_size, w.memory_cap, false);
	FreezeSioFifo(&r, in);

	expect((int)in.size() == live, "the count came back");
	expect(r.idx == w.idx, "both passes covered the same bytes");

	for (i = 0; i < live; i++)
	{
		if (in.data[in.head + i] != out.data[out.head + i])
		{
			printf("  FAIL: byte %d of a %d-deep fifo (head %d)\n",
			       i, live, (int)out.head);
			failures++;
			break;
		}
	}
	checks++;

	free(w.memory);
	out.free_storage();
	in.free_storage();
}

int main(void)
{
	int drained, live;

	/* Across a growth step (capacity starts at 64 and doubles) and on
	 * both sides of it, at every head offset that fits. */
	for (live = 0; live <= 200; live += live < 8 ? 1 : 17)
		for (drained = 0; drained <= 70; drained += drained < 4 ? 1 : 23)
			round_trip(drained, live);

	printf("%s: sio fifo, %ld checks, %ld failures\n",
	       failures ? "FAIL" : "PASS", checks, failures);
	return failures != 0;
}
