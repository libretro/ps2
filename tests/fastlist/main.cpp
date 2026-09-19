/* FastList now allocates on the first insertion rather than in clear().
 * Everything it does must be unchanged, including on a list that has
 * never held anything. */
#include <cstdio>
#include <vector>
#include <algorithm>
#include "common/Pcsx2Defs.h"
#include "GS/Renderers/Common/GSFastList.h"
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL: %s\n", m); fails++; } } while (0)
int main()
{
	{	/* never touched */
		FastList<int> l;
		CHECK(l.size() == 0, "fresh: size 0");
		CHECK(l.empty(), "fresh: empty");
		CHECK(l.begin() == l.end(), "fresh: begin == end");
		int n = 0;
		for (auto it = l.begin(); it != l.end(); ++it) n++;
		CHECK(n == 0, "fresh: iterates nothing");
		n = 0;
		for (auto it = l.rbegin(); it != l.rend(); ++it) n++;
		CHECK(n == 0, "fresh: reverse-iterates nothing");
		l.clear();
		CHECK(l.empty(), "cleared while empty: still empty");
	}
	{	/* insert, read back, erase, and again after a clear */
		FastList<int> l;
		for (int pass = 0; pass < 3; pass++)
		{
			std::vector<int> want;
			for (int i = 0; i < 300; i++) { l.push_front(i); want.push_back(i); }
			std::reverse(want.begin(), want.end());
			CHECK(l.size() == 300, "300 inserted: size");
			std::vector<int> got;
			for (auto it = l.begin(); it != l.end(); ++it) got.push_back(*it);
			CHECK(got == want, "front insertion order preserved");
			std::vector<int> rgot;
			for (auto it = l.rbegin(); it != l.rend(); ++it) rgot.push_back(*it);
			std::reverse(rgot.begin(), rgot.end());
			CHECK(rgot == want, "reverse iteration is the mirror");
			CHECK(l.back() == 0, "back is the oldest");
			l.pop_back();
			CHECK(l.size() == 299, "pop_back: size");
			for (auto it = l.begin(); it != l.end();)
				it = (*it % 2) ? l.erase(it) : ++it;
			CHECK(l.size() == 149, "erase every odd: size (1..299 has 150 odds)");
			for (auto it = l.begin(); it != l.end(); ++it)
				CHECK(*it % 2 == 0, "only evens remain");
			l.clear();
			CHECK(l.size() == 0 && l.begin() == l.end(), "clear empties it");
		}
	}
	{	/* MoveFront, which walks the chain through index 0 */
		FastList<int> l;
		u16 idx[8];
		for (int i = 0; i < 8; i++) idx[i] = l.InsertFront(i);
		l.MoveFront(idx[0]);
		CHECK(*l.begin() == 0, "MoveFront brings it to the front");
		l.MoveFront(idx[0]);
		CHECK(*l.begin() == 0, "MoveFront of the front is a no-op");
	}
	printf(fails ? "fastlist: FAILED (%d)\n" : "fastlist: ok\n", fails);
	return fails != 0;
}
