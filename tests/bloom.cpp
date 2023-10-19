#include "performancecounters/benchmarker.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <random>
#include <stdlib.h>
#include <vector>
#include "filterapi.h"

#define FILTER_SIZE 500000
#define DATA_SIZE 1000000
#define TEST_SIZE 500000
#define BOGUS_SIZE 1000000
#define QUERY_SIZE (DATA_SIZE - TEST_SIZE)

std::string random_string()
{
  auto randchar = []() -> char
  {
    const char charset[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    const size_t max_index = (sizeof(charset) - 1);
    return charset[rand() % max_index];
  };
  size_t desired_length = rand() % 128;
  std::string str(desired_length, 0);
  std::generate_n(str.begin(), desired_length, randchar);
  return str;
}

void pretty_print(size_t volume, size_t bytes, std::string name,
                  event_aggregate agg, FILE *filename)
{
  fprintf(filename, ",%.2f,%.2f,%.2f", bytes / agg.fastest_elapsed_ns(), volume * 1000.0 / agg.fastest_elapsed_ns(), agg.fastest_elapsed_ns() / volume);
}

uint64_t simple_hash(const std::string &line)
{
  uint64_t h = 0;
  for (unsigned char c : line)
  {
    h = (h * 177) + c;
  }
  h ^= line.size();
  return h;
}

int main(int argc, char **argv)
{
  std::vector<std::string> inputs;
  std::vector<std::pair<bool, bool>> dataValidity(DATA_SIZE, {false, false}); // original, modified

  if (argc == 1)
  {
    printf("You must pass a list of URLs (one per line). For instance:\n");
    printf("./benchmark data/top-1m.csv  \n");
    return EXIT_FAILURE;
  }
  else
  {
    std::ifstream input(argv[1]);
    if (!input)
    {
      std::cerr << "Could not open " << argv[1] << std::endl;
      exit(EXIT_FAILURE);
    }
    size_t volume = 0;
    for (std::string line; std::getline(input, line);)
    {
      std::string ref = line;
      ref.erase(std::find_if(ref.rbegin(), ref.rend(),
                             [](unsigned char ch)
                             { return !std::isspace(ch); })
                    .base(),
                ref.end());
      volume += ref.size();
      inputs.push_back(ref);
    }
    std::cout << "loaded " << inputs.size() << " names" << std::endl;
    std::cout << "average length " << double(volume) / inputs.size()
              << " bytes/name" << std::endl;
  }
  printf("\n");
  /* We are going to check for duplicates. If you have too many duplicates, something might be wrong. */

  std::sort(inputs.begin(), inputs.end());
  auto dup_str = std::adjacent_find(inputs.begin(), inputs.end());
  while (dup_str != inputs.end())
  {
    std::cout << "duplicated string " << *dup_str << std::endl;
    dup_str = std::adjacent_find(dup_str + 1, inputs.end());
  }
  size_t bytes = 0;
  for (int i = 0; i < TEST_SIZE; i++)
  {
    bytes += inputs[i].size();
  }
  printf("total volume %zu bytes\n", bytes);
  /* We are going to test our hash function to make sure that it is sane. */

  // hashes is *temporary* and does not count in the memory budget
  std::vector<uint64_t> test_hashes(TEST_SIZE), hashes(DATA_SIZE), bogus_hashes(BOGUS_SIZE);
  for (size_t i = 0; i < DATA_SIZE; i++)
  {
    hashes[i] = simple_hash(inputs[i]);
    if (i < TEST_SIZE)
    {
      test_hashes[i] = hashes[i];
      dataValidity[i].first = true;
    }
  }
  std::sort(test_hashes.begin(), test_hashes.end());
  auto dup = std::adjacent_find(test_hashes.begin(), test_hashes.end());
  size_t count = 0;
  while (dup != test_hashes.end())
  {
    count++;
    dup = std::adjacent_find(dup + 1, test_hashes.end());
  }
  printf("number of duplicates hashes %zu\n", count);
  printf("ratio of duplicates  hashes %f\n", count / double(test_hashes.size()));

  size_t size = test_hashes.size();

  printf("\n");
  printf("Test size(added to filter): %d \n", TEST_SIZE);
  printf("Query size(not added to filter): %d \n", QUERY_SIZE);
  printf("Bogus size(randomly generated strings): %d \n", BOGUS_SIZE);
  printf("\n");

  printf("-------------- Bloom addAll- 8 Filter --------------\n");
  /*******************************
   * Let us benchmark the filter!
   ******************************/
  /**
   * A filter is a simple data structure that can be easily serialized (e.g., to disk).
   * https://github.com/FastFilter/xor_singleheader#persistent-usage
   */
  // // Memory allocation (trivial):
  BloomFilter<uint64_t, 8, false> filter(FILTER_SIZE), filter_test(FILTER_SIZE);

  // Construction
  filter.AddAll(test_hashes, 0, TEST_SIZE);

  // Let us check the size of the filter in bytes:
  size_t filter_volume = filter.SizeInBytes();
  printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
         100.0 * filter_volume / bytes);
  printf("\nfilter memory usage : %1.f bits/entry\n",
         8.0 * filter_volume / test_hashes.size());
  printf("\n");

  // Let us test the query with bogus strings
  std::vector<std::string> query_set_bogus;
  size_t bogus_volume = 0;
  for (size_t i = 0; i < BOGUS_SIZE; i++)
  {
    query_set_bogus.push_back(random_string());
  }

  size_t fpp = 0;
  for (int i = 0; i < BOGUS_SIZE; i++)
  {
    bogus_volume += query_set_bogus[i].size();
    bogus_hashes[i] = simple_hash(query_set_bogus[i]);
    bool in_set = 1 - filter.Contain(bogus_hashes[i]);
    if (in_set)
    {
      fpp++;
    }
  }

  printf("Bogus false-positives: %zu\n", fpp);
  printf("Bogus false-positive rate %f\n", fpp / double(query_set_bogus.size()));

  // volatile size_t basic_count = 0;
  // printf("Benchmarking queries:\n");

  // pretty_print(query_set_bogus.size(), bogus_volume, "binary_fuse16_contain",
  //              bench([&query_set_bogus, &filter, &basic_count]() {
  //                for (std::string &ref : query_set_bogus) {
  //                  basic_count +=
  //                      binary_fuse16_contain(simple_hash(ref), &filter);
  //                }
  //              }));
  printf("Benchmarking construction speed\n");

  pretty_print(test_hashes.size(), bytes, "binary_fuse16_populate",
               bench([&test_hashes, &filter_test, &size]()
                     { filter_test.AddAll(test_hashes, 0, size); }));

  // Testing
  for (int i = 0; i < DATA_SIZE; i++)
  {
    dataValidity[i].second = 1 - filter.Contain(hashes[i]);
  }

  size_t falsePositive = 0;
  size_t falseNegative = 0;
  size_t truePositive = 0;
  size_t trueNegative = 0;

  for (int i = 0; i < DATA_SIZE; i++)
  {
    if (dataValidity[i].first == true && dataValidity[i].second == true)
    {
      truePositive++;
    }
    else if (dataValidity[i].first == true && dataValidity[i].second == false)
    {
      falseNegative++;
    }
    else if (dataValidity[i].first == false && dataValidity[i].second == true)
    {
      falsePositive++;
    }
    else if (dataValidity[i].first == false && dataValidity[i].second == false)
    {
      trueNegative++;
    }
  }

  printf("\n");
  printf("Tested with total data set (test + query): %d \n", DATA_SIZE);
  printf("True Positive: %zu", truePositive);
  printf("\t");
  printf("True Negative: %zu", trueNegative);
  printf("\n");
  printf("False Positive: %zu", falsePositive);
  printf("\t");
  printf("False Negative: %zu", falseNegative);
  printf("\n");
  printf("\n");

  printf("-------------- Bloom addAll - 12 Filter --------------\n");
  BloomFilter<uint64_t, 12, false> filter2(FILTER_SIZE);
  filter2.AddAll(test_hashes, 0, TEST_SIZE);

  // Let us check the size of the filter in bytes:
  filter_volume = filter2.SizeInBytes();
  printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
         100.0 * filter_volume / bytes);
  printf("\nfilter memory usage : %1.f bits/entry\n",
         8.0 * filter_volume / test_hashes.size());
  printf("\n");
  // Let us test the query with bogus strings

  fpp = 0;
  for (auto &ref : bogus_hashes)
  {
    bool in_set = 1 - filter2.Contain(ref);
    if (in_set)
    {
      fpp++;
    }
  }

  printf("Bogus false-positives: %zu\n", fpp);
  printf("Bogus false-positive rate %f\n", fpp / double(query_set_bogus.size()));

  // Testing
  for (int i = 0; i < DATA_SIZE; i++)
  {
    dataValidity[i].second = 1 - filter2.Contain(hashes[i]);
  }

  falsePositive = falseNegative = truePositive = trueNegative = 0;

  for (int i = 0; i < DATA_SIZE; i++)
  {
    if (dataValidity[i].first == true && dataValidity[i].second == true)
    {
      truePositive++;
    }
    else if (dataValidity[i].first == true && dataValidity[i].second == false)
    {
      falseNegative++;
    }
    else if (dataValidity[i].first == false && dataValidity[i].second == true)
    {
      falsePositive++;
    }
    else if (dataValidity[i].first == false && dataValidity[i].second == false)
    {
      trueNegative++;
    }
  }

  printf("\n");
  printf("Tested with total data set (test + query): %d \n", DATA_SIZE);
  printf("True Positive: %zu", truePositive);
  printf("\t");
  printf("True Negative: %zu", trueNegative);
  printf("\n");
  printf("False Positive: %zu", falsePositive);
  printf("\t");
  printf("False Negative: %zu", falseNegative);
  printf("\n");
  printf("\n");

  printf("-------------- Bloom addAll - 16 Filter --------------\n");
  BloomFilter<uint64_t, 16, false> filter3(FILTER_SIZE);
  filter3.AddAll(test_hashes, 0, TEST_SIZE);

  // Let us check the size of the filter in bytes:
  filter_volume = filter3.SizeInBytes();
  printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
         100.0 * filter_volume / bytes);
  printf("\nfilter memory usage : %1.f bits/entry\n",
         8.0 * filter_volume / test_hashes.size());
  printf("\n");
  // Let us test the query with bogus strings

  fpp = 0;
  for (auto &ref : bogus_hashes)
  {
    bool in_set = 1 - filter3.Contain(ref);
    if (in_set)
    {
      fpp++;
    }
  }

  printf("Bogus false-positives: %zu\n", fpp);
  printf("Bogus false-positive rate %f\n", fpp / double(query_set_bogus.size()));

  // Testing
  for (int i = 0; i < DATA_SIZE; i++)
  {
    dataValidity[i].second = 1 - filter3.Contain(hashes[i]);
  }

  falsePositive = falseNegative = truePositive = trueNegative = 0;

  for (int i = 0; i < DATA_SIZE; i++)
  {
    if (dataValidity[i].first == true && dataValidity[i].second == true)
      truePositive++;
    else if (dataValidity[i].first == true && dataValidity[i].second == false)
      falseNegative++;
    else if (dataValidity[i].first == false && dataValidity[i].second == true)
      falsePositive++;
    else if (dataValidity[i].first == false && dataValidity[i].second == false)
      trueNegative++;
  }

  printf("\n");
  printf("Tested with total data set (test + query): %d \n", DATA_SIZE);
  printf("True Positive: %zu", truePositive);
  printf("\t");
  printf("True Negative: %zu", trueNegative);
  printf("\n");
  printf("False Positive: %zu", falsePositive);
  printf("\t");
  printf("False Negative: %zu", falseNegative);
  printf("\n");

  // Bloom Derivatives

  printf("-------------- Bloom - 8 Filter --------------\n");
  BloomFilter<uint64_t, 8, false> bloom_8(FILTER_SIZE);

  // Construction
  for (int i = 0; i < TEST_SIZE; i++)
  {
    bloom_8.Add(hashes[i]);
  }

  // Let us check the size of the filter in bytes:
  filter_volume = bloom_8.SizeInBytes();
  printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
         100.0 * filter_volume / bytes);
  printf("\nfilter memory usage : %1.f bits/entry\n",
         8.0 * filter_volume / test_hashes.size());
  printf("\n");
  // Let us test the query with bogus strings

  fpp = 0;
  for (auto &ref : bogus_hashes)
  {
    bool in_set = 1 - bloom_8.Contain(ref);
    if (in_set)
    {
      fpp++;
    }
  }

  printf("Bogus false-positives: %zu\n", fpp);
  printf("Bogus false-positive rate %f\n", fpp / double(query_set_bogus.size()));

  // Testing
  for (int i = 0; i < DATA_SIZE; i++)
  {
    dataValidity[i].second = 1 - bloom_8.Contain(hashes[i]);
  }

  falsePositive = falseNegative = truePositive = trueNegative = 0;

  for (int i = 0; i < DATA_SIZE; i++)
  {
    if (dataValidity[i].first == true && dataValidity[i].second == true)
      truePositive++;
    else if (dataValidity[i].first == true && dataValidity[i].second == false)
      falseNegative++;
    else if (dataValidity[i].first == false && dataValidity[i].second == true)
      falsePositive++;
    else if (dataValidity[i].first == false && dataValidity[i].second == false)
      trueNegative++;
  }

  printf("\n");
  printf("Tested with total data set (test + query): %d \n", DATA_SIZE);
  printf("True Positive: %zu", truePositive);
  printf("\t");
  printf("True Negative: %zu", trueNegative);
  printf("\n");
  printf("False Positive: %zu", falsePositive);
  printf("\t");
  printf("False Negative: %zu", falseNegative);
  printf("\n");

  printf("-------------- Bloom - 16 Filter --------------\n");
  BloomFilter<uint64_t, 16, false> bloom_16(FILTER_SIZE);

  // Construction
  for (int i = 0; i < TEST_SIZE; i++)
  {
    bloom_16.Add(hashes[i]);
  }

  // Let us check the size of the filter in bytes:
  filter_volume = bloom_16.SizeInBytes();
  printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
         100.0 * filter_volume / bytes);
  printf("\nfilter memory usage : %1.f bits/entry\n",
         8.0 * filter_volume / test_hashes.size());
  printf("\n");
  // Let us test the query with bogus strings

  fpp = 0;
  for (auto &ref : bogus_hashes)
  {
    bool in_set = 1 - bloom_16.Contain(ref);
    if (in_set)
    {
      fpp++;
    }
  }

  printf("Bogus false-positives: %zu\n", fpp);
  printf("Bogus false-positive rate %f\n", fpp / double(query_set_bogus.size()));

  // Testing
  for (int i = 0; i < DATA_SIZE; i++)
  {
    dataValidity[i].second = 1 - bloom_16.Contain(hashes[i]);
  }

  falsePositive = falseNegative = truePositive = trueNegative = 0;

  for (int i = 0; i < DATA_SIZE; i++)
  {
    if (dataValidity[i].first == true && dataValidity[i].second == true)
      truePositive++;
    else if (dataValidity[i].first == true && dataValidity[i].second == false)
      falseNegative++;
    else if (dataValidity[i].first == false && dataValidity[i].second == true)
      falsePositive++;
    else if (dataValidity[i].first == false && dataValidity[i].second == false)
      trueNegative++;
  }

  printf("\n");
  printf("Tested with total data set (test + query): %d \n", DATA_SIZE);
  printf("True Positive: %zu", truePositive);
  printf("\t");
  printf("True Negative: %zu", trueNegative);
  printf("\n");
  printf("False Positive: %zu", falsePositive);
  printf("\t");
  printf("False Negative: %zu", falseNegative);
  printf("\n");

  return EXIT_SUCCESS;
}