#ifndef BINARYFUSEFILTER_NEW
#define BINARYFUSEFILTER_NEW
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef XOR_MAX_ITERATIONS
#define XOR_MAX_ITERATIONS \
  100 // probability of success should always be > 0.5 so 100 iterations is
      // highly unlikely
#endif

namespace binary_fuse
{
  static int cmpfunc(const void *a, const void *b)
  {
    return (*(const uint64_t *)a - *(const uint64_t *)b);
  }

  static size_t sort_and_remove_dup(uint64_t *keys, size_t length)
  {
    qsort(keys, length, sizeof(uint64_t), cmpfunc);
    size_t j = 0;
    for (size_t i = 1; i < length; i++)
    {
      if (keys[i] != keys[i - 1])
      {
        keys[j] = keys[i];
        j++;
      }
    }
    return j + 1;
  }

  static inline uint64_t murmur64(uint64_t h)
  {
    h ^= h >> 33;
    h *= UINT64_C(0xff51afd7ed558ccd);
    h ^= h >> 33;
    h *= UINT64_C(0xc4ceb9fe1a85ec53);
    h ^= h >> 33;
    return h;
  }
  static inline uint64_t mix_split(uint64_t key, uint64_t seed)
  {
    return murmur64(key + seed);
  }
  static inline uint64_t rotl64(uint64_t n, unsigned int c)
  {
    return (n << (c & 63)) | (n >> ((-c) & 63));
  }
  static inline uint32_t reduce(uint32_t hash, uint32_t n)
  {
    // http://lemire.me/blog/2016/06/27/a-fast-alternative-to-the-modulo-reduction/
    return (uint32_t)(((uint64_t)hash * n) >> 32);
  }

  static inline uint64_t fingerprint(uint64_t hash)
  {
    return hash ^ (hash >> 32);
  }

  static inline uint64_t rng_splitmix64(uint64_t *seed)
  {
    uint64_t z = (*seed += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
  }

// #ifdefs adapted from:
//  https://stackoverflow.com/a/50958815
#ifdef __SIZEOF_INT128__ // compilers supporting __uint128, e.g., gcc, clang
  static inline uint64_t mulhi(uint64_t a, uint64_t b)
  {
    return ((__uint128_t)a * b) >> 64;
  }
#elif defined(_M_X64) || defined(_MARM64) // MSVC
  static inline uint64_t mulhi(uint64_t a, uint64_t b)
  {
    return __umulh(a, b);
  }
#elif defined(_M_IA64)                    // also MSVC
  static inline uint64_t mulhi(uint64_t a, uint64_t b)
  {
    unsigned __int64 hi;
    (void)_umul128(a, b, &hi);
    return hi;
  }
#else                                     // portable implementation using uint64_t
  static inline uint64_t mulhi(uint64_t a, uint64_t b)
  {
    // Adapted from:
    //  https://stackoverflow.com/a/51587262

    /*
          This is implementing schoolbook multiplication:

                  a1 a0
          X       b1 b0
          -------------
                     00  LOW PART
          -------------
                  00
               10 10     MIDDLE PART
          +       01
          -------------
               01
          + 11 11        HIGH PART
          -------------
    */

    const uint64_t a0 = (uint32_t)a;
    const uint64_t a1 = a >> 32;
    const uint64_t b0 = (uint32_t)b;
    const uint64_t b1 = b >> 32;
    const uint64_t p11 = a1 * b1;
    const uint64_t p01 = a0 * b1;
    const uint64_t p10 = a1 * b0;
    const uint64_t p00 = a0 * b0;

    // 64-bit product + two 32-bit values
    const uint64_t middle = p10 + (p00 >> 32) + (uint32_t)p01;

    /*
      Proof that 64-bit products can accumulate two more 32-bit values
      without overflowing:

      Max 32-bit value is 2^32 - 1.
      PSum = (2^32-1) * (2^32-1) + (2^32-1) + (2^32-1)
           = 2^64 - 2^32 - 2^32 + 1 + 2^32 - 1 + 2^32 - 1
           = 2^64 - 1
      Therefore the high half below cannot overflow regardless of input.
    */

    // high half
    return p11 + (middle >> 32) + (p01 >> 32);

    // low half (which we don't care about, but here it is)
    // (middle << 32) | (uint32_t) p00;
  }
#endif

  static inline uint32_t calculate_segment_length(uint32_t arity,
                                                  uint32_t size)
  {
    // These parameters are very sensitive. Replacing 'floor' by 'round' can
    // substantially affect the construction time.
    if (arity == 3)
    {
      return ((uint32_t)1) << (int)(floor(log((double)(size)) / log(3.33) + 2.25));
    }
    else if (arity == 4)
    {
      return ((uint32_t)1) << (int)(floor(log((double)(size)) / log(2.91) - 0.5));
    }
    else
    {
      return 65536;
    }
  }

  static inline double binary_fuse_max(double a, double b)
  {
    if (a < b)
    {
      return b;
    }
    return a;
  }

  static inline double calculate_size_factor(uint32_t arity,
                                             uint32_t size)
  {
    if (arity == 3)
    {
      return binary_fuse_max(1.125, 0.875 + 0.25 * log(1000000.0) / log((double)size));
    }
    else if (arity == 4)
    {
      return binary_fuse_max(1.075, 0.77 + 0.305 * log(600000.0) / log((double)size));
    }
    else
    {
      return 2.0;
    }
  }

  static inline uint8_t mod3(uint8_t x)
  {
    return x > 2 ? x - 3 : x;
  }

  typedef struct binary_hashes_s
  {
    uint32_t h0;
    uint32_t h1;
    uint32_t h2;
  } binary_hashes_t;

  template <typename ItemType, typename FingerprintType>
  class BinaryFuseFilter
  {
    // typedef struct binary_fuse32_s
    // {
    uint64_t Seed;
    uint32_t SegmentLength;
    uint32_t SegmentLengthMask;
    uint32_t SegmentCount;
    uint32_t SegmentCountLength;
    uint32_t ArrayLength;
    FingerprintType *Fingerprints;
    // } binary_fuse_t;

    typedef struct binary_fuse32_s
    {
      uint64_t Seed;
      uint32_t SegmentLength;
      uint32_t SegmentLengthMask;
      uint32_t SegmentCount;
      uint32_t SegmentCountLength;
      uint32_t ArrayLength;
      FingerprintType *Fingerprints;
    } binary_fuse_t;

    binary_hashes_t hash_batch(uint64_t hash)
    //  const binary_fuse_t *filter)
    {
      uint64_t hi = mulhi(hash, SegmentCountLength);
      binary_hashes_t ans;
      ans.h0 = (uint32_t)hi;
      ans.h1 = ans.h0 + SegmentLength;
      ans.h2 = ans.h1 + SegmentLength;
      ans.h1 ^= (uint32_t)(hash >> 18) & SegmentLengthMask;
      ans.h2 ^= (uint32_t)(hash)&SegmentLengthMask;
      return ans;
    }

    uint32_t fuse_hash(int index, uint64_t hash) const
    //  const binary_fuse_t *filter)
    {
      uint64_t h = mulhi(hash, SegmentCountLength);
      h += index * SegmentLength;
      // keep the lower 36 bits
      uint64_t hh = hash & ((1UL << 36) - 1);
      // index 0: right shift by 36; index 1: right shift by 18; index 2: no shift
      h ^= (size_t)((hh >> (36 - 18 * index)) & SegmentLengthMask);
      return h;
    }

    binary_fuse_t *filter;

  public:
    BinaryFuseFilter(const size_t size)
    {
      uint32_t arity = 3;
      SegmentLength = size == 0 ? 4 : calculate_segment_length(arity, size);
      if (SegmentLength > 262144)
      {
        SegmentLength = 262144;
      }
      SegmentLengthMask = SegmentLength - 1;
      double sizeFactor = size <= 1 ? 0 : calculate_size_factor(arity, size);
      uint32_t capacity = size <= 1 ? 0 : (uint32_t)(round((double)size * sizeFactor));
      uint32_t initSegmentCount =
          (capacity + SegmentLength - 1) / SegmentLength -
          (arity - 1);
      ArrayLength = (initSegmentCount + arity - 1) * SegmentLength;
      SegmentCount =
          (ArrayLength + SegmentLength - 1) / SegmentLength;
      if (SegmentCount <= arity - 1)
      {
        SegmentCount = 1;
      }
      else
      {
        SegmentCount = SegmentCount - (arity - 1);
      }
      ArrayLength =
          (SegmentCount + arity - 1) * SegmentLength;
      SegmentCountLength = SegmentCount * SegmentLength;
      Fingerprints = (FingerprintType *)malloc(ArrayLength * sizeof(FingerprintType));
      // return Fingerprints != NULL;
    }

    ~BinaryFuseFilter()
    {
      free(Fingerprints);
      Fingerprints = NULL;
      Seed = 0;
      SegmentLength = 0;
      SegmentLengthMask = 0;
      SegmentCount = 0;
      SegmentCountLength = 0;
      ArrayLength = 0;
    }

    bool Populate(uint64_t *keys, uint32_t size);

    bool Contain(const ItemType key) const
    {
      uint64_t hash = murmur64(key + Seed);
      uint32_t h0 = fuse_hash(0, hash);
      uint32_t h1 = fuse_hash(1, hash);
      uint32_t h2 = fuse_hash(2, hash);
      FingerprintType f = fingerprint(hash);
      return (f == (Fingerprints[h0] ^ Fingerprints[h1] ^ Fingerprints[h2]));
    }

    size_t SizeInBytes() const
    {
      return ArrayLength * sizeof(FingerprintType) + sizeof(binary_fuse_t);
    }
  };

  template <typename ItemType, typename FingerprintType>
  bool BinaryFuseFilter<ItemType, FingerprintType>::Populate(uint64_t *keys, uint32_t size)
  {
    uint64_t rng_counter = 0x726b2b9d438b9d4d;
    Seed = rng_splitmix64(&rng_counter);
    uint64_t *reverseOrder = (uint64_t *)calloc((size + 1), sizeof(uint64_t));
    uint32_t capacity = ArrayLength;
    uint32_t *alone = (uint32_t *)malloc(capacity * sizeof(uint32_t));
    uint8_t *t2count = (uint8_t *)calloc(capacity, sizeof(uint8_t));
    uint8_t *reverseH = (uint8_t *)malloc(size * sizeof(uint8_t));
    uint64_t *t2hash = (uint64_t *)calloc(capacity, sizeof(uint64_t));

    uint32_t blockBits = 1;
    while (((uint32_t)1 << blockBits) < SegmentCount)
    {
      blockBits += 1;
    }
    uint32_t block = ((uint32_t)1 << blockBits);
    uint32_t *startPos = (uint32_t *)malloc((1 << blockBits) * sizeof(uint32_t));
    uint32_t h012[5];

    if ((alone == NULL) || (t2count == NULL) || (reverseH == NULL) ||
        (t2hash == NULL) || (reverseOrder == NULL) || (startPos == NULL))
    {
      free(alone);
      free(t2count);
      free(reverseH);
      free(t2hash);
      free(reverseOrder);
      free(startPos);
      return false;
    }
    reverseOrder[size] = 1;
    for (int loop = 0; true; ++loop)
    {
      if (loop + 1 > XOR_MAX_ITERATIONS)
      {
        // The probability of this happening is lower than the
        // the cosmic-ray probability (i.e., a cosmic ray corrupts your system)
        memset(Fingerprints, ~0, ArrayLength);
        free(alone);
        free(t2count);
        free(reverseH);
        free(t2hash);
        free(reverseOrder);
        free(startPos);
        return false;
      }

      for (uint32_t i = 0; i < block; i++)
      {
        // important : i * size would overflow as a 32-bit number in some
        // cases.
        startPos[i] = ((uint64_t)i * size) >> blockBits;
      }

      uint64_t maskblock = block - 1;
      for (uint32_t i = 0; i < size; i++)
      {
        uint64_t hash = murmur64(keys[i] + Seed);
        uint64_t segment_index = hash >> (64 - blockBits);
        while (reverseOrder[startPos[segment_index]] != 0)
        {
          segment_index++;
          segment_index &= maskblock;
        }
        reverseOrder[startPos[segment_index]] = hash;
        startPos[segment_index]++;
      }
      int error = 0;
      uint32_t duplicates = 0;
      for (uint32_t i = 0; i < size; i++)
      {
        uint64_t hash = reverseOrder[i];
        uint32_t h0 = fuse_hash(0, hash);
        t2count[h0] += 4;
        t2hash[h0] ^= hash;
        uint32_t h1 = fuse_hash(1, hash);
        t2count[h1] += 4;
        t2count[h1] ^= 1;
        t2hash[h1] ^= hash;
        uint32_t h2 = fuse_hash(2, hash);
        t2count[h2] += 4;
        t2hash[h2] ^= hash;
        t2count[h2] ^= 2;
        if ((t2hash[h0] & t2hash[h1] & t2hash[h2]) == 0)
        {
          if (((t2hash[h0] == 0) && (t2count[h0] == 8)) || ((t2hash[h1] == 0) && (t2count[h1] == 8)) || ((t2hash[h2] == 0) && (t2count[h2] == 8)))
          {
            duplicates += 1;
            t2count[h0] -= 4;
            t2hash[h0] ^= hash;
            t2count[h1] -= 4;
            t2count[h1] ^= 1;
            t2hash[h1] ^= hash;
            t2count[h2] -= 4;
            t2count[h2] ^= 2;
            t2hash[h2] ^= hash;
          }
        }
        error = (t2count[h0] < 4) ? 1 : error;
        error = (t2count[h1] < 4) ? 1 : error;
        error = (t2count[h2] < 4) ? 1 : error;
      }
      if (error)
      {
        memset(reverseOrder, 0, sizeof(uint64_t) * size);
        memset(t2count, 0, sizeof(uint8_t) * capacity);
        memset(t2hash, 0, sizeof(uint64_t) * capacity);
        Seed = rng_splitmix64(&rng_counter);
        continue;
      }

      // End of key addition
      uint32_t Qsize = 0;
      // Add sets with one key to the queue.
      for (uint32_t i = 0; i < capacity; i++)
      {
        alone[Qsize] = i;
        Qsize += ((t2count[i] >> 2) == 1) ? 1 : 0;
      }
      uint32_t stacksize = 0;
      while (Qsize > 0)
      {
        Qsize--;
        uint32_t index = alone[Qsize];
        if ((t2count[index] >> 2) == 1)
        {
          uint64_t hash = t2hash[index];

          // h012[0] = fuse_hash(0, hash);
          h012[1] = fuse_hash(1, hash);
          h012[2] = fuse_hash(2, hash);
          h012[3] = fuse_hash(0, hash); // == h012[0];
          h012[4] = h012[1];
          uint8_t found = t2count[index] & 3;
          reverseH[stacksize] = found;
          reverseOrder[stacksize] = hash;
          stacksize++;
          uint32_t other_index1 = h012[found + 1];
          alone[Qsize] = other_index1;
          Qsize += ((t2count[other_index1] >> 2) == 2 ? 1 : 0);

          t2count[other_index1] -= 4;
          t2count[other_index1] ^= mod3(found + 1);
          t2hash[other_index1] ^= hash;

          uint32_t other_index2 = h012[found + 2];
          alone[Qsize] = other_index2;
          Qsize += ((t2count[other_index2] >> 2) == 2 ? 1 : 0);
          t2count[other_index2] -= 4;
          t2count[other_index2] ^= mod3(found + 2);
          t2hash[other_index2] ^= hash;
        }
      }
      if (stacksize + duplicates == size)
      {
        // success
        size = stacksize;
        break;
      }
      else if (duplicates > 0)
      {
        size = sort_and_remove_dup(keys, size);
      }
      memset(reverseOrder, 0, sizeof(uint64_t) * size);
      memset(t2count, 0, sizeof(uint8_t) * capacity);
      memset(t2hash, 0, sizeof(uint64_t) * capacity);
      Seed = rng_splitmix64(&rng_counter);
    }

    for (uint32_t i = size - 1; i < size; i--)
    {
      // the hash of the key we insert next
      uint64_t hash = reverseOrder[i];
      FingerprintType xor2 = fingerprint(hash);
      uint8_t found = reverseH[i];
      h012[0] = fuse_hash(0, hash);
      h012[1] = fuse_hash(1, hash);
      h012[2] = fuse_hash(2, hash);
      h012[3] = h012[0];
      h012[4] = h012[1];
      Fingerprints[h012[found]] = xor2 ^
                                  Fingerprints[h012[found + 1]] ^
                                  Fingerprints[h012[found + 2]];
    }
    free(alone);
    free(t2count);
    free(reverseH);
    free(t2hash);
    free(reverseOrder);
    free(startPos);
    return true;
  }
} // namespace binary_fuse
#endif
