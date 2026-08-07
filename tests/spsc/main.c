/* retro_spsc concurrency stress.
 *
 * Two real threads (producer/consumer) hammer the queue through both
 * the copying API and the zero-copy span API, across many wrap points,
 * with randomized chunk sizes and a payload that self-verifies
 * ordering and integrity (monotonic byte sequence).  Designed to run
 * under ThreadSanitizer: happens-before violations in the cursor
 * protocol are detected by interleaving alone, so a 1-vCPU host is
 * sufficient - true parallelism is not required for TSan validity.
 *
 * Build/run (from repo root):
 *   sh tests/spsc/build.sh && ./tests/spsc/spsc_test
 *   TSan: see build.sh, then
 *   TSAN_OPTIONS=halt_on_error=1 ./tests/spsc/spsc_test_tsan
 *
 * Exit 0 on success; nonzero plus a diagnostic on any integrity or
 * protocol failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#include <retro_spsc.h>

#define QUEUE_MIN_CAP   4096          /* small: forces frequent wraps */
#define TOTAL_BYTES     (32u << 20)   /* 32 MiB through the queue */
#define MAX_CHUNK       1500          /* not a divisor of capacity:  */
                                      /* guarantees unaligned wraps  */

static retro_spsc_t q;

/* Deterministic per-side xorshift so chunk sizes differ between
 * producer and consumer but runs are reproducible. */
static uint32_t xs(uint32_t* s)
{
   uint32_t x = *s;
   x ^= x << 13;
   x ^= x >> 17;
   x ^= x << 5;
   return *s = x;
}

static void* producer(void* arg)
{
   uint32_t rng  = 0x12345678u;
   uint64_t sent = 0;
   uint8_t  next = 0;
   uint8_t  tmp[MAX_CHUNK];
   (void)arg;

   while (sent < TOTAL_BYTES)
   {
      uint32_t want = 1 + (xs(&rng) % MAX_CHUNK);
      if (sent + want > TOTAL_BYTES)
         want = (uint32_t)(TOTAL_BYTES - sent);

      if (xs(&rng) & 1)
      {
         /* Zero-copy path: write directly into the span.  The span
          * can be shorter than the request near the wrap; take what
          * is offered (bounded), never exceed it. */
         void*  span;
         size_t span_len = retro_spsc_write_begin(&q, &span);
         size_t take     = span_len < want ? span_len : want;
         size_t k;
         if (take == 0)
         {
            retro_spsc_write_end(&q, 0); /* abandon; queue full/at wrap edge */
            continue;
         }
         for (k = 0; k < take; k++)
            ((uint8_t*)span)[k] = (uint8_t)(next + k);
         retro_spsc_write_end(&q, take);
         next  = (uint8_t)(next + take);
         sent += take;
      }
      else
      {
         /* Copying path. */
         size_t   avail = retro_spsc_write_avail(&q);
         uint32_t take  = avail < want ? (uint32_t)avail : want;
         uint32_t k;
         size_t   wrote;
         if (take == 0)
            continue;
         for (k = 0; k < take; k++)
            tmp[k] = (uint8_t)(next + k);
         wrote = retro_spsc_write(&q, tmp, take);
         if (wrote != take)
         {
            fprintf(stderr, "FAIL: short copying write (%zu of %u) after avail said %zu\n",
                    wrote, take, avail);
            exit(2);
         }
         next  = (uint8_t)(next + take);
         sent += take;
      }
   }
   return NULL;
}

static void* consumer(void* arg)
{
   uint32_t rng  = 0x9e3779b9u;
   uint64_t got  = 0;
   uint8_t  next = 0;
   uint8_t  tmp[MAX_CHUNK];
   (void)arg;

   while (got < TOTAL_BYTES)
   {
      if (xs(&rng) & 1)
      {
         /* Zero-copy drain: verify in place. */
         const void* span;
         size_t      span_len = retro_spsc_read_begin(&q, &span);
         size_t      k;
         if (span_len == 0)
         {
            retro_spsc_read_end(&q, 0);
            continue;
         }
         if (span_len > MAX_CHUNK && (xs(&rng) & 3) == 0)
            span_len = MAX_CHUNK; /* sometimes consume partial spans */
         for (k = 0; k < span_len; k++)
         {
            const uint8_t v = ((const uint8_t*)span)[k];
            if (v != (uint8_t)(next + k))
            {
               fprintf(stderr, "FAIL: span byte %llu: got %02x want %02x\n",
                       (unsigned long long)(got + k), v, (uint8_t)(next + k));
               exit(3);
            }
         }
         retro_spsc_read_end(&q, span_len);
         next = (uint8_t)(next + span_len);
         got += span_len;
      }
      else
      {
         uint32_t want = 1 + (xs(&rng) % MAX_CHUNK);
         size_t   rd   = retro_spsc_read(&q, tmp, want);
         uint32_t k;
         for (k = 0; k < rd; k++)
         {
            if (tmp[k] != (uint8_t)(next + k))
            {
               fprintf(stderr, "FAIL: copy byte %llu: got %02x want %02x\n",
                       (unsigned long long)(got + k), tmp[k], (uint8_t)(next + k));
               exit(4);
            }
         }
         next = (uint8_t)(next + rd);
         got += rd;
      }
   }

   /* Everything consumed: queue must report empty. */
   if (retro_spsc_read_avail(&q) != 0)
   {
      fprintf(stderr, "FAIL: %zu bytes left after full drain\n",
              retro_spsc_read_avail(&q));
      exit(5);
   }
   return NULL;
}

int main(void)
{
   pthread_t pt, ct;

   if (!retro_spsc_init(&q, QUEUE_MIN_CAP))
   {
      fprintf(stderr, "FAIL: init\n");
      return 1;
   }

   pthread_create(&pt, NULL, producer, NULL);
   pthread_create(&ct, NULL, consumer, NULL);
   pthread_join(pt, NULL);
   pthread_join(ct, NULL);

   retro_spsc_free(&q);
   printf("spsc_test OK: %u MiB, copying+span APIs, capacity %u\n",
          TOTAL_BYTES >> 20, QUEUE_MIN_CAP);
   return 0;
}
